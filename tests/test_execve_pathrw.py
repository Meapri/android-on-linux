"""ADR-003 §6 P2 (B-1) — execve x0 vs *at-style x1 path-mediation decision model.

Host-side, pure-python regression of the *decision* the supervisor's
PTRACE_EVENT_SECCOMP path-rewrite handler must make once exec re-entry lands
(runtime_report.cpp L2086-2135). NO real ptrace, NO device: darwin cannot drive
aarch64 ptrace, so only the register-index + path-predicate logic is exercised
here (the inheritance/rewrite *effect* is DEVICE-ONLY — M-R4 gates).

The load-bearing code facts this model pins (ADR-003 §3, verified against
runtime_report.cpp):

  - *at-style path syscalls (openat/newfstatat/...) carry the path in **x1**
    (regs[1]). The current handler reads `path_addr = regs[1]` (L2092) and only
    rewrites when `!is_exec`.
  - execve/execveat carry the path in **x0** (regs[0]); x1 is argv (char**),
    x2 is envp (char**). The code explicitly SKIPS exec today (L2089-2093,
    comment "never treat an exec syscall's x1 as a path") because blindly
    rewriting x1 would corrupt argv. (B-1) must read regs[0] for exec.
  - argv (x1) and envp (x2) are NEVER touched — only the path register.
  - /proc, /sys, /dev are NOT rewritten (kernel virtual; keeps /proc/self/exe
    valid, L2118-2127).
  - rootfs-internal absolute paths are idempotency-guarded: never re-translated
    (already-host, L2128-2135).

This file is the executable spec of those rules; if the future (B-1) patch wires
the exec branch to the wrong register or starts touching argv/envp, this turns
red on the host before any device build.
"""
from __future__ import annotations

import pytest

# AArch64 syscall numbers (Linux generic / arm64 — stable ABI).
NR_OPENAT = 56
NR_NEWFSTATAT = 79
NR_FACCESSAT = 48
NR_EXECVE = 221
NR_EXECVEAT = 281

# The path-taking *at-style syscalls the interposer/supervisor mediates via x1.
AT_STYLE_PATH_NRS = (NR_OPENAT, NR_NEWFSTATAT, NR_FACCESSAT)
EXEC_NRS = (NR_EXECVE, NR_EXECVEAT)

KERNEL_VIRTUAL_PREFIXES = ("/proc", "/sys", "/dev")


# ---------------------------------------------------------------------------
# The decision model under test — a pure function mirroring the supervisor's
# register/path choice. (regs is the GETREGSET x[] array; regs[0]=x0 ... etc.)
# ---------------------------------------------------------------------------
def _under(path: str, d: str) -> bool:
    if not path.startswith(d):
        return False
    tail = path[len(d):]
    return tail == "" or tail.startswith("/")


def path_register_for(sysno: int) -> int:
    """Which register index holds the pathname for this syscall.

    ADR-003 §3 fix: exec uses x0 (regs[0]); *at-style path syscalls use x1.
    """
    if sysno in EXEC_NRS:
        return 0  # x0 — execve(path, argv, envp)
    return 1  # x1 — *at(dirfd, path, ...)


def should_rewrite_path(path: str, *, rootfs_dir: str) -> bool:
    """Whether the supervisor would translate this guest path into the rootfs.

    Mirrors L2135: rewrite iff absolute AND not a kernel-virtual dir AND not
    already a rootfs-host path (idempotency).
    """
    if not path or path[0] != "/":
        return False  # relative paths resolve vs guest cwd — left untouched
    if any(_under(path, d) for d in KERNEL_VIRTUAL_PREFIXES):
        return False  # /proc, /sys, /dev — not rewritten (keeps /proc/self/exe)
    if _under(path, rootfs_dir):
        return False  # already-host (idempotency guard)
    return True


def mediation_plan(sysno: int, regs: list[int], guest_paths: dict[int, str],
                   *, rootfs_dir: str) -> dict:
    """Decide which register (if any) gets rewritten for this trapped syscall.

    `guest_paths` maps a register's pointer value to the NUL-terminated string
    the supervisor would pread from /proc/<tid>/mem at that address. Returns a
    plan describing: the path register, the read path, whether to rewrite, and
    an invariant assertion that argv/envp registers are never the rewrite target.
    """
    preg = path_register_for(sysno)
    path_ptr = regs[preg]
    path = guest_paths.get(path_ptr, "")
    rewrite = should_rewrite_path(path, rootfs_dir=rootfs_dir)
    plan = {
        "path_register": preg,
        "path": path,
        "rewrite": rewrite,
        # For exec, x1/x2 are argv/envp and must NEVER be the rewrite target.
        "argv_register": 1 if sysno in EXEC_NRS else None,
        "envp_register": 2 if sysno in EXEC_NRS else None,
    }
    return plan


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("nr", AT_STYLE_PATH_NRS)
def test_at_style_reads_x1(nr):
    assert path_register_for(nr) == 1, "at-style path syscalls take the path in x1"


