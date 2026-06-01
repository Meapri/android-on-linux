from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_runtime_report_has_syscall_capability_probe():
    text = CPP.read_text()
    assert "nativeAlrSyscallSandboxProbe" in text
    assert "build_syscall_capability_probe" in text
    assert "ALR SYSCALL SANDBOX PROBE: " in text
    # ENOSYS distinguishes a seccomp-blocked syscall from one that merely failed
    # on bogus arguments.
    assert "BLOCKED_ENOSYS" in text
    # The proot-relevant syscalls that Debian userland needs are probed.
    for sc in ("__NR_execveat", "__NR_symlink", "__NR_symlinkat", "__NR_mknodat"):
        assert sc in text


def test_main_activity_surfaces_syscall_sandbox_probe():
    text = MAIN.read_text()
    assert "nativeAlrSyscallSandboxProbe" in text
    assert "alrSyscallSandboxProbe" in text
    assert "ALR syscall sandbox capability probe:" in text
    assert "alr syscall sandbox blocked count=" in text
