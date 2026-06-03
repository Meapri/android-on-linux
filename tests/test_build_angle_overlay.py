"""Host tests for tools/build_angle_overlay.py (system-ANGLE staging overlay).

All OFFLINE — the overlay assembly + §5-E conformance is exercised against
SYNTHETIC fake-ELF bytes, never a .deb download. The NETWORK pack (real Debian
bookworm chromium-common ANGLE libEGL.so/libGLESv2.so + libxnvctrl0 →
angle-stage.tar, 0 missing .so over base∪overlay) is verified by hand on the
host; the GLES2 app loads ANGLE → Vulkan → our guest ICD on-device gate is the
device/compositor test.

Contract (DECIDED strategy: Vulkan-first + ANGLE-for-GLES):
  * the ANGLE real .so are placed at the VERSIONED SONAMEs the loader/apps expect
    (libEGL.so.1 / libGLESv2.so.2), mode 0755 (the ALR file-backed PROT_EXEC
    requirement), under /usr/lib/androlinux — mirroring the gpushim/vk-icd layout;
  * unversioned dev/runtime symlinks libEGL.so / libGLESv2.so are relative + in-dir;
  * the one base-missing DT_NEEDED of ANGLE's libGLESv2, libXNVCtrl.so.0, is
    shipped FLAT (real file at the bare SONAME) so there is no §5-E non-flat
    warning and no base downgrade;
  * the produced tar is §5-E ./-rooted and stage_tar_spec CONFORMANT with NO
    warnings;
  * the offline byte-source path (--so-egl/--so-gles/--so-nvctrl) builds an
    identical-shape overlay without touching the network.
"""

from __future__ import annotations

import tarfile
from pathlib import Path

import pytest

from tools import build_angle_overlay as bao
from tools.build_angle_overlay import (
    ANDROLINUX_DIR,
    EGL_SONAME,
    EGL_UNVERSIONED,
    GLES_SONAME,
    GLES_UNVERSIONED,
    NVCTRL_SONAME,
    build_overlay,
)
from tools.stage_tar_spec import validate_stage_tar


# Synthetic fake-ELF blobs (valid magic; not loadable — shape tests only).
FAKE_EGL = b"\x7fELF" + b"E" * 512
FAKE_GLES = b"\x7fELF" + b"G" * 8192
FAKE_NVCTRL = b"\x7fELF" + b"N" * 512


def _build(tmp_path: Path) -> Path:
    out = tmp_path / "angle-stage.tar"
    build_overlay(out, egl_bytes=FAKE_EGL, gles_bytes=FAKE_GLES, nvctrl_bytes=FAKE_NVCTRL)
    return out


def _members(tar_path: Path) -> dict[str, tarfile.TarInfo]:
    with tarfile.open(tar_path, "r") as t:
        return {m.name: m for m in t.getmembers()}


# --------------------------------------------------------------------------- #
# Module + constants
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bao.__file__).is_file()


def test_target_sonames_are_the_standard_versioned_linux_names():
    # A normal GLES app links -lEGL -lGLESv2 → DT_NEEDED records these; the ANGLE
    # bytes must land at these filenames (ld.so resolves NEEDED by filename).
    assert EGL_SONAME == "libEGL.so.1"
    assert GLES_SONAME == "libGLESv2.so.2"
    assert NVCTRL_SONAME == "libXNVCtrl.so.0"


def test_angle_source_is_debian_chromium_common():
    # ANGLE is chromium's bundled libEGL/libGLESv2 — for bookworm 147 they live in
    # the chromium-common .deb (not the main chromium .deb).
    assert bao.ANGLE_PACKAGE == "chromium-common"
    assert bao.ANGLE_DEB_EGL == "libEGL.so"
    assert bao.ANGLE_DEB_GLES == "libGLESv2.so"
    assert bao.NVCTRL_PACKAGE == "libxnvctrl0"


# --------------------------------------------------------------------------- #
# Overlay shape
# --------------------------------------------------------------------------- #

def test_all_members_are_dot_rooted_relative(tmp_path):
    out = _build(tmp_path)
    for name in _members(out):
        assert name.startswith("./"), name
        assert ".." not in name.split("/"), name


