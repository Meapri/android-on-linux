"""Build the *window-opening* GUI demo overlays + an apt/dpkg install overlay
(WS-4 §5-E / §10 round-4).

Why this exists (what the previous round did NOT do)
----------------------------------------------------
``build_toolkit_overlays.py`` (the previous round) staged toolkit *libraries* plus
a **display-free CLI** smoke per toolkit:

    qt6  -> /usr/lib/qt6/bin/qtpaths6   (--version, a CLI: NO window)
    sdl2 -> .../SDL2/testver            (prints SDL version: NO window)

So they proved the libs *load*, but nothing ever opened a window. This builder adds
the missing piece — a binary that, run on the ALR Wayland compositor, **opens an
actual GUI window**:

    qt6gui  <- qt6-base-examples + qt6-wayland
              /usr/lib/aarch64-linux-gnu/qt6/examples/widgets/widgets/analogclock/analogclock
              (a pure QtWidgets analog-clock window; qt6-wayland supplies the
               `libqwayland-generic.so` QPA platform plugin so QPA=wayland works)

    sdl2gui <- libsdl2-2.0-0 + libsdl2-tests
              /usr/libexec/installed-tests/SDL2/testsprite2
              (the upstream SDL2 bouncing-sprites demo — opens a window, draws
               sprites every frame; libSDL2 has the Wayland video driver built in)

and a third deliverable that lets the guest *install a package*:

    aptdemo <- the raw `hello` .deb at /root/hello.deb + an apt sources/keyring-free
              config, paired with the reconstructed dpkg admin DB (build_dpkg_db) so
              dpkg/apt see the base as a real install. The guest can then
              `dpkg -i /root/hello.deb` (UNPACK works offline). HONEST LIMIT: the
              *configure* step runs maintainer scripts via fork+exec which is a
              separate large ALR feature (exec re-entry) — see APT_DEMO_README below.

Window-opening, not headless
----------------------------
A toolkit "GUI demo" here means a binary whose normal run creates a top-level
surface. We do NOT pass a display-free flag — the integration/launch session runs
it against the ALR compositor (WAYLAND_DISPLAY set) so a window appears. DT_NEEDED
(host-verified with tools.elf_needed):

  analogclock : libQt6Core/Gui/Widgets.so.6 (overlay) + libstdc++/libgcc/libc (base)
                + dlopen libqwayland-generic.so (qt6-wayland leaf, kept entirely)
  testsprite2 : libSDL2-2.0.so.0 (overlay) + libc/ld (base); icon.bmp ships alongside

§5-E conformance
----------------
Each overlay is a ``./``-rooted, flat-SONAME tar built by
``deb_closure.build_minimal_overlay`` (leaf files kept entirely + DT_NEEDED libs the
base lacks, base-subtracted by SONAME *and* path so nothing downgrades the base),
then **every dlopen'd ``.so`` is forced to 0o755** (ALR file-backed PROT_EXEC dlopen
under untrusted_app rejects a non-x ``.so`` — the babl/gegl/SVG class of fault;
RootfsInstaller also force-adds the bit, we make the tar itself correct). Validated
with overlay_guard + stage_tar_spec. NETWORK build (ports.ubuntu.com noble
main+universe); ``--selftest`` is OFFLINE.

This module reuses, and does NOT duplicate:
  * tools.deb_closure.build_minimal_overlay  — the DT_NEEDED-minimal §5-E engine
  * tools.build_dpkg_db.build_dpkg_db        — the dpkg admin-DB reconstruction
  * tools.deb_closure._download_deb / fetch_packages_index / parse_packages
  * tools.overlay_guard / tools.stage_tar_spec — apply-time + structural gates
"""

from __future__ import annotations

import argparse
import io
import tarfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from tools.deb_closure import (
    _download_deb,
    build_minimal_overlay,
    fetch_packages_index,
    parse_packages,
)

