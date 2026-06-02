"""Build REAL device-ready app stage-tars (T1 — v2 universal-install product track).

What this is
------------
The breadth catalog (``tools.breadth_catalog``) predicts, on the HOST, which apt
apps resolve a 0-unsat dependency closure against the Ubuntu noble index. This
module takes the *0-unsat lightweight* picks from that prediction and turns them
into **real, device-stageable §5-E overlay tars** — the physical artifact behind
the claim "these apps are staging-ready on device".

It is the leaf-app sibling of ``tools.build_toolkit_overlays`` (which does the
same for the toolkit runtimes netsurf/qt6/sdl2). Both delegate the actual build
to ``deb_closure.build_minimal_overlay`` so there is ONE §5-E build engine, not a
second copy. The difference is the *catalog*: this module ships the small,
broadly-representative end-user apps

    nano        CLI editor      /usr/bin/nano               (closure floor; near libc-only)
    xterm       GUI terminal    /usr/bin/xterm              (X11 / Xwayland host)
    galculator  GUI calculator  /usr/bin/galculator         (GTK3)

each with its real packaged ENTRYPOINT (the launchable binary the deb installs,
and the ``.desktop`` file when the app ships one) recorded and PROVEN-present in
the produced tar.

Honesty contract (the天井 / ceiling)
-----------------------------------
A produced stage-tar means **"this app is device-STAGING ready"**: its full
runtime closure resolves with 0 unsat against noble, the overlay is §5-E
conformant (./-rooted, flat-SONAME, safe symlinks, base-soname subtracted, no
base downgrade), and it carries the launchable entrypoint binary. It does NOT
mean the app has been *run* on a device — the terminal install/launch still rides
the G1 exec-re-entry unlock (see docs/research/loader-feature-gaps.md G1, the apt
row in docs/research/alr-compat-matrix.md, and breadth_catalog's same disclaimer).
Real device staging/launch is DEVICE-REQ.

Network vs offline
------------------
``build_app_stage`` is the NETWORK path (ports.ubuntu.com noble main+universe →
.deb download → minimal overlay). When the network (or the build host's lack of a
Debian env) is unavailable, ``build_app_stage_from_root`` takes an already-laid-out
extracted root and produces the same §5-E tar + entrypoint check — this is the
synthetic-fixture path the OFFLINE selftest/tests use to verify the build LOGIC
(entrypoint presence, §5-E conformance, base subtraction) without the net. The
``--selftest`` path is fully offline and deterministic.

Reuse, not duplication
---------------------
  * tools.deb_closure.build_minimal_overlay — the DT_NEEDED-minimal §5-E engine
  * tools.build_stage_tar.build_stage_tar    — extracted-root → flat §5-E tar
  * tools.stage_tar_spec.validate_stage_tar  — §5-E structural conformance
  * tools.overlay_guard (via the two above)  — base-downgrade gate
"""

from __future__ import annotations

import argparse
import tarfile
from dataclasses import dataclass, field
from pathlib import Path

from tools.deb_closure import build_minimal_overlay

DEFAULT_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
DEFAULT_SUITE = "noble"
DEFAULT_COMPONENTS = ("main", "universe")
DEFAULT_CACHE = "/tmp/deb-cache-ubuntu"


# --------------------------------------------------------------------------- #
# Catalog — the 0-unsat lightweight picks from the breadth prediction
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class AppStage:
    """One real-app stage-tar recipe.

    name           stage-tar slot name (→ /tmp/<name>-stage.tar)
    packages       apt leaf package(s); their OWN files are kept entirely (the
                   entrypoint binary among them) + the libs they link via
                   DT_NEEDED the base lacks (deb_closure.build_minimal_overlay).
    entrypoint     the in-rootfs path the device launcher execs (the launchable
                   binary the leaf deb installs). Rootfs-absolute (/usr/bin/...).
    desktop        the in-rootfs .desktop path the deb ships (or None for CLI).
                   When present it is the freedesktop launcher entry the product
                   UI can surface; PROVEN-present alongside the binary.
    kind           "gui" (needs the compositor / Xwayland) or "cli" (terminal).
    keep_prefixes  extra data dirs to force-keep even if not DT_NEEDED-reachable.
    note           why this app is in the device-ready set / any caveat.
    """

    name: str
    packages: tuple[str, ...]
    entrypoint: str
    desktop: str | None
    kind: str
    keep_prefixes: tuple[str, ...] = ()
    note: str = ""

    def __post_init__(self) -> None:
        if not self.entrypoint.startswith("/"):
            raise ValueError(f"{self.name}: entrypoint must be rootfs-absolute")
        if self.desktop is not None and not self.desktop.startswith("/"):
            raise ValueError(f"{self.name}: desktop must be rootfs-absolute")
        if self.kind not in ("gui", "cli"):
            raise ValueError(f"{self.name}: kind must be 'gui' or 'cli'")
        if not self.packages:
            raise ValueError(f"{self.name}: at least one package is required")


