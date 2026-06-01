"""Build the babl/gegl GIMP-module overlay (babl-gegl-stage.tar) — WS-4 §10(a).

Why
---
GIMP 3.0.2 launches on the device (proven), but several filters / colour-space
conversions are driven by **babl** colour-model modules and **gegl** operation
modules that are *dlopen'd at runtime* from::

    /usr/lib/aarch64-linux-gnu/babl-0.1/*.so      (babl model/extension ops)
    /usr/lib/aarch64-linux-gnu/gegl-0.4/*.so       (gegl operation modules)

The base rootfs already SHIPS the full set (30 babl + 37 gegl ops, byte-identical
to Ubuntu noble's ``libbabl-0.1-0`` / ``libgegl-0.4-0t64``), but it ships them as
**0644 (non-executable) regular files**. ALR's file-backed ``PROT_EXEC`` dlopen
under ``untrusted_app`` **rejects a non-executable ``.so``** (the exact class of
fault that broke the gtk3 svg pixbuf loader — STAGE_TAR_SPEC §10.1, memory:
ALR .so x-bit). So on the device any GIMP filter that dlopen's a babl/gegl op can
fail to load it → the filter / conversion silently doesn't work.

``RootfsInstaller.extractEntry`` was taught (ws-4) to add the exec bit to ``*.so``
on base re-extraction, which covers these on the next cold start. This overlay is
the **belt-and-suspenders, marker-gated** delivery: it lands regardless of whether
the base is re-extracted (its own ``.babl-gegl-staged-<size>`` marker), shipping
every babl/gegl op module as a real **0o755** file so dlopen always succeeds.

What it ships (and deliberately does NOT)
-----------------------------------------
- SHIPS: every real ``.so`` under ``babl-0.1/`` and ``gegl-0.4/`` from Ubuntu noble
  ``libbabl-0.1-0`` + ``libgegl-0.4-0t64``, each as 0o755.
- DROPS the core SONAME libraries (``libbabl-0.1.so.0*``, ``libgegl-0.4.so.0*``,
  ``libgegl-npd-0.4.so``, ``libgegl-sc-0.4.so``): the base owns those as frozen
  flat SONAMEs (overlay_guard would BLOCK shipping a versioned ``libgegl-0.4.so.0.447.1``
  for a base flat ``libgegl-0.4.so.0``). The op modules are versionless ``*.so`` and
  are NOT SONAME-frozen, so overwriting them in place (same path, byte-identical,
  only the mode changes) is downgrade-safe.

The op modules sit at the SAME rootfs paths as the base, so this is a pure
"restore with the exec bit" overlay — overlay_guard 0 BLOCK (op .so are not
versioned sonames). Emits the §5-E ``./``-rooted tar consumed by a guarded
``extractOverlayTar`` slot in MainActivity (integration wires ``babl-gegl`` into
the toolkit-stage loop; no Kotlin change in this WS-4 change).

Noble source matches the base glibc 2.39 ABI (`libbabl-0.1-0` 0.1.108-1,
`libgegl-0.4-0t64` 0.4.48-2.4build2). Override --suite to pin another release.
"""

from __future__ import annotations

import argparse
import io
import shutil
import tarfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from tools.deb_closure import _download_deb, fetch_packages_index, parse_packages
from tools.build_stage_tar import extract_deb

# packages that carry the dlopen'd op modules (Ubuntu noble t64 names)
DEFAULT_PACKAGES = ("libbabl-0.1-0", "libgegl-0.4-0t64")
# the op-module directories we lift (relative, no leading ./). Anything under
# these whose basename ends in ".so" is a dlopen'd module → ship it 0o755.
OP_DIRS = (
    "usr/lib/aarch64-linux-gnu/babl-0.1",
    "usr/lib/aarch64-linux-gnu/gegl-0.4",
)


@dataclass(frozen=True)
class BablGeglOverlayResult:
    out_tar: str
    packages: tuple[str, ...]
    babl_ops: int
    gegl_ops: int
    total_ops: int
    op_names: tuple[str, ...]


def _norm(name: str) -> str:
    while name.startswith("./"):
        name = name[2:]
    return name.lstrip("/")


def _is_op_module(rel: str) -> bool:
    """True if ``rel`` is a dlopen'd babl/gegl op module (under an OP_DIR, *.so)."""
    rel = _norm(rel)
    if not rel.endswith(".so"):
        return False
    parent = PurePosixPath(rel).parent.as_posix()
    return parent in OP_DIRS


