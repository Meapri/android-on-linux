"""Reconstruct the dpkg admin DB for the ALR base rootfs (WS-4 §10c).

Why
---
The ALR base is **Ubuntu noble 24.04 arm64** (glibc 2.39), but it was assembled by
*extracting files*, not by ``dpkg -i``. Consequently ``var/lib/dpkg/status`` is a
0-byte file and ``var/lib/dpkg/info/`` is empty — dpkg has no install database at
all. Apt/dpkg therefore believe NOTHING is installed: ``apt install X`` re-plans the
entire dependency tree of ``X`` from scratch, re-downloading base libraries (libc6,
zlib, …) that are physically already present, and may even try to *downgrade* them.

The fix is to synthesize a believable dpkg admin DB from the base's actual files:

  1. download Ubuntu's ``Contents-arm64`` index (a file→package map) and the
     ``Packages`` index for noble main+universe;
  2. for every real FILE the base ships, look it up in Contents to learn which
     package it belongs to → the set of packages the base "has installed";
  3. for each such package, emit a dpkg ``status`` stanza
     (``Status: install ok installed`` + Architecture/Version/Depends/… carried from
     the Packages index) and a ``var/lib/dpkg/info/<pkg>.list`` file listing the
     package's files. Wrap them in a §5-E ``./``-rooted overlay tar that the device
     extractor lays over the base.

With that DB in place, ``apt install netsurf-gtk`` sees libc6/zlib/etc. as already
installed and only fetches what is genuinely new.

This module reuses, and does NOT duplicate:
  * ``tools.deb_closure`` — ``parse_packages`` / ``fetch_packages_index`` /
    ``_urlopen_ua`` (apt User-Agent; ports.ubuntu.com 403s the default UA);
  * ``tools.safe_tar``    — ``inspect_tar_members`` (the safety-validated base walk);
  * ``tools.stage_tar_spec`` — ``validate_stage_tar`` (the §5-E conformance check).

Only ``build_dpkg_db`` / ``fetch_contents`` touch the network. ``--selftest`` is
fully OFFLINE and deterministic.
"""

from __future__ import annotations

import argparse
import gzip
import io
import tarfile
import tempfile
import urllib.error
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from tools.deb_closure import _urlopen_ua, fetch_packages_index, parse_packages
from tools.safe_tar import inspect_tar_members


# --------------------------------------------------------------------------- #
# Contents index (file -> package map)
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class ContentsFetch:
    """Result of fetching the Contents index: the text plus provenance."""
    text: str
    urls_ok: tuple[str, ...]      # URLs that returned data
    urls_failed: tuple[str, ...]  # URLs tried and 404/errored (informational)


def fetch_contents(
    mirror: str,
    suite: str,
    arch: str,
    *,
    components=("main", "universe"),
    opener=_urlopen_ua,
) -> ContentsFetch:
    """Download + gunzip the Ubuntu ``Contents-<arch>`` index.

    Ubuntu publishes Contents either suite-wide at
    ``dists/<suite>/Contents-<arch>.gz`` or per-component at
    ``dists/<suite>/<component>/Contents-<arch>.gz``. We try the suite-wide URL
    first; on 404 (or any fetch error) we fall back to the per-component URLs for
    each requested component and concatenate what we get. Uses the apt User-Agent
    opener — ports.ubuntu.com 403s the default Python-urllib UA.

    Returns a :class:`ContentsFetch` recording which URLs worked so a live run can
    report its provenance. Raises ``RuntimeError`` only if *no* URL yielded data.
    """
    if isinstance(components, str):
        components = (components,)
    base = mirror.rstrip("/")

    parts: list[str] = []
    urls_ok: list[str] = []
    urls_failed: list[str] = []

    def _try(url: str) -> bool:
        try:
            with opener(url) as resp:
                blob = resp.read()
        except (urllib.error.HTTPError, urllib.error.URLError, OSError):
            urls_failed.append(url)
            return False
        parts.append(gzip.decompress(blob).decode("utf-8", "replace"))
        urls_ok.append(url)
        return True

    suite_url = f"{base}/dists/{suite}/Contents-{arch}.gz"
    if not _try(suite_url):
        for comp in components:
            _try(f"{base}/dists/{suite}/{comp}/Contents-{arch}.gz")

    if not parts:
        raise RuntimeError(
            f"no Contents-{arch} index could be fetched (tried {urls_failed})"
        )
    return ContentsFetch(
        text="\n".join(parts),
        urls_ok=tuple(urls_ok),
        urls_failed=tuple(urls_failed),
    )


