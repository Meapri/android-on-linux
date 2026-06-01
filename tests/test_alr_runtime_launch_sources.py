from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAUNCH_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_launch.hpp"
LAUNCH_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_launch.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_launch_test.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
LAUNCHER = ROOT / "app/src/main/cpp/alr_runtime_launcher.cpp"


def test_launch_attempt_api_is_declared_and_implemented():
    header = LAUNCH_HPP.read_text()
    source = LAUNCH_CPP.read_text()
    assert "struct LaunchAttemptPolicy" in header
    assert "struct LaunchAttemptResult" in header
    assert "attempt_guest_launch" in header
    assert "resolve_guest_executable" in source
    assert "build_exec_continuation_plan" in source
    assert "fork" in source
    assert "execve" in source
    assert "ALR LAUNCH ATTEMPT: " in source
    assert "ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: " in source
    assert "continuation.report" in source
    assert "direct rootfs host exec disabled by ALR policy" in source


def test_launch_attempt_is_built_and_host_tested():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    assert "alr_runtime/alr_launch.cpp" in cmake
    assert "native_alr_runtime_launch_test.cpp" in script
    assert "alr_runtime/alr_launch.cpp" in script
    assert "alr-native-runtime-launch-test" in script
    assert "allow_direct_host_exec = true" in NATIVE_TEST.read_text()
    assert "ALR EXECVE CHILD CONTINUITY PLAN: PASS" in NATIVE_TEST.read_text()
    assert "alr runtime launch native test ok" in NATIVE_TEST.read_text()


def test_runtime_and_launcher_reports_include_launch_attempt():
    plan = PLAN_CPP.read_text()
    launcher = LAUNCHER.read_text()
    assert "attempt_guest_launch" in plan
    assert "launch_attempt.report" in plan
    assert "attempt_guest_launch" in launcher
    assert "launch_attempt.report" in launcher
