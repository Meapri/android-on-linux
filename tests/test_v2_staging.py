"""End-to-end §5-E contract for the v2 apt/dpkg install staging tars.

This is the *integration-contract* test the device drain stands on: it asserts
the SHAPE of the two staging tars a device must extract before a non-root
`apt install` / `dpkg -i hello.deb` can even be attempted —

  1. ``fakeroot-stage.tar``   (tools/build_fakeroot_overlay.py)
       ./usr/lib/androlinux/libalr_fakeroot.so  — the credential/chown/stat shim
       that chains onto libalr_interpose.so (fakeroot first, interpose kept).
  2. ``apt-dpkg-stage.tar``   (tools/build_apt_dpkg_overlay.py)
       the apt+dpkg runtime closure (base-subtracted) + the dpkg admindir
       scaffold, optionally self-contained front-end binaries.

The per-builder internals are already covered by test_build_fakeroot_overlay.py
and test_build_apt_dpkg_overlay.py. THIS file pins the cross-builder §5-E regime
the integration session relies on:

  * every staging member is ``./``-rooted, relative, no ``..``, no device nodes;
  * the fakeroot .so lands at exactly ./usr/lib/androlinux/ (next to the
    interposer the loader injects), flat SONAME, 0o755;
  * symlink targets stay in-tree;
  * the admindir scaffold gives dpkg a working DB root;
  * the device LD_PRELOAD chain keeps BOTH .so (fakeroot first) — never replaces
    the interposer.

Gating (so the suite stays green on any host):
  * the real fakeroot .so compile is skipped when ``zig`` is absent (the stub-.so
    pack path still gates the §5-E shape);
  * the live noble closure download + ``hello`` .deb fetch is opt-in behind
    ``ALR_V2_STAGING_NET=1`` (NETWORK). Offline it is asserted via a synthetic
    Packages index — no socket needed.

HONEST SCOPE: HOST-ONLY. None of this *proves the on-device unpack passes* — that
needs the device (exec-re-entry re-map + the chained preload). It proves the
staging tars are byte-shaped exactly as the device extractor + the WS-1 drain
expect, so the drain is unblocked the moment the loader re-map lands.
"""

from __future__ import annotations

import os
import shutil
import tarfile
from pathlib import Path

import pytest

from tools.build_apt_dpkg_overlay import (
    ADMINDIR_DIRS,
    ADMINDIR_FILES,
    SELF_CONTAINED_BINS,
    TEST_DEB,
    build_apt_dpkg_overlay,
    fetch_test_deb,
    resolve_apt_dpkg_closure,
    write_admindir_scaffold,
)
from tools.build_fakeroot_overlay import (
    INTERPOSE_REL,
    ROOTFS_MEMBER as FAKEROOT_MEMBER,
    ROOTFS_REL as FAKEROOT_REL,
    build_fakeroot_overlay,
    device_cmd,
    pack_overlay as pack_fakeroot,
)
from tools.safe_tar import inspect_tar_members
from tools.stage_tar_spec import validate_stage_tar

NET = os.environ.get("ALR_V2_STAGING_NET") == "1"
HAVE_ZIG = shutil.which("zig") is not None
HAVE_AR = shutil.which("ar") is not None
net_only = pytest.mark.skipif(
    not NET, reason="set ALR_V2_STAGING_NET=1 to run the live noble closure/fetch"
)


# --------------------------------------------------------------------------- #
# Helpers — assert the §5-E root layout invariants shared by every staging tar
# --------------------------------------------------------------------------- #

def _members(tar: str | Path):
    with tarfile.open(tar) as t:
        return {m.name: m for m in t.getmembers()}


def _assert_dot_rooted_and_safe(tar: str | Path) -> None:
    """Every member is ``./``-rooted, relative, ``..``-free, device-node-free."""
    names = list(_members(tar))
    assert names, f"{tar} is empty"
    for n in names:
        assert n.startswith("./"), f"member not ./-rooted: {n}"
        assert not n.startswith("/"), f"absolute member: {n}"
        assert ".." not in Path(n).parts, f"`..` escape in member: {n}"
    # inspect_tar_members raises UnsafeTarArchive on abs/.. /device — a clean
    # return is the strongest single assertion of root-layout safety.
    inspect_tar_members(tar)


