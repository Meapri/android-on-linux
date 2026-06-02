from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
HDR = ROOT / "app/src/main/cpp/alr_gpu/alr_gpu_host_service.hpp"


def test_native_runtime_report_has_gpu_live_probe():
    text = CPP.read_text()
    assert "nativeAlrGpuLiveProbe" in text
    assert "alr::gpu::run_live_integration_probe" in text
    assert '#include "alr_gpu/alr_gpu_host_service.hpp"' in text


def test_host_service_header_present():
    # The M4 live-integration backbone: a host executor thread owning the Mali
    # GLES2 context + AHB-FBO, fed by a guest over the SPSC ring, presenting per
    # frame via the req_seq/reply_seq handshake.
    text = HDR.read_text()
    assert "class GpuExecutorService" in text
    assert "run_live_integration_probe" in text
    assert "ALR GPU LIVE INTEGRATION:" in text


def test_main_activity_reports_gpu_live_gate():
    text = MAIN.read_text()
    assert "nativeAlrGpuLiveProbe" in text
    assert "alrGpuLiveProbe" in text
    assert "alrGpuLivePassed" in text
    assert "ALR GPU LIVE INTEGRATION:" in text
    assert "build: 0.4.136-r5-v136" in text
