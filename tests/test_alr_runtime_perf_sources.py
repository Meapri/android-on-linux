from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PERF_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_perf.hpp"
PERF_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_perf.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_perf_test.cpp"


def test_perf_api_is_declared_and_implemented():
    header = PERF_HPP.read_text()
    source = PERF_CPP.read_text()
    assert "struct PerfSample" in header
    assert "struct PerfComparison" in header
    assert "run_perf_comparison" in header
    assert "ALR PERF HARNESS: " in source
    # These two markers are emitted via a status_line() helper, so the source
    # holds the label without the trailing ": " that the helper appends.
    assert "ALR PERF ALR PATH XLATE MEASURED" in source
    assert "ALR PERF SYSCALL ROUNDTRIP MEASURED" in source


def test_perf_keeps_proot_comparison_pending_on_host():
    header = PERF_HPP.read_text()
    source = PERF_CPP.read_text()
    # The host harness must never fabricate a PRoot ptrace number or a verdict.
    assert "PENDING_DEVICE" in source
    assert "ALR PERF PROOT PTRACE DEVICE BASELINE: " in source
    assert "ALR PERF PROOT VS ALR DEVICE COMPARISON: " in source
    assert "proot_device_baseline_pending" in header
    assert "comparison_device_pending" in header
    # Honest interpretation: the path-translation cost is shared by both backends.
    assert "both PRoot and ALR pay the path-translation cost" in source
    assert "ptrace" in source


def test_perf_measures_real_alr_path_translation_and_syscall():
    source = PERF_CPP.read_text()
    assert "translate_rootfs_path" in source
    assert "::getppid()" in source
    assert "steady_clock" in source
    # Anti-elision sinks so the optimizer cannot delete the measured work.
    assert "volatile" in source


def test_perf_is_built_and_host_tested():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    native = NATIVE_TEST.read_text()
    assert "alr_runtime/alr_perf.cpp" in cmake
    assert "native_alr_runtime_perf_test.cpp" in script
    assert "app/src/main/cpp/alr_runtime/alr_perf.cpp" in script
    assert "alr-native-runtime-perf-test" in script
    assert "alr runtime perf native test ok" in native


def test_perf_native_test_rejects_fabricated_comparison():
    native = NATIVE_TEST.read_text()
    assert "proot baseline pending on host" in native
    assert "comparison pending on host" in native
