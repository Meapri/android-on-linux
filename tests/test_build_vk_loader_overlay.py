"""Host tests for tools/build_vk_loader_overlay.py (Khronos Vulkan-Loader overlay).

All OFFLINE — the overlay assembly + §5-E conformance is exercised against
SYNTHETIC fake-ELF bytes, never a .deb download. The NETWORK pack (real Ubuntu
noble libvulkan1 = upstream Khronos Vulkan-Loader → vk-loader-stage.tar, 0 missing
.so over base∪overlay) is confirmed by hand on the host (see the module docstring's
audit). The device gate (ANGLE dlopen("libvulkan.so.1") → our staged Khronos loader
→ alr_icd.json → libalr_mali_icd.so → Mali) is the device/compositor test.

This overlay is "Part B" of the ICD discovery redirect: it ships the REAL Khronos
loader on the guest path + a manifest the loader discovers via VK_DRIVER_FILES so it
loads our (renamed) guest Mali ICD instead of the host /system/lib64 loader.

Contract:
  * the Khronos loader real .so is placed at the bare SONAME libvulkan.so.1, mode
    0755 (the ALR file-backed PROT_EXEC requirement), under /usr/lib/androlinux —
    mirroring the gpushim/vk-icd/angle layout;
  * an unversioned dev/runtime symlink libvulkan.so is relative + in-dir;
  * the alr_icd.json manifest's library_path is the ABSOLUTE path of the (wave-2
    renamed) ICD /usr/lib/androlinux/libalr_mali_icd.so — NOT the loader's own
    libvulkan.so.1 (a relative ./libvulkan.so.1 would make the loader load itself);
  * file_format_version is 1.0.1;
  * the produced tar is §5-E ./-rooted and stage_tar_spec CONFORMANT with NO
    warnings (the base ships no libvulkan → zero conflict);
  * the offline byte-source path (--so) builds an identical-shape overlay without
    touching the network.
"""

from __future__ import annotations

import json
import tarfile
from pathlib import Path

import pytest

from tools import build_vk_loader_overlay as bvl
from tools.build_vk_loader_overlay import (
    ANDROLINUX_DIR,
    ICD_BASENAME,
    ICD_LIBRARY_PATH,
    MANIFEST,
    SONAME,
    UNVERSIONED,
    build_overlay,
)
from tools.stage_tar_spec import validate_stage_tar


# Synthetic fake-ELF blob (valid magic; not a loadable loader — shape tests only).
FAKE_LOADER = b"\x7fELF" + b"L" * 4096


def _build(tmp_path: Path) -> Path:
    out = tmp_path / "vk-loader-stage.tar"
    build_overlay(out, loader_bytes=FAKE_LOADER)
    return out


def _members(tar_path: Path) -> dict[str, tarfile.TarInfo]:
    with tarfile.open(tar_path, "r") as t:
        return {m.name: m for m in t.getmembers()}


# --------------------------------------------------------------------------- #
# Module + constants
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bvl.__file__).is_file()


def test_target_soname_is_the_standard_versioned_linux_name():
    # A guest links -lvulkan / dlopens "libvulkan.so.1" → DT_NEEDED records this; the
    # Khronos loader bytes must land at this filename (ld.so resolves NEEDED by name).
    assert SONAME == "libvulkan.so.1"
    assert UNVERSIONED == "libvulkan.so"
    assert ANDROLINUX_DIR == "usr/lib/androlinux"


def test_loader_source_is_noble_libvulkan1():
    # The upstream Khronos Vulkan-Loader is the noble libvulkan1 .deb (Source:
    # vulkan-loader), arm64, component main, from ports.ubuntu.com.
    assert bvl.LOADER_PACKAGE == "libvulkan1"
    assert bvl.SUITE == "noble"
    assert bvl.ARCH == "arm64"
    assert bvl.COMPONENTS == ("main",)
    assert "ports.ubuntu.com" in bvl.MIRROR


# --------------------------------------------------------------------------- #
# Overlay shape
# --------------------------------------------------------------------------- #

def test_all_members_are_dot_rooted_relative(tmp_path):
    out = _build(tmp_path)
    for name in _members(out):
        assert name.startswith("./"), name
        assert ".." not in name.split("/"), name


def test_loader_is_real_file_at_bare_soname(tmp_path):
    m = _members(_build(tmp_path))
    ti = m[f"./{ANDROLINUX_DIR}/{SONAME}"]
    assert ti.isfile(), "libvulkan.so.1 must be a real file"
    assert ti.size == len(FAKE_LOADER)
    # Flat real file at the bare SONAME — NOT the Debian versioned layout pair.
    assert f"./{ANDROLINUX_DIR}/libvulkan.so.1.3.275" not in m


