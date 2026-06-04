"""Tests for tools.app_closure_audit — device-accurate apt-install PASS/FAIL audit.

The audit's claim is that the exit-73 `dpkg --configure` cascade is predicted NOT by
"the closure mentions systemd/dbus" (every GTK3 app's closure does, including the
device-PROVEN galculator) but by a small set of MAINTAINER-SCRIPT cascade triggers in
the install DELTA (closure minus the base's reconstructed-dpkg-DB installed-set).

RE-CLASSIFICATION (general maintscript-shim): those maintainer-script triggers split into
two — :data:`NEUTRALIZED_BY_SHIM` (the DEBCONF / INIT-SCRIPT / CONFFILE-MAINTSCRIPT /
schema-registration class the now-ALWAYS-staged maintscript-shim drives to exit 0:
x11-common, libpaper1, xfonts-*, appstream, session-migration, gsettings-desktop-schemas,
glib-networking*) and :data:`CASCADE_TRIGGERS` (the GENUINELY-unsatisfiable class the shim
cannot fake: perl/dict, bubblewrap/ghostscript, live daemons). Only the latter makes an app
HEAVY now — so xpdf/nsxiv/feh/qiv and gnome-calculator FLIP to reachable, while
mousepad/evince/eog/nautilus stay HEAVY for their real (sandbox/perl/daemon) reasons.

These tests pin that logic on a fully OFFLINE synthetic noble-shaped index that reproduces
the galculator-passes / mousepad-fails / gnome-calculator-FLIPS / sandboxapp-stays-heavy
split, plus the two trigger-membership rules. A single LIVE test (real noble index +
Contents) is gated behind ALR_AUDIT_NET=1 and re-checks the audit against the actual
device-proven PASS/FAIL ground truth — the strongest correctness anchor — staying offline by
default per the project's network-gated convention.
"""

import os

import pytest

from tools.app_closure_audit import (
    CASCADE_TRIGGERS,
    NEUTRALIZED_BY_SHIM,
    NOBLE_ARCH,
    NOBLE_COMPONENTS,
    NOBLE_MIRROR,
    NOBLE_SUITE,
    AuditResult,
    Verdict,
    _selftest,
    audit_catalog,
    classify,
    is_cascade_trigger,
    is_neutralized_by_shim,
)
from tools.deb_closure import parse_packages

