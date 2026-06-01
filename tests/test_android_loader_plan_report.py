from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_jni_report_includes_loader_launch_plan():
    text = CPP.read_text()
    assert "build_loader_launch_plan" in text
    assert "loader argv:" in text
    assert "ALR_ROOTFS" in text
    assert "ALR_PROGRAM" in text


def test_jni_report_includes_absent_safe_low_overhead_probe_lines():
    text = PLAN_CPP.read_text()
    assert "LOW-OVERHEAD BACKEND PROBE FRAMEWORK: " in text
    assert "OPTIONAL RUNTIME BACKEND AVAILABLE: " in text
    assert "optional runtime backend source=" in text
    assert "SKIP" in text
    assert "none" in text


def test_jni_report_includes_alr_runtime_launcher_plan_section():
    text = CPP.read_text()
    plan = PLAN_CPP.read_text()
    assert "build_alr_runtime_launch_plan" in text
    assert "alr runtime argv:" in text
    assert "alr runtime env:" in text
    assert "ALR_HOOK_PATH" in text
    assert "ALR_INTERPOSER_PATH" in text
    assert "ALR_BRIDGE_PATH" in text
    assert "ALR_CONFIG_FORMAT" in text
    assert "ALR RUNTIME LAUNCHER AVAILABLE: PASS" in plan
    assert "ALR RUNTIME CONFIG BUILD: PASS" in plan
    assert "ALR CONFIG SERIALIZE: PASS" in plan
    exec_source = (ROOT / "app/src/main/cpp/alr_runtime/alr_exec.cpp").read_text()
    launch_source = (ROOT / "app/src/main/cpp/alr_runtime/alr_launch.cpp").read_text()
    assert "ALR EXEC RESOLVE: " in exec_source
    assert "ALR EXEC STRATEGY: " in exec_source
    assert "exec_resolution.report" in plan
    assert "ALR LAUNCH ATTEMPT: " in launch_source
    assert "ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: " in launch_source
    assert "ALR ELF LOAD PLAN: " in (ROOT / "app/src/main/cpp/alr_runtime/alr_elf.cpp").read_text()
    assert "ALR ELF STATIC HELLO CANDIDATE: " in (ROOT / "app/src/main/cpp/alr_runtime/alr_elf.cpp").read_text()
    trampoline_source = (ROOT / "app/src/main/cpp/alr_runtime/alr_trampoline.cpp").read_text()
    assert "ALR TRAMPOLINE AVAILABLE: " in trampoline_source
    assert "ALR TRAMPOLINE CONFIG HANDOFF: " in trampoline_source
    assert "ALR STATIC HELLO VIA TRAMPOLINE: " in trampoline_source
    assert "launch_attempt.report" in plan
    assert "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS" in plan


def test_main_activity_requests_tiny_rootfs_program():
    text = MAIN.read_text()
    assert '"/bin/hello"' in text
