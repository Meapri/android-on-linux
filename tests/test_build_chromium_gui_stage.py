"""Host tests for tools/build_chromium_gui_stage.py (full-browser GUI stage-tar).

All OFFLINE — the build LOGIC (compat-alias planning, the repack that adds the
flat libpulsecommon + NSS dlopen plugins + DT_NEEDED-major aliases, the
ozone-wayland token proof, §5-E conformance) is exercised against synthetic
tars with no .deb download. The NETWORK pack (real bookworm chromium .deb →
337.8 MiB tar, 0 unsatisfied DT_NEEDED) is verified by hand on the host; the
on-screen --ozone-platform=wayland window is the device/compositor gate.

Contract:
  * the GUI browser entrypoint is /usr/lib/chromium/chromium (the ELF, not the
    /usr/bin shell wrapper), and the headless-shell binary is DROPPED from the
    leaf's kept files;
  * the Wayland Ozone backend is proven present by the binary's ozone tokens;
  * libpulse's libpulsecommon (DT_NEEDED but only on libpulse's absolute RUNPATH)
    is additionally flattened onto the LD_LIBRARY_PATH dir;
  * the NSS dlopen plugins (libsoftokn3/libfreebl3/…) are folded in flat + nss/;
  * a DT_NEEDED-major mismatch (libopenh264.so.7 wanted vs .deb's .so.2) is
    satisfied by a flat compat-alias copy;
  * the produced tar is §5-E ./-rooted and stage_tar_spec conformant.
"""

from __future__ import annotations

import tarfile
from pathlib import Path

import pytest

from tools import build_chromium_gui_stage as bcg
from tools.build_chromium_gui_stage import (
    ENTRYPOINT,
    LIBDIR,
    NSS_MODULES,
    OZONE_WAYLAND_TOKENS,
    binary_has_ozone_wayland,
    compat_alias_plan,
    _add_bytes,
    _member_names,
    _read_member,
    _repack_with_addons,
)
from tools.stage_tar_spec import validate_stage_tar


# --------------------------------------------------------------------------- #
# Module + constants
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bcg.__file__).is_file()


def test_entrypoint_is_the_elf_not_the_wrapper():
    # The ALR loader maps the ELF directly; /usr/bin/chromium is a shell wrapper.
    assert ENTRYPOINT == "/usr/lib/chromium/chromium"


def test_selftest_passes():
    # The module's own offline selftest must pass (exit 0).
    assert bcg._selftest() == 0


# --------------------------------------------------------------------------- #
# Ozone-wayland token proof (full GUI browser, not headless shell)
# --------------------------------------------------------------------------- #

def test_ozone_wayland_detector_needs_both_tokens():
    assert binary_has_ozone_wayland(b"x ozone_platform_wayland.cc y ozone-platform z")
    assert not binary_has_ozone_wayland(b"only ozone-platform here")
    assert not binary_has_ozone_wayland(b"only ozone_platform_wayland.cc here")
    assert not binary_has_ozone_wayland(b"headless shell, no backend")


def test_ozone_tokens_are_wayland_specific():
    assert b"ozone_platform_wayland.cc" in OZONE_WAYLAND_TOKENS


# --------------------------------------------------------------------------- #
# compat_alias_plan (DT_NEEDED-major aliasing) — pure logic
# --------------------------------------------------------------------------- #

def test_compat_alias_plan_aliases_same_stem_different_major():
    plan = compat_alias_plan(
        ["libopenh264.so.7"],
        base_lib_names=set(),
        overlay_real={"libopenh264.so.2": f"{LIBDIR}/libopenh264.so.2"},
    )
    assert plan == {"libopenh264.so.7": f"{LIBDIR}/libopenh264.so.2"}


def test_compat_alias_plan_skips_exact_base_and_overlay_match():
    plan = compat_alias_plan(
        ["libc.so.6", "libdav1d.so.6"],
        base_lib_names={"libc.so.6"},
        overlay_real={"libdav1d.so.6": f"{LIBDIR}/libdav1d.so.6"},
    )
    assert plan == {}  # both already satisfied by exact name


def test_compat_alias_plan_skips_need_with_no_same_stem_lib():
    plan = compat_alias_plan(
        ["libfoo.so.9"],
        base_lib_names=set(),
        overlay_real={"libbar.so.1": f"{LIBDIR}/libbar.so.1"},
    )
    assert plan == {}  # no libfoo in the overlay → genuine miss, not aliasable


