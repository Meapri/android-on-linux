"""Build a REAL "apt-installable AND launchable" app stage-tar — v2 breadth, generalized.

Why
---
v2 proved a NON-root ``dpkg -i <pkg>.deb`` actually unpacks+configures+installs on
device (the exec-re-entry re-map + fakeroot + interpose chain), then *launches the
installed binary* on the ALR compositor/terminal. Two apps carry that end-to-end
proof today: ``hello`` (CLI toy, its .deb rides apt-dpkg-stage.tar) and
``galculator`` (GTK3 GUI, its .deb + closure ride galculator-stage.tar). This module
GENERALIZES the galculator staging recipe — *closure overlay PLUS the leaf .deb
dropped at its apt-cache path* — to any app in the :mod:`tools.build_app_stage`
catalog, so the next device batch can install+run a wider set (htop, nano, xterm, …)
with no new staging code per app.

It is the install-AND-launch sibling of :mod:`tools.build_app_stage`. ``build_app_stage``
ships ONLY the unpacked base-subtracted closure — enough to *launch by path*, but with
no ``.deb`` for an on-device ``dpkg -i`` (no apt-install proof). This module folds the
SAME closure overlay together with the leaf ``.deb`` at
``var/cache/apt/archives/<pkg>_<ver>_<arch>.deb`` (the on-device ``dpkg -i`` TARGET) so
ONE ``adb push <app>-stage.tar`` + one ``extractOverlayTar`` arms BOTH the apt-install
drain and the compositor/terminal launch — exactly the galculator-stage shape.

What it ships (per app)
-----------------------
ONE §5-E ``./``-rooted overlay tar with two complementary payloads:

1. The leaf ``.deb`` at its real apt-cache path ``var/cache/apt/archives/<deb>`` — the
   on-device ``dpkg -i`` install TARGET. The app's full runtime closure is resolved
   0-unsat against the noble index and (for these lightweight picks) is satisfied by
   the base rootfs, so a single ``dpkg -i <pkg>.deb`` installs cleanly.

2. The UNPACKED, base-subtracted closure — the leaf's own files (the entrypoint binary
   + ``.desktop`` + data) plus ONLY the DT_NEEDED libs the base lacks — produced by
   :func:`tools.deb_closure.build_minimal_overlay`. This is the compositor/terminal
   launch payload (and the runtime closure the installed copy resolves against).

Reuse, not duplication
----------------------
  * tools.build_app_stage.APPS            — the entrypoint/.desktop SSOT catalog.
  * tools.deb_closure.build_minimal_overlay — the DT_NEEDED-minimal §5-E engine.
  * tools.build_galculator_stage.inject_deb_into_cache — the apt-cache .deb injector
    (the SAME one the galculator builder uses; one SSOT for the cache-path shape).
  * tools.stage_tar_spec.validate_stage_tar / tools.overlay_guard — §5-E + base-gate.

So galculator-stage.tar and (e.g.) htop-stage.tar are produced by the IDENTICAL code
path; ``build_galculator_stage`` remains as the named entrypoint the device drain wires
(MainActivity.aptDrainTargetFor knows "galculator"), and the other apps are built here.

Honest scope / the ceiling
---------------------------
HOST build + verify. A produced stage-tar means **the app is device-STAGING ready**:
its closure resolves 0-unsat, the overlay is §5-E conformant (./-rooted, flat-SONAME,
base-soname subtracted, no base downgrade), the ``.deb`` is at its apt-cache path, and
the launchable entrypoint binary is present. It does NOT mean the app has been *run* on
a device — the on-device ``dpkg -i`` unpack/configure + launch ride the G1
exec-re-entry unlock, AND the device drain only auto-fires for packages
``MainActivity.aptDrainTargetFor`` recognizes (today: galculator, hello). Wiring a new
app's drain target is a one-line addition to that (device/native) code — tracked as
DEVICE-REQ, never claimed here. See docs/research/alr-compat-matrix.md (apt row) and
docs/research/loader-feature-gaps.md G1.
"""

from __future__ import annotations

import argparse
import json
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.build_app_stage import APPS, AppStage, overlay_has_path
from tools.build_galculator_stage import inject_deb_into_cache
from tools.deb_closure import (
    _download_deb,
    build_minimal_overlay,
    fetch_packages_index,
    parse_packages,
)

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main", "universe")

