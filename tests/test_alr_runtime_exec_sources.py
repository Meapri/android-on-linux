from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXEC_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_exec.hpp"
EXEC_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_exec.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_exec_test.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
LAUNCHER = ROOT / "app/src/main/cpp/alr_runtime_launcher.cpp"


def test_exec_resolver_api_is_declared_and_implemented():
    header = EXEC_HPP.read_text()
    source = EXEC_CPP.read_text()
    assert "enum class ExecutableKind" in header
    assert "struct ExecutableResolution" in header
    assert "enum class ExecContinuationKind" in header
    assert "struct ExecContinuationPlan" in header
    assert "resolve_guest_executable" in header
    assert "build_exec_continuation_plan" in header
    assert "executable_kind_name" in header
    assert "translate_rootfs_path" in source
    assert "ALR EXEC RESOLVE: " in source
    assert "ALR EXEC CLASSIFY: " in source
    assert "ALR EXEC STRATEGY: " in source
    assert "ALR EXEC CONTINUITY PLAN: " in source
    assert "ALR EXECVE CHILD CONTINUITY PLAN: " in source
    assert "ALR EXECVP PATH LOOKUP PLAN: " in source
    assert "ALR POSIX_SPAWN CHILD PLAN: " in source
    assert "shebang" in source
    assert "ELF" in source


def test_exec_resolver_is_built_and_host_tested():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    assert "alr_runtime/alr_exec.cpp" in cmake
    assert "native_alr_runtime_exec_test.cpp" in script
    assert "alr_runtime/alr_config.cpp" in script
    assert "alr_runtime/alr_exec.cpp" in script
    assert "alr-native-runtime-exec-test" in script
    assert "alr runtime exec native test ok" in NATIVE_TEST.read_text()


def test_runtime_and_launcher_reports_include_exec_resolution():
    plan = PLAN_CPP.read_text()
    launcher = LAUNCHER.read_text()
    assert "resolve_guest_executable" in plan
    assert "exec_resolution.report" in plan
    assert "resolve_guest_executable" in launcher
    assert "resolution.report" in launcher


def test_exec_continuity_plan_has_native_coverage():
    text = NATIVE_TEST.read_text()
    assert "ExecContinuationKind::Execve" in text
    assert "ExecContinuationKind::Execvp" in text
    assert "ExecContinuationKind::PosixSpawn" in text
    assert "ALR EXECVE CHILD CONTINUITY PLAN: PASS" in text
    assert "ALR EXECVP PATH LOOKUP PLAN: PASS" in text
    assert "ALR POSIX_SPAWN CHILD PLAN: PASS" in text