# --------------------------------------------------------------------------- #
# 1. fakeroot-stage.tar — the libalr_fakeroot.so credential shim
# --------------------------------------------------------------------------- #

def test_fakeroot_stage_dot_rooted_and_conformant(tmp_path: Path):
    # Pack a stub ELF .so (no compiler needed) — pin the §5-E SHAPE.
    so = tmp_path / "libalr_fakeroot.so"
    so.write_bytes(b"\x7fELF\x02\x01\x01" + b"\x00" * 256)
    out = tmp_path / "fakeroot-stage.tar"
    member = pack_fakeroot(so, out)

    assert member == FAKEROOT_MEMBER == "./usr/lib/androlinux/libalr_fakeroot.so"
    _assert_dot_rooted_and_safe(out)
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


def test_fakeroot_so_lands_next_to_the_interposer(tmp_path: Path):
    # The loader injects ./usr/lib/androlinux/libalr_interpose.so; the fakeroot
    # .so MUST install into the SAME dir so the chain resolves on device.
    so = tmp_path / "libalr_fakeroot.so"
    so.write_bytes(b"\x7fELF" + b"\x00" * 64)
    out = tmp_path / "fakeroot-stage.tar"
    pack_fakeroot(so, out)

    fr_dir = str(Path(FAKEROOT_REL).parent)              # usr/lib/androlinux
    ip_dir = str(Path(INTERPOSE_REL).parent)
    assert fr_dir == ip_dir == "usr/lib/androlinux"
    names = _members(out)
    assert FAKEROOT_MEMBER in names
    assert names[FAKEROOT_MEMBER].isfile()
    assert names[FAKEROOT_MEMBER].mode == 0o755           # exec-loadable .so
    assert names["./usr/lib/androlinux"].isdir()          # parent dirs shipped


def test_fakeroot_stage_ships_a_flat_soname_no_versioned_symlink(tmp_path: Path):
    # libalr_fakeroot.so is a flat real file — no libNAME.so.X -> ...so.X.Y.Z
    # symlink pair that the overlay guard would flag as a base-downgrade risk.
    so = tmp_path / "libalr_fakeroot.so"
    so.write_bytes(b"\x7fELF" + b"\x00" * 64)
    out = tmp_path / "fakeroot-stage.tar"
    pack_fakeroot(so, out)
    for m in _members(out).values():
        assert not m.issym(), f"unexpected symlink in fakeroot stage: {m.name}"
    rep = validate_stage_tar(str(out))
    assert rep.warnings == [], rep.warnings        # no non-flat-SONAME warning


def test_device_chain_keeps_interpose_fakeroot_first():
    rootfs = "/data/data/dev.chanwoo.androlinux/files/rootfs"
    cmd = device_cmd(rootfs)
    fr = f"{rootfs}/{FAKEROOT_REL}"
    ip = f"{rootfs}/{INTERPOSE_REL}"
    assert fr in cmd and ip in cmd                  # interpose NOT dropped
    assert f"{fr}:{ip}" in cmd                       # fakeroot FIRST, colon-joined
    # both entries are rootfs-ABSOLUTE host paths (R3: a guest path won't load)
    assert "/usr/lib/androlinux/libalr_fakeroot.so" in cmd
    assert "/usr/lib/androlinux/libalr_interpose.so" in cmd
    assert "FAKEROOTUID=0" in cmd and "dpkg" in cmd


@pytest.mark.skipif(not HAVE_ZIG, reason="needs zig to cross-compile the shim")
def test_fakeroot_stage_real_compile_is_aarch64_elf(tmp_path: Path):
    out = tmp_path / "fakeroot-stage.tar"
    res = build_fakeroot_overlay(out, keep_so=tmp_path / "libalr_fakeroot.so")
    assert res.so_bytes > 0 and len(res.so_sha256) == 64
    _assert_dot_rooted_and_safe(out)
    body = _members(out)[FAKEROOT_MEMBER]
    with tarfile.open(out) as t:
        elf = t.extractfile(FAKEROOT_MEMBER).read()
    assert body.mode == 0o755
    assert elf[:4] == b"\x7fELF"
    assert elf[4] == 2                                # ELFCLASS64
    assert elf[18] == 183                             # EM_AARCH64 — guest arch
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# 2. apt-dpkg-stage.tar — admindir scaffold (offline) + the closure SHAPE
# --------------------------------------------------------------------------- #

