#!/usr/bin/env python3
"""Build the FONT overlay (font-stage.tar) so GUI / text / browser apps render glyphs.

Why fonts are a top breadth blocker
-----------------------------------
The §5-E deb closure (``tools.deb_closure``) follows ``DT_NEEDED`` only, so it pulls
in the fontconfig / FreeType / harfbuzz *libraries* but NEVER the actual *font
files* — those are data, not linked objects. A base rootfs with ``libfontconfig``
present but ``/usr/share/fonts`` empty makes a whole CLASS of apps degrade or die:

  * GTK / Qt apps fall back to no glyphs (boxes) or abort when the default sans
    family resolves to nothing;
  * a browser (chromium) renders blank text runs — fontconfig's ``FcFontMatch``
    returns no file for "sans-serif", and Skia has no fallback face;
  * any app that calls ``FcConfigBuildFonts`` over an empty dir gets an empty set.

This is a per-app symptom with ONE root cause and ONE fix: stage a sensible base
font set + a fontconfig config that points at it. That unblocks GUI/text/browser
apps as a class, not one at a time.

What this stages
----------------
A curated, metric-broad base set, each family a CLI flag (all on by default except
CJK which is large and behind ``--cjk``):

  * ``fonts-dejavu-core``  → DejaVu Sans / Serif / Sans Mono  (the de-facto Linux
    default; broad Latin/Cyrillic/Greek coverage, hinted, ~1.5 MB)
  * ``fonts-liberation2``  → Liberation Sans / Serif / Mono   (metric-compatible
    drop-ins for Arial / Times New Roman / Courier New — critical so documents &
    web pages that *name* those MS families lay out correctly, ~1.5 MB)
  * ``fonts-noto-core``    → Noto Sans / Serif + many scripts (Google Noto Latin
    core + a wide non-CJK script range; the standard "no tofu" fallback, ~12 MB)
  * ``fonts-noto-cjk``     → Noto Sans CJK   (``--cjk`` ONLY — ~110 MB; needed for
    Chinese / Japanese / Korean text, but far too large to stage unconditionally)

Layout produced (a §5-E ``./``-rooted overlay tar)::

    ./usr/share/fonts/truetype/dejavu/*.ttf
    ./usr/share/fonts/truetype/liberation2/*.ttf
    ./usr/share/fonts/opentype/noto/*.ttf        (noto-core ships TTF; cjk ships OTC)
    ./etc/fonts/fonts.conf                        (minimal; <dir> -> the staged trees)
    ./etc/fonts/conf.d/00-alr-fonts.conf          (generic-family aliases + fallback)

Font cache
----------
fontconfig keeps a per-(arch,version) binary cache under ``/var/cache/fontconfig``
(or ``$XDG_CACHE_HOME/fontconfig``). That cache format is tied to the fontconfig
build on the TARGET (guest arm64 glibc), so a host-built cache (macOS fontconfig)
would be the wrong format and ignored. We therefore do NOT ship a cache: the guest
fontconfig builds it transparently on first ``FcInit`` (fontconfig's documented
auto-rebuild when the cache is missing/stale). ``--gen-cache`` can run a host
``fc-cache`` against the staged tree only as a *content* sanity check (its output
is NOT packed). This is the orthodox, portable choice and is documented for the
integrator.

Reuse (read-only) of the WS-4 overlay engine:
  * ``tools.deb_closure``    — fetch_packages_index / parse_packages / _download_deb
  * ``tools.build_stage_tar``— extract_deb (.deb cracker, noble data.tar.zst)
  * ``tools.stage_tar_spec`` — validate_stage_tar (§5-E conformance + base check)
"""

from __future__ import annotations

import argparse
import io
import shutil
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from tools.deb_closure import _download_deb, fetch_packages_index, parse_packages
from tools.build_stage_tar import extract_deb
from tools.stage_tar_spec import validate_stage_tar

# The ALR base is Ubuntu noble 24.04 arm64 — fonts live in main/universe.
MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main", "universe")

FONT_PREFIX = "usr/share/fonts/truetype"
FONTCONFIG_DIR = "etc/fonts"