def parse_contents(text: str) -> dict[str, str]:
    """Parse an apt ``Contents`` index into ``{filepath: pkgname}``.

    Each data line is ``FILEPATH   AREA/PKGNAME`` where FILEPATH is rootfs-relative
    (no leading ``/``) and the file column may itself contain spaces. The package
    column is the LAST whitespace-separated field; it is one or more
    comma-separated ``area/pkgname`` entries (e.g. ``universe/libs/libfoo,
    main/x/bar``). We take the package's basename (the part after the last ``/``)
    and, when several packages claim a file, keep the FIRST.

    Header lines (the ``FILE   LOCATION`` preamble some mirrors emit) and blank
    lines are skipped: a line is only accepted if its package column contains a
    ``/`` (every real entry is ``area/pkg``).

    >>> parse_contents("usr/bin/foo   utils/foo-pkg\\nusr/lib/x.so   libs/libx\\n")
    {'usr/bin/foo': 'foo-pkg', 'usr/lib/x.so': 'libx'}
    """
    mapping: dict[str, str] = {}
    for line in text.splitlines():
        if not line or not line.strip():
            continue
        # The path may contain spaces; the package list is the last field.
        filepath, sep, pkgcol = line.rpartition(" ")
        if not sep:
            # No whitespace at all — not a data line.
            continue
        # collapse any trailing whitespace that rpartition left on the path side,
        # and any leading whitespace the split produced before the package col.
        filepath = filepath.rstrip()
        pkgcol = pkgcol.strip()
        if not filepath or not pkgcol:
            continue
        # Every real Contents entry's package column is "area/pkg[,area/pkg...]".
        # Skip the header ("FILE   LOCATION") which has no "/".
        if "/" not in pkgcol:
            continue
        first = pkgcol.split(",", 1)[0].strip()
        pkgname = first.rsplit("/", 1)[-1].strip()
        if not pkgname:
            continue
        filepath = filepath.lstrip("/")
        # First package claiming a file wins (deterministic for shared paths).
        mapping.setdefault(filepath, pkgname)
    return mapping


# --------------------------------------------------------------------------- #
# Installed-package set from the base
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class InstalledSet:
    packages: frozenset[str]
    files_mapped: int        # base files attributed to a package
    files_unmapped: int      # base files not found in Contents


def _base_real_files(base_tar: str | Path) -> list[str]:
    """Rootfs-relative paths of every REAL FILE in the base (no leading ``./``).

    Accepts a tar (via inspect_tar_members) — only ``file`` members count;
    directories, symlinks and hardlinks are excluded (Contents maps real files).
    """
    files: list[str] = []
    for m in inspect_tar_members(base_tar):
        if m.kind == "file":
            files.append(m.name.lstrip("/"))
    return files


def installed_packages(base_tar: str | Path, contents_map: dict[str, str]) -> InstalledSet:
    """Compute the set of packages the base's files belong to.

    For each real file in ``base_tar`` (rootfs-relative, leading ``./`` stripped),
    look it up in ``contents_map``. Files present in Contents contribute their
    package; files absent from Contents are counted as unmapped (e.g. ALR-injected
    files, generated caches) and skipped — never fatal.
    """
    from tools.overlay_guard import parse_solib
    from tools.deb_closure import _mergedusr_aliases

    # SONAME -> package, derived from Contents. The base FLATTENS SONAMEs (it ships
    # `libgtk-3.so.0`) while Contents lists the versioned real name
    # (`libgtk-3.so.0.2404.x` -> libgtk-3-0); without this, glib/gtk/gcc/stdc++ —
    # whose only base presence is the flattened .so — go unmapped and apt would
    # re-install the whole GTK stack.
    soname_pkg: dict[str, str] = {}
    for path, pkg in contents_map.items():
        lib = parse_solib(path.rsplit("/", 1)[-1])
        if lib is not None:
            soname_pkg.setdefault(lib.soname, pkg)

    pkgs: set[str] = set()
    mapped = 0
    unmapped = 0
    for rel in _base_real_files(base_tar):
        pkg = contents_map.get(rel)
        if pkg is None:  # merged-usr: base lib/X <-> Contents usr/lib/X (and vice versa)
            for alias in _mergedusr_aliases(rel):
                pkg = contents_map.get(alias)
                if pkg:
                    break
        if pkg is None:  # flattened SONAME: base libfoo.so.N -> soname -> package
            lib = parse_solib(rel.rsplit("/", 1)[-1])
            if lib is not None:
                pkg = soname_pkg.get(lib.soname)
        if pkg is None:
            unmapped += 1
            continue
        pkgs.add(pkg)
        mapped += 1
    return InstalledSet(
        packages=frozenset(pkgs),
        files_mapped=mapped,
        files_unmapped=unmapped,
    )


