"""Host tests for tools/build_xwayland_overlay.py (WS-4 §5 M4 — ROOTFUL Xwayland).

All OFFLINE — exercise the X-overlay recipe matrix, the overlay binary-presence
check (against synthetic tars), and the ROOTFUL launch argv. The network build
path (ports.ubuntu.com noble) is run by hand on the host and is the
integration/device gate.

The contract this WS-4 pass establishes (ALR had NO Xwayland — X11-only apps
could not run; only a SurfaceProtocol.X11 enum stub existed):
  * the `xwayland` package is staged as a §5-E overlay carrying /usr/bin/Xwayland
    plus the DT_NEEDED libs the base lacks (the base already provides the X CLIENT
    libs + xkb data, so only the X SERVER + its private deps are added);
  * Xwayland is launched ROOTFUL (the default — no -rootless, so NO X window
    manager is required) as a normal wl client → one X screen as a single
    wl_surface the compositor presents with no compositor change;
  * it is launched -shm (the ALR compositor is wl_shm-only) so glamor/DRI3/GBM/EGL
    stay inert — X apps are software-rendered for this first rung;
  * a simple X11 app (x11-apps: xcalc/xeyes/xlogo) runs with DISPLAY=:0;
  * overlay_has_exec proves the produced overlay actually contains the binary.
"""

import io
import tarfile
from pathlib import Path

from tools.build_xwayland_overlay import (
    X_OVERLAYS,
    XOverlay,
    XOverlayBuild,
    XWAYLAND_DISPLAY,
    XWAYLAND_ENV,
    overlay_has_exec,
    xwayland_launch_argv,
)


def test_two_overlays_defined():
    assert set(X_OVERLAYS) == {"xwayland", "x11app"}
    assert all(isinstance(ov, XOverlay) for ov in X_OVERLAYS.values())


def test_every_exec_path_is_rootfs_absolute():
    for name, ov in X_OVERLAYS.items():
        assert ov.exec_path.startswith("/"), name


def test_every_overlay_has_a_leaf_package():
    assert all(ov.leaf_packages for ov in X_OVERLAYS.values())


def test_xwayland_leaf_is_the_xwayland_package():
    xw = X_OVERLAYS["xwayland"]
    assert xw.leaf_packages == ("xwayland",)
    # the X SERVER binary (capital X) — the base ships only the X client libs.
    assert xw.exec_path == "/usr/bin/Xwayland"
    # display-free exec smoke for the loader (prints the version banner, exits).
    assert xw.launch_arg == "-version"


def test_x11app_leaf_is_x11_apps_with_a_simple_binary():
    app = X_OVERLAYS["x11app"]
    assert app.leaf_packages == ("x11-apps",)
    # a simple installable X11 app to prove the screen renders.
    assert app.exec_path == "/usr/bin/xcalc"


def test_rootful_launch_argv_is_correct():
    """The load-bearing invocation: ROOTFUL (default, no -rootless → no XWM needed),
    serves DISPLAY :0, forces the wl_shm backend, and sizes the single rootful X
    screen to the device panel via -geometry."""
    argv = xwayland_launch_argv(1200, 1920)
    assert argv[0] == "/usr/bin/Xwayland"
    assert argv[1] == ":0"
    assert ":0" == XWAYLAND_DISPLAY
    assert "-shm" in argv  # compositor is wl_shm-only → software backend
    assert "-geometry" in argv
    assert "1200x1920" in argv
    # ROOTFUL is the default: -rootless (which would REQUIRE an X window manager)
    # must NOT be present.
    assert "-rootless" not in argv


def test_rootful_geometry_tracks_panel_size():
    """-geometry must reflect whatever device panel size is passed (not hardcoded)."""
    argv = xwayland_launch_argv(800, 600)
    assert "800x600" in argv
    assert "1200x1920" not in argv


def test_no_glamor_env_pairs_with_shm():
    """Belt-and-braces with -shm: XWAYLAND_NO_GLAMOR=1 disables glamor/DRI3 entirely
    so Xwayland never tries EGL/GBM/DRI3 (the compositor offers none)."""
    assert XWAYLAND_ENV.get("XWAYLAND_NO_GLAMOR") == "1"


def test_overlay_has_exec_detects_present_and_absent(tmp_path: Path):
    tar_path = tmp_path / "t.tar"
    present = ["./usr/bin/Xwayland", "./usr/bin/xcalc", "./usr/bin/xeyes"]
    with tarfile.open(tar_path, "w") as t:
        for member in present:
            payload = b"\x7fELF" + member.encode()
            ti = tarfile.TarInfo(member)
            ti.size = len(payload)
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))

    assert overlay_has_exec(tar_path, "/usr/bin/Xwayland")
    assert overlay_has_exec(tar_path, "/usr/bin/xcalc")
    # leading-slash agnostic
    assert overlay_has_exec(tar_path, "usr/bin/Xwayland")
    # genuinely absent
    assert not overlay_has_exec(tar_path, "/usr/bin/Xorg")
    assert not overlay_has_exec(tar_path, "/usr/bin/does-not-exist")


def test_x_overlay_build_ok_aggregation():
    clean = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, ("liba",), (), (), True, has_exec=True)
    assert clean.ok
    # missing binary → not ok (the whole point: no "Xwayland missing")
    assert not XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), (), (), True, has_exec=False).ok
    # guard violation → not ok
    assert not XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), (), ("BLOCK foo",), True, has_exec=True).ok
    # unresolved DT_NEEDED soname → not ok
    assert not XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), ("libz.so.9",), (), True, has_exec=True).ok
    # non-conformant → not ok
    assert not XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), (), (), False, has_exec=True).ok


def test_builder_selftest_passes():
    from tools.build_xwayland_overlay import _selftest

    assert _selftest() == 0
