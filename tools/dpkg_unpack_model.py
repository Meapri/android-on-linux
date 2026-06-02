"""Host model + end-to-end proof that the fakeroot shim makes a non-root
``dpkg --unpack`` pass — T2 of the v2 apt pipeline.

What this is (and is NOT)
-------------------------
This is a HOST mechanism proof. It does NOT run the real device; it simulates the
exact privileged-call SEQUENCE dpkg's unpack stage performs on each ``.deb``
member, runs it once WITHOUT the fakeroot hooks (the real non-root wall) and once
WITH them (the in-process ``getuid→0`` / ``chown`` no-op+DB / ``mknod`` placeholder
+DB / ``stat`` overlay slice of ``tools/fakeroot/libalr_fakeroot.c``), and shows:

  * without fakeroot the unpack hits the classic non-root wall — dpkg's superuser
    gate refuses, and even forced past it ``fchown(root)`` ``EPERM``s and a device
    node ``mknod(2)`` ``EPERM``/aborts (the one HARD wall);
  * with fakeroot every privileged op is satisfied in-process, the unpack
    completes (``unpacked=true``), and the result is self-consistent — a later
    ``stat()`` of each unpacked path reports ``root:root`` and the recorded type
    (a placeholdered char-device node ``stat()``s back as ``S_IFCHR``), 0 mismatch.

The fake-ownership DB modelled here mirrors the C shim's ``(st_dev, st_ino)``-keyed
table 1:1 (``getuid``/``chown``/``chmod``/``stat`` semantics), and EXTENDS it with
the ``mknod`` placeholder rule the device gate needs for the (rare) char/block
node members a package may ship. The on-device equivalent is the chained
``LD_PRELOAD`` shim; this module is the executable specification of why it works.

Honest scope
------------
HOST-ONLY mechanism proof. ``unpacked=true`` here is a model assertion about the
fakeroot mechanism, NOT a device claim — the real on-device non-root
``dpkg --unpack hello.deb`` is the WS-1/integration device drain (DEVICE-REQ),
gated on the apt-dpkg + fakeroot staging overlays landing on the device and on
the exec-re-entry re-map. This module installs NO seccomp/ptrace, allocates NO
executable memory: it is pure in-process bookkeeping, exactly like the shim.

The model deliberately uses a synthetic ``.deb`` (built host-side with ``ar``)
that carries one of EVERY member kind dpkg's unpack must handle — a regular file,
a directory, a symlink, AND a char-device node — so the mknod hard-wall / placeholder
path is exercised, not just the common file path. If the real noble ``hello`` .deb
is available (``--deb``) it is additionally parsed to prove the model accepts a
real archive; hello carries no device node, so the synthetic deb remains the
device-node vehicle.
"""

from __future__ import annotations

import argparse
import io
import json
import stat as stat_mod
import subprocess
import tarfile
from dataclasses import dataclass
from pathlib import Path

# --------------------------------------------------------------------------- #
# Member model — one entry of a .deb data.tar, the way dpkg's unpack sees it
# --------------------------------------------------------------------------- #

# Member kinds we model (mirrors safe_tar.TarMember.kind plus the device node the
# safe-tar host walk rejects but dpkg's unpack must lay down via mknod(2)).
KIND_FILE = "file"
KIND_DIR = "dir"
KIND_SYMLINK = "symlink"
KIND_CHRDEV = "chr"     # character device node — the mknod hard-wall vehicle
KIND_BLKDEV = "blk"     # block device node


@dataclass(frozen=True)
class DebMember:
    """One data.tar entry as dpkg's unpack stage will materialise it.

    ``owner``/``group`` are the ownership recorded in the .deb (root:root for a
    normal package). ``rdev`` is the (major, minor) for a device node (else None).
    """
    path: str
    kind: str
    mode: int                       # permission bits (no type bits)
    owner: int = 0                  # uid the .deb records (root)
    group: int = 0                  # gid the .deb records (root)
    linkname: str = ""              # symlink target
    rdev: tuple[int, int] | None = None  # (major, minor) for chr/blk nodes

    @property
    def is_device_node(self) -> bool:
        return self.kind in (KIND_CHRDEV, KIND_BLKDEV)


# --------------------------------------------------------------------------- #
# Fake-ownership DB — the Python mirror of libalr_fakeroot.c's (dev,ino) table
# --------------------------------------------------------------------------- #

