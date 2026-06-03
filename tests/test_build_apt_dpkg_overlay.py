"""Host tests for tools/build_apt_dpkg_overlay.py (R8-B full apt+dpkg staging).

All OFFLINE — exercise the closure resolver against a synthetic noble-like
Packages index, the dpkg admindir scaffold, the self-contained binary set, and
the absolute-symlink-tolerant deb member extractor. The NETWORK build path
(download + base-subtract + overlay_guard) is verified by hand on the host (see
the R8-B handoff) and is the integration/device gate.

Contract:
  * the closure of `apt apt-utils dpkg` + the unpack toolchain resolves with no
    unsatisfied deps, pulling libapt-pkg / gpgv / the compression libs;
  * the dpkg admindir scaffold ships the var/lib/dpkg skeleton dpkg needs to
    record a fresh `dpkg -i`, ./-rooted and §5-E conformant;
  * the self-contained binary set covers the load-bearing front-ends
    (dpkg/dpkg-deb/dpkg-split/dpkg-query/apt/apt-get/tar);
  * extract_named_files recovers a wanted regular file from a .deb even when an
    absolute-symlink member (tar's ./etc/rmt) is present (the member that aborts
    build_stage_tar.extract_deb).
"""

from __future__ import annotations

import io
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from tools.build_apt_dpkg_overlay import (
    ADMINDIR_DIRS,
    APT_KEY_VERIFY_DEPS,
    DEFAULT_TARGETS,
    SELF_CONTAINED_BINS,
    TEST_DEB,
    _append_admindir,
    build_apt_dpkg_overlay,
    extract_named_files,
    resolve_apt_dpkg_closure,
    write_admindir_scaffold,
)
from tools.stage_tar_spec import validate_stage_tar


# A synthetic noble-like Packages index covering the closure brain. Sizes are the
# real noble compressed sizes so total-download accounting is exercised too.
INDEX = {
    "apt": {"Version": "2.8.3", "Architecture": "arm64", "Size": "1336188",
            "Depends": "libapt-pkg6.0t64, gpgv, dpkg (>= 1.17.2)"},
    "apt-utils": {"Version": "2.8.3", "Architecture": "arm64", "Size": "205248",
                  "Depends": "apt (= 2.8.3), libapt-pkg6.0t64"},
    "dpkg": {"Version": "1.22.6", "Architecture": "arm64", "Size": "1265468",
             "Depends": "tar, libzstd1"},
    "tar": {"Version": "1.35", "Architecture": "arm64", "Size": "247906"},
    "gzip": {"Version": "1.12", "Architecture": "arm64", "Size": "97192"},
    "xz-utils": {"Version": "5.6", "Architecture": "arm64", "Size": "268364",
                 "Depends": "liblzma5"},
    "zstd": {"Version": "1.5.5", "Architecture": "arm64", "Size": "574810",
             "Depends": "libzstd1"},
    "coreutils": {"Version": "9.4", "Architecture": "arm64", "Size": "1362772"},
    "sed": {"Version": "4.9", "Architecture": "arm64", "Size": "171910"},
    "bash": {"Version": "5.2", "Architecture": "arm64", "Size": "780262"},
    "dash": {"Version": "0.5.12", "Architecture": "arm64", "Size": "90376"},
    "gpgv": {"Version": "2.4.4", "Architecture": "arm64", "Size": "149882",
             "Depends": "libgcrypt20"},
    "libapt-pkg6.0t64": {"Version": "2.8.3", "Architecture": "arm64", "Size": "934734",
                         "Depends": "libzstd1, libgcrypt20"},
    "libgcrypt20": {"Version": "1.10", "Architecture": "arm64", "Size": "471954"},
    "libzstd1": {"Version": "1.5.5", "Architecture": "arm64", "Size": "271224"},
    "liblzma5": {"Version": "5.6", "Architecture": "arm64", "Size": "125356"},
}


# --------------------------------------------------------------------------- #
# Closure resolution
# --------------------------------------------------------------------------- #

def test_closure_pulls_apt_dpkg_and_transitive_deps():
    plan = resolve_apt_dpkg_closure(index=INDEX)
    for must in ("apt", "apt-utils", "dpkg", "tar", "gpgv",
                 "libapt-pkg6.0t64", "libzstd1", "libgcrypt20"):
        assert must in plan.closure, (must, plan.closure)


def test_closure_has_no_unsatisfied_deps():
    plan = resolve_apt_dpkg_closure(index=INDEX)
    assert plan.missing == (), plan.missing