def test_admindir_scaffold_gives_dpkg_a_db_root(tmp_path: Path):
    out = tmp_path / "apt-admindir.tar"
    written = write_admindir_scaffold(out)
    _assert_dot_rooted_and_safe(out)
    names = _members(out)
    # the dirs dpkg refuses to run without
    for d in ADMINDIR_DIRS:
        assert names["./" + d].isdir(), d
    # the seed files (status/available/arch/triggers/*)
    for f in ADMINDIR_FILES:
        assert ("./" + f) in names, f
    # arch pins arm64 — the guest architecture dpkg records installs against
    with tarfile.open(out) as t:
        arch_body = t.extractfile("./var/lib/dpkg/arch").read()
    assert arch_body == b"arm64\n"
    assert all(w.startswith("./") for w in written)
    assert validate_stage_tar(str(out)).conformant


def test_admindir_no_status_drops_status_keeps_arch(tmp_path: Path):
    out = tmp_path / "apt-admindir-nostatus.tar"
    write_admindir_scaffold(out, include_status=False)
    names = _members(out)
    assert "./var/lib/dpkg/status" not in names
    assert "./var/lib/dpkg/available" not in names
    assert "./var/lib/dpkg/arch" in names             # arch still pinned


def test_self_contained_set_covers_the_unpack_toolchain():
    # The maintainer-script exec chain (apt -> dpkg -> dpkg-deb/tar -> sh) must be
    # shippable even on a slimmed base. Assert the front-end map covers it.
    flat = {p for paths in SELF_CONTAINED_BINS.values() for p in paths}
    for need in (
        "usr/bin/dpkg", "usr/bin/dpkg-deb", "usr/bin/dpkg-split",
        "usr/bin/dpkg-query", "usr/bin/apt", "usr/bin/apt-get", "usr/bin/tar",
    ):
        assert need in flat, need


def test_closure_shape_offline_against_a_synthetic_index():
    # No network: feed resolve_apt_dpkg_closure a synthetic Packages index and
    # assert the front-ends pull their transitive runtime libs with 0 unsat deps.
    index = {
        "apt": {"Package": "apt", "Depends": "libapt-pkg6.0t64, libc6, gpgv",
                "Size": "1336188", "Filename": "pool/main/a/apt/apt.deb"},
        "apt-utils": {"Package": "apt-utils", "Depends": "apt, libapt-pkg6.0t64",
                      "Size": "205248", "Filename": "pool/main/a/apt/apt-utils.deb"},
        "dpkg": {"Package": "dpkg", "Depends": "libc6, tar",
                 "Size": "1265468", "Filename": "pool/main/d/dpkg/dpkg.deb"},
        "libapt-pkg6.0t64": {"Package": "libapt-pkg6.0t64", "Depends": "libc6, libzstd1",
                             "Size": "934734", "Filename": "pool/main/a/apt/libapt.deb"},
        "libc6": {"Package": "libc6", "Size": "2774086", "Filename": "pool/main/g/glibc/libc6.deb"},
        "libzstd1": {"Package": "libzstd1", "Depends": "libc6",
                     "Size": "271224", "Filename": "pool/main/libz/libzstd1.deb"},
        "gpgv": {"Package": "gpgv", "Depends": "libc6",
                 "Size": "149882", "Filename": "pool/main/g/gnupg2/gpgv.deb"},
        "tar": {"Package": "tar", "Depends": "libc6",
                "Size": "247906", "Filename": "pool/main/t/tar/tar.deb"},
        # the rest of DEFAULT_TARGETS so the closure resolves with 0 unsat
        "gzip": {"Package": "gzip", "Depends": "libc6", "Size": "97192",
                 "Filename": "pool/main/g/gzip/gzip.deb"},
        "xz-utils": {"Package": "xz-utils", "Depends": "libc6", "Size": "268364",
                     "Filename": "pool/main/x/xz/xz.deb"},
        "zstd": {"Package": "zstd", "Depends": "libc6, libzstd1", "Size": "574810",
                 "Filename": "pool/main/libz/zstd.deb"},
        "coreutils": {"Package": "coreutils", "Depends": "libc6", "Size": "1362772",
                      "Filename": "pool/main/c/coreutils/coreutils.deb"},
        "sed": {"Package": "sed", "Depends": "libc6", "Size": "171910",
                "Filename": "pool/main/s/sed/sed.deb"},
        "bash": {"Package": "bash", "Depends": "libc6", "Size": "780262",
                 "Filename": "pool/main/b/bash/bash.deb"},
        "dash": {"Package": "dash", "Depends": "libc6", "Size": "90376",
                 "Filename": "pool/main/d/dash/dash.deb"},
    }
    plan = resolve_apt_dpkg_closure(index=index)
    assert plan.missing == (), f"unsatisfied deps: {plan.missing}"
    # the front-ends + their transitive libs are all present
    for pkg in ("apt", "dpkg", "libapt-pkg6.0t64", "libc6", "libzstd1", "gpgv", "tar"):
        assert pkg in plan.closure, pkg
    assert plan.total_download_bytes > 0


