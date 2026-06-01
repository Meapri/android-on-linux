from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INTERPOSER_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_interposer.hpp"
INTERPOSER_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_interposer.cpp"
INTERPOSER_SHARED = ROOT / "app/src/main/cpp/alr_runtime_interposer.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_interposer_test.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
REPORT_CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"


def test_interposer_scaffold_api_is_declared_and_implemented():
    header = INTERPOSER_HPP.read_text()
    source = INTERPOSER_CPP.read_text()
    assert "struct InterposerConfig" in header
    assert "struct InterposedPathResult" in header
    assert "run_interposer_path_smoke" in header
    assert "translate_rootfs_path" in source
    assert "::stat" in source
    assert "::open" in source
    assert "ALR INTERPOSER STAT PATH: " in source
    assert "ALR INTERPOSER OPEN PATH: " in source


def test_interposer_procfs_aware_resolution_is_declared_and_tested():
    header = INTERPOSER_HPP.read_text()
    source = INTERPOSER_CPP.read_text()
    native = NATIVE_TEST.read_text()
    assert "enum class InterposedKind" in header
    assert "resolve_interposed_access" in header
    # /proc/self/exe and /proc/<pid>/exe are served from the synthesized guest
    # view, never from the host /proc.
    assert "ProcSelfExe" in source
    assert "ProcMounts" in source
    assert "matches_proc_suffix" in source
    assert "render_guest_mounts" in source
    assert "ALR INTERPOSE KIND: " in source
    assert "ALR INTERPOSE VIRTUALIZED: " in source
    # Coverage: the guest exe target is the guest path, mounts hide the host
    # rootfs path, and a normal path still maps to a rootfs host path.
    assert "self exe target is guest path" in native
    assert "mounts hide host rootfs path" in native
    assert "etc host path translated" in native


def test_interposer_shared_library_exports_status_and_smoke_report():
    text = INTERPOSER_SHARED.read_text()
    assert "alr_runtime_interposer_status" in text
    assert "alr_runtime_interposer_smoke_report" in text
    assert "ALR INTERPOSER LOAD: PASS" in text
    assert "run_interposer_path_smoke" in text


def test_cmake_packages_interposer_library():
    text = CMAKE.read_text()
    assert "alr_runtime/alr_interposer.cpp" in text
    assert "add_library(alr_runtime_interposer SHARED" in text
    assert "alr_runtime_interposer.cpp" in text
    assert "target_compile_features(alr_runtime_interposer PRIVATE cxx_std_20)" in text
    assert "target_link_libraries(alr_runtime_interposer PRIVATE alr_runtime)" in text


def test_native_core_script_runs_interposer_smoke_test():
    text = NATIVE_SCRIPT.read_text()
    assert "native_alr_runtime_interposer_test.cpp" in text
    assert "alr_runtime/alr_interposer.cpp" in text
    assert "alr-native-runtime-interposer-test" in text
    assert "alr runtime interposer native test ok" in NATIVE_TEST.read_text()


def test_runtime_reports_include_interposer_status_and_dlopen_probe():
    plan = PLAN_CPP.read_text()
    report = REPORT_CPP.read_text()
    assert "ALR INTERPOSER LOAD: PASS" in plan
    assert "ALR INTERPOSER CONFIG BUILD: PASS" in plan
    assert "ALR_INTERPOSER_PATH" in plan
    assert "libalr_runtime_interposer.so" in report
