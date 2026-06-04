"""Touch-calibrated guest DPI env (product UX BUG-1) — source + derivation guards.

ALR launches stock Linux GUI toolkits (GTK3/4, Qt6, X11-via-Xwayland) with NO scale env, so
on a touch device their default buttons/menus/fonts render finger-UNFRIENDLY (desktop ~96dpi,
1×). The fix matches the toolkits to Android's already-touch-calibrated `densityDpi`
(≈ densityDpi/160 effective scale) via a SINGLE pure derivation (`runtime/TouchDpiEnv.kt`),
wired into every launch path (NativeAppSession product path + MainActivity chromium/probe path).

The Kotlin is not compiled here, so these are HOST guards in the established source-assertion
style (mirroring test_x11_routing_sources.py / test_running_surface_activity_sources.py):

  (A) the derivation is correct AND double-scale-safe vs the compositor's wl_output buffer
      scale — re-implemented in Python from the SAME formula the Kotlin + the native compositor
      use, and the values pinned (incl. the device density 213).
  (B) the helper exists with a pure Int→Map API and the per-toolkit knobs / no-GDK_SCALE
      (no-double-scale) decision are present.
  (C) every launch path calls the helper (no hardcoded scale), and the X11 path seeds Xft.dpi
      into the X resource DB.
"""

from __future__ import annotations

import math
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/TouchDpiEnv.kt"
SESSION = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/NativeAppSession.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
COMPOSITOR = ROOT / "app/src/main/cpp/runtime_report.cpp"

BASELINE_DPI = 160
MIN_SANE_DPI = 100

# Device under test: Samsung SM-X236N, 1200×1920, reports densityDpi=213 (see
# docs/evidence/2026-06-01-ws5-cp1-display-verified.md "density=213").
DEVICE_DPI = 213