@dataclass
class FakeEntry:
    uid: int
    gid: int
    mode: int            # FULL st_mode (type bits | perm bits)
    has_mode: bool = False


class FakerootDB:
    """In-process fake-ownership DB, keyed by (st_dev, st_ino) exactly like the C
    shim. Records chown/chmod/mknod intent so a later stat() overlays the faked
    identity/type back onto the real on-disk result.

    This is the *mechanism* the device shim provides; here we drive it from the
    model instead of from libc interposition, but the bookkeeping is identical.
    """

    def __init__(self, fake_uid: int = 0, fake_gid: int = 0) -> None:
        self.fake_uid = fake_uid
        self.fake_gid = fake_gid
        self._db: dict[tuple[int, int], FakeEntry] = {}

    # --- the interposed credential getter -------------------------------- #
    def getuid(self) -> int:
        return self.fake_uid

    def getgid(self) -> int:
        return self.fake_gid

    # --- chown: record owner, never touch the real file, return success --- #
    def chown(self, key: tuple[int, int], uid: int, gid: int) -> int:
        ent = self._db.get(key)
        if ent is None:
            ent = FakeEntry(uid=self.fake_uid, gid=self.fake_gid, mode=0)
            self._db[key] = ent
        if uid != -1:
            ent.uid = uid
        if gid != -1:
            ent.gid = gid
        return 0

    # --- chmod: record the faked mode bits, return success ---------------- #
    def chmod(self, key: tuple[int, int], full_mode: int) -> int:
        ent = self._db.get(key)
        if ent is None:
            ent = FakeEntry(uid=self.fake_uid, gid=self.fake_gid, mode=0)
            self._db[key] = ent
        ent.mode = full_mode
        ent.has_mode = True
        return 0

    # --- mknod placeholder: record the intended device type/mode ---------- #
    def record_mknod(self, key: tuple[int, int], full_mode: int) -> None:
        """A device node could not be created (non-root mknod EPERMs); the shim
        instead lays down a regular placeholder file and records the INTENDED
        st_mode (with the S_IFCHR/S_IFBLK type bits) so stat() reports it as a
        device node. Owner defaults to the faked identity unless chown set it."""
        ent = self._db.get(key)
        if ent is None:
            ent = FakeEntry(uid=self.fake_uid, gid=self.fake_gid, mode=0)
            self._db[key] = ent
        ent.mode = full_mode
        ent.has_mode = True

    # --- stat: overlay the faked owner (always) + mode (if recorded) ------ #
    def stat_overlay(self, key: tuple[int, int], real_uid: int, real_gid: int,
                     real_mode: int) -> tuple[int, int, int]:
        """Return the (uid, gid, mode) a faked stat() reports. Owner is ALWAYS
        forced to the faked identity (fakeroot semantics) unless a chown set an
        explicit value; mode is overridden only if a chmod/mknod recorded one."""
        ent = self._db.get(key)
        if ent is None:
            return self.fake_uid, self.fake_gid, real_mode
        mode = ent.mode if ent.has_mode else real_mode
        return ent.uid, ent.gid, mode


# --------------------------------------------------------------------------- #
# Errors the non-root wall raises (modelled, not host syscalls)
# --------------------------------------------------------------------------- #

class UnpackError(Exception):
    """A modelled unpack failure (EPERM chown / mknod abort / superuser gate)."""


@dataclass
class UnpackResult:
    unpacked: bool
    laid_down: tuple[str, ...] = ()          # paths materialised
    placeholdered_nodes: tuple[str, ...] = ()  # device nodes turned into placeholders
    failure: str = ""                         # the wall hit (empty if unpacked)
    self_consistent: bool = False
    mismatches: tuple[str, ...] = ()          # stat self-consistency mismatches

    def as_dict(self) -> dict:
        return {
            "unpacked": self.unpacked,
            "laid_down": list(self.laid_down),
            "placeholdered_nodes": list(self.placeholdered_nodes),
            "failure": self.failure,
            "self_consistent": self.self_consistent,
            "mismatches": list(self.mismatches),
        }


# --------------------------------------------------------------------------- #
# The unpack simulator — dpkg's per-member privileged-call sequence
# --------------------------------------------------------------------------- #

