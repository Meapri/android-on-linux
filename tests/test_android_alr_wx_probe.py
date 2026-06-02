from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_native_runtime_report_builds_wx_safe_exec_strategy():
    text = CPP.read_text()
    assert "nativeAlrWxSafeExecProbe" in text
    assert "build_wx_safe_exec_report" in text
    assert "build_wx_safe_exec_strategy" in text
    assert "ALR WX-SAFE EXEC PROBE: android-native-plan" in text


def test_main_activity_reports_android_wx_safe_exec_gate():
    text = MAIN.read_text()
    assert "nativeAlrWxSafeExecProbe" in text
    assert "alrWxSafeExecProbe" in text
    assert 'lineStartingWith("ALR WX-SAFE EXEC STRATEGY:") == "ALR WX-SAFE EXEC STRATEGY: PASS"' in text
    assert "ALR WX-SAFE EXEC STRATEGY: " in text
    assert "alr wx-safe exec passed=" in text
    assert "ALR W^X-safe exec strategy probe:" in text
    assert "build: 0.4.143-r10-v143" in text


def test_execution_backend_docs_require_wx_safe_exec_strategy_probe():
    text = DOC.read_text()
    assert "APK report must include `ALR WX-SAFE EXEC STRATEGY: PASS`" in text
    assert "memfd-execveat" in text
    assert "anon-mmap-loader" in text
    assert "proot-baseline" in text
    # Honesty guardrail: planning only, SELinux still needs device proof.
    assert "plan-only" in text