def test_closure_reports_total_download_size():
    plan = resolve_apt_dpkg_closure(index=INDEX)
    assert plan.total_download_bytes == sum(int(INDEX[n]["Size"]) for n in plan.closure)
    assert plan.total_download_bytes > 0


def test_default_targets_include_apt_dpkg_and_unpack_toolchain():
    for must in ("apt", "apt-utils", "dpkg", "tar", "gzip",
                 "xz-utils", "zstd", "bash", "dash", "gpgv"):
        assert must in DEFAULT_TARGETS


# --------------------------------------------------------------------------- #
# dpkg admindir scaffold
# --------------------------------------------------------------------------- #

def test_admindir_scaffold_ships_dpkg_db_skeleton(tmp_path: Path):
    out = tmp_path / "admindir.tar"
    members = write_admindir_scaffold(out)
    with tarfile.open(out) as t:
        names = {m.name: m for m in t.getmembers()}
    for d in ADMINDIR_DIRS:
        assert "./" + d in names and names["./" + d].isdir()
    assert "./var/lib/dpkg/status" in names
    assert "./var/lib/dpkg/available" in names
    assert "./var/lib/dpkg/arch" in names
    assert all(n.startswith("./") for n in names)
    assert "./var/lib/dpkg/status" in members


def test_admindir_arch_pins_arm64(tmp_path: Path):
    out = tmp_path / "admindir.tar"
    write_admindir_scaffold(out)
    with tarfile.open(out) as t:
        assert t.extractfile("./var/lib/dpkg/arch").read() == b"arm64\n"


def test_admindir_scaffold_is_stage_tar_conformant(tmp_path: Path):
    out = tmp_path / "admindir.tar"
    write_admindir_scaffold(out)
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


def test_no_status_drops_status_but_keeps_arch(tmp_path: Path):
    out = tmp_path / "admindir.tar"
    members = write_admindir_scaffold(out, include_status=False)
    assert "./var/lib/dpkg/status" not in members
    assert "./var/lib/dpkg/available" not in members
    assert "./var/lib/dpkg/arch" in members


def test_append_admindir_preserves_closure_payload(tmp_path: Path):
    closure = tmp_path / "closure.tar"
    with tarfile.open(closure, "w") as t:
        ti = tarfile.TarInfo("./usr/bin/dpkg")
        payload = b"\x7fELF dpkg"
        ti.size = len(payload)
        ti.mode = 0o755
        t.addfile(ti, io.BytesIO(payload))
    _append_admindir(closure)
    with tarfile.open(closure) as t:
        names = {m.name for m in t.getmembers()}
    assert "./usr/bin/dpkg" in names           # closure binary preserved
    assert "./var/lib/dpkg/status" in names     # admindir folded in
    assert "./var/lib/dpkg/info" in names


# --------------------------------------------------------------------------- #
# Self-contained binary set
# --------------------------------------------------------------------------- #

def test_self_contained_set_covers_load_bearing_frontends():
    flat = {p for ps in SELF_CONTAINED_BINS.values() for p in ps}
    for must in ("usr/bin/dpkg", "usr/bin/dpkg-deb", "usr/bin/dpkg-split",
                 "usr/bin/dpkg-query", "usr/bin/apt", "usr/bin/apt-get",
                 "usr/bin/tar"):
        assert must in flat, must


# --------------------------------------------------------------------------- #
# AUTHENTICATED-apt path (GAP 1) — the apt-key `verify` happy path is self-contained
# --------------------------------------------------------------------------- #

def test_self_contained_set_ships_the_whole_apt_key_verify_path():
    """noble apt 2.8.3 verifies an InRelease by exec'ing apt-key → gpgv, and
    apt-key shells out to several coreutils for its temp gpg home. The slim base
    ships NONE of these, so --self-contained must stage the WHOLE happy path or
    authenticated `apt-get update` dies "Unknown error executing apt-key"."""
    flat = {p for ps in SELF_CONTAINED_BINS.values() for p in ps}
    for must in APT_KEY_VERIFY_DEPS:
        assert must in flat, f"apt-key verify dep {must} missing from self-contained set"
    # the load-bearing trio: the apt-key script, the gpgv METHOD it runs from, and
    # the gpgv BINARY the method/apt-key actually verifies with.
    assert "usr/bin/apt-key" in flat
    assert "usr/lib/apt/methods/gpgv" in flat
    assert "usr/bin/gpgv" in flat
    # sed: apt-key's escape_shell pipes through it (non-fatal on the verify path,
    # but essential:yes + needed by general maint scripts); the slim base dropped it.
    assert "usr/bin/sed" in flat


