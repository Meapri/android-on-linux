from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
HDR = ROOT / "app/src/main/cpp/alr_jit/alr_jit_probe.hpp"


def test_jit_wx_probe_header_present():
    # V8-style iterative W^X (RW<->RX) executable-memory cycle probe — decides
    # whether Chromium's V8/SwiftShader JIT needs --jitless on untrusted_app.
    text = HDR.read_text()
    assert "run_jit_wx_cycle_probe" in text
    assert "ALR JIT WX CYCLE:" in text
    assert "mprotect" in text


def test_native_runtime_report_has_jit_wx_probe():
    text = CPP.read_text()
    assert "nativeJitWxProbe" in text
    assert "alr::jit::run_jit_wx_cycle_probe" in text
    assert '#include "alr_jit/alr_jit_probe.hpp"' in text


def test_main_activity_reports_jit_wx_gate():
    text = MAIN.read_text()
    assert "nativeJitWxProbe" in text
    assert "jitWxProbe" in text
    assert "jitWxPassed" in text
    assert "ALR JIT WX CYCLE" in text
    assert "build: 0.4.144-r11-v144" in text