@dataclass(frozen=True)
class FontFamily:
    """One stageable font family = one owning noble .deb + a staging subdir."""

    key: str                 # CLI flag stem and dict key, e.g. "dejavu"
    package: str             # owning noble package, e.g. "fonts-dejavu-core"
    subdir: str              # staged under usr/share/fonts/truetype/<subdir>/
    description: str
    default: bool = True     # staged unless explicitly turned off
    extensions: tuple[str, ...] = (".ttf", ".ttc", ".otf", ".otc")


# The curated base set. Order is the staging/listing order (deterministic).
FAMILIES: tuple[FontFamily, ...] = (
    FontFamily(
        key="dejavu",
        package="fonts-dejavu-core",
        subdir="dejavu",
        description="DejaVu Sans/Serif/Mono — de-facto Linux default (Latin/Cyrillic/Greek)",
    ),
    FontFamily(
        key="liberation",
        package="fonts-liberation2",
        subdir="liberation2",
        description="Liberation Sans/Serif/Mono — metric-compatible Arial/Times/Courier",
    ),
    FontFamily(
        key="noto",
        package="fonts-noto-core",
        subdir="noto",
        description="Noto Sans/Serif core — Google 'no tofu' broad-script fallback (Latin+)",
    ),
    FontFamily(
        key="cjk",
        package="fonts-noto-cjk",
        subdir="noto-cjk",
        description="Noto Sans CJK — Chinese/Japanese/Korean (LARGE ~110MB; --cjk only)",
        default=False,
    ),
)

FAMILIES_BY_KEY: dict[str, FontFamily] = {f.key: f for f in FAMILIES}


# Minimal fontconfig top-level config: point at the staged font trees and pull in
# conf.d. Deliberately tiny — we do NOT depend on the full Debian fontconfig-config
# package's conf.d (that pulls in dozens of files); generic-family aliasing lives in
# the single 00-alr-fonts.conf we ship. <cachedir> is writable guest state.
FONTS_CONF = """\
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<!-- ALR font overlay: minimal fontconfig config (build_font_overlay.py). -->
<fontconfig>
  <!-- Staged font trees. -->
  <dir>/usr/share/fonts</dir>
  <dir>/usr/local/share/fonts</dir>
  <dir prefix="xdg">fonts</dir>
  <dir>~/.fonts</dir>

  <!-- Guest-writable cache: fontconfig rebuilds this on first run for the guest
       arm64 build (a host cache would be the wrong arch/version). -->
  <cachedir>/var/cache/fontconfig</cachedir>
  <cachedir prefix="xdg">fontconfig</cachedir>

  <include ignore_missing="yes">/etc/fonts/conf.d</include>
</fontconfig>
"""

# Generic-family aliases + a last-resort fallback chain so "sans-serif"/"serif"/
# "monospace" always resolve to a staged face even when an app names a family we
# don't ship (e.g. "Arial" -> Liberation Sans via metric compat; unknown -> DejaVu).
ALR_FONTS_CONF = """\
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<!-- ALR font overlay: generic-family aliases + fallback (build_font_overlay.py). -->
<fontconfig>
  <!-- Default generic families -> staged faces. -->
  <alias>
    <family>sans-serif</family>
    <prefer>
      <family>DejaVu Sans</family>
      <family>Liberation Sans</family>
      <family>Noto Sans</family>
    </prefer>
  </alias>
  <alias>
    <family>serif</family>
    <prefer>
      <family>DejaVu Serif</family>
      <family>Liberation Serif</family>
      <family>Noto Serif</family>
    </prefer>
  </alias>
  <alias>
    <family>monospace</family>
    <prefer>
      <family>DejaVu Sans Mono</family>
      <family>Liberation Mono</family>
    </prefer>
  </alias>

  <!-- Metric-compatible substitutions for the common MS families documents name. -->
  <alias binding="same"><family>Arial</family><accept><family>Liberation Sans</family></accept></alias>
  <alias binding="same"><family>Helvetica</family><accept><family>Liberation Sans</family></accept></alias>
  <alias binding="same"><family>Times New Roman</family><accept><family>Liberation Serif</family></accept></alias>
  <alias binding="same"><family>Times</family><accept><family>Liberation Serif</family></accept></alias>
  <alias binding="same"><family>Courier New</family><accept><family>Liberation Mono</family></accept></alias>
  <alias binding="same"><family>Courier</family><accept><family>Liberation Mono</family></accept></alias>

  <!-- Last-resort fallback: anything unresolved gets DejaVu then Noto. -->
  <match target="pattern">
    <test name="family" qual="all" compare="not_eq"><string>__never__</string></test>
    <edit name="family" mode="append_last">
      <string>DejaVu Sans</string>
      <string>Noto Sans</string>
    </edit>
  </match>
</fontconfig>
"""


