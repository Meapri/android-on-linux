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
    # The X server + x11-xkb-utils (xkbcomp, the keymap compiler the ROOTFUL X server
    # exec()s — without it Xwayland aborts AFTER taking the X0 lock with "XKB: Failed
    # to compile keymap" / "Failed to activate virtual core keyboard: 2").
    assert xw.leaf_packages == ("xwayland", "x11-xkb-utils")
    # xkbcomp is exec'd (not a DT_NEEDED), so it is asserted as a required file member.
    assert "/usr/bin/xkbcomp" in xw.expect_files
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


# --------------------------------------------------------------------------- #
# libXaw.so.7 SONAME-alias contract (the gap that broke xcalc on device)
# --------------------------------------------------------------------------- #

def test_x11app_requires_the_libxaw_soname_alias():
    """Every x11-apps Xaw client (xcalc/xclock/xlogo/xload/xgc/xmag) DT_NEEDEDs
    libXaw.so.7, but libxaw7 ships the real file libXaw7.so.7.0.0 (DT_SONAME
    libXaw.so.7) reached via libXaw.so.7 -> libXaw7.so.7 -> libXaw7.so.7.0.0 —
    §5-E flattening keys off the FILENAME and would drop the libXaw.so.7 link. The
    recipe must REQUIRE the alias so a flattener regression fails the build."""
    assert X_OVERLAYS["x11app"].expect_sonames == ("libXaw.so.7",)
    # xwayland's own private libs all have filename == DT_SONAME → no alias needed.
    assert X_OVERLAYS["xwayland"].expect_sonames == ()


def test_overlay_has_soname_real_or_alias_dir_agnostic(tmp_path: Path):
    from tools.build_xwayland_overlay import overlay_has_soname

    tar_path = tmp_path / "s.tar"
    with tarfile.open(tar_path, "w") as t:
        payload = b"\x7fELF" + b"flat-real"
        ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libXaw7.so.7")
        ti.size = len(payload)
        ti.mode = 0o644
        t.addfile(ti, io.BytesIO(payload))
        link = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libXaw.so.7")
        link.type = tarfile.SYMTYPE
        link.linkname = "libXaw7.so.7"
        t.addfile(link)

    # found as an alias symlink, and as a real flat lib, regardless of dir
    assert overlay_has_soname(tar_path, "libXaw.so.7")
    assert overlay_has_soname(tar_path, "libXaw7.so.7")
    assert not overlay_has_soname(tar_path, "libXt.so.6")


def test_missing_required_soname_fails_the_build_gate():
    # has the binary + conformant + no unresolved DT_NEEDED, but the REQUIRED
    # libXaw.so.7 alias is absent → ok must be False (would die on device).
    bad = XOverlayBuild(
        "x11app", "/tmp/x.tar", "/usr/bin/xcalc", 5, (), (), (), True,
        has_exec=True, missing_expected=("libXaw.so.7",),
    )
    assert not bad.ok
    good = XOverlayBuild(
        "x11app", "/tmp/x.tar", "/usr/bin/xcalc", 5, (), (), (), True,
        has_exec=True, missing_expected=(),
    )
    assert good.ok


def test_flatten_resynthesizes_libxaw_alias_end_to_end(tmp_path: Path):
    """INTEGRATION (offline): reproduce libxaw7's on-disk layout, run the SAME
    flattener build_x_overlay uses (build_stage_tar), and prove the produced tar
    satisfies the x11app recipe's expect_sonames (libXaw.so.7 resolvable)."""
    import struct

    from tools.build_stage_tar import build_stage_tar
    from tools.build_xwayland_overlay import overlay_has_soname

    def make_so(soname: str) -> bytes:
        strtab = b"\x00" + soname.encode() + b"\x00"
        ehdr_sz, phent, nph = 64, 56, 2
        ph_off = ehdr_sz
        dyn_off = ph_off + nph * phent
        dyn = struct.pack("<qQ", 14, 1)  # DT_SONAME -> offset 1
        dyn_final = len(dyn) + 3 * 16
        strtab_off = dyn_off + dyn_final
        dyn += struct.pack("<qQ", 5, strtab_off)   # DT_STRTAB
        dyn += struct.pack("<qQ", 10, len(strtab))  # DT_STRSZ
        dyn += struct.pack("<qQ", 0, 0)             # DT_NULL
        total = strtab_off + len(strtab)
        e_ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9
        ehdr = e_ident + struct.pack(
            "<HHIQQQIHHHHHH", 3, 183, 1, 0, ph_off, 0, 0, ehdr_sz, phent, nph, 0, 0, 0
        )
        ph_load = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, total, total, 0x1000)
        ph_dyn = struct.pack("<IIQQQQQQ", 2, 6, dyn_off, dyn_off, dyn_off, len(dyn), len(dyn), 8)
        blob = bytearray(total)
        blob[0:ehdr_sz] = ehdr
        blob[ph_off:ph_off + phent] = ph_load
        blob[ph_off + phent:ph_off + 2 * phent] = ph_dyn
        blob[dyn_off:dyn_off + len(dyn)] = dyn
        blob[strtab_off:strtab_off + len(strtab)] = strtab
        return bytes(blob)

    src = tmp_path / "src"
    libdir = src / "usr" / "lib" / "aarch64-linux-gnu"
    libdir.mkdir(parents=True)
    (libdir / "libXaw7.so.7.0.0").write_bytes(make_so("libXaw.so.7"))
    (libdir / "libXaw7.so.7").symlink_to("libXaw7.so.7.0.0")
    (libdir / "libXaw.so.7").symlink_to("libXaw7.so.7")

    out = tmp_path / "x11app-stage.tar"
    build_stage_tar(src, out)

    # the recipe expectation is satisfied by the re-synthesized alias
    for soname in X_OVERLAYS["x11app"].expect_sonames:
        assert overlay_has_soname(out, soname), soname
