"""Host proof that the fakeroot mechanism makes a non-root ``dpkg --unpack`` pass.

This is the T2 host-side proof of "fakeroot unblocks dpkg unpack" (the device run
is DEVICE-REQ — WS-1/integration). It exercises tools.dpkg_unpack_model, which
simulates dpkg's per-member privileged-call SEQUENCE (superuser gate → lay down →
chown → chmod, plus mknod for device nodes) over a .deb at a non-root uid:

  * WITHOUT the fakeroot hooks the unpack hits the classic non-root wall — dpkg's
    superuser gate refuses, and even past it chown(root) EPERMs and a device-node
    mknod(2) is a HARD wall (abort);
  * WITH the hooks (getuid→0, chown no-op+DB, mknod placeholder+DB, stat overlay —
    the exact slice of tools/fakeroot/libalr_fakeroot.c, extended with the mknod
    placeholder rule) the unpack completes (unpacked=true) AND is self-consistent:
    a later stat() of every path reports root:root and the intended type/mode,
    0 mismatch (a placeholdered char-device node stat()s back as S_IFCHR).

The model uses a synthetic .deb carrying one of EVERY member kind (regular file,
dir, symlink, char-device node) so the mknod hard-wall / placeholder path is
covered, not just the common file path.
"""

from __future__ import annotations

import shutil
import stat as stat_mod
from pathlib import Path

import pytest

from tools.dpkg_unpack_model import (
    KIND_BLKDEV,
    KIND_CHRDEV,
    KIND_DIR,
    KIND_FILE,
    KIND_SYMLINK,
    DebMember,
    FakerootDB,
    build_synthetic_deb,
    members_from_deb,
    prove,
    simulate_unpack,
    synthetic_members,
)

UNTRUSTED_APP_UID = 10257  # a typical Android untrusted_app uid (non-root)


# --------------------------------------------------------------------------- #
# (1) Without fakeroot: the classic non-root wall
# --------------------------------------------------------------------------- #

def test_without_fakeroot_superuser_gate_refuses_unpack():
    """A non-root dpkg refuses before it even unpacks: 'requires superuser
    privilege'. unpacked=false."""
    res = simulate_unpack(synthetic_members(), real_uid=UNTRUSTED_APP_UID, fakeroot=None)
    assert res.unpacked is False
    assert "requires superuser privilege" in res.failure
    assert f"getuid()=={UNTRUSTED_APP_UID}" in res.failure


def test_without_fakeroot_chown_root_eperms_when_gate_bypassed():
    """If dpkg is forced past its superuser gate (getuid faked) but chown is NOT
    hooked, laying down a root:root file at a non-root uid EPERMs at fchown — the
    second classic non-root wall the chown no-op rule clears."""
    from tools.dpkg_unpack_model import _chown_hardwall_probe

    res = _chown_hardwall_probe(UNTRUSTED_APP_UID)
    assert res.unpacked is False
    assert "fchown(" in res.failure
    assert "Operation not permitted" in res.failure
    # the file bytes DID land before the chown wall (unprivileged step succeeds)
    assert "/usr/bin/x" in res.laid_down


def test_mknod_is_the_hard_wall_without_the_placeholder_rule():
    """The ONE residual hard wall: even with getuid faked (gate passed) and chown a
    no-op, a device node's bare mknod(2) reaches the kernel and EPERMs/aborts. This
    is exactly what the placeholder rule clears."""
    from tools.dpkg_unpack_model import _mknod_hardwall_probe

    res = _mknod_hardwall_probe(UNTRUSTED_APP_UID)
    assert res.unpacked is False
    assert "mknod(" in res.failure
    assert "HARD WALL" in res.failure


def test_current_c_shim_does_not_yet_intercept_mknod():
    """Honesty guard: the shipped C shim (tools/fakeroot/libalr_fakeroot.c) does
    NOT yet intercept mknod — so the placeholder rule modelled here is a documented
    DEVICE extension the shim still needs for packages shipping device nodes. (The
    basic gate target, hello.deb, ships no device node, so it is unaffected.) This
    test fails loudly if someone adds mknod to the shim so the model can be tightened
    to match."""
    shim = Path(__file__).resolve().parents[1] / "tools" / "fakeroot" / "libalr_fakeroot.c"
    src = shim.read_text()
    assert "mknod" not in src, (
        "the C shim now references mknod — update dpkg_unpack_model + this guard so "
        "the host model mirrors the shim's real device-node handling"
    )


