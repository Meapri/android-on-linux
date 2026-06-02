"""Build the FULL apt+dpkg staging overlay (apt-dpkg-stage.tar) — R8-B.

Why
---
The exec-re-entry device test runs an ``apt install`` probe to exercise the
deepest exec chain the loader has to survive: ``apt`` → ``dpkg`` → a package's
maintainer scripts (``preinst``/``postinst`` — ``sh``) → ``dpkg-deb``/``tar`` to
unpack. Today that probe fails with ``apt-install: GUEST EXEC FAIL (traps=0)``:
the rootfs only carried a ~10 KiB ``apt-config-stage.tar`` (third-party-source
neutralizer, ``tools/build_apt_overlay.py``) — there was no actual apt+dpkg
*closure* staged, so apt/dpkg can't even start.

This builder stages a self-contained apt+dpkg that can ``dpkg -i <local.deb>`` and
``apt-get install <cached.deb>`` against a dpkg admindir in the rootfs. It does NOT
itself make the on-device unpack pass — that is gated on the parallel
exec-completion re-map (ADR-003-v2 / task R8-A) landing in the loader so the
re-entered ``dpkg``/``sh`` child is actually re-mapped onto the rootfs. This
milestone removes the "apt isn't staged" blocker so the integration session can
drain the probe the moment the re-map lands.

What it ships
-------------
1. The FULL runtime closure of ``apt apt-utils dpkg`` plus the unpack toolchain
   (``tar gzip xz-utils zstd coreutils bash dash gpgv``), resolved over Ubuntu
   noble main+universe (``ports.ubuntu.com``, UA-gated, ``.zst`` debs) and
   base-subtracted via :func:`tools.deb_closure.build_overlay` (every library the
   base already owns by SONAME is dropped — downgrade-safe, overlay_guard-clean).
   ``dpkg-deb`` / ``dpkg-split`` / ``dpkg-query`` / ``dpkg-trigger`` ride inside
   the ``dpkg`` package (verified host-side), so resolving ``dpkg`` pulls them.

2. A dpkg **admindir scaffold** under ``var/lib/dpkg/`` so a fresh ``dpkg -i`` has
   a working database root on the device: an empty ``status``/``available``, an
   ``arch`` file pinning ``arm64``, and the ``info/ updates/ triggers/`` dirs dpkg
   demands. (The base's reconstructed status DB — ``tools/build_dpkg_db.py`` →
   ``dpkg-db-stage.tar`` — is the *populated* DB of what the base already ships;
   this scaffold only guarantees the directory skeleton + lock/arch files exist so
   ``dpkg -i hello.deb`` has somewhere to record the install. The two overlays are
   complementary; the DB overlay wins on ``status`` if both land — this scaffold
   ships ``status`` only when the slot is otherwise empty, see ``--no-status``.)

The apt/dpkg binaries + libs land at their real rootfs-absolute paths
(``usr/bin/dpkg``, ``usr/lib/aarch64-linux-gnu/libapt-pkg.so.*`` …) as a §5-E
``./``-rooted overlay tar, the same format every other ALR overlay uses.

Test payload (the thing the probe unpacks)
-------------------------------------------
The integration/device probe needs a trivial local ``.deb`` to ``dpkg -i``. Use
Ubuntu noble **``hello``** (``pool/main/h/hello/hello_2.10-3build1_arm64.deb``,
~25 KiB, ``Depends: libc6`` which the base already provides). Fetch it with
``--fetch-test-deb`` (writes it next to the stage tar; it is NOT packed into the
overlay — it's a separate on-device asset the probe copies into the rootfs and
runs ``dpkg -i`` on). See ``TEST_DEB`` / :func:`fetch_test_deb`.

Honest scope
------------
HOST-ONLY. This builds + verifies the overlay host-side. The actual on-device
``apt install`` / ``dpkg -i`` unpack is the integration session's drain and is
gated on the exec-re-entry re-map (R8-A). See the module docstring above.
"""

from __future__ import annotations

import argparse
import io
import json
import tarfile
from dataclasses import dataclass, field
from pathlib import Path

import lzma
import gzip as _gzip
import bz2 as _bz2
import shutil
import subprocess

