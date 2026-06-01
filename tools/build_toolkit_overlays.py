"""Build the launchable toolkit overlays — netsurf / qt6 / sdl2 (WS-4 §10b/§11).

Why this exists
---------------
The first toolkit overlays (`STAGE_TAR_SPEC.md` §11/§15) were built from the
*library* leaf packages only:

    netsurf  ← netsurf-gtk        (ships /usr/bin/netsurf-gtk  — a real binary)
    qt6      ← qt6-wayland        (libQt6* + qtwayland PLUGINS — NO executable)
    sdl2     ← libsdl2-2.0-0      (libSDL2-2.0.so.0            — NO executable)

So the device toolkit-launch probe (`MainActivity.launchToolkitProbes`, owned by
the integration session) reported **qt6 / sdl2 "missing"** — the runtime libs were
staged but there was no binary to exec. `netsurf` actually DID ship its binary
(`/usr/bin/netsurf-gtk`, the probe's 2nd candidate); it only looked missing if the
overlay had not finished extracting.

The fix (this builder): add the package that ships a *display-free runnable binary*
to each toolkit's leaf set, so `deb_closure.build_minimal_overlay` keeps that binary
(leaf-package files are kept ENTIRELY) plus the libs it links via DT_NEEDED that the
base lacks. The base already provides libc/ld/libstdc++/libgcc/GTK3, so the overlays
stay small and downgrade-safe.

Leaf sets (Ubuntu noble, ports.ubuntu.com main+universe — the base is noble 2.39)
---------------------------------------------------------------------------------
| toolkit | leaf packages                       | runnable binary (in-rootfs path)            |
|---------|-------------------------------------|----------------------------------------------|
| netsurf | netsurf-gtk                         | /usr/bin/netsurf-gtk           (-v banner)   |
| qt6     | qt6-wayland + qt6-base-dev-tools    | /usr/lib/qt6/bin/qtpaths6      (--version)    |
|         |                                     |   (deb /usr/bin/qtpaths6 symlink is dropped) |
| sdl2    | libsdl2-2.0-0 + libsdl2-tests       | /usr/libexec/installed-tests/SDL2/testver    |
|         |                                     |   (also testplatform/testautomation, same dir)|

Each binary's DT_NEEDED (host-verified with tools.elf_needed):
- netsurf-gtk : base GTK3 closure + libcurl/libssh (overlay) + libc/ld (base)
- qtpaths6    : libQt6Core.so.6 (overlay) + libstdc++/libgcc/libc (base)  — `--version` is display-free
- testver     : libSDL2-2.0.so.0 (overlay) + libc/ld (base) — prints the compiled/linked
                SDL version + revision and exits 0, no window (testplatform is an
                alternative: prints platform/CPU/endian + "All 64bit instrinsic tests passed")

Output: §5-E `./`-rooted, flat-SONAME overlay tars (built by build_minimal_overlay
→ build_stage_tar), binaries 0o755 (deb mode preserved), base-subtracted by SONAME
(downgrade-frozen) and by path, validated against the base by overlay_guard +
stage_tar_spec. NETWORK build path (ports.ubuntu.com); `--selftest` is OFFLINE.

This module reuses, and does NOT duplicate:
  * tools.deb_closure.build_minimal_overlay — the DT_NEEDED-minimal §5-E build engine
  * tools.overlay_guard / tools.stage_tar_spec — the apply-time + structural gates
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from tools.deb_closure import build_minimal_overlay

DEFAULT_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
DEFAULT_SUITE = "noble"
DEFAULT_COMPONENTS = ("main", "universe")
DEFAULT_CACHE = "/tmp/deb-cache-ubuntu"


@dataclass(frozen=True)
class Toolkit:
    """One toolkit overlay recipe.

    name          stage-tar slot name (→ /tmp/<name>-stage.tar, MainActivity loop key)
    leaf_packages packages whose OWN files are kept entirely (binaries + plugins +
                  data); their DT_NEEDED-reachable libs the base lacks are added.
    exec_path     the in-rootfs path the integration session should probe/launch
                  (the runnable, display-free binary). For a symlinked binary this
                  is the convenient `/usr/bin` spelling; `real_exec_path` is the
                  target. The launch arg exits without a display.
    real_exec_path  the real ELF the symlink resolves to (None when exec_path is real)
    launch_arg    a display-free argument (e.g. --version) for the launch smoke
    ok_marker     a substring the binary prints on success (for the probe to assert)
    keep_prefixes data dirs to force-keep even if not DT_NEEDED-reachable
    """

    name: str
    leaf_packages: tuple[str, ...]
    exec_path: str
    real_exec_path: str | None
    launch_arg: str
    ok_marker: str
    keep_prefixes: tuple[str, ...] = ()
    note: str = ""


# The matrix. Order = easiest→hardest (matches STAGE_TAR_SPEC §5 M2 order).
TOOLKITS: dict[str, Toolkit] = {
    # netsurf already shipped its binary; keep it in the matrix so one builder
    # (re)produces all three consistently and reports the path the probe matches.
    "netsurf": Toolkit(
        name="netsurf",
        leaf_packages=("netsurf-gtk",),
        exec_path="/usr/bin/netsurf-gtk",
        real_exec_path=None,
        launch_arg="-v",
        ok_marker="NetSurf",
    ),
    # qt6-wayland gives the libs + qtwayland platform plugins; qt6-base-dev-tools
    # adds the runnable CLI qtpaths6 (links only libQt6Core + base libstdc++/libc;
    # `--version` prints the Qt version and exits with no display).
    # NOTE the deb's /usr/bin/qtpaths6 is a `../lib/qt6/bin/qtpaths6` symlink — that
    # ESCAPING (..) symlink is dropped by the §5-E safe-symlink rule, so the overlay
    # ships ONLY the real binary at /usr/lib/qt6/bin/qtpaths6. The integration probe
    # must point there (exec_path), not at /usr/bin/qtpaths6.
    "qt6": Toolkit(
        name="qt6",
        leaf_packages=("qt6-wayland", "qt6-base-dev-tools"),
        exec_path="/usr/lib/qt6/bin/qtpaths6",
        real_exec_path=None,
        launch_arg="--version",
        ok_marker="Qt",
        note="deb /usr/bin/qtpaths6 -> ../lib/qt6/bin/qtpaths6 symlink is dropped "
        "(escaping ..); probe the real /usr/lib/qt6/bin/qtpaths6",
    ),
    # libsdl2-2.0-0 is the runtime lib; libsdl2-tests adds the upstream SDL2 test
    # binaries. testver links only libSDL2 + base libc and prints the compiled/linked
    # SDL version + revision, then exits 0 without opening a window — a real SDL2
    # binary smoke that proves libSDL2 loads & initialises through the loader.
    "sdl2": Toolkit(
        name="sdl2",
        leaf_packages=("libsdl2-2.0-0", "libsdl2-tests"),
        exec_path="/usr/libexec/installed-tests/SDL2/testver",
        real_exec_path=None,
        launch_arg="",  # testver takes no args; prints SDL version info and exits 0
        ok_marker="SDL",  # prints "Compiled version: …" / "Linked version: …" + revision
    ),
}


@dataclass(frozen=True)
class ToolkitBuild:
    toolkit: str
    out_tar: str
    exec_path: str
    real_exec_path: str | None
    file_count: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    violations: tuple[str, ...]
    conformant: bool

    @property
    def ok(self) -> bool:
        return (
            not self.violations
            and not self.missing_soname
            and self.conformant
        )


def build_toolkit_overlay(
    toolkit: Toolkit,
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = DEFAULT_MIRROR,
    suite: str = DEFAULT_SUITE,
    arch: str = "arm64",
    components=DEFAULT_COMPONENTS,
    cache_dir: str | Path | None = DEFAULT_CACHE,
) -> ToolkitBuild:
    """Build one toolkit's §5-E DT_NEEDED-minimal overlay (NETWORK).

    Delegates to ``deb_closure.build_minimal_overlay`` — keeps the leaf packages'
    own files (the runnable binary among them) + the libs they link via DT_NEEDED
    that the base lacks, base-subtracted and flattened to a §5-E tar, then validated
    with overlay_guard + stage_tar_spec.
    """
    m = build_minimal_overlay(
        list(toolkit.leaf_packages),
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache_dir,
        keep_prefixes=toolkit.keep_prefixes,
    )

    # structural §5-E conformance (composes overlay_guard) for the final verdict.
    from tools.stage_tar_spec import validate_stage_tar

    rep = validate_stage_tar(m["out_tar"], base=base)

    return ToolkitBuild(
        toolkit=toolkit.name,
        out_tar=m["out_tar"],
        exec_path=toolkit.exec_path,
        real_exec_path=toolkit.real_exec_path,
        file_count=m["file_count"],
        reachable_libs=tuple(m["reachable_libs"]),
        missing_soname=tuple(m["missing_soname"]),
        violations=tuple(m["violations"]),
        conformant=rep.conformant,
    )


def overlay_has_exec(out_tar: str | Path, exec_path: str) -> bool:
    """True if ``out_tar`` contains the given in-rootfs exec path as a real member.

    Accepts both the convenient ``/usr/bin`` spelling and the real target — a tar
    member is matched ./-rooted, and a symlinked binary counts (its real target is
    in the same overlay). Used by the builder report + the host test to PROVE the
    overlay actually carries the launchable binary (the whole point of this WS-4
    pass: no more "missing" toolkit probe).
    """
    import tarfile

    want = "./" + exec_path.lstrip("/")
    with tarfile.open(out_tar, "r:*") as tar:
        names = set(tar.getnames())
    return want in names


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_toolkit_overlays",
        description="Build launchable netsurf/qt6/sdl2 §5-E overlays from Ubuntu "
        "noble (each carries a display-free runnable binary).",
    )
    parser.add_argument(
        "--toolkit",
        action="append",
        dest="toolkits",
        choices=sorted(TOOLKITS),
        help="toolkit to build (repeatable; default: all)",
    )
    parser.add_argument("--base", help="base rootfs tar|dir (for soname/path subtraction + guard)")
    parser.add_argument(
        "--out-dir",
        default="/tmp",
        help="output directory for <toolkit>-stage.tar (default: %(default)s)",
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
    names = args.toolkits or sorted(TOOLKITS)
    out_dir = Path(args.out_dir)

    overall_ok = True
    for name in names:
        tk = TOOLKITS[name]
        out_tar = out_dir / f"{name}-stage.tar"
        build = build_toolkit_overlay(
            tk, args.base, out_tar,
            mirror=args.mirror, suite=args.suite, arch=args.arch,
            components=components, cache_dir=args.cache,
        )
        has_bin = overlay_has_exec(build.out_tar, tk.exec_path)
        if tk.real_exec_path and not has_bin:
            has_bin = overlay_has_exec(build.out_tar, tk.real_exec_path)
        print(f"[{name}] {build.out_tar}")
        print(f"    files:           {build.file_count}")
        print(f"    exec (probe):    {build.exec_path}"
              + (f"  → {build.real_exec_path}" if build.real_exec_path else ""))
        print(f"    exec in overlay: {'YES' if has_bin else 'NO — MISSING'}")
        print(f"    reachable libs:  {len(build.reachable_libs)} {list(build.reachable_libs)}")
        if build.missing_soname:
            print(f"    MISSING sonames: {list(build.missing_soname)}")
        print(f"    overlay_guard:   {'OK' if not build.violations else str(len(build.violations)) + ' violation(s)'}")
        for v in build.violations:
            print(f"      {v}")
        print(f"    stage_tar_spec:  {'CONFORMANT' if build.conformant else 'NON-CONFORMANT'}")
        ok = build.ok and has_bin
        print(f"    => {'PASS' if ok else 'FAIL'}")
        overall_ok = overall_ok and ok

    return 0 if overall_ok else 1


def _selftest() -> int:
    """OFFLINE: validates the recipe matrix + overlay_has_exec, no network."""
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
    check("3 toolkits defined (netsurf, qt6, sdl2)",
          set(TOOLKITS) == {"netsurf", "qt6", "sdl2"})
    check("every toolkit names at least one leaf package",
          all(tk.leaf_packages for tk in TOOLKITS.values()))
    check("every exec_path is rootfs-absolute (/...)",
          all(tk.exec_path.startswith("/") for tk in TOOLKITS.values()))
    # qt6 must include the binary-shipping dev-tools package (the libs-only
    # qt6-wayland was exactly the "missing" cause).
    check("qt6 leaf set includes qt6-base-dev-tools (ships qtpaths6)",
          "qt6-base-dev-tools" in TOOLKITS["qt6"].leaf_packages)
    check("qt6 exec is the real /usr/lib/qt6/bin/qtpaths6 (deb /usr/bin symlink dropped)",
          TOOLKITS["qt6"].exec_path == "/usr/lib/qt6/bin/qtpaths6")
    # sdl2 must include libsdl2-tests (the runtime lib alone has no binary).
    check("sdl2 leaf set includes libsdl2-tests (ships test binaries)",
          "libsdl2-tests" in TOOLKITS["sdl2"].leaf_packages)
    check("sdl2 exec is an SDL2 installed-test binary",
          TOOLKITS["sdl2"].exec_path.endswith("/SDL2/testver"))
    check("netsurf exec is /usr/bin/netsurf-gtk",
          TOOLKITS["netsurf"].exec_path == "/usr/bin/netsurf-gtk")

    # --- overlay_has_exec --------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = Path(tmp) / "t.tar"
        with tarfile.open(tar_path, "w") as t:
            for member in (
                "./usr/bin/netsurf-gtk",
                "./usr/lib/qt6/bin/qtpaths6",
                "./usr/libexec/installed-tests/SDL2/testver",
            ):
                payload = b"\x7fELF" + member.encode()
                ti = tarfile.TarInfo(member)
                ti.size = len(payload)
                ti.mode = 0o755
                t.addfile(ti, io.BytesIO(payload))
        check("overlay_has_exec finds /usr/bin/netsurf-gtk",
              overlay_has_exec(tar_path, "/usr/bin/netsurf-gtk"))
        check("overlay_has_exec finds the real qtpaths6 target",
              overlay_has_exec(tar_path, "/usr/lib/qt6/bin/qtpaths6"))
        check("overlay_has_exec finds the SDL2 testver binary",
              overlay_has_exec(tar_path, "/usr/libexec/installed-tests/SDL2/testver"))
        check("overlay_has_exec reports a genuinely missing binary as False",
              not overlay_has_exec(tar_path, "/usr/bin/does-not-exist"))
        check("overlay_has_exec is leading-slash agnostic",
              overlay_has_exec(tar_path, "usr/bin/netsurf-gtk"))

    # ToolkitBuild.ok aggregation
    good = ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, ("liba",), (), (), True)
    bad_guard = ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, (), (), ("BLOCK foo",), True)
    bad_miss = ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, (), ("libz.so.9",), (), True)
    bad_conf = ToolkitBuild("x", "/tmp/x.tar", "/usr/bin/x", None, 3, (), (), (), False)
    check("ToolkitBuild.ok True when clean", good.ok)
    check("ToolkitBuild.ok False on guard violation", not bad_guard.ok)
    check("ToolkitBuild.ok False on missing soname", not bad_miss.ok)
    check("ToolkitBuild.ok False when non-conformant", not bad_conf.ok)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
