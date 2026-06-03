"""Build the Xwayland (+ an X11 test app) §5-E overlays (WS-4 §5 M4 — ROOTFUL).

Why this exists
---------------
ALR runs Wayland-native Linux apps today (GIMP/galculator via GDK→wl_shm→
SurfaceView on the in-app compositor), but has **no X server** — so X11-only apps
cannot run. Only a `SurfaceProtocol.X11` enum stub and roadmap metadata exist; no
`Xwayland` binary is staged and no X client/server runtime is wired.

This builder stages the missing pieces so a simple X11 app renders on ALR:

    xwayland   ← the `xwayland` package   → /usr/bin/Xwayland   (the X server)
    x11-apps   ← the `x11-apps` package   → /usr/bin/xcalc,xeyes,xlogo,xclock,…

Xwayland is launched as a NORMAL CLIENT of the in-app Wayland compositor in
ROOTFUL mode: it creates ONE X screen presented as a single wl_surface (all X
apps share that screen), so the compositor — which already presents a maximized
toplevel — presents Xwayland's root surface with no compositor change. Rootful is
the Xwayland DEFAULT and, unlike `-rootless`, does NOT require the Wayland server
to be an X window manager (rootless+XWM is the follow-up). Xwayland then exports
``DISPLAY=:0`` and the X app connects there.

The base already provides the X CLIENT side
-------------------------------------------
The base rootfs (Ubuntu noble arm64, glibc 2.39) already ships the X client libs
(libX11.so.6, libxcb.so.1, libxcb-render/shm, libXau.so.6, libXdmcp.so.6,
libpixman-1.so.0, libXext/Xrender/Xfixes/Xi/Xrandr/Xcursor, libwayland-client,
libxkbcommon) AND the full /usr/share/X11/xkb data. It does NOT ship the X SERVER
(Xwayland) nor a handful of Xwayland's private deps. The host-audited gap (real
libs missing from base, by SONAME) is:

    libXfont2.so.2  libfontenc.so.1  libtirpc.so.3  libxshmfence.so.1
    libwayland-server.so.0  libgbm.so.1  libdrm.so.2  libxcvt.so.0
    libGL.so.1 libGLdispatch.so.0 libGLX.so.0 libEGL.so.1   (glamor — inert under -shm)
    libunwind.so.8  libnettle.so.*  (+ whatever DT_NEEDED pulls that the base lacks)

We do NOT hand-maintain that list: ``deb_closure.build_minimal_overlay`` keeps the
leaf package's own files (the Xwayland binary) + ONLY the libs it links via
DT_NEEDED that the base lacks (SONAME-frozen + path subtracted), so the base lib
always wins (no harfbuzz-class downgrade) and the overlay carries just the delta.

Software rendering for the first rung
-------------------------------------
The ALR compositor is wl_shm-only (no dmabuf/DRI3/EGL for clients), so Xwayland is
launched with ``-shm`` (+ ``XWAYLAND_NO_GLAMOR=1``) → it uses the shared-memory
backend and never touches glamor/DRI3/GBM/EGL at RUNTIME. The GL/GBM/DRM libs may
still be DT_NEEDED-linked by the Xwayland binary (so they enter the overlay), but
they are never exercised on the ``-shm`` path. X apps are therefore software-
rendered for now (honest gap; GPU-backed Xwayland is a later rung).

Output: §5-E ``./``-rooted, flat-SONAME overlay tars (build_minimal_overlay →
build_stage_tar): the Xwayland binary 0o755 (deb mode preserved), base-subtracted
by SONAME (downgrade-frozen) and by path, validated against the base by
overlay_guard + stage_tar_spec. NETWORK build path (ports.ubuntu.com noble);
``--selftest`` is fully OFFLINE.

This module reuses, and does NOT duplicate:
  * tools.deb_closure.build_minimal_overlay — the DT_NEEDED-minimal §5-E build engine
  * tools.overlay_guard / tools.stage_tar_spec — the apply-time + structural gates
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path

from tools.deb_closure import build_minimal_overlay

# The ALR base is Ubuntu noble 24.04 arm64 (glibc 2.39); its libs split across
# main + universe, so both components are needed or many deps look "missing".
DEFAULT_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
DEFAULT_SUITE = "noble"
DEFAULT_COMPONENTS = ("main", "universe")
DEFAULT_CACHE = "/tmp/deb-cache-ubuntu"

# The exact ROOTFUL Xwayland invocation the run path uses (verified against the
# Xwayland(1) manpage, xwayland 23.2 in noble):
#   * rootful is the DEFAULT (no -rootless) — one X screen as a single wl_surface,
#     and unlike -rootless it does NOT require the compositor to be an X WM;
#   * -geometry WxH sizes the rootful X screen (we pass the device panel size);
#   * -shm forces the shared-memory backend (the ALR compositor is wl_shm-only),
#     so Xwayland never touches glamor/DRI3/GBM/EGL — software rendering;
#   * Xwayland exports DISPLAY=:0; the X app is then run with DISPLAY=:0 + the
#     same WAYLAND_DISPLAY/XDG_RUNTIME_DIR the wl guests already use.
# (-displayfd <fd> — Xwayland writes the chosen display number to <fd> when the
# socket is ready — is the readiness signal for the auto-launch path; the
# marker-gated first rung uses a fixed :0 and polls for /tmp/.X11-unix/X0.)
XWAYLAND_DISPLAY = ":0"
XWAYLAND_ROOTFUL_ARGS = (XWAYLAND_DISPLAY, "-shm", "-geometry", "{W}x{H}")
XWAYLAND_ENV = {
    "XWAYLAND_NO_GLAMOR": "1",  # belt-and-braces with -shm: no glamor/DRI3 at all
}


@dataclass(frozen=True)
class XOverlay:
    """One X-stack overlay recipe (mirrors build_toolkit_overlays.Toolkit).

    name           stage-tar slot name (→ /tmp/<name>-stage.tar, MainActivity loop key)
    leaf_packages  packages whose OWN files are kept entirely (binaries + data);
                   their DT_NEEDED-reachable libs the base lacks are added.
    exec_path      the in-rootfs path the run path execs / the probe asserts.
    launch_arg     a display-free probe argument (e.g. -version) for an exec smoke,
                   or "" when the binary needs a display/no such flag.
    ok_marker      a substring the binary prints on the probe path (advisory).
    keep_prefixes  data dirs to force-keep even if not DT_NEEDED-reachable.
    expect_sonames a DT_SONAME name (basename) the flattened overlay MUST ship as a
                   real-or-alias member because the package's real-file BASENAME
                   encodes a *different* soname (e.g. libxaw7's libXaw7.so.7.0.0 has
                   DT_SONAME libXaw.so.7 — every x11-apps binary DT_NEEDEDs the
                   latter). build_stage_tar re-synthesizes the dropped SONAME alias;
                   this lists what must be present so a flattener regression that
                   re-drops it FAILS the build (not silently ships an app that dies
                   "libXaw.so.7: cannot open shared object file" on device).
    """

    name: str
    leaf_packages: tuple[str, ...]
    exec_path: str
    launch_arg: str
    ok_marker: str
    keep_prefixes: tuple[str, ...] = ()
    expect_sonames: tuple[str, ...] = ()
    expect_files: tuple[str, ...] = ()  # rootfs-absolute paths the overlay MUST ship as
                                        # real files (e.g. /usr/bin/xkbcomp) — a leaf
                                        # binary the run path exec()s but no DT_NEEDED
                                        # pulls; asserted so a recipe regression that
                                        # drops it FAILS the build, not the device.
    note: str = ""


# The matrix: the X server first, then a simple X11 test app.
X_OVERLAYS: dict[str, XOverlay] = {
    # The X server itself. `Xwayland -version` prints the version banner and exits
    # WITHOUT needing a display/compositor (a pure exec smoke through the loader).
    # build_minimal_overlay keeps /usr/bin/Xwayland + the DT_NEEDED libs the base
    # lacks (libXfont2/libfontenc/libtirpc/libxshmfence/libwayland-server/… and the
    # glamor GL/GBM/DRM libs, which are inert under -shm). keep_prefixes pulls the
    # SecurityPolicy / serverconfig data Xwayland reads at startup.
    "xwayland": XOverlay(
        name="xwayland",
        # `xwayland` is the X server; `x11-xkb-utils` provides `xkbcomp` — the EXTERNAL
        # keyboard-map compiler Xwayland exec()s at startup to build its virtual-core
        # keyboard keymap. WITHOUT it a ROOTFUL Xwayland aborts AFTER taking the X0 lock
        # with "XKB: Failed to compile keymap / Keyboard initialization failed ... missing
        # or incorrect setup of xkeyboard-config" → "(EE) Failed to activate virtual core
        # keyboard: 2" (device-proven: the X0-lock EPERM is fixed by the interposer's
        # link→rename fallback, exposing THIS next blocker). xkbcomp is exec'd (not a
        # DT_NEEDED of Xwayland), so the DT_NEEDED-minimal flattener never pulls it — it
        # must be named as its own leaf package. The xkb DATA (rules/symbols/keycodes) is
        # already in the base under /usr/share/X11/xkb; xkbcomp links libxkbfile.so.1
        # (base-provided). Riding the SAME xwayland-stage.tar keeps the staging wiring.
        leaf_packages=("xwayland", "x11-xkb-utils"),
        exec_path="/usr/bin/Xwayland",
        launch_arg="-version",
        ok_marker="X.Org",  # "X.Org X Server" / "XWAYLAND" appears in -version output
        keep_prefixes=(
            "/usr/share/X11/xkb",          # already in base, but harmless to force-keep
            "/usr/lib/xorg",               # any Xwayland-private modules under here
            "/usr/share/X11/XErrorDB",
        ),
        # xkbcomp is the load-bearing addition: assert it ships so a recipe/flattener
        # regression that drops it fails the BUILD, not the device (Xwayland keymap).
        expect_files=("/usr/bin/xkbcomp",),
        note="ROOTFUL is the default (no -rootless → no XWM needed); launched -shm "
        "(compositor is wl_shm-only) so glamor/DRI3/GBM/EGL stay inert. DISPLAY=:0. "
        "Bundles xkbcomp (x11-xkb-utils) for the X server's virtual-core keymap compile.",
    ),
    # A simple X11 app to prove the screen: x11-apps ships xcalc/xeyes/xlogo/xclock.
    # These link the X CLIENT libs the BASE already provides (libX11/libXt/libXaw/…)
    # — so this overlay should be tiny (mostly the app binaries + libXaw/libXt/libXmu
    # if the base lacks them). The run path execs e.g. `DISPLAY=:0 xcalc`.
    #
    # SONAME audit (readelf -d on every x11-apps binary vs what flattening emits):
    # the ONLY DT_NEEDED whose flat name would NOT match the linked soname is
    # libXaw.so.7. libxaw7 ships the real file libXaw7.so.7.0.0 (DT_SONAME
    # "libXaw.so.7") reached via libXaw.so.7 -> libXaw7.so.7 -> libXaw7.so.7.0.0;
    # §5-E flattening keys off the FILENAME so it emits libXaw7.so.7 and drops the
    # libXaw.so.7 link, yet xcalc/xclock/xlogo/xload/xgc/xmag all DT_NEEDED
    # libXaw.so.7. build_stage_tar now re-synthesizes that alias; expect_sonames
    # asserts it so a flattener regression re-breaks the BUILD, not the device.
    # All other x11-apps DT_NEEDED (libXt.so.6, libXmu.so.6, libXft.so.2,
    # libxkbfile.so.1, libXrender.so.1, libXext.so.6, libSM.so.6, libXi.so.6,
    # libXmuu.so.1, libxcb-*.so.0) either match their filename soname after flatten
    # or are already in the base — no extra alias needed.
    "x11app": XOverlay(
        name="x11app",
        leaf_packages=("x11-apps",),
        exec_path="/usr/bin/xcalc",
        launch_arg="",  # xcalc needs a display; the smoke is the rootful run, not -version
        ok_marker="",
        keep_prefixes=(
            "/usr/share/X11/app-defaults",  # Xaw widget resources xcalc/xlogo read
        ),
        expect_sonames=("libXaw.so.7",),
        note="xcalc/xeyes/xlogo/xclock; needs DISPLAY=:0 from Xwayland. Mostly links "
        "base-provided X client libs, so the overlay is small. libXaw.so.7 alias is "
        "re-synthesized by the flattener (libxaw7 real file basename != DT_SONAME).",
    ),
}


@dataclass(frozen=True)
class XOverlayBuild:
    overlay: str
    out_tar: str
    exec_path: str
    file_count: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    violations: tuple[str, ...]
    conformant: bool
    has_exec: bool = False
    # DT_SONAME names the recipe REQUIRED (expect_sonames) that are NOT present in
    # the built tar as a real-or-alias member. Non-empty => the overlay would ship
    # an app that can't resolve a linked soname on device (e.g. libXaw.so.7) => FAIL.
    missing_expected: tuple[str, ...] = ()
    # rootfs-absolute paths the recipe REQUIRED (expect_files) that are NOT present
    # as real file members. Non-empty => the overlay is missing an exec'd helper the
    # run path needs (e.g. /usr/bin/xkbcomp for the Xwayland keymap compile) => FAIL.
    missing_files: tuple[str, ...] = ()

    @property
    def ok(self) -> bool:
        return (
            self.has_exec
            and not self.violations
            and not self.missing_soname
            and not self.missing_expected
            and not self.missing_files
            and self.conformant
        )


def build_x_overlay(
    overlay: XOverlay,
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = DEFAULT_MIRROR,
    suite: str = DEFAULT_SUITE,
    arch: str = "arm64",
    components=DEFAULT_COMPONENTS,
    cache_dir: str | Path | None = DEFAULT_CACHE,
) -> XOverlayBuild:
    """Build one X-stack overlay's §5-E DT_NEEDED-minimal tar (NETWORK).

    Delegates to ``deb_closure.build_minimal_overlay`` — keeps the leaf package's
    own files (the binary among them) + the libs it links via DT_NEEDED that the
    base lacks, base-subtracted (SONAME-frozen + path) and flattened to a §5-E tar,
    then validated with overlay_guard + stage_tar_spec. Returns an XOverlayBuild.
    """
    m = build_minimal_overlay(
        list(overlay.leaf_packages),
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache_dir,
        keep_prefixes=overlay.keep_prefixes,
    )

    from tools.stage_tar_spec import validate_stage_tar

    rep = validate_stage_tar(m["out_tar"], base=base)
    has_exec = overlay_has_exec(m["out_tar"], overlay.exec_path)
    missing_expected = tuple(
        s for s in overlay.expect_sonames
        if not overlay_has_soname(m["out_tar"], s)
    )
    missing_files = tuple(
        f for f in overlay.expect_files
        if not overlay_has_exec(m["out_tar"], f)
    )

    return XOverlayBuild(
        overlay=overlay.name,
        out_tar=m["out_tar"],
        exec_path=overlay.exec_path,
        file_count=m["file_count"],
        reachable_libs=tuple(m["reachable_libs"]),
        missing_soname=tuple(m["missing_soname"]),
        violations=tuple(m["violations"]),
        conformant=rep.conformant,
        has_exec=has_exec,
        missing_expected=missing_expected,
        missing_files=missing_files,
    )


def overlay_has_exec(out_tar: str | Path, exec_path: str) -> bool:
    """True if ``out_tar`` contains the given in-rootfs exec path as a member.

    Member matched ./-rooted; leading-slash agnostic. Used by the builder report +
    the host test to PROVE the overlay actually carries the launchable binary
    (the whole point — no "Xwayland missing" on device).
    """
    import tarfile

    want = "./" + exec_path.lstrip("/")
    with tarfile.open(out_tar, "r:*") as tar:
        names = set(tar.getnames())
    return want in names


def overlay_has_soname(out_tar: str | Path, soname: str) -> bool:
    """True if ``out_tar`` provides the given DT_SONAME (basename) as a member in
    ANY directory — either a real flat file or a SONAME-alias symlink. Used to
    PROVE the overlay can satisfy a linked soname whose package real-file basename
    differs from the DT_SONAME (the libXaw.so.7 / libxaw7 case), which §5-E
    flattening would otherwise silently drop. Matched by basename so it is agnostic
    to the lib directory (/usr/lib/aarch64-linux-gnu vs /lib/...)."""
    import posixpath
    import tarfile

    with tarfile.open(out_tar, "r:*") as tar:
        for name in tar.getnames():
            if posixpath.basename(name) == soname:
                return True
    return False


def xwayland_launch_argv(width: int, height: int, display: str = XWAYLAND_DISPLAY) -> list[str]:
    """The exact ROOTFUL Xwayland argv the run path should exec (newline-joined into
    the loader's program-spec). ``-shm`` forces the wl_shm backend; ``-geometry``
    sizes the single rootful X screen to the device panel; ``display`` is the X
    display Xwayland serves (DISPLAY=:0 for the X app). Pure/offline-testable."""
    argv = [overlay_for("xwayland").exec_path, display, "-shm", "-geometry", f"{width}x{height}"]
    return argv


def overlay_for(name: str) -> XOverlay:
    return X_OVERLAYS[name]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_xwayland_overlay",
        description="Build the ROOTFUL Xwayland + an X11 test app §5-E overlays from "
        "Ubuntu noble (DT_NEEDED-minimal, base-subtracted, downgrade-safe).",
    )
    parser.add_argument(
        "--overlay",
        action="append",
        dest="overlays",
        choices=sorted(X_OVERLAYS),
        help="overlay to build (repeatable; default: all)",
    )
    parser.add_argument("--base", help="base rootfs tar|dir (for soname/path subtraction + guard)")
    parser.add_argument(
        "--out-dir",
        default="/tmp",
        help="output directory for <overlay>-stage.tar (default: %(default)s)",
    )
    parser.add_argument("--mirror", default=DEFAULT_MIRROR)
    parser.add_argument("--suite", default=DEFAULT_SUITE)
    parser.add_argument("--arch", default="arm64")
    parser.add_argument(
        "--component", action="append", dest="components",
        help="repo component (repeatable; default main + universe)",
    )
    parser.add_argument("--cache", default=DEFAULT_CACHE)
    parser.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.base:
        parser.error("--base is required (or use --selftest)")

    components = tuple(args.components) if args.components else DEFAULT_COMPONENTS
    names = args.overlays or sorted(X_OVERLAYS)
    out_dir = Path(args.out_dir)

    overall_ok = True
    for name in names:
        ov = X_OVERLAYS[name]
        out_tar = out_dir / f"{name}-stage.tar"
        build = build_x_overlay(
            ov, args.base, out_tar,
            mirror=args.mirror, suite=args.suite, arch=args.arch,
            components=components, cache_dir=args.cache,
        )
        print(f"[{name}] {build.out_tar}")
        print(f"    files:           {build.file_count}")
        print(f"    exec (probe):    {build.exec_path}")
        print(f"    exec in overlay: {'YES' if build.has_exec else 'NO — MISSING'}")
        print(f"    reachable libs:  {len(build.reachable_libs)} {list(build.reachable_libs)}")
        if ov.expect_sonames:
            present = [s for s in ov.expect_sonames if s not in build.missing_expected]
            print(f"    expect sonames:  {list(ov.expect_sonames)} → "
                  f"present {present}" +
                  (f"  MISSING {list(build.missing_expected)}" if build.missing_expected else ""))
        if build.missing_soname:
            print(f"    MISSING sonames: {list(build.missing_soname)}")
        print(f"    overlay_guard:   {'OK' if not build.violations else str(len(build.violations)) + ' violation(s)'}")
        for v in build.violations:
            print(f"      {v}")
        print(f"    stage_tar_spec:  {'CONFORMANT' if build.conformant else 'NON-CONFORMANT'}")
        if name == "xwayland":
            argv_demo = xwayland_launch_argv(1200, 1920)
            print(f"    rootful launch:  {' '.join(argv_demo)}   (DISPLAY={XWAYLAND_DISPLAY}, XWAYLAND_NO_GLAMOR=1)")
        print(f"    => {'PASS' if build.ok else 'FAIL'}")
        overall_ok = overall_ok and build.ok

    return 0 if overall_ok else 1


def _selftest() -> int:
    """OFFLINE: validates the recipe matrix + overlay_has_exec + the launch argv,
    no network / no real .deb."""
    import io
    import tarfile
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- matrix invariants -------------------------------------------------
    check("2 overlays defined (xwayland, x11app)",
          set(X_OVERLAYS) == {"xwayland", "x11app"})
    check("every overlay names at least one leaf package",
          all(ov.leaf_packages for ov in X_OVERLAYS.values()))
    check("every exec_path is rootfs-absolute (/...)",
          all(ov.exec_path.startswith("/") for ov in X_OVERLAYS.values()))
    check("xwayland leaf = xwayland + x11-xkb-utils (xkbcomp for the keymap compile)",
          X_OVERLAYS["xwayland"].leaf_packages == ("xwayland", "x11-xkb-utils"))
    check("xwayland exec is /usr/bin/Xwayland (the X server binary)",
          X_OVERLAYS["xwayland"].exec_path == "/usr/bin/Xwayland")
    check("xwayland requires /usr/bin/xkbcomp (exec'd; no DT_NEEDED pulls it)",
          "/usr/bin/xkbcomp" in X_OVERLAYS["xwayland"].expect_files)
    check("x11app leaf is the `x11-apps` package",
          X_OVERLAYS["x11app"].leaf_packages == ("x11-apps",))
    check("x11app exec is an x11-apps binary (xcalc)",
          X_OVERLAYS["x11app"].exec_path == "/usr/bin/xcalc")
    # libXaw.so.7 is the one DT_NEEDED whose flat name != soname; the recipe must
    # require it so a flattener regression that re-drops the alias FAILS the build.
    check("x11app requires the libXaw.so.7 SONAME alias (libxaw7 basename mismatch)",
          "libXaw.so.7" in X_OVERLAYS["x11app"].expect_sonames)
    check("xwayland requires no extra SONAME alias (its libs' filenames == soname)",
          X_OVERLAYS["xwayland"].expect_sonames == ())

    # --- ROOTFUL launch argv (the load-bearing invocation) -----------------
    argv = xwayland_launch_argv(1200, 1920)
    check("launch argv starts with /usr/bin/Xwayland",
          argv[0] == "/usr/bin/Xwayland")
    check("launch argv serves DISPLAY :0", argv[1] == ":0")
    check("launch argv forces the wl_shm backend (-shm)", "-shm" in argv)
    check("launch argv sizes the rootful screen via -geometry",
          "-geometry" in argv and "1200x1920" in argv)
    check("launch argv is ROOTFUL (no -rootless → no XWM required)",
          "-rootless" not in argv)
    check("XWAYLAND_NO_GLAMOR=1 is in the launch env (belt-and-braces with -shm)",
          XWAYLAND_ENV.get("XWAYLAND_NO_GLAMOR") == "1")
    # geometry templating in the constant matches the helper output
    check("XWAYLAND_ROOTFUL_ARGS templates W/H",
          "-geometry" in XWAYLAND_ROOTFUL_ARGS and "{W}x{H}" in XWAYLAND_ROOTFUL_ARGS)

    # --- overlay_has_exec --------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "t.tar"
        with tarfile.open(tar_path, "w") as t:
            for member in ("./usr/bin/Xwayland", "./usr/bin/xcalc"):
                payload = b"\x7fELF" + member.encode()
                ti = tarfile.TarInfo(member)
                ti.size = len(payload)
                ti.mode = 0o755
                t.addfile(ti, io.BytesIO(payload))
        check("overlay_has_exec finds /usr/bin/Xwayland",
              overlay_has_exec(tar_path, "/usr/bin/Xwayland"))
        check("overlay_has_exec finds /usr/bin/xcalc",
              overlay_has_exec(tar_path, "/usr/bin/xcalc"))
        check("overlay_has_exec is leading-slash agnostic",
              overlay_has_exec(tar_path, "usr/bin/Xwayland"))
        check("overlay_has_exec reports a genuinely missing binary as False",
              not overlay_has_exec(tar_path, "/usr/bin/Xorg"))

    # --- overlay_has_soname (real-or-alias, dir-agnostic) ------------------
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "s.tar"
        with tarfile.open(tar_path, "w") as t:
            # a flat real lib + a SONAME-alias symlink (the libXaw shape)
            payload = b"\x7fELF" + b"flat"
            ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libXaw7.so.7")
            ti.size = len(payload); ti.mode = 0o644
            t.addfile(ti, io.BytesIO(payload))
            link = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libXaw.so.7")
            link.type = tarfile.SYMTYPE; link.linkname = "libXaw7.so.7"
            t.addfile(link)
        check("overlay_has_soname finds an aliased soname (libXaw.so.7 symlink)",
              overlay_has_soname(tar_path, "libXaw.so.7"))
        check("overlay_has_soname finds a real flat soname (libXaw7.so.7)",
              overlay_has_soname(tar_path, "libXaw7.so.7"))
        check("overlay_has_soname reports a genuinely absent soname as False",
              not overlay_has_soname(tar_path, "libXt.so.6"))

    # --- XOverlayBuild.ok aggregation --------------------------------------
    good = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, ("liba",), (), (), True, has_exec=True)
    no_bin = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), (), (), True, has_exec=False)
    bad_guard = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), (), ("BLOCK foo",), True, has_exec=True)
    bad_miss = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), ("libz.so.9",), (), True, has_exec=True)
    bad_conf = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/Xwayland", 5, (), (), (), False, has_exec=True)
    bad_expect = XOverlayBuild("x", "/tmp/x.tar", "/usr/bin/xcalc", 5, (), (), (), True,
                               has_exec=True, missing_expected=("libXaw.so.7",))
    check("XOverlayBuild.ok True when clean + has binary", good.ok)
    check("XOverlayBuild.ok False when the binary is missing", not no_bin.ok)
    check("XOverlayBuild.ok False on guard violation", not bad_guard.ok)
    check("XOverlayBuild.ok False on missing soname", not bad_miss.ok)
    check("XOverlayBuild.ok False when non-conformant", not bad_conf.ok)
    check("XOverlayBuild.ok False when a required SONAME alias is missing",
          not bad_expect.ok)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