@pytest.mark.parametrize("nr", EXEC_NRS)
def test_exec_reads_x0_not_x1(nr):
    """The ADR-003 §3 correction: exec's path is x0, NOT x1 (x1 is argv)."""
    assert path_register_for(nr) == 0, (
        "execve/execveat path is x0 — reading x1 would corrupt argv (ADR-003 §3)"
    )


def test_exec_argv_envp_registers_are_never_the_path():
    """x1=argv, x2=envp for exec; the path register must differ from both."""
    for nr in EXEC_NRS:
        preg = path_register_for(nr)
        assert preg != 1, "x1 (argv) must never be the exec path register"
        assert preg != 2, "x2 (envp) must never be the exec path register"


def test_exec_plan_rewrites_x0_and_leaves_argv_envp():
    rootfs = "/data/rootfs"
    # x0 -> "/usr/lib/chromium/chrome" (rootfs-외 절대 → rewrite), x1=argv, x2=envp.
    regs = [0x1000, 0x2000, 0x3000] + [0] * 28
    guest_paths = {0x1000: "/usr/lib/chromium/chrome"}
    plan = mediation_plan(NR_EXECVE, regs, guest_paths, rootfs_dir=rootfs)
    assert plan["path_register"] == 0
    assert plan["path"] == "/usr/lib/chromium/chrome"
    assert plan["rewrite"] is True
    assert plan["argv_register"] == 1 and plan["envp_register"] == 2
    # The argv/envp pointers (0x2000/0x3000) are never read as a path.
    assert plan["path"] != guest_paths.get(0x2000, "")


def test_proc_self_exe_is_not_rewritten():
    """ADR-003 §3 / §4 가정-3: /proc/self/exe stays native (host image risk)."""
    rootfs = "/data/rootfs"
    assert should_rewrite_path("/proc/self/exe", rootfs_dir=rootfs) is False
    # And via the full exec plan: read from x0, but rewrite=False.
    regs = [0x1000, 0x2000, 0x3000] + [0] * 28
    plan = mediation_plan(NR_EXECVE, regs, {0x1000: "/proc/self/exe"}, rootfs_dir=rootfs)
    assert plan["path_register"] == 0
    assert plan["rewrite"] is False


@pytest.mark.parametrize("path", ["/proc/1/maps", "/sys/class/x", "/dev/null"])
def test_kernel_virtual_dirs_not_rewritten(path):
    assert should_rewrite_path(path, rootfs_dir="/data/rootfs") is False


def test_rootfs_internal_path_is_idempotent():
    """Already-host paths must not be re-translated (no <rootfs><rootfs>/…)."""
    rootfs = "/data/rootfs"
    assert should_rewrite_path("/data/rootfs/usr/lib/chromium/chrome",
                               rootfs_dir=rootfs) is False


def test_rootfs_external_absolute_is_rewritten():
    rootfs = "/data/rootfs"
    assert should_rewrite_path("/usr/lib/chromium/chrome", rootfs_dir=rootfs) is True
    assert should_rewrite_path("/bin/dash", rootfs_dir=rootfs) is True


def test_relative_path_not_rewritten():
    assert should_rewrite_path("chrome", rootfs_dir="/data/rootfs") is False
    assert should_rewrite_path("./helper", rootfs_dir="/data/rootfs") is False


def test_at_style_plan_reads_x1_path():
    """openat carries its path in x1; x0 (dirfd) must not be read as a path."""
    rootfs = "/data/rootfs"
    regs = [0xAA, 0x1000] + [0] * 29  # x0=dirfd(AT_FDCWD-ish), x1=path ptr
    plan = mediation_plan(NR_OPENAT, regs, {0x1000: "/etc/passwd"}, rootfs_dir=rootfs)
    assert plan["path_register"] == 1
    assert plan["path"] == "/etc/passwd"
    assert plan["rewrite"] is True
    # at-style has no argv/envp registers in this model.
    assert plan["argv_register"] is None and plan["envp_register"] is None