def build_babl_gegl_overlay(
    out_tar: str | Path,
    *,
    packages=DEFAULT_PACKAGES,
    mirror: str = "http://ports.ubuntu.com/ubuntu-ports",
    suite: str = "noble",
    arch: str = "arm64",
    components=("main", "universe"),
    cache_dir: str | Path | None = None,
    source_root: str | Path | None = None,
) -> BablGeglOverlayResult:
    """Build the §5-E babl/gegl op-module overlay tar (every op .so as 0o755).

    If ``source_root`` is given it is used as the already-extracted package root
    (offline/selftest). Otherwise each package in ``packages`` is downloaded from
    the mirror and extracted into one merged root.
    """
    out_tar = Path(out_tar)

    if source_root is not None:
        root = Path(source_root)
        staged_pkgs: tuple[str, ...] = tuple(packages)
    else:
        cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache-ubuntu")
        cache.mkdir(parents=True, exist_ok=True)
        index = parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )
        root = cache / "_babl-gegl-root"
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)
        staged_list: list[str] = []
        for pkg in packages:
            fields = index.get(pkg)
            if not fields or "Filename" not in fields:
                raise RuntimeError(f"{pkg} not found in {suite}/{arch} index")
            deb = _download_deb(mirror, fields["Filename"], cache)
            extract_deb(deb, root)
            staged_list.append(pkg)
        staged_pkgs = tuple(staged_list)

    # collect every op module under the OP_DIRS (real files only; the deb layout
    # ships them as plain regular files, no symlinks here).
    ops: list[tuple[str, Path]] = []
    for op_dir in OP_DIRS:
        d = root / op_dir
        if not d.is_dir():
            continue
        for path in sorted(d.iterdir()):
            rel = path.relative_to(root).as_posix()
            if path.is_file() and not path.is_symlink() and _is_op_module(rel):
                ops.append((rel, path))

    if not ops:
        raise RuntimeError(
            f"no babl/gegl op modules found under {OP_DIRS} in {root} "
            "(wrong package set or extraction failed?)"
        )

    babl = 0
    gegl = 0
    names: list[str] = []
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out_tar, "w") as tar:
        for rel, full in sorted(ops):
            data = full.read_bytes()
            ti = tarfile.TarInfo("./" + rel)
            ti.size = len(data)
            # 0o755: ALR file-backed PROT_EXEC dlopen rejects a non-x .so. The tar
            # itself carries the x bit so the device extractor lands it executable
            # (RootfsInstaller also force-adds it, but we make the tar correct too).
            ti.mode = 0o755
            tar.addfile(ti, io.BytesIO(data))
            base = PurePosixPath(rel).name
            names.append(base)
            if "/babl-0.1/" in "/" + rel:
                babl += 1
            elif "/gegl-0.4/" in "/" + rel:
                gegl += 1

    return BablGeglOverlayResult(
        out_tar=str(out_tar),
        packages=staged_pkgs,
        babl_ops=babl,
        gegl_ops=gegl,
        total_ops=len(ops),
        op_names=tuple(sorted(names)),
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_babl_gegl_overlay",
        description="Build babl-gegl-stage.tar (babl/gegl op modules, .so 0o755) "
        "from Ubuntu noble libbabl-0.1-0 + libgegl-0.4-0t64.",
    )
    parser.add_argument("--out", help="output tar (e.g. /tmp/babl-gegl-stage.tar)")
    parser.add_argument("--base", help="base rootfs tar|dir to run overlay_guard against")
    parser.add_argument("--package", action="append", dest="packages",
                        help="override package set (repeatable; default libbabl-0.1-0 + libgegl-0.4-0t64)")
    parser.add_argument("--suite", default="noble")
    parser.add_argument("--mirror", default="http://ports.ubuntu.com/ubuntu-ports")
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main + universe)")
    parser.add_argument("--cache", default="/tmp/deb-cache-ubuntu")
    parser.add_argument("--strict", action="store_true",
                        help="fail on overlay_guard WARN as well as BLOCK")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.out:
        parser.error("--out is required (or use --selftest)")

    packages = tuple(args.packages) if args.packages else DEFAULT_PACKAGES
    components = tuple(args.components) if args.components else ("main", "universe")
    res = build_babl_gegl_overlay(
        args.out, packages=packages, mirror=args.mirror, suite=args.suite,
        components=components, cache_dir=args.cache,
    )
    print(f"built {res.out_tar}: packages={res.packages} "
          f"babl_ops={res.babl_ops} gegl_ops={res.gegl_ops} total={res.total_ops}")
    rc = 0
    if args.base:
        from tools.overlay_guard import scan_overlay_violations, BLOCK, WARN
        from tools.stage_tar_spec import validate_stage_tar
        violations = scan_overlay_violations(args.base, res.out_tar)
        blocks = [v for v in violations if v.severity == BLOCK]
        warns = [v for v in violations if v.severity == WARN]
        for v in violations:
            print("  " + v.render())
        print(f"overlay_guard: {len(blocks)} BLOCK, {len(warns)} WARN")
        rep = validate_stage_tar(res.out_tar, base=args.base)
        print(f"stage_tar_spec: {'CONFORMANT' if rep.conformant else 'NON-CONFORMANT'} "
              f"errors={rep.errors}")
        rc = 1 if (blocks or (args.strict and warns) or not rep.conformant) else 0
    return rc


