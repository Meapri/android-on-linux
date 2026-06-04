"""Tests for tools.app_closure_audit — device-accurate apt-install PASS/FAIL audit.

The audit's claim is that the exit-73 `dpkg --configure` cascade is predicted NOT by
"the closure mentions systemd/dbus" (every GTK3 app's closure does, including the
device-PROVEN galculator) but by a small set of MAINTAINER-SCRIPT cascade triggers in
the install DELTA (closure minus the base's reconstructed-dpkg-DB installed-set).

These tests pin that logic on a fully OFFLINE synthetic noble-shaped index that
reproduces the galculator-passes / mousepad-fails / gnome-calculator-fails split, plus
the cascade-trigger membership rules. A single LIVE test (real noble index + Contents)
is gated behind ALR_AUDIT_NET=1 and re-checks the audit against the actual
device-proven PASS/FAIL ground truth — the strongest correctness anchor — staying
offline by default per the project's network-gated convention.
"""

import os

import pytest

from tools.app_closure_audit import (
    CASCADE_TRIGGERS,
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
)
from tools.deb_closure import parse_packages

# A noble-shaped slice reproducing the discrimination. gtkapp == galculator-class
# (drags systemd/dbus/dconf via libgtk but PASSES); dictapp == mousepad-class
# (adds perl-base + dictionaries-common → FAIL); gnomeapp == gnome-calculator-class
# (adds gsettings-desktop-schemas + libappstream5 → FAIL).
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
"""

# What the reconstructed base dpkg DB marks installed (base ships the GTK .so + glib
# + smi, but NOT the dbus/systemd/dconf-service/libpam-systemd daemon binaries).
BASE_INSTALLED = {"libc6", "libgtk-3-0t64", "libglib2.0-0t64", "shared-mime-info"}


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
    r = classify("dictapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.HEAVY.value
    assert r.likely_pass is False
    assert {"perl-base", "dictionaries-common"} <= set(r.cascade_triggers)


def test_gnome_app_is_heavy_via_gsettings_appstream(index):
    r = classify("gnomeapp", index, BASE_INSTALLED)
    assert r.verdict == Verdict.HEAVY.value
    assert {"gsettings-desktop-schemas", "libappstream5"} <= set(r.cascade_triggers)


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
                                 "emacsen-common", "gsettings-desktop-schemas",
                                 "appstream", "libappstream5", "session-migration",
                                 "glib-networking", "bubblewrap", "ghostscript"])
def test_known_cascade_triggers(pkg):
    assert is_cascade_trigger(pkg)
    assert pkg in CASCADE_TRIGGERS or pkg == "appstream"


def test_gstreamer_plugin_prefix_is_a_trigger():
    assert is_cascade_trigger("gstreamer1.0-plugins-base")
    assert is_cascade_trigger("gstreamer1.0-plugins-good")
    assert not is_cascade_trigger("gstreamer1.0-x")  # not a plugin metapackage


# --------------------------------------------------------------------------- #
# batch + determinism + serialisation
# --------------------------------------------------------------------------- #
def test_audit_catalog_batch(index):
    res = audit_catalog(["gtkapp", "dictapp", "gnomeapp", "brokenapp"], index, BASE_INSTALLED)
    assert len(res) == 4
    by = {r.package: r.verdict for r in res}
    assert by["gtkapp"] == "LIKELY-PASS"
    assert by["dictapp"] == "HEAVY"
    assert by["gnomeapp"] == "HEAVY"
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

    # Device-proven ground truth (project memory).
    proven_pass = ["galculator", "l3afpad", "htop", "gimp", "foot", "netsurf-gtk"]
    proven_fail = ["mousepad", "gnome-calculator", "gedit", "eog"]

    for pkg in proven_pass:
        r = classify(pkg, index, base_installed)
        assert r.verdict == "LIKELY-PASS", (
            f"{pkg} should be LIKELY-PASS but got {r.verdict} "
            f"(cascade={r.cascade_triggers})"
        )
    for pkg in proven_fail:
        r = classify(pkg, index, base_installed)
        assert r.verdict == "HEAVY", (
            f"{pkg} should be HEAVY but got {r.verdict}"
        )
        assert r.cascade_triggers, f"{pkg} HEAVY must name ≥1 cascade trigger"

    # The new catalog additions must all audit LIKELY-PASS.
    for pkg in ["gpicview", "xarchiver", "sakura", "viewnior", "xzgv", "xpdf",
                "qalculate-gtk", "nsxiv"]:
        r = classify(pkg, index, base_installed)
        assert r.verdict == "LIKELY-PASS", (
            f"catalog entry {pkg} should be LIKELY-PASS but got {r.verdict} "
            f"(cascade={r.cascade_triggers})"
        )
