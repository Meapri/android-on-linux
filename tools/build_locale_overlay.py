"""Build a C.UTF-8 (and friends) locale overlay for the ALR rootfs (WS-4 §10a).

Why
---
The base rootfs ships glibc 2.39 and the guest env exports ``LC_ALL=C.UTF-8`` /
``LANG=C.UTF-8`` (WS-1, runtime_report.cpp), but the base has **no**
``/usr/lib/locale`` at all (0 compiled locales). Debian glibc does NOT compile
C.UTF-8 into libc.so.6 — it ships the precompiled locale dir
``/usr/lib/locale/C.utf8`` in the ``libc-bin`` package (~392 KB). With that dir
missing, ``setlocale(LC_ALL, "C.UTF-8")`` returns NULL and GLib/GTK aborts
(SIGABRT) at startup — the residual gtk3-widget-factory / gimp crash in §10(a).

Fix: stage ``/usr/lib/locale/C.utf8`` (glibc normalizes the requested name
"C.UTF-8" -> directory "C.utf8") plus a ``C.UTF-8`` symlink for belt-and-suspenders.
The dir-format locale is read by glibc when no locale-archive is present.

This emits the §5-E ``./``-rooted overlay tar consumed by the already-wired,
guarded ``xkb-gegl-stage.tar`` slot in MainActivity (no Kotlin change needed).

Source `libc-bin` is glibc 2.36 (bookworm) by default; the LC_* category format is
backward-compatible with the 2.39 base. Override --suite to match exactly.
"""

from __future__ import annotations

import argparse
import io
import shutil
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.deb_closure import _download_deb, fetch_packages_index, parse_packages
from tools.build_stage_tar import extract_deb
from tools.stage_tar_spec import validate_stage_tar

# locale dir -> alias symlink names that glibc may look up
DEFAULT_LOCALES = ("C.utf8",)
DEFAULT_ALIASES = {"C.UTF-8": "C.utf8"}
LOCALE_PREFIX = "usr/lib/locale"


@dataclass(frozen=True)
class LocaleOverlayResult:
    out_tar: str
    locales: tuple[str, ...]
    aliases: tuple[str, ...]
    file_count: int
    total_bytes: int


def _add_dir_tree(tar: tarfile.TarFile, src_root: Path, arc_root: str) -> tuple[int, int]:
    """Add every file/dir under src_root to tar as ./<arc_root>/... Returns (files, bytes)."""
    files = 0
    total = 0
    for path in sorted(src_root.rglob("*")):
        rel = path.relative_to(src_root).as_posix()
        arc = f"./{arc_root}/{rel}"
        if path.is_dir():
            ti = tarfile.TarInfo(arc + "/")
            ti.type = tarfile.DIRTYPE
            ti.mode = 0o755
            tar.addfile(ti)
        elif path.is_file():
            data = path.read_bytes()
            ti = tarfile.TarInfo(arc)
            ti.size = len(data)
            ti.mode = 0o644
            tar.addfile(ti, io.BytesIO(data))
            files += 1
            total += len(data)
    return files, total


