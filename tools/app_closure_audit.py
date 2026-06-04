"""Device-accurate ``apt install`` PASS/FAIL audit for the bundled catalog.

What this answers
-----------------
``breadth_catalog`` predicts whether an app's closure *resolves* (0 unsatisfied
deps). That is necessary but NOT sufficient: galculator's full noble Depends
closure (157 pkgs) drags ``systemd``, ``dbus``, ``dconf-service`` and
``libpam-systemd`` — and yet galculator installs+configures cleanly on a non-root,
no-systemd device (project memory: DEVICE-PROVEN). So "closure mentions systemd"
is the WRONG signal. mousepad / gnome-calculator, by contrast, fail
``dpkg --configure`` with exit 73.

This module pins the signal that actually discriminates, grounded in the
device-proven ground truth:

  PASS (proven): galculator, l3afpad, htop, gimp, foot, netsurf-gtk
  FAIL (proven): mousepad, gnome-calculator, gedit, eog

The discriminator is the **install DELTA** — the closure MINUS the set the base
rootfs already provides (the reconstructed dpkg admin DB; :mod:`tools.build_dpkg_db`
seeds ``Status: install ok installed`` for every package whose files/SONAMEs the
base ships). apt only unpacks+configures the delta. Within that delta, the
exit-73 cascade is triggered ONLY by a small set of **maintainer-script packages** —
and crucially NOT by ``systemd``/``dbus``/``dconf``/``libpam-systemd`` themselves,
which galculator proves are inert in this environment (they ship, ldconfig runs,
no daemon is started, configure still exits 0).

THE GENERAL MAINTSCRIPT-SHIM (re-classification)
------------------------------------------------
The maintainer-script triggers split into TWO classes, because the general
maintscript-shim overlay (:mod:`tools.build_maintscript_shim_overlay`) is now staged for
EVERY apt install (``AptInstaller.MAINTSCRIPT_SHIM_OVERLAY`` — previously gated to
gnome-platform packages). The shim ships successful no-op stubs (a ``confmodule`` whose
``db_*`` return 0, ``policy-rc.d``=101, ``ucf``, ``update-rc.d``/``invoke-rc.d``,
``deb-systemd-helper``, ``dpkg-reconfigure``, and a ``dpkg-maintscript-helper`` whose
``supports``→0 and ``rm_conffile``/``mv_conffile``→0). So:

  * :data:`NEUTRALIZED_BY_SHIM` — the DEBCONF / INIT-SCRIPT / CONFFILE-MAINTSCRIPT /
    schema-and-module registration class the stubs drive to exit 0: ``x11-common`` (was
    exit 127), ``libpaper1`` (was exit 2), ``xfonts-*``/``xserver-common``, ``appstream`` /
    ``libappstream*`` (the rm_conffile PREINST), ``session-migration``, and the
    ``gsettings-desktop-schemas`` / ``glib-networking*`` registration. An app that was HEAVY
    *only* because of this class is now **LIKELY-PASS** (the X11-image-viewer + apt-Qt-GUI
    unlock: nsxiv/feh/qiv/xpdf/qpdfview).
  * :data:`CASCADE_TRIGGERS` — the GENUINELY-unsatisfiable class the shim CANNOT fake (real
    perl/dict state, a setuid/namespace sandbox, a font/ICC cache, or a live init/dbus/codec
    daemon): ``perl``/``perl-base``/``dictionaries-common``/``emacsen-common`` (mousepad),
    ``bubblewrap``/``ghostscript`` (eog/evince/nautilus), ``gstreamer1.0-plugins-*`` and the
    explicit daemons (``avahi-daemon``/``cups-daemon``/``rtkit``/``polkitd``/``policykit-1``/
    ``accountsservice``/``packagekit``/``colord``). An app whose delta has any of these stays
    **HEAVY**.

A catalog candidate is LIKELY-PASS iff its delta has 0 GENUINELY-heavy
:data:`CASCADE_TRIGGERS` AND its closure has 0 unsatisfied deps; otherwise HEAVY
(drop / document). The :data:`NEUTRALIZED_BY_SHIM` members in a delta no longer count
against it. Re-validated against the device-proven ground truth: gnome-calculator (was
HEAVY for appstream + session-migration + gsettings + glib-networking — ALL now in
NEUTRALIZED_BY_SHIM) flips to **reachable**; mousepad (perl/dict), evince (bubblewrap),
eog/nautilus (bubblewrap) stay HEAVY for their real reasons; xpdf/nsxiv/feh/qiv (only
x11-common/libpaper1/xfonts-*) flip to LIKELY-PASS. See
docs/research/maintscript-shim-generalization.md.

Reuse, not duplication
----------------------
  * closure resolution — :func:`tools.deb_closure.resolve_closure` (+ the
    ``fetch_packages_index`` / ``parse_packages`` / ``build_provides_map`` it needs);
  * base installed-set — :func:`tools.build_dpkg_db.installed_packages` over the
    noble ``Contents`` index (what the reconstructed dpkg DB marks installed).

``classify`` is PURE (feed it a parsed index + a base installed-set) so the
selftest is fully OFFLINE/deterministic over an in-memory fixture that reproduces
the galculator-passes / mousepad-fails discrimination. The ``--live`` path fetches
the real noble index + Contents and audits the real catalog.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from enum import Enum

from tools.deb_closure import (
    build_provides_map,
    fetch_packages_index,
    parse_packages,
    resolve_closure,
)

# Noble ports (the ALR base is Ubuntu noble 24.04 arm64).
NOBLE_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
NOBLE_SUITE = "noble"
NOBLE_ARCH = "arm64"
NOBLE_COMPONENTS = ("main", "universe")


# --------------------------------------------------------------------------- #
# Two classes of maintainer-script trigger: NEUTRALIZED-by-the-shim vs GENUINELY-HEAVY
# --------------------------------------------------------------------------- #
# The exit-73 `dpkg --configure` cascade on a non-root no-systemd device is driven by
# maintainer-script packages in the install DELTA. We now split that set in two, because
# the GENERAL maintscript-shim overlay (tools/build_maintscript_shim_overlay.py) is staged
# for EVERY apt install (AptInstaller.MAINTSCRIPT_SHIM_OVERLAY — previously gnome-gated):
#
#   NEUTRALIZED_BY_SHIM — the DEBCONF / INIT-SCRIPT / CONFFILE-MAINTSCRIPT / schema-and-
#     module-registration class. The shim ships successful no-op stubs that drive exactly
#     these postinsts/preinsts to exit 0: a `confmodule` whose `db_*` all return 0,
#     `policy-rc.d`=101, `ucf`/`ucfr`, `update-rc.d`/`invoke-rc.d`, `deb-systemd-helper`/
#     `-invoke`, `dpkg-reconfigure`, and a `dpkg-maintscript-helper` answering `supports`→0 +
#     every `rm_conffile`/`mv_conffile` operation→0. With the shim ALWAYS staged, these are
#     no longer fatal — an app HEAVY *only* because of this class is now LIKELY-PASS.
#
#   CASCADE_TRIGGERS — the GENUINELY-unsatisfiable class the shim CANNOT fake: real perl/
#     dictionary registration, a setuid/namespace sandbox install, a font/ICC cache rebuild,
#     and real daemons that `systemctl enable` a service / claim a system-bus name. An app
#     whose delta contains any of these stays HEAVY (no init/dbus/codec to make it work).
#
# Both sets deliberately EXCLUDE systemd / dbus / dconf-service / libpam-systemd: galculator
# ships all of those in its delta and configures exit-0, proving they are inert here.
#
# Device-proven anchors (project memory): `apt install xpdf` failed with `x11-common`
# postinst exit 127 (sources the debconf confmodule + calls update-rc.d/invoke-rc.d, none in
# the base ⇒ "command not found") + `libpaper1` postinst exit 2 (`. confmodule; db_get; ucf`
# under set -e, no frontend, no ucf). The maintscript-shim's stubs target EXACTLY those calls
# → both now exit 0, so x11-common + libpaper1 (+ their xfonts-*/xserver-common siblings) move
# into NEUTRALIZED_BY_SHIM. The appstream PREINST exit-73 (`dpkg-maintscript-helper rm_conffile
# /etc/appstream.conf … -- "$@"` unconditionally under set -e) is likewise the stub helper's
# job → appstream/libappstream* move too. session-migration (deb-systemd-helper --user, each
# `|| true`) and the gsettings/gio registration (gsettings-desktop-schemas / glib-networking*)
# are dpkg-trigger / registration no-ops in the guest with the shim staged — their compiled
# output is shipped for gnome apps by the gnome-schemas overlay, and a non-gnome app's own
# schema/module registration is a cosmetic trigger with no interested party (libglib2.0-bin is
# not in the base) → NOT fatal. (HONESTY: gsettings/glib-networking are device-re-verify items;
# they are placed in NEUTRALIZED_BY_SHIM because the cascade they were blamed for is the
# appstream/session-migration class the shim fixes, NOT a standalone hard failure — see
# docs/research/maintscript-shim-generalization.md.)
NEUTRALIZED_BY_SHIM: frozenset[str] = frozenset({
    # X11 debconf/init-script postinsts (xpdf/nsxiv/feh/qiv/Qt-GUI class). DEVICE-PROVEN
    # exit 127 / exit 2 → fixed by the stub confmodule + update-rc.d/invoke-rc.d/ucf stubs.
    "x11-common", "libpaper1",
    "xfonts-utils", "xfonts-encodings", "xfonts-base", "xserver-common",
    # conffile-maintscript (rm_conffile/mv_conffile via dpkg-maintscript-helper). The pinned
    # gnome-calculator appstream PREINST exit-73 → fixed by the stub dpkg-maintscript-helper.
    "appstream", "libappstream5", "libappstream4",
    # systemd-unit / session registration (deb-systemd-helper --user, each `|| true`).
    "session-migration",
    # GSettings schema + GIO module registration (gnome-calculator/eog class). dpkg-trigger
    # no-ops in the guest (no libglib2.0-bin interest holder); gnome apps also get the
    # precompiled gschemas overlay. NOT a standalone hard failure with the shim staged.
    "gsettings-desktop-schemas",
    "glib-networking", "glib-networking-services", "glib-networking-common",
})

# The GENUINELY-unsatisfiable class — the shim cannot make these succeed (real perl/dict
# state, setuid/namespace sandbox, font/ICC cache, or a live init/dbus/codec daemon). An app
# whose install delta contains any of these stays HEAVY. Validated against the device-proven
# FAIL set: mousepad (perl/dict), evince (bubblewrap/perl/gstreamer), eog/nautilus (bubblewrap).
CASCADE_TRIGGERS: frozenset[str] = frozenset({
    # perl / dictionary registration postinsts (mousepad / gedit / quassel class): run real
    # perl `update-*` registration + `dpkg-reconfigure` over installed word-lists — NOT a
    # debconf-read no-op, so the stub confmodule/dpkg-reconfigure cannot satisfy them.
    "perl", "perl-base", "dictionaries-common", "emacsen-common",
    # sandbox / heavy registration (eog / evince / atril / surf class): bubblewrap needs a
    # setuid/namespace install the guest cannot grant; ghostscript rebuilds a font/ICC cache.
    "bubblewrap", "ghostscript",
    # explicit daemons whose postinst would `systemctl enable` a service / claim a bus name —
    # there is no init or system bus, so these are reachability-class-A (out of shim scope).
    "avahi-daemon", "cups-daemon", "rtkit",
    "policykit-1", "polkitd", "accountsservice", "packagekit", "colord",
})

# Package-name PREFIXES that are also GENUINELY-HEAVY cascade triggers (family installs that
# run a shared registration postinst the shim cannot fake). gstreamer plugin packages register
# codecs against a daemon/registry the guest does not run.
CASCADE_PREFIXES: tuple[str, ...] = ("gstreamer1.0-plugins",)


def is_neutralized_by_shim(pkg: str) -> bool:
    """True iff ``pkg``'s maintainer-script failure is the DEBCONF / INIT-SCRIPT / CONFFILE-
    MAINTSCRIPT / registration class that the always-applied maintscript-shim drives to exit 0.

    These were exit-73 cascade triggers BEFORE the shim was generalized to every install; they
    no longer condemn an app (see NEUTRALIZED_BY_SHIM). Kept as a queryable predicate so the
    audit + tests can assert the reclassification explicitly.
    """
    return pkg in NEUTRALIZED_BY_SHIM


def is_cascade_trigger(pkg: str) -> bool:
    """True iff ``pkg`` is a GENUINELY-unsatisfiable maintainer-script cascade trigger — one the
    maintscript-shim CANNOT neutralize (real perl/dict state, setuid sandbox, font/ICC cache, or
    a live daemon). A package in NEUTRALIZED_BY_SHIM is NOT a trigger (the shim handles it)."""
    if pkg in NEUTRALIZED_BY_SHIM:
        return False
    if pkg in CASCADE_TRIGGERS:
        return True
    return any(pkg.startswith(p) for p in CASCADE_PREFIXES)


class Verdict(str, Enum):
    """Audit outcome. String-valued for direct JSON serialisation."""

    LIKELY_PASS = "LIKELY-PASS"   # 0 GENUINELY-heavy triggers, 0 unsat → galculator-class
                                  # (NEUTRALIZED_BY_SHIM members in the delta are OK)
    HEAVY = "HEAVY"               # ≥1 GENUINELY-unsatisfiable cascade trigger in the delta
                                  # (the maintscript-shim cannot fake it)
    UNSAT = "UNSAT"               # closure has unsatisfied deps (cannot apt-install)
    MISSING = "MISSING"           # target package absent from the index
    ALREADY_INSTALLED = "ALREADY-INSTALLED"  # target itself is base-provided (apt no-op)


# --------------------------------------------------------------------------- #
# Audit
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class AuditResult:
    """Per-package audit: closure, install delta, cascade triggers, verdict."""

    package: str
    verdict: str
    closure_size: int
    delta_size: int                       # packages apt must unpack+configure
    cascade_triggers: tuple[str, ...]     # the exit-73 packages found in the delta
    unsatisfied: tuple[str, ...]          # closure log lines (dep group not in index)
    delta: tuple[str, ...] = ()           # full delta package list (debug/reporting)

    @property
    def likely_pass(self) -> bool:
        """True iff the app installs+configures clean: galculator-class (LIKELY-PASS) OR
        already in the base (ALREADY-INSTALLED — `apt install` is a no-op, so there is no
        configure cascade to fail). Both are "reachable today, no shim"."""
        return self.verdict in (Verdict.LIKELY_PASS.value, Verdict.ALREADY_INSTALLED.value)

    def as_dict(self) -> dict:
        return {
            "package": self.package,
            "verdict": self.verdict,
            "closure_size": self.closure_size,
            "delta_size": self.delta_size,
            "cascade_triggers": list(self.cascade_triggers),
            "unsatisfied": list(self.unsatisfied),
        }


def classify(
    package: str,
    index: dict[str, dict],
    base_installed: set[str],
    *,
    provides_map: dict[str, list[str]] | None = None,
) -> AuditResult:
    """Audit one package against ``index`` + the base ``base_installed`` set.

    PURE / offline. The verdict is:
      * ALREADY-INSTALLED — the TARGET package is itself in ``base_installed`` (the base
                   rootfs already ships it): ``apt install`` is a no-op, so NO maintainer
                   script in its closure ever runs → it cannot trigger the exit-73 cascade.
                   This is checked FIRST and is why a base-provided app like ``gimp`` (whose
                   *closure* drags x11-common+libpaper1) is PASS on device even though those
                   two packages ARE exit-73 triggers: their postinsts simply never execute,
                   because gimp+its deps are already configured in the base.
      * MISSING  — the target does not resolve to any package in the index;
      * UNSAT    — the closure resolves but has ≥1 unsatisfied dep group;
      * HEAVY    — the install delta (closure − base_installed) contains ≥1 GENUINELY-
                   unsatisfiable cascade trigger (:func:`is_cascade_trigger`) — one the
                   always-applied maintscript-shim CANNOT neutralize (perl/dict, bubblewrap/
                   ghostscript, a live daemon). A :data:`NEUTRALIZED_BY_SHIM` member in the
                   delta (x11-common/libpaper1/appstream/…) does NOT make it HEAVY;
      * LIKELY-PASS — otherwise (galculator-class: every new package is inert OR shim-handled).
    """
    if provides_map is None:
        provides_map = build_provides_map(index)

    # Base-provided target → `apt install` is a no-op; no maintainer script runs. Checked
    # BEFORE the trigger scan so a base app (gimp) is never condemned by a trigger that
    # lives only in its already-satisfied closure (x11-common/libpaper1). This mirrors the
    # runtime: NativeAlrRuntime.install() returns Done idempotently if already installed.
    if package in base_installed:
        return AuditResult(
            package=package, verdict=Verdict.ALREADY_INSTALLED.value,
            closure_size=0, delta_size=0, cascade_triggers=(), unsatisfied=(),
        )

    log: list[str] = []
    closure = resolve_closure([package], index, provides_map=provides_map, log=log)
    if not closure:
        return AuditResult(
            package=package, verdict=Verdict.MISSING.value,
            closure_size=0, delta_size=0, cascade_triggers=(), unsatisfied=(),
        )

    unsat = tuple(l for l in log if l.startswith("unsatisfied dep group"))
    delta = sorted(set(closure) - base_installed)
    triggers = tuple(p for p in delta if is_cascade_trigger(p))

    if triggers:
        verdict = Verdict.HEAVY
    elif unsat:
        verdict = Verdict.UNSAT
    else:
        verdict = Verdict.LIKELY_PASS

    return AuditResult(
        package=package,
        verdict=verdict.value,
        closure_size=len(closure),
        delta_size=len(delta),
        cascade_triggers=triggers,
        unsatisfied=unsat,
        delta=tuple(delta),
    )


def audit_catalog(
    packages: list[str],
    index: dict[str, dict],
    base_installed: set[str],
) -> list[AuditResult]:
    """Audit each package over one shared index + base set (provides map built once)."""
    pm = build_provides_map(index)
    return [classify(p, index, base_installed, provides_map=pm) for p in packages]


# --------------------------------------------------------------------------- #
# OFFLINE selftest — reproduces the galculator-passes / mousepad-fails split
# --------------------------------------------------------------------------- #
# A noble-shaped slice. ``gtkapp`` mirrors galculator: its closure drags
# ``systemd``+``dbus``+``dconf-service``+``libpam-systemd`` (via libgtk) but NONE of
# those is a cascade trigger → LIKELY-PASS. ``dictapp`` mirrors mousepad: it adds
# ``perl-base``+``dictionaries-common`` (GENUINELY-heavy, shim cannot fake) → HEAVY.
# ``gnomeapp`` mirrors POST-SHIM gnome-calculator: it adds ``gsettings-desktop-schemas``+
# ``libappstream5`` (BOTH now NEUTRALIZED_BY_SHIM) and nothing genuinely-heavy → LIKELY-PASS
# (the re-classification: it FLIPS from the old HEAVY). ``sandboxapp`` mirrors eog/nautilus:
# it adds ``bubblewrap`` (GENUINELY-heavy) ON TOP of the shim-neutralized gsettings → stays
# HEAVY. ``x11app`` mirrors xpdf/nsxiv: ``x11-common``+``libpaper1`` only (NEUTRALIZED) →
# LIKELY-PASS. ``brokenapp`` has an unsatisfiable dep → UNSAT. Sizes omitted (irrelevant).
_FIXTURE = """\
Package: libc6
Version: 2.39
Filename: pool/main/g/glibc/libc6_2.39_arm64.deb

