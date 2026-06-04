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
exit-73 cascade is triggered ONLY by a small set of **maintainer-script packages**
whose ``postinst`` needs a running init / dbus / perl / appstream registration —
and crucially NOT by ``systemd``/``dbus``/``dconf``/``libpam-systemd`` themselves,
which galculator proves are inert in this environment (they ship, ldconfig runs,
no daemon is started, configure still exits 0).

The validated cascade trigger set (:data:`CASCADE_TRIGGERS`) is:
  * perl / dictionary registration: ``perl-base``, ``perl``, ``dictionaries-common``,
    ``emacsen-common`` (mousepad/gedit class);
  * GNOME-platform registration: ``gsettings-desktop-schemas``, ``appstream`` /
    ``libappstream5``, ``session-migration``, ``glib-networking*`` (gnome-calculator
    /gedit/eog class);
  * sandbox / heavy daemon registration: ``bubblewrap``, ``ghostscript``,
    ``gstreamer1.0-plugins-*``, and the explicit daemons (``avahi-daemon``,
    ``cups-daemon``, ``rtkit``, ``polkitd``/``policykit-1``, ``accountsservice``,
    ``packagekit``, ``colord``).

A catalog candidate is LIKELY-PASS iff its delta has 0 cascade triggers AND its
closure has 0 unsatisfied deps; otherwise HEAVY (drop / document).

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
# The validated maintainer-script cascade trigger set
# --------------------------------------------------------------------------- #
# Packages whose postinst requires a running init / dbus session / perl / appstream
# registration → the `dpkg --configure -a` exit-73 cascade on a non-root no-systemd
# device. Derived from + validated against the device-proven PASS/FAIL ground truth
# (see module docstring and tests/test_app_closure_audit.py). Deliberately EXCLUDES
# systemd / dbus / dconf-service / libpam-systemd: galculator ships all of those in
# its delta and configures exit-0, proving they are inert here (no daemon is run).
CASCADE_TRIGGERS: frozenset[str] = frozenset({
    # perl / dictionary registration postinsts (mousepad / gedit class)
    "perl", "perl-base", "dictionaries-common", "emacsen-common",
    # GNOME-platform registration (gnome-calculator / gedit / eog class)
    "gsettings-desktop-schemas",
    "appstream", "libappstream5", "libappstream4",
    "session-migration",
    "glib-networking", "glib-networking-services", "glib-networking-common",
    # sandbox / heavy registration (eog / evince / atril / surf class)
    "bubblewrap", "ghostscript",
    # explicit daemons whose postinst would need a live init
    "avahi-daemon", "cups-daemon", "rtkit",
    "policykit-1", "polkitd", "accountsservice", "packagekit", "colord",
    # X11 init-script + debconf/ucf registration postinsts (xpdf class). DEVICE-PROVEN:
    # `apt install xpdf` failed `dpkg --configure` with `x11-common` postinst exit 127 +
    # `libpaper1` postinst exit 2 (project memory). x11-common's postinst sources the
    # debconf confmodule (`db_purge`) and, when an init script exists, calls
    # `update-rc.d`/`invoke-rc.d` — none of which the base ships (only dpkg-trigger is
    # present), so it exits 127 (command not found). libpaper1's postinst does
    # `. /usr/share/debconf/confmodule; db_get libpaper/defaultpaper; ucf …` under
    # `set -e` with NO running debconf frontend and NO `ucf`, so it exits 2. Their X11
    # siblings (`xfonts-*`/`xserver-common`) run the same `update-fonts-*`/`update-rc.d`
    # registration and fail identically. This is NOT "all X11": a pure-Xlib app whose
    # delta has none of these (e.g. one served by the x11-stage overlay) is unaffected —
    # `xcalc` (package `x11-apps`) is not even in noble main+universe so it never reaches
    # this audit; it runs via the dedicated x11 overlay, not an apt-configure. The
    # exit-73 trigger is specifically this debconf/init-script-postinst set in the DELTA.
    "x11-common", "libpaper1",
    "xfonts-utils", "xfonts-encodings", "xfonts-base", "xserver-common",
})

# Package-name PREFIXES that are also cascade triggers (family installs that run a
# shared registration postinst). gstreamer plugin packages register codecs.
CASCADE_PREFIXES: tuple[str, ...] = ("gstreamer1.0-plugins",)