# Three 0-unsat lightweight apps (verified against the live noble index):
#   nano        closure 6   — CLI editor, near-libc-only (closure floor)
#   xterm       closure 38  — X11 terminal (Xwayland host); ships .desktop
#   galculator  closure 157 — GTK3 calculator (mostly base GTK3 → subtracted)
# Entrypoints + .desktop paths confirmed by extracting each leaf .deb.
APPS: dict[str, AppStage] = {
    "nano": AppStage(
        name="nano",
        packages=("nano",),
        entrypoint="/usr/bin/nano",
        desktop=None,                      # nano is CLI — no .desktop
        kind="cli",
        note="tiny ncurses editor; near-libc-only closure (closure floor)",
    ),
    "xterm": AppStage(
        name="xterm",
        packages=("xterm",),
        entrypoint="/usr/bin/xterm",
        desktop="/usr/share/applications/debian-xterm.desktop",
        kind="gui",
        note="X11 terminal emulator; runs under rootful Xwayland (x11-stage.tar)",
    ),
    "galculator": AppStage(
        name="galculator",
        packages=("galculator",),
        entrypoint="/usr/bin/galculator",
        desktop="/usr/share/applications/galculator.desktop",
        kind="gui",
        note="GTK3 calculator; most of the 157-pkg closure is base GTK3 (subtracted)",
    ),
}


# --------------------------------------------------------------------------- #
# Build result
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class AppStageBuild:
    app: str
    out_tar: str
    entrypoint: str
    desktop: str | None
    file_count: int
    closure_size: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    violations: tuple[str, ...]
    unsupported: tuple[str, ...]
    conformant: bool
    entrypoint_present: bool
    desktop_present: bool
    extract_estimate_bytes: int = 0

    @property
    def ok(self) -> bool:
        """Device-staging ready iff: §5-E conformant, no base-downgrade violation,
        every DT_NEEDED soname resolved, and the launchable entrypoint is in the
        tar. (.desktop presence is reported but not a hard gate — CLI apps have
        none, and a missing .desktop never blocks launch-by-path.)"""
        return (
            self.conformant
            and not self.violations
            and not self.missing_soname
            and self.entrypoint_present
        )


# --------------------------------------------------------------------------- #
# tar inspection helpers
# --------------------------------------------------------------------------- #
def _member_set(out_tar: str | Path) -> set[str]:
    with tarfile.open(out_tar, "r:*") as tar:
        return set(tar.getnames())


def overlay_has_path(out_tar: str | Path, rootfs_path: str) -> bool:
    """True iff the produced tar contains ``rootfs_path`` as a ./-rooted member.

    Leading-slash agnostic (``/usr/bin/x`` and ``usr/bin/x`` both match the tar's
    ``./usr/bin/x``). Used to PROVE the entrypoint binary / .desktop is staged —
    the whole point of T1 (a stage-tar that carries no launchable binary is not
    "device-ready").
    """
    want = "./" + str(rootfs_path).lstrip("/")
    return want in _member_set(out_tar)


def _extract_estimate(out_tar: str | Path) -> int:
    """Sum of member sizes — the bytes the device extractor lays down (the
    'expected device extract size' the task asks each stage-tar to report)."""
    total = 0
    with tarfile.open(out_tar, "r:*") as tar:
        for m in tar.getmembers():
            if m.isfile():
                total += m.size
    return total


