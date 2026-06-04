"""Source-invariant guards for the X11-only → Xwayland launch routing (TASK-A).

ALR's compositor is Wayland-only; an X11-only guest app (links libX11 but NOT
libwayland-client — xzgv/xli) cannot bind it and dies "cannot open display" unless it is
routed through a ROOTFUL Xwayland. This pass makes the launch path AUTO-ROUTE such apps via
a catalog flag (`CatalogApp.needsXwayland`) + a self-contained helper
(`NativeAppSession.XwaylandLaunch`) that replicates the device-proven MainActivity Xwayland
sequence (socket prep at sticky 1777, persistent `Xwayland :0 -shm`, X0-socket wait via
exists(), DISPLAY=:0 injection).

These are host structural guards (the Kotlin is not compiled here): they assert the wiring
is present and correctly shaped so a regression that drops the flag, the helper, or the
proven socket idioms is caught before a device run — mirroring
test_android_alr_xwayland_x11_fix.py (the MainActivity Xwayland guard) and
test_bundled_catalog_sources.py.
"""

from __future__ import annotations

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
MODELS = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/AppModels.kt"
SESSION = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/NativeAppSession.kt"
RUNTIME = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/NativeAlrRuntime.kt"


@pytest.fixture(scope="module")
def models_text() -> str:
    return MODELS.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def session_text() -> str:
    return SESSION.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def runtime_text() -> str:
    return RUNTIME.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# (1) the catalog flag
# --------------------------------------------------------------------------- #
def test_catalog_app_has_needs_xwayland_flag(models_text: str):
    # CatalogApp declares a boolean needsXwayland defaulting to false (so every existing
    # entry is byte-identical / Wayland-direct).
    assert "val needsXwayland: Boolean = false" in models_text


def test_x11_only_viewers_are_marked_needs_xwayland(runtime_text: str):
    # Every X11-only / Qt-xcb viewer must carry the flag: xzgv/xli (kept) + nsxiv/feh/qiv/xpdf
    # (X11) + qpdfview (Qt-xcb) re-added via the general maintscript-shim install unlock.
    for app_id in ("xzgv", "xli", "nsxiv", "feh", "qiv", "xpdf", "qpdfview"):
        i = runtime_text.index(f'appId = "{app_id}"')
        # the CatalogApp(...) for this app closes at the next "source = AppSource"
        j = runtime_text.index("source = AppSource", i)
        # needsXwayland = true must appear within this entry's body (after source line we
        # also allow it; scan a generous window covering the whole entry).
        entry = runtime_text[i:j + 200]
        assert "needsXwayland = true" in entry, f"{app_id} must be marked needsXwayland = true"


def test_wayland_apps_not_marked_needs_xwayland(runtime_text: str):
    # GTK3 Wayland apps (mate-calc/geany/galculator) must NOT carry needsXwayland=true —
    # they bind the compositor directly via the GDK Wayland backend.
    for app_id in ("mate-calc", "geany", "galculator"):
        i = runtime_text.index(f'appId = "{app_id}"')
        j = runtime_text.index("source = AppSource", i)
        entry = runtime_text[i:j + 60]
        assert "needsXwayland = true" not in entry, (
            f"{app_id} is Wayland-capable and must NOT be routed through Xwayland"
        )


def test_nsxiv_readded_via_general_shim(runtime_text: str):
    # nsxiv was previously DROPPED (X11-only but install HEAVY for x11-common/libpaper1). Once
    # the maintscript-shim is generalized to EVERY install, those postinsts are neutralized →
    # nsxiv is RE-ADDED as a needsXwayland=true entry. The source must document the unlock.
    assert 'appId = "nsxiv"' in runtime_text
    assert "x11-common" in runtime_text and "libpaper1" in runtime_text
    assert "maintscript-shim" in runtime_text


def test_catalog_exposes_xwayland_ssot(runtime_text: str):
    # BundledCatalog must expose the needsXwayland SSOT (derived from the entries) so the UI
    # layer can set protocol=X11 without duplicating the app-id list. This is what makes the
    # catalog flag the single edit needed to route a new X11 app.
    assert "val xwaylandAppIds" in runtime_text
    assert "fun needsXwayland(appId: String)" in runtime_text
    assert "apps.filter { it.needsXwayland }" in runtime_text