# A noble-shaped slice reproducing the discrimination. gtkapp == galculator-class
# (drags systemd/dbus/dconf via libgtk but PASSES); dictapp == mousepad-class
# (adds perl-base + dictionaries-common → genuinely HEAVY); gnomeapp == POST-SHIM
# gnome-calculator (adds gsettings-desktop-schemas + libappstream5 + session-migration,
# ALL shim-neutralized → FLIPS to LIKELY-PASS); sandboxapp == eog/nautilus-class (adds
# bubblewrap → stays HEAVY); x11app == xpdf/nsxiv-class (x11-common+libpaper1 → LIKELY-PASS).
FIXTURE = """\
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

# What the reconstructed base dpkg DB marks installed (base ships the GTK .so + glib
# + smi, but NOT the dbus/systemd/dconf-service/libpam-systemd daemon binaries).
# ``baseapp`` mirrors gimp: base-provided itself, so ALREADY-INSTALLED even though its
# closure drags the exit-73 X11 triggers x11-common+libpaper1.
BASE_INSTALLED = {"libc6", "libgtk-3-0t64", "libglib2.0-0t64", "shared-mime-info",
                  "baseapp"}


@pytest.fixture(scope="module")
def index():
    return parse_packages(FIXTURE)


# --------------------------------------------------------------------------- #
# the central claim: systemd/dbus in the closure does NOT mean FAIL
# --------------------------------------------------------------------------- #
def test_gtk_app_passes_despite_dragging_systemd_dbus_dconf(index):
    r = classify("gtkapp", index, BASE_INSTALLED)
    assert isinstance(r, AuditResult)
    assert r.verdict == Verdict.LIKELY_PASS.value
    assert r.likely_pass is True
    # the delta really does contain systemd/dbus/dconf-service/libpam-systemd …
    assert {"systemd", "dbus", "dconf-service", "libpam-systemd"} <= set(r.delta)
    # … yet NONE of those is a cascade trigger, so the verdict is PASS.
    assert r.cascade_triggers == ()


def test_dict_app_is_heavy_via_perl_dictionary_postinsts(index):
    # mousepad-class: perl/dict is GENUINELY-heavy — the shim cannot fake real perl
    # registration / dpkg-reconfigure word-lists, so dictapp stays HEAVY.
    r = classify("dictapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.HEAVY.value
    assert r.likely_pass is False
    assert {"perl-base", "dictionaries-common"} <= set(r.cascade_triggers)


def test_gnome_app_flips_to_likely_pass_after_shim(index):
    # POST-SHIM gnome-calculator: gsettings-desktop-schemas + libappstream5 +
    # session-migration are ALL NEUTRALIZED_BY_SHIM (the appstream rm_conffile PREINST + the
    # gschemas/session registration). With no GENUINELY-heavy trigger left, it FLIPS from the
    # old HEAVY to LIKELY-PASS — the re-classification the general maintscript-shim unlocks.
    r = classify("gnomeapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.LIKELY_PASS.value
    assert r.likely_pass is True
    # the delta STILL drags them (closure unchanged) — they are just no longer fatal.
    assert {"gsettings-desktop-schemas", "libappstream5", "session-migration"} <= set(r.delta)
    assert r.cascade_triggers == ()


def test_sandbox_app_stays_heavy_via_bubblewrap_despite_neutralized_gsettings(index):
    # eog/nautilus-class: bubblewrap is GENUINELY-heavy (setuid/namespace install the guest
    # cannot grant) → HEAVY even though its gsettings-desktop-schemas is shim-neutralized.
    # This is WHY eog/nautilus stay HEAVY while gnome-calculator flips.
    r = classify("sandboxapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.HEAVY.value
    assert r.likely_pass is False
    assert r.cascade_triggers == ("bubblewrap",)  # gsettings filtered out of triggers
    assert "gsettings-desktop-schemas" in r.delta  # still in the delta, just not a trigger


def test_x11_app_flips_to_likely_pass_after_shim(index):
    # xpdf/nsxiv/feh/qiv-class: x11-common (was postinst exit 127) + libpaper1 (was postinst
    # exit 2) are NEUTRALIZED by the always-applied maintscript-shim → LIKELY-PASS. This is the
    # X11-image-viewer + apt-Qt-GUI unlock (a Qt app's only blocker is x11-common via libsm6).
    r = classify("x11app", index, BASE_INSTALLED)
    assert r.verdict == Verdict.LIKELY_PASS.value
    assert r.likely_pass is True
    assert r.cascade_triggers == ()
    # the delta still drags them; they are neutralized, not removed.
    assert {"x11-common", "libpaper1"} <= set(r.delta)


def test_base_provided_target_is_already_installed(index):
    # gimp-class: the target is itself base-provided → `apt install` is a no-op →
    # ALREADY-INSTALLED, PASS. The short-circuit fires BEFORE the closure is computed, so it is
    # orthogonal to the trigger reclassification (held under the old model, holds now).
    r = classify("baseapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.ALREADY_INSTALLED.value
    assert r.likely_pass is True
    # closure is short-circuited (not computed) for an already-installed target.
    assert r.cascade_triggers == ()
    assert r.delta == ()
    assert r.closure_size == 0


def test_unsatisfiable_dep_is_unsat(index):
    r = classify("brokenapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.UNSAT.value
    assert any("libdoesnotexist-1" in u for u in r.unsatisfied)


def test_missing_target_is_missing(index):
    r = classify("totally-absent", index, BASE_INSTALLED)
    assert r.verdict == Verdict.MISSING.value
    assert r.closure_size == 0
    assert r.delta_size == 0


# --------------------------------------------------------------------------- #
# cascade-trigger membership (the inert-vs-fatal boundary)
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize("pkg", ["systemd", "dbus", "dconf-service", "libpam-systemd",
                                 "libdconf1", "dbus-daemon", "systemd-sysv"])
def test_systemd_dbus_dconf_are_NOT_cascade_triggers(pkg):
    # These are exactly the packages galculator proves are inert on the device.
    assert not is_cascade_trigger(pkg)


@pytest.mark.parametrize("pkg", ["perl-base", "perl", "dictionaries-common",
                                 "emacsen-common", "bubblewrap", "ghostscript",
                                 "avahi-daemon", "cups-daemon", "rtkit", "policykit-1",
                                 "polkitd", "accountsservice", "packagekit", "colord"])
def test_known_GENUINELY_heavy_cascade_triggers(pkg):
    # The class the maintscript-shim CANNOT fake — still triggers, still HEAVY.
    assert is_cascade_trigger(pkg)
    assert pkg in CASCADE_TRIGGERS
    assert not is_neutralized_by_shim(pkg)


@pytest.mark.parametrize("pkg", ["x11-common", "libpaper1", "xfonts-utils",
                                 "xfonts-encodings", "xfonts-base", "xserver-common",
                                 "appstream", "libappstream5", "libappstream4",
                                 "session-migration", "gsettings-desktop-schemas",
                                 "glib-networking", "glib-networking-services",
                                 "glib-networking-common"])
def test_neutralized_by_shim_are_no_longer_triggers(pkg):
    # The DEBCONF / INIT-SCRIPT / CONFFILE-MAINTSCRIPT / registration class the always-applied
    # maintscript-shim drives to exit 0 → reclassified OUT of the fatal trigger set.
    assert is_neutralized_by_shim(pkg)
    assert pkg in NEUTRALIZED_BY_SHIM
    assert not is_cascade_trigger(pkg), f"{pkg} should be shim-neutralized, not a trigger"


def test_gstreamer_plugin_prefix_is_a_trigger():
    assert is_cascade_trigger("gstreamer1.0-plugins-base")
    assert is_cascade_trigger("gstreamer1.0-plugins-good")
    assert not is_cascade_trigger("gstreamer1.0-x")  # not a plugin metapackage


def test_neutralized_set_and_trigger_set_are_disjoint():
    # A package is EITHER shim-neutralized OR a genuine trigger, never both (is_cascade_trigger
    # short-circuits on the neutralized set). gstreamer prefix members aren't in either literal.
    assert NEUTRALIZED_BY_SHIM.isdisjoint(CASCADE_TRIGGERS)
    for pkg in NEUTRALIZED_BY_SHIM:
        assert not is_cascade_trigger(pkg)
    for pkg in CASCADE_TRIGGERS:
        assert not is_neutralized_by_shim(pkg)


# --------------------------------------------------------------------------- #
# batch + determinism + serialisation
# --------------------------------------------------------------------------- #
def test_audit_catalog_batch(index):
    res = audit_catalog(
        ["gtkapp", "dictapp", "gnomeapp", "sandboxapp", "x11app", "brokenapp"],
        index, BASE_INSTALLED)
    assert len(res) == 6
    by = {r.package: r.verdict for r in res}
    assert by["gtkapp"] == "LIKELY-PASS"
    assert by["dictapp"] == "HEAVY"             # perl/dict — genuinely heavy
    assert by["gnomeapp"] == "LIKELY-PASS"      # FLIPS: gsettings/appstream/session-migration neutralized
    assert by["sandboxapp"] == "HEAVY"          # bubblewrap — genuinely heavy
    assert by["x11app"] == "LIKELY-PASS"        # x11-common/libpaper1 neutralized
    assert by["brokenapp"] == "UNSAT"


def test_classify_is_deterministic_and_serialises(index):
    a = classify("gtkapp", index, BASE_INSTALLED)
    b = classify("gtkapp", index, BASE_INSTALLED)
    assert a.as_dict() == b.as_dict()
    d = a.as_dict()
    assert d["verdict"] == "LIKELY-PASS"
    assert isinstance(d["cascade_triggers"], list)
    assert d["closure_size"] > 0


def test_module_selftest_passes(capsys):
    rc = _selftest()
    out = capsys.readouterr().out
    assert rc == 0, out
    assert "ALL PASS" in out


# --------------------------------------------------------------------------- #
# LIVE — re-validate against the real device-proven PASS/FAIL ground truth
# (network-gated; off by default)
# --------------------------------------------------------------------------- #
@pytest.mark.skipif(
    os.environ.get("ALR_AUDIT_NET") != "1",
    reason="network-gated; set ALR_AUDIT_NET=1 to fetch the real noble index + Contents",
)
def test_live_audit_matches_device_ground_truth():
    from pathlib import Path

    from tools.app_closure_audit import fetch_packages_index
    from tools.build_dpkg_db import fetch_contents, installed_packages, parse_contents

    base_tar = Path(__file__).resolve().parents[1] / (
        "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    )
    if not base_tar.is_file():
        pytest.skip("base rootfs payload absent")

    index = parse_packages(
        fetch_packages_index(NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS)
    )
    cf = fetch_contents(NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS)
    base_installed = set(installed_packages(base_tar, parse_contents(cf.text)).packages)

    # Device-proven ground truth (project memory), re-validated under the GENERAL maintscript-
    # shim model. gimp is base-PROVIDED → ALREADY-INSTALLED (apt no-op). gnome-calculator now
    # FLIPS to reachable: its only blockers (appstream + session-migration + gsettings-desktop-
    # schemas + glib-networking*) are ALL NEUTRALIZED_BY_SHIM. The still-HEAVY set keeps its
    # REAL reasons: mousepad/gedit (perl/dict), eog (bubblewrap) — none shim-fakeable.
    proven_pass = ["galculator", "l3afpad", "htop", "gimp", "foot", "netsurf-gtk"]
    still_heavy = ["mousepad", "gedit", "eog"]   # perl/dict (mousepad/gedit), bubblewrap (eog)

    for pkg in proven_pass:
        r = classify(pkg, index, base_installed)
        assert r.likely_pass, (
            f"{pkg} should be PASS-class (LIKELY-PASS|ALREADY-INSTALLED) but got "
            f"{r.verdict} (cascade={r.cascade_triggers})"
        )
    # gimp specifically is the base-provided case → ALREADY-INSTALLED.
    assert classify("gimp", index, base_installed).verdict == "ALREADY-INSTALLED", (
        "gimp is base-provided; it must audit ALREADY-INSTALLED (apt no-op)"
    )
    # gnome-calculator FLIPS to reachable under the general shim (the TASK assertion): its
    # blockers were all the appstream/session-migration/gsettings/glib-networking class.
    gc = classify("gnome-calculator", index, base_installed)
    assert gc.verdict == "LIKELY-PASS", (
        f"gnome-calculator must FLIP to LIKELY-PASS under the general maintscript-shim "
        f"(was HEAVY for appstream/gsettings/session-migration, all now neutralized); "
        f"got {gc.verdict} (cascade={gc.cascade_triggers})"
    )
    for pkg in still_heavy:
        r = classify(pkg, index, base_installed)
        assert r.verdict == "HEAVY", (
            f"{pkg} should STAY HEAVY (real perl/dict|sandbox reason) but got {r.verdict}"
        )
        assert r.cascade_triggers, f"{pkg} HEAVY must name ≥1 GENUINELY-heavy trigger"
        # and that trigger must be a GENUINELY-heavy one, not a shim-neutralized package.
        assert all(not is_neutralized_by_shim(t) for t in r.cascade_triggers), (
            f"{pkg} HEAVY triggers must all be genuinely-heavy; got {r.cascade_triggers}"
        )

    # The catalog additions must all audit LIKELY-PASS — now INCLUDING the X11-viewer +
    # apt-Qt-GUI re-adds (nsxiv/feh/qiv/xpdf/qpdfview) whose ONLY blockers were the shim-
    # neutralized x11-common/libpaper1/xfonts-* class.
    for pkg in ["gpicview", "xarchiver", "sakura", "viewnior", "xzgv", "qalculate-gtk",
                "nsxiv", "feh", "qiv", "xpdf", "qpdfview"]:
        r = classify(pkg, index, base_installed)
        assert r.verdict == "LIKELY-PASS", (
            f"catalog/re-add entry {pkg} should be LIKELY-PASS but got {r.verdict} "
            f"(cascade={r.cascade_triggers})"
        )


@pytest.mark.skipif(
    os.environ.get("ALR_AUDIT_NET") != "1",
    reason="network-gated; set ALR_AUDIT_NET=1 to fetch the real noble index + Contents",
)
def test_xpdf_nsxiv_flip_to_likely_pass_under_general_shim():
    """The xpdf/nsxiv X11-viewer class was HEAVY only because of x11-common (was postinst
    exit 127) + libpaper1 (was postinst exit 2) + xfonts-* — exactly the debconf/init-script
    class the now-ALWAYS-applied maintscript-shim drives to exit 0. So they FLIP to
    LIKELY-PASS (the X11-image-viewer unlock). Their x11-common/libpaper1 are still in the
    delta (closure unchanged) but are NEUTRALIZED_BY_SHIM, not cascade triggers."""
    from pathlib import Path

    from tools.app_closure_audit import fetch_packages_index
    from tools.build_dpkg_db import fetch_contents, installed_packages, parse_contents

    base_tar = Path(__file__).resolve().parents[1] / (
        "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    )
    if not base_tar.is_file():
        pytest.skip("base rootfs payload absent")

    index = parse_packages(
        fetch_packages_index(NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS)
    )
    cf = fetch_contents(NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS)
    base_installed = set(installed_packages(base_tar, parse_contents(cf.text)).packages)

    for pkg in ["xpdf", "nsxiv", "feh", "qiv"]:
        r = classify(pkg, index, base_installed)
        assert r.verdict == "LIKELY-PASS", (
            f"{pkg} should FLIP to LIKELY-PASS under the general shim but got {r.verdict} "
            f"(cascade={r.cascade_triggers})"
        )
        assert r.cascade_triggers == (), f"{pkg} must have 0 genuine triggers; got {r.cascade_triggers}"
        # x11-common / libpaper1 are still dragged by the closure — neutralized, not removed.
        assert "x11-common" in r.delta, f"{pkg} delta should still drag x11-common"
        assert all(is_neutralized_by_shim(p) for p in r.delta if p in NEUTRALIZED_BY_SHIM)


@pytest.mark.skipif(
    os.environ.get("ALR_AUDIT_NET") != "1",
    reason="network-gated; set ALR_AUDIT_NET=1 to fetch the real noble index + Contents",
)
def test_qt_gui_app_qpdfview_flips_via_x11_common():
    """The apt Qt-GUI class was closure-blocked by x11-common (libqt6gui6t64 → libsm6 →
    x11-common). qpdfview (a Qt PDF viewer) had x11-common as its ONLY blocker → it FLIPS to
    LIKELY-PASS under the general shim, the apt-Qt-GUI unlock."""
    from pathlib import Path

    from tools.app_closure_audit import fetch_packages_index
    from tools.build_dpkg_db import fetch_contents, installed_packages, parse_contents

    base_tar = Path(__file__).resolve().parents[1] / (
        "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    )
    if not base_tar.is_file():
        pytest.skip("base rootfs payload absent")

    index = parse_packages(
        fetch_packages_index(NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS)
    )
    cf = fetch_contents(NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS)
    base_installed = set(installed_packages(base_tar, parse_contents(cf.text)).packages)

    r = classify("qpdfview", index, base_installed)
    assert r.verdict == "LIKELY-PASS", (
        f"qpdfview (Qt, x11-common-only blocker) should FLIP to LIKELY-PASS but got "
        f"{r.verdict} (cascade={r.cascade_triggers})"
    )
    assert r.cascade_triggers == ()