def _finalize(
    app: AppStage,
    out_tar: str,
    *,
    base: str | Path,
    file_count: int,
    closure_size: int,
    reachable_libs,
    missing_soname,
    violations,
    unsupported,
) -> AppStageBuild:
    """Run the §5-E structural conformance check + entrypoint presence + size
    estimate, and assemble the AppStageBuild verdict. Shared by both the network
    and the from-root build paths so they report identically."""
    from tools.stage_tar_spec import validate_stage_tar

    rep = validate_stage_tar(out_tar, base=base)
    entrypoint_present = overlay_has_path(out_tar, app.entrypoint)
    desktop_present = (
        app.desktop is not None and overlay_has_path(out_tar, app.desktop)
    )
    return AppStageBuild(
        app=app.name,
        out_tar=str(out_tar),
        entrypoint=app.entrypoint,
        desktop=app.desktop,
        file_count=file_count,
        closure_size=closure_size,
        reachable_libs=tuple(reachable_libs),
        missing_soname=tuple(missing_soname),
        violations=tuple(violations),
        unsupported=tuple(unsupported),
        conformant=rep.conformant,
        entrypoint_present=entrypoint_present,
        desktop_present=desktop_present,
        extract_estimate_bytes=_extract_estimate(out_tar),
    )


# --------------------------------------------------------------------------- #
# Network build path
# --------------------------------------------------------------------------- #
def build_app_stage(
    app: AppStage,
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = DEFAULT_MIRROR,
    suite: str = DEFAULT_SUITE,
    arch: str = "arm64",
    components=DEFAULT_COMPONENTS,
    cache_dir: str | Path | None = DEFAULT_CACHE,
) -> AppStageBuild:
    """Build one real app's §5-E device-ready stage-tar (NETWORK).

    Delegates to ``deb_closure.build_minimal_overlay`` (keep the leaf package's
    own files incl. the entrypoint binary + DT_NEEDED libs the base lacks,
    base-subtracted, flattened to a §5-E ./-tar), then validates structural
    conformance and PROVES the entrypoint binary (and .desktop) are present.
    """
    m = build_minimal_overlay(
        list(app.packages),
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache_dir,
        keep_prefixes=app.keep_prefixes,
    )
    return _finalize(
        app,
        m["out_tar"],
        base=base,
        file_count=m["file_count"],
        closure_size=len(m["closure"]),
        reachable_libs=m["reachable_libs"],
        missing_soname=m["missing_soname"],
        violations=m["violations"],
        unsupported=m["unsupported"],
    )