def test_test_deb_is_the_trivial_local_hello():
    # The on-device unpack target is a separate asset (NOT packed in the overlay).
    assert TEST_DEB["package"] == "hello"
    assert TEST_DEB["filename"].endswith("hello_2.10-3build1_arm64.deb")


# --------------------------------------------------------------------------- #
# 3. NETWORK — the live noble build + hello fetch (opt-in)
# --------------------------------------------------------------------------- #

@net_only
def test_live_closure_resolves_zero_unsat():
    plan = resolve_apt_dpkg_closure()
    assert plan.missing == (), f"unsatisfied: {plan.missing}"
    assert len(plan.closure) >= 50            # the apt+dpkg runtime closure
    for need in ("apt", "dpkg", "libc6"):
        assert need in plan.closure, need
    assert plan.total_download_bytes > 10 * 1024 * 1024     # ~18.7 MiB


@net_only
@pytest.mark.skipif(not HAVE_AR, reason="needs `ar` to crack .deb members")
def test_live_apt_dpkg_overlay_self_contained_is_conformant(tmp_path: Path):
    base = Path(__file__).resolve().parents[1] / "rootfs" / "tiny-rootfs.tar"
    assert base.is_file(), base
    out = tmp_path / "apt-dpkg-stage.tar"
    cache = tmp_path / "deb-cache"
    res = build_apt_dpkg_overlay(out, base, cache_dir=cache, self_contained=True)

    # the build itself is downgrade-clean and complete
    assert res.violations == (), res.violations
    assert res.missing == ()
    assert res.file_count > 100
    assert len(res.self_contained_bins) > 0

    # §5-E conformant against the real base (no errors, no warnings)
    rep = validate_stage_tar(str(out), base=str(base))
    assert rep.conformant, rep.errors
    _assert_dot_rooted_and_safe(out)

    names = _members(out)
    # self-contained mode force-ships the front-ends + admindir DB root
    for need in ("./usr/bin/dpkg", "./usr/bin/dpkg-deb", "./usr/bin/apt",
                 "./usr/bin/apt-get", "./usr/bin/tar"):
        assert need in names, need
        assert names[need].mode & 0o111, f"front-end not executable: {need}"
    assert "./var/lib/dpkg/status" in names
    assert "./var/lib/dpkg/arch" in names


@net_only
def test_live_fetch_hello_deb(tmp_path: Path):
    deb = fetch_test_deb(tmp_path)
    p = Path(deb)
    assert p.is_file() and p.stat().st_size > 10_000
    # it is a real ar archive with zstd data members (noble convention)
    with open(p, "rb") as fh:
        assert fh.read(8) == b"!<arch>\n"