# The apt-cache directory a fresh `apt-get install` / `dpkg -i` reads local debs from.
APT_ARCHIVES = "var/cache/apt/archives"


@dataclass
class InstallStageResult:
    """Verdict for one app's install-AND-launch stage-tar (galculator-stage shape)."""

    app: str
    out_tar: str
    deb_name: str
    deb_cache_path: str
    deb_bytes: int
    entrypoint: str
    entrypoint_present: bool
    desktop: str | None
    desktop_present: bool
    deb_present: bool
    closure_size: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    unsupported: tuple[str, ...]
    violations: tuple[str, ...]
    file_count: int

    @property
    def ok(self) -> bool:
        """Device-arm ready iff: the .deb is at its apt-cache path (the dpkg -i
        install target), the entrypoint binary is in the overlay (the launch
        target), every DT_NEEDED soname resolved, and overlay_guard found no
        base-downgrade BLOCK."""
        return (
            self.deb_present
            and self.entrypoint_present
            and not self.missing_soname
            and not [v for v in self.violations if "BLOCK" in v]
        )

    def as_dict(self) -> dict:
        return {
            "app": self.app,
            "out_tar": self.out_tar,
            "deb_name": self.deb_name,
            "deb_cache_path": self.deb_cache_path,
            "deb_bytes": self.deb_bytes,
            "entrypoint": self.entrypoint,
            "entrypoint_present": self.entrypoint_present,
            "desktop": self.desktop,
            "desktop_present": self.desktop_present,
            "deb_present": self.deb_present,
            "closure_size": self.closure_size,
            "reachable_libs": list(self.reachable_libs),
            "missing_soname": list(self.missing_soname),
            "unsupported": list(self.unsupported),
            "violations": list(self.violations),
            "file_count": self.file_count,
            "ok": self.ok,
        }


def _leaf_filename(index: dict[str, dict], pkg: str) -> str:
    """The pool Filename of ``pkg``'s .deb from the noble index."""
    f = index.get(pkg, {}).get("Filename")
    if not f:
        raise RuntimeError(f"{pkg} not found in the noble Packages index")
    return f


