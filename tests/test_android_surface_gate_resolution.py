from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_surface_gates_start_pending_in_summary():
    # The summary is built before the SurfaceView callback, so the surface gates
    # start as PENDING_SURFACE_CALLBACK in the source literal.
    text = MAIN.read_text()
    assert "HOST GPU SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" in text
    assert "ANDROID HOST VULKAN SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" in text


def test_surface_callback_resolves_pending_gates_from_render_results():
    text = MAIN.read_text()
    # surfaceCreated rewrites the PENDING gates using the real render results.
    assert "replaceFirst" in text
    assert "PENDING_SURFACE_CALLBACK" in text
    assert "after Surface callback" in text
    # Resolution is gated on hardware (non-software) renderer + zero dropped frames.
    assert 'intFieldAfter("surface frames dropped=")' in text
    assert 'contains("surface gpu software renderer=false")' in text
    assert 'intFieldAfter("vulkan surface frames rendered=")' in text
