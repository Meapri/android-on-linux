from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_native_runtime_report_executes_packaged_trampoline_continue_probe():
    text = CPP.read_text()
    assert "nativeAlrTrampolineContinueProbe" in text
    assert "run_packaged_trampoline_continue_probe" in text
    assert "build_exec_continuation_plan" in text
    assert "ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: " in text
    assert "ALR TRAMPOLINE CONTINUE EXEC: PASS" in text
    assert "::execve(argv_storage.front().c_str()" in text
    assert "continuation.env_overrides" in text


def test_main_activity_reports_android_packaged_trampoline_continue_gate():
    text = MAIN.read_text()
    assert "nativeAlrTrampolineContinueProbe" in text
    assert "ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION:" in text
    assert "alr packaged trampoline continue passed=" in text
    assert "ALR packaged trampoline continue probe:" in text
    assert "build: 0.4.114-android-gimp-ahb-present-v114" in text


def test_execution_backend_docs_require_android_packaged_trampoline_probe():
    text = DOC.read_text()
    assert "packaged trampoline has a `--continue-exec` dry-run mode" in text
    assert "APK report must include `ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: PASS`" in text