# --------------------------------------------------------------------------- #
# (2) With fakeroot: the unpack passes and is self-consistent
# --------------------------------------------------------------------------- #

def test_with_fakeroot_unpack_completes():
    db = FakerootDB(fake_uid=0, fake_gid=0)
    res = simulate_unpack(synthetic_members(), real_uid=UNTRUSTED_APP_UID, fakeroot=db)
    assert res.unpacked is True
    assert res.failure == ""
    assert len(res.laid_down) == len(synthetic_members())


def test_with_fakeroot_zero_mismatch_self_consistency():
    """The device-gate contract: after unpack, stat() of every path reports the
    recorded root:root ownership and intended mode — 0 mismatch."""
    db = FakerootDB(fake_uid=0, fake_gid=0)
    res = simulate_unpack(synthetic_members(), real_uid=UNTRUSTED_APP_UID, fakeroot=db)
    assert res.self_consistent is True
    assert res.mismatches == ()


def test_with_fakeroot_stat_reports_root_root_for_every_member():
    """stat() overlays root:root onto every laid-down path, regardless of the real
    on-disk owner (the running non-root uid)."""
    db = FakerootDB(fake_uid=0, fake_gid=0)
    members = synthetic_members()
    res = simulate_unpack(members, real_uid=UNTRUSTED_APP_UID, fakeroot=db)
    assert res.unpacked and res.self_consistent
    # spot-check the DB directly: a chowned file stat()s back root:root.
    uid, gid, _ = db.stat_overlay((0xC0FFEE, 1000), UNTRUSTED_APP_UID,
                                  UNTRUSTED_APP_UID, stat_mod.S_IFREG | 0o755)
    assert (uid, gid) == (0, 0)


def test_with_fakeroot_device_node_is_placeholdered_and_stats_as_chr():
    """The char-device node is laid down as a regular placeholder (mknod EPERM
    sidestepped) but stat()s back as a CHAR device with its recorded perms."""
    db = FakerootDB(fake_uid=0, fake_gid=0)
    members = synthetic_members()
    res = simulate_unpack(members, real_uid=UNTRUSTED_APP_UID, fakeroot=db)
    assert "/dev/hellonull" in res.placeholdered_nodes
    assert res.self_consistent
    # the device member's key is its index in the member list
    dev_idx = next(i for i, m in enumerate(members) if m.kind == KIND_CHRDEV)
    _, _, mode = db.stat_overlay((0xC0FFEE, 1000 + dev_idx), UNTRUSTED_APP_UID,
                                 UNTRUSTED_APP_UID, stat_mod.S_IFREG | 0o644)
    assert stat_mod.S_ISCHR(mode)
    assert (mode & 0o777) == 0o666


def test_block_device_node_also_placeholders_and_stats_as_blk():
    """Block-device members take the same placeholder path (S_IFBLK)."""
    members = [
        DebMember("/usr/bin/x", KIND_FILE, 0o755, owner=0, group=0),
        DebMember("/dev/hellosd", KIND_BLKDEV, 0o660, owner=0, group=0, rdev=(8, 0)),
    ]
    db = FakerootDB(fake_uid=0, fake_gid=0)
    res = simulate_unpack(members, real_uid=UNTRUSTED_APP_UID, fakeroot=db)
    assert res.unpacked and res.self_consistent
    assert "/dev/hellosd" in res.placeholdered_nodes
    _, _, mode = db.stat_overlay((0xC0FFEE, 1001), UNTRUSTED_APP_UID,
                                 UNTRUSTED_APP_UID, stat_mod.S_IFREG | 0o644)
    assert stat_mod.S_ISBLK(mode)


# --------------------------------------------------------------------------- #
# (3) DB semantics mirror the C shim's (dev,ino) table
# --------------------------------------------------------------------------- #

def test_db_getuid_reports_faked_root():
    assert FakerootDB(fake_uid=0, fake_gid=0).getuid() == 0


