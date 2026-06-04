"""Host tests for tools/build_toolkit_overlays.py (WS-4 §10b/§11 launchable toolkits).

All OFFLINE — exercise the toolkit recipe matrix + the overlay binary-presence
check against synthetic tars (no network / no real .deb). The network build path
(ports.ubuntu.com noble) is run by hand on the host and is the integration/device
gate.

The contract this WS-4 pass establishes (the toolkit-launch probe was reporting
qt6/sdl2 "missing" because their overlays carried libs but no runnable binary):
  * each toolkit's leaf set includes a package that ships a display-free runnable
    binary — netsurf-gtk, qt6-base-dev-tools (qtpaths6), libsdl2-tests (testver);
  * the reported exec_path is rootfs-absolute and is the path the integration
    session points MainActivity's toolkit probe at;
  * overlay_has_exec proves the produced overlay actually contains that binary.
"""

import io
import tarfile
from pathlib import Path

from tools.build_toolkit_overlays import (
    TOOLKITS,
    Toolkit,
    ToolkitBuild,
    overlay_has_exec,
)


def test_toolkit_matrix_defined():
    # netsurf/qt6/sdl2 are the display-free CLI smokes; qt6-gui (TASK-B) is the windowed
    # Qt6 Quick-on-Wayland app-class (the lightest REACHABLE Qt GUI — apt Qt-GUI is closure-
    # blocked by libqt6gui6→libice6→x11-common, so it ships as an overlay, not BundledCatalog).
    assert set(TOOLKITS) == {"netsurf", "qt6", "sdl2", "qt6-gui"}
    assert all(isinstance(tk, Toolkit) for tk in TOOLKITS.values())


def test_qt6_gui_is_a_windowed_qt_quick_app():
    """qt6-gui ships the real qmleasing Qt6 Quick GUI binary + the qtwayland platform
    plugin, forcing the generic wl_shm QPA (EGL/dmabuf integration excluded)."""
    qg = TOOLKITS["qt6-gui"]
    assert qg.gui is True
    assert "qt6-declarative-dev-tools" in qg.leaf_packages  # ships qmleasing
    assert qg.exec_path == "/usr/lib/qt6/bin/qmleasing"
    assert "qt6-wayland" in qg.leaf_packages                # the platform plugin
    assert "qml6-module-qtquick" in qg.leaf_packages        # QtQuick QML module (dlopen'd)
    # wl_shm-only: the EGL/dmabuf wayland integration must be excluded so Qt picks generic SHM.
    assert "wayland-egl" in qg.exclude_leaf_substrings
    assert "dmabuf" in qg.exclude_leaf_substrings
    # the other three are display-free CLI smokes (gui defaults False)
    assert all(not TOOLKITS[n].gui for n in ("netsurf", "qt6", "sdl2"))


def test_every_exec_path_is_rootfs_absolute():
    for name, tk in TOOLKITS.items():
        assert tk.exec_path.startswith("/"), name
        if tk.real_exec_path is not None:
            assert tk.real_exec_path.startswith("/"), name


def test_every_toolkit_has_a_leaf_package():
    assert all(tk.leaf_packages for tk in TOOLKITS.values())


def test_qt6_leaf_set_includes_the_binary_shipping_package():
    """qt6-wayland alone is libs+plugins (the original "missing" cause); the leaf
    set must add qt6-base-dev-tools, which ships the runnable qtpaths6 CLI."""
    qt6 = TOOLKITS["qt6"]
    assert "qt6-base-dev-tools" in qt6.leaf_packages
    assert "qt6-wayland" in qt6.leaf_packages
    # the deb /usr/bin/qtpaths6 is a ../lib escaping symlink dropped by §5-E, so the
    # overlay ships (and the probe must target) the real /usr/lib/qt6/bin/qtpaths6.
    assert qt6.exec_path == "/usr/lib/qt6/bin/qtpaths6"
    assert qt6.launch_arg == "--version"


def test_sdl2_leaf_set_includes_the_test_binary_package():
    """libsdl2-2.0-0 alone is a pure shared lib (no CLI — the original "missing"
    cause); the leaf set must add libsdl2-tests, which ships the SDL2 test bins."""
    sdl2 = TOOLKITS["sdl2"]
    assert "libsdl2-tests" in sdl2.leaf_packages
    assert "libsdl2-2.0-0" in sdl2.leaf_packages
    assert sdl2.exec_path == "/usr/libexec/installed-tests/SDL2/testver"


def test_netsurf_exec_path_matches_the_packaged_binary():
    """netsurf-gtk already ships /usr/bin/netsurf-gtk — the probe's 2nd candidate.
    The matrix records that exact path (not the gtk3 spelling the probe waits on)."""
    netsurf = TOOLKITS["netsurf"]
    assert netsurf.leaf_packages == ("netsurf-gtk",)
    assert netsurf.exec_path == "/usr/bin/netsurf-gtk"


def test_overlay_has_exec_detects_present_and_absent(tmp_path: Path):
    tar_path = tmp_path / "t.tar"
    present = [
        "./usr/bin/netsurf-gtk",
        "./usr/lib/qt6/bin/qtpaths6",
        "./usr/libexec/installed-tests/SDL2/testver",
    ]
    with tarfile.open(tar_path, "w") as t:
        for member in present:
            payload = b"\x7fELF" + member.encode()
            ti = tarfile.TarInfo(member)
            ti.size = len(payload)
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))

    assert overlay_has_exec(tar_path, "/usr/bin/netsurf-gtk")
    assert overlay_has_exec(tar_path, "/usr/lib/qt6/bin/qtpaths6")
    assert overlay_has_exec(tar_path, "/usr/libexec/installed-tests/SDL2/testver")
    # leading-slash agnostic
    assert overlay_has_exec(tar_path, "usr/bin/netsurf-gtk")
    # genuinely absent
    assert not overlay_has_exec(tar_path, "/usr/bin/qmake6")
    assert not overlay_has_exec(tar_path, "/usr/bin/does-not-exist")


def test_toolkit_build_ok_aggregation():
    clean = ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, ("liba",), (), (), True)
    assert clean.ok
    assert not ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, (), (), ("BLOCK foo",), True).ok
    assert not ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, (), ("libz.so.9",), (), True).ok
    assert not ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, (), (), (), False).ok


def test_builder_selftest_passes():
    from tools.build_toolkit_overlays import _selftest

    assert _selftest() == 0