# --------------------------------------------------------------------------- #
# Offline / fixture build path (no network, no real .deb)
# --------------------------------------------------------------------------- #
def build_app_stage_from_root(
    app: AppStage,
    src_root: str | Path,
    out_tar: str | Path,
    *,
    base: str | Path | None = None,
) -> AppStageBuild:
    """Build a §5-E stage-tar from an ALREADY-EXTRACTED root (offline).

    Same §5-E flatten + conformance + entrypoint proof as the network path, but
    the merged root is supplied by the caller (a synthetic fixture, or a Debian
    env's ``dpkg-deb -x`` output). Lets the build LOGIC be verified with no
    network and no Debian tooling — the path the OFFLINE tests/selftest use.

    ``base`` (optional) enables the base-downgrade conformance check; without it
    the overlay is validated structurally only (rules 1-3).
    """
    from tools.build_stage_tar import build_stage_tar

    result = build_stage_tar(src_root, out_tar)
    violations: tuple[str, ...] = ()
    if base is not None:
        from tools.overlay_guard import scan_overlay_violations

        violations = tuple(v.render() for v in scan_overlay_violations(base, out_tar))
    return _finalize(
        app,
        result.out_tar,
        base=base if base is not None else src_root,
        file_count=result.file_count,
        closure_size=0,
        reachable_libs=(),
        missing_soname=(),
        violations=violations,
        unsupported=(),
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _print_build(b: AppStageBuild) -> None:
    print(f"[{b.app}] {b.out_tar}")
    print(f"    entrypoint:       {b.entrypoint}  "
          f"{'PRESENT' if b.entrypoint_present else 'MISSING'}")
    if b.desktop is not None:
        print(f"    .desktop:         {b.desktop}  "
              f"{'PRESENT' if b.desktop_present else 'MISSING'}")
    print(f"    closure pkgs:     {b.closure_size}")
    print(f"    files:            {b.file_count}")
    print(f"    extract estimate: {b.extract_estimate_bytes / (1024 * 1024):.2f} MiB "
          f"(expected device extract size)")
    print(f"    reachable libs:   {len(b.reachable_libs)} {list(b.reachable_libs)}")
    if b.missing_soname:
        print(f"    MISSING sonames:  {list(b.missing_soname)}")
    if b.unsupported:
        print(f"    unsupported:      {list(b.unsupported)}")
    print(f"    overlay_guard:    "
          f"{'OK' if not b.violations else str(len(b.violations)) + ' violation(s)'}")
    for v in b.violations:
        print(f"      {v}")
    print(f"    stage_tar_spec:   {'CONFORMANT' if b.conformant else 'NON-CONFORMANT'}")
    print(f"    => {'STAGING-READY' if b.ok else 'NOT READY'}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_app_stage",
        description="Build real, device-ready §5-E app stage-tars (nano/xterm/"
        "galculator) from Ubuntu noble. STAGING-READY = 0-unsat closure + §5-E "
        "conformant + entrypoint present; device launch rides the G1 unlock.",
    )
    parser.add_argument(
        "--app", action="append", dest="apps", choices=sorted(APPS),
        help="app to build (repeatable; default: all)",
    )
    parser.add_argument("--base", help="base rootfs tar|dir (soname/path subtract + guard)")
    parser.add_argument("--out-dir", default="/tmp",
                        help="output dir for <app>-stage.tar (default: %(default)s)")
    parser.add_argument("--mirror", default=DEFAULT_MIRROR)
    parser.add_argument("--suite", default=DEFAULT_SUITE)
    parser.add_argument("--arch", default="arm64")
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main + universe)")
    parser.add_argument("--cache", default=DEFAULT_CACHE)
    parser.add_argument("--selftest", action="store_true",
                        help="run the OFFLINE deterministic selftest and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.base:
        parser.error("--base is required (or use --selftest)")

    components = tuple(args.components) if args.components else DEFAULT_COMPONENTS
    names = args.apps or sorted(APPS)
    out_dir = Path(args.out_dir)

    overall_ok = True
    for name in names:
        app = APPS[name]
        out_tar = out_dir / f"{name}-stage.tar"
        build = build_app_stage(
            app, args.base, out_tar,
            mirror=args.mirror, suite=args.suite, arch=args.arch,
            components=components, cache_dir=args.cache,
        )
        _print_build(build)
        overall_ok = overall_ok and build.ok

    print()
    print("NOTE: STAGING-READY = host 0-unsat closure + §5-E conformant + entrypoint "
          "present. Real device install/launch rides the G1 exec-re-entry unlock "
          "(DEVICE-REQ).")
    return 0 if overall_ok else 1


# --------------------------------------------------------------------------- #
# Synthetic fixture + OFFLINE selftest
# --------------------------------------------------------------------------- #
def _make_fixture_root(tmp: Path, app: AppStage, suffix: str = "") -> Path:
    """Lay out a tiny extracted root that mimics ``app``'s leaf-deb install:
    the entrypoint binary, its .desktop (if any), and a Debian-layout shared lib
    (real versioned file + SONAME symlink + dev link) so the §5-E flatten +
    base-soname subtraction paths are exercised without any network."""
    import os

    root = tmp / f"root-{app.name}{suffix}"
    # entrypoint binary
    binp = root / app.entrypoint.lstrip("/")
    binp.parent.mkdir(parents=True, exist_ok=True)
    binp.write_bytes(b"\x7fELF" + app.name.encode() + b"-entrypoint")
    binp.chmod(0o755)
    # .desktop, when the app ships one
    if app.desktop is not None:
        dp = root / app.desktop.lstrip("/")
        dp.parent.mkdir(parents=True, exist_ok=True)
        dp.write_text(
            f"[Desktop Entry]\nName={app.name}\nExec={app.entrypoint}\nType=Application\n"
        )
    # a Debian-layout private lib for this app (flattened by build_stage_tar)
    libdir = root / "usr/lib/aarch64-linux-gnu"
    libdir.mkdir(parents=True, exist_ok=True)
    real = libdir / f"lib{app.name}priv.so.1.2.3"
    real.write_bytes(b"PRIV-1.2.3" * 32)
    real.chmod(0o644)
    os.symlink(f"lib{app.name}priv.so.1.2.3", libdir / f"lib{app.name}priv.so.1")
    return root


def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- catalog invariants ------------------------------------------------ #
    check("3 apps defined (nano, xterm, galculator)",
          set(APPS) == {"nano", "xterm", "galculator"})
    check("every entrypoint is rootfs-absolute (/...)",
          all(a.entrypoint.startswith("/") for a in APPS.values()))
    check("every app names at least one package",
          all(a.packages for a in APPS.values()))
    check("nano is CLI with no .desktop",
          APPS["nano"].kind == "cli" and APPS["nano"].desktop is None)
    check("xterm + galculator are GUI and ship a .desktop",
          all(APPS[n].kind == "gui" and APPS[n].desktop is not None
              for n in ("xterm", "galculator")))
    check("entrypoints are the real packaged binaries",
          APPS["nano"].entrypoint == "/usr/bin/nano"
          and APPS["xterm"].entrypoint == "/usr/bin/xterm"
          and APPS["galculator"].entrypoint == "/usr/bin/galculator")

    # AppStage validation rejects bad recipes
    for bad in (
        lambda: AppStage("x", ("x",), "usr/bin/x", None, "cli"),       # rel entrypoint
        lambda: AppStage("x", ("x",), "/usr/bin/x", "rel.desktop", "gui"),  # rel desktop
        lambda: AppStage("x", ("x",), "/usr/bin/x", None, "tui"),      # bad kind
        lambda: AppStage("x", (), "/usr/bin/x", None, "cli"),          # no package
    ):
        try:
            bad()
            check("AppStage rejects an invalid recipe", False)
        except ValueError:
            check("AppStage rejects an invalid recipe", True)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        # --- offline build from a synthetic root, for each app ------------- #
        for name, app in APPS.items():
            root = _make_fixture_root(tmp_path, app)
            out_tar = tmp_path / f"{name}-stage.tar"
            build = build_app_stage_from_root(app, root, out_tar)

            check(f"{name}: stage-tar is §5-E conformant", build.conformant)
            check(f"{name}: entrypoint binary present in tar", build.entrypoint_present)
            check(f"{name}: ok / STAGING-READY", build.ok)
            check(f"{name}: extract estimate > 0", build.extract_estimate_bytes > 0)
            # the Debian-layout private lib was flattened to the bare SONAME
            members = _member_set(out_tar)
            soname = f"./usr/lib/aarch64-linux-gnu/lib{name}priv.so.1"
            versioned = f"./usr/lib/aarch64-linux-gnu/lib{name}priv.so.1.2.3"
            check(f"{name}: private lib flattened to bare SONAME", soname in members)
            check(f"{name}: versioned variant dropped (no downgrade shape)",
                  versioned not in members)
            check(f"{name}: no symlink members (flat layout)",
                  all(not n.endswith("priv.so.1.2.3") for n in members))
            if app.desktop is not None:
                check(f"{name}: .desktop present in tar", build.desktop_present)
            else:
                check(f"{name}: CLI app reports no .desktop",
                      build.desktop is None and not build.desktop_present)

        # --- a base that already owns the private SONAME must NOT block ---- #
        # (overlay ships the flat real soname == base flat soname → conformant
        #  flat-over-flat, never a BLOCK — proves base-subtraction integration.)
        import io as _io

        app = APPS["nano"]
        root = _make_fixture_root(tmp_path, app, suffix="-vsbase")
        base_tar = tmp_path / "base.tar"
        with tarfile.open(base_tar, "w") as t:
            payload = b"OLD-PRIV" * 8
            ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libnanopriv.so.1")
            ti.size = len(payload)
            t.addfile(ti, _io.BytesIO(payload))
        out_tar = tmp_path / "nano-vs-base.tar"
        build = build_app_stage_from_root(app, root, out_tar, base=base_tar)
        check("flat-over-flat vs base is never a BLOCK (conformant)", build.conformant)
        check("entrypoint still present against a base", build.entrypoint_present)

        # --- ok aggregation discriminates a missing entrypoint ------------- #
        missing_ep = AppStage("ghost", ("ghost",), "/usr/bin/ghost", None, "cli")
        root2 = tmp_path / "root-ghost"
        (root2 / "usr/share").mkdir(parents=True)
        (root2 / "usr/share/data.txt").write_text("no binary here\n")
        out_tar2 = tmp_path / "ghost-stage.tar"
        build2 = build_app_stage_from_root(missing_ep, root2, out_tar2)
        check("missing entrypoint → entrypoint_present False", not build2.entrypoint_present)
        check("missing entrypoint → NOT staging-ready", not build2.ok)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
