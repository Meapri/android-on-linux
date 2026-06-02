#!/usr/bin/env python3
"""fakeroot_metadb_model.py — host model of the ALR fakeroot F2 metadata DB.

Pure-Python executable specification of the metadata-DB semantics that
libalr_fakeroot.c implements, so the load-bearing logic (key choice,
overlay-on-real-stat, fork-shared coherence, rename/hardlink/inode-reuse
robustness) is testable on the host with NO device and NO cross-compile.

This is a MODEL, not the shipping code. The C .so must produce byte-identical
overlay decisions for the same (key, op) sequence. tests/test_fakeroot_metadb.py
drives it.

KEY DECISION (mirrors libfakeroot): the DB is keyed by (st_dev, st_ino), NOT by
path. Rationale and the consequences are encoded in the tests:
  - chown(path) -> resolve path to (dev,ino) via the REAL lstat, store overlay
  - stat(path)  -> REAL stat, then if (dev,ino) in DB overlay uid/gid/mode/rdev
  - rename(a,b) -> inode unchanged -> overlay follows the file for free (no work)
  - hardlink    -> same inode -> both names share one overlay (correct: real
                   hardlinks share ownership too)
  - delete+recreate same path -> NEW inode -> stale overlay does NOT apply
                   (correct: it is a different file). A path-keyed DB would
                   wrongly carry the old fake-root ownership onto the new file.
  - mknod(path,mode,dev) -> we cannot really mknod (EPERM, no CAP_MKNOD); we
                   create a regular placeholder file, then store an overlay whose
                   st_mode carries the S_IFCHR/S_IFBLK type bits and st_rdev=dev,
                   so a later stat() reports a device node. dpkg is satisfied.
"""

from __future__ import annotations
import os
import stat as statmod
from dataclasses import dataclass


@dataclass
class Overlay:
    uid: int | None = None
    gid: int | None = None
    mode: int | None = None      # full st_mode if set by chmod/mknod, else None
    rdev: int | None = None      # device number for mknod'd nodes
    # type override (S_IFCHR/S_IFBLK/S_IFIFO) for fake special files
    ifmt: int | None = None


class FakeRootDB:
    """In-process model of the (dev,ino)->Overlay table.

    The C implementation backs this with an mmap'd open-addressing hash table in
    a shared file (see db_approach in the design); this model uses a dict but
    exposes the SAME operations so the semantics are pinned by tests.
    """

    def __init__(self) -> None:
        self._t: dict[tuple[int, int], Overlay] = {}
        # faked credentials: fakeroot reports euid/egid 0 to the guest
        self.fake_uid = 0
        self.fake_gid = 0

    # ---- key derivation: ALWAYS via the real stat of the real (rootfs) file ---
    @staticmethod
    def key_for(real_path: str, follow: bool = True) -> tuple[int, int]:
        st = os.stat(real_path) if follow else os.lstat(real_path)
        return (st.st_dev, st.st_ino)

    # ---- credential getters (geteuid/getuid/... -> 0) -------------------------
    def geteuid(self) -> int:
        return self.fake_uid

    def getegid(self) -> int:
        return self.fake_gid

    # ---- mutators: store overlay, never touch the real file's owner -----------
    def chown(self, real_path: str, uid: int, gid: int, follow: bool = True) -> None:
        k = self.key_for(real_path, follow)
        ov = self._t.setdefault(k, Overlay())
        if uid != -1:
            ov.uid = uid
        if gid != -1:
            ov.gid = gid

    def chmod(self, real_path: str, mode: int, follow: bool = True) -> None:
        k = self.key_for(real_path, follow)
        ov = self._t.setdefault(k, Overlay())
        ov.mode = mode & 0o7777   # permission bits only; type bits come from ifmt

    def mknod(self, real_path: str, mode: int, dev: int) -> None:
        """Emulate mknod: real file is a placeholder regular file; overlay carries
        the special-file type + rdev so a later stat reports a device node."""
        # placeholder already created by the wrapper (open O_CREAT); here we key it
        k = self.key_for(real_path, follow=False)
        ov = self._t.setdefault(k, Overlay())
        ov.ifmt = statmod.S_IFMT(mode)         # S_IFCHR / S_IFBLK / S_IFIFO ...
        ov.mode = mode & 0o7777
        ov.rdev = dev
        # mknod is performed as root: owner becomes the fake root
        ov.uid = self.fake_uid
        ov.gid = self.fake_gid

    # ---- the read path: real stat, then overlay -------------------------------
    def stat_overlay(self, real_path: str, follow: bool = True) -> os.stat_result:
        st = os.stat(real_path) if follow else os.lstat(real_path)
        k = (st.st_dev, st.st_ino)
        ov = self._t.get(k)
        # materialize a mutable copy of the 10-tuple stat fields
        fields = list(st)  # st_mode,ino,dev,nlink,uid,gid,size,atime,mtime,ctime
        if ov is not None:
            mode = fields[0]
            if ov.ifmt is not None:
                mode = ov.ifmt | (mode & 0o7777)
            if ov.mode is not None:
                mode = (statmod.S_IFMT(mode)) | ov.mode
            fields[0] = mode
            if ov.uid is not None:
                fields[4] = ov.uid
            if ov.gid is not None:
                fields[5] = ov.gid
        return os.stat_result(fields)

    # ---- serialization parity with the mmap layout ---------------------------
    def n_entries(self) -> int:
        return len(self._t)
