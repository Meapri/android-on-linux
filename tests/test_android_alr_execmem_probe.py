from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"


def test_native_runtime_report_has_execmem_probe():
    text = CPP.read_text()
    assert "nativeAlrExecmemProbe" in text
    assert "build_execmem_probe" in text
    assert "ALR EXECMEM ANON RX EXECUTION: " in text
    # Anonymous RW page -> arm64 stub -> mprotect RX -> call, isolated in a child.
    assert "PROT_READ | PROT_WRITE" in text
    assert "PROT_READ | PROT_EXEC" in text
    assert "__builtin___clear_cache" in text
    assert "MAP_ANONYMOUS" in text


def test_main_activity_reports_execmem_gate():
    text = MAIN.read_text()
    assert "nativeAlrExecmemProbe" in text
    assert "alrExecmemProbe" in text
    assert "ALR EXECMEM ANON RX NATIVE EXEC: " in text
    assert "ALR execmem anon RX native exec probe:" in text
    assert "build: 0.4.136-r5-v136" in text


