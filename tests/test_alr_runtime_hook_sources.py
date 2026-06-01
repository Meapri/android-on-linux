from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOOK_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_hook.hpp"
HOOK_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_hook.cpp"
HOOK_SHARED = ROOT / "app/src/main/cpp/alr_runtime_hook.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_hook_test.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
REPORT_CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"


def test_alr_hook_smoke_api_is_declared_and_implemented():
    header = HOOK_HPP.read_text()
    source = HOOK_CPP.read_text()
    assert "struct PathHookSmokeResult" in header
    assert "run_path_hook_smoke" in header
    assert "translate_rootfs_path" in source
    assert "::stat" in source
    assert "::open" in source
    assert "ALR STAT ROOTFS FILE: " in source
    assert "ALR OPEN ROOTFS FILE: " in source


def test_alr_hook_shared_library_exports_status_and_smoke_report():
    text = HOOK_SHARED.read_text()
    assert "alr_runtime_hook_status" in text
    assert "alr_runtime_hook_smoke_report" in text
    assert "ALR HOOK LOAD: PASS" in text
    assert "run_path_hook_smoke" in text


def test_cmake_packages_alr_runtime_hook_library():
    text = CMAKE.read_text()
    assert "alr_runtime/alr_hook.cpp" in text
    assert "add_library(alr_runtime_hook SHARED" in text
    assert "alr_runtime_hook.cpp" in text
    assert "target_compile_features(alr_runtime_hook PRIVATE cxx_std_20)" in text
    assert "target_link_libraries(alr_runtime_hook PRIVATE alr_runtime)" in text


def test_native_core_script_runs_alr_hook_smoke_test():
    text = NATIVE_SCRIPT.read_text()
    assert "native_alr_runtime_hook_test.cpp" in text
    assert "alr_runtime/alr_hook.cpp" in text
    assert "alr-native-runtime-hook-test" in text
    assert "alr runtime hook native test ok" in NATIVE_TEST.read_text()


def test_runtime_reports_include_hook_status_and_dlopen_probe():
    plan = PLAN_CPP.read_text()
    report = REPORT_CPP.read_text()
    assert "ALR HOOK LOAD: PASS" in plan
    assert "ALR HOOK CONFIG BUILD: PASS" in plan
    assert "libalr_runtime_hook.so" in report
