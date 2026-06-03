"""Host tests for tools/build_content_shell_stage.py (content_shell stage-tar).

All OFFLINE — the content_shell-specific wiring (entrypoint = the chromium-shell
ELF, leaf package set, the reused repack that folds in flat libpulsecommon + NSS
dlopen plugins + DT_NEEDED-major aliases, the ozone-wayland token proof, §5-E
conformance, and the DT_NEEDED-satisfaction gate over a synthetic base) is
exercised against synthetic tars with no .deb download.

The NETWORK pack (real bookworm chromium-shell + chromium-common .debs →
~293 MiB tar, 0 unsatisfied DT_NEEDED, ozone-wayland proven, 182.7 MiB ELF) is
verified by hand on the host; the on-screen --ozone-platform=wayland window and
the single-process memory-fit claim are the device/compositor gate (DEVICE-REQ in
docs/research/chromium-window-lightweight-options.md).

Contract:
  * content_shell IS a real distro .deb — Debian's ``chromium-shell`` (the only
    distro that packages Chromium's content_shell); leaf = (chromium-shell,
    chromium-common);
  * the entrypoint is the ELF /usr/lib/chromium/chromium-shell (NOT the 52-byte
    /usr/bin/chromium-shell shell wrapper the ALR loader must not map);
  * the Wayland Ozone backend is proven present by the binary's ozone tokens
    (this is the lightest *window-capable* chromium, not the headless shell);
  * there is NO headless-shell binary to drop from the leaf (unlike the full
    chromium .deb), so the module defines no DROP_LEAF;
  * libpulsecommon is flattened, the NSS dlopen plugins are folded in flat + nss/,
    a DT_NEEDED-major mismatch is satisfied by a compat-alias copy;
  * the produced tar is §5-E ./-rooted and stage_tar_spec conformant.
"""

from __future__ import annotations

import tarfile
from pathlib import Path

import pytest

from tools import build_content_shell_stage as bcs
from tools.build_content_shell_stage import (
    ENTRYPOINT,
    ORIGIN_DIR,
    PACKAGES,
    SUITE,
    ARCH_TRIPLET,
    binary_has_ozone_wayland,
    _add_bytes,
    _entry_needed,
    _flat_lib_basenames,
    _flatten_origin_needed,
    _member_names,
    _origin_needed_to_flatten,
    _read_member,
    _synthetic_elf,
    _unsatisfied_needed,
    _repack_with_addons,
)
from tools.stage_tar_spec import validate_stage_tar

LIBREL = f"usr/lib/{ARCH_TRIPLET}"


# --------------------------------------------------------------------------- #
# Module + constants
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bcs.__file__).is_file()


def test_entrypoint_is_the_content_shell_elf_not_the_wrapper():
    # The ALR loader maps the ELF directly; /usr/bin/chromium-shell is a 52-byte
    # shell wrapper (would not run under the loader).
    assert ENTRYPOINT == "/usr/lib/chromium/chromium-shell"


def test_leaf_is_chromium_shell_plus_common():
    # content_shell is Debian's chromium-shell (source: chromium); chromium-common
    # supplies the shared resources/data the shell loads at runtime.
    assert PACKAGES == ("chromium-shell", "chromium-common")


def test_targets_debian_bookworm():
    # Ubuntu ships chromium as a snap; Debian is the only distro that ships a real
    # content_shell .deb, and bookworm is the proven forward-compat suite.
    assert SUITE == "bookworm"


def test_no_drop_leaf_attribute():
    # Unlike the full chromium .deb (which bundles chromium-headless-shell that the
    # full-GUI builder must drop), the chromium-shell .deb ships only the
    # content_shell binary — nothing to drop.
    assert not hasattr(bcs, "DROP_LEAF")


def test_selftest_passes():
    # The module's own offline selftest must pass (exit 0).
    assert bcs._selftest() == 0


# --------------------------------------------------------------------------- #
# Ozone-wayland token proof (window-capable shell, not headless)
# --------------------------------------------------------------------------- #

def test_ozone_wayland_detector_needs_both_tokens():
    assert binary_has_ozone_wayland(b"x ozone_platform_wayland.cc y ozone-platform z")
    assert not binary_has_ozone_wayland(b"only ozone-platform here")
    assert not binary_has_ozone_wayland(b"only ozone_platform_wayland.cc here")
    assert not binary_has_ozone_wayland(b"headless shell, no backend")


# --------------------------------------------------------------------------- #
# repack: flat libpulsecommon + NSS plugins + compat alias (reused engine,
# exercised through THIS module's import surface to prove the wiring)
# --------------------------------------------------------------------------- #