DEFAULT_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
DEFAULT_SUITE = "noble"
DEFAULT_COMPONENTS = ("main", "universe")
DEFAULT_CACHE = "/tmp/deb-cache-ubuntu"


# --------------------------------------------------------------------------- #
# GUI demo matrix (window-opening binaries)
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class GuiDemo:
    """One window-opening GUI demo overlay recipe.

    name          stage-tar slot (→ /tmp/<name>-stage.tar, launch-loop key)
    leaf_packages packages kept entirely (the demo binary + the dlopen'd platform
                  plugin live among these; their DT_NEEDED libs the base lacks are
                  added). For Qt the plugin package (qt6-wayland) MUST be a leaf so
                  the QPA wayland plugin (dlopen'd, not DT_NEEDED) survives.
    exec_path     the in-rootfs binary the launch session runs to OPEN A WINDOW
    needs         data assets the binary loads (informational; leaf files are kept)
    """

    name: str
    leaf_packages: tuple[str, ...]
    exec_path: str
    note: str = ""


GUI_DEMOS: dict[str, GuiDemo] = {
    # Qt6 Widgets analog clock — a top-level QWidget window. qt6-base-examples ships
    # the prebuilt ELF; qt6-wayland supplies the dlopen'd Wayland QPA platform plugin
    # (libqwayland-generic.so) so `-platform wayland` / QT_QPA_PLATFORM=wayland opens
    # a real window on the ALR compositor.
    "qt6gui": GuiDemo(
        name="qt6gui",
        leaf_packages=("qt6-base-examples", "qt6-wayland"),
        exec_path=(
            "/usr/lib/aarch64-linux-gnu/qt6/examples/widgets/widgets/"
            "analogclock/analogclock"
        ),
        note="QtWidgets window; run with QT_QPA_PLATFORM=wayland. qt6-wayland leaf "
        "carries the libqwayland-generic.so QPA plugin (dlopen'd, forced 0o755).",
    ),
    # SDL2 bouncing-sprites demo — opens a window and renders sprites each frame.
    # libSDL2 has the Wayland video driver compiled in (SDL_VIDEODRIVER=wayland).
    "sdl2gui": GuiDemo(
        name="sdl2gui",
        leaf_packages=("libsdl2-2.0-0", "libsdl2-tests"),
        exec_path="/usr/libexec/installed-tests/SDL2/testsprite2",
        note="SDL2 window demo; run with SDL_VIDEODRIVER=wayland. icon.bmp ships "
        "in the same dir (leaf files kept entirely).",
    ),
}


@dataclass(frozen=True)
class GuiDemoBuild:
    name: str
    out_tar: str
    exec_path: str
    file_count: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    violations: tuple[str, ...]
    so_made_exec: int          # .so members forced to 0o755 in the final tar
    exec_in_overlay: bool
    conformant: bool

    @property
    def ok(self) -> bool:
        return (
            self.exec_in_overlay
            and not self.violations
            and not self.missing_soname
            and self.conformant
        )


# --------------------------------------------------------------------------- #
# .so x-bit enforcement (PROT_EXEC dlopen) — applied to the produced tar
# --------------------------------------------------------------------------- #

def force_so_executable(tar_path: str | Path) -> int:
    """Rewrite ``tar_path`` in place so every ``.so`` regular-file member is 0o755.

    ALR's file-backed ``PROT_EXEC`` dlopen under ``untrusted_app`` REJECTS a
    non-executable ``.so`` (STAGE_TAR_SPEC §10.1, memory: ALR .so x-bit). Debian
    ships dlopen'd plugins (Qt QPA platform plugins, SDL drivers) as 0644, and
    ``build_stage_tar`` preserves the deb mode — so without this they'd land
    non-executable. We make the tar itself correct (RootfsInstaller also force-adds
    the bit on extract; this is belt-and-suspenders + a host-verifiable artifact).

    A member is treated as a shared object if its basename ends in ``.so`` or
    contains ``.so.`` (``libfoo.so``, ``libfoo.so.1``, ``libqwayland-generic.so``).
    Returns the count of members whose mode was raised to 0o755.
    """
    tar_path = Path(tar_path)
    members: list[tuple[tarfile.TarInfo, bytes | None]] = []
    changed = 0
    with tarfile.open(tar_path, "r:*") as tar:
        for ti in tar.getmembers():
            data = tar.extractfile(ti).read() if ti.isreg() else None
            members.append((ti, data))

    def _is_so(name: str) -> bool:
        base = PurePosixPath(name).name
        return base.endswith(".so") or ".so." in base

    with tarfile.open(tar_path, "w") as tar:
        for ti, data in members:
            if ti.isreg() and _is_so(ti.name) and (ti.mode & 0o777) != 0o755:
                ti.mode = 0o755
                changed += 1
            if data is not None:
                tar.addfile(ti, io.BytesIO(data))
            else:
                tar.addfile(ti)
    return changed