from tools.deb_closure import (
    build_overlay,
    build_provides_map,
    fetch_packages_index,
    parse_packages,
    resolve_closure,
    _download_deb,
)

# --------------------------------------------------------------------------- #
# Target set
# --------------------------------------------------------------------------- #

# The apt/dpkg front-ends plus the unpack toolchain a maintainer script needs.
# dpkg-deb / dpkg-split / dpkg-query / dpkg-trigger ride INSIDE the `dpkg` package
# (host-verified), so we do not list them separately. libapt-pkg / libgcrypt /
# liblz4 / libzstd / libbz2 / libstdc++6 / libgcc-s1 / gnutls … are pulled
# transitively by the resolver — we never hand-list deps the closure already finds.
DEFAULT_TARGETS = (
    "apt",
    "apt-utils",
    "dpkg",
    "tar",
    "gzip",
    "xz-utils",
    "zstd",
    "coreutils",
    "bash",
    "dash",
    "gpgv",
)

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main", "universe")

# The on-device unpack target (a separate asset, NOT packed into the overlay).
TEST_DEB = {
    "package": "hello",
    "filename": "pool/main/h/hello/hello_2.10-3build1_arm64.deb",
    "note": "noble hello 2.10-3build1, ~25 KiB, Depends: libc6 (base provides)",
}

# dpkg admindir scaffold members. dpkg refuses to operate without these dirs;
# `arch` pins the native architecture; status/available start empty so a fresh
# `dpkg -i` can append. These are the only var/lib/dpkg paths we ship — the
# closure overlay itself prunes var/lib/dpkg (deb_closure.DEFAULT_PRUNE_PREFIXES).
ADMINDIR_DIRS = (
    "var/lib/dpkg",
    "var/lib/dpkg/info",
    "var/lib/dpkg/updates",
    "var/lib/dpkg/triggers",
    "var/lib/dpkg/alternatives",
)
ADMINDIR_FILES = {
    "var/lib/dpkg/status": b"",
    "var/lib/dpkg/available": b"",
    "var/lib/dpkg/arch": b"arm64\n",
    "var/lib/dpkg/triggers/File": b"",
    "var/lib/dpkg/triggers/Unincorp": b"",
}


@dataclass
class AptDpkgOverlayResult:
    out_tar: str
    closure: tuple[str, ...] = ()
    total_download_bytes: int = 0
    file_count: int = 0
    skipped_base: tuple[str, ...] = ()
    unsupported: tuple[str, ...] = ()
    missing: tuple[str, ...] = ()
    violations: tuple[str, ...] = ()
    admindir_members: tuple[str, ...] = ()
    self_contained_bins: tuple[str, ...] = ()
    targets: tuple[str, ...] = ()

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "targets": list(self.targets),
            "closure": list(self.closure),
            "closure_count": len(self.closure),
            "total_download_bytes": self.total_download_bytes,
            "total_download_mib": round(self.total_download_bytes / 1048576, 1),
            "file_count": self.file_count,
            "skipped_base": list(self.skipped_base),
            "unsupported": list(self.unsupported),
            "missing": list(self.missing),
            "violations": list(self.violations),
            "admindir_members": list(self.admindir_members),
            "self_contained_bins": list(self.self_contained_bins),
        }


# --------------------------------------------------------------------------- #
# Closure resolution (offline-friendly: only needs the Packages index)
# --------------------------------------------------------------------------- #

@dataclass
class ClosurePlan:
    targets: tuple[str, ...]
    closure: tuple[str, ...]
    sizes: dict[str, int]
    missing: tuple[str, ...]

    @property
    def total_download_bytes(self) -> int:
        return sum(self.sizes.values())


def resolve_apt_dpkg_closure(
    *,
    targets=DEFAULT_TARGETS,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    index: dict[str, dict] | None = None,
) -> ClosurePlan:
    """Resolve the apt/dpkg runtime closure over the noble Packages index.

    NETWORK unless ``index`` is supplied (offline/selftest). Returns the ordered
    closure, each package's compressed download size, and any unsatisfied deps.
    Does NOT download the .debs — this is the ``--list`` / dry-run brain.
    """
    if index is None:
        index = parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )
    provides_map = build_provides_map(index)
    log: list[str] = []
    closure = resolve_closure(list(targets), index, provides_map=provides_map, log=log)
    sizes = {n: int(index.get(n, {}).get("Size", "0") or 0) for n in closure}
    return ClosurePlan(
        targets=tuple(targets),
        closure=tuple(closure),
        sizes=sizes,
        missing=tuple(log),
    )