def test_ui_sets_x11_protocol_for_needs_xwayland_apps():
    # The launch-request builders must set protocol=X11 for needsXwayland apps so
    # XwaylandLaunch.needsX11 fires via its `protocol == SurfaceProtocol.X11` branch (the
    # routing works without editing the chromium-owned NativeAppSession X11_ONLY_APP_IDS set).
    alr_app = (ROOT / "app/src/main/java/dev/chanwoo/androlinux/ui/AlrApp.kt").read_text("utf-8")
    launcher_vm = (
        ROOT / "app/src/main/java/dev/chanwoo/androlinux/ui/viewmodel/LauncherViewModel.kt"
    ).read_text("utf-8")
    detail_vm = (
        ROOT / "app/src/main/java/dev/chanwoo/androlinux/ui/viewmodel/AppDetailViewModel.kt"
    ).read_text("utf-8")
    # CatalogApp uses its own field; InstalledApp paths look up the catalog SSOT by appId.
    assert "if (needsXwayland) SurfaceProtocol.X11" in alr_app
    assert "BundledCatalog.needsXwayland(appId)" in alr_app
    assert "BundledCatalog.needsXwayland(appId)" in launcher_vm
    assert "BundledCatalog.needsXwayland(app.appId)" in detail_vm


# --------------------------------------------------------------------------- #
# (2) the XwaylandLaunch helper — present, gated, in its own region
# --------------------------------------------------------------------------- #
def test_xwayland_launch_helper_exists(session_text: str):
    assert "internal object XwaylandLaunch" in session_text
    # the three-part API: needsX11 (gate) / ensureUp (start+wait) / envFor (DISPLAY).
    assert "fun needsX11(" in session_text
    assert "fun ensureUp(" in session_text
    assert "fun envFor(" in session_text


def test_routing_is_a_separate_region_from_the_env_flag_block(session_text: str):
    # The X11-routing CALL must live in its OWN region, NOT inside the ALR_REEXEC_INPROC
    # env-flag block (so a concurrent edit there merges cleanly). Assert the call exists and
    # that the helper is a sibling object to GnomePlatformShim (its own top-level region).
    assert "XwaylandLaunch.needsX11(appId, request.protocol, request.entryPath)" in session_text
    assert "XwaylandLaunch.ensureUp(" in session_text
    # the region banner marks it as separate
    assert "X11-only routing via ROOTFUL Xwayland (TASK-A)" in session_text
    # helper is defined AFTER GnomePlatformShim (separate region, end of file)
    assert session_text.index("internal object GnomePlatformShim") < session_text.index(
        "internal object XwaylandLaunch"
    )


def test_helper_gate_honors_both_catalog_flag_and_x11_protocol(session_text: str):
    # needsX11 must trigger on EITHER an explicit protocol==X11 OR the X11-only appId set.
    assert "protocol == SurfaceProtocol.X11" in session_text
    assert "appId in X11_ONLY_APP_IDS" in session_text
    # the X11-only app set must list xzgv + xli (lock-step with the catalog flag).
    i = session_text.index("X11_ONLY_APP_IDS")
    block = session_text[i:i + 400]
    assert '"xzgv"' in block and '"xli"' in block


# --------------------------------------------------------------------------- #
# (3) the proven Xwayland idioms are replicated (regression guard)
# --------------------------------------------------------------------------- #
def test_helper_preps_tmp_and_x11_unix_sticky_1777(session_text: str):
    # The X server requires /tmp + /tmp/.X11-unix at sticky 01777; Os.chmod (not File.set*)
    # because Java cannot set the sticky bit. Same idiom the MainActivity guard pins.
    assert '.X11-unix' in session_text
    assert "android.system.Os.chmod" in session_text
    assert "0x3FF" in session_text  # 01777
    assert "1777" in session_text


def test_helper_launches_rootful_xwayland_shm(session_text: str):
    # ROOTFUL Xwayland :0 with -shm (wl_shm backend) + -geometry, via the loader probe.
    assert "usr/bin/Xwayland" in session_text
    assert "-shm" in session_text
    assert "-geometry" in session_text
    assert "nativeAlrNativeLoaderProbe" in session_text
    # DISPLAY :0 is what gets injected into the guest env.
    assert '"DISPLAY" to' in session_text


def test_helper_socket_readiness_uses_exists_not_isfile(session_text: str):
    # A bound AF_UNIX socket is a special file → isFile() is false for it; the wait MUST use
    # exists() (same correctness point the MainActivity guard pins).
    assert "tmp/.X11-unix/X0" in session_text
    assert "xSock.exists()" in session_text


def test_xwayland_overlay_is_staged(session_text: str):
    # The session must stage the xwayland overlay (ships /usr/bin/Xwayland) best-effort, plus
    # the qt6-gui overlay (TASK-B). Both presence-guarded. Pin the DEFINITION (the `= listOf`),
    # not the `for name in OVERLAY_NAMES` use-site.
    i = session_text.index("private val OVERLAY_NAMES")
    block = session_text[i:i + 400]
    assert '"xwayland"' in block
    assert '"qt6-gui"' in block
