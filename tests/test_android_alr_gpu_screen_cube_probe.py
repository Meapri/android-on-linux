from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
SCREEN = ROOT / "app/src/main/cpp/alr_gpu/alr_gpu_screen.hpp"
SVC = ROOT / "app/src/main/cpp/alr_gpu/alr_gpu_host_service.hpp"


def test_native_runtime_report_has_screen_cube_probe():
    text = CPP.read_text()
    assert "nativeAlrGpuScreenCube" in text
    assert "alr::gpu::run_screen_cube_demo" in text
    assert '#include "alr_gpu/alr_gpu_screen.hpp"' in text
    # Presents to a real SurfaceView's window.
    assert "ANativeWindow_fromSurface" in text


def test_screen_header_present():
    # STEP B-1: a spinning textured cube driven through the live pipeline
    # (op stream -> ring -> executor -> AHB) and presented to an ANativeWindow
    # via external-OES from a SECOND EGL context (dodging the same-context hazard).
    text = SCREEN.read_text()
    assert "build_spinning_cube_stream" in text
    assert "run_screen_cube_demo" in text
    assert "ALR GPU SCREEN CUBE:" in text


def test_executor_service_takes_window():
    # GpuExecutorService gained an optional ANativeWindow for on-screen present;
    # nullptr preserves the pbuffer-only in-process probe behavior.
    text = SVC.read_text()
    assert "ANativeWindow" in text


def test_main_activity_invokes_screen_cube():
    text = MAIN.read_text()
    assert "nativeAlrGpuScreenCube" in text
    assert "cubeReport" in text
    assert "ALR GPU SCREEN CUBE" in text
    assert "build: 0.4.162-sd-v162" in text
