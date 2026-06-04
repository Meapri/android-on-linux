"""Host tests for the in-process PTY emulation (the TERMINAL app class).

Two layers:

1. BEHAVIOURAL — compile and run the syscall-free PTY-emulation core
   (app/src/main/cpp/alr_interpose/alr_pts.c) together with its native unit test
   (tests/native_alr_pts_test.c) using a host cc. This exercises path
   classification, /dev/pts/N parsing, termios/winsize defaults, and the full
   ioctl dispatch (TIOCGPTN / TIOCSPTLCK / TIOCGPTPEER / TIOC[GS]WINSZ /
   TCGETS / TCSETS / TCGETS2 / TCSETS2 / TIOC[GS]PGRP / TIOCGSID / TIOCSCTTY /
   TIOCNOTTY) against an in-memory struct alr_pty.

2. STRUCTURAL — assert the header declares the core API, the interposer
   (libalr_interpose.c) wires the open hook + the ioctl/isatty/tc*/ptsname/
   grantpt/unlockpt/posix_openpt shims, the build script compiles both TUs, and
   the device .so cross-compiles for arm64 with zig.  Also pins the sakura
   TERMINAL catalog entry.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
ITP_DIR = ROOT / "app/src/main/cpp/alr_interpose"
PTS_H = ITP_DIR / "alr_pts.h"
PTS_C = ITP_DIR / "alr_pts.c"
ITP_C = ITP_DIR / "libalr_interpose.c"
NATIVE_TEST = ROOT / "tests/native_alr_pts_test.c"
BUILD_SH = ROOT / "scripts/build-interpose.sh"
CATALOG = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/NativeAlrRuntime.kt"


def _cc():
    for c in ("cc", "clang", "gcc"):
        p = shutil.which(c)
        if p:
            return p
    return None


# ----------------------------- behavioural ----------------------------- #


def test_pts_core_unit_tests_pass(tmp_path):
    """Compile + run the PTY core's native unit tests; require exit 0."""
    cc = _cc()
    if not cc:
        pytest.skip("no host C compiler (cc/clang/gcc) available")
    bin_ = tmp_path / "alr-pts-test"
    cmd = [
        cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ITP_DIR),
        str(NATIVE_TEST), str(PTS_C),
        "-o", str(bin_),
    ]
    cp = subprocess.run(cmd, capture_output=True, text=True)
    assert cp.returncode == 0, f"compile failed:\n{cp.stderr}"
    run = subprocess.run([str(bin_)], capture_output=True, text=True)
    assert run.returncode == 0, f"pts core tests failed:\n{run.stdout}\n{run.stderr}"
    assert "all tests passed" in run.stdout


# ----------------------------- structural ----------------------------- #


def test_header_declares_core_api():
    h = PTS_H.read_text()
    for sym in (
        "alr_pty",
        "alr_ktermios",
        "alr_ktermios2",
        "alr_pty_init_termios",
        "alr_pty_init_winsize",
        "alr_pts_is_ptmx_path",
        "alr_pts_is_ptsdir_path",
        "alr_pts_is_tty_path",
        "alr_pts_parse_slave_path",
        "alr_pts_emulate_ioctl",
        "ALR_PTS_IOCTL_PASS",
        "ALR_PTS_IOCTL_GPTPEER",
        "ALR_TCGETS2",
        "ALR_TCSETS2",
    ):
        assert sym in h, f"missing {sym} in alr_pts.h"


def test_core_is_syscall_free_and_dep_light():
    """The core must touch no syscalls / sockets: only memory + the caller buf.

    Check actual #include lines (prose in comments legitimately mentions
    socketpair/ioctl etc.)."""
    c = PTS_C.read_text()
    include_lines = [
        ln.strip() for ln in c.splitlines() if ln.lstrip().startswith("#include")
    ]
    for forbidden in ("<sys/socket.h>", "<unistd.h>", "<dlfcn.h>", "<sys/syscall.h>"):
        assert not any(forbidden in ln for ln in include_lines), \
            f"pts core must not #include {forbidden}"
    # No raw syscalls / fd machinery in the pure core.
    for banned in ("socketpair(", "syscall(", "dlsym(", "alr_tramp_syscall("):
        assert banned not in c, f"pts core must not call {banned}"


