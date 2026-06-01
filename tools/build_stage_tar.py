"""Build a §5-E "flat SONAME" overlay stage tar from an extracted Debian root.

Background — why flattening is the build-time fix
-------------------------------------------------
The base rootfs ships each shared library as a *single real file* named exactly
``libNAME.so.MAJOR`` (e.g. ``libharfbuzz.so.0`` IS the real harfbuzz binary — no
symlink, no versioned sibling). The Debian package layout instead ships::

    libNAME.so.MAJOR          -> libNAME.so.MAJOR.MINOR.PATCH   (symlink, the SONAME)
    libNAME.so.MAJOR.MINOR.PATCH                                (real file)
    libNAME.so                -> libNAME.so.MAJOR               (dev symlink)

Extracting that Debian-layout overlay over the flat base *repoints* the SONAME
to an older versioned file = a silent downgrade (the harfbuzz 8.3.0 -> 6.0.0
regression the overlay guard now blocks). ``tools.overlay_guard`` rejects such
overlays at apply time; this tool produces the conformant shape at BUILD time so
they never need rejecting:

    pick the highest-version REAL file in each (dir, soname) group, emit it ONCE
    renamed to the bare SONAME ``libNAME.so.MAJOR``, and DROP the versioned
    filename(s) and every SONAME / dev symlink.

A sidecar ``<out_tar>.lib-versions.json`` records the real MAJOR.MINOR.PATCH each
flattened SONAME came from, so the guard can do reliable flat-over-flat version
comparisons later (closing the "flat overlay version unverifiable from filename"
gap noted in overlay_guard.scan_overlay_violations).

This module reuses :func:`tools.overlay_guard.parse_solib` as the canonical
SONAME parser — the same (stem, soname, version) decomposition.
"""

from __future__ import annotations

import argparse
import bz2
import gzip
import io
import json
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

from tools.overlay_guard import SoLib, parse_solib


@dataclass(frozen=True)
class BuildResult:
    out_tar: str
    sidecar: str
    file_count: int
    flattened: tuple[str, ...]      # directory-scoped soname keys flattened
    dropped_symlinks: int


# --------------------------------------------------------------------------- #
# path safety (mirrors tools.safe_tar._is_safe_relative_path)
# --------------------------------------------------------------------------- #

def _is_safe_relative_path(value: str) -> bool:
    candidate = PurePosixPath(value)
    return bool(value) and not candidate.is_absolute() and ".." not in candidate.parts


def _rel_posix(root: Path, full: Path) -> str:
    return full.relative_to(root).as_posix()


def _ver_le(a: tuple, b: tuple) -> bool:
    """a <= b with zero-padding so (0, 5) and (0, 5, 0) compare equal."""
    width = max(len(a), len(b))
    return a + (0,) * (width - len(a)) <= b + (0,) * (width - len(b))


def _fmt_version(version: tuple) -> str:
    # Render at least MAJOR.MINOR.PATCH so the sidecar is stable for the guard.
    parts = list(version)
    while len(parts) < 3:
        parts.append(0)
    return ".".join(str(v) for v in parts)


# --------------------------------------------------------------------------- #
# walk + classification
# --------------------------------------------------------------------------- #

@dataclass
class _SoGroup:
    """All filesystem entries that map to one (dir, soname)."""
    soname: str
    directory: str                          # POSIX dir, "" for root
    real_versioned: list[tuple[tuple, str, Path]] = field(default_factory=list)
    # (version, rel_path, full_path) for REAL versioned files (len(version) > 1)
    real_flat: list[tuple[tuple, str, Path]] = field(default_factory=list)
    # (version, rel_path, full_path) for REAL bare-soname files (len(version)==1)
    symlinks: list[str] = field(default_factory=list)   # rel paths of symlinks


def _soname_key(directory: str, soname: str) -> str:
    return f"{directory}|{soname}"