def test_angle_libs_are_real_files_at_versioned_sonames(tmp_path):
    m = _members(_build(tmp_path))
    for so, blob in ((EGL_SONAME, FAKE_EGL), (GLES_SONAME, FAKE_GLES)):
        ti = m[f"./{ANDROLINUX_DIR}/{so}"]
        assert ti.isfile(), so
        assert ti.size == len(blob)


def test_so_files_are_executable_0755(tmp_path):
    # The ALR file-backed PROT_EXEC under untrusted_app rejects a non-exec .so.
    m = _members(_build(tmp_path))
    for so in (EGL_SONAME, GLES_SONAME, NVCTRL_SONAME):
        ti = m[f"./{ANDROLINUX_DIR}/{so}"]
        assert ti.mode & 0o111, f"{so} not executable (mode {oct(ti.mode)})"


def test_unversioned_symlinks_are_relative_in_dir(tmp_path):
    m = _members(_build(tmp_path))
    for link, want in ((EGL_UNVERSIONED, EGL_SONAME), (GLES_UNVERSIONED, GLES_SONAME)):
        ti = m[f"./{ANDROLINUX_DIR}/{link}"]
        assert ti.issym(), link
        assert ti.linkname == want
        assert "/" not in ti.linkname  # in-dir, not escaping


def test_libxnvctrl_shipped_flat_not_versioned_layout(tmp_path):
    # Flat real file at the bare SONAME — NOT the Debian
    # libXNVCtrl.so.0 -> libXNVCtrl.so.0.0.0 symlink+versioned pair.
    m = _members(_build(tmp_path))
    ti = m[f"./{ANDROLINUX_DIR}/{NVCTRL_SONAME}"]
    assert ti.isfile(), "libXNVCtrl.so.0 must be a real flat file"
    assert f"./{ANDROLINUX_DIR}/libXNVCtrl.so.0.0.0" not in m


# --------------------------------------------------------------------------- #
# §5-E conformance
# --------------------------------------------------------------------------- #

def test_stage_tar_spec_conformant_no_warnings(tmp_path):
    out = _build(tmp_path)
    report = validate_stage_tar(out)
    assert report.conformant, report.errors
    # Flat real .so at bare SONAMEs → no non-flat-SONAME warning.
    assert report.warnings == [], report.warnings


def test_selftest_passes():
    # The module's own offline selftest must pass (exit 0).
    assert bao._selftest() == 0


# --------------------------------------------------------------------------- #
# Offline byte-source CLI path
# --------------------------------------------------------------------------- #

def test_offline_cli_builds_identical_shape(tmp_path):
    egl = tmp_path / "libEGL.so"
    gles = tmp_path / "libGLESv2.so"
    nvctrl = tmp_path / "libXNVCtrl.so.0.0.0"
    egl.write_bytes(FAKE_EGL)
    gles.write_bytes(FAKE_GLES)
    nvctrl.write_bytes(FAKE_NVCTRL)
    out = tmp_path / "cli-angle-stage.tar"

    rc = bao.main([
        "--out", str(out),
        "--so-egl", str(egl),
        "--so-gles", str(gles),
        "--so-nvctrl", str(nvctrl),
        "--base", "",  # skip the downgrade guard (no base in this offline test)
    ])
    assert rc == 0
    m = _members(out)
    assert f"./{ANDROLINUX_DIR}/{EGL_SONAME}" in m
    assert f"./{ANDROLINUX_DIR}/{GLES_SONAME}" in m
    assert f"./{ANDROLINUX_DIR}/{NVCTRL_SONAME}" in m
    assert m[f"./{ANDROLINUX_DIR}/{GLES_SONAME}"].size == len(FAKE_GLES)


def test_offline_cli_requires_all_three_sources(tmp_path):
    egl = tmp_path / "libEGL.so"
    egl.write_bytes(FAKE_EGL)
    # Only --so-egl given → must error (needs all three).
    with pytest.raises(SystemExit):
        bao.main(["--out", str(tmp_path / "x.tar"), "--so-egl", str(egl), "--base", ""])