def test_interposer_wires_pty_open_hook_and_ioctl():
    c = ITP_C.read_text()
    # the open hook + its forward declaration
    assert "alr_pts_try_open" in c
    assert '#include "alr_pts.h"' in c
    # minting is backed by a socketpair through the trampoline
    assert "__NR_socketpair" in c
    assert "alr_pts_mint" in c
    # the FIRST ioctl wrapper, plus the GPTPEER dup path
    assert "int ioctl(int fd, unsigned long request, ...)" in c
    assert "ALR_PTS_IOCTL_GPTPEER" in c
    assert "alr_pts_dup_slave" in c
    # the open hook is reached from alr_open_emit (the PCGATE chokepoint)
    assert "alr_open_emit" in c
    # the unconditional launch marker (device-verify signal in logcat)
    assert "ALR-PTY ptmx-served ptn=" in c


def test_interposer_carries_pty_identity_across_exec():
    """The exec'd shell gets a fresh (empty) pty table, so the slave-stdio tty
    identity must be re-established. The interposer sets ALR_PTY_ACTIVE on mint
    (inherited by the forked shell) and, when set, lazily adopts an inherited
    AF_UNIX SOCK_STREAM stdio fd as a pty slave via SO_DOMAIN/SO_TYPE probing."""
    c = ITP_C.read_text()
    assert 'setenv("ALR_PTY_ACTIVE", "1", 1)' in c
    assert 'getenv("ALR_PTY_ACTIVE")' in c
    assert "alr_pts_resolve" in c            # the lazy-adopt resolver
    assert "alr_fd_is_unix_stream" in c      # the socket-type gate
    assert "SO_DOMAIN" in c and "SO_TYPE" in c
    # the TTY shims route through the lazy resolver (not the table-only lookup)
    assert "alr_pts_resolve(fd, NULL)" in c  # isatty/tcflush/…
    assert "alr_pts_resolve(fd, &is_master)" in c  # tcgetattr/tcsetattr/ttyname_r
    # lazy-adopt is scoped to the exec'd shell (not the minter) + stdio fds only
    assert "g_pty_minted" in c
    assert "fd > 2" in c
    # the window size is carried across exec so a TUI draws at the real size
    assert "ALR_PTY_WINSZ" in c


def test_interposer_provides_tty_libc_shims():
    c = ITP_C.read_text()
    for fn in (
        "int isatty(int fd)",
        "int tcgetattr(int fd",
        "int tcsetattr(int fd",
        "pid_t tcgetpgrp(int fd)",
        "int tcsetpgrp(int fd",
        "int ptsname_r(int fd",
        "char *ptsname(int fd)",
        "int ttyname_r(int fd",
        "int grantpt(int fd)",
        "int unlockpt(int fd)",
        "int posix_openpt(int oflags)",
        "int openpty(int *amaster",
        "pid_t forkpty(int *amaster",
        "int close(int fd)",
    ):
        assert fn in c, f"missing TTY libc shim: {fn}"


def test_build_script_compiles_both_tus_and_rootfs_path():
    sh = BUILD_SH.read_text()
    assert "aarch64-linux-gnu" in sh
    assert "./usr/lib/androlinux/libalr_interpose.so" in sh
    assert "libalr_interpose.c" in sh
    assert "alr_pts.c" in sh
    assert "$SRC_PTS" in sh or "alr_pts.c" in sh


def test_device_interposer_cross_compiles_with_zig(tmp_path):
    """If zig is present, the device .so (both TUs) must cross-compile for arm64."""
    zig = shutil.which("zig")
    if not zig:
        pytest.skip("zig not available for cross-compile check")
    out = tmp_path / "libalr_interpose.so"
    cmd = [
        zig, "cc", "--target=aarch64-linux-gnu.2.36",
        "-shared", "-fPIC", "-O2",
        "-I", str(ITP_DIR),
        str(ITP_C), str(PTS_C),
        "-o", str(out),
    ]
    cp = subprocess.run(cmd, capture_output=True, text=True)
    assert cp.returncode == 0, f"zig cross-compile failed:\n{cp.stderr}"
    assert out.exists() and out.stat().st_size > 0
    # the emulation's exported libc shims must be present in the .so
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if nm:
        syms = subprocess.run([nm, "-D", str(out)], capture_output=True, text=True).stdout
        for s in ("ioctl", "isatty", "tcgetattr", "ptsname", "posix_openpt",
                  "unlockpt", "openpty", "forkpty"):
            assert f" T {s}" in syms or f" t {s}" in syms, f"{s} not exported by the .so"


def test_sakura_terminal_catalog_entry_present():
    """The TERMINAL app class is wired: sakura (VTE) is catalogued as an apt app
    with the TERMINAL category and the /usr/bin/sakura entrypoint."""
    k = CATALOG.read_text()
    assert 'appId = "sakura"' in k
    assert "AppCategory.TERMINAL" in k
    assert "/usr/bin/sakura" in k
    assert 'RootfsDep(RootfsDepKind.APT, "sakura"' in k