def test_db_honors_nondefault_faked_identity():
    db = FakerootDB(fake_uid=123, fake_gid=456)
    assert db.getuid() == 123 and db.getgid() == 456
    uid, gid, _ = db.stat_overlay((1, 2), 999, 999, stat_mod.S_IFREG | 0o644)
    assert (uid, gid) == (123, 456)   # faked even with no prior chown


def test_db_chown_minus_one_preserves_prior_value():
    """chown(-1, gid) must leave the recorded uid alone (mirrors the C shim's
    (uint32_t)-1 'leave this id alone' rule)."""
    db = FakerootDB(fake_uid=0, fake_gid=0)
    db.chown((1, 2), 100, 200)
    db.chown((1, 2), -1, 999)          # change only gid
    uid, gid, _ = db.stat_overlay((1, 2), 5, 5, stat_mod.S_IFREG | 0o644)
    assert uid == 100 and gid == 999


def test_db_stat_overlay_keeps_real_mode_until_chmod():
    """Until a chmod/mknod records a mode, stat() returns the REAL mode (only the
    owner is forced)."""
    db = FakerootDB(fake_uid=0, fake_gid=0)
    db.chown((1, 2), 0, 0)
    _, _, mode = db.stat_overlay((1, 2), 5, 5, stat_mod.S_IFREG | 0o600)
    assert mode == (stat_mod.S_IFREG | 0o600)   # real mode preserved
    db.chmod((1, 2), stat_mod.S_IFREG | 0o755)
    _, _, mode = db.stat_overlay((1, 2), 5, 5, stat_mod.S_IFREG | 0o600)
    assert mode == (stat_mod.S_IFREG | 0o755)   # now the faked mode wins


# --------------------------------------------------------------------------- #
# (4) End-to-end prove() and a real (synthetic) .deb round-trip
# --------------------------------------------------------------------------- #

def test_prove_contrasts_wall_and_pass():
    rep = prove(real_uid=UNTRUSTED_APP_UID)
    assert rep.without_fakeroot.unpacked is False        # the wall
    assert rep.with_fakeroot.unpacked is True            # the shim clears it
    assert rep.with_fakeroot.self_consistent is True
    assert KIND_CHRDEV in rep.member_kinds               # device node exercised


@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to build a .deb")
def test_synthetic_deb_round_trips_every_member_kind(tmp_path: Path):
    deb = build_synthetic_deb(tmp_path / "synthetic.deb")
    assert deb.is_file() and deb.stat().st_size > 0
    parsed = members_from_deb(deb)
    kinds = {m.kind for m in parsed}
    assert {KIND_FILE, KIND_DIR, KIND_SYMLINK, KIND_CHRDEV} <= kinds


@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to build a .deb")
def test_parsed_real_deb_reproduces_the_proof(tmp_path: Path):
    """Parsing a real on-disk .deb (not just the in-memory member list) reproduces
    the wall-vs-pass outcome — the model accepts a real archive."""
    deb = build_synthetic_deb(tmp_path / "synthetic.deb")
    parsed = members_from_deb(deb)
    rep = prove(parsed, real_uid=UNTRUSTED_APP_UID)
    assert rep.without_fakeroot.unpacked is False
    assert rep.with_fakeroot.unpacked is True
    assert rep.with_fakeroot.self_consistent is True


@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to build a .deb")
def test_char_device_node_survives_deb_round_trip(tmp_path: Path):
    """The CHRTYPE member round-trips through ar+tar with its (major,minor) intact
    (1,3 = /dev/null-like), so the placeholder path is driven by a real archive."""
    deb = build_synthetic_deb(tmp_path / "synthetic.deb")
    parsed = members_from_deb(deb)
    chr_members = [m for m in parsed if m.kind == KIND_CHRDEV]
    assert chr_members, "synthetic .deb lost its char-device node"
    assert chr_members[0].rdev == (1, 3)


# --------------------------------------------------------------------------- #
# (5) The module selftest is green
# --------------------------------------------------------------------------- #

def test_model_selftest_passes():
    from tools.dpkg_unpack_model import _selftest

    assert _selftest() == 0