def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        # synthetic extracted package root: babl + gegl op dirs (0644, like a .deb),
        # plus the core SONAME libs that MUST be excluded, plus a doc file.
        root = tmp_path / "root"
        babl_d = root / "usr/lib/aarch64-linux-gnu/babl-0.1"
        gegl_d = root / "usr/lib/aarch64-linux-gnu/gegl-0.4"
        libdir = root / "usr/lib/aarch64-linux-gnu"
        babl_d.mkdir(parents=True)
        gegl_d.mkdir(parents=True)
        for n in ("CIE.so", "sse2-int8.so", "oklab.so"):
            (babl_d / n).write_bytes(b"\x7fELF babl " + n.encode())
            (babl_d / n).chmod(0o644)
        for n in ("exr-load.so", "gegl-common.so", "transformops.so"):
            (gegl_d / n).write_bytes(b"\x7fELF gegl " + n.encode())
            (gegl_d / n).chmod(0o644)
        # core SONAME libs (frozen in base) — must NOT appear in the overlay
        (libdir / "libbabl-0.1.so.0.207.1").write_bytes(b"core babl")
        (libdir / "libgegl-0.4.so.0.447.1").write_bytes(b"core gegl")
        (libdir / "libgegl-npd-0.4.so").write_bytes(b"npd")  # versionless but NOT under an op dir
        (root / "usr/share/doc/libgegl-common").mkdir(parents=True)
        (root / "usr/share/doc/libgegl-common/copyright").write_bytes(b"GPL")

        out = tmp_path / "babl-gegl-stage.tar"
        res = build_babl_gegl_overlay(out, source_root=root)

        with tarfile.open(out) as t:
            members = {m.name: m for m in t.getmembers()}

        check("staged 3 babl ops", res.babl_ops == 3)
        check("staged 3 gegl ops", res.gegl_ops == 3)
        check("total ops == 6", res.total_ops == 6)
        check("./-rooted babl op member present",
              "./usr/lib/aarch64-linux-gnu/babl-0.1/CIE.so" in members)
        check("./-rooted gegl op member present",
              "./usr/lib/aarch64-linux-gnu/gegl-0.4/exr-load.so" in members)
        check("EVERY op .so is 0o755 (x-bit for PROT_EXEC dlopen)",
              all((m.mode & 0o777) == 0o755 for n, m in members.items() if n.endswith(".so")))
        check("EVERY op .so is a real file (no symlinks)",
              all(m.isfile() for m in members.values()))
        check("core libbabl SONAME NOT shipped (base frozen)",
              not any("libbabl-0.1.so" in n for n in members))
        check("core libgegl SONAME NOT shipped (base frozen)",
              not any("libgegl-0.4.so" in n for n in members))
        check("libgegl-npd helper (not an op-dir module) NOT shipped",
              not any("libgegl-npd-0.4.so" in n for n in members))
        check("doc/copyright NOT shipped (only op modules)",
              not any("copyright" in n for n in members))

        # §5-E validator (structural, no base) + overlay_guard against a base that
        # owns the flat SONAMEs as real files: op .so are versionless → 0 BLOCK.
        from tools.stage_tar_spec import validate_stage_tar
        from tools.overlay_guard import scan_overlay_violations, BLOCK

        rep = validate_stage_tar(str(out))
        check("stage_tar_spec conformant (structural)", rep.conformant and not rep.errors)

        base = tmp_path / "base.tar"
        with tarfile.open(base, "w") as bt:
            # base ships the flat SONAME libs (frozen) + 0644 op modules
            for soname, payload in (("libbabl-0.1.so.0", b"base babl"),
                                    ("libgegl-0.4.so.0", b"base gegl")):
                ti = tarfile.TarInfo(f"./usr/lib/aarch64-linux-gnu/{soname}")
                ti.size = len(payload); ti.mode = 0o644
                bt.addfile(ti, io.BytesIO(payload))
            for opdir, name in (("babl-0.1", "CIE.so"), ("gegl-0.4", "exr-load.so")):
                ti = tarfile.TarInfo(f"./usr/lib/aarch64-linux-gnu/{opdir}/{name}")
                payload = b"\x7fELF old " + name.encode()
                ti.size = len(payload); ti.mode = 0o644
                bt.addfile(ti, io.BytesIO(payload))
        violations = scan_overlay_violations(base, str(out))
        blocks = [v for v in violations if v.severity == BLOCK]
        check("overlay_guard: 0 BLOCK against a base owning the flat SONAMEs",
              blocks == [])

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
