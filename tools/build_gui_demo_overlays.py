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

    name           stage-tar slot (→ /tmp/<name>-stage.tar, launch-loop key)
    leaf_packages  packages kept entirely (the demo binary + the dlopen'd platform
                   plugin live among these; their DT_NEEDED libs the base lacks are
                   added). For Qt the plugin packages (qt6-wayland) MUST be leaves so
                   the dlopen'd QPA platform plugin + its sibling shell/decoration/
                   graphics-integration plugins (none of which are DT_NEEDED by the
                   demo binary) survive. ``build_minimal_overlay`` then BFS-walks
                   DT_NEEDED *from every leaf ELF* (the demo AND each plugin .so), so
                   the plugins' own private deps (libQt6WaylandClient.so.6, …) are
                   pulled in even though nothing in the demo binary links them.
    exec_path      the in-rootfs binary the launch session runs to OPEN A WINDOW
    require_plugin_dirs  plugin sub-dirs (under .../qt6/plugins/) that MUST be present
                   and non-empty in the built overlay — the dlopen'd closure a Qt
                   wayland window needs (platforms, shell-integration, …). The QPA
                   platform plugin is dlopen'd by name, so its absence is a SILENT
                   init SIGSEGV, not a link error — we assert it structurally here.
    require_sonames  SONAMEs that MUST be reachable (in the overlay or the base) — the
                   private libs the plugins pull in; a gap here is the exact "plugin
                   can't find libQt6WaylandClient → crash" failure mode.
    inject_machine_id  write an /etc/machine-id (+ /var/lib/dbus/machine-id) into the
                   overlay. Qt6 Gui touches the D-Bus session bus at init; the noble
                   base ships libdbus but NO machine-id, and a missing machine-id can
                   abort the bus handshake. A synthetic id makes that path graceful.
    note           free-form recipe note.
    """

    name: str
    leaf_packages: tuple[str, ...]
    exec_path: str
    require_plugin_dirs: tuple[str, ...] = ()
    require_sonames: tuple[str, ...] = ()
    inject_machine_id: bool = False
    note: str = ""


GUI_DEMOS: dict[str, GuiDemo] = {
    # Qt6 Widgets analog clock — a top-level QWidget RASTER window (wl_shm only, no
    # GL needed, which matches the ALR compositor). qt6-base-examples ships the
    # prebuilt ELF; qt6-wayland is the leaf that carries the COMPLETE dlopen'd Wayland
    # QPA closure: the platform plugins (platforms/libqwayland-generic.so — the SHM
    # path — and platforms/libqwayland-egl.so), the shell-integration plugins
    # (wayland-shell-integration/libxdg-shell.so &c), the decoration-client plugin
    # (wayland-decoration-client/libbradient.so) and the graphics-integration-client
    # plugins. None of these are DT_NEEDED by analogclock — they are dlopen'd at QPA
    # init — so qt6-wayland being a LEAF (its files kept entirely) is what makes them
    # survive, and build_minimal_overlay BFS-walking DT_NEEDED *from each plugin .so*
    # is what pulls their private deps (libQt6WaylandClient.so.6 / libQt6Gui /
    # libwayland-* / libxkbcommon) into the overlay. The round-4 SIGSEGV(rendered=false)
    # was investigated host-side: the closure built by this recipe is ALREADY complete
    # (35 reachable libs, missing_soname=0, every plugin .so forced 0o755, guard+spec
    # OK) — so the remaining init crash is a RUNTIME platform/compositor concern (EGL
    # platform selection or a compositor global), not a missing overlay file. We assert
    # the closure here so a future overlay regression can never re-introduce a real gap.
    "qt6gui": GuiDemo(
        name="qt6gui",
        leaf_packages=("qt6-base-examples", "qt6-wayland"),
        exec_path=(
            "/usr/lib/aarch64-linux-gnu/qt6/examples/widgets/widgets/"
            "analogclock/analogclock"
        ),
        # The dlopen'd-plugin closure a Qt wayland window needs. platforms = the QPA
        # plugin (dlopen by name → silent crash if absent); shell-integration =
        # xdg-shell (the surface role the ALR compositor speaks).
        require_plugin_dirs=(
            "platforms",
            "wayland-shell-integration",
            "wayland-decoration-client",
            "wayland-graphics-integration-client",
        ),
        # The private libs those plugins pull in. A gap here is the classic
        # "QPA plugin loaded but libQt6WaylandClient unresolved → SIGSEGV".
        require_sonames=(
            "libQt6WaylandClient.so.6",
            "libQt6Gui.so.6",
            "libQt6Widgets.so.6",
            "libQt6Core.so.6",
            "libwayland-client.so.0",
        ),
        inject_machine_id=True,
        note="QtWidgets RASTER window; run with QT_QPA_PLATFORM=wayland (the generic "
        "SHM platform — NOT wayland-egl, the ALR compositor is wl_shm-only). qt6-wayland "
        "leaf carries the full dlopen'd QPA plugin closure (all forced 0o755). "
        "/etc/machine-id injected so Qt's D-Bus init is graceful (base has none).",
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
    closure: ClosureCheck | None = None     # dlopen'd-plugin closure verdict (Qt-style)
    machine_id_added: tuple[str, ...] = ()   # D-Bus machine-id members injected

    @property
    def ok(self) -> bool:
        return (
            self.exec_in_overlay
            and not self.violations
            and not self.missing_soname
            and self.conformant
            and (self.closure is None or self.closure.ok)
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


# A fixed, syntactically-valid D-Bus machine-id (32 lowercase hex chars + newline).
# Qt6 Gui pokes the D-Bus session bus at init; the noble base ships libdbus but no
# /etc/machine-id, and dbus_get_local_machine_id() failing can abort the handshake.
# A static id is fine for a single-tenant guest (it is not a security boundary here).
_ALR_MACHINE_ID = "a1b2c3d4e5f60718293a4b5c6d7e8f90"


def inject_machine_id(tar_path: str | Path) -> list[str]:
    """Append /etc/machine-id (+ /var/lib/dbus/machine-id) to ``tar_path`` if absent.

    Both are the canonical D-Bus machine-id locations; dbus reads /var/lib/dbus first
    then /etc. We add whichever the tar does not already carry. Returns the list of
    members added (``./``-rooted, deterministic). No-op if both already present.
    """
    tar_path = Path(tar_path)
    body = (_ALR_MACHINE_ID + "\n").encode("ascii")
    wanted = ("./etc/machine-id", "./var/lib/dbus/machine-id")
    with tarfile.open(tar_path, "r:*") as tar:
        have = set(tar.getnames())
    to_add = [m for m in wanted if m not in have]
    if not to_add:
        return []
    with tarfile.open(tar_path, "a") as tar:
        for arc in to_add:
            _add_bytes(tar, arc, body, mode=0o644)
    return to_add


@dataclass(frozen=True)
class ClosureCheck:
    """Structural verdict on a Qt-style dlopen'd-plugin overlay closure."""

    plugin_dirs_present: tuple[str, ...]
    plugin_dirs_missing: tuple[str, ...]
    sonames_reachable: tuple[str, ...]
    sonames_unreachable: tuple[str, ...]
    plugins_non_exec: tuple[str, ...]   # plugin .so members NOT 0o755 (dlopen would fail)

    @property
    def ok(self) -> bool:
        return not (
            self.plugin_dirs_missing or self.sonames_unreachable or self.plugins_non_exec
        )


