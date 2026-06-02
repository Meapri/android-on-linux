from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_native_runtime_report_runs_procfs_interpose_mechanism():
    text = CPP.read_text()
    assert "nativeAlrInterposeProcfsProbe" in text
    assert "build_interposer_procfs_report" in text
    assert "resolve_interposed_access" in text
    assert "ALR INTERPOSE PROBE: android-native-mechanism" in text
    assert "ALR INTERPOSE SELF EXE VIRTUALIZED: " in text
    assert "ALR INTERPOSE MOUNTS NO HOST LEAK: " in text


def test_main_activity_reports_procfs_interpose_mechanism_gate():
    text = MAIN.read_text()
    assert "nativeAlrInterposeProcfsProbe" in text
    assert "alrInterposeProcfsProbe" in text
    assert 'lineStartingWith("ALR INTERPOSE SELF EXE VIRTUALIZED:")' in text
    assert "ALR PROCFS INTERPOSE MECHANISM: " in text
    assert "alr procfs interpose passed=" in text
    assert "ALR procfs interpose mechanism probe:" in text
    assert "build: 0.4.161-sd-v161" in text


def test_execution_backend_docs_require_procfs_interpose_mechanism():
    text = DOC.read_text()
    assert "ALR PROCFS INTERPOSE MECHANISM: PASS" in text
    assert "resolve_interposed_access" in text
    # Honesty: this is the in-process resolver, not yet real LD_PRELOAD hooks.
    assert "not yet real LD_PRELOAD hooks" in text
