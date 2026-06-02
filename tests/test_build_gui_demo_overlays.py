"""Host tests for tools/build_gui_demo_overlays.py (WS-4 §5-E round-4).

All OFFLINE — exercise the window-opening GUI demo matrix, the .so x-bit
enforcement, the apt/dpkg install overlay assembly and the build verdict
aggregation against synthetic tars (no network / no real .deb). The network build
path (ports.ubuntu.com noble) is run by hand on the host and is the
integration/device gate.

The contract this WS-4 round establishes (the previous round staged only
display-free CLIs — qtpaths6 --version, SDL2 testver — which never open a window):
  * qt6gui ships the analogclock QtWidgets WINDOW binary + qt6-wayland's dlopen'd
    QPA platform plugin (so QT_QPA_PLATFORM=wayland actually creates a surface);
  * sdl2gui ships testsprite2, a windowing SDL2 demo (not the headless testver);
  * every dlopen'd .so is forced 0o755 (ALR PROT_EXEC dlopen rejects a non-x .so);
  * aptdemo bundles the raw `hello` .deb at /root/hello.deb + apt config so the
    guest can dpkg --unpack it (configure honestly deferred — maintainer-script
    fork+exec is a separate ALR feature).
"""

import io
import tarfile
from pathlib import Path

from tools.build_gui_demo_overlays import (
    APT_DEMO_DEB_DEST,
    APT_DEMO_PACKAGE,
    APT_DEMO_README,
    AptDemoBuild,
    GUI_DEMOS,
    GuiDemo,
    GuiDemoBuild,
    _add_bytes,
    force_so_executable,
    overlay_has_exec,
)


def test_two_gui_demos_defined():
    assert set(GUI_DEMOS) == {"qt6gui", "sdl2gui"}
    assert all(isinstance(d, GuiDemo) for d in GUI_DEMOS.values())


def test_every_exec_path_is_rootfs_absolute():
    for name, d in GUI_DEMOS.items():
        assert d.exec_path.startswith("/"), name


def test_qt6gui_carries_window_binary_and_wayland_plugin():
    """qt6-base-examples ships the analogclock WINDOW binary; qt6-wayland ships the
    dlopen'd QPA platform plugin (libqwayland-generic.so) — BOTH are required for a
    window to appear on the ALR Wayland compositor."""
    qt = GUI_DEMOS["qt6gui"]
    assert "qt6-base-examples" in qt.leaf_packages
    assert "qt6-wayland" in qt.leaf_packages
    assert qt.exec_path.endswith("/analogclock/analogclock")


def test_sdl2gui_is_a_windowing_demo_not_the_headless_testver():
    sdl = GUI_DEMOS["sdl2gui"]
    assert "libsdl2-tests" in sdl.leaf_packages
    assert "libsdl2-2.0-0" in sdl.leaf_packages
    # the previous round's testver opens no window; this round uses testsprite2.
    assert sdl.exec_path.endswith("/SDL2/testsprite2")
    assert "testver" not in sdl.exec_path


def test_apt_demo_constants():
    assert APT_DEMO_PACKAGE == "hello"
    assert APT_DEMO_DEB_DEST.startswith("/root/")
    # the README must be honest that configure (maintainer-script fork+exec) is
    # a separate ALR feature and only UNPACK is enabled today.
    assert "UNPACK" in APT_DEMO_README
    assert "configure" in APT_DEMO_README


def test_force_so_executable_raises_only_non_x_so(tmp_path: Path):
    tar_path = tmp_path / "t.tar"
    layout = [
        ("./usr/lib/qt6/plugins/platforms/libqwayland-generic.so", 0o644),  # plugin
        ("./usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0", 0o644),            # versioned
        ("./usr/bin/analogclock", 0o755),       # already exec -> untouched
        ("./root/APT_DEMO_README.txt", 0o644),  # not a .so -> untouched
    ]
    with tarfile.open(tar_path, "w") as t:
        for name, mode in layout:
            payload = b"\x7fELF" + name.encode()
            ti = tarfile.TarInfo(name)
            ti.size = len(payload)
            ti.mode = mode
            t.addfile(ti, io.BytesIO(payload))

    changed = force_so_executable(tar_path)
    assert changed == 2

    with tarfile.open(tar_path, "r:*") as t:
        modes = {m.name: (m.mode & 0o777) for m in t.getmembers()}
        members = t.getmembers()
    assert modes["./usr/lib/qt6/plugins/platforms/libqwayland-generic.so"] == 0o755
    assert modes["./usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0"] == 0o755
    assert modes["./usr/bin/analogclock"] == 0o755          # preserved
    assert modes["./root/APT_DEMO_README.txt"] == 0o644     # untouched
    assert len(members) == 4                                # nothing lost