Package: libgtk-3-0t64
Version: 3.24
Depends: libc6, libglib2.0-0t64, dconf-gsettings-backend, shared-mime-info
Filename: pool/main/g/gtk/libgtk-3-0t64_3.24_arm64.deb

Package: libglib2.0-0t64
Version: 2.80
Depends: libc6
Filename: pool/main/g/glib/libglib2.0-0t64_2.80_arm64.deb

Package: shared-mime-info
Version: 2.4
Depends: libc6
Filename: pool/main/s/smi/shared-mime-info_2.4_arm64.deb

Package: dconf-gsettings-backend
Version: 0.40
Depends: libc6, dconf-service
Filename: pool/main/d/dconf/dconf-gsettings-backend_0.40_arm64.deb

Package: dconf-service
Version: 0.40
Depends: libc6, dbus
Filename: pool/main/d/dconf/dconf-service_0.40_arm64.deb

Package: dbus
Version: 1.14
Depends: libc6, libpam-systemd
Filename: pool/main/d/dbus/dbus_1.14_arm64.deb

Package: libpam-systemd
Version: 255
Depends: libc6, systemd
Filename: pool/main/s/systemd/libpam-systemd_255_arm64.deb

Package: systemd
Version: 255
Depends: libc6
Filename: pool/main/s/systemd/systemd_255_arm64.deb