def check_plugin_closure(
    out_tar: str | Path,
    base: str | Path,
    *,
    require_plugin_dirs: tuple[str, ...],
    require_sonames: tuple[str, ...],
) -> ClosureCheck:
    """Assert the dlopen'd-plugin closure a Qt wayland window needs is COMPLETE.

    DT_NEEDED validation (overlay_guard / missing_soname) only covers what the demo
    binary *links*. A Qt QPA platform plugin is dlopen'd by name at init, and it in
    turn dlopens shell/decoration/graphics-integration plugins — none of which are in
    any DT_NEEDED graph. A gap there is a SILENT init SIGSEGV, exactly the round-4
    failure mode. This checks, structurally against the produced tar + the base:

      * every ``require_plugin_dirs`` entry exists under .../qt6/plugins/ with ≥1 .so;
      * every ``require_sonames`` is satisfied either by an overlay member (flat or
        versioned) OR by the base (base libs win and are not re-shipped);
      * every plugin .so under .../qt6/plugins/ is 0o755 (ALR file-backed PROT_EXEC
        dlopen rejects a non-x .so — a non-exec QPA plugin = no window).
    """
    from tools.deb_closure import base_soname_set
    from tools.overlay_guard import parse_solib

    with tarfile.open(out_tar, "r:*") as tar:
        members = [(m.name, m.mode & 0o777, m.isreg()) for m in tar.getmembers()]

    plugin_marker = "/qt6/plugins/"
    # plugin dirs present (those with ≥1 .so under .../qt6/plugins/<dir>/)
    present: set[str] = set()
    plugins_non_exec: list[str] = []
    overlay_sonames: set[str] = set()
    for name, mode, isreg in members:
        base_name = PurePosixPath(name).name
        lib = parse_solib(base_name)
        if lib is not None:
            overlay_sonames.add(lib.soname)
        # also record bare flat soname spelling (build_stage_tar may flatten)
        if base_name.endswith(".so") or ".so." in base_name:
            overlay_sonames.add(base_name)
        if plugin_marker in name and name.endswith(".so"):
            after = name.split(plugin_marker, 1)[1]
            top = after.split("/", 1)[0]
            present.add(top)
            if isreg and mode != 0o755:
                plugins_non_exec.append(name)

    dirs_present = tuple(d for d in require_plugin_dirs if d in present)
    dirs_missing = tuple(d for d in require_plugin_dirs if d not in present)

    base_sonames = base_soname_set(base)
    reachable: list[str] = []
    unreachable: list[str] = []
    for so in require_sonames:
        if so in base_sonames or so in overlay_sonames:
            reachable.append(so)
        else:
            unreachable.append(so)

    return ClosureCheck(
        plugin_dirs_present=dirs_present,
        plugin_dirs_missing=dirs_missing,
        sonames_reachable=tuple(reachable),
        sonames_unreachable=tuple(unreachable),
        plugins_non_exec=tuple(sorted(plugins_non_exec)),
    )


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

    # Force every dlopen'd .so (QPA platform + shell/decoration/graphics plugins)
    # to 0o755 BEFORE we inject extra members, so the count reflects the real libs.
    so_made_exec = force_so_executable(m["out_tar"])

    # Qt6 Gui touches the D-Bus session bus at init; the noble base has no machine-id.
    machine_id_added = (
        tuple(inject_machine_id(m["out_tar"])) if demo.inject_machine_id else ()
    )

    # Structurally assert the dlopen'd-plugin closure is complete (the round-4
    # SIGSEGV class: a QPA platform plugin that loads but can't resolve its own
    # private libs, or a non-executable plugin .so). Skipped for demos that declare
    # no plugin closure (e.g. sdl2gui — its drivers are compiled into libSDL2).
    closure: ClosureCheck | None = None
    if demo.require_plugin_dirs or demo.require_sonames:
        closure = check_plugin_closure(
            m["out_tar"], base,
            require_plugin_dirs=demo.require_plugin_dirs,
            require_sonames=demo.require_sonames,
        )

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
        closure=closure,
        machine_id_added=machine_id_added,
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
            if b.machine_id_added:
                print(f"    machine-id:       injected {list(b.machine_id_added)}")
            if b.missing_soname:
                print(f"    MISSING sonames:  {list(b.missing_soname)}")
            print(f"    overlay_guard:    {'OK' if not b.violations else str(len(b.violations)) + ' violation(s)'}")
            for v in b.violations:
                print(f"      {v}")
            print(f"    stage_tar_spec:   {'CONFORMANT' if b.conformant else 'NON-CONFORMANT'}")
            if b.closure is not None:
                c = b.closure
                print(f"    plugin dirs:      {'OK ' + str(list(c.plugin_dirs_present)) if not c.plugin_dirs_missing else 'MISSING ' + str(list(c.plugin_dirs_missing))}")
                print(f"    plugin sonames:   {'OK (' + str(len(c.sonames_reachable)) + ' reachable)' if not c.sonames_unreachable else 'UNREACHABLE ' + str(list(c.sonames_unreachable))}")
                if c.plugins_non_exec:
                    print(f"    NON-EXEC plugins: {list(c.plugins_non_exec)}")
                print(f"    plugin closure:   {'OK' if c.ok else 'INCOMPLETE'}")
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
    # qt6gui declares the dlopen'd-plugin closure a Qt wayland window needs — the
    # platforms QPA plugin + shell-integration (xdg-shell) at minimum — and the
    # private libs those plugins pull in (libQt6WaylandClient &c). These are NOT in
    # any DT_NEEDED graph, so without an explicit assertion an overlay regression
    # could silently drop them (the round-4 SIGSEGV class).
    check("qt6gui requires the platforms QPA plugin dir",
          "platforms" in qt.require_plugin_dirs)
    check("qt6gui requires the wayland-shell-integration plugin dir",
          "wayland-shell-integration" in qt.require_plugin_dirs)
    check("qt6gui requires libQt6WaylandClient.so.6 reachable",
          "libQt6WaylandClient.so.6" in qt.require_sonames)
    check("qt6gui requires libQt6Widgets/Gui/Core reachable",
          {"libQt6Widgets.so.6", "libQt6Gui.so.6", "libQt6Core.so.6"}
          <= set(qt.require_sonames))
    check("qt6gui injects a D-Bus machine-id (base ships none)",
          qt.inject_machine_id is True)
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

    # --- inject_machine_id -------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "mi.tar"
        with tarfile.open(tar_path, "w") as t:
            payload = b"\x7fELF"
            ti = tarfile.TarInfo("./usr/bin/x")
            ti.size = len(payload)
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))
        added = inject_machine_id(tar_path)
        check("inject_machine_id adds both machine-id locations",
              set(added) == {"./etc/machine-id", "./var/lib/dbus/machine-id"})
        with tarfile.open(tar_path, "r:*") as t:
            names = set(t.getnames())
            mid = t.extractfile("./etc/machine-id").read().decode().strip()
        check("machine-id member is present after inject",
              "./etc/machine-id" in names and "./var/lib/dbus/machine-id" in names)
        check("machine-id body is 32 lowercase hex chars",
              len(mid) == 32 and all(c in "0123456789abcdef" for c in mid))
        check("inject_machine_id is idempotent (no dup on 2nd pass)",
              inject_machine_id(tar_path) == [])

    # --- check_plugin_closure (offline: synthetic overlay + base) ----------
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / "base.tar"
        with tarfile.open(base, "w") as t:
            # base owns libwayland-client.so.0 (provided, not re-shipped by overlay)
            ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libwayland-client.so.0")
            ti.size = 4
            ti.mode = 0o755
            t.addfile(ti, io.BytesIO(b"\x7fELF"))

        # GOOD overlay: plugin dirs present, plugin .so 0o755, the one overlay soname
        # present, the base-provided soname satisfied by the base.
        good_tar = Path(tmp) / "good.tar"
        with tarfile.open(good_tar, "w") as t:
            for arc, mode in (
                ("./usr/lib/aarch64-linux-gnu/qt6/plugins/platforms/libqwayland-generic.so", 0o755),
                ("./usr/lib/aarch64-linux-gnu/qt6/plugins/wayland-shell-integration/libxdg-shell.so", 0o755),
                ("./usr/lib/aarch64-linux-gnu/libQt6WaylandClient.so.6", 0o755),
            ):
                payload = b"\x7fELF"
                ti = tarfile.TarInfo(arc)
                ti.size = len(payload)
                ti.mode = mode
                t.addfile(ti, io.BytesIO(payload))
        cc = check_plugin_closure(
            good_tar, base,
            require_plugin_dirs=("platforms", "wayland-shell-integration"),
            require_sonames=("libQt6WaylandClient.so.6", "libwayland-client.so.0"),
        )
        check("closure OK: plugin dirs present", not cc.plugin_dirs_missing)
        check("closure OK: overlay soname reachable",
              "libQt6WaylandClient.so.6" in cc.sonames_reachable)
        check("closure OK: base-provided soname counts as reachable",
              "libwayland-client.so.0" in cc.sonames_reachable)
        check("closure OK: no non-exec plugin", not cc.plugins_non_exec)
        check("ClosureCheck.ok True when complete", cc.ok)

        # BAD overlay: missing the shell-integration dir, plugin .so left 0o644,
        # a required private soname absent from both overlay and base.
        bad_tar = Path(tmp) / "bad.tar"
        with tarfile.open(bad_tar, "w") as t:
            ti = tarfile.TarInfo(
                "./usr/lib/aarch64-linux-gnu/qt6/plugins/platforms/libqwayland-generic.so")
            ti.size = 4
            ti.mode = 0o644            # NOT executable → dlopen would fail
            t.addfile(ti, io.BytesIO(b"\x7fELF"))
        cc_bad = check_plugin_closure(
            bad_tar, base,
            require_plugin_dirs=("platforms", "wayland-shell-integration"),
            require_sonames=("libQt6WaylandClient.so.6",),
        )
        check("closure BAD: shell-integration dir reported missing",
              "wayland-shell-integration" in cc_bad.plugin_dirs_missing)
        check("closure BAD: unresolved private soname reported",
              "libQt6WaylandClient.so.6" in cc_bad.sonames_unreachable)
        check("closure BAD: non-exec plugin .so reported",
              any(p.endswith("libqwayland-generic.so") for p in cc_bad.plugins_non_exec))
        check("ClosureCheck.ok False when incomplete", not cc_bad.ok)

    # --- build verdict aggregation -----------------------------------------
    good = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, ("liba",), (), (), 1, True, True)
    bad_missing = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), (), 1, False, True)
    bad_guard = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), ("BLOCK z",), 1, True, True)
    bad_soname = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), ("libz.so.9",), (), 1, True, True)
    bad_conf = GuiDemoBuild("x", "/tmp/x.tar", "/usr/bin/x", 3, (), (), (), 1, True, False)
    ok_closure = ClosureCheck(("platforms",), (), ("libQt6Core.so.6",), (), ())
    bad_closure = ClosureCheck((), ("platforms",), (), ("libQt6Core.so.6",), ())
    good_with_closure = GuiDemoBuild(
        "x", "/tmp/x.tar", "/usr/bin/x", 3, ("liba",), (), (), 1, True, True, ok_closure)
    bad_with_closure = GuiDemoBuild(
        "x", "/tmp/x.tar", "/usr/bin/x", 3, ("liba",), (), (), 1, True, True, bad_closure)
    check("GuiDemoBuild.ok True when clean + exec present", good.ok)
    check("GuiDemoBuild.ok False when exec missing", not bad_missing.ok)
    check("GuiDemoBuild.ok False on guard violation", not bad_guard.ok)
    check("GuiDemoBuild.ok False on missing soname", not bad_soname.ok)
    check("GuiDemoBuild.ok False when non-conformant", not bad_conf.ok)
    check("GuiDemoBuild.ok True when plugin closure is complete", good_with_closure.ok)
    check("GuiDemoBuild.ok False when plugin closure incomplete", not bad_with_closure.ok)

    apt_good = AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 100, "hello", "2.10", (), True, ())
    apt_bad = AptDemoBuild("/tmp/a.tar", "/root/hello.deb", 0, "hello", "2.10", (), True, ())
    check("AptDemoBuild.ok True when deb present + conformant", apt_good.ok)
    check("AptDemoBuild.ok False when deb empty", not apt_bad.ok)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