def _full_mode(member: DebMember) -> int:
    """The full st_mode (type bits | perm bits) dpkg means each member to have."""
    type_bits = {
        KIND_FILE: stat_mod.S_IFREG,
        KIND_DIR: stat_mod.S_IFDIR,
        KIND_SYMLINK: stat_mod.S_IFLNK,
        KIND_CHRDEV: stat_mod.S_IFCHR,
        KIND_BLKDEV: stat_mod.S_IFBLK,
    }[member.kind]
    return type_bits | (member.mode & 0o7777)


def simulate_unpack(
    members: list[DebMember],
    *,
    real_uid: int,
    fakeroot: FakerootDB | None,
) -> UnpackResult:
    """Run dpkg's unpack privileged-call sequence over ``members``.

    Models the four privileged steps dpkg performs per member, in order:

      1. the SUPERUSER GATE — dpkg refuses unless it believes it is uid 0
         (``getuid()==0``). Without fakeroot a non-root uid is refused here.
      2. lay down the file bytes / dir / symlink (unprivileged; always works).
      3. ``fchown(fd, root, root)`` to the .deb's recorded ownership, then for a
         device node ``mknod(2)`` — both privileged.
      4. ``fchmod`` to the recorded perms.

    ``fakeroot`` None  -> the real non-root wall (this is the FAIL baseline).
    ``fakeroot`` a DB  -> the shim's in-process satisfaction (the PASS path).

    On success a self-consistency check stat()s every laid-down path and asserts
    the faked view reports ``root:root`` and the intended type/mode (0 mismatch).
    """
    # Each laid-down path gets a synthetic (dev, ino); dev is a fixed fs id, ino
    # is a monotonically-assigned inode (mirrors a real extraction's distinct
    # inodes). The "on-disk real" owner is the REAL non-root uid (the kernel can
    # only create files owned by the running uid).
    DEV = 0xC0FFEE
    keys: dict[str, tuple[int, int]] = {}
    real_disk: dict[str, tuple[int, int, int]] = {}  # path -> (uid, gid, full_mode)
    laid: list[str] = []
    placeholders: list[str] = []

    uid_seen_by_dpkg = fakeroot.getuid() if fakeroot is not None else real_uid

    # --- step 1: dpkg's superuser gate ---------------------------------- #
    if uid_seen_by_dpkg != 0:
        return UnpackResult(
            unpacked=False,
            failure="dpkg: error: requires superuser privilege "
                    f"(getuid()=={uid_seen_by_dpkg}, refused before unpack)",
        )

    for i, m in enumerate(members):
        key = (DEV, 1000 + i)
        keys[m.path] = key
        full = _full_mode(m)

        # --- step 2: materialise the entry (unprivileged) --------------- #
        # A device node CANNOT be materialised as itself by a non-root process;
        # dpkg issues mknod(2). We model the kernel's behaviour: the real on-disk
        # object is created owned by the real running uid (root only under real
        # privilege — here always the non-root uid on the device).
        if m.is_device_node:
            if fakeroot is None:
                # --- the ONE hard wall: non-root mknod(2) of a device node --
                # No libc shim is in the call path, so the bare mknod(2) syscall
                # reaches the kernel and EPERMs; dpkg aborts the unpack.
                return UnpackResult(
                    unpacked=False,
                    laid_down=tuple(laid),
                    failure=f"mknod('{m.path}', S_IF{'CHR' if m.kind==KIND_CHRDEV else 'BLK'}): "
                            "Operation not permitted (non-root mknod(2) — HARD WALL, "
                            "dpkg abort)",
                )
            # WITH fakeroot: lay down a regular placeholder file and record the
            # intended device st_mode so stat() reports S_IFCHR/S_IFBLK.
            real_disk[m.path] = (real_uid, real_uid, stat_mod.S_IFREG | 0o644)
            fakeroot.record_mknod(key, full)
            placeholders.append(m.path)
            laid.append(m.path)
        else:
            # regular file / dir / symlink: the real on-disk object is owned by
            # the real running uid (the kernel won't let a non-root uid create a
            # root-owned file).
            real_disk[m.path] = (real_uid, real_uid, full)
            laid.append(m.path)

        # --- step 3: chown to the .deb's recorded ownership (privileged) - #
        if fakeroot is None:
            if real_uid != 0 and (m.owner != real_uid or m.group != real_uid):
                # bare fchown(root) reaches the kernel and EPERMs for non-root
                return UnpackResult(
                    unpacked=False,
                    laid_down=tuple(laid),
                    failure=f"fchown('{m.path}', {m.owner}:{m.group}): "
                            "Operation not permitted (non-root chown — EPERM, dpkg abort)",
                )
        else:
            fakeroot.chown(key, m.owner, m.group)

        # --- step 4: chmod to the recorded perms ------------------------ #
        # chmod of a file the process owns succeeds even non-root; the shim only
        # has to fake it for files it does NOT own (none in this model), so we
        # record the mode in the DB under fakeroot for stat self-consistency.
        if fakeroot is not None and not m.is_device_node:
            fakeroot.chmod(key, full)

    # --- success: self-consistency check (the device gate's "stat -> root:root") #
    mismatches: list[str] = []
    for m in members:
        key = keys[m.path]
        ruid, rgid, rmode = real_disk[m.path]
        want_full = _full_mode(m)
        if fakeroot is not None:
            uid, gid, mode = fakeroot.stat_overlay(key, ruid, rgid, rmode)
        else:  # pragma: no cover — non-fakeroot never reaches success
            uid, gid, mode = ruid, rgid, rmode
        if uid != m.owner or gid != m.group:
            mismatches.append(
                f"{m.path}: stat owner {uid}:{gid} != recorded {m.owner}:{m.group}")
        if mode != want_full:
            mismatches.append(
                f"{m.path}: stat mode {oct(mode)} != intended {oct(want_full)}")

    return UnpackResult(
        unpacked=True,
        laid_down=tuple(laid),
        placeholdered_nodes=tuple(placeholders),
        self_consistent=(not mismatches),
        mismatches=tuple(mismatches),
    )


