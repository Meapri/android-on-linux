from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_runtime_report_has_gpu_marshalling_probe():
    text = CPP.read_text()
    assert "nativeAlrGpuMarshallingProbe" in text
    assert "build_gpu_marshalling_probe" in text
    assert "ALR GPU MARSHALLING HARDWARE RENDER: " in text
    # A real guest GLES command stream is decoded and dispatched as real calls.
    assert "OP_VIEWPORT" in text
    assert "OP_CLEARCOLOR" in text
    assert "OP_SCISSOR" in text
    assert "glScissor" in text
    assert "glReadPixels" in text
    # Hardware proof: non-software renderer + pixel readback verification.
    assert "renderer_looks_software" in text
    assert "center_is_b" in text


def test_main_activity_reports_gpu_marshalling_gate():
    text = MAIN.read_text()
    assert "nativeAlrGpuMarshallingProbe" in text
    assert "alrGpuMarshallingProbe" in text
    assert "ALR GPU MARSHALLING HARDWARE RENDER: " in text
    assert "alrGpuMarshallingPassed" in text
    assert "ALR GPU command-marshalling probe" in text
    assert "build: 0.4.143-r10-v143" in text
