from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_native_runtime_report_builds_procfs_virtualization_plan():
    text = CPP.read_text()
    assert "nativeAlrProcfsVirtualizationProbe" in text
    assert "build_procfs_virtualization_report" in text
    assert "build_procfs_virtualization_plan" in text
    assert "ALR PROCFS VIRTUALIZATION PROBE: android-native-plan" in text
    # The host truth fed to the planner is the packaged trampoline path, and the
    # identity is the process's own uid (never scraped from host /proc).
    assert 'launch.env.at("ALR_TRAMPOLINE_PATH")' in text
    assert "::getuid()" in text


def test_main_activity_reports_android_procfs_virtualization_gate():
    text = MAIN.read_text()
    assert "nativeAlrProcfsVirtualizationProbe" in text
    assert "alrProcfsVirtualizationProbe" in text
    assert 'lineStartingWith("ALR PROCFS PLAN:") == "ALR PROCFS PLAN: PASS"' in text
    assert "ALR PROCFS VIRTUALIZATION PLAN: " in text
    assert "alr procfs virtualization passed=" in text
    assert "ALR procfs virtualization plan probe:" in text
    assert "build: 0.4.162-sd-v162" in text


def test_execution_backend_docs_require_android_procfs_virtualization_probe():
    text = DOC.read_text()
    assert "APK report must include `ALR PROCFS VIRTUALIZATION PLAN: PASS`" in text
    assert "/proc/self/exe" in text
    assert "/proc/self/status" in text
    assert "/proc/mounts" in text
    # Honesty guardrail: planning only, not device-proven.
    assert "plan-only" in text