@dataclass(frozen=True)
class FontOverlayResult:
    out_tar: str
    families: tuple[str, ...]              # family keys staged
    packages: tuple[str, ...]              # owning .debs resolved
    font_count: int                        # number of font FILES packed
    config_files: tuple[str, ...]          # rootfs-rel config paths packed
    total_bytes: int
    per_family: dict[str, int] = field(default_factory=dict)  # key -> font file count

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "families": list(self.families),
            "packages": list(self.packages),
            "font_count": self.font_count,
            "config_files": list(self.config_files),
            "total_bytes": self.total_bytes,
            "per_family": dict(self.per_family),
        }


def select_families(
    *,
    dejavu: bool = True,
    liberation: bool = True,
    noto: bool = True,
    cjk: bool = False,
) -> list[FontFamily]:
    """Resolve the per-flag booleans into the ordered list of families to stage.

    Pure / offline — drives both the CLI and the test. Defaults match the curated
    base set: DejaVu + Liberation + Noto core on, CJK off (it is ~110 MB).
    """
    chosen = {"dejavu": dejavu, "liberation": liberation, "noto": noto, "cjk": cjk}
    return [fam for fam in FAMILIES if chosen.get(fam.key, fam.default)]


def _iter_font_files(tree: Path, extensions: tuple[str, ...]):
    """Yield (relative_name, full_path) for every font file under an extracted tree.

    Matches by extension (case-insensitive). Deterministic (sorted by rel path).
    The relative name is just the basename — Debian font packages put all faces of
    a family flat in one dir, so flattening to ``<subdir>/<basename>`` is correct.
    """
    exts = tuple(e.lower() for e in extensions)
    out: list[tuple[str, Path]] = []
    if not tree.is_dir():
        return out
    for path in tree.rglob("*"):
        if path.is_file() and path.suffix.lower() in exts:
            out.append((path.name, path))
    out.sort(key=lambda t: t[0])
    return out


def _add_bytes(tar: tarfile.TarFile, arcname: str, data: bytes, mode: int = 0o644) -> None:
    ti = tarfile.TarInfo("./" + arcname.lstrip("./"))
    ti.size = len(data)
    ti.mode = mode
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    tar.addfile(ti, io.BytesIO(data))


def _add_file(tar: tarfile.TarFile, arcname: str, src: Path, mode: int = 0o644) -> int:
    data = src.read_bytes()
    _add_bytes(tar, arcname, data, mode=mode)
    return len(data)


def _resolve_family_tree(
    fam: FontFamily,
    *,
    mirror: str,
    suite: str,
    arch: str,
    components,
    cache: Path,
    source_root: Path | None,
    opener=None,
) -> Path:
    """Return the extracted tree containing ``fam``'s font files.

    If ``source_root`` is given it is an already-extracted root (offline tests):
    we look for ``<source_root>/usr/share/fonts`` (any depth). Otherwise the owning
    .deb is fetched from the mirror and extracted into the cache.
    """
    if source_root is not None:
        # Offline: caller supplies an extracted root; fonts may live under either
        # usr/share/fonts/truetype/<subdir> or be flat under usr/share/fonts.
        cand = source_root / FONT_PREFIX / fam.subdir
        if cand.is_dir():
            return cand
        share = source_root / "usr/share/fonts"
        return share if share.is_dir() else source_root

    kw = {"components": components}
    if opener is not None:
        kw["opener"] = opener
    index = parse_packages(fetch_packages_index(mirror, suite, arch, **kw))
    fields = index.get(fam.package)
    if not fields or "Filename" not in fields:
        raise RuntimeError(f"{fam.package} not found in {suite}/{arch} index")
    dl_kw = {"opener": opener} if opener is not None else {}
    deb = _download_deb(mirror, fields["Filename"], cache, **dl_kw)
    root = cache / f"_font_{fam.key}"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    extract_deb(deb, root)
    return root