Package: gtkapp
Version: 1.0
Depends: libc6, libgtk-3-0t64
Filename: pool/main/g/gtkapp/gtkapp_1.0_arm64.deb

Package: dictapp
Version: 1.0
Depends: libc6, libgtk-3-0t64, dictionaries-common, perl-base
Filename: pool/main/d/dictapp/dictapp_1.0_arm64.deb

Package: dictionaries-common
Version: 1.2
Depends: libc6, perl-base
Filename: pool/main/d/dc/dictionaries-common_1.2_arm64.deb

Package: perl-base
Version: 5.38
Depends: libc6
Filename: pool/main/p/perl/perl-base_5.38_arm64.deb

Package: gnomeapp
Version: 1.0
Depends: libc6, libgtk-3-0t64, gsettings-desktop-schemas, libappstream5, session-migration
Filename: pool/main/g/gnomeapp/gnomeapp_1.0_arm64.deb

Package: gsettings-desktop-schemas
Version: 46
Depends: libc6
Filename: pool/main/g/gsds/gsettings-desktop-schemas_46_arm64.deb

Package: libappstream5
Version: 1.0
Depends: libc6
Filename: pool/main/a/appstream/libappstream5_1.0_arm64.deb

Package: session-migration
Version: 0.3
Depends: libc6
Filename: pool/main/s/session-migration/session-migration_0.3_arm64.deb

