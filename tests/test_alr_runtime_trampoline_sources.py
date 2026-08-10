from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
GRADLE = ROOT / "app/build.gradle.kts"
LAUNCH_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_launch.cpp"
TRAMPOLINE_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_trampoline.hpp"
TRAMPOLINE_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_trampoline.cpp"
TRAMPOLINE_MAIN = ROOT / "app/src/main/cpp/alr_runtime_trampoline.cpp"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_trampoline_test.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"


def test_packaged_trampoline_is_declared_and_packaged():
    cmake = CMAKE.read_text()
    gradle = GRADLE.read_text()
    assert "alr_runtime/alr_trampoline.cpp" in cmake
    assert "add_executable(alr_runtime_trampoline" in cmake
    assert 'OUTPUT_NAME "alr-runtime-trampoline"' in cmake
    assert "target_link_libraries(alr_runtime_trampoline PRIVATE alr_runtime)" in cmake
    assert "libalr_runtime_trampoline.so" in gradle


def test_trampoline_report_contract_exists():
    header = TRAMPOLINE_HPP.read_text()
    source = TRAMPOLINE_CPP.read_text()
    main = TRAMPOLINE_MAIN.read_text()
    for line in [
        "ALR TRAMPOLINE AVAILABLE: ",
        "ALR TRAMPOLINE CONFIG HANDOFF: ",
        "ALR TRAMPOLINE POLICY PREFLIGHT: ",
        "ALR STATIC HELLO VIA TRAMPOLINE: ",
        "alr trampoline path=",
        "alr trampoline exit=",
    ]:
        assert line in source
    assert "TrampolineAttemptPolicy" in header
    assert "ALR TRAMPOLINE PREFLIGHT:" in main
    assert "--continue-exec" in main
    assert "ALR TRAMPOLINE CONTINUE EXEC: " in main
    assert "ALR TRAMPOLINE CONTINUE CONFIG: " in main
    assert "ALR TRAMPOLINE CONTINUE CHECKSUM: " in main
    assert "parse_runtime_config" in main
    assert "ALR_TRAMPOLINE_TARGET_HOST_PATH" in source


def test_launch_report_and_runtime_plan_include_trampoline():
    launch = LAUNCH_CPP.read_text()
    plan = PLAN_CPP.read_text()
    assert "attempt_packaged_trampoline" in launch
    assert "trampoline.report" in launch
    assert "ALR_TRAMPOLINE_PATH" in plan
    assert "alr runtime trampoline path=" in plan


def test_trampoline_has_native_coverage():
    script = NATIVE_SCRIPT.read_text()
    test = NATIVE_TEST.read_text()
    assert "native_alr_runtime_trampoline_test.cpp" in script
    assert "native_alr_runtime_trampoline_continue_test.cpp" in script
    assert "alr_runtime_trampoline.cpp" in script
    assert "alr_runtime/alr_trampoline.cpp" in script
    assert "alr-native-runtime-trampoline-test" in script
    assert "alr-native-runtime-trampoline-continue-test" in script
    assert "ALR STATIC HELLO VIA TRAMPOLINE: SKIP" in test
    continue_test = (ROOT / "tests/native_alr_runtime_trampoline_continue_test.cpp").read_text()
    assert "ALR TRAMPOLINE CONTINUE EXEC: PASS" in continue_test
    assert "alr runtime trampoline continue native test ok" in continue_test