def overlay_has_exec(out_tar: str | Path, exec_path: str) -> bool:
    """True if ``out_tar`` contains ``exec_path`` as a ``./``-rooted member."""
    want = "./" + exec_path.lstrip("/")
    with tarfile.open(out_tar, "r:*") as tar:
        return want in set(tar.getnames())


# --------------------------------------------------------------------------- #
# GUI demo overlay build (network)
# --------------------------------------------------------------------------- #

def build_gui_demo_overlay(
    demo: GuiDemo,
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = DEFAULT_MIRROR,
    suite: str = DEFAULT_SUITE,
    arch: str = "arm64",
    components=DEFAULT_COMPONENTS,
    cache_dir: str | Path | None = DEFAULT_CACHE,
) -> GuiDemoBuild:
    """Build one window-opening GUI demo's §5-E DT_NEEDED-minimal overlay (NETWORK).

    Keeps the leaf packages' own files (the demo binary + dlopen'd platform plugin)
    plus the DT_NEEDED libs the base lacks, base-subtracted and flattened, then
    forces every ``.so`` to 0o755 and validates with overlay_guard + stage_tar_spec.
    """
    m = build_minimal_overlay(
        list(demo.leaf_packages),
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache_dir,
    )

    so_made_exec = force_so_executable(m["out_tar"])

    from tools.stage_tar_spec import validate_stage_tar

    rep = validate_stage_tar(m["out_tar"], base=base)

    return GuiDemoBuild(
        name=demo.name,
        out_tar=m["out_tar"],
        exec_path=demo.exec_path,
        file_count=m["file_count"],
        reachable_libs=tuple(m["reachable_libs"]),
        missing_soname=tuple(m["missing_soname"]),
        violations=tuple(m["violations"]),
        so_made_exec=so_made_exec,
        exec_in_overlay=overlay_has_exec(m["out_tar"], demo.exec_path),
        conformant=rep.conformant,
    )


# --------------------------------------------------------------------------- #
# apt/dpkg install overlay: a raw .deb in /root/ + apt config
# --------------------------------------------------------------------------- #

# The package the guest will install. `hello` is the canonical trivial GNU package
# (one ELF /usr/bin/hello + a couple of docs); it depends only on libc6, which the
# base provides. Its data member is zstd (guest dpkg handles that); we embed the
# .deb RAW — no host extraction — so the guest does the install.
APT_DEMO_PACKAGE = "hello"
APT_DEMO_DEB_DEST = "/root/hello.deb"

