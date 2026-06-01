from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_surface_renderer_links_android_window_and_egl():
    assert "android" in CMAKE.read_text()
    text = CPP.read_text()
    assert "#include <android/native_window_jni.h>" in text
    assert "ANativeWindow_fromSurface" in text
    assert "eglCreateWindowSurface" in text
    assert "eglSwapBuffers" in text
    assert "surface gpu hardware render=" in text
    assert "SwiftShader".lower() in text.lower()


def test_main_activity_owns_surface_view_and_appends_native_surface_report():
    text = MAIN.read_text()
    assert "SurfaceView" in text
    assert "SurfaceHolder.Callback" in text
    assert "nativeRenderGpuSurface" in text
    assert "HOST GPU SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" in text
    assert "Linux guest Wayland/X11 GUI GPU surface renderer" in text