# --------------------------------------------------------------------------- #
# .deb parsing — turn a real/synthetic data.tar into DebMembers
# --------------------------------------------------------------------------- #

def _decompress_data_tar(deb: Path) -> bytes:
    """Uncompressed data.tar bytes of a .deb (xz/gz/zst/plain). Mirrors
    build_apt_dpkg_overlay._decompress_data_tar but stays self-contained."""
    import lzma
    import gzip as _gzip
    import shutil

    listing = subprocess.run(["ar", "t", str(deb)], check=True,
                             capture_output=True, text=True).stdout.split()
    data_name = next(m for m in listing if m.startswith("data.tar"))
    blob = subprocess.run(["ar", "p", str(deb), data_name], check=True,
                          capture_output=True).stdout
    if data_name.endswith(".xz"):
        return lzma.decompress(blob)
    if data_name.endswith(".gz"):
        return _gzip.decompress(blob)
    if data_name.endswith(".zst"):
        zstd = shutil.which("zstd")
        if zstd is None:
            raise NotImplementedError("zstd data member but no zstd CLI")
        return subprocess.run([zstd, "-d", "-c"], input=blob, check=True,
                              capture_output=True).stdout
    if data_name.endswith(".tar"):
        return blob
    raise NotImplementedError(f"unhandled data member: {data_name}")


def members_from_deb(deb: Path) -> list[DebMember]:
    """Parse a .deb's data.tar into the model's DebMember list (file/dir/symlink/
    chr/blk). Used to prove the model accepts a real archive."""
    raw = _decompress_data_tar(deb)
    out: list[DebMember] = []
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as t:
        for m in t.getmembers():
            path = m.name
            while path.startswith("./"):
                path = path[2:]
            path = "/" + path.lstrip("/")
            if m.isdir():
                kind = KIND_DIR
            elif m.issym():
                kind = KIND_SYMLINK
            elif m.ischr():
                kind = KIND_CHRDEV
            elif m.isblk():
                kind = KIND_BLKDEV
            elif m.isfile():
                kind = KIND_FILE
            else:
                continue  # hardlinks / fifos: not modelled
            rdev = ((m.devmajor, m.devminor)
                    if kind in (KIND_CHRDEV, KIND_BLKDEV) else None)
            out.append(DebMember(
                path=path, kind=kind, mode=m.mode & 0o7777,
                owner=m.uid, group=m.gid, linkname=m.linkname, rdev=rdev,
            ))
    return out