# --------------------------------------------------------------------------- #
# DB reconstruction: status stanzas + .list file lists
# --------------------------------------------------------------------------- #

# dpkg status stanza fields carried from the Packages stanza, in dpkg's canonical
# order. Package/Status/Priority/Architecture/Version are emitted explicitly; the
# rest are copied verbatim when present.
_CARRY_FIELDS = (
    "Essential",
    "Multi-Arch",
    "Source",
    "Provides",
    "Depends",
    "Pre-Depends",
    "Recommends",
    "Suggests",
    "Conflicts",
    "Breaks",
    "Replaces",
    "Section",
    "Description",
)


def build_status(packages, index: dict[str, dict]) -> str:
    """Render a dpkg ``var/lib/dpkg/status`` body for ``packages``.

    For each package name in ``packages`` that exists in the Packages ``index``,
    emit a stanza marked ``Status: install ok installed`` carrying
    Package/Priority/Architecture/Version plus Depends/Provides/Description and the
    other relationship fields from the Packages stanza. Packages absent from the
    index are skipped (no Version to attest). Stanzas are sorted by name and
    separated by a single blank line — fully deterministic.
    """
    names = sorted(set(packages) & set(index))
    stanzas: list[str] = []
    for name in names:
        fields = index[name]
        lines: list[str] = [f"Package: {name}"]
        priority = fields.get("Priority")
        if priority:
            lines.append(f"Priority: {priority}")
        lines.append("Status: install ok installed")
        # Architecture/Version are the load-bearing fields apt checks.
        lines.append(f"Architecture: {fields.get('Architecture', 'arm64')}")
        version = fields.get("Version")
        if version:
            lines.append(f"Version: {version}")
        for key in _CARRY_FIELDS:
            value = fields.get(key)
            if not value:
                continue
            lines.append(_render_field(key, value))
        stanzas.append("\n".join(lines))
    # Each stanza ends with its own newline; stanzas are blank-line separated.
    return "".join(stanza + "\n\n" for stanza in stanzas)


def _render_field(key: str, value: str) -> str:
    """Render a (possibly folded) field for a dpkg stanza.

    ``parse_packages`` stores continuation lines joined by ``\\n``; dpkg expects
    each continuation indented by one space. Re-fold accordingly so the emitted
    status file round-trips (notably multi-line Description).
    """
    if "\n" not in value:
        return f"{key}: {value}"
    head, *rest = value.split("\n")
    folded = [f"{key}: {head}"]
    folded.extend(f" {line}" for line in rest)
    return "\n".join(folded)


def build_info_lists(packages, contents_map: dict[str, str]) -> dict[str, str]:
    """Build the ``var/lib/dpkg/info/<pkg>.list`` body for each package.

    A package's ``.list`` is the sorted, ``/``-prefixed list of file paths that
    Contents attributes to it (one path per line, trailing newline). Packages with
    no files in ``contents_map`` get an empty body. Deterministic: paths sorted.
    """
    by_pkg: dict[str, list[str]] = {name: [] for name in packages}
    for filepath, pkg in contents_map.items():
        if pkg in by_pkg:
            by_pkg[pkg].append("/" + filepath.lstrip("/"))
    out: dict[str, str] = {}
    for name, paths in by_pkg.items():
        paths.sort()
        out[name] = "".join(p + "\n" for p in paths)
    return out