# A minimal offline apt config: point apt at /root for `apt-get install ./hello.deb`
# style local installs and disable signature checks for the local file path. We do
# NOT add a network mirror line (the guest may be offline); the value here is that
# `dpkg -i /root/hello.deb` and `apt-get install -y --no-download /root/hello.deb`
# can UNPACK the package against the reconstructed dpkg DB.
APT_DEMO_SOURCES = "# ALR apt demo: install the bundled local .deb, no network needed.\n"
APT_DEMO_CONF = (
    'APT::Get::AllowUnauthenticated "true";\n'
    'Acquire::AllowInsecureRepositories "true";\n'
    'APT::Sandbox::User "root";\n'
)
APT_DEMO_README = (
    "ALR apt/dpkg install demo\n"
    "=========================\n"
    "A bundled `hello` .deb sits at /root/hello.deb. With the reconstructed dpkg DB\n"
    "(var/lib/dpkg/status via build_dpkg_db) in place, the guest can UNPACK it:\n"
    "\n"
    "    dpkg --unpack /root/hello.deb        # extracts files, status -> 'unpacked'\n"
    "    # or:  apt-get install -y /root/hello.deb\n"
    "\n"
    "HONEST LIMIT: `dpkg --configure hello` (and a plain `dpkg -i`'s configure pass)\n"
    "runs the package's maintainer scripts via fork+exec. Maintainer-script exec is\n"
    "a separate, large ALR feature (exec re-entry through the loader/supervisor), so\n"
    "the *configure* step may not complete yet. UNPACK (files on disk) is what this\n"
    "overlay enables today.\n"
)


@dataclass(frozen=True)
class AptDemoBuild:
    out_tar: str
    deb_dest: str
    deb_bytes: int
    package: str
    version: str
    members: tuple[str, ...]
    conformant: bool
    violations: tuple[str, ...]

    @property
    def ok(self) -> bool:
        return self.deb_bytes > 0 and self.conformant and not self.violations


def build_apt_demo_overlay(
    base: str | Path,
    out_tar: str | Path,
    *,
    package: str = APT_DEMO_PACKAGE,
    deb_dest: str = APT_DEMO_DEB_DEST,
    mirror: str = DEFAULT_MIRROR,
    suite: str = DEFAULT_SUITE,
    arch: str = "arm64",
    components=DEFAULT_COMPONENTS,
    cache_dir: str | Path | None = DEFAULT_CACHE,
) -> AptDemoBuild:
    """Build the §5-E apt/dpkg-install overlay (NETWORK: fetches the demo .deb).

    Embeds the raw ``<package>`` .deb at ``deb_dest`` (default /root/hello.deb) plus
    a small apt config + a README documenting the unpack-vs-configure honesty. The
    .deb is copied byte-for-byte (NOT extracted) — the guest's own dpkg installs it.
    Pair this with the dpkg-DB overlay (build_dpkg_db) so the install sees the base
    as a proper prior install.
    """
    cache = Path(cache_dir) if cache_dir is not None else Path(DEFAULT_CACHE)
    cache.mkdir(parents=True, exist_ok=True)

    index = parse_packages(
        fetch_packages_index(mirror, suite, arch, components=components)
    )
    fields = index.get(package)
    if not fields or "Filename" not in fields:
        raise RuntimeError(f"{package} not in {suite}/{arch} index (no Filename)")
    deb_path = _download_deb(mirror, fields["Filename"], cache)
    deb_bytes = deb_path.read_bytes()
    version = fields.get("Version", "")

    out_path = Path(out_tar)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    members: list[str] = []
    with tarfile.open(out_path, "w") as tar:
        _add_bytes(tar, "./" + deb_dest.lstrip("/"), deb_bytes, mode=0o644)
        members.append("./" + deb_dest.lstrip("/"))
        for arc, body, mode in (
            ("./etc/apt/sources.list.d/alr-demo.list", APT_DEMO_SOURCES, 0o644),
            ("./etc/apt/apt.conf.d/99alr-demo", APT_DEMO_CONF, 0o644),
            ("./root/APT_DEMO_README.txt", APT_DEMO_README, 0o644),
        ):
            _add_bytes(tar, arc, body.encode("utf-8"), mode=mode)
            members.append(arc)

    from tools.overlay_guard import scan_overlay_violations, BLOCK
    from tools.stage_tar_spec import validate_stage_tar

    blocks = [
        v.render()
        for v in scan_overlay_violations(base, str(out_path))
        if v.severity == BLOCK
    ]
    rep = validate_stage_tar(str(out_path), base=base)

    return AptDemoBuild(
        out_tar=str(out_path),
        deb_dest=deb_dest,
        deb_bytes=len(deb_bytes),
        package=package,
        version=version,
        members=tuple(members),
        conformant=rep.conformant,
        violations=tuple(blocks),
    )