def is_cascade_trigger(pkg: str) -> bool:
    """True iff ``pkg`` is a maintainer-script cascade trigger (exit-73 risk)."""
    if pkg in CASCADE_TRIGGERS:
        return True
    return any(pkg.startswith(p) for p in CASCADE_PREFIXES)


class Verdict(str, Enum):
    """Audit outcome. String-valued for direct JSON serialisation."""

    LIKELY_PASS = "LIKELY-PASS"   # 0 cascade triggers, 0 unsat → galculator-class
    HEAVY = "HEAVY"               # ≥1 cascade trigger in the install delta
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
      * HEAVY    — the install delta (closure − base_installed) contains ≥1 cascade
                   trigger (:func:`is_cascade_trigger`);
      * LIKELY-PASS — otherwise (galculator-class: every new package is inert).
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
# ``perl-base``+``dictionaries-common`` → HEAVY. ``gnomeapp`` mirrors
# gnome-calculator: it adds ``gsettings-desktop-schemas``+``libappstream5`` → HEAVY.
# ``brokenapp`` has an unsatisfiable dep → UNSAT. Sizes omitted (irrelevant here).
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
Depends: libc6, libgtk-3-0t64, gsettings-desktop-schemas, libappstream5
Filename: pool/main/g/gnomeapp/gnomeapp_1.0_arm64.deb

Package: gsettings-desktop-schemas
Version: 46
Depends: libc6
Filename: pool/main/g/gsds/gsettings-desktop-schemas_46_arm64.deb

Package: libappstream5
Version: 1.0
Depends: libc6
Filename: pool/main/a/appstream/libappstream5_1.0_arm64.deb

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
# mirrors gimp: it is itself base-provided, so it is ALREADY-INSTALLED even though its
# closure drags the exit-73 X11 triggers x11-common+libpaper1 (their postinsts never run).
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

    # --- gnomeapp: gnome-calculator-class — gsettings/appstream → HEAVY ----- #
    gn = classify("gnomeapp", index, base)
    check("gnomeapp verdict HEAVY", gn.verdict == "HEAVY")
    check("gnomeapp cascade triggers include gsettings-desktop-schemas+libappstream5",
          {"gsettings-desktop-schemas", "libappstream5"} <= set(gn.cascade_triggers))

    # --- x11app: xpdf-class — x11-common+libpaper1 debconf/init postinsts → HEAVY --- #
    x11 = classify("x11app", index, base)
    check("x11app verdict HEAVY (x11-common+libpaper1 postinsts)", x11.verdict == "HEAVY")
    check("x11app cascade triggers include x11-common + libpaper1",
          {"x11-common", "libpaper1"} <= set(x11.cascade_triggers))
    check("x11app not likely_pass", x11.likely_pass is False)

    # --- baseapp: gimp-class — base-provided target → ALREADY-INSTALLED ------ #
    # Its closure ALSO drags x11-common+libpaper1, but because the TARGET itself is in the
    # base installed-set, `apt install` is a no-op and those postinsts never run → PASS.
    bp = classify("baseapp", index, base)
    check("baseapp verdict ALREADY-INSTALLED (base-provided target)",
          bp.verdict == "ALREADY-INSTALLED")
    check("baseapp likely_pass True despite x11 triggers in its closure",
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

    # --- cascade-trigger membership ----------------------------------------- #
    check("systemd is NOT a cascade trigger", not is_cascade_trigger("systemd"))
    check("dbus is NOT a cascade trigger", not is_cascade_trigger("dbus"))
    check("dconf-service is NOT a cascade trigger", not is_cascade_trigger("dconf-service"))
    check("perl-base IS a cascade trigger", is_cascade_trigger("perl-base"))
    check("gsettings-desktop-schemas IS a cascade trigger",
          is_cascade_trigger("gsettings-desktop-schemas"))
    check("gstreamer1.0-plugins-base IS a cascade trigger (prefix)",
          is_cascade_trigger("gstreamer1.0-plugins-base"))
    check("x11-common IS a cascade trigger (device-proven exit 127)",
          is_cascade_trigger("x11-common"))
    check("libpaper1 IS a cascade trigger (device-proven exit 2)",
          is_cascade_trigger("libpaper1"))
    check("xfonts-utils IS a cascade trigger (X11 sibling)",
          is_cascade_trigger("xfonts-utils"))

    # --- determinism + catalog audit --------------------------------------- #
    res = audit_catalog(["gtkapp", "dictapp", "gnomeapp", "brokenapp"], index, base)
    check("audit_catalog returns one result per package", len(res) == 4)
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