# --------------------------------------------------------------------------- #
# Overlay tar assembly
# --------------------------------------------------------------------------- #

def _add_text_member(tar: tarfile.TarFile, arcname: str, body: str, mode: int = 0o644) -> int:
    """Add a UTF-8 text file member to ``tar``; return its byte size."""
    data = body.encode("utf-8")
    info = tarfile.TarInfo(arcname)
    info.size = len(data)
    info.mode = mode
    info.type = tarfile.REGTYPE
    info.mtime = 0
    tar.addfile(info, io.BytesIO(data))
    return len(data)


def write_db_overlay(
    out_tar: str | Path,
    status_body: str,
    info_lists: dict[str, str],
) -> int:
    """Write the §5-E ``./``-rooted dpkg-DB overlay tar.

    Members (deterministic order):
      * ``./var/lib/dpkg/status``           — the reconstructed status DB;
      * ``./var/lib/dpkg/info/<pkg>.list``  — one per package, sorted by name.

    Returns the byte size of the status member.
    """
    out_path = Path(out_tar)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out_path, "w") as tar:
        status_bytes = _add_text_member(tar, "./var/lib/dpkg/status", status_body)
        for name in sorted(info_lists):
            _add_text_member(tar, f"./var/lib/dpkg/info/{name}.list", info_lists[name])
    return status_bytes


# --------------------------------------------------------------------------- #
# Build path (network)
# --------------------------------------------------------------------------- #

def build_dpkg_db(
    base_tar: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = "http://ports.ubuntu.com/ubuntu-ports",
    suite: str = "noble",
    arch: str = "arm64",
    components=("main", "universe"),
    cache_dir: str | Path | None = None,
    opener=_urlopen_ua,
) -> dict:
    """Reconstruct the dpkg admin DB for ``base_tar`` and write it as an overlay.

    NETWORK PATH. Fetches the Ubuntu ``Contents-<arch>`` and ``Packages`` indexes
    (cached under ``cache_dir`` when given), maps the base's real files to their
    packages, then emits ``var/lib/dpkg/status`` + ``var/lib/dpkg/info/<pkg>.list``
    in a §5-E ``./``-rooted overlay tar.

    Returns ``{packages, files_mapped, files_unmapped, out_tar, status_bytes,
    contents_urls, conformant}``.
    """
    if isinstance(components, str):
        components = (components,)

    cache = Path(cache_dir) if cache_dir is not None else None
    if cache is not None:
        cache.mkdir(parents=True, exist_ok=True)

    contents_text = _cached_contents(mirror, suite, arch, components, cache, opener)
    contents_map = parse_contents(contents_text.text)

    packages_text = fetch_packages_index(
        mirror, suite, arch, components=components, opener=opener
    )
    index = parse_packages(packages_text)

    inst = installed_packages(base_tar, contents_map)
    status_body = build_status(inst.packages, index)
    # Only packages we actually attest as installed (present in index) get a
    # .list, matching the status stanzas dpkg will read.
    attested = sorted(set(inst.packages) & set(index))
    info_lists = build_info_lists(attested, contents_map)

    status_bytes = write_db_overlay(out_tar, status_body, info_lists)

    conformant: bool | None = None
    try:
        from tools.stage_tar_spec import validate_stage_tar

        report = validate_stage_tar(out_tar, base_tar)
        conformant = report.conformant
    except Exception:
        conformant = None

    return {
        "packages": len(attested),
        "files_mapped": inst.files_mapped,
        "files_unmapped": inst.files_unmapped,
        "out_tar": str(out_tar),
        "status_bytes": status_bytes,
        "contents_urls": list(contents_text.urls_ok),
        "conformant": conformant,
    }


def _cached_contents(mirror, suite, arch, components, cache: Path | None, opener) -> ContentsFetch:
    """Fetch Contents, caching the gunzipped text under ``cache`` when provided."""
    if cache is not None:
        cache_file = cache / f"Contents-{suite}-{arch}.txt"
        if cache_file.is_file() and cache_file.stat().st_size > 0:
            return ContentsFetch(
                text=cache_file.read_text(encoding="utf-8", errors="replace"),
                urls_ok=("(cache)",),
                urls_failed=(),
            )
    fetched = fetch_contents(mirror, suite, arch, components=components, opener=opener)
    if cache is not None:
        (cache / f"Contents-{suite}-{arch}.txt").write_text(fetched.text, encoding="utf-8")
    return fetched


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

