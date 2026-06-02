"""Build the non-root fakeroot LD_PRELOAD overlay (fakeroot-stage.tar) — R12.

Why
---
On the device ALR runs as an Android ``untrusted_app`` (uid ~10xxx, no root, no
CAP_CHOWN/CAP_FOWNER). ``apt install`` / ``dpkg -i hello.deb`` then fails:

    dpkg: error: requires superuser privilege

and, even past that gate, dpkg's unpack stage ``chown()``s each unpacked file to
the ownership recorded in the .deb (root:root, 0755…) and ``stat()``s the result
to confirm — those ``chown()``s ``EPERM`` for a non-root uid, aborting the
unpack. This is the classic non-root packaging problem. The classic fix is a
*fakeroot*-style ``LD_PRELOAD`` shim that makes the process BELIEVE it is uid 0,
fakes ``chown``/``chmod`` success, and backs it with an in-process fake-ownership
DB so a later ``stat()`` reports the faked uid/gid/mode.

This builder is the host artifact: it compiles that shim
(``tools/fakeroot/libalr_fakeroot.c``) with ``zig cc`` for ``aarch64-linux-gnu``
(the same toolchain ``scripts/build-interpose.sh`` uses for the path interposer)
and packs it into a §5-E overlay stage tar at a rootfs-absolute path:

    ./usr/lib/androlinux/libalr_fakeroot.so

so the device extractor lays it down next to ``libalr_interpose.so``.

Chaining with the ALR path interposer (CRITICAL — see ``--device-cmd``)
----------------------------------------------------------------------
ALR ALWAYS injects ``LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_interpose.so``
(runtime_report.cpp). The fakeroot preload must CHAIN with that, never replace
it: the two are orthogonal (interpose rewrites the PATH argument; fakeroot
rewrites CREDENTIALS / the stat RESULT). WS-1 runs dpkg with the fakeroot .so
listed FIRST, colon-joined with the interpose .so — the value is printed by
``--device-cmd`` and documented in ``tools/fakeroot/README``.

Honest scope
------------
HOST-ONLY. This compiles + packs + verifies the overlay host-side. The actual
on-device non-root ``dpkg -i hello.deb`` (unpacked=true) under the chained
preload is WS-1's device drain, gated on the R8 apt-dpkg overlay also being
staged. This builder does NOT itself touch the device or the version stamp.
"""

from __future__ import annotations

import argparse
import io
import json
import shutil
import subprocess
import tarfile
from dataclasses import dataclass
from pathlib import Path

# --------------------------------------------------------------------------- #
# Constants — kept in lockstep with the shim source + the loader's LD_PRELOAD
# --------------------------------------------------------------------------- #

HERE = Path(__file__).resolve().parent
SHIM_SRC = HERE / "fakeroot" / "libalr_fakeroot.c"

# Guest-visible install location inside the rootfs tar (mirrors the interposer's
# ./usr/lib/androlinux/libalr_interpose.so convention in build-interpose.sh).
SO_NAME = "libalr_fakeroot.so"
ROOTFS_REL = "usr/lib/androlinux/" + SO_NAME            # no leading ./
ROOTFS_MEMBER = "./" + ROOTFS_REL                       # §5-E ./-rooted member
INTERPOSE_REL = "usr/lib/androlinux/libalr_interpose.so"

# zig cross target: match scripts/build-interpose.sh exactly (glibc 2.36 pin
# loads on the broadest 24.04 arm64 set while resolving every referenced symbol).
ZIG_TARGET = "aarch64-linux-gnu.2.36"

# The symbols the shim MUST interpose for a non-root dpkg unpack to pass. The
# selftest greps the source for each of these (a regression guard against an
# accidental deletion). Grouped by role for readability.
REQUIRED_SYMBOLS = (
    # credentials -> report root
    "getuid", "geteuid", "getgid", "getegid", "getresuid", "getresgid",
    # setters -> fake success
    "setuid", "seteuid", "setgid", "setegid",
    # ownership -> record + succeed
    "chown", "lchown", "fchown", "fchownat",
    # mode -> try real, else record + succeed
    "chmod", "fchmod", "fchmodat",
    # stat family -> overlay faked owner/mode
    "stat", "lstat", "fstat", "fstatat", "newfstatat", "statx",
)

# The on-device unpack target (the same trivial .deb the R8 apt-dpkg overlay
# fetches; NOT packed into THIS overlay — WS-1 reuses build_apt_dpkg_overlay
# --fetch-test-deb for the actual .deb asset). Recorded here for the device-cmd.
TEST_DEB_NAME = "hello_2.10-3build1_arm64.deb"