def _classify(src_root: Path):
    """Walk src_root, returning (so_groups, plain_files, dev_symlinks).

    so_groups: dict[key -> _SoGroup]  (versioned/flat .so files + their symlinks)
    plain_files: list[(rel_path, full_path)]  non-library regular files
    dev_symlinks: list[(rel_path, linkname)]  symlinks NOT part of any so group
                  (e.g. libfoo.so dev link, or arbitrary tree symlinks)
    """
    so_groups: dict[str, _SoGroup] = {}
    plain_files: list[tuple[str, Path]] = []
    dev_symlinks: list[tuple[str, str]] = []

    # libNAME.so (no version) dev symlinks: parse_solib returns None for them, so
    # track them separately and drop any whose stem matches a flattened group.
    pending_dev: list[tuple[str, str, Path]] = []   # (rel, stem, full)

    for root, _dirs, files in os.walk(src_root):
        for fname in files:
            full = Path(root) / fname
            rel = _rel_posix(src_root, full)

            if not _is_safe_relative_path(rel):
                # paranoia: os.walk under a clean root cannot produce these,
                # but reject defensively rather than emit an unsafe member.
                continue

            directory = PurePosixPath(rel).parent.as_posix()
            if directory == ".":
                directory = ""

            is_symlink = full.is_symlink()
            lib = parse_solib(fname)

            if lib is not None:
                key = _soname_key(directory, lib.soname)
                group = so_groups.get(key)
                if group is None:
                    group = _SoGroup(soname=lib.soname, directory=directory)
                    so_groups[key] = group
                if is_symlink:
                    group.symlinks.append(rel)
                elif lib.is_flat:
                    group.real_flat.append((lib.version, rel, full))
                else:
                    group.real_versioned.append((lib.version, rel, full))
                continue

            # not a versioned .so name
            if is_symlink:
                # could be a "libNAME.so" dev symlink (stem matches a group),
                # or an arbitrary symlink in the tree.
                stem = PurePosixPath(fname).name
                if stem.endswith(".so"):
                    pending_dev.append((rel, f"{directory}|{stem}", full))
                else:
                    dev_symlinks.append((rel, os.readlink(full)))
            else:
                plain_files.append((rel, full))

    # Decide pending dev symlinks: a libNAME.so dev symlink for a library we will
    # flatten is dropped (§5-E keeps no dev links); any other ".so" symlink that
    # does not correspond to a flattened group is kept as a normal symlink.
    flattened_stems: set[str] = set()
    for key, group in so_groups.items():
        directory = group.directory
        stem = group.soname.rsplit(".", 1)[0]   # "libfoo.so.1" -> "libfoo.so"
        flattened_stems.add(f"{directory}|{stem}")

    extra_dropped_dev: list[str] = []
    for rel, dev_key, full in pending_dev:
        if dev_key in flattened_stems:
            extra_dropped_dev.append(rel)        # drop: dev link to flattened lib
        else:
            dev_symlinks.append((rel, os.readlink(full)))

    return so_groups, plain_files, dev_symlinks, extra_dropped_dev


# --------------------------------------------------------------------------- #
# build
# --------------------------------------------------------------------------- #