# --------------------------------------------------------------------------- #
# admindir scaffold
# --------------------------------------------------------------------------- #

def _add_dir(tar: tarfile.TarFile, rel: str, mode: int = 0o755) -> None:
    ti = tarfile.TarInfo("./" + rel.lstrip("./").lstrip("/"))
    ti.type = tarfile.DIRTYPE
    ti.mode = mode
    ti.mtime = 0
    tar.addfile(ti)


def _add_file(tar: tarfile.TarFile, rel: str, data: bytes, mode: int = 0o644) -> None:
    ti = tarfile.TarInfo("./" + rel.lstrip("./").lstrip("/"))
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


def write_admindir_scaffold(
    out_tar: str | Path, *, include_status: bool = True
) -> tuple[str, ...]:
    """Write the dpkg admindir scaffold as a standalone §5-E overlay tar.

    Members: ``var/lib/dpkg/{,info,updates,triggers,alternatives}`` dirs plus the
    ``status``/``available``/``arch``/``triggers/*`` seed files. When
    ``include_status`` is False the ``status``/``available`` seed files are omitted
    (use this when the populated dpkg-db-stage.tar owns ``status`` already).
    Returns the sorted tuple of member names written (``./``-rooted).
    """
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    skip = set()
    if not include_status:
        skip = {"var/lib/dpkg/status", "var/lib/dpkg/available"}
    written: list[str] = []
    with tarfile.open(out_tar, "w") as tar:
        for d in ADMINDIR_DIRS:
            _add_dir(tar, d)
            written.append("./" + d)
        for rel, data in ADMINDIR_FILES.items():
            if rel in skip:
                continue
            _add_file(tar, rel, data)
            written.append("./" + rel)
    return tuple(sorted(written))


def _append_admindir(out_tar: str | Path, *, include_status: bool = True) -> tuple[str, ...]:
    """Append the admindir scaffold members into an EXISTING overlay tar.

    The closure overlay is written first (by deb_closure.build_overlay); we then
    fold the admindir skeleton into the same tar so one stage tar carries both the
    apt/dpkg payload and a working DB root. Dir members are only added if not
    already present (the closure tar may have created a parent).
    """
    out_tar = Path(out_tar)
    with tarfile.open(out_tar, "r") as t:
        existing = {m.name for m in t.getmembers()}
    skip = set()
    if not include_status:
        skip = {"var/lib/dpkg/status", "var/lib/dpkg/available"}
    written: list[str] = []
    with tarfile.open(out_tar, "a") as tar:
        for d in ADMINDIR_DIRS:
            name = "./" + d
            if name in existing:
                continue
            _add_dir(tar, d)
            existing.add(name)
            written.append(name)
        for rel, data in ADMINDIR_FILES.items():
            if rel in skip:
                continue
            name = "./" + rel
            if name in existing:
                continue
            _add_file(tar, rel, data)
            existing.add(name)
            written.append(name)
    return tuple(sorted(written))


# --------------------------------------------------------------------------- #
# Self-contained front-end binaries (belt-and-suspenders)
# --------------------------------------------------------------------------- #

