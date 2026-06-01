from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_loader_links_android_egl_and_gles():
    text = CMAKE.read_text()
    assert "EGL" in text
    assert "GLESv2" in text


def test_host_gpu_probe_uses_android_egl_gles_not_cpu_renderer_claim():
    text = CPP.read_text()
    assert "#include <EGL/egl.h>" in text
    assert "#include <GLES2/gl2.h>" in text
    assert "eglCreatePbufferSurface" in text
    assert "eglCreateContext" in text
    assert "safe_gl_string(GL_RENDERER)" in text
    assert "SwiftShader".lower() in text.lower()
    assert "llvmpipe" in text
    assert "host gpu hardware candidate=" in text


def test_main_activity_reports_host_gpu_probe_before_verbose_report():
    text = MAIN.read_text()
    assert "nativeHostGpuProbe" in text
    assert "HOST GPU EGL/GLES EXECUTION:" in text
    assert "host gpu renderer=" in text
    assert "host gpu vendor=" in text
    assert "host gpu hardware candidate=" in text
    assert text.index("HOST GPU EGL/GLES EXECUTION:") < text.index("--- verbose report ---")