def build_stage_tar(
    src_root: str | Path,
    out_tar: str | Path,
    *,
    include=None,
    versions: dict[str, str] | None = None,
) -> BuildResult:
    """Flatten an extracted Debian root into a §5-E overlay stage tar.

    Parameters
    ----------
    src_root:
        Directory tree (e.g. ``dpkg-deb -x`` / inner ``data.tar`` extraction).
    out_tar:
        Output tar path. Members are ``./``-rooted (``./usr/bin/foo``).
    include:
        Optional iterable of rootfs-relative path prefixes (POSIX, no leading
        ``./``); when given, only files under one of those prefixes are emitted.
    versions:
        Optional ``{soname: "MAJOR.MINOR.PATCH"}`` override (e.g. parsed from the
        dpkg control ``Version:``) used for the sidecar instead of the filename
        version. Keyed by bare soname (``libfoo.so.1``); applies to every dir.

    Returns
    -------
    BuildResult
    """
    src_root = Path(src_root).resolve()
    if not src_root.is_dir():
        raise NotADirectoryError(f"src_root is not a directory: {src_root}")

    out_path = Path(out_tar)
    sidecar_path = Path(str(out_path) + ".lib-versions.json")

    include_prefixes = None
    if include is not None:
        include_prefixes = tuple(
            p.lstrip("./").rstrip("/") for p in include if p
        )

    def included(rel: str) -> bool:
        if include_prefixes is None:
            return True
        return any(
            rel == pref or rel.startswith(pref + "/") for pref in include_prefixes
        )

    so_groups, plain_files, dev_symlinks, extra_dropped_dev = _classify(src_root)

    sidecar: dict[str, str] = {}
    flattened: list[str] = []
    dropped_symlinks = 0
    file_count = 0

    out_path.parent.mkdir(parents=True, exist_ok=True)

    with tarfile.open(out_path, "w") as tar:
        # 1. plain (non-library) files — copy as-is, mode preserved.
        for rel, full in sorted(plain_files):
            if not included(rel):
                continue
            _add_real_file(tar, rel, full)
            file_count += 1

        # 2. shared-library groups — flatten to the bare SONAME real file.
        for key in sorted(so_groups):
            group = so_groups[key]
            directory = group.directory
            soname = group.soname
            soname_rel = f"{directory}/{soname}" if directory else soname

            if not included(soname_rel):
                # still account for symlinks we are dropping inside the filter
                dropped_symlinks += len(group.symlinks)
                continue

            # every symlink in the group is dropped (SONAME links repoint base).
            dropped_symlinks += len(group.symlinks)

            # choose the REAL source file: highest-version versioned file wins;
            # fall back to a flat real file if no versioned file exists.
            chosen_version: tuple | None = None
            chosen_full: Path | None = None
            for version, _rel, full in group.real_versioned:
                if chosen_version is None or not _ver_le(version, chosen_version):
                    chosen_version, chosen_full = version, full
            if chosen_full is None:
                for version, _rel, full in group.real_flat:
                    if chosen_version is None or not _ver_le(version, chosen_version):
                        chosen_version, chosen_full = version, full

            if chosen_full is None:
                # soname group with only symlinks and no real file: nothing to
                # emit; the symlinks were dropped above. (Dangling SONAME.)
                continue

            _add_real_file(tar, soname_rel, chosen_full, override_name=soname_rel)
            file_count += 1
            flattened.append(key)

            # sidecar: prefer an explicit version override for this soname.
            override = versions.get(soname) if versions else None
            sidecar[soname_rel] = override if override else _fmt_version(chosen_version)

        # 3. arbitrary (non-library) tree symlinks — keep, but count dropped dev
        # links separately. Drop absolute / .. targets defensively.
        dropped_symlinks += len(extra_dropped_dev)
        for rel, linkname in sorted(dev_symlinks):
            if not included(rel):
                continue
            if not _is_safe_relative_path(linkname):
                dropped_symlinks += 1
                continue
            _add_symlink(tar, rel, linkname)
            file_count += 1

    sidecar_path.parent.mkdir(parents=True, exist_ok=True)
    sidecar_path.write_text(json.dumps(sidecar, indent=2, sort_keys=True) + "\n")

    return BuildResult(
        out_tar=str(out_path),
        sidecar=str(sidecar_path),
        file_count=file_count,
        flattened=tuple(sorted(flattened)),
        dropped_symlinks=dropped_symlinks,
    )


def _member_name(rel: str) -> str:
    """Rootfs-relative -> ``./``-rooted tar member name."""
    rel = rel.lstrip("/")
    return "./" + rel if not rel.startswith("./") else rel


def _add_real_file(
    tar: tarfile.TarFile,
    rel: str,
    full: Path,
    *,
    override_name: str | None = None,
) -> None:
    name = _member_name(override_name if override_name is not None else rel)
    st = full.stat()  # follows symlink: we always resolve to the real target
    info = tarfile.TarInfo(name)
    info.size = st.st_size
    info.mode = st.st_mode & 0o7777
    info.mtime = int(st.st_mtime)
    info.type = tarfile.REGTYPE
    with full.open("rb") as handle:
        tar.addfile(info, handle)


def _add_symlink(tar: tarfile.TarFile, rel: str, target: str) -> None:
    info = tarfile.TarInfo(_member_name(rel))
    info.type = tarfile.SYMTYPE
    info.linkname = target
    tar.addfile(info)


# --------------------------------------------------------------------------- #
# .deb convenience extractor (Debian-env path; NOT used by --selftest)
# --------------------------------------------------------------------------- #