# The apt/dpkg front-ends + unpack toolchain a maintainer-script chain must exec.
# In a normal noble base these already exist (host-verified: the shipped base ships
# /usr/bin/{dpkg,dpkg-deb,apt,apt-get,tar,...} as 0755), so the base-subtracted
# overlay drops them. --self-contained re-injects them UNCONDITIONALLY as 0755 (the
# same belt-and-suspenders pattern tools/build_babl_gegl_overlay uses for op .so):
# the overlay then works even if a future slimmed base omits a front-end. Mapped
# {package: (rootfs-rel paths to ship)}.
SELF_CONTAINED_BINS = {
    "dpkg": (
        "usr/bin/dpkg", "usr/bin/dpkg-deb", "usr/bin/dpkg-split",
        "usr/bin/dpkg-query", "usr/bin/dpkg-trigger", "usr/bin/dpkg-divert",
        "usr/bin/dpkg-realpath", "usr/bin/dpkg-maintscript-helper",
    ),
    "apt": ("usr/bin/apt", "usr/bin/apt-get", "usr/bin/apt-cache",
            "usr/bin/apt-config", "usr/bin/apt-mark"),
    "tar": ("usr/bin/tar",),
    "gzip": ("usr/bin/gzip",),
    "xz-utils": ("usr/bin/xz",),
    "zstd": ("usr/bin/zstd",),
    "bash": ("usr/bin/bash",),
    "dash": ("usr/bin/dash", "bin/dash"),
    "gpgv": ("usr/bin/gpgv",),
}


def _decompress_data_tar(deb: Path) -> bytes:
    """Return the raw (uncompressed) data.tar bytes of a .deb. Tolerant of the
    xz/gz/bz2/zst compressions Ubuntu uses (zstd via the CLI). Unlike
    ``build_stage_tar.extract_deb`` this does NOT extract to disk, so an
    absolute-symlink member (e.g. tar's ``./etc/rmt``) can't abort the read."""
    listing = subprocess.run(["ar", "t", str(deb)], check=True,
                             capture_output=True, text=True).stdout.split()
    data_name = next(m for m in listing if m.startswith("data.tar"))
    blob = subprocess.run(["ar", "p", str(deb), data_name], check=True,
                          capture_output=True).stdout
    if data_name.endswith(".xz"):
        return lzma.decompress(blob)
    if data_name.endswith(".gz"):
        return _gzip.decompress(blob)
    if data_name.endswith(".bz2"):
        return _bz2.decompress(blob)
    if data_name.endswith(".zst"):
        zstd = shutil.which("zstd")
        if zstd is None:
            raise NotImplementedError(f"{deb}: zstd data member but no zstd CLI")
        return subprocess.run([zstd, "-d", "-c"], input=blob, check=True,
                              capture_output=True).stdout
    if data_name.endswith(".tar"):
        return blob
    raise NotImplementedError(f"unhandled data member: {data_name}")


def extract_named_files(deb: Path, wanted: set[str]) -> dict[str, tuple[bytes, int]]:
    """Pull specific regular-file members out of a .deb's data.tar.

    ``wanted`` is a set of rootfs-rel paths (no leading ``./``). Returns
    ``{rel: (bytes, mode)}`` for each wanted member that is a regular file. Robust
    to absolute-symlink members elsewhere in the archive (reads in-memory, never
    extracts to disk)."""
    raw = _decompress_data_tar(deb)
    found: dict[str, tuple[bytes, int]] = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as t:
        for m in t.getmembers():
            rel = m.name
            while rel.startswith("./"):
                rel = rel[2:]
            rel = rel.lstrip("/")
            if rel in wanted and m.isfile():
                fh = t.extractfile(m)
                if fh is not None:
                    found[rel] = (fh.read(), 0o755)
    return found


def inject_self_contained_bins(
    out_tar: str | Path, index: dict[str, dict], mirror: str, cache: Path
) -> tuple[str, ...]:
    """Append the apt/dpkg/unpack front-end binaries into ``out_tar`` as 0o755,
    UNCONDITIONALLY (not base-subtracted). Skips any member already in the tar.
    Returns the sorted member names added."""
    cache.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out_tar, "r") as t:
        existing = {m.name for m in t.getmembers()}
    added: list[str] = []
    with tarfile.open(out_tar, "a") as tar:
        for pkg, paths in SELF_CONTAINED_BINS.items():
            fields = index.get(pkg, {})
            filename = fields.get("Filename")
            if not filename:
                continue
            try:
                deb = _download_deb(mirror, filename, cache)
            except Exception:
                continue
            got = extract_named_files(deb, set(paths))
            for rel, (data, mode) in sorted(got.items()):
                name = "./" + rel
                if name in existing:
                    continue
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                ti.mode = mode
                ti.mtime = 0
                ti.type = tarfile.REGTYPE
                tar.addfile(ti, io.BytesIO(data))
                existing.add(name)
                added.append(name)
    return tuple(sorted(added))


