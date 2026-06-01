from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_native_runtime_report_attempts_memfd_execveat():
    text = CPP.read_text()
    assert "nativeAlrMemfdExecProbe" in text
    assert "build_memfd_exec_probe" in text
    # Real W^X-safe native exec: anonymous memfd + execveat(AT_EMPTY_PATH).
    assert "__NR_memfd_create" in text
    assert "__NR_execveat" in text
    assert "ALR MEMFD EXECVEAT W^X-SAFE EXECUTION: " in text
    # MFD_EXEC fallback for Linux 6.3+ NOEXEC-by-default memfds.
    assert "MFD_EXEC" in text
    # Pass is only declared when the static guest actually printed its output.
    assert "hello from static arm64 rootfs" in text


def test_main_activity_reports_memfd_native_exec_gate():
    text = MAIN.read_text()
    assert "nativeAlrMemfdExecProbe" in text
    assert "alrMemfdExecProbe" in text
    assert 'lineStartingWith("ALR MEMFD EXECVEAT W^X-SAFE EXECUTION:")' in text
    assert "ALR MEMFD W^X-SAFE NATIVE EXEC: " in text
    assert "ALR memfd W^X-safe native exec probe:" in text
    assert "build: 0.4.120-chromium-jitwx-probe-v120" in text


def test_execution_backend_docs_describe_memfd_attempt():
    text = DOC.read_text()
    assert "ALR MEMFD EXECVEAT W^X-SAFE EXECUTION" in text
    # Honesty: it is an on-device attempt; SELinux may still block it.
    assert "SELinux" in text