@pytest.fixture
def closure_tar(tmp_path: Path) -> Path:
    """A synthetic closure tar mimicking build_minimal_overlay output for the
    content_shell leaf."""
    p = tmp_path / "closure.tar"
    with tarfile.open(p, "w") as t:
        _add_bytes(t, "usr/lib/chromium/chromium-shell",
                   b"\x7fELF ozone_platform_wayland.cc ozone-platform", mode=0o755)
        _add_bytes(t, "usr/lib/chromium/content_shell.pak", b"PAK")
        _add_bytes(t, f"{LIBREL}/libdav1d.so.6", b"DAV1D")
        _add_bytes(t, f"{LIBREL}/pulseaudio/libpulsecommon-16.1.so", b"PULSECOMMON")
        _add_bytes(t, f"{LIBREL}/libopenh264.so.2", b"OPENH264v2")
    return p


def test_repack_flattens_libpulsecommon(closure_tar, tmp_path):
    out = tmp_path / "cs.tar"
    pulse_flat, _nss, _al = _repack_with_addons(closure_tar, out, {})
    names = _member_names(out)
    assert f"./{LIBREL}/libpulsecommon-16.1.so" in names           # flat copy added
    assert f"./{LIBREL}/pulseaudio/libpulsecommon-16.1.so" in names  # original kept
    assert pulse_flat == f"{LIBREL}/libpulsecommon-16.1.so"
    assert _read_member(out, f"{LIBREL}/libpulsecommon-16.1.so") == b"PULSECOMMON"


def test_repack_adds_nss_plugins_flat_and_nss_dir(closure_tar, tmp_path):
    out = tmp_path / "cs.tar"
    blobs = {"libsoftokn3.so": b"S", "libfreebl3.so": b"F", "libsoftokn3.chk": b"C"}
    _pf, nss_added, _al = _repack_with_addons(closure_tar, out, blobs)
    names = _member_names(out)
    for mod in ("libsoftokn3.so", "libfreebl3.so"):
        assert f"./{LIBREL}/{mod}" in names
        assert f"./{LIBREL}/nss/{mod}" in names
    assert f"./{LIBREL}/libsoftokn3.chk" in names
    assert set(nss_added) == {"libsoftokn3.so", "libfreebl3.so"}


def test_repack_creates_compat_alias(closure_tar, tmp_path):
    out = tmp_path / "cs.tar"
    plan = {"libopenh264.so.7": f"{LIBREL}/libopenh264.so.2"}
    _pf, _nss, aliases = _repack_with_addons(closure_tar, out, {}, plan)
    names = _member_names(out)
    assert f"./{LIBREL}/libopenh264.so.7" in names
    assert _read_member(out, f"{LIBREL}/libopenh264.so.7") == b"OPENH264v2"
    assert aliases == ("libopenh264.so.7 <- libopenh264.so.2",)


def test_produced_tar_is_stage_tar_conformant(closure_tar, tmp_path):
    out = tmp_path / "cs.tar"
    _repack_with_addons(closure_tar, out, {"libsoftokn3.so": b"S"},
                        {"libopenh264.so.7": f"{LIBREL}/libopenh264.so.2"})
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors
    with tarfile.open(out) as t:
        names = t.getnames()
        assert all(n.startswith("./") for n in names)
        assert not any(m.issym() for m in t.getmembers())


# --------------------------------------------------------------------------- #
# DT_NEEDED satisfaction gate (parametrized on the content_shell entrypoint)
# --------------------------------------------------------------------------- #

def test_entry_needed_empty_for_synthetic_elf(closure_tar):
    # The synthetic ELF has no real .dynamic section → no DT_NEEDED to read.
    assert _entry_needed(closure_tar) == []


def test_unsatisfied_needed_empty_when_no_needed(closure_tar, tmp_path):
    # With no DT_NEEDED on the synthetic binary, the gate reports nothing missing.
    base = tmp_path / "base.tar"
    with tarfile.open(base, "w") as t:
        _add_bytes(t, f"lib/{ARCH_TRIPLET}/libc.so.6", b"BASE")
    assert _unsatisfied_needed(closure_tar, base) == ()


# --------------------------------------------------------------------------- #
# $ORIGIN-only DT_NEEDED flatten — the libtest_trace_processor.so device fix.
#
# chromium-shell is linked RUNPATH=$ORIGIN and DT_NEEDEDs libtest_trace_processor.so,
# which the leaf .deb installs ONLY at $ORIGIN (/usr/lib/chromium/). The ALR loader
# resolves DT_NEEDED solely on the flat LD_LIBRARY_PATH and does NOT search $ORIGIN,
# so the device FATALed "libtest_trace_processor.so: cannot open shared object file"
# even though the file was in the tar. These tests pin the flatten + the flat-aware
# gate that makes that false-negative impossible to ship again.
# --------------------------------------------------------------------------- #

