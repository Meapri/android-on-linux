"""Host tests for tools/build_fakeroot_overlay.py (R12 non-root fakeroot shim).

OFFLINE except the optional zig-compile path (auto-skipped when zig is absent).
Verifies that the builder:
  * compiles/packs the fakeroot shim to ./usr/lib/androlinux/libalr_fakeroot.so,
    §5-E conformant, 0o755;
  * the shim SOURCE intercepts every credential/ownership/stat symbol a non-root
    dpkg unpack needs (regression guard against a deleted wrapper);
  * the device command CHAINS the fakeroot .so with the ALR interpose .so
    (fakeroot first, interpose kept) — never replaces it;
  * the shim is W^X-safe (no exec memory / seccomp / ptrace).

The actual on-device non-root `dpkg -i hello.deb` (unpacked=true) under the
chained preload is WS-1's device drain, not run here.
"""

from __future__ import annotations

import shutil
import tarfile
from pathlib import Path

import pytest

from tools.build_fakeroot_overlay import (
    INTERPOSE_REL,
    REQUIRED_SYMBOLS,
    ROOTFS_MEMBER,
    ROOTFS_REL,
    SHIM_SRC,
    build_fakeroot_overlay,
    device_cmd,
    intercepted_symbols,
    missing_symbols,
    pack_overlay,
    shim_source,
)
from tools.stage_tar_spec import validate_stage_tar


# --------------------------------------------------------------------------- #
# Shim source intercepts the right symbols
# --------------------------------------------------------------------------- #

def test_shim_source_exists():
    assert SHIM_SRC.is_file(), SHIM_SRC


def test_shim_intercepts_all_required_symbols():
    have = intercepted_symbols()
    for s in REQUIRED_SYMBOLS:
        assert s in have, s
    assert missing_symbols() == ()


def test_required_symbols_cover_credentials_ownership_and_stat():
    # the three pillars of a fakeroot unpack
    for cred in ("getuid", "geteuid", "getgid", "getegid"):
        assert cred in REQUIRED_SYMBOLS
    for own in ("chown", "lchown", "fchown", "fchownat"):
        assert own in REQUIRED_SYMBOLS
    for st in ("stat", "lstat", "fstatat", "statx"):
        assert st in REQUIRED_SYMBOLS


def test_credential_getters_return_faked_uid():
    src = shim_source()
    assert "uid_t getuid(void)" in src
    assert "return g_fake_uid;" in src
    assert "uid_t geteuid(void)" in src


def test_chown_fakes_success_and_records_owner():
    src = shim_source()
    # chown records into the DB and returns 0 unconditionally
    assert "int chown(const char *path" in src
    assert "fr_set_owner(" in src


def test_shim_chains_via_rtld_next_not_replacing_interposer():
    src = shim_source()
    # Every wrapper reaches the next impl via RTLD_NEXT — that is what makes the
    # shim CHAIN onto libalr_interpose instead of bypassing it.
    assert "RTLD_NEXT" in src
    assert "dlsym(RTLD_NEXT" in src


def test_shim_honors_fakeroot_env_contract():
    src = shim_source()
    assert "FAKEROOTUID" in src
    assert "FAKEROOTGID" in src


def test_shim_is_wx_safe_no_exec_mem_seccomp_ptrace():
    from tools.build_fakeroot_overlay import _strip_c_comments

    # Strip comments first: the header comment legitimately mentions seccomp /
    # ptrace to promise their absence. Assert there is no CALL SITE in code.
    code = _strip_c_comments(shim_source())
    assert "seccomp(" not in code
    assert "ptrace(" not in code
    assert "prctl(" not in code
    assert "mmap(" not in code            # no exec-memory allocation


# --------------------------------------------------------------------------- #
# Packing (no compiler needed — uses an ELF-ish stub)
# --------------------------------------------------------------------------- #

def test_pack_overlay_places_so_at_rootfs_absolute_path(tmp_path: Path):
    fake_so = tmp_path / "libalr_fakeroot.so"
    fake_so.write_bytes(b"\x7fELF" + b"\x00" * 64)
    out = tmp_path / "fakeroot-stage.tar"
    member = pack_overlay(fake_so, out)
    assert member == ROOTFS_MEMBER == "./usr/lib/androlinux/libalr_fakeroot.so"
    with tarfile.open(out) as t:
        names = {m.name: m for m in t.getmembers()}
    assert ROOTFS_MEMBER in names
    assert names[ROOTFS_MEMBER].isfile()
    assert names[ROOTFS_MEMBER].mode == 0o755
    assert names["./usr/lib/androlinux"].isdir()
    assert all(n.startswith("./") for n in names)


def test_packed_overlay_is_stage_tar_conformant(tmp_path: Path):
    fake_so = tmp_path / "libalr_fakeroot.so"
    fake_so.write_bytes(b"\x7fELF" + b"\x00" * 64)
    out = tmp_path / "fakeroot-stage.tar"
    pack_overlay(fake_so, out)
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# Device command chains the two preloads (fakeroot first, interpose kept)
# --------------------------------------------------------------------------- #

def test_device_cmd_chains_fakeroot_first_keeps_interpose():
    cmd = device_cmd("/data/r")
    fr = "/data/r/" + ROOTFS_REL
    ip = "/data/r/" + INTERPOSE_REL
    assert fr in cmd
    assert ip in cmd                       # interpose NOT dropped
    assert f"{fr}:{ip}" in cmd             # fakeroot first, colon-joined
    assert "LD_PRELOAD=" in cmd


def test_device_cmd_runs_dpkg_unpack():
    cmd = device_cmd("/data/r")
    assert "dpkg" in cmd
    assert "-i" in cmd
    assert "FAKEROOTUID=0" in cmd


def test_device_cmd_paths_are_rootfs_absolute():
    cmd = device_cmd("/mnt/rootfs")
    # both preload entries are <rootfs>-absolute host paths (R3 finding: a guest
    # path would hit the host fs and fail to load)
    assert "/mnt/rootfs/usr/lib/androlinux/libalr_fakeroot.so" in cmd
    assert "/mnt/rootfs/usr/lib/androlinux/libalr_interpose.so" in cmd


# --------------------------------------------------------------------------- #
# Builder selftest + optional real compile
# --------------------------------------------------------------------------- #

def test_builder_selftest_passes():
    from tools.build_fakeroot_overlay import _selftest

    assert _selftest() == 0


@pytest.mark.skipif(shutil.which("zig") is None, reason="needs zig to cross-compile")
def test_zig_build_produces_aarch64_elf_so(tmp_path: Path):
    out = tmp_path / "fakeroot-stage.tar"
    res = build_fakeroot_overlay(out, keep_so=tmp_path / "libalr_fakeroot.so")
    assert res.so_bytes > 0
    assert len(res.so_sha256) == 64
    with tarfile.open(out) as t:
        body = t.extractfile(ROOTFS_MEMBER).read()
    assert body[:4] == b"\x7fELF"          # ELF magic
    assert body[4] == 2                      # ELFCLASS64
    assert body[18] == 183                   # EM_AARCH64