def build_font_overlay(
    out_tar: str | Path,
    families: list[FontFamily],
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = None,
    source_root: str | Path | None = None,
    include_config: bool = True,
    opener=None,
) -> FontOverlayResult:
    """Build the §5-E font overlay tar for ``families``.

    For each family the owning noble .deb is fetched + extracted (or read from
    ``source_root`` offline) and every font file is packed flat at
    ``/usr/share/fonts/truetype/<subdir>/``. When ``include_config`` (default) a
    minimal ``/etc/fonts/fonts.conf`` + ``/etc/fonts/conf.d/00-alr-fonts.conf`` are
    written so generic families resolve and MS-named families map to the Liberation
    metric-compatible faces.

    All font paths are BRAND NEW relative to the base rootfs (the base ships the
    fontconfig *libraries* but no fonts), so no base subtraction is needed; the
    output is still §5-E conformant and can be validated against a base via
    ``validate_stage_tar`` for belt-and-suspenders.
    """
    out_tar = Path(out_tar)
    cache = Path(cache_dir) if cache_dir is not None else Path(tempfile.mkdtemp(prefix="alr-font-"))
    cache.mkdir(parents=True, exist_ok=True)
    src_root = Path(source_root) if source_root is not None else None

    staged_keys: list[str] = []
    staged_pkgs: list[str] = []
    per_family: dict[str, int] = {}
    config_files: list[str] = []
    font_count = 0
    total = 0

    out_tar.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out_tar, "w") as tar:
        for fam in families:
            tree = _resolve_family_tree(
                fam,
                mirror=mirror,
                suite=suite,
                arch=arch,
                components=components,
                cache=cache,
                source_root=src_root,
                opener=opener,
            )
            files = _iter_font_files(tree, fam.extensions)
            if not files:
                # A family with no font files is a hard error for an explicitly
                # requested family — staging an empty dir would silently no-op.
                raise RuntimeError(
                    f"no font files ({fam.extensions}) found for {fam.package} under {tree}"
                )
            for name, src in files:
                arc = f"{FONT_PREFIX}/{fam.subdir}/{name}"
                total += _add_file(tar, arc, src, mode=0o644)
                font_count += 1
            per_family[fam.key] = len(files)
            staged_keys.append(fam.key)
            staged_pkgs.append(fam.package)

        if include_config:
            _add_bytes(tar, f"{FONTCONFIG_DIR}/fonts.conf", FONTS_CONF.encode(), mode=0o644)
            _add_bytes(tar, f"{FONTCONFIG_DIR}/conf.d/00-alr-fonts.conf",
                       ALR_FONTS_CONF.encode(), mode=0o644)
            config_files = [
                f"{FONTCONFIG_DIR}/fonts.conf",
                f"{FONTCONFIG_DIR}/conf.d/00-alr-fonts.conf",
            ]
            total += len(FONTS_CONF.encode()) + len(ALR_FONTS_CONF.encode())

    return FontOverlayResult(
        out_tar=str(out_tar),
        families=tuple(staged_keys),
        packages=tuple(staged_pkgs),
        font_count=font_count,
        config_files=tuple(config_files),
        total_bytes=total,
        per_family=per_family,
    )