def extract_deb(deb_path: str | Path, dest_dir: str | Path) -> Path:
    """Best-effort: crack a .deb with ``ar`` then extract its inner ``data.tar``.

    Returns the destination directory. The .deb is an ``ar`` archive containing
    ``debian-binary``, ``control.tar.*`` and ``data.tar.{xz,gz,bz2,zst}``. We use
    the system ``ar`` to list/extract the data member, then decompress it with
    python stdlib. ``.zst`` data members raise NotImplementedError with a clear
    note (no stdlib zstd in this environment; do the extraction in the Debian env
    with ``dpkg-deb -x`` instead).
    """
    deb_path = Path(deb_path)
    dest = Path(dest_dir)
    dest.mkdir(parents=True, exist_ok=True)

    listing = subprocess.run(
        ["ar", "t", str(deb_path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()
    data_members = [m for m in listing if m.startswith("data.tar")]
    if not data_members:
        raise ValueError(f"no data.tar member in {deb_path} (ar t: {listing})")
    data_name = data_members[0]

    # extract the data member via ar, then decompress in-python (xz/gz/bz2) or via
    # the `zstd` CLI (Ubuntu .deb use data.tar.zst — the ALR base is Ubuntu noble).
    blob = subprocess.run(
        ["ar", "p", str(deb_path), data_name],
        check=True,
        capture_output=True,
    ).stdout

    if data_name.endswith(".xz"):
        raw = lzma.decompress(blob)
    elif data_name.endswith(".gz"):
        raw = gzip.decompress(blob)
    elif data_name.endswith(".bz2"):
        raw = bz2.decompress(blob)
    elif data_name.endswith(".zst"):
        zstd = shutil.which("zstd")
        if zstd is None:
            raise NotImplementedError(
                f"{deb_path}: data member {data_name} is zstd-compressed and no "
                "`zstd` CLI is on PATH (no stdlib zstd). Install zstd or extract "
                "this .deb in the Debian build env."
            )
        raw = subprocess.run(
            [zstd, "-d", "-c"], input=blob, check=True, capture_output=True
        ).stdout
    elif data_name.endswith(".tar"):
        raw = blob
    else:
        raise NotImplementedError(f"unhandled data member compression: {data_name}")

    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tar:
        # honor the same member-safety rules the rest of the pipeline uses.
        safe = []
        for m in tar.getmembers():
            if m.ischr() or m.isblk() or m.isfifo():
                continue
            if not _is_safe_relative_path(m.name):
                continue
            safe.append(m)
        _safe_extractall(tar, dest, safe)
    return dest


def _safe_extractall(tar: tarfile.TarFile, dest: Path, members) -> None:
    dest = dest.resolve()
    for m in members:
        target = (dest / m.name).resolve()
        if not str(target).startswith(str(dest)):
            continue  # path escapes destination — skip
        tar.extract(m, dest, set_attrs=True)


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        src = tmp_path / "src"
        libdir = src / "usr/lib/aarch64-linux-gnu"
        libdir.mkdir(parents=True)
        (src / "usr/bin").mkdir(parents=True)

        # regular exec file
        app = src / "usr/bin/app"
        app.write_bytes(b"\x7fELF app payload")
        app.chmod(0o755)

        # the Debian library layout: real versioned file + SONAME symlink + dev link
        real = libdir / "libfoo.so.1.2.3"
        real.write_bytes(b"FOO-1.2.3" * 64)
        real.chmod(0o644)
        os.symlink("libfoo.so.1.2.3", libdir / "libfoo.so.1")   # SONAME symlink
        os.symlink("libfoo.so.1", libdir / "libfoo.so")          # dev symlink

        out_tar = tmp_path / "stage.tar"
        result = build_stage_tar(src, out_tar)

        # inspect produced tar
        with tarfile.open(out_tar, "r:*") as tar:
            members = {m.name: m for m in tar.getmembers()}

        soname_member = "./usr/lib/aarch64-linux-gnu/libfoo.so.1"
        versioned = "./usr/lib/aarch64-linux-gnu/libfoo.so.1.2.3"
        devlink = "./usr/lib/aarch64-linux-gnu/libfoo.so"

        check("tar contains ./usr/bin/app", "./usr/bin/app" in members)
        check("app mode preserved (0755)", members.get("./usr/bin/app") is not None
              and (members["./usr/bin/app"].mode & 0o777) == 0o755)
        check("tar contains flat real ./usr/lib/.../libfoo.so.1",
              soname_member in members)
        check("flat libfoo.so.1 is a REAL file (not symlink)",
              soname_member in members and members[soname_member].isfile())
        check("flat libfoo.so.1 carries the real 1.2.3 bytes",
              soname_member in members and members[soname_member].size == real.stat().st_size)
        check("tar does NOT contain versioned libfoo.so.1.2.3",
              versioned not in members)
        check("tar does NOT contain dev symlink libfoo.so", devlink not in members)
        check("no symlink members at all",
              all(not m.issym() for m in members.values()))

        # sidecar
        sidecar = json.loads(Path(result.sidecar).read_text())
        key = "usr/lib/aarch64-linux-gnu/libfoo.so.1"
        check("sidecar maps soname -> 1.2.3", sidecar.get(key) == "1.2.3")

        # BuildResult bookkeeping
        check("BuildResult.file_count == 2 (app + flat lib)", result.file_count == 2)
        check("BuildResult.flattened lists the soname group",
              result.flattened == ("usr/lib/aarch64-linux-gnu|libfoo.so.1",))
        check("BuildResult.dropped_symlinks == 2 (SONAME + dev link)",
              result.dropped_symlinks == 2)

        # version override path
        out_tar2 = tmp_path / "stage2.tar"
        result2 = build_stage_tar(src, out_tar2, versions={"libfoo.so.1": "1.9.9"})
        sidecar2 = json.loads(Path(result2.sidecar).read_text())
        check("version override applied to sidecar", sidecar2.get(key) == "1.9.9")

        # cross-check against overlay_guard: the produced (flat) tar must NOT
        # violate an empty base.
        try:
            from tools.overlay_guard import scan_overlay_violations
            empty_base = tmp_path / "empty-base.tar"
            with tarfile.open(empty_base, "w") as t:
                pass
            violations = scan_overlay_violations(empty_base, out_tar)
            check("produced tar passes overlay_guard vs empty base (no violations)",
                  violations == [])

            # And: the produced flat tar must NOT be flagged as a downgrade when
            # the base already provides the same flat soname at an equal/older
            # version — it should be at worst a WARN (ambiguous), never a BLOCK.
            flat_base = tmp_path / "flat-base.tar"
            with tarfile.open(flat_base, "w") as t:
                info = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libfoo.so.1")
                payload = b"OLD-FOO" * 8
                info.size = len(payload)
                t.addfile(info, io.BytesIO(payload))
            v2 = scan_overlay_violations(flat_base, out_tar)
            check("flat-over-flat is never a BLOCK from our output",
                  all(viol.severity != "block" for viol in v2))
        except Exception as exc:  # pragma: no cover - guard import optional
            print(f"  [INFO] overlay_guard cross-check skipped: {exc!r}")

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_stage_tar",
        description="Flatten an extracted Debian root into a §5-E overlay stage tar.",
    )
    parser.add_argument("--src", help="extracted Debian root directory")
    parser.add_argument("--out", help="output stage tar path")
    parser.add_argument(
        "--versions",
        help="JSON file with {soname: 'MAJOR.MINOR.PATCH'} version overrides",
    )
    parser.add_argument(
        "--include",
        action="append",
        help="only emit files under this rootfs-relative prefix (repeatable)",
    )
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.src or not args.out:
        parser.error("--src and --out are required (or use --selftest)")

    versions = None
    if args.versions:
        versions = json.loads(Path(args.versions).read_text())

    result = build_stage_tar(
        args.src, args.out, include=args.include, versions=versions
    )
    print(f"wrote {result.out_tar}")
    print(f"  sidecar:          {result.sidecar}")
    print(f"  files:            {result.file_count}")
    print(f"  flattened sonames:{len(result.flattened)}")
    for key in result.flattened:
        print(f"    - {key}")
    print(f"  dropped symlinks: {result.dropped_symlinks}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
