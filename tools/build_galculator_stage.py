"""Build the galculator app stage-tar (galculator-stage.tar) — v2 breadth.

Why
---
v2 proved a NON-root ``dpkg -i hello.deb`` actually unpacks+configures+installs on
device (the exec-re-entry re-map + fakeroot + interpose chain). ``hello`` is a CLI
toy; this generalizes the SAME apt-install pipeline to a real end-user GUI app —
``galculator`` (a GTK3 calculator) — then launches the *installed* binary on the ALR
Wayland compositor. That is the v2 "real daily-driver app" proof: install via apt/dpkg
→ run on the in-app compositor.

What it ships
-------------
ONE §5-E ``./``-rooted overlay tar with two complementary payloads:

1. The galculator ``.deb`` itself, dropped at its real apt-cache path
   ``var/cache/apt/archives/galculator_<ver>_arm64.deb`` — the on-device ``dpkg -i``
   TARGET. galculator's entire runtime Depends (libc6, libglib2.0, libgtk-3-0t64,
   libpango — all base-provided GTK3) is already satisfied by the base rootfs, so a
   single ``dpkg -i galculator.deb`` installs cleanly with NO extra dep debs needed
   (exactly the ``hello.deb`` model: the closure is pre-satisfied, the deb is the
   lone install target). This is the apt-install PROOF.

2. The UNPACKED galculator closure — the leaf package's own files (the
   ``/usr/bin/galculator`` entrypoint + its ``.desktop`` + data) plus the ONLY shared
   libs galculator links via DT_NEEDED that the base does NOT already own — produced
   by :func:`tools.deb_closure.build_minimal_overlay` (base-SONAME-subtracted,
   §5-E-flattened, overlay_guard-clean). This is what lets the ALR compositor launch
   ``/usr/bin/galculator`` immediately, WITHOUT first having to run the on-device
   install (and is the runtime closure the installed copy would resolve against). For
   galculator the non-base set is ~empty (all GTK3 is base), so this is essentially
   just the entrypoint + .desktop — the render PROOF's launchable binary.

The two payloads are folded into ONE tar so a single ``adb push galculator-stage.tar``
+ one ``extractOverlayTar`` arms BOTH the install drain and the compositor launch.

Reuse, not duplication
---------------------
  * tools.deb_closure.build_minimal_overlay — the DT_NEEDED-minimal §5-E engine (the
    SAME one tools.build_app_stage / build_toolkit_overlays use). We do NOT re-implement
    closure resolution, base-subtraction, or §5-E flattening.
  * The galculator AppStage recipe (entrypoint + .desktop) is taken from
    tools.build_app_stage.APPS["galculator"] so there is one SSOT for the entrypoint.

Honest scope
------------
HOST build + verify. The actual on-device ``dpkg -i`` unpack/configure + the compositor
launch are the integration session's device drain (the v2 exec-re-entry re-map carries
the dpkg fork+exec children; see MEMORY v2/G1). This builder removes the "galculator
isn't staged" blocker and PROVES, host-side: 0-unsat closure, the .deb is at its cache
path, the entrypoint binary is in the overlay, §5-E conformant, overlay_guard-clean.
"""

from __future__ import annotations