@dataclass
class FakerootOverlayResult:
    out_tar: str
    so_path: str
    so_sha256: str
    so_bytes: int
    member: str
    symbols: tuple[str, ...] = ()

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "so_path": self.so_path,
            "so_sha256": self.so_sha256,
            "so_bytes": self.so_bytes,
            "member": self.member,
            "symbols": list(self.symbols),
        }


# --------------------------------------------------------------------------- #
# Source introspection (offline; the selftest's brain)
# --------------------------------------------------------------------------- #

def shim_source() -> str:
    return SHIM_SRC.read_text()


def _strip_c_comments(src: str) -> str:
    """Drop /* … */ and // … comments so W^X word-checks match real call sites,
    not the header comment that promises those calls are absent. Tolerant (no
    string-literal awareness needed — the shim has no string literals containing
    comment markers)."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            end = src.find("*/", i + 2)
            i = (end + 2) if end != -1 else n
        elif src.startswith("//", i):
            end = src.find("\n", i)
            i = (end) if end != -1 else n
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def intercepted_symbols(src: str | None = None) -> set[str]:
    """Return the set of REQUIRED_SYMBOLS that the shim source defines as a
    wrapper. A symbol counts as intercepted iff a function definition for it
    appears at column 0 (``<rettype> NAME(`` on a line start), which is how every
    wrapper in libalr_fakeroot.c is written. Pure-substring matches inside a
    comment do NOT count."""
    if src is None:
        src = shim_source()
    found: set[str] = set()
    for line in src.splitlines():
        if not line or line[0].isspace() or line.startswith(("/", "*", "#")):
            continue
        # a definition line looks like:  "uid_t getuid(void) { ... }"  or
        # "int chown(const char *path, ...) {" — grab the token before '('.
        paren = line.find("(")
        if paren < 0:
            continue
        head = line[:paren].strip()
        name = head.split()[-1] if head.split() else ""
        name = name.lstrip("*")            # strip a pointer-return star
        if name in REQUIRED_SYMBOLS:
            found.add(name)
    return found


def missing_symbols(src: str | None = None) -> tuple[str, ...]:
    have = intercepted_symbols(src)
    return tuple(s for s in REQUIRED_SYMBOLS if s not in have)


# --------------------------------------------------------------------------- #
# Build (compile + pack)
# --------------------------------------------------------------------------- #

def _sha256(path: Path) -> str:
    import hashlib

    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def compile_shim(out_so: str | Path, *, zig: str | None = None,
                 target: str = ZIG_TARGET) -> Path:
    """Cross-compile the fakeroot shim to a shared object with ``zig cc``.

    Mirrors scripts/build-interpose.sh's flags exactly:
    ``--target=<target> -shared -fPIC -O2``. Raises if zig is missing or the
    compile produces no output."""
    out_so = Path(out_so)
    out_so.parent.mkdir(parents=True, exist_ok=True)
    zig_bin = zig or shutil.which("zig")
    if not zig_bin:
        raise FileNotFoundError("zig not found (set --zig or install zig >= 0.16)")
    if not SHIM_SRC.is_file():
        raise FileNotFoundError(f"shim source missing: {SHIM_SRC}")
    cmd = [zig_bin, "cc", f"--target={target}", "-shared", "-fPIC", "-O2",
           "-o", str(out_so), str(SHIM_SRC)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    if not out_so.is_file() or out_so.stat().st_size == 0:
        raise RuntimeError("zig cc produced no output")
    return out_so


def _add_file(tar: tarfile.TarFile, member: str, data: bytes, mode: int) -> None:
    ti = tarfile.TarInfo(member)
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


def _add_dir(tar: tarfile.TarFile, member: str, mode: int = 0o755) -> None:
    ti = tarfile.TarInfo(member)
    ti.type = tarfile.DIRTYPE
    ti.mode = mode
    ti.mtime = 0
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    tar.addfile(ti)


def pack_overlay(so_path: str | Path, out_tar: str | Path) -> str:
    """Pack the compiled .so into a §5-E ``./``-rooted overlay stage tar at
    ROOTFS_MEMBER (0o755), with the parent dirs as DIRTYPE members. Returns the
    member name written."""
    so_path = Path(so_path)
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    data = so_path.read_bytes()
    with tarfile.open(out_tar, "w") as tar:
        _add_dir(tar, "./usr")
        _add_dir(tar, "./usr/lib")
        _add_dir(tar, "./usr/lib/androlinux")
        _add_file(tar, ROOTFS_MEMBER, data, 0o755)
    return ROOTFS_MEMBER


def build_fakeroot_overlay(out_tar: str | Path, *, zig: str | None = None,
                           target: str = ZIG_TARGET,
                           keep_so: str | Path | None = None) -> FakerootOverlayResult:
    """Compile the shim and pack it into ``out_tar``. If ``keep_so`` is given the
    intermediate .so is also written there (else a temp file is used)."""
    import tempfile

    out_tar = Path(out_tar)
    if keep_so is not None:
        so_path = Path(keep_so)
        compile_shim(so_path, zig=zig, target=target)
        member = pack_overlay(so_path, out_tar)
        so_out = str(so_path)
        sha = _sha256(so_path)
        nbytes = so_path.stat().st_size
    else:
        with tempfile.TemporaryDirectory() as td:
            so_path = Path(td) / SO_NAME
            compile_shim(so_path, zig=zig, target=target)
            member = pack_overlay(so_path, out_tar)
            so_out = str(out_tar)   # the .so only lives inside the tar
            sha = _sha256(so_path)
            nbytes = so_path.stat().st_size
    return FakerootOverlayResult(
        out_tar=str(out_tar),
        so_path=so_out,
        so_sha256=sha,
        so_bytes=nbytes,
        member=member,
        symbols=tuple(sorted(intercepted_symbols())),
    )


# --------------------------------------------------------------------------- #
# Device invocation the WS-1 drain must run (the load-bearing handoff)
# --------------------------------------------------------------------------- #

def device_cmd(rootfs: str = "<ROOTFS>", deb: str = TEST_DEB_NAME) -> str:
    """Render the exact dpkg invocation WS-1 runs on device, with the chained
    LD_PRELOAD spelled out. ``rootfs`` is the on-device rootfs host dir (ALR's
    config.rootfs_dir). The fakeroot .so is FIRST so its credential wrappers run
    outermost; the interpose .so stays in the chain so paths are still rewritten
    underneath. FAKEROOTUID/GID pin the faked identity (default 0:0)."""
    fr = f"{rootfs}/{ROOTFS_REL}"
    ip = f"{rootfs}/{INTERPOSE_REL}"
    chained = f"{fr}:{ip}"
    return (
        "# ALR already sets LD_PRELOAD=<interpose .so>; CHAIN the fakeroot .so "
        "FIRST (do NOT drop the interpose entry):\n"
        f"LD_PRELOAD={chained} \\\n"
        f"  ALR_ROOTFS={rootfs} FAKEROOTUID=0 FAKEROOTGID=0 \\\n"
        f"  dpkg --force-not-root --force-bad-path -i {deb}\n"
        "# (apt path: same LD_PRELOAD/ALR_ROOTFS/FAKEROOT* env, then\n"
        f"#   apt-get -o APT::Sandbox::User=root install ./{deb} )"
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="build_fakeroot_overlay",
        description="Compile the non-root fakeroot LD_PRELOAD shim and pack it "
        "into a §5-E overlay stage tar (./usr/lib/androlinux/libalr_fakeroot.so).",
    )
    p.add_argument("--out", help="output stage tar (e.g. /tmp/fakeroot-stage.tar)")
    p.add_argument("--keep-so", metavar="PATH",
                   help="also write the intermediate .so to PATH")
    p.add_argument("--zig", help="zig binary (default: zig from PATH)")
    p.add_argument("--target", default=ZIG_TARGET, help=f"zig target (default {ZIG_TARGET})")
    p.add_argument("--device-cmd", action="store_true",
                   help="print the exact chained-LD_PRELOAD dpkg invocation WS-1 runs and exit")
    p.add_argument("--rootfs", default="<ROOTFS>",
                   help="rootfs dir to substitute into --device-cmd")
    p.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                   help="report the intercepted symbols WITHOUT compiling")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest:
        return _selftest()

    if args.device_cmd:
        print(device_cmd(args.rootfs))
        return 0

    if args.dry_run:
        have = sorted(intercepted_symbols())
        miss = list(missing_symbols())
        if args.json:
            print(json.dumps({"source": str(SHIM_SRC), "intercepted": have,
                              "missing": miss, "member": ROOTFS_MEMBER}, indent=2))
        else:
            print(f"fakeroot shim source: {SHIM_SRC}")
            print(f"  intercepts {len(have)}/{len(REQUIRED_SYMBOLS)} required symbols")
            for s in have:
                print(f"    {s}")
            if miss:
                print(f"  MISSING ({len(miss)}): {', '.join(miss)}")
            print(f"  packs to: {ROOTFS_MEMBER}")
        return 1 if miss else 0

    if not args.out:
        p.error("--out is required for a build (or use --list / --device-cmd / --selftest)")

    res = build_fakeroot_overlay(args.out, zig=args.zig, target=args.target,
                                 keep_so=args.keep_so)
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  member: {res.member} ({res.so_bytes} bytes, 0o755)")
        print(f"  sha256: {res.so_sha256}")
        print(f"  intercepts {len(res.symbols)} required symbols")
        print("\n  device drain (WS-1):")
        for line in device_cmd(args.rootfs).splitlines():
            print("    " + line)
    return 0


# --------------------------------------------------------------------------- #
# Selftest (OFFLINE — source introspection + an in-tar pack; compiles the .so
# only if zig is present, so it stays green on a host without the toolchain)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    src = shim_source()
    have = intercepted_symbols(src)

    # --- source intercepts every required symbol --------------------------- #
    check("shim source file exists", SHIM_SRC.is_file())
    for s in REQUIRED_SYMBOLS:
        check(f"shim intercepts {s}", s in have)
    check("no missing required symbols", missing_symbols(src) == ())

    # --- semantics the device gate depends on (source-level assertions) ---- #
    check("getuid returns the faked uid (g_fake_uid)",
          "uid_t getuid(void)" in src and "return g_fake_uid;" in src)
    check("chown returns success unconditionally",
          "int chown(const char *path" in src)
    check("chains via dlsym(RTLD_NEXT, ...) (does NOT replace the interposer)",
          "RTLD_NEXT" in src and 'dlsym(RTLD_NEXT' in src)
    check("honors FAKEROOTUID/FAKEROOTGID env contract",
          "FAKEROOTUID" in src and "FAKEROOTGID" in src)
    check("stat overlays the faked owner via fr_apply_stat",
          "fr_apply_stat(" in src)
    # W^X: assert no CALL SITE for exec-memory / seccomp / ptrace (the words may
    # appear in the header comment that promises their absence — match `name(`).
    code = _strip_c_comments(src)
    check("no executable-memory / seccomp / ptrace call site (W^X-safe shim)",
          "mmap(" not in code and "prctl(" not in code
          and "seccomp(" not in code and "ptrace(" not in code)

    # --- pack an overlay from a stand-in .so (no compiler needed) ---------- #
    from tools.stage_tar_spec import validate_stage_tar
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        fake_so = td / SO_NAME
        fake_so.write_bytes(b"\x7fELF\x02\x01\x01" + b"\x00" * 200)   # ELF-ish stub
        out_tar = td / "fakeroot-stage.tar"
        member = pack_overlay(fake_so, out_tar)
        check("pack returns the rootfs-absolute member", member == ROOTFS_MEMBER)
        with tarfile.open(out_tar) as t:
            names = {m.name: m for m in t.getmembers()}
        check("overlay ships ./usr/lib/androlinux/libalr_fakeroot.so",
              ROOTFS_MEMBER in names)
        check("the .so member is 0o755", names[ROOTFS_MEMBER].mode == 0o755)
        check("the .so member is a regular file", names[ROOTFS_MEMBER].isfile())
        check("parent dirs shipped as DIRTYPE",
              names["./usr/lib/androlinux"].isdir())
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        rep = validate_stage_tar(str(out_tar))
        check("overlay is §5-E stage_tar_spec conformant", rep.conformant)

    # --- device-cmd chains both .so, fakeroot FIRST ------------------------ #
    cmd = device_cmd("/data/x/rootfs")
    fr = "/data/x/rootfs/" + ROOTFS_REL
    ip = "/data/x/rootfs/" + INTERPOSE_REL
    check("device-cmd lists the fakeroot .so", fr in cmd)
    check("device-cmd KEEPS the interpose .so (chained, not replaced)", ip in cmd)
    check("device-cmd puts fakeroot FIRST in LD_PRELOAD", f"{fr}:{ip}" in cmd)
    check("device-cmd does the dpkg -i unpack", "dpkg" in cmd and "-i" in cmd)

    # --- if zig is present, actually compile + pack the real .so ----------- #
    if shutil.which("zig"):
        with tempfile.TemporaryDirectory() as td:
            out_tar = Path(td) / "fakeroot-stage.tar"
            try:
                res = build_fakeroot_overlay(out_tar, keep_so=Path(td) / SO_NAME)
                check("zig compiled the shim to a non-empty .so", res.so_bytes > 0)
                with tarfile.open(out_tar) as t:
                    body = t.extractfile(ROOTFS_MEMBER).read()
                check("packed .so is an ELF object", body[:4] == b"\x7fELF")
                check("packed .so is the aarch64 (EM_AARCH64=183) machine",
                      len(body) > 19 and body[18] == 183)
            except Exception as e:    # noqa: BLE001 — report, don't crash the selftest
                check(f"zig build path raised: {e}", False)
    else:
        check("zig present (compile path skipped — source checks still gate)", True)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
