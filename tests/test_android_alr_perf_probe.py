from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_native_runtime_report_runs_perf_comparison():
    text = CPP.read_text()
    assert "nativeAlrPerfComparisonProbe" in text
    assert "build_perf_comparison_report" in text
    assert "run_perf_comparison" in text
    assert "ALR PERF PROBE: android-native-measure" in text


def test_main_activity_reports_android_perf_harness_and_pending_comparison():
    text = MAIN.read_text()
    assert "nativeAlrPerfComparisonProbe" in text
    assert "alrPerfComparisonProbe" in text
    assert 'lineStartingWith("ALR PERF HARNESS:") == "ALR PERF HARNESS: PASS"' in text
    assert "ALR PERF HARNESS: " in text
    assert "ALR PERF PROOT VS ALR DEVICE COMPARISON: " in text
    assert "alr perf harness ran=" in text
    assert "ALR PRoot-vs-ALR perf comparison probe:" in text
    assert "build: 0.4.149-cr1-v149" in text


def test_execution_backend_docs_require_perf_scaffolding_probe():
    text = DOC.read_text()
    assert "APK report must include `ALR PERF HARNESS: PASS`" in text
    assert "PENDING_DEVICE" in text
    assert "ptrace" in text