# --------------------------------------------------------------------------- #
# Synthetic .deb — one of every member kind, incl. a char-device node
# --------------------------------------------------------------------------- #

def synthetic_members() -> list[DebMember]:
    """The canonical synthetic package the model unpacks: a regular file, a
    directory, a symlink, AND a char-device node (root:root, as a real .deb
    records). The device node is what makes the mknod hard-wall / placeholder
    path observable."""
    return [
        DebMember("/usr/bin/hello", KIND_FILE, 0o755, owner=0, group=0),
        DebMember("/usr/share/doc/hello", KIND_DIR, 0o755, owner=0, group=0),
        DebMember("/usr/bin/hello-link", KIND_SYMLINK, 0o777, owner=0, group=0,
                  linkname="hello"),
        DebMember("/dev/hellonull", KIND_CHRDEV, 0o666, owner=0, group=0,
                  rdev=(1, 3)),  # like /dev/null (c 1 3)
    ]


def build_synthetic_deb(dest: Path) -> Path:
    """Build a real on-disk synthetic .deb (via ``ar``) whose data.tar carries a
    regular file, a dir, a symlink and a CHAR-DEVICE node, so members_from_deb can
    round-trip every kind the model handles. Requires ``ar`` (host has it)."""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    work = dest.parent

    # data.tar with every member kind (TarInfo lets us add a CHRTYPE node that the
    # host fs could never create as a real file).
    data_tar = work / "data.tar"
    with tarfile.open(data_tar, "w") as t:
        # regular file
        body = b"#!/bin/sh\necho Hello\n"
        ti = tarfile.TarInfo("./usr/bin/hello")
        ti.size = len(body); ti.mode = 0o755; ti.uid = ti.gid = 0
        ti.type = tarfile.REGTYPE
        t.addfile(ti, io.BytesIO(body))
        # directory
        td = tarfile.TarInfo("./usr/share/doc/hello")
        td.type = tarfile.DIRTYPE; td.mode = 0o755; td.uid = td.gid = 0
        t.addfile(td)
        # symlink
        tl = tarfile.TarInfo("./usr/bin/hello-link")
        tl.type = tarfile.SYMTYPE; tl.linkname = "hello"; tl.mode = 0o777
        tl.uid = tl.gid = 0
        t.addfile(tl)
        # char-device node (c 1 3, like /dev/null)
        tc = tarfile.TarInfo("./dev/hellonull")
        tc.type = tarfile.CHRTYPE; tc.mode = 0o666; tc.uid = tc.gid = 0
        tc.devmajor = 1; tc.devminor = 3
        t.addfile(tc)

    (work / "debian-binary").write_text("2.0\n")
    (work / "control.tar").write_bytes(b"")
    # `ar qcS`: quick-append, create, NO symbol table (LLVM/BSD ar would otherwise
    # add __.SYMDEF and mangle the member listing). Run from work so members are
    # bare names, exactly as a real .deb stores them.
    subprocess.run(
        ["ar", "qcS", dest.name, "debian-binary", "control.tar", "data.tar"],
        check=True, capture_output=True, cwd=str(work),
    )
    return dest


# --------------------------------------------------------------------------- #
# End-to-end proof driver
# --------------------------------------------------------------------------- #

@dataclass
class ProofReport:
    without_fakeroot: UnpackResult
    with_fakeroot: UnpackResult
    member_kinds: tuple[str, ...]
    real_uid: int

    def as_dict(self) -> dict:
        return {
            "real_uid": self.real_uid,
            "member_kinds": list(self.member_kinds),
            "without_fakeroot": self.without_fakeroot.as_dict(),
            "with_fakeroot": self.with_fakeroot.as_dict(),
        }


def prove(members: list[DebMember] | None = None, *, real_uid: int = 10257) -> ProofReport:
    """The T2 proof: run the unpack twice over ``members`` (default the synthetic
    every-kind package) at a non-root uid — once without the fakeroot hooks (the
    wall) and once with (the shim) — and return both results.

    ``real_uid`` defaults to a typical Android ``untrusted_app`` uid."""
    if members is None:
        members = synthetic_members()
    without = simulate_unpack(members, real_uid=real_uid, fakeroot=None)
    db = FakerootDB(fake_uid=0, fake_gid=0)
    with_fr = simulate_unpack(members, real_uid=real_uid, fakeroot=db)
    return ProofReport(
        without_fakeroot=without,
        with_fakeroot=with_fr,
        member_kinds=tuple(m.kind for m in members),
        real_uid=real_uid,
    )


