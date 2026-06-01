from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAN = ROOT / "docs/plans/v51-v60-gui-gles-expansion.md"


def _plan_text() -> str:
    return PLAN.read_text()


def test_v51_v60_gui_gles_expansion_plan_exists_and_is_planning_only():
    text = _plan_text()
    assert "# V51–V60 GUI/GLES Expansion Plan" in text
    assert "planning/test guardrail document only" in text
    assert "must not require changes to the current V40 runtime code path" in text
    assert "V51–V55: Wayland/X11 Proxy Shape" in text
    assert "V56–V60: EGL/GLES Shim Expansion" in text


def test_gui_proxy_acceptance_lines_and_guardrails_are_explicit():
    text = _plan_text()
    required_lines = [
        "WAYLAND DISPLAY SOCKET AVAILABLE: PASS",
        "X11 DISPLAY SOCKET AVAILABLE: PASS",
        "GUI PROXY ENDPOINTS ARE MINIMAL SHAPE ONLY: PASS",
        "NOT FULL COMPOSITOR/SERVER COMPATIBILITY YET: PASS",
        "WAYLAND PROXY MINIMAL MESSAGE SHAPE: PASS",
        "WAYLAND FRAME COMMIT TRANSLATED TO HOST SURFACE COMMAND: PASS",
        "X11 PROXY MINIMAL MESSAGE SHAPE: PASS",
        "X11 DRAWABLE UPDATE TRANSLATED TO HOST SURFACE COMMAND: PASS",
        "GUEST GUI APP FRAME COMMIT EXECUTION: PASS",
        "ANDROID GPU SURFACE COMPOSITOR EXECUTION: PASS",
        "ANDROID SURFACE/EGL HARDWARE RENDERER: PASS",
        "V55 GUI PROXY SHAPE APK BUILT: PASS",
        "V40 GUI GPU PROOF REGRESSION: PASS",
    ]
    for line in required_lines:
        assert line in text

    guardrails = [
        "This is not full compositor/server compatibility yet.",
        "must not claim full Wayland compositor compatibility",
        "must not claim full X11 server compatibility",
        "Android Surface/EGL hardware proof is required",
        "Device logs remain the source of truth",
    ]
    for guardrail in guardrails:
        assert guardrail in text


def test_gles_shim_subset_and_acceptance_lines_are_explicit():
    text = _plan_text()
    required_subset = [
        "`eglGetDisplay`",
        "`eglInitialize`",
        "`eglChooseConfig`",
        "`eglCreateContext`",
        "`eglMakeCurrent`",
        "`glViewport`",
        "`glClearColor`",
        "`glClear`",
        "`eglSwapBuffers`",
        "`eglDestroyContext`",
        "`eglTerminate`",
    ]
    for symbol in required_subset:
        assert symbol in text

    required_lines = [
        "GUEST EGL/GLES SHIM LIBRARIES VISIBLE: PASS",
        "GLES SHIM REQUIRED SUBSET LISTED: PASS",
        "GLES SHIM SCOPE IS CONSTRAINED: PASS",
        "GUEST EGL INIT VIA SHIM: PASS",
        "GUEST EGL CONTEXT VIA SHIM: PASS",
        "GUEST GLES VIEWPORT VIA SHIM: PASS",
        "GUEST GLES CLEAR VIA SHIM: PASS",
        "GUEST GLES COMMAND SEQUENCE LOSSLESS: PASS",
        "GUEST EGL SWAP VIA ANDROID SURFACE: PASS",
        "GUEST GLES HARDWARE RENDER: PASS",
        "SOFTWARE GLES RENDERER REJECTED: PASS",
        "V60 GLES SHIM EXPANSION APK BUILT: PASS",
        "GLES SHIM SUBSET DEVICE EVIDENCE COLLECTED: PASS",
        "NOT FULL EGL/GLES COMPATIBILITY YET: PASS",
    ]
    for line in required_lines:
        assert line in text


def test_hardware_evidence_block_prevents_pytest_only_acceleration_claims():
    text = _plan_text()
    evidence_lines = [
        "android surface available=true",
        "android egl initialized=true",
        "android gl renderer=<non-software renderer string>",
        "software renderer rejected=true",
        "surface frames submitted=<n>",
        "surface frames rendered=<n>",
        "surface frames dropped=0",
    ]
    for line in evidence_lines:
        assert line in text

    assert "host pytest result alone is not acceleration proof" in text
    assert "Do not mark V51–V60 complete unless Android Surface/EGL hardware proof is present" in text
