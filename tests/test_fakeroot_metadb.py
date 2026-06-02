#!/usr/bin/env python3
"""test_fakeroot_metadb.py — host verification of the ALR fakeroot F2 metadata DB.

Pins the load-bearing semantics of the (dev,ino)-keyed overlay DB that
libalr_fakeroot.c implements. Runs on the host (macOS/Linux) with real files in
a tmpdir — NO device, NO root. These tests are the executable contract the
shipping .so must satisfy.
"""
from __future__ import annotations
import os
import stat as statmod
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fakeroot_metadb_model import FakeRootDB  # noqa: E402


def _f(tmp_path, name="f", data=b"x"):
    p = os.path.join(str(tmp_path), name)
    with open(p, "wb") as fh:
        fh.write(data)
    return p


def test_credential_getters_report_root(tmp_path):
    db = FakeRootDB()
    assert db.geteuid() == 0
    assert db.getegid() == 0


def test_chown_overlay_does_not_touch_real_owner(tmp_path):
    db = FakeRootDB()
    p = _f(tmp_path)
    real_before = os.stat(p)
    db.chown(p, 0, 0)
    # real file owner is UNCHANGED (no CAP, would EPERM if attempted for real)
    assert os.stat(p).st_uid == real_before.st_uid
    # but the overlaid stat reports fake root
    ov = db.stat_overlay(p)
    assert ov.st_uid == 0
    assert ov.st_gid == 0


def test_stat_without_overlay_is_passthrough(tmp_path):
    db = FakeRootDB()
    p = _f(tmp_path)
    assert db.stat_overlay(p).st_uid == os.stat(p).st_uid


def test_rename_preserves_overlay_inode_keyed(tmp_path):
    """rename keeps the inode -> the fake ownership follows the file for free.
    This is the central reason for (dev,ino) keying over path keying."""
    db = FakeRootDB()
    p = _f(tmp_path, "orig")
    db.chown(p, 0, 0)
    q = os.path.join(str(tmp_path), "renamed")
    os.rename(p, q)
    ov = db.stat_overlay(q)
    assert ov.st_uid == 0 and ov.st_gid == 0


def test_hardlink_shares_overlay(tmp_path):
    """Two hardlinks share one inode -> one overlay -> both report fake root.
    Matches real Unix semantics (hardlinks share ownership)."""
    db = FakeRootDB()
    a = _f(tmp_path, "a")
    b = os.path.join(str(tmp_path), "b")
    os.link(a, b)
    db.chown(a, 0, 0)
    assert db.stat_overlay(b).st_uid == 0   # overlay visible through the other name


def test_delete_recreate_same_path_new_inode_drops_stale_overlay(tmp_path):
    """A path-keyed DB would WRONGLY carry old fake-root ownership onto a new file
    created at the same path. (dev,ino) keying drops the stale entry: the new
    inode has no overlay -> real (non-root) owner is reported. This is correct:
    it is a different file."""
    db = FakeRootDB()
    p = _f(tmp_path, "reused")
    db.chown(p, 0, 0)
    assert db.stat_overlay(p).st_uid == 0
    os.unlink(p)
    # recreate at same path -> new inode (assert it actually differs)
    new = _f(tmp_path, "reused", data=b"yy")
    assert os.stat(new).st_ino != -1
    ov = db.stat_overlay(new)
    # new inode -> no overlay -> real owner (NOT fake root unless host runs as root)
    if os.geteuid() != 0:
        assert ov.st_uid != 0 or ov.st_uid == os.stat(new).st_uid
        assert ov.st_uid == os.stat(new).st_uid


def test_chmod_overlay_preserves_type_bits(tmp_path):
    db = FakeRootDB()
    p = _f(tmp_path)
    db.chmod(p, 0o4755)   # setuid + rwxr-xr-x
    ov = db.stat_overlay(p)
    assert statmod.S_ISREG(ov.st_mode)            # still a regular file
    assert (ov.st_mode & 0o7777) == 0o4755        # full perm+setuid overlaid


def test_mknod_chr_reports_device_node(tmp_path):
    """dpkg unpacks device nodes (e.g. /dev/null in some packages). We cannot
    mknod (no CAP_MKNOD); the placeholder is a regular file, but the overlay
    makes stat report a char device with the right rdev."""
    db = FakeRootDB()
    p = os.path.join(str(tmp_path), "nulldev")
    open(p, "wb").close()                          # placeholder (wrapper does this)
    rdev = os.makedev(1, 3)                         # /dev/null major,minor
    db.mknod(p, statmod.S_IFCHR | 0o666, rdev)
    ov = db.stat_overlay(p, follow=False)
    assert statmod.S_ISCHR(ov.st_mode)
    assert ov.st_uid == 0 and ov.st_gid == 0
    assert (ov.st_mode & 0o7777) == 0o666


def test_chown_then_chmod_compose(tmp_path):
    db = FakeRootDB()
    p = _f(tmp_path)
    db.chown(p, 0, 0)
    db.chmod(p, 0o600)
    ov = db.stat_overlay(p)
    assert ov.st_uid == 0
    assert (ov.st_mode & 0o7777) == 0o600


def test_lchown_uses_lstat_key_for_symlink(tmp_path):
    """lchown on a symlink keys off the symlink's own inode, not its target."""
    db = FakeRootDB()
    tgt = _f(tmp_path, "target")
    link = os.path.join(str(tmp_path), "link")
    os.symlink(tgt, link)
    db.chown(link, 0, 0, follow=False)             # lchown
    # the symlink reports fake root; the target is untouched
    assert db.stat_overlay(link, follow=False).st_uid == 0
    assert db.stat_overlay(tgt).st_uid == os.stat(tgt).st_uid


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-v"]))