def test_force_so_executable_is_idempotent(tmp_path: Path):
    tar_path = tmp_path / "t.tar"
    with tarfile.open(tar_path, "w") as t:
        payload = b"\x7fELF"
        ti = tarfile.TarInfo("./usr/lib/x/libfoo.so")
        ti.size = len(payload)
        ti.mode = 0o644
        t.addfile(ti, io.BytesIO(payload))
    assert force_so_executable(tar_path) == 1
    assert force_so_executable(tar_path) == 0  # second pass: nothing to change


def test_force_so_executable_preserves_bytes(tmp_path: Path):
    tar_path = tmp_path / "t.tar"
    blob = b"\x7fELF" + b"sdl2-payload" * 7
    with tarfile.open(tar_path, "w") as t:
        ti = tarfile.TarInfo("./usr/lib/libSDL2-2.0.so.0")
        ti.size = len(blob)
        ti.mode = 0o644
        t.addfile(ti, io.BytesIO(blob))
    force_so_executable(tar_path)
    with tarfile.open(tar_path, "r:*") as t:
        assert t.extractfile("./usr/lib/libSDL2-2.0.so.0").read() == blob


def test_overlay_has_exec_present_and_absent(tmp_path: Path):
    tar_path = tmp_path / "o.tar"
    exe = GUI_DEMOS["qt6gui"].exec_path
    with tarfile.open(tar_path, "w") as t:
        payload = b"\x7fELF"
        ti = tarfile.TarInfo("./" + exe.lstrip("/"))
        ti.size = len(payload)
        ti.mode = 0o755
        t.addfile(ti, io.BytesIO(payload))
    assert overlay_has_exec(tar_path, exe)
    assert overlay_has_exec(tar_path, exe.lstrip("/"))  # slash-agnostic
    assert not overlay_has_exec(tar_path, "/usr/bin/nope")


def test_apt_demo_overlay_assembly_is_conformant(tmp_path: Path):
    """The apt-demo tar-assembly half (no network): a raw .deb at /root/hello.deb +
    apt config must be §5-E conformant and introduce no base downgrade."""
    base = tmp_path / "base.tar"
    with tarfile.open(base, "w") as t:
        ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libc.so.6")
        ti.size = 4
        ti.mode = 0o755
        t.addfile(ti, io.BytesIO(b"\x7fELF"))

    out = tmp_path / "aptdemo-stage.tar"
    with tarfile.open(out, "w") as tar:
        _add_bytes(tar, "./root/hello.deb", b"!<arch>\nfake", mode=0o644)
        _add_bytes(tar, "./etc/apt/apt.conf.d/99alr-demo", b'APT::x "y";\n', mode=0o644)
        _add_bytes(tar, "./root/APT_DEMO_README.txt", APT_DEMO_README.encode(), mode=0o644)

    from tools.stage_tar_spec import validate_stage_tar
    from tools.overlay_guard import scan_overlay_violations, BLOCK

    rep = validate_stage_tar(str(out), base=str(base))
    assert rep.conformant, rep.errors
    blocks = [v for v in scan_overlay_violations(str(base), str(out)) if v.severity == BLOCK]
    assert blocks == []

    with tarfile.open(out, "r:*") as t:
        names = set(t.getnames())
    assert "./root/hello.deb" in names
    assert "./etc/apt/apt.conf.d/99alr-demo" in names


def test_gui_demo_build_ok_aggregation():
    clean = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, ("liba",), (), (), 1, True, True)
    assert clean.ok
    # exec missing
    assert not GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), (), 1, False, True).ok
    # guard violation
    assert not GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), ("BLOCK z",), 1, True, True).ok
    # missing soname
    assert not GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), ("libz.so.9",), (), 1, True, True).ok
    # non-conformant
    assert not GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), (), 1, True, False).ok


def test_apt_demo_build_ok_aggregation():
    assert AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 100, "hello", "2.10", (), True, ()).ok
    assert not AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 0, "hello", "2.10", (), True, ()).ok
    assert not AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 100, "hello", "2.10", (), False, ()).ok
    assert not AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 100, "hello", "2.10", (), True, ("BLOCK",)).ok


def test_builder_selftest_passes():
    from tools.build_gui_demo_overlays import _selftest

    assert _selftest() == 0