def build_locale_overlay(
    out_tar: str | Path,
    *,
    locales=DEFAULT_LOCALES,
    aliases=None,
    mirror: str = "http://deb.debian.org/debian",
    suite: str = "bookworm",
    arch: str = "arm64",
    cache_dir: str | Path | None = None,
    source_root: str | Path | None = None,
) -> LocaleOverlayResult:
    """Build the §5-E C.UTF-8 locale overlay tar.

    If ``source_root`` is given, it is used as the extracted ``libc-bin`` root
    (offline). Otherwise libc-bin is downloaded from the mirror and extracted.
    """
    aliases = DEFAULT_ALIASES if aliases is None else aliases
    out_tar = Path(out_tar)

    if source_root is not None:
        root = Path(source_root)
    else:
        cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache")
        cache.mkdir(parents=True, exist_ok=True)
        index = parse_packages(fetch_packages_index(mirror, suite, arch))
        fields = index.get("libc-bin")
        if not fields or "Filename" not in fields:
            raise RuntimeError(f"libc-bin not found in {suite}/{arch} index")
        deb = _download_deb(mirror, fields["Filename"], cache)
        root = cache / "_libc-bin-root"
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)
        extract_deb(deb, root)

    locale_base = root / LOCALE_PREFIX
    staged: list[str] = []
    files = 0
    total = 0
    with tarfile.open(out_tar, "w") as tar:
        for name in locales:
            src = locale_base / name
            if not src.is_dir():
                raise RuntimeError(f"locale dir {name!r} not in source ({src})")
            f, b = _add_dir_tree(tar, src, f"{LOCALE_PREFIX}/{name}")
            files += f
            total += b
            staged.append(name)
        for alias, target in aliases.items():
            # only add the alias if its target is among the staged locales
            if target in staged:
                ti = tarfile.TarInfo(f"./{LOCALE_PREFIX}/{alias}")
                ti.type = tarfile.SYMTYPE
                ti.linkname = target  # relative, same dir -> in-tree (safe in extractOverlayTar)
                tar.addfile(ti)

    return LocaleOverlayResult(
        out_tar=str(out_tar),
        locales=tuple(staged),
        aliases=tuple(a for a, t in aliases.items() if t in staged),
        file_count=files,
        total_bytes=total,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_locale_overlay",
        description="Build a C.UTF-8 locale overlay tar from libc-bin.",
    )
    parser.add_argument("--out", help="output tar path (e.g. /tmp/xkb-gegl-stage.tar)")
    parser.add_argument("--locale", action="append", help="locale dir to stage (default C.utf8)")
    parser.add_argument("--suite", default="bookworm")
    parser.add_argument("--cache", default="/tmp/deb-cache")
    parser.add_argument("--base", help="base rootfs (tar|dir) to validate the overlay against")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.out:
        parser.error("--out is required (or use --selftest)")
    locales = tuple(args.locale) if args.locale else DEFAULT_LOCALES
    res = build_locale_overlay(args.out, locales=locales, suite=args.suite, cache_dir=args.cache)
    print(f"built {res.out_tar}: locales={res.locales} aliases={res.aliases} "
          f"files={res.file_count} bytes={res.total_bytes}")
    if args.base:
        rep = validate_stage_tar(res.out_tar, base=args.base)
        print(f"stage_tar_spec: {'CONFORMANT' if rep.conformant else 'NON-CONFORMANT'} "
              f"errors={rep.errors} warnings={rep.warnings}")
        return 0 if rep.conformant else 1
    return 0


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
        # synthetic libc-bin root with a C.utf8 locale dir
        cdir = tmp_path / "root" / LOCALE_PREFIX / "C.utf8"
        (cdir / "LC_MESSAGES").mkdir(parents=True)
        (cdir / "LC_CTYPE").write_bytes(b"\x00" * 4096)
        (cdir / "LC_COLLATE").write_bytes(b"\x01" * 256)
        (cdir / "LC_MESSAGES" / "SYS_LC_MESSAGES").write_bytes(b"\x02" * 64)
        out = tmp_path / "locale-stage.tar"
        res = build_locale_overlay(out, source_root=tmp_path / "root")

        with tarfile.open(out) as t:
            names = t.getnames()
            sym = [m for m in t.getmembers() if m.issym()]
        check("staged C.utf8", "C.utf8" in res.locales)
        check("tar has ./usr/lib/locale/C.utf8/LC_CTYPE",
              "./usr/lib/locale/C.utf8/LC_CTYPE" in names)
        check("tar has nested LC_MESSAGES/SYS_LC_MESSAGES",
              "./usr/lib/locale/C.utf8/LC_MESSAGES/SYS_LC_MESSAGES" in names)
        check("C.UTF-8 alias symlink present -> C.utf8",
              any(m.name == "./usr/lib/locale/C.UTF-8" and m.linkname == "C.utf8" for m in sym))
        check("file count counts the 3 real files", res.file_count == 3)

        # passes the §5-E validator (no base -> structural only) and is guard-clean
        rep = validate_stage_tar(out)
        check("stage_tar_spec conformant (no errors)", rep.conformant and not rep.errors)

        # against a base that lacks usr/lib/locale -> no downgrade, no violation
        base = tmp_path / "base.tar"
        with tarfile.open(base, "w") as bt:
            ti = tarfile.TarInfo("./usr/bin/true"); ti.size = 3
            bt.addfile(ti, io.BytesIO(b"ELF"))
        rep2 = validate_stage_tar(out, base=base)
        check("conformant against a real base (new paths, no downgrade)", rep2.conformant)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