import argparse
import io
import json
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.build_app_stage import APPS, overlay_has_path
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
class GalculatorStageResult:
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
        """Device-arm ready iff: the .deb is at its apt-cache path (install target),
        the entrypoint binary is in the overlay (compositor launch target), every
        DT_NEEDED soname resolved, and overlay_guard found no base-downgrade."""
        return (
            self.deb_present
            and self.entrypoint_present
            and not self.missing_soname
            and not [v for v in self.violations if "BLOCK" in v]
        )

    def as_dict(self) -> dict:
        return {
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


def _galculator_filename(index: dict[str, dict]) -> str:
    """The pool Filename of the galculator .deb from the noble index."""
    f = index.get("galculator", {}).get("Filename")
    if not f:
        raise RuntimeError("galculator not found in the noble Packages index")
    return f


def inject_deb_into_cache(
    out_tar: str | Path, deb: Path, *, archives_dir: str = APT_ARCHIVES
) -> tuple[str, int]:
    """Append ``deb`` into the overlay tar at ``<archives_dir>/<deb.name>`` (0o644,
    ``./``-rooted). Returns (member_name, bytes). Skips if already present.

    This is the on-device ``dpkg -i`` target. We append rather than rebuild so the
    closure overlay produced by build_minimal_overlay is untouched.
    """
    data = deb.read_bytes()
    name = "./" + f"{archives_dir.rstrip('/')}/{deb.name}"
    with tarfile.open(out_tar, "r") as t:
        if name in {m.name for m in t.getmembers()}:
            return name, len(data)
    with tarfile.open(out_tar, "a") as tar:
        ti = tarfile.TarInfo(name)
        ti.size = len(data)
        ti.mode = 0o644
        ti.mtime = 0
        ti.type = tarfile.REGTYPE
        tar.addfile(ti, io.BytesIO(data))
    return name, len(data)


def build_galculator_stage(
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = "/tmp/deb-cache-ubuntu",
) -> GalculatorStageResult:
    """Build galculator-stage.tar: unpacked base-subtracted closure (compositor
    launch) + the galculator .deb at its apt-cache path (dpkg -i install target).

    NETWORK PATH. Delegates closure resolution / base-subtraction / §5-E flatten to
    :func:`tools.deb_closure.build_minimal_overlay`, then folds the leaf .deb into the
    same tar's apt-cache slot. Reports an honest verdict (.deb present, entrypoint
    present, no unsat soname, overlay_guard-clean).
    """
    app = APPS["galculator"]
    cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache-ubuntu")

    # (1) unpacked, base-subtracted galculator closure → the §5-E overlay tar.
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

    # (2) fold the galculator .deb into var/cache/apt/archives/ (dpkg -i target).
    index = parse_packages(
        fetch_packages_index(mirror, suite, arch, components=components)
    )
    deb = _download_deb(mirror, _galculator_filename(index), cache)
    deb_name, deb_bytes = inject_deb_into_cache(out_tar, deb)

    # verify both payloads landed. inject returns a ./-rooted member name; strip the
    # leading ./ before handing it to overlay_has_path (which re-prefixes ./), so the
    # deb-present check matches the actual member (a double ./ would false-negative).
    entrypoint_present = overlay_has_path(out_tar, app.entrypoint)
    desktop_present = app.desktop is not None and overlay_has_path(out_tar, app.desktop)
    deb_present = overlay_has_path(out_tar, deb_name.lstrip("."))
    with tarfile.open(out_tar, "r") as t:
        file_count = sum(1 for x in t.getmembers() if x.isfile())

    return GalculatorStageResult(
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
def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="build_galculator_stage",
        description="Build galculator-stage.tar: the galculator .deb at its apt-cache "
        "path (dpkg -i target) + the unpacked base-subtracted closure (compositor "
        "launch target). Ubuntu noble main+universe.",
    )
    p.add_argument("--out", help="output stage tar (e.g. /tmp/galculator-stage.tar)")
    p.add_argument("--base", help="base rootfs tar|dir (drives base-subtraction + guard)")
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
    if not args.out:
        p.error("--out is required (or use --selftest)")
    if not args.base:
        p.error("--base is required (drives base-subtraction + overlay_guard)")

    components = tuple(args.components) if args.components else COMPONENTS
    res = build_galculator_stage(
        args.base, args.out, mirror=args.mirror, suite=args.suite, arch=args.arch,
        components=components, cache_dir=args.cache,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  galculator .deb:  {res.deb_cache_path}  "
              f"{'PRESENT' if res.deb_present else 'MISSING'}  ({res.deb_bytes} bytes)")
        print(f"  entrypoint:       {res.entrypoint}  "
              f"{'PRESENT' if res.entrypoint_present else 'MISSING'}")
        if res.desktop is not None:
            print(f"  .desktop:         {res.desktop}  "
                  f"{'PRESENT' if res.desktop_present else 'MISSING'}")
        print(f"  closure pkgs:     {res.closure_size}")
        print(f"  files in overlay: {res.file_count}")
        print(f"  non-base libs:    {len(res.reachable_libs)} {list(res.reachable_libs)}")
        if res.missing_soname:
            print(f"  MISSING sonames:  {list(res.missing_soname)}")
        if res.unsupported:
            print(f"  unsupported:      {list(res.unsupported)}")
        if res.violations:
            print(f"  overlay_guard VIOLATIONS — {len(res.violations)}:")
            for v in res.violations:
                print(f"    - {v}")
        print(f"  => {'ARM-READY' if res.ok else 'NOT READY'}")
    return 0 if res.ok else 1


# --------------------------------------------------------------------------- #
# OFFLINE selftest — synthetic deb-cache injection (no network)
# --------------------------------------------------------------------------- #
def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # catalog SSOT: galculator recipe is the one from build_app_stage
    app = APPS["galculator"]
    check("galculator entrypoint is /usr/bin/galculator",
          app.entrypoint == "/usr/bin/galculator")
    check("galculator ships a .desktop", app.desktop is not None)
    check("galculator is a GUI app", app.kind == "gui")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # a minimal §5-E-ish overlay tar (the closure-overlay stand-in) + a fake deb
        overlay = tmp / "galculator-stage.tar"
        with tarfile.open(overlay, "w") as t:
            payload = b"\x7fELF galculator-entrypoint"
            ti = tarfile.TarInfo("./usr/bin/galculator")
            ti.size = len(payload); ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))
        fake_deb = tmp / "galculator_2.1.4-1.2build2_arm64.deb"
        fake_deb.write_bytes(b"!<arch>\n" + b"deb-bytes" * 64)

        name, n = inject_deb_into_cache(overlay, fake_deb)
        check("inject returns the apt-cache member name",
              name == "./var/cache/apt/archives/galculator_2.1.4-1.2build2_arm64.deb")
        check("inject reports the deb byte count", n == len(fake_deb.read_bytes()))
        check("overlay now has the .deb at its apt-cache path",
              overlay_has_path(overlay, "var/cache/apt/archives/galculator_2.1.4-1.2build2_arm64.deb"))
        # the returned ./-rooted name, with the leading . stripped, must match too
        # (the verdict path uses name.lstrip('.') — a regression guard for the
        # double-./ false-negative that build_galculator_stage's deb_present check hit)
        check("deb-present verdict path (./-stripped) matches the member",
              overlay_has_path(overlay, name.lstrip(".")))
        check("overlay still has the entrypoint binary",
              overlay_has_path(overlay, "/usr/bin/galculator"))

        # idempotent: re-injecting the same deb does not duplicate the member
        inject_deb_into_cache(overlay, fake_deb)
        with tarfile.open(overlay, "r") as t:
            cnt = sum(1 for m in t.getmembers()
                      if m.name.endswith("galculator_2.1.4-1.2build2_arm64.deb"))
        check("re-inject is idempotent (one .deb member)", cnt == 1)

        # the injected member is a regular 0o644 file with the deb bytes intact
        with tarfile.open(overlay, "r") as t:
            m = t.getmember("./var/cache/apt/archives/galculator_2.1.4-1.2build2_arm64.deb")
            body = t.extractfile(m).read()
        check("injected .deb is a regular file", m.isfile())
        check("injected .deb mode is 0o644", m.mode == 0o644)
        check("injected .deb bytes round-trip", body == fake_deb.read_bytes())

        # §5-E structural conformance of the resulting tar
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(overlay))
        check("resulting overlay is stage_tar_spec conformant", rep.conformant)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
