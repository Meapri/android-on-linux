from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "app/src/main/cpp/alr_runtime_launcher.cpp"
PLAN_HPP = ROOT / "app/src/main/cpp/runtime_plan.hpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
SYNC = ROOT / "docs/agent-sync.md"


def test_alr_runtime_launcher_exports_absent_safe_status_api():
    text = LAUNCHER.read_text()
    assert "alr_runtime_launcher_status" in text
    assert "alr_runtime_launcher_can_execute_guest" in text
    assert "alr_runtime_launcher_policy" in text
    assert "ALR RUNTIME LAUNCHER AVAILABLE: PASS" in text
    assert "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS" in text
    assert "can execute guest=no" in text


def test_alr_runtime_launch_plan_is_declared_and_implemented():
    header = PLAN_HPP.read_text()
    source = PLAN_CPP.read_text()
    assert "AlrRuntime" in header
    assert "build_alr_runtime_launch_plan" in header
    assert "libalr_runtime_launcher.so" in source
    assert "libalr_runtime_hook.so" in source
    assert "libalr_runtime_interposer.so" in source
    assert "libalr_runtime_bridge.so" in source
    assert "ALR HOOK LOAD: PASS" in source
    assert "ALR INTERPOSER LOAD: PASS" in source
    assert '"--dry-run"' in source
    assert "guest execution is not implemented yet" in source


def test_agent_sync_records_bundle_c_start():
    text = SYNC.read_text()
    assert "Bundle C Start" in text
    assert "codex/alr-runtime-launcher" in text