def test_compat_alias_plan_picks_highest_major_source():
    plan = compat_alias_plan(
        ["libqux.so.9"],
        base_lib_names=set(),
        overlay_real={
            "libqux.so.1": f"{LIBDIR}/libqux.so.1",
            "libqux.so.3": f"{LIBDIR}/libqux.so.3",
        },
    )
    assert plan == {"libqux.so.9": f"{LIBDIR}/libqux.so.3"}


# --------------------------------------------------------------------------- #
# repack: flat libpulsecommon + NSS plugins + compat alias
# --------------------------------------------------------------------------- #

@pytest.fixture
def closure_tar(tmp_path: Path) -> Path:
    """A synthetic closure tar mimicking build_minimal_overlay output."""
    p = tmp_path / "closure.tar"
    with tarfile.open(p, "w") as t:
        _add_bytes(t, "usr/lib/chromium/chromium",
                   b"\x7fELF ozone_platform_wayland.cc ozone-platform", mode=0o755)
        _add_bytes(t, f"{LIBDIR}/libdav1d.so.6", b"DAV1D")
        _add_bytes(t, f"{LIBDIR}/pulseaudio/libpulsecommon-16.1.so", b"PULSECOMMON")
        _add_bytes(t, f"{LIBDIR}/libopenh264.so.2", b"OPENH264v2")
    return p


def test_repack_flattens_libpulsecommon(closure_tar, tmp_path):
    out = tmp_path / "gui.tar"
    pulse_flat, _nss, _al = _repack_with_addons(closure_tar, out, {})
    names = _member_names(out)
    assert f"./{LIBDIR}/libpulsecommon-16.1.so" in names          # flat copy added
    assert f"./{LIBDIR}/pulseaudio/libpulsecommon-16.1.so" in names  # original kept
    assert pulse_flat == f"{LIBDIR}/libpulsecommon-16.1.so"
    assert _read_member(out, f"{LIBDIR}/libpulsecommon-16.1.so") == b"PULSECOMMON"


def test_repack_adds_nss_plugins_flat_and_nss_dir(closure_tar, tmp_path):
    out = tmp_path / "gui.tar"
    blobs = {"libsoftokn3.so": b"S", "libfreebl3.so": b"F", "libsoftokn3.chk": b"C"}
    _pf, nss_added, _al = _repack_with_addons(closure_tar, out, blobs)
    names = _member_names(out)
    for mod in ("libsoftokn3.so", "libfreebl3.so"):
        assert f"./{LIBDIR}/{mod}" in names
        assert f"./{LIBDIR}/nss/{mod}" in names
    assert f"./{LIBDIR}/libsoftokn3.chk" in names           # .chk sidecar carried
    assert set(nss_added) == {"libsoftokn3.so", "libfreebl3.so"}  # .chk not counted


def test_repack_creates_compat_alias(closure_tar, tmp_path):
    out = tmp_path / "gui.tar"
    plan = {"libopenh264.so.7": f"{LIBDIR}/libopenh264.so.2"}
    _pf, _nss, aliases = _repack_with_addons(closure_tar, out, {}, plan)
    names = _member_names(out)
    assert f"./{LIBDIR}/libopenh264.so.7" in names
    assert _read_member(out, f"{LIBDIR}/libopenh264.so.7") == b"OPENH264v2"
    assert aliases == ("libopenh264.so.7 <- libopenh264.so.2",)


def test_repack_is_idempotent(closure_tar, tmp_path):
    blobs = {"libsoftokn3.so": b"S"}
    plan = {"libopenh264.so.7": f"{LIBDIR}/libopenh264.so.2"}
    first = tmp_path / "gui1.tar"
    _repack_with_addons(closure_tar, first, blobs, plan)
    second = tmp_path / "gui2.tar"
    pf2, nss2, al2 = _repack_with_addons(first, second, blobs, plan)
    assert pf2 is None and nss2 == () and al2 == ()  # nothing new to add


def test_produced_tar_is_stage_tar_conformant(closure_tar, tmp_path):
    out = tmp_path / "gui.tar"
    _repack_with_addons(closure_tar, out, {"libsoftokn3.so": b"S"},
                        {"libopenh264.so.7": f"{LIBDIR}/libopenh264.so.2"})
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors
    # §5-E: every member ./-rooted, none absolute, none escaping, no symlinks.
    with tarfile.open(out) as t:
        names = t.getnames()
        assert all(n.startswith("./") for n in names)
        assert not any(m.issym() for m in t.getmembers())


def test_headless_shell_is_in_drop_list():
    # The GUI build must drop the headless-shell binary from the leaf's kept files.
    assert "usr/lib/chromium/chromium-headless-shell" in bcg.DROP_LEAF