def _add_bytes(tar: tarfile.TarFile, arcname: str, data: bytes, *, mode: int) -> None:
    info = tarfile.TarInfo(arcname)
    info.size = len(data)
    info.mode = mode
    info.type = tarfile.REGTYPE
    info.mtime = 0
    tar.addfile(info, io.BytesIO(data))


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_gui_demo_overlays",
        description="Build window-opening Qt6/SDL2 GUI demo overlays + an apt/dpkg "
        "install overlay (+dpkg DB) from Ubuntu noble.",
    )
    parser.add_argument(
        "--target",
        action="append",
        dest="targets",
        choices=["qt6gui", "sdl2gui", "aptdemo", "dpkgdb"],
        help="what to build (repeatable; default: all four)",
    )
    parser.add_argument("--base", help="base rootfs tar|dir (soname/path subtract + guard)")
    parser.add_argument("--out-dir", default="/tmp", help="output dir (default: %(default)s)")
    parser.add_argument("--mirror", default=DEFAULT_MIRROR)
    parser.add_argument("--suite", default=DEFAULT_SUITE)
    parser.add_argument("--arch", default="arm64")
    parser.add_argument("--component", action="append", dest="components")
    parser.add_argument("--cache", default=DEFAULT_CACHE)
    parser.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.base:
        parser.error("--base is required (or use --selftest)")

    components = tuple(args.components) if args.components else DEFAULT_COMPONENTS
    targets = args.targets or ["qt6gui", "sdl2gui", "aptdemo", "dpkgdb"]
    out_dir = Path(args.out_dir)
    overall_ok = True

    for name in targets:
        if name in GUI_DEMOS:
            demo = GUI_DEMOS[name]
            out_tar = out_dir / f"{name}-stage.tar"
            b = build_gui_demo_overlay(
                demo, args.base, out_tar,
                mirror=args.mirror, suite=args.suite, arch=args.arch,
                components=components, cache_dir=args.cache,
            )
            print(f"[{name}] {b.out_tar}")
            print(f"    files:            {b.file_count}")
            print(f"    exec (window):    {b.exec_path}")
            print(f"    exec in overlay:  {'YES' if b.exec_in_overlay else 'NO — MISSING'}")
            print(f"    reachable libs:   {len(b.reachable_libs)} {list(b.reachable_libs)}")
            print(f"    .so forced 0o755: {b.so_made_exec}")
            if b.missing_soname:
                print(f"    MISSING sonames:  {list(b.missing_soname)}")
            print(f"    overlay_guard:    {'OK' if not b.violations else str(len(b.violations)) + ' violation(s)'}")
            for v in b.violations:
                print(f"      {v}")
            print(f"    stage_tar_spec:   {'CONFORMANT' if b.conformant else 'NON-CONFORMANT'}")
            print(f"    => {'PASS' if b.ok else 'FAIL'}")
            overall_ok = overall_ok and b.ok

        elif name == "aptdemo":
            out_tar = out_dir / "aptdemo-stage.tar"
            a = build_apt_demo_overlay(
                args.base, out_tar,
                mirror=args.mirror, suite=args.suite, arch=args.arch,
                components=components, cache_dir=args.cache,
            )
            print(f"[aptdemo] {a.out_tar}")
            print(f"    package:          {a.package} {a.version}")
            print(f"    .deb at:          {a.deb_dest} ({a.deb_bytes} bytes)")
            print(f"    members:          {list(a.members)}")
            print(f"    overlay_guard:    {'OK' if not a.violations else str(len(a.violations)) + ' BLOCK'}")
            print(f"    stage_tar_spec:   {'CONFORMANT' if a.conformant else 'NON-CONFORMANT'}")
            print(f"    => {'PASS' if a.ok else 'FAIL'}")
            overall_ok = overall_ok and a.ok

        elif name == "dpkgdb":
            from tools.build_dpkg_db import build_dpkg_db

            out_tar = out_dir / "dpkg-db-stage.tar"
            r = build_dpkg_db(
                args.base, out_tar,
                mirror=args.mirror, suite=args.suite, arch=args.arch,
                components=components, cache_dir=args.cache,
            )
            print(f"[dpkgdb] {r['out_tar']}")
            print(f"    packages attested:{r['packages']}")
            print(f"    base files mapped:{r['files_mapped']}")
            print(f"    status bytes:     {r['status_bytes']}")
            ok = r["conformant"] is not False
            print(f"    stage_tar_spec:   {'CONFORMANT' if r['conformant'] else 'NON-CONFORMANT'}")
            print(f"    => {'PASS' if ok else 'FAIL'}")
            overall_ok = overall_ok and ok

    return 0 if overall_ok else 1