def test_loader_so_is_executable_0755(tmp_path):
    # The ALR file-backed PROT_EXEC under untrusted_app rejects a non-exec .so.
    m = _members(_build(tmp_path))
    ti = m[f"./{ANDROLINUX_DIR}/{SONAME}"]
    assert ti.mode & 0o111, f"{SONAME} not executable (mode {oct(ti.mode)})"


def test_unversioned_symlink_is_relative_in_dir(tmp_path):
    m = _members(_build(tmp_path))
    ti = m[f"./{ANDROLINUX_DIR}/{UNVERSIONED}"]
    assert ti.issym(), UNVERSIONED
    assert ti.linkname == SONAME
    assert "/" not in ti.linkname  # in-dir, not escaping


# --------------------------------------------------------------------------- #
# ICD manifest — the discovery redirect (must point at the renamed ICD)
# --------------------------------------------------------------------------- #

def test_manifest_present_and_parses(tmp_path):
    m = _members(_build(tmp_path))
    man_member = f"./{ANDROLINUX_DIR}/{MANIFEST}"
    assert man_member in m, "alr_icd.json must be present"
    with tarfile.open(_build(tmp_path), "r") as t:
        man = json.loads(t.extractfile(man_member).read())
    assert man["file_format_version"] == "1.0.1"
    assert man["ICD"]["api_version"] == "1.3.0"


def test_manifest_library_path_is_absolute_and_names_the_renamed_icd(tmp_path):
    # The library_path must be the ABSOLUTE path of our (wave-2 renamed) guest ICD,
    # libalr_mali_icd.so — distinct from the loader's own libvulkan.so.1 so the two
    # coexist. A ./libvulkan.so.1 here would make the loader load ITSELF as the ICD.
    out = _build(tmp_path)
    man_member = f"./{ANDROLINUX_DIR}/{MANIFEST}"
    with tarfile.open(out, "r") as t:
        man = json.loads(t.extractfile(man_member).read())
    lib = man["ICD"]["library_path"]
    assert lib == ICD_LIBRARY_PATH
    assert lib == "/usr/lib/androlinux/libalr_mali_icd.so"
    assert lib.startswith("/"), "library_path must be absolute"
    assert ICD_BASENAME in lib
    assert SONAME not in lib, "manifest must NOT point at the loader itself"


# --------------------------------------------------------------------------- #
# §5-E conformance
# --------------------------------------------------------------------------- #

def test_stage_tar_spec_conformant_no_warnings(tmp_path):
    out = _build(tmp_path)
    report = validate_stage_tar(out)
    assert report.conformant, report.errors
    # Flat real .so at the bare SONAME → no non-flat-SONAME warning.
    assert report.warnings == [], report.warnings


def test_selftest_passes():
    # The module's own offline selftest must pass (exit 0).
    assert bvl._selftest() == 0


# --------------------------------------------------------------------------- #
# Offline byte-source CLI path
# --------------------------------------------------------------------------- #

def test_offline_cli_builds_identical_shape(tmp_path):
    so = tmp_path / "libvulkan.so.1"
    so.write_bytes(FAKE_LOADER)
    out = tmp_path / "cli-vk-loader-stage.tar"

    rc = bvl.main([
        "--out", str(out),
        "--so", str(so),
        "--base", "",  # skip the downgrade guard (no base in this offline test)
    ])
    assert rc == 0
    m = _members(out)
    assert f"./{ANDROLINUX_DIR}/{SONAME}" in m
    assert f"./{ANDROLINUX_DIR}/{UNVERSIONED}" in m
    assert f"./{ANDROLINUX_DIR}/{MANIFEST}" in m
    assert m[f"./{ANDROLINUX_DIR}/{SONAME}"].size == len(FAKE_LOADER)


def test_offline_cli_validates_against_base_no_conflict(tmp_path):
    # The base ships no libvulkan, so the overlay is a clean drop: building with a
    # synthetic base that lacks libvulkan must pass the downgrade guard (rc 0).
    so = tmp_path / "libvulkan.so.1"
    so.write_bytes(FAKE_LOADER)
    base = tmp_path / "base.tar"
    with tarfile.open(base, "w") as t:
        # a base with some unrelated lib, NO libvulkan
        info = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libc.so.6")
        payload = b"\x7fELF" + b"C" * 32
        info.size = len(payload)
        t.addfile(info, __import__("io").BytesIO(payload))
    out = tmp_path / "vk-loader-vs-base.tar"
    rc = bvl.main(["--out", str(out), "--so", str(so), "--base", str(base)])
    assert rc == 0
    report = validate_stage_tar(out, base)
    assert report.conformant, report.errors
    assert report.warnings == [], report.warnings