Package: sandboxapp
Version: 1.0
Depends: libc6, libgtk-3-0t64, gsettings-desktop-schemas, bubblewrap
Filename: pool/main/s/sandboxapp/sandboxapp_1.0_arm64.deb

Package: bubblewrap
Version: 0.9
Depends: libc6
Filename: pool/main/b/bubblewrap/bubblewrap_0.9_arm64.deb

Package: brokenapp
Version: 1.0
Depends: libc6, libdoesnotexist-1
Filename: pool/main/b/brokenapp/brokenapp_1.0_arm64.deb

Package: x11app
Version: 1.0
Depends: libc6, x11-common, libpaper1
Filename: pool/main/x/x11app/x11app_1.0_arm64.deb

Package: x11-common
Version: 7.7
Depends: libc6
Filename: pool/main/x/xorg/x11-common_7.7_arm64.deb

Package: libpaper1
Version: 1.1
Depends: libc6
Filename: pool/main/libp/libpaper/libpaper1_1.1_arm64.deb

Package: baseapp
Version: 1.0
Depends: libc6, x11-common, libpaper1
Filename: pool/main/b/baseapp/baseapp_1.0_arm64.deb
"""

# What the reconstructed base dpkg DB marks installed: the GTK stack + glib + smi
# (the base ships their .so/data) but NOT dbus/systemd/dconf-service/libpam-systemd
# (no daemon binaries in the base) — matching the real base installed-set. ``baseapp``
# mirrors gimp: it is itself base-provided, so it is ALREADY-INSTALLED (apt no-op, its
# closure's postinsts never run) — the short-circuit is orthogonal to the trigger
# reclassification (it fires before the closure is even computed).
_FIXTURE_BASE_INSTALLED = {
    "libc6", "libgtk-3-0t64", "libglib2.0-0t64", "shared-mime-info", "baseapp",
}


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    index = parse_packages(_FIXTURE)
    base = set(_FIXTURE_BASE_INSTALLED)

    # --- gtkapp: galculator-class — drags systemd/dbus/dconf but PASSES ----- #
    gtk = classify("gtkapp", index, base)
    check("gtkapp verdict LIKELY-PASS", gtk.verdict == "LIKELY-PASS")
    check("gtkapp delta DRAGS systemd+dbus+dconf (closure not clean)",
          {"systemd", "dbus", "dconf-service", "libpam-systemd"} <= set(gtk.delta))
    check("gtkapp has 0 cascade triggers (those pkgs are inert here)",
          gtk.cascade_triggers == ())
    check("gtkapp likely_pass property", gtk.likely_pass is True)

    # --- dictapp: mousepad-class — perl/dict postinst → HEAVY --------------- #
    dic = classify("dictapp", index, base)
    check("dictapp verdict HEAVY", dic.verdict == "HEAVY")
    check("dictapp cascade triggers include perl-base + dictionaries-common",
          {"perl-base", "dictionaries-common"} <= set(dic.cascade_triggers))
    check("dictapp not likely_pass", dic.likely_pass is False)

    # --- gnomeapp: POST-SHIM gnome-calculator — gsettings/appstream/session-migration are
    #     ALL neutralized by the always-applied shim → FLIPS to LIKELY-PASS. ------------- #
    gn = classify("gnomeapp", index, base)
    check("gnomeapp verdict LIKELY-PASS (gsettings/appstream/session-migration neutralized)",
          gn.verdict == "LIKELY-PASS")
    check("gnomeapp delta still DRAGS gsettings/appstream/session-migration (closure not clean)",
          {"gsettings-desktop-schemas", "libappstream5", "session-migration"} <= set(gn.delta))
    check("gnomeapp has 0 GENUINELY-heavy cascade triggers (all are shim-neutralized)",
          gn.cascade_triggers == ())
    check("gnomeapp likely_pass property", gn.likely_pass is True)

    # --- sandboxapp: eog/nautilus-class — bubblewrap is GENUINELY-heavy (shim cannot fake a
    #     setuid/namespace install) → stays HEAVY EVEN THOUGH its gsettings is neutralized. - #
    sb = classify("sandboxapp", index, base)
    check("sandboxapp verdict HEAVY (bubblewrap is genuinely-unsatisfiable)",
          sb.verdict == "HEAVY")
    check("sandboxapp cascade triggers = bubblewrap ONLY (gsettings shim-neutralized out)",
          sb.cascade_triggers == ("bubblewrap",))
    check("sandboxapp not likely_pass", sb.likely_pass is False)

    # --- x11app: xpdf/nsxiv-class — x11-common+libpaper1 are NEUTRALIZED by the always-
    #     applied maintscript-shim → LIKELY-PASS (the X11-viewer + apt-Qt-GUI unlock). --- #
    x11 = classify("x11app", index, base)
    check("x11app verdict LIKELY-PASS (x11-common+libpaper1 shim-neutralized)",
          x11.verdict == "LIKELY-PASS")
    check("x11app has 0 GENUINELY-heavy cascade triggers", x11.cascade_triggers == ())
    check("x11app likely_pass", x11.likely_pass is True)
    check("x11-common is neutralized by the shim", is_neutralized_by_shim("x11-common"))
    check("libpaper1 is neutralized by the shim", is_neutralized_by_shim("libpaper1"))

    # --- baseapp: gimp-class — base-provided target → ALREADY-INSTALLED ------ #
    # The TARGET itself is in the base installed-set, so `apt install` is a no-op and its
    # closure's postinsts never run → PASS. (Orthogonal to the shim: the short-circuit fires
    # before the closure is even computed; it held under the old model and holds now.)
    bp = classify("baseapp", index, base)
    check("baseapp verdict ALREADY-INSTALLED (base-provided target)",
          bp.verdict == "ALREADY-INSTALLED")
    check("baseapp likely_pass True (apt no-op for a base-provided target)",
          bp.likely_pass is True)
    check("baseapp reports no triggers (closure not even computed)",
          bp.cascade_triggers == () and bp.delta == ())

    # --- brokenapp: unsatisfiable dep → UNSAT ------------------------------ #
    bk = classify("brokenapp", index, base)
    check("brokenapp verdict UNSAT", bk.verdict == "UNSAT")
    check("brokenapp names the missing dep",
          any("libdoesnotexist-1" in u for u in bk.unsatisfied))

    # --- missing target ----------------------------------------------------- #
    ms = classify("totally-absent", index, base)
    check("missing target verdict MISSING", ms.verdict == "MISSING")
    check("missing target empty closure", ms.closure_size == 0)

    # --- trigger membership: GENUINELY-heavy vs NEUTRALIZED-by-shim ----------- #
    check("systemd is NOT a cascade trigger", not is_cascade_trigger("systemd"))
    check("dbus is NOT a cascade trigger", not is_cascade_trigger("dbus"))
    check("dconf-service is NOT a cascade trigger", not is_cascade_trigger("dconf-service"))
    # GENUINELY-heavy (shim cannot fake) — stay triggers:
    check("perl-base IS a GENUINELY-heavy cascade trigger", is_cascade_trigger("perl-base"))
    check("bubblewrap IS a GENUINELY-heavy cascade trigger", is_cascade_trigger("bubblewrap"))
    check("ghostscript IS a GENUINELY-heavy cascade trigger", is_cascade_trigger("ghostscript"))
    check("accountsservice IS a GENUINELY-heavy cascade trigger (daemon)",
          is_cascade_trigger("accountsservice"))
    check("gstreamer1.0-plugins-base IS a GENUINELY-heavy cascade trigger (prefix)",
          is_cascade_trigger("gstreamer1.0-plugins-base"))
    # NEUTRALIZED by the always-applied maintscript-shim — NO LONGER triggers:
    check("x11-common is NEUTRALIZED (was device-proven exit 127) → not a trigger",
          is_neutralized_by_shim("x11-common") and not is_cascade_trigger("x11-common"))
    check("libpaper1 is NEUTRALIZED (was device-proven exit 2) → not a trigger",
          is_neutralized_by_shim("libpaper1") and not is_cascade_trigger("libpaper1"))
    check("xfonts-utils is NEUTRALIZED (X11 sibling) → not a trigger",
          is_neutralized_by_shim("xfonts-utils") and not is_cascade_trigger("xfonts-utils"))
    check("appstream is NEUTRALIZED (rm_conffile PREINST) → not a trigger",
          is_neutralized_by_shim("appstream") and not is_cascade_trigger("appstream"))
    check("session-migration is NEUTRALIZED → not a trigger",
          is_neutralized_by_shim("session-migration")
          and not is_cascade_trigger("session-migration"))
    check("gsettings-desktop-schemas is NEUTRALIZED → not a trigger",
          is_neutralized_by_shim("gsettings-desktop-schemas")
          and not is_cascade_trigger("gsettings-desktop-schemas"))

    # --- determinism + catalog audit --------------------------------------- #
    res = audit_catalog(
        ["gtkapp", "dictapp", "gnomeapp", "sandboxapp", "x11app", "brokenapp"], index, base)
    check("audit_catalog returns one result per package", len(res) == 6)
    by = {r.package: r.verdict for r in res}
    check("audit_catalog: gnomeapp flips LIKELY-PASS, sandboxapp/dictapp HEAVY, x11app PASS",
          by == {"gtkapp": "LIKELY-PASS", "dictapp": "HEAVY", "gnomeapp": "LIKELY-PASS",
                 "sandboxapp": "HEAVY", "x11app": "LIKELY-PASS", "brokenapp": "UNSAT"})
    again = classify("gtkapp", index, base)
    check("classify deterministic", again.as_dict() == gtk.as_dict())

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _render_table(results: list[AuditResult]) -> str:
    rows = [
        "| package | verdict | closure | delta | cascade triggers |",
        "|---------|---------|--------:|------:|------------------|",
    ]
    for r in results:
        trig = ", ".join(r.cascade_triggers) if r.cascade_triggers else (
            ", ".join(u.split()[-1] for u in r.unsatisfied[:3]) if r.unsatisfied else "—"
        )
        rows.append(
            f"| {r.package} | {r.verdict} | {r.closure_size} | {r.delta_size} | {trig} |"
        )
    return "\n".join(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="app_closure_audit",
        description="Device-accurate apt-install PASS/FAIL audit (closure delta vs the "
                    "base installed-set; flags the maintainer-script exit-73 cascade). "
                    "Reuses tools.deb_closure + tools.build_dpkg_db.",
    )
    parser.add_argument("--selftest", action="store_true",
                        help="run the OFFLINE deterministic selftest and exit")
    parser.add_argument("--live", action="store_true",
                        help="fetch the real noble index + Contents and audit --package set")
    parser.add_argument("--package", action="append", dest="packages",
                        help="package to audit (repeatable; --live only)")
    parser.add_argument("--base", help="base rootfs tar (for the installed-set; --live)")
    parser.add_argument("--mirror", default=NOBLE_MIRROR)
    parser.add_argument("--suite", default=NOBLE_SUITE)
    parser.add_argument("--arch", default=NOBLE_ARCH)
    parser.add_argument("--component", action="append", dest="components",
                        help="repeatable; defaults to main+universe")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.live:
        parser.error("supply --selftest (offline) or --live (network noble index)")
    if not args.packages:
        parser.error("--live requires at least one --package")
    if not args.base:
        parser.error("--live requires --base (the base rootfs tar)")

    from tools.build_dpkg_db import fetch_contents, parse_contents, installed_packages

    components = tuple(args.components) if args.components else NOBLE_COMPONENTS
    index = parse_packages(
        fetch_packages_index(args.mirror, args.suite, args.arch, components=components)
    )
    cf = fetch_contents(args.mirror, args.suite, args.arch, components=components)
    base_installed = set(installed_packages(args.base, parse_contents(cf.text)).packages)

    results = audit_catalog(args.packages, index, base_installed)
    if args.json:
        print(json.dumps([r.as_dict() for r in results], indent=2, sort_keys=True))
    else:
        print(_render_table(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