def build_install_stage(
    app: AppStage,
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = "/tmp/deb-cache-ubuntu",
) -> InstallStageResult:
    """Build ``<app>-stage.tar``: unpacked base-subtracted closure (compositor/
    terminal launch) + the leaf .deb at its apt-cache path (dpkg -i install target).

    NETWORK PATH. Identical code path to :func:`tools.build_galculator_stage.
    build_galculator_stage`, just parametrized over any catalog :class:`AppStage`.
    Delegates closure resolution / base-subtraction / §5-E flatten to
    :func:`tools.deb_closure.build_minimal_overlay`, then folds the leaf .deb into the
    same tar's apt-cache slot via the shared ``inject_deb_into_cache``. Reports an
    honest verdict (.deb present, entrypoint present, no unsat soname, guard-clean).
    """
    cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache-ubuntu")

    # (1) unpacked, base-subtracted closure → the §5-E overlay tar.
    m = build_minimal_overlay(
        list(app.packages),
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache,
        keep_prefixes=app.keep_prefixes,
    )

    # (2) fold the leaf .deb into var/cache/apt/archives/ (dpkg -i target). We use
    # the FIRST package as the install leaf (matches AppStage's single-leaf apps).
    index = parse_packages(
        fetch_packages_index(mirror, suite, arch, components=components)
    )
    deb = _download_deb(mirror, _leaf_filename(index, app.packages[0]), cache)
    deb_name, deb_bytes = inject_deb_into_cache(out_tar, deb)

    entrypoint_present = overlay_has_path(out_tar, app.entrypoint)
    desktop_present = app.desktop is not None and overlay_has_path(out_tar, app.desktop)
    deb_present = overlay_has_path(out_tar, deb_name.lstrip("."))
    with tarfile.open(out_tar, "r") as t:
        file_count = sum(1 for x in t.getmembers() if x.isfile())

    return InstallStageResult(
        app=app.name,
        out_tar=str(out_tar),
        deb_name=deb.name,
        deb_cache_path=deb_name.lstrip("."),
        deb_bytes=deb_bytes,
        entrypoint=app.entrypoint,
        entrypoint_present=entrypoint_present,
        desktop=app.desktop,
        desktop_present=desktop_present,
        deb_present=deb_present,
        closure_size=len(m["closure"]),
        reachable_libs=tuple(m["reachable_libs"]),
        missing_soname=tuple(m["missing_soname"]),
        unsupported=tuple(m["unsupported"]),
        violations=tuple(m["violations"]),
        file_count=file_count,
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _print_result(res: InstallStageResult) -> None:
    print(f"[{res.app}] {res.out_tar}")
    print(f"    .deb (dpkg -i):   {res.deb_cache_path}  "
          f"{'PRESENT' if res.deb_present else 'MISSING'}  ({res.deb_bytes} bytes)")
    print(f"    entrypoint:       {res.entrypoint}  "
          f"{'PRESENT' if res.entrypoint_present else 'MISSING'}")
    if res.desktop is not None:
        print(f"    .desktop:         {res.desktop}  "
              f"{'PRESENT' if res.desktop_present else 'MISSING'}")
    print(f"    closure pkgs:     {res.closure_size}")
    print(f"    files in overlay: {res.file_count}")
    print(f"    non-base libs:    {len(res.reachable_libs)} {list(res.reachable_libs)}")
    if res.missing_soname:
        print(f"    MISSING sonames:  {list(res.missing_soname)}")
    if res.unsupported:
        print(f"    unsupported:      {list(res.unsupported)}")
    if res.violations:
        print(f"    overlay_guard VIOLATIONS — {len(res.violations)}:")
        for v in res.violations:
            print(f"      - {v}")
    print(f"    => {'ARM-READY' if res.ok else 'NOT READY'}")


def main(argv: list[str] | None = None) -> int:
    # galculator has its OWN named builder (the device drain wires it by name); the
    # rest of the catalog is buildable here. Default to the non-galculator apps.
    choices = sorted(n for n in APPS)
    p = argparse.ArgumentParser(
        prog="build_install_stage",
        description="Build install-AND-launch §5-E app stage-tars (galculator shape): "
        "the leaf .deb at its apt-cache path (dpkg -i target) + the unpacked "
        "base-subtracted closure (launch target). Ubuntu noble main+universe.",
    )
    p.add_argument("--app", action="append", dest="apps", choices=choices,
                   help="app to build (repeatable; default: htop, nano, xterm)")
    p.add_argument("--base", help="base rootfs tar|dir (drives base-subtraction + guard)")
    p.add_argument("--out-dir", default="/tmp",
                   help="output dir for <app>-stage.tar (default: %(default)s)")
    p.add_argument("--mirror", default=MIRROR)
    p.add_argument("--suite", default=SUITE)
    p.add_argument("--arch", default=ARCH)
    p.add_argument("--component", action="append", dest="components",
                   help="repo component (repeatable; default main + universe)")
    p.add_argument("--cache", default="/tmp/deb-cache-ubuntu")
    p.add_argument("--json", action="store_true")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.base:
        p.error("--base is required (drives base-subtraction + overlay_guard); or --selftest")

    components = tuple(args.components) if args.components else COMPONENTS
    # default set: the lightweight installable picks that are NOT galculator (which
    # has its own named builder for the device drain).
    names = args.apps or ["htop", "nano", "xterm"]
    out_dir = Path(args.out_dir)

    results = []
    overall_ok = True
    for name in names:
        app = APPS[name]
        out_tar = out_dir / f"{name}-stage.tar"
        res = build_install_stage(
            app, args.base, out_tar, mirror=args.mirror, suite=args.suite,
            arch=args.arch, components=components, cache_dir=args.cache,
        )
        results.append(res)
        overall_ok = overall_ok and res.ok
        if not args.json:
            _print_result(res)

    if args.json:
        print(json.dumps([r.as_dict() for r in results], indent=2))
    else:
        print()
        print("NOTE: ARM-READY = host 0-unsat closure + §5-E conformant + .deb at "
              "apt-cache path + entrypoint present. On-device dpkg -i + launch rides "
              "the G1 exec-re-entry unlock AND a per-app aptDrainTargetFor entry "
              "(DEVICE-REQ).")
    return 0 if overall_ok else 1


# --------------------------------------------------------------------------- #
# OFFLINE selftest — synthetic closure overlay + deb-cache injection (no network)
# --------------------------------------------------------------------------- #
def _selftest() -> int:
    import io
    import tempfile

    from tools.stage_tar_spec import validate_stage_tar

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # the builder is catalog-driven: every default app is a real catalog entry with
    # a rootfs-absolute entrypoint (the SSOT we generalize galculator's recipe over).
    for name in ("htop", "nano", "xterm"):
        check(f"{name} is in the build_app_stage catalog", name in APPS)
        check(f"{name} entrypoint is rootfs-absolute", APPS[name].entrypoint.startswith("/"))
    check("htop entrypoint is /usr/bin/htop", APPS["htop"].entrypoint == "/usr/bin/htop")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # stand in for build_minimal_overlay's output: a §5-E overlay carrying the
        # htop entrypoint, then inject a fake .deb at the apt-cache path and assert
        # the galculator-shape verdict (both payloads land, idempotent, conformant).
        overlay = tmp / "htop-stage.tar"
        with tarfile.open(overlay, "w") as t:
            payload = b"\x7fELF htop-entrypoint"
            ti = tarfile.TarInfo("./usr/bin/htop")
            ti.size = len(payload); ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))
        fake_deb = tmp / "htop_3.3.0-4build1_arm64.deb"
        fake_deb.write_bytes(b"!<arch>\n" + b"deb" * 64)

        name, n = inject_deb_into_cache(overlay, fake_deb)
        check("inject returns the apt-cache member name",
              name == "./var/cache/apt/archives/htop_3.3.0-4build1_arm64.deb")
        check("inject reports the deb byte count", n == len(fake_deb.read_bytes()))
        check("overlay now has the .deb at its apt-cache path",
              overlay_has_path(overlay, "var/cache/apt/archives/htop_3.3.0-4build1_arm64.deb"))
        check("deb-present verdict path (./-stripped) matches the member",
              overlay_has_path(overlay, name.lstrip(".")))
        check("overlay still has the entrypoint binary",
              overlay_has_path(overlay, "/usr/bin/htop"))

        inject_deb_into_cache(overlay, fake_deb)  # idempotent
        with tarfile.open(overlay, "r") as t:
            cnt = sum(1 for m in t.getmembers()
                      if m.name.endswith("htop_3.3.0-4build1_arm64.deb"))
        check("re-inject is idempotent (one .deb member)", cnt == 1)

        rep = validate_stage_tar(str(overlay))
        check("resulting overlay is stage_tar_spec conformant", rep.conformant)

        # the verdict's ok-gate discriminates a missing entrypoint (libs-only tar is
        # NOT arm-ready even with the .deb present).
        res_missing = InstallStageResult(
            app="ghost", out_tar=str(overlay), deb_name="ghost.deb",
            deb_cache_path="var/cache/apt/archives/ghost.deb", deb_bytes=10,
            entrypoint="/usr/bin/ghost", entrypoint_present=False, desktop=None,
            desktop_present=False, deb_present=True, closure_size=1,
            reachable_libs=(), missing_soname=(), unsupported=(), violations=(),
            file_count=1,
        )
        check("missing entrypoint → NOT arm-ready", not res_missing.ok)
        res_ok = InstallStageResult(
            app="htop", out_tar=str(overlay), deb_name="htop.deb",
            deb_cache_path="var/cache/apt/archives/htop.deb", deb_bytes=10,
            entrypoint="/usr/bin/htop", entrypoint_present=True, desktop=None,
            desktop_present=False, deb_present=True, closure_size=8,
            reachable_libs=(), missing_soname=(), unsupported=(), violations=(),
            file_count=2,
        )
        check("deb + entrypoint present, no unsat → arm-ready", res_ok.ok)
        check("a BLOCK violation flips arm-ready off",
              not InstallStageResult(
                  app="htop", out_tar="x", deb_name="d", deb_cache_path="p",
                  deb_bytes=1, entrypoint="/usr/bin/htop", entrypoint_present=True,
                  desktop=None, desktop_present=False, deb_present=True,
                  closure_size=1, reachable_libs=(), missing_soname=(),
                  unsupported=(), violations=("BLOCK libfoo.so.1 downgrade",),
                  file_count=1,
              ).ok)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