def gen_host_cache_check(staged_tree: Path) -> str | None:
    """Best-effort host ``fc-cache`` over the staged tree as a CONTENT sanity check.

    The produced cache is for the HOST fontconfig arch/version and is NOT packed
    (the guest rebuilds its own). Returns fc-cache's combined output, or None if no
    host ``fc-cache`` is available. Never raises — purely advisory.
    """
    fc = shutil.which("fc-cache")
    if fc is None:
        return None
    try:
        proc = subprocess.run(
            [fc, "-f", "-v", str(staged_tree)],
            capture_output=True, text=True, timeout=120,
        )
    except Exception as exc:  # pragma: no cover - advisory only
        return f"fc-cache failed: {exc!r}"
    return (proc.stdout or "") + (proc.stderr or "")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_font_overlay",
        description="Build a font overlay (font-stage.tar) for GUI/text/browser apps.",
    )
    parser.add_argument("--out", default="/tmp/font-stage.tar",
                        help="output overlay tar (default: %(default)s)")
    parser.add_argument("--cache", default="/tmp/alr-deb-cache",
                        help="package/index cache dir (default: %(default)s)")
    parser.add_argument("--mirror", default=MIRROR, help="mirror base URL (default: %(default)s)")
    parser.add_argument("--suite", default=SUITE, help="suite (default: %(default)s)")
    parser.add_argument("--arch", default=ARCH, help="architecture (default: %(default)s)")
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main+universe)")
    # Per-family flags. Defaults: dejavu/liberation/noto ON, cjk OFF.
    parser.add_argument("--no-dejavu", action="store_true", help="omit DejaVu core")
    parser.add_argument("--no-liberation", action="store_true", help="omit Liberation 2")
    parser.add_argument("--no-noto", action="store_true", help="omit Noto core")
    parser.add_argument("--cjk", action="store_true",
                        help="ALSO stage Noto CJK (~110MB; Chinese/Japanese/Korean)")
    parser.add_argument("--no-config", action="store_true",
                        help="omit the /etc/fonts config files (fonts only)")
    parser.add_argument("--base", help="base rootfs (tar|dir) to validate the overlay against")
    parser.add_argument("--gen-cache", action="store_true",
                        help="run host fc-cache over the staged tree as a content check "
                             "(NOT packed; guest rebuilds its own cache on first run)")
    parser.add_argument("--list", action="store_true",
                        help="print the planned families + owning packages + targets, no fetch")
    args = parser.parse_args(argv)

    families = select_families(
        dejavu=not args.no_dejavu,
        liberation=not args.no_liberation,
        noto=not args.no_noto,
        cjk=args.cjk,
    )

    if args.list:
        print("font overlay plan (build_font_overlay):")
        print(f"  mirror={args.mirror} suite={args.suite} arch={args.arch}")
        if not families:
            print("  (no families selected)")
        for fam in families:
            print(f"  [{fam.key:11s}] {fam.package}")
            print(f"               -> /{FONT_PREFIX}/{fam.subdir}/*.ttf")
            print(f"               {fam.description}")
        print(f"  config       -> /{FONTCONFIG_DIR}/fonts.conf "
              f"+ /{FONTCONFIG_DIR}/conf.d/00-alr-fonts.conf")
        print("  cache        -> NOT packed; guest fontconfig rebuilds on first FcInit "
              "(host cache is wrong arch).")
        # Always list the FULL catalog too, so --list documents every flag.
        print("  available families:")
        for fam in FAMILIES:
            state = "default-on" if fam.default else "opt-in (--cjk)"
            print(f"    {fam.key:11s} {fam.package:20s} [{state}]")
        return 0

    if not families:
        parser.error("no font families selected (all families disabled)")

    components = tuple(args.components) if args.components else COMPONENTS
    res = build_font_overlay(
        args.out,
        families,
        mirror=args.mirror,
        suite=args.suite,
        arch=args.arch,
        components=components,
        cache_dir=args.cache,
        include_config=not args.no_config,
    )
    print(f"built {res.out_tar}")
    print(f"  families:  {', '.join(res.families)}")
    print(f"  packages:  {', '.join(res.packages)}")
    print(f"  fonts:     {res.font_count} file(s) "
          f"({', '.join(f'{k}={v}' for k, v in res.per_family.items())})")
    if res.config_files:
        print(f"  config:    {', '.join(res.config_files)}")
    print(f"  bytes:     {res.total_bytes}")

    if args.gen_cache:
        with tempfile.TemporaryDirectory() as td:
            tree = Path(td) / "fonts"
            tree.mkdir()
            with tarfile.open(res.out_tar) as t:
                for m in t.getmembers():
                    if m.isfile() and m.name.startswith(f"./{FONT_PREFIX}/"):
                        rel = m.name[len(f"./{FONT_PREFIX}/"):]
                        dest = tree / rel
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        f = t.extractfile(m)
                        if f is not None:
                            dest.write_bytes(f.read())
            out = gen_host_cache_check(tree)
            print("  fc-cache (host content check, NOT packed):")
            print("    " + ("(no host fc-cache)" if out is None
                            else out.strip().replace("\n", "\n    ")))

    if args.base:
        rep = validate_stage_tar(res.out_tar, base=args.base)
        print(f"  stage_tar_spec: {'CONFORMANT' if rep.conformant else 'NON-CONFORMANT'} "
              f"errors={len(rep.errors)} warnings={len(rep.warnings)}")
        for e in rep.errors:
            print(f"    ERROR {e}")
        return 0 if rep.conformant else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