# --------------------------------------------------------------------------- #
# selftest (OFFLINE)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- matrix invariants -------------------------------------------------
    check("two GUI demos defined (qt6gui, sdl2gui)",
          set(GUI_DEMOS) == {"qt6gui", "sdl2gui"})
    check("every demo exec_path is rootfs-absolute",
          all(d.exec_path.startswith("/") for d in GUI_DEMOS.values()))
    # qt6gui MUST carry qt6-wayland (the dlopen'd QPA platform plugin) AND the
    # examples package (the window binary) — both, or no window opens.
    qt = GUI_DEMOS["qt6gui"]
    check("qt6gui leaf set includes qt6-base-examples (window binary)",
          "qt6-base-examples" in qt.leaf_packages)
    check("qt6gui leaf set includes qt6-wayland (QPA platform plugin)",
          "qt6-wayland" in qt.leaf_packages)
    check("qt6gui exec is the analogclock window binary",
          qt.exec_path.endswith("/analogclock/analogclock"))
    sdl = GUI_DEMOS["sdl2gui"]
    check("sdl2gui leaf set includes libsdl2-tests (window demos)",
          "libsdl2-tests" in sdl.leaf_packages)
    check("sdl2gui exec is a windowing SDL2 demo (testsprite2)",
          sdl.exec_path.endswith("/SDL2/testsprite2"))

    # --- apt demo constants ------------------------------------------------
    check("apt demo package is `hello`", APT_DEMO_PACKAGE == "hello")
    check("apt demo .deb lands in /root", APT_DEMO_DEB_DEST.startswith("/root/"))
    check("apt demo README documents the unpack-vs-configure honesty",
          "UNPACK" in APT_DEMO_README and "configure" in APT_DEMO_README)

    # --- force_so_executable -----------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "t.tar"
        with tarfile.open(tar_path, "w") as t:
            for name, mode in (
                ("./usr/lib/qt6/plugins/platforms/libqwayland-generic.so", 0o644),
                ("./usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0", 0o644),
                ("./usr/bin/analogclock", 0o755),       # already exec, untouched
                ("./root/APT_DEMO_README.txt", 0o644),  # not a .so, untouched
            ):
                payload = b"\x7fELF" + name.encode()
                ti = tarfile.TarInfo(name)
                ti.size = len(payload)
                ti.mode = mode
                t.addfile(ti, io.BytesIO(payload))
        changed = force_so_executable(tar_path)
        check("force_so_executable raised exactly the 2 non-x .so members",
              changed == 2)
        with tarfile.open(tar_path, "r:*") as t:
            modes = {m.name: (m.mode & 0o777) for m in t.getmembers()}
        check("dlopen'd plugin .so is now 0o755",
              modes["./usr/lib/qt6/plugins/platforms/libqwayland-generic.so"] == 0o755)
        check("versioned lib .so.N is now 0o755",
              modes["./usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0"] == 0o755)
        check("already-exec binary mode preserved (0o755)",
              modes["./usr/bin/analogclock"] == 0o755)
        check("non-.so file mode untouched (0o644)",
              modes["./root/APT_DEMO_README.txt"] == 0o644)
        check("force_so_executable is idempotent (0 changes on 2nd pass)",
              force_so_executable(tar_path) == 0)
        # member bytes/count survive the rewrite
        with tarfile.open(tar_path, "r:*") as t:
            check("rewrite preserved member count", len(t.getmembers()) == 4)
            payload = t.extractfile(
                "./usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0"
            ).read()
            check("rewrite preserved member bytes", payload.startswith(b"\x7fELF"))

    # --- overlay_has_exec --------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "o.tar"
        with tarfile.open(tar_path, "w") as t:
            payload = b"\x7fELF"
            ti = tarfile.TarInfo("./" + qt.exec_path.lstrip("/"))
            ti.size = len(payload)
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))
        check("overlay_has_exec finds the analogclock window binary",
              overlay_has_exec(tar_path, qt.exec_path))
        check("overlay_has_exec is leading-slash agnostic",
              overlay_has_exec(tar_path, qt.exec_path.lstrip("/")))
        check("overlay_has_exec reports a missing binary as False",
              not overlay_has_exec(tar_path, "/usr/bin/nope"))

    # --- apt demo overlay assembly (offline: synthetic .deb + base) --------
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / "base.tar"
        with tarfile.open(base, "w") as t:
            ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libc.so.6")
            ti.size = 4
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(b"\x7fELF"))
        # exercise the tar-assembly half directly (no network) by faking the deb
        out = Path(tmp) / "aptdemo-stage.tar"
        fake_deb = b"!<arch>\nfake-deb-bytes"
        with tarfile.open(out, "w") as tar:
            _add_bytes(tar, "./root/hello.deb", fake_deb, mode=0o644)
            _add_bytes(tar, "./etc/apt/apt.conf.d/99alr-demo",
                       APT_DEMO_CONF.encode(), mode=0o644)
            _add_bytes(tar, "./root/APT_DEMO_README.txt",
                       APT_DEMO_README.encode(), mode=0o644)
        from tools.stage_tar_spec import validate_stage_tar
        from tools.overlay_guard import scan_overlay_violations, BLOCK

        rep = validate_stage_tar(str(out), base=str(base))
        blocks = [v for v in scan_overlay_violations(str(base), str(out))
                  if v.severity == BLOCK]
        check("apt demo overlay is §5-E CONFORMANT (no base downgrade)",
              rep.conformant)
        check("apt demo overlay has 0 BLOCK against base", blocks == [])
        with tarfile.open(out, "r:*") as t:
            names = set(t.getnames())
        check("apt demo overlay carries the raw .deb at /root/hello.deb",
              "./root/hello.deb" in names)
        check("apt demo overlay carries apt config",
              "./etc/apt/apt.conf.d/99alr-demo" in names)

    # --- build verdict aggregation -----------------------------------------
    good = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, ("liba",), (), (), 1, True, True)
    bad_missing = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), (), 1, False, True)
    bad_guard = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), ("BLOCK z",), 1, True, True)
    bad_soname = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), ("libz.so.9",), (), 1, True, True)
    bad_conf = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), (), 1, True, False)
    check("GuiDemoBuild.ok True when clean + exec present", good.ok)
    check("GuiDemoBuild.ok False when exec missing", not bad_missing.ok)
    check("GuiDemoBuild.ok False on guard violation", not bad_guard.ok)
    check("GuiDemoBuild.ok False on missing soname", not bad_soname.ok)
    check("GuiDemoBuild.ok False when non-conformant", not bad_conf.ok)

    apt_good = AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 100, "hello", "2.10", (), True, ())
    apt_bad = AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 0, "hello", "2.10", (), True, ())
    check("AptDemoBuild.ok True when deb present + conformant", apt_good.ok)
    check("AptDemoBuild.ok False when deb empty", not apt_bad.ok)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