def test_apt_key_verify_deps_include_the_required_coreutils():
    """The specific coreutils apt-key's `verify --keyring X.gpg` path needs:
    mktemp/chmod (create_gpg_home), touch (create_new_keyring), head (dearmor
    sniff), rm/cat (cleanup). All ride in coreutils."""
    for must in ("usr/bin/mktemp", "usr/bin/chmod", "usr/bin/touch",
                 "usr/bin/head", "usr/bin/rm", "usr/bin/cat"):
        assert must in APT_KEY_VERIFY_DEPS, must


def test_self_contained_does_not_drag_in_heavy_gnupg():
    """A plain `.gpg` Signed-By keyring needs NO gpg/gpgconf (dearmor of a .gpg is a
    no-op, the merge is `cat`, cleanup's gpgconf is command_available-guarded), so
    the overlay must NOT pull the heavy gnupg stack."""
    flat = {p for ps in SELF_CONTAINED_BINS.values() for p in ps}
    assert "usr/bin/gpg" not in flat
    assert "usr/bin/gpgconf" not in flat
    assert "usr/bin/gpg-agent" not in flat


def test_coreutils_is_a_default_target_so_the_closure_ships_apt_key_deps():
    """The self-contained set is belt-and-suspenders; the closure itself ships the
    coreutils (and thus apt-key's deps) because coreutils is a DEFAULT_TARGET."""
    assert "coreutils" in DEFAULT_TARGETS


# --------------------------------------------------------------------------- #
# extract_named_files tolerance of an absolute-symlink member
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to synthesize a .deb")
def test_extract_named_files_survives_absolute_symlink_member(tmp_path: Path):
    """A real `tar` .deb carries ./etc/rmt -> /usr/sbin/rmt (an ABSOLUTE symlink)
    that aborts build_stage_tar.extract_deb's disk extraction. extract_named_files
    reads in-memory and must still recover ./usr/bin/tar."""
    data_root = tmp_path / "dataroot"
    (data_root / "usr/bin").mkdir(parents=True)
    (data_root / "usr/bin/tar").write_bytes(b"\x7fELF tar-bin")
    (data_root / "usr/bin/tar").chmod(0o755)
    (data_root / "etc").mkdir()
    (data_root / "etc/rmt").symlink_to("/usr/sbin/rmt")  # absolute

    data_tar = tmp_path / "data.tar"
    with tarfile.open(data_tar, "w") as dt:
        dt.add(data_root / "usr/bin/tar", arcname="./usr/bin/tar")
        dt.add(data_root / "etc/rmt", arcname="./etc/rmt")
    (tmp_path / "debian-binary").write_text("2.0\n")
    (tmp_path / "control.tar").write_bytes(b"")
    # `ar qcS`: quick-create with NO symbol table (LLVM/BSD ar adds __.SYMDEF
    # otherwise); run in tmp_path so members are bare names like a real .deb.
    subprocess.run(
        ["ar", "qcS", "synthetic.deb", "debian-binary", "control.tar", "data.tar"],
        check=True, capture_output=True, cwd=str(tmp_path),
    )
    got = extract_named_files(tmp_path / "synthetic.deb", {"usr/bin/tar"})
    assert "usr/bin/tar" in got
    assert got["usr/bin/tar"][0] == b"\x7fELF tar-bin"
    assert got["usr/bin/tar"][1] == 0o755         # forced executable
    assert "etc/rmt" not in got                    # only wanted members returned


# --------------------------------------------------------------------------- #
# Test payload metadata
# --------------------------------------------------------------------------- #

def test_test_deb_points_at_noble_hello():
    assert TEST_DEB["package"] == "hello"
    assert TEST_DEB["filename"].endswith("_arm64.deb")
    assert "hello" in TEST_DEB["filename"]


# --------------------------------------------------------------------------- #
# Builder selftest
# --------------------------------------------------------------------------- #

def test_builder_selftest_passes():
    from tools.build_apt_dpkg_overlay import _selftest

    assert _selftest() == 0


def test_build_function_requires_no_network_for_signature():
    """Smoke: the public build entry point exists with the documented kwargs
    (a guard against accidental signature drift; the network path is the device
    gate, not run here)."""
    import inspect

    sig = inspect.signature(build_apt_dpkg_overlay)
    for kw in ("targets", "mirror", "suite", "components", "cache_dir",
               "include_status", "self_contained"):
        assert kw in sig.parameters, kw