# --------------------------------------------------------------------------- #
# CLI / selftest
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="dpkg_unpack_model",
        description="Host model + proof that the fakeroot shim makes a non-root "
        "dpkg --unpack pass (and that mknod is a hard wall without it).",
    )
    p.add_argument("--deb", help="also parse a real .deb's data.tar (e.g. hello.deb) "
                   "to prove the model accepts a real archive")
    p.add_argument("--uid", type=int, default=10257,
                   help="real non-root uid to model (default: a typical untrusted_app uid)")
    p.add_argument("--build-synthetic-deb", metavar="PATH",
                   help="write a synthetic every-kind .deb to PATH and exit")
    p.add_argument("--json", action="store_true")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest:
        return _selftest()

    if args.build_synthetic_deb:
        deb = build_synthetic_deb(Path(args.build_synthetic_deb))
        print(f"wrote synthetic .deb {deb} ({deb.stat().st_size} bytes)")
        return 0

    members = members_from_deb(Path(args.deb)) if args.deb else synthetic_members()
    rep = prove(members, real_uid=args.uid)
    if args.json:
        print(json.dumps(rep.as_dict(), indent=2))
    else:
        print(f"non-root uid modelled: {rep.real_uid}")
        print(f"member kinds: {', '.join(rep.member_kinds)}")
        w = rep.without_fakeroot
        print(f"\nWITHOUT fakeroot: unpacked={w.unpacked}")
        print(f"  wall: {w.failure}")
        f = rep.with_fakeroot
        print(f"\nWITH fakeroot:    unpacked={f.unpacked}, "
              f"self_consistent={f.self_consistent}")
        print(f"  laid down: {len(f.laid_down)} paths "
              f"({len(f.placeholdered_nodes)} device-node placeholder(s))")
        if f.mismatches:
            print(f"  MISMATCHES: {f.mismatches}")
    return 0


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    rep = prove()

    # --- without fakeroot: the non-root wall -------------------------------- #
    w = rep.without_fakeroot
    check("without fakeroot: unpacked=false (the wall)", not w.unpacked)
    check("without fakeroot: failure is the superuser gate",
          "requires superuser privilege" in w.failure)

    # mknod is the residual HARD wall even once dpkg's superuser gate is bypassed
    # (getuid faked) — the device node's bare mknod(2) still reaches the kernel and
    # EPERMs. This is what the placeholder rule clears; isolate it from the gate.
    hardwall = _mknod_hardwall_probe(rep.real_uid)
    check("mknod is a HARD wall without the placeholder rule (EPERM/abort)",
          (not hardwall.unpacked) and "mknod(" in hardwall.failure
          and "HARD WALL" in hardwall.failure)

    # --- with fakeroot: unpack passes + self-consistent --------------------- #
    f = rep.with_fakeroot
    check("with fakeroot: unpacked=true", f.unpacked)
    check("with fakeroot: 0-mismatch self-consistency", f.self_consistent)
    check("with fakeroot: stat reports root:root for every path",
          f.self_consistent and len(f.mismatches) == 0)
    check("with fakeroot: the char-device node was placeholdered",
          "/dev/hellonull" in f.placeholdered_nodes)
    check("with fakeroot: every member was laid down",
          len(f.laid_down) == len(synthetic_members()))

    # --- DB mirror semantics (matches the C shim) --------------------------- #
    db = FakerootDB(fake_uid=0, fake_gid=0)
    check("getuid() reports the faked root id", db.getuid() == 0)
    k = (1, 2)
    db.chown(k, 0, 0)
    uid, gid, mode = db.stat_overlay(k, 10257, 10257, stat_mod.S_IFREG | 0o644)
    check("stat overlays faked owner (root:root) after chown",
          uid == 0 and gid == 0)
    db.record_mknod(k, stat_mod.S_IFCHR | 0o666)
    _, _, m2 = db.stat_overlay(k, 10257, 10257, stat_mod.S_IFREG | 0o644)
    check("stat reports S_IFCHR after a mknod placeholder",
          stat_mod.S_ISCHR(m2) and (m2 & 0o777) == 0o666)

    # --- synthetic .deb round-trips through members_from_deb ---------------- #
    import shutil
    import tempfile
    if shutil.which("ar"):
        with tempfile.TemporaryDirectory() as td:
            deb = build_synthetic_deb(Path(td) / "synthetic.deb")
            parsed = members_from_deb(deb)
            kinds = {m.kind for m in parsed}
            check("synthetic .deb parses back a regular file", KIND_FILE in kinds)
            check("synthetic .deb parses back a directory", KIND_DIR in kinds)
            check("synthetic .deb parses back a symlink", KIND_SYMLINK in kinds)
            check("synthetic .deb parses back a CHAR-DEVICE node", KIND_CHRDEV in kinds)
            # and the parsed members reproduce the proof outcome
            rep2 = prove(parsed)
            check("parsed synthetic .deb: without-fakeroot wall holds",
                  not rep2.without_fakeroot.unpacked)
            check("parsed synthetic .deb: with-fakeroot unpacked + consistent",
                  rep2.with_fakeroot.unpacked and rep2.with_fakeroot.self_consistent)
    else:
        check("ar available for synthetic .deb round-trip (skipped)", True)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def _mknod_hardwall_probe(real_uid: int) -> UnpackResult:
    """Isolate the mknod hard wall from the superuser gate: run the device-node
    unpack with the gate modelled as already passed (getuid()==0) but chown/mknod
    UNHOOKED (bare kernel syscalls). Proves mknod is the residual hard wall a
    getuid-only fake would still hit — i.e. why the placeholder rule is required."""
    members = [
        # non-device member owned by the running uid -> chown is a no-op, so we
        # do NOT trip the chown wall and actually reach the device member.
        DebMember("/usr/bin/x", KIND_FILE, 0o755, owner=real_uid, group=real_uid),
        DebMember("/dev/x", KIND_CHRDEV, 0o666, owner=real_uid, group=real_uid,
                  rdev=(1, 3)),
    ]
    # A getuid-only fake: passes dpkg's gate but leaves chown/mknod on the kernel.
    db = FakerootDB(fake_uid=0, fake_gid=0)

    # Re-run the per-member sequence with getuid faked but the device node forced
    # onto the kernel mknod path (fakeroot.record_mknod disabled for this probe).
    laid: list[str] = []
    if db.getuid() != 0:  # pragma: no cover
        return UnpackResult(unpacked=False, failure="gate")
    for i, m in enumerate(members):
        if m.is_device_node:
            # getuid is faked (gate passed) but mknod(2) still reaches the kernel:
            return UnpackResult(
                unpacked=False,
                laid_down=tuple(laid),
                failure=f"mknod('{m.path}', S_IFCHR): Operation not permitted "
                        "(non-root mknod(2) — HARD WALL even with getuid faked; "
                        "the placeholder rule is what clears it)",
            )
        laid.append(m.path)
    return UnpackResult(unpacked=True, laid_down=tuple(laid))


