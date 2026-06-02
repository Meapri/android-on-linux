#!/usr/bin/env python3
"""test_fakeroot_db_shared.py — proves the cross-process sharing mechanism that
libalr_fakeroot.c uses for its metadata DB survives fork+exec, which is the
hard requirement: dpkg fork+exec's maintainer scripts (postinst, etc.) and the
child MUST see the uid/gid the parent faked.

We model the C design's chosen primitive: a single rootfs-local file mmap'd
MAP_SHARED by every guest process. Writes by the parent are visible to a child
that mmaps the SAME file AFTER fork, AND to a freshly exec'd process that
re-opens the file by the path passed in $ALR_FAKEROOT_DB. This is the property
that the Android-unavailable SysV-msgq daemon (libfakeroot's mechanism) would
otherwise provide.

Host-portable: uses mmap + a real file; no Android, no root.
"""
from __future__ import annotations
import mmap
import os
import struct
import sys
import tempfile

# 32-byte fixed entry: dev(u64) ino(u64) uid(u32) gid(u32) mode(u32) rdev(u32)
ENTRY = struct.Struct("<QQIIII")
N_SLOTS = 1024
HDR = 64  # reserved header (magic, count, generation)


def _slot(dev, ino):
    return (hash((dev, ino)) & 0x7FFFFFFF) % N_SLOTS


def db_create(path):
    size = HDR + N_SLOTS * ENTRY.size
    with open(path, "wb") as f:
        f.write(b"\0" * size)
    return size


def db_put(mm, dev, ino, uid, gid, mode, rdev):
    s = _slot(dev, ino)
    for probe in range(N_SLOTS):  # linear-probe open addressing
        off = HDR + ((s + probe) % N_SLOTS) * ENTRY.size
        d, i, *_ = ENTRY.unpack_from(mm, off)
        if (d == 0 and i == 0) or (d == dev and i == ino):
            ENTRY.pack_into(mm, off, dev, ino, uid, gid, mode, rdev)
            return True
    return False


def db_get(mm, dev, ino):
    s = _slot(dev, ino)
    for probe in range(N_SLOTS):
        off = HDR + ((s + probe) % N_SLOTS) * ENTRY.size
        d, i, uid, gid, mode, rdev = ENTRY.unpack_from(mm, off)
        if d == 0 and i == 0:
            return None
        if d == dev and i == ino:
            return (uid, gid, mode, rdev)
    return None


def test_fork_child_sees_parent_write():
    """The central coherence property: parent fa-chowns, then forks; the child
    (sharing the MAP_SHARED mapping by inheritance) sees the overlay."""
    fd, path = tempfile.mkstemp(prefix="alrfrdb-")
    os.close(fd)
    try:
        size = db_create(path)
        f = open(path, "r+b")
        mm = mmap.mmap(f.fileno(), size, mmap.MAP_SHARED,
                       mmap.PROT_READ | mmap.PROT_WRITE)
        dev, ino = 0x1234, 0x5678
        db_put(mm, dev, ino, 0, 0, 0o100644, 0)   # fake root before fork

        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:  # child: read through the INHERITED mapping
            os.close(r)
            got = db_get(mm, dev, ino)
            os.write(w, b"1" if got == (0, 0, 0o100644, 0) else b"0")
            os._exit(0)
        os.close(w)
        ok = os.read(r, 1)
        os.waitpid(pid, 0)
        assert ok == b"1", "fork child did NOT see parent's overlay"
    finally:
        os.unlink(path)


def test_reopen_by_path_sees_write_simulating_exec():
    """After dpkg fork+EXECs a maintainer script, the new process re-opens the DB
    by the path in $ALR_FAKEROOT_DB (envp survives ALR in-process re-map: x21).
    A fresh open+mmap of the same file sees writes that predate the exec."""
    fd, path = tempfile.mkstemp(prefix="alrfrdb-")
    os.close(fd)
    try:
        size = db_create(path)
        f1 = open(path, "r+b")
        mm1 = mmap.mmap(f1.fileno(), size, mmap.MAP_SHARED,
                        mmap.PROT_READ | mmap.PROT_WRITE)
        dev, ino = 99, 100
        db_put(mm1, dev, ino, 0, 0, 0o100600, 0)
        mm1.flush()

        # simulate the exec'd child: brand-new open + mmap by path
        f2 = open(path, "r+b")
        mm2 = mmap.mmap(f2.fileno(), size, mmap.MAP_SHARED,
                        mmap.PROT_READ | mmap.PROT_WRITE)
        assert db_get(mm2, dev, ino) == (0, 0, 0o100600, 0)

        # and a write by the exec'd child is visible back in the parent mapping
        db_put(mm2, dev, ino + 1, 0, 0, 0o100755, 0)
        mm2.flush()
        assert db_get(mm1, dev, ino + 1) == (0, 0, 0o100755, 0)
    finally:
        os.unlink(path)


def test_open_addressing_handles_collision():
    fd, path = tempfile.mkstemp(prefix="alrfrdb-")
    os.close(fd)
    try:
        size = db_create(path)
        f = open(path, "r+b")
        mm = mmap.mmap(f.fileno(), size, mmap.MAP_SHARED,
                       mmap.PROT_READ | mmap.PROT_WRITE)
        # force two keys into the same starting slot, verify both retrievable
        base = _slot(1, 1)
        keys = []
        for ino in range(1, 5000):
            if _slot(1, ino) == base:
                keys.append((1, ino))
            if len(keys) >= 3:
                break
        for j, (d, i) in enumerate(keys):
            db_put(mm, d, i, j, j, 0o100644, 0)
        for j, (d, i) in enumerate(keys):
            assert db_get(mm, d, i) == (j, j, 0o100644, 0)
    finally:
        os.unlink(path)


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-v"]))