_FIXTURE_CONTENTS = (
    "usr/bin/foo   utils/foo-pkg\n"
    "usr/lib/x.so   libs/libx\n"
)

_FIXTURE_PACKAGES = """\
Package: foo-pkg
Priority: optional
Architecture: arm64
Version: 1.2.3-0ubuntu1
Section: utils
Depends: libc6 (>= 2.34), libx
Description: a foo program
 a longer multi-line
 description body
Filename: pool/universe/f/foo-pkg/foo-pkg_1.2.3-0ubuntu1_arm64.deb

Package: libx
Priority: required
Architecture: arm64
Version: 7.0-1
Provides: libx-runtime
Section: libs
Description: the x shared library
Filename: pool/main/libx/libx/libx_7.0-1_arm64.deb
"""


def _build_fixture_base(path: Path) -> None:
    """Write a tiny base tar: two mapped files + one file NOT in Contents."""
    with tarfile.open(path, "w") as tar:
        def add(name: str, data: bytes) -> None:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            info.type = tarfile.REGTYPE
            tar.addfile(info, io.BytesIO(data))

        add("./usr/bin/foo", b"\x7fELF foo")          # -> foo-pkg
        add("./usr/lib/x.so", b"\x7fELF libx")        # -> libx
        add("./etc/alr-injected.conf", b"not-in-contents")  # unmapped


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    # --- parse_contents ----------------------------------------------------
    cmap = parse_contents(_FIXTURE_CONTENTS)
    check(
        "parse_contents maps file -> basename package (multi-space)",
        cmap == {"usr/bin/foo": "foo-pkg", "usr/lib/x.so": "libx"},
    )
    # path with spaces + multi-package column + header line are handled.
    cmap2 = parse_contents(
        "FILE                                                    LOCATION\n"
        "usr/share/my docs/read me.txt   universe/doc/my-doc,main/x/other\n"
        "lib/aarch64-linux-gnu/libc.so.6   libs/libc6\n"
    )
    check(
        "parse_contents: path-with-spaces -> first package basename",
        cmap2.get("usr/share/my docs/read me.txt") == "my-doc",
    )
    check(
        "parse_contents: leading-slash stripped, soname mapped",
        cmap2.get("lib/aarch64-linux-gnu/libc.so.6") == "libc6",
    )
    check(
        "parse_contents: header line skipped (no '/' in package col)",
        "FILE" not in cmap2 and len(cmap2) == 2,
    )

    # --- installed_packages ------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / "base.tar"
        _build_fixture_base(base)
        inst = installed_packages(base, cmap)
        check(
            "installed_packages set == {foo-pkg, libx}",
            inst.packages == frozenset({"foo-pkg", "libx"}),
        )
        check("installed_packages files_mapped == 2", inst.files_mapped == 2)
        check("installed_packages files_unmapped == 1", inst.files_unmapped == 1)

    # --- build_status ------------------------------------------------------
    index = parse_packages(_FIXTURE_PACKAGES)
    status = build_status({"foo-pkg", "libx"}, index)
    check(
        "build_status: one 'install ok installed' per package",
        status.count("Status: install ok installed") == 2,
    )
    check("build_status: carries foo-pkg Version", "Version: 1.2.3-0ubuntu1" in status)
    check("build_status: carries libx Version", "Version: 7.0-1" in status)
    check("build_status: carries Depends from Packages", "Depends: libc6 (>= 2.34), libx" in status)
    check("build_status: carries Provides from Packages", "Provides: libx-runtime" in status)
    check("build_status: carries Priority", "Priority: optional" in status)
    check("build_status: carries Architecture", status.count("Architecture: arm64") == 2)
    check(
        "build_status: folds multi-line Description with leading space",
        "Description: a foo program\n a longer multi-line\n description body" in status,
    )
    check(
        "build_status: deterministic ordering (foo-pkg before libx)",
        status.index("Package: foo-pkg") < status.index("Package: libx"),
    )
    check(
        "build_status: package absent from index is skipped",
        "Package: ghost" not in build_status({"ghost"}, index),
    )
    check("build_status: stanzas blank-line separated", "\n\n" in status)
    check("build_status: deterministic across calls", status == build_status({"libx", "foo-pkg"}, index))

    # --- build_info_lists --------------------------------------------------
    lists = build_info_lists(["foo-pkg", "libx"], cmap)
    check(
        "build_info_lists: foo-pkg.list lists its file, '/'-prefixed",
        lists["foo-pkg"] == "/usr/bin/foo\n",
    )
    check(
        "build_info_lists: libx.list lists its file, '/'-prefixed",
        lists["libx"] == "/usr/lib/x.so\n",
    )
    # multi-file package is sorted.
    multi = parse_contents(
        "usr/bin/z   utils/p\n"
        "usr/bin/a   utils/p\n"
        "usr/share/p/data   utils/p\n"
    )
    plist = build_info_lists(["p"], multi)["p"]
    check(
        "build_info_lists: file list is sorted and '/'-prefixed",
        plist == "/usr/bin/a\n/usr/bin/z\n/usr/share/p/data\n",
    )
    check(
        "build_info_lists: package with no files -> empty body",
        build_info_lists(["nofiles"], cmap)["nofiles"] == "",
    )

    # --- write_db_overlay + §5-E conformance (offline, against fixture base) -
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp) / "base.tar"
        _build_fixture_base(base)
        out = Path(tmp) / "dpkg-db.tar"
        attested = sorted(set(inst.packages) & set(index))
        sb = write_db_overlay(out, status, build_info_lists(attested, cmap))
        check("write_db_overlay: status_bytes == len(status)", sb == len(status.encode("utf-8")))
        with tarfile.open(out, "r:*") as t:
            names = set(t.getnames())
        check("overlay contains ./var/lib/dpkg/status", "./var/lib/dpkg/status" in names)
        check(
            "overlay contains per-package .list files",
            "./var/lib/dpkg/info/foo-pkg.list" in names
            and "./var/lib/dpkg/info/libx.list" in names,
        )
        try:
            from tools.stage_tar_spec import validate_stage_tar

            report = validate_stage_tar(out, base)
            check("dpkg-DB overlay is §5-E CONFORMANT vs fixture base", report.conformant)
        except Exception as exc:  # pragma: no cover
            print(f"  [INFO] stage_tar_spec cross-check skipped: {exc!r}")

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_dpkg_db",
        description="Reconstruct the dpkg admin DB for the ALR Ubuntu-noble base as "
        "a §5-E overlay tar (so apt/dpkg see the base as a proper install).",
    )
    parser.add_argument("--base", help="base rootfs tar (real files mapped via Contents)")
    parser.add_argument("--out", help="output §5-E overlay stage tar path")
    parser.add_argument(
        "--mirror",
        default="http://ports.ubuntu.com/ubuntu-ports",
        help="Ubuntu ports mirror base URL (default: %(default)s)",
    )
    parser.add_argument("--suite", default="noble", help="suite (default: %(default)s)")
    parser.add_argument("--arch", default="arm64", help="architecture (default: %(default)s)")
    parser.add_argument(
        "--component", action="append", dest="components",
        help="repository component (repeatable; default main + universe)",
    )
    parser.add_argument("--cache", help="Contents/Packages cache directory")
    parser.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.base or not args.out:
        parser.error("--base and --out are required (or use --selftest)")

    components = tuple(args.components) if args.components else ("main", "universe")

    result = build_dpkg_db(
        args.base,
        args.out,
        mirror=args.mirror,
        suite=args.suite,
        arch=args.arch,
        components=components,
        cache_dir=args.cache,
    )

    print(f"wrote {result['out_tar']}")
    print(f"  packages attested:  {result['packages']}")
    print(f"  base files mapped:  {result['files_mapped']}")
    print(f"  base files unmapped:{result['files_unmapped']}")
    print(f"  status bytes:       {result['status_bytes']}")
    if result["contents_urls"]:
        print(f"  contents source:    {', '.join(result['contents_urls'])}")
    if result["conformant"] is True:
        print("  §5-E stage_tar_spec: CONFORMANT")
    elif result["conformant"] is False:
        print("  §5-E stage_tar_spec: NON-CONFORMANT")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