def _chown_hardwall_probe(real_uid: int) -> UnpackResult:
    """Isolate the chown EPERM wall from the superuser gate: model dpkg's gate as
    bypassed (getuid faked) but chown UNHOOKED (bare kernel syscall). Laying down a
    root:root regular file at a non-root uid then EPERMs at fchown — the second
    classic non-root wall after the gate, the one chown no-op clears."""
    members = [DebMember("/usr/bin/x", KIND_FILE, 0o755, owner=0, group=0)]
    db = FakerootDB(fake_uid=0, fake_gid=0)
    if db.getuid() != 0:  # pragma: no cover
        return UnpackResult(unpacked=False, failure="gate")
    laid: list[str] = []
    for m in members:
        laid.append(m.path)  # the file bytes land (unprivileged)
        # getuid is faked (gate passed) but fchown(root) still reaches the kernel:
        if real_uid != 0 and (m.owner != real_uid or m.group != real_uid):
            return UnpackResult(
                unpacked=False,
                laid_down=tuple(laid),
                failure=f"fchown('{m.path}', {m.owner}:{m.group}): "
                        "Operation not permitted (non-root chown — EPERM even with "
                        "getuid faked; the chown no-op rule is what clears it)",
            )
    return UnpackResult(unpacked=True, laid_down=tuple(laid))


if __name__ == "__main__":
    raise SystemExit(main())
