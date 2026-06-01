"""Build the GUI-completeness overlay (xkb-gegl-stage.tar) — WS-4 §10(a).

Device drain (v127) pinned TWO residual GUI faults, both rootfs-side:
1. **gtk3-widget-factory SIGABRT = the SVG pixbuf loader fails to open.** The base
   tar DOES ship `loaders/libpixbufloader_svg.so` + `librsvg-2.so.2` + a complete
   `loaders.cache` (with the svg stanza) and has NO missing DT_NEEDED dep — yet the
   device rootfs (extracted from an earlier APK, not re-extracted because the rootfs
   version marker didn't bump) lacks them, so `gdk-pixbuf` dlopen of the svg loader
   fails ("cannot open shared object file") → Gtk:ERROR → abort(6). An OVERLAY is
   gated by its OWN `.staged` marker, so it lands on the device regardless of the
   base re-extraction — delivering the svg loader directly.
2. **C.UTF-8 `setlocale` fails** ("Locale not supported") — the base has no compiled
   locale; the fix is the noble `C.utf8` dir (see build_locale_overlay).

This assembles BOTH into the single already-wired, guarded `xkb-gegl-stage.tar` slot:
  - C.utf8 (+ C.UTF-8 symlink) from Ubuntu noble libc-bin (matching glibc 2.39), and
  - libpixbufloader_svg.so + librsvg-2.so.2 + loaders.cache, copied from the (correct,
    complete) base tar — their core deps (libcairo/libxml2/…) predate the svg fix and
    are already on the device.
"""

from __future__ import annotations

import argparse
import io
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.build_locale_overlay import build_locale_overlay

# svg pixbuf-loader pieces to lift from the base tar (rootfs-relative, no leading ./)
SVG_PIECES = (
    "usr/lib/aarch64-linux-gnu/librsvg-2.so.2",
    "usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader_svg.so",
    "usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache",
)


@dataclass(frozen=True)
class GuiOverlayResult:
    out_tar: str
    locale_files: int
    svg_files: tuple[str, ...]
    missing_svg: tuple[str, ...]


def _norm(name: str) -> str:
    while name.startswith("./"):
        name = name[2:]
    return name


def build_gui_overlay(
    out_tar: str | Path,
    base_tar: str | Path,
    *,
    mirror: str = "http://ports.ubuntu.com/ubuntu-ports",
    suite: str = "noble",
    cache_dir: str | Path | None = None,
    svg_pieces=SVG_PIECES,
) -> GuiOverlayResult:
    """Assemble xkb-gegl-stage.tar = noble C.utf8 locale + svg loader pieces (from base_tar)."""
    out_tar = Path(out_tar)
    tmp_locale = out_tar.with_suffix(".locale.tar")
    loc = build_locale_overlay(tmp_locale, mirror=mirror, suite=suite, cache_dir=cache_dir)

    want = {_norm(p) for p in svg_pieces}
    found: dict[str, tarfile.TarInfo] = {}
    with tarfile.open(base_tar) as bt:
        members = {_norm(m.name): m for m in bt.getmembers()}
        with tarfile.open(out_tar, "w") as out:
            # 1) carry over every locale member from the temp locale tar
            with tarfile.open(tmp_locale) as lt:
                for m in lt.getmembers():
                    if m.isfile():
                        out.addfile(m, lt.extractfile(m))
                    else:
                        out.addfile(m)
            # 2) add the svg pieces from the base tar (real files)
            for rel in svg_pieces:
                rel_n = _norm(rel)
                m = members.get(rel_n)
                if m is None or not m.isfile():
                    continue
                data = bt.extractfile(m).read()
                ti = tarfile.TarInfo("./" + rel_n)
                ti.size = len(data)
                ti.mode = m.mode or 0o644
                out.addfile(ti, io.BytesIO(data))
                found[rel_n] = m

    tmp_locale.unlink(missing_ok=True)
    Path(str(tmp_locale) + ".lib-versions.json").unlink(missing_ok=True)
    missing = tuple(sorted(want - set(found)))
    return GuiOverlayResult(
        out_tar=str(out_tar),
        locale_files=loc.file_count,
        svg_files=tuple(sorted(found)),
        missing_svg=missing,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_gui_overlay",
        description="Build xkb-gegl-stage.tar (C.UTF-8 locale + svg pixbuf loader).",
    )
    parser.add_argument("--out", help="output tar (e.g. /tmp/xkb-gegl-stage.tar)")
    parser.add_argument("--base", help="base rootfs tar to lift svg pieces from")
    parser.add_argument("--suite", default="noble")
    parser.add_argument("--mirror", default="http://ports.ubuntu.com/ubuntu-ports")
    parser.add_argument("--cache", default="/tmp/deb-cache-ubuntu")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.out or not args.base:
        parser.error("--out and --base are required (or use --selftest)")
    res = build_gui_overlay(args.out, args.base, mirror=args.mirror, suite=args.suite, cache_dir=args.cache)
    print(f"built {res.out_tar}: locale_files={res.locale_files} svg_files={len(res.svg_files)}")
    for s in res.svg_files:
        print(f"    + {s}")
    if res.missing_svg:
        print(f"  WARNING missing svg pieces (base lacks): {res.missing_svg}")
    return 1 if res.missing_svg else 0


def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # Use the REAL base tar (svg pieces are static) + the real noble locale build.
    repo = Path(__file__).resolve().parents[1]
    base = repo / "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    if not base.is_file():
        print("  [skip] base tar not found"); print("\nselftest: SKIP"); return 0
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "xkb-gegl-stage.tar"
        res = build_gui_overlay(out, base, cache_dir="/tmp/deb-cache-ubuntu")
        with tarfile.open(out) as t:
            names = set(t.getnames())
        check("C.utf8 locale present", any("usr/lib/locale/C.utf8/LC_CTYPE" in n for n in names))
        check("C.UTF-8 symlink present", any(m.issym() and m.name.endswith("C.UTF-8")
                                             for m in tarfile.open(out).getmembers()))
        check("svg loader present", any("loaders/libpixbufloader_svg.so" in n for n in names))
        check("librsvg-2.so.2 present", any("librsvg-2.so.2" in n for n in names))
        check("loaders.cache present", any(n.endswith("loaders.cache") for n in names))
        check("no missing svg pieces", res.missing_svg == ())
        # validate against the base
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(out), base=str(base))
        check("stage_tar_spec conformant", rep.conformant)
    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