# --------------------------------------------------------------------------- #
# Python mirror of TouchDpiEnv (the SAME formula as the Kotlin + the compositor)
# --------------------------------------------------------------------------- #
def buffer_scale(dpi: int) -> int:
    """round(dpi/160), min 1 — MUST equal runtime_report.cpp `(density_dpi + 80) / 160`."""
    if dpi < MIN_SANE_DPI:
        return 1
    s = (dpi + BASELINE_DPI // 2) // BASELINE_DPI
    return 1 if s < 1 else s


def target_scale(dpi: int) -> float:
    if dpi < MIN_SANE_DPI:
        return 1.0
    return max(1.0, dpi / BASELINE_DPI)


def fmt(v: float) -> str:
    r = round(v * 100) / 100.0
    if r == math.floor(r):
        return str(int(r))
    return f"{r:.2f}".rstrip("0").rstrip(".")


def env_for(dpi: int) -> dict[str, str]:
    if dpi < MIN_SANE_DPI:
        return {}
    gdk = target_scale(dpi) / buffer_scale(dpi)
    return {
        "GDK_DPI_SCALE": fmt(gdk),
        "QT_FONT_DPI": str(dpi),
        "QT_AUTO_SCREEN_SCALE_FACTOR": "1",
        "Xft.dpi": str(dpi),
    }


@pytest.fixture(scope="module")
def helper_text() -> str:
    return HELPER.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def session_text() -> str:
    return SESSION.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def main_text() -> str:
    return MAIN.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# (A) derivation correctness + no-double-scale
# --------------------------------------------------------------------------- #
def test_buffer_scale_matches_native_compositor_formula():
    # The Kotlin bufferScale MUST mirror the native compositor's integer wl_output scale
    # `(density_dpi + 80) / 160` (runtime_report.cpp), else GDK_DPI_SCALE would double-scale.
    comp = COMPOSITOR.read_text(encoding="utf-8")
    assert "(density_dpi + 80) / 160" in comp  # the native round(dpi/160)
    # Spot-check equality across the density range.
    for dpi in (160, 213, 240, 320, 440):
        assert buffer_scale(dpi) == (dpi + 80) // 160


def test_net_scale_reconstructs_target_density():
    # Wayland GTK applies bufferScale (integer, from the compositor) × GDK_DPI_SCALE (the env
    # residual). Their product MUST reconstruct the target density scale (no double/under-scale
    # beyond 2-decimal rounding), which is the whole correctness argument for NOT setting
    # GDK_SCALE.
    for dpi in (160, 213, 240, 320):
        net = buffer_scale(dpi) * float(env_for(dpi)["GDK_DPI_SCALE"])
        assert abs(net - target_scale(dpi)) <= 0.01, f"dpi={dpi} net={net}"


def test_device_density_213_env_values():
    # The shipped device (densityDpi=213): bufferScale=1 (so the compositor advertises scale 1,
    # NO HiDPI buffer) → the full 1.33× must come from the env, with NO double-scale.
    assert buffer_scale(DEVICE_DPI) == 1
    env = env_for(DEVICE_DPI)
    assert env["GDK_DPI_SCALE"] == "1.33"
    assert env["QT_FONT_DPI"] == "213"
    assert env["QT_AUTO_SCREEN_SCALE_FACTOR"] == "1"
    assert env["Xft.dpi"] == "213"


def test_low_or_zero_density_yields_no_scaling():
    # A desktop-DPI / nonsensical density must be a no-op (empty env) so a normal-DPI panel is
    # byte-identical to before.
    for dpi in (0, 96, 99):
        assert env_for(dpi) == {}


def test_320_dpi_is_pure_integer_scale():
    # 320dpi (xhdpi): bufferScale=2, residual=1.0 → GDK_DPI_SCALE="1" (trimmed), QT/Xft=320.
    env = env_for(320)
    assert env["GDK_DPI_SCALE"] == "1"
    assert env["QT_FONT_DPI"] == "320" and env["Xft.dpi"] == "320"


# --------------------------------------------------------------------------- #
# (B) the helper exists, pure API, per-toolkit knobs, no-double-scale decision
# --------------------------------------------------------------------------- #
def test_helper_exists_with_pure_api(helper_text: str):
    assert "object TouchDpiEnv" in helper_text
    assert "fun envFor(densityDpi: Int): Map<String, String>" in helper_text
    assert "fun bufferScale(densityDpi: Int): Int" in helper_text
    assert "fun targetScale(densityDpi: Int): Double" in helper_text


def test_helper_sets_each_toolkit_knob(helper_text: str):
    # GTK fractional residual, Qt font DPI + auto screen scale, X11 font DPI.
    assert '"GDK_DPI_SCALE"' in helper_text
    assert '"QT_FONT_DPI"' in helper_text
    assert '"QT_AUTO_SCREEN_SCALE_FACTOR"' in helper_text
    assert '"Xft.dpi"' in helper_text


def test_helper_does_not_set_gdk_scale_to_avoid_double_scale(helper_text: str):
    # GDK_SCALE (integer device-pixel multiplier) MUST NOT be set: the compositor's wl_output
    # buffer scale already supplies the integer factor on Wayland; setting GDK_SCALE would
    # stack on top and double-scale. The doc must state the decision; the env must not set it.
    assert '"GDK_SCALE"' not in helper_text
    assert "double-scale" in helper_text.lower() or "double scale" in helper_text.lower()


def test_helper_mirrors_compositor_buffer_scale_in_docs(helper_text: str):
    # The residual math depends on mirroring the compositor's (density+80)/160 round.
    assert "(density_dpi + 80) / 160" in helper_text or "round(dpi/160)" in helper_text


# --------------------------------------------------------------------------- #
# (C) every launch path calls the helper (no hardcoded scale) + X11 xrdb seed
# --------------------------------------------------------------------------- #
def test_product_path_applies_touch_dpi_before_request_env(session_text: str):
    # NativeAppSession applies the derived env, and BEFORE request.env so a per-app launch can
    # still override a single knob.
    assert "TouchDpiEnv.envFor(dm.densityDpi)" in session_text
    i = session_text.index("TouchDpiEnv.envFor(dm.densityDpi)")
    j = session_text.index("for ((k, v) in request.env) setEnv(k, v)")
    assert i < j, "touch-dpi env must be set before request.env (so request can override)"


def test_chromium_and_probe_paths_apply_touch_dpi(main_text: str):
    # Both MainActivity launch paths (chromium standalone + the probe GUI battery) use the SAME
    # single derivation — assert it appears at least twice (no hardcoded scale duplicated).
    assert main_text.count(
        "dev.chanwoo.androlinux.runtime.TouchDpiEnv.envFor(dm.densityDpi)"
    ) >= 2


def test_no_hardcoded_gtk_scale_env_in_launch_paths(session_text: str, main_text: str):
    # The scale must come from the helper, never a literal GDK_SCALE/GDK_DPI_SCALE assignment in
    # the launch code (which would defeat the device-adaptive derivation / risk double-scale).
    for txt in (session_text, main_text):
        assert 'setenv("GDK_SCALE"' not in txt
        assert 'setEnv("GDK_SCALE"' not in txt
        assert 'setenv("GDK_DPI_SCALE"' not in txt
        assert 'setEnv("GDK_DPI_SCALE"' not in txt


def test_x11_path_seeds_xft_dpi_into_resource_db(session_text: str):
    # X11/Xwayland clients that read xrdb (not the env) get the touch DPI via a seeded
    # /root/.Xresources `Xft.dpi: <density>`. XwaylandLaunch.ensureUp must take the density and
    # seed it.
    assert "fun seedXftDpi(" in session_text
    assert "Xft.dpi:" in session_text
    assert "root/.Xresources" in session_text
    # ensureUp threads densityDpi through, and the call site passes dm.densityDpi.
    assert "densityDpi: Int" in session_text
    assert "rootfsName, outW, outH, dm.densityDpi" in session_text
