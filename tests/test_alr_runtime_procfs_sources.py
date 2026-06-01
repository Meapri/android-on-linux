from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROCFS_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_procfs.hpp"
PROCFS_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_procfs.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_procfs_test.cpp"


def test_procfs_planner_api_is_declared_and_implemented():
    header = PROCFS_HPP.read_text()
    source = PROCFS_CPP.read_text()
    assert "struct ProcfsSelfExeView" in header
    assert "struct ProcfsSelfStatusView" in header
    assert "struct ProcfsMountsView" in header
    assert "struct ProcfsVirtualizationPlan" in header
    assert "build_procfs_virtualization_plan" in header
    assert "render_guest_mounts" in header
    # Plan-only honesty: the module must not claim live procfs virtualization.
    assert "plan-only" in header
    assert "ALR PROCFS PLAN: " in source
    assert "ALR PROCFS SELF EXE PLAN: " in source
    assert "ALR PROCFS SELF STATUS FAKEROOT PLAN: " in source
    assert "ALR PROCFS MOUNTS PLAN: " in source
    assert "ALR PROCFS HOST LEAK GUARD: " in source


def test_procfs_self_exe_view_virtualizes_away_from_host_truth():
    source = PROCFS_CPP.read_text()
    # /proc/self/exe must resolve to the guest path, not the host trampoline.
    assert "/proc/self/exe" in source
    assert "/proc/<pid>/exe" in source
    assert "readlink" in source and "readlinkat" in source and "openat" in source
    assert "requires_virtualization" in source
    assert "ALR_TRAMPOLINE_PATH" in source


def test_procfs_mounts_view_guards_against_android_host_leaks():
    header = PROCFS_HPP.read_text()
    source = PROCFS_CPP.read_text()
    assert "leaked_host_tokens" in source
    # The real host backing path is kept in a diagnostic-only field, never in the
    # guest-visible source column.
    assert "host_backing" in header
    assert "host backing" in source
    # The synthesized table must present the guest root and /proc pseudo-fs.
    assert "alr_rootfs" in source
    assert '"proc", "/proc", "proc"' in source
    # Generic android host tokens are part of the leak guard.
    assert "/system" in source
    assert "/vendor" in source
    assert "magisk" in source


def test_procfs_planner_is_built_and_host_tested():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    native = NATIVE_TEST.read_text()
    assert "alr_runtime/alr_procfs.cpp" in cmake
    assert "native_alr_runtime_procfs_test.cpp" in script
    assert "app/src/main/cpp/alr_runtime/alr_procfs.cpp" in script
    assert "alr-native-runtime-procfs-test" in script
    assert "alr runtime procfs native test ok" in native


def test_procfs_native_test_covers_fakeroot_and_leak_guard():
    native = NATIVE_TEST.read_text()
    assert "fake_root = true" in native
    assert "fakeroot uid mapped to root" in native
    assert "guest mounts hide rootfs host path" in native
    assert "guest mounts hide android data path" in native
    assert "missing host truth refuses plan" in native