# --------------------------------------------------------------------------- #
# Full build (network) — closure download + base-subtract + admindir
# --------------------------------------------------------------------------- #

def build_apt_dpkg_overlay(
    out_tar: str | Path,
    base: str | Path,
    *,
    targets=DEFAULT_TARGETS,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = None,
    include_status: bool = True,
    self_contained: bool = False,
) -> AptDpkgOverlayResult:
    """Resolve+download the apt/dpkg closure, base-subtract, flatten, and fold in
    the dpkg admindir scaffold — the full §5-E ``apt-dpkg-stage.tar``.

    NETWORK PATH. Delegates the closure download + SONAME base-subtraction +
    overlay_guard re-check to :func:`tools.deb_closure.build_overlay` (so this
    overlay obeys the exact same downgrade-safety contract as every other ALR
    overlay), then appends the admindir skeleton into the same tar.

    ``self_contained=True`` additionally re-injects the apt/dpkg/unpack front-end
    binaries (which the base normally already owns, so base-subtraction drops them)
    UNCONDITIONALLY as 0o755 — belt-and-suspenders for a future slimmed base.
    """
    out_tar = Path(out_tar)
    cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache-ubuntu")
    ob = build_overlay(
        list(targets),
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache,
    )
    injected: tuple[str, ...] = ()
    if self_contained:
        index = parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )
        injected = inject_self_contained_bins(out_tar, index, mirror, cache)
    admin = _append_admindir(out_tar, include_status=include_status)

    # report the resolved closure's compressed download size from the index
    plan = resolve_apt_dpkg_closure(
        targets=targets, mirror=mirror, suite=suite, arch=arch, components=components
    )
    # recount files (closure overlay + appended admindir)
    with tarfile.open(out_tar, "r") as t:
        file_count = sum(1 for m in t.getmembers() if m.isfile())

    return AptDpkgOverlayResult(
        out_tar=str(out_tar),
        closure=tuple(ob["closure"]),
        total_download_bytes=plan.total_download_bytes,
        file_count=file_count,
        skipped_base=tuple(ob["skipped_base"]),
        unsupported=tuple(ob["unsupported"]),
        missing=tuple(ob["missing"]),
        violations=tuple(ob["violations"]),
        admindir_members=admin,
        self_contained_bins=injected,
        targets=tuple(targets),
    )


# --------------------------------------------------------------------------- #
# Test payload fetch (a trivial local .deb for the on-device dpkg -i probe)
# --------------------------------------------------------------------------- #