@pytest.fixture
def origin_overlay(tmp_path: Path) -> Path:
    """An overlay whose entrypoint is a REAL (parseable) ELF: RUNPATH=$ORIGIN,
    DT_NEEDED libtest_trace_processor.so (+ libc.so.6), with the lib placed ONLY at
    $ORIGIN — exactly the chromium-shell-on-device shape."""
    p = tmp_path / "origin-ov.tar"
    with tarfile.open(p, "w") as t:
        _add_bytes(
            t, ENTRYPOINT.lstrip("/"),
            _synthetic_elf(("libtest_trace_processor.so", "libc.so.6"),
                           runpath="$ORIGIN",
                           extra_tokens=b"ozone_platform_wayland.cc ozone-platform"),
            mode=0o755,
        )
        _add_bytes(t, f"{ORIGIN_DIR}/libtest_trace_processor.so", b"TTP")
    return p


@pytest.fixture
def base_with_libc(tmp_path: Path) -> Path:
    base = tmp_path / "base.tar"
    with tarfile.open(base, "w") as t:
        _add_bytes(t, f"lib/{ARCH_TRIPLET}/libc.so.6", b"BASE-libc")
    return base


def test_entry_needed_reads_real_synthetic_elf(origin_overlay):
    # The synthetic ELF is real enough that elf_needed parses its DT_NEEDED.
    assert _entry_needed(origin_overlay) == ["libtest_trace_processor.so", "libc.so.6"]


def test_flat_lib_basenames_excludes_origin_dir(origin_overlay):
    # The $ORIGIN copy lives in /usr/lib/chromium/, which is NOT a flat load dir, so
    # the flat-name set must NOT contain it (this is the crux of the old false 0).
    flat = _flat_lib_basenames(origin_overlay)
    assert "libtest_trace_processor.so" not in flat


def test_flat_lib_basenames_counts_flat_libdir(tmp_path):
    p = tmp_path / "ov.tar"
    with tarfile.open(p, "w") as t:
        _add_bytes(t, f"{LIBREL}/libdav1d.so.6", b"D")          # flat usr/lib
        _add_bytes(t, f"lib/{ARCH_TRIPLET}/libc.so.6", b"C")    # flat lib (merged-usr)
        _add_bytes(t, f"{LIBREL}/pulseaudio/libpulsecommon-16.1.so", b"P")  # subdir → no
        _add_bytes(t, f"{LIBREL}/nss/libsoftokn3.so", b"S")     # subdir → no
    flat = _flat_lib_basenames(p)
    assert {"libdav1d.so.6", "libc.so.6"} <= flat
    assert "libpulsecommon-16.1.so" not in flat   # buried in pulseaudio/
    assert "libsoftokn3.so" not in flat           # buried in nss/


def test_gate_flags_origin_only_lib_before_flatten(origin_overlay, base_with_libc):
    # THE REGRESSION GUARD: before flattening, the flat-aware gate must report the
    # $ORIGIN-only lib as unsatisfied (the device-true state the old gate hid).
    assert _unsatisfied_needed(origin_overlay, base_with_libc) == ("libtest_trace_processor.so",)


def test_origin_plan_targets_the_lib_from_origin(origin_overlay):
    plan = _origin_needed_to_flatten(origin_overlay)
    assert plan == {"libtest_trace_processor.so": f"{ORIGIN_DIR}/libtest_trace_processor.so"}


def test_flatten_adds_flat_copy_and_keeps_origin(origin_overlay):
    flattened = _flatten_origin_needed(origin_overlay, _origin_needed_to_flatten(origin_overlay))
    assert flattened == ("libtest_trace_processor.so",)
    names = _member_names(origin_overlay)
    # the flat copy the loader CAN find …
    assert f"./{LIBREL}/libtest_trace_processor.so" in names
    assert _read_member(origin_overlay, f"{LIBREL}/libtest_trace_processor.so") == b"TTP"
    # … and the original $ORIGIN copy is left in place (harmless).
    assert f"./{ORIGIN_DIR}/libtest_trace_processor.so" in names


def test_gate_clears_after_flatten(origin_overlay, base_with_libc):
    _flatten_origin_needed(origin_overlay, _origin_needed_to_flatten(origin_overlay))
    assert _unsatisfied_needed(origin_overlay, base_with_libc) == ()


def test_flatten_is_idempotent(origin_overlay):
    _flatten_origin_needed(origin_overlay, _origin_needed_to_flatten(origin_overlay))
    # second pass: the flat copy already exists → nothing to add
    assert _flatten_origin_needed(origin_overlay, _origin_needed_to_flatten(origin_overlay)) == ()


def test_flatten_never_invents_a_missing_lib(tmp_path):
    # A NEEDED with no $ORIGIN copy must NOT be conjured — it stays a genuine miss.
    p = tmp_path / "ov.tar"
    with tarfile.open(p, "w") as t:
        _add_bytes(t, ENTRYPOINT.lstrip("/"),
                   _synthetic_elf(("libabsent.so.9",)), mode=0o755)
    assert _origin_needed_to_flatten(p) == {}