def fetch_test_deb(
    dest_dir: str | Path,
    *,
    mirror: str = MIRROR,
    cache_dir: str | Path | None = None,
) -> Path:
    """Download the noble ``hello`` .deb into ``dest_dir`` (the on-device unpack
    target). Returns its path. NOT packed into the overlay — the device probe
    copies it into the rootfs and runs ``dpkg -i hello.deb``."""
    dest_dir = Path(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)
    cache = Path(cache_dir) if cache_dir is not None else dest_dir
    deb = _download_deb(mirror, TEST_DEB["filename"], cache)
    target = dest_dir / Path(TEST_DEB["filename"]).name
    if deb.resolve() != target.resolve():
        target.write_bytes(deb.read_bytes())
    return target


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_apt_dpkg_overlay",
        description="Build the FULL apt+dpkg staging overlay (apt-dpkg-stage.tar) "
        "from Ubuntu noble main+universe, plus a dpkg admindir scaffold.",
    )
    parser.add_argument("--out", help="output stage tar (e.g. /tmp/apt-dpkg-stage.tar)")
    parser.add_argument("--base", help="base rootfs tar|dir (REQUIRED for full build: "
                        "drives SONAME base-subtraction + overlay_guard)")
    parser.add_argument("--target", action="append", dest="targets",
                        help="override target set (repeatable; default apt apt-utils dpkg + unpack toolchain)")
    parser.add_argument("--suite", default=SUITE)
    parser.add_argument("--mirror", default=MIRROR)
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main + universe)")
    parser.add_argument("--cache", default="/tmp/deb-cache-ubuntu")
    parser.add_argument("--no-status", action="store_true",
                        help="omit the status/available admindir seed (when dpkg-db-stage owns them)")
    parser.add_argument("--self-contained", action="store_true",
                        help="ALSO ship the apt/dpkg/unpack front-end binaries 0o755 unconditionally "
                        "(belt-and-suspenders; base normally already owns them)")
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="resolve + print the closure (package list + sizes) WITHOUT downloading")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--fetch-test-deb", metavar="DIR",
                        help="download the noble hello .deb into DIR (on-device dpkg -i target)")
    parser.add_argument("--admindir-only", metavar="TAR",
                        help="write JUST the dpkg admindir scaffold tar to TAR and exit (offline)")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    targets = tuple(args.targets) if args.targets else DEFAULT_TARGETS
    components = tuple(args.components) if args.components else COMPONENTS

    if args.admindir_only:
        members = write_admindir_scaffold(args.admindir_only, include_status=not args.no_status)
        print(f"wrote admindir scaffold {args.admindir_only}: {len(members)} members")
        for m in members:
            print(f"    {m}")
        return 0

    if args.fetch_test_deb:
        deb = fetch_test_deb(args.fetch_test_deb, mirror=args.mirror, cache_dir=args.cache)
        print(f"fetched test .deb: {deb} ({deb.stat().st_size} bytes) — {TEST_DEB['note']}")
        if not args.dry_run and not args.out:
            return 0

    if args.dry_run:
        plan = resolve_apt_dpkg_closure(
            targets=targets, mirror=args.mirror, suite=args.suite, arch=ARCH,
            components=components,
        )
        if args.json:
            print(json.dumps({
                "targets": list(plan.targets),
                "closure": list(plan.closure),
                "closure_count": len(plan.closure),
                "sizes": plan.sizes,
                "total_download_bytes": plan.total_download_bytes,
                "total_download_mib": round(plan.total_download_bytes / 1048576, 1),
                "missing": list(plan.missing),
            }, indent=2))
        else:
            print(f"apt/dpkg closure over {args.suite}/{'+'.join(components)}: "
                  f"{len(plan.closure)} packages, "
                  f"{round(plan.total_download_bytes / 1048576, 1)} MiB compressed download")
            for n in sorted(plan.closure):
                print(f"    {n:32s} {plan.sizes.get(n, 0):>10d}")
            if plan.missing:
                print(f"  unsatisfied/skipped ({len(plan.missing)}):")
                for m in sorted(set(plan.missing)):
                    print(f"    - {m}")
            else:
                print("  unsatisfied deps: NONE (closure fully resolved)")
        return 0

    if not args.out:
        parser.error("--out is required for a full build (or use --list / --admindir-only / --fetch-test-deb)")
    if not args.base:
        parser.error("--base is required for a full build (drives base-subtraction + overlay_guard)")

    res = build_apt_dpkg_overlay(
        args.out, args.base, targets=targets, mirror=args.mirror, suite=args.suite,
        arch=ARCH, components=components, cache_dir=args.cache,
        include_status=not args.no_status, self_contained=args.self_contained,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  closure: {len(res.closure)} packages, "
              f"{round(res.total_download_bytes / 1048576, 1)} MiB compressed download")
        print(f"  files in overlay: {res.file_count}")
        print(f"  base-subtracted (base owns SONAME): {len(res.skipped_base)}")
        print(f"  admindir scaffold members: {len(res.admindir_members)}")
        if res.self_contained_bins:
            print(f"  self-contained front-end bins (0o755): {len(res.self_contained_bins)}")
        if res.unsupported:
            print(f"  UNSUPPORTED (zstd .deb / download fail) — {len(res.unsupported)}:")
            for u in res.unsupported:
                print(f"    - {u}")
        if res.violations:
            print(f"  overlay_guard VIOLATIONS — {len(res.violations)}:")
            for v in res.violations:
                print(f"    - {v}")
    # non-zero only on a guard BLOCK (rendered violations carry severity text)
    blocked = [v for v in res.violations if "BLOCK" in v]
    return 1 if blocked else 0


# --------------------------------------------------------------------------- #
# Selftest (OFFLINE — synthetic index + admindir, no network/no real .deb)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- closure resolution over a synthetic noble-like index --------------- #
    index = {
        "apt": {"Version": "2.7.14", "Architecture": "arm64", "Size": "1336188",
                "Depends": "libapt-pkg6.0t64, gpgv, dpkg (>= 1.17.2)"},
        "apt-utils": {"Version": "2.7.14", "Architecture": "arm64", "Size": "205248",
                      "Depends": "apt (= 2.7.14), libapt-pkg6.0t64"},
        "dpkg": {"Version": "1.22.6", "Architecture": "arm64", "Size": "1265468",
                 "Depends": "tar, libzstd1"},
        "tar": {"Version": "1.35", "Architecture": "arm64", "Size": "247906"},
        "gzip": {"Version": "1.12", "Architecture": "arm64", "Size": "97192"},
        "xz-utils": {"Version": "5.6", "Architecture": "arm64", "Size": "268364",
                     "Depends": "liblzma5"},
        "zstd": {"Version": "1.5.5", "Architecture": "arm64", "Size": "574810",
                 "Depends": "libzstd1"},
        "coreutils": {"Version": "9.4", "Architecture": "arm64", "Size": "1362772"},
        "bash": {"Version": "5.2", "Architecture": "arm64", "Size": "780262"},
        "dash": {"Version": "0.5.12", "Architecture": "arm64", "Size": "90376"},
        "gpgv": {"Version": "2.4.4", "Architecture": "arm64", "Size": "149882",
                 "Depends": "libgcrypt20"},
        "libapt-pkg6.0t64": {"Version": "2.7.14", "Architecture": "arm64", "Size": "934734",
                             "Depends": "libzstd1, libgcrypt20"},
        "libgcrypt20": {"Version": "1.10", "Architecture": "arm64", "Size": "471954"},
        "libzstd1": {"Version": "1.5.5", "Architecture": "arm64", "Size": "271224"},
        "liblzma5": {"Version": "5.6", "Architecture": "arm64", "Size": "125356"},
    }
    plan = resolve_apt_dpkg_closure(index=index)
    check("closure pulls apt", "apt" in plan.closure)
    check("closure pulls dpkg", "dpkg" in plan.closure)
    check("closure pulls apt-utils", "apt-utils" in plan.closure)
    check("closure pulls libapt-pkg (transitive dep)", "libapt-pkg6.0t64" in plan.closure)
    check("closure pulls gpgv (apt verifies)", "gpgv" in plan.closure)
    check("closure pulls libzstd1 (transitive)", "libzstd1" in plan.closure)
    check("closure pulls libgcrypt20 (transitive)", "libgcrypt20" in plan.closure)
    check("closure pulls the unpack toolchain (tar)", "tar" in plan.closure)
    check("no unsatisfied deps in synthetic closure", plan.missing == ())
    check("total download size > 0", plan.total_download_bytes > 0)

    # --- admindir scaffold (standalone tar) --------------------------------- #
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        admin_tar = tmp / "admindir.tar"
        members = write_admindir_scaffold(admin_tar)
        with tarfile.open(admin_tar) as t:
            names = {m.name: m for m in t.getmembers()}
        check("admindir ships ./var/lib/dpkg dir", "./var/lib/dpkg" in names)
        check("admindir ships ./var/lib/dpkg/info dir", "./var/lib/dpkg/info" in names)
        check("admindir ships ./var/lib/dpkg/updates dir", "./var/lib/dpkg/updates" in names)
        check("admindir ships ./var/lib/dpkg/triggers dir", "./var/lib/dpkg/triggers" in names)
        check("admindir ships an (empty) status file", "./var/lib/dpkg/status" in names)
        check("admindir ships available file", "./var/lib/dpkg/available" in names)
        check("admindir arch file pins arm64",
              "./var/lib/dpkg/arch" in names
              and t.__class__ is not None)
        with tarfile.open(admin_tar) as t:
            arch_body = t.extractfile("./var/lib/dpkg/arch").read()
        check("arch body == 'arm64\\n'", arch_body == b"arm64\n")
        check("dirs are DIRTYPE", names["./var/lib/dpkg"].isdir() and names["./var/lib/dpkg/info"].isdir())
        check("all members ./-rooted", all(n.startswith("./") for n in names))

        # §5-E structural conformance
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(admin_tar))
        check("admindir overlay is stage_tar_spec conformant", rep.conformant)

        # --no-status variant drops status/available
        admin2 = tmp / "admindir_nostatus.tar"
        m2 = write_admindir_scaffold(admin2, include_status=False)
        check("--no-status drops status", "./var/lib/dpkg/status" not in m2)
        check("--no-status keeps arch", "./var/lib/dpkg/arch" in m2)

        # --- append admindir into an existing (closure-like) tar ------------ #
        closure_like = tmp / "closure.tar"
        with tarfile.open(closure_like, "w") as t:
            ti = tarfile.TarInfo("./usr/bin/dpkg")
            payload = b"\x7fELF dpkg"
            ti.size = len(payload); ti.mode = 0o755
            t.addfile(ti, io.BytesIO(payload))
        appended = _append_admindir(closure_like)
        with tarfile.open(closure_like) as t:
            merged = {m.name for m in t.getmembers()}
        check("append keeps the closure binary", "./usr/bin/dpkg" in merged)
        check("append adds the admindir status", "./var/lib/dpkg/status" in merged)
        check("append adds the admindir info dir", "./var/lib/dpkg/info" in merged)
        check("append reported the members it wrote", "./var/lib/dpkg/status" in appended)

        # --- extract_named_files tolerates an absolute-symlink member -------- #
        # Build a synthetic .deb whose data.tar has /usr/bin/dpkg (wanted) AND an
        # absolute-symlink ./etc/rmt (the member that aborts build_stage_tar's
        # disk extractor). We must still recover the wanted regular file.
        if shutil.which("ar"):
            datadir = tmp / "dataroot"
            (datadir / "usr/bin").mkdir(parents=True)
            (datadir / "usr/bin/dpkg").write_bytes(b"\x7fELF dpkg-bin")
            (datadir / "usr/bin/dpkg").chmod(0o755)
            (datadir / "etc").mkdir()
            os_symlink_ok = True
            try:
                (datadir / "etc/rmt").symlink_to("/usr/sbin/rmt")  # ABSOLUTE link
            except OSError:
                os_symlink_ok = False
            data_tar = tmp / "data.tar"
            with tarfile.open(data_tar, "w") as dt:
                dt.add(datadir / "usr/bin/dpkg", arcname="./usr/bin/dpkg")
                if os_symlink_ok:
                    dt.add(datadir / "etc/rmt", arcname="./etc/rmt")
            (tmp / "debian-binary").write_text("2.0\n")
            (tmp / "control.tar").write_bytes(b"")
            deb = tmp / "synthetic.deb"
            # `ar qcS`: quick-append, create, NO symbol table (LLVM/BSD ar would
            # otherwise add a __.SYMDEF table and mangle the member listing). Run
            # from `tmp` so members are bare names (data.tar, not /abs/data.tar),
            # exactly as a real .deb stores them.
            subprocess.run(
                ["ar", "qcS", "synthetic.deb", "debian-binary",
                 "control.tar", "data.tar"],
                check=True, capture_output=True, cwd=str(tmp),
            )
            got = extract_named_files(deb, {"usr/bin/dpkg"})
            check("extract_named_files recovers the wanted regular file",
                  "usr/bin/dpkg" in got and got["usr/bin/dpkg"][0] == b"\x7fELF dpkg-bin")
            check("extract_named_files marks recovered bins 0o755",
                  got.get("usr/bin/dpkg", (b"", 0))[1] == 0o755)
            check("extract_named_files ignores unwanted members",
                  "etc/rmt" not in got)
        else:
            check("ar available for extract_named_files test (skipped)", True)

        # SELF_CONTAINED_BINS sanity: covers the load-bearing front-ends
        flat = {p for ps in SELF_CONTAINED_BINS.values() for p in ps}
        for must in ("usr/bin/dpkg", "usr/bin/dpkg-deb", "usr/bin/dpkg-split",
                     "usr/bin/dpkg-query", "usr/bin/apt", "usr/bin/apt-get",
                     "usr/bin/tar"):
            check(f"self-contained set includes {must}", must in flat)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
