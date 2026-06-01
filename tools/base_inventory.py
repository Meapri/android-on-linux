"""Base rootfs inventory (WS-4 / L4 rootfs).

What this is for
----------------
Before an overlay is built or audited, you need to know what the *base* rootfs
already provides — which SONAMEs ship as real library files, which executables
live in the bin dirs, whether the base already carries xkb/fontconfig data, and
how big it is. Overlay builders then SUBTRACT that set (only ship what the base
lacks), and coverage audits reason about it.

This is the read-side companion to :mod:`tools.overlay_guard`: the guard refuses
overlays that would *downgrade* a base soname, while this module enumerates the
full base provision so a closure can be diffed against it.

The base follows the §5-E "flat SONAME" convention — a shared library is a real
regular file named exactly ``libNAME.so.MAJOR`` — so a SONAME is "provided by the
base" iff the base ships a real (non-symlink) file whose basename parses via
:func:`tools.overlay_guard.parse_solib`.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import tarfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath

from tools.overlay_guard import parse_solib
from tools.safe_tar import inspect_tar_members

# Directories whose direct (and nested) contents we count as "executables".
_BIN_DIRS: tuple[str, ...] = ("bin", "usr/bin", "sbin", "usr/sbin")

# Marker dirs/files that prove the base already carries these subsystems.
_XKB_DIR = "usr/share/X11/xkb"
_FONTCONFIG_DIRS: tuple[str, ...] = ("etc/fonts", "usr/share/fontconfig")


@dataclass(frozen=True)
class BaseInventory:
    """What the base rootfs provides, for overlay subtraction / coverage audits."""

    sonames: tuple[str, ...]      # sorted-unique SONAMEs of real .so library files
    binaries: tuple[str, ...]     # sorted-unique executable basenames under bin dirs
    lib_dirs: tuple[str, ...]     # sorted-unique dirs that contain real library files
    has_xkb: bool                 # usr/share/X11/xkb present
    has_fontconfig: bool          # etc/fonts or usr/share/fontconfig present
    file_count: int               # number of real regular files
    total_size: int               # summed bytes of real regular files

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, sort_keys=True)

    def diff_against(self, other_sonames: set[str]) -> dict:
        """Split a would-be overlay/closure soname set against the base.

        Returns ``{"already_in_base": [...], "new": [...]}`` (both sorted): the
        SONAMEs the base already provides (overlay should DROP them) versus the
        ones it genuinely adds. This is the subtraction helper overlay builders
        call to avoid re-shipping — and re-versioning — base libraries.
        """
        base = set(self.sonames)
        already = sorted(s for s in other_sonames if s in base)
        new = sorted(s for s in other_sonames if s not in base)
        return {"already_in_base": already, "new": new}

    def render(self) -> str:
        """Human-readable summary: counts plus a few sample entries (not a dump)."""

        def sample(items: tuple[str, ...], n: int = 8) -> str:
            head = ", ".join(items[:n])
            more = f", … (+{len(items) - n} more)" if len(items) > n else ""
            return head + more if items else "(none)"

        mib = self.total_size / (1024 * 1024)
        return (
            "Base rootfs inventory\n"
            f"  files:        {self.file_count}\n"
            f"  total size:   {self.total_size} bytes ({mib:.1f} MiB)\n"
            f"  sonames:      {len(self.sonames)}\n"
            f"  binaries:     {len(self.binaries)}\n"
            f"  lib dirs:     {len(self.lib_dirs)}\n"
            f"  has_xkb:      {self.has_xkb}\n"
            f"  has_fontconfig: {self.has_fontconfig}\n"
            f"  sample sonames:  {sample(self.sonames)}\n"
            f"  sample binaries: {sample(self.binaries)}\n"
            f"  sample lib dirs: {sample(self.lib_dirs)}"
        )


def _iter_members(rootfs: str | Path):
    """Yield (rel_path, kind, size) for a base tar OR an extracted directory.

    ``rel_path`` is a rootfs-relative POSIX path (no leading ``./``). ``kind`` is
    "file"/"dir"/"symlink"/"hardlink" for a tar; for a directory walk it is
    "file"/"symlink"/"dir" (only files carry a meaningful size).
    """
    path = Path(rootfs)
    if path.is_dir():
        base_dir = path.resolve()
        for root, _dirs, files in os.walk(base_dir):
            for fname in files:
                full = Path(root) / fname
                rel = full.relative_to(base_dir).as_posix()
                if full.is_symlink():
                    yield rel, "symlink", 0
                else:
                    try:
                        size = full.stat().st_size
                    except OSError:
                        size = 0
                    yield rel, "file", size
    else:
        for m in inspect_tar_members(path):
            yield m.name, m.kind, m.size


def _under_bin_dir(rel_path: str) -> bool:
    """True if ``rel_path`` lives under one of the recognised bin directories."""
    posix = PurePosixPath(rel_path)
    for bindir in _BIN_DIRS:
        bin_parts = PurePosixPath(bindir).parts
        if posix.parts[: len(bin_parts)] == bin_parts and len(posix.parts) > len(bin_parts):
            return True
    return False


def inventory_base(rootfs: str | Path) -> BaseInventory:
    """Inventory what a base rootfs (tar or extracted dir) provides.

    Classification:
      * real library file (basename parses via :func:`parse_solib` AND kind is
        "file") -> records its SONAME and its containing directory;
      * a real or hardlinked file under a bin dir -> records its basename as an
        executable (we cannot read the +x bit reliably from every source, so the
        bin-dir location is the convention used across these tools);
      * presence of the xkb / fontconfig marker dirs is detected from any member
        path (file, dir, or symlink) under them.

    Only real regular files contribute to ``file_count``/``total_size`` and to the
    soname set — symlinks never *define* a provided soname (matching the guard).
    """
    sonames: set[str] = set()
    binaries: set[str] = set()
    lib_dirs: set[str] = set()
    has_xkb = False
    has_fontconfig = False
    file_count = 0
    total_size = 0

    for rel_path, kind, size in _iter_members(rootfs):
        # Subsystem markers: any member path under the marker dir proves presence.
        if not has_xkb and (rel_path == _XKB_DIR or rel_path.startswith(_XKB_DIR + "/")):
            has_xkb = True
        if not has_fontconfig:
            for fc in _FONTCONFIG_DIRS:
                if rel_path == fc or rel_path.startswith(fc + "/"):
                    has_fontconfig = True
                    break

        if kind == "file":
            file_count += 1
            total_size += size
            basename = PurePosixPath(rel_path).name
            lib = parse_solib(basename)
            if lib is not None:
                sonames.add(lib.soname)
                lib_dirs.add(PurePosixPath(rel_path).parent.as_posix())
            if _under_bin_dir(rel_path):
                binaries.add(basename)
        elif kind == "hardlink":
            # A hardlink to a real file is still an executable if it lands in a
            # bin dir (e.g. busybox-style multi-call links).
            if _under_bin_dir(rel_path):
                binaries.add(PurePosixPath(rel_path).name)

    return BaseInventory(
        sonames=tuple(sorted(sonames)),
        binaries=tuple(sorted(binaries)),
        lib_dirs=tuple(sorted(lib_dirs)),
        has_xkb=has_xkb,
        has_fontconfig=has_fontconfig,
        file_count=file_count,
        total_size=total_size,
    )


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #


def _add_file(tar: tarfile.TarFile, name: str, data: bytes, mode: int = 0o644) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    tar.addfile(info, io.BytesIO(data))


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
        synth = Path(tmp) / "synth.tar"
        libdir = "./usr/lib/aarch64-linux-gnu"
        with tarfile.open(synth, "w") as t:
            _add_file(t, f"{libdir}/libfoo.so.1", b"FOO" * 32)
            _add_file(t, f"{libdir}/libbar.so.2", b"BAR" * 32)
            _add_file(t, "./usr/bin/app", b"ELF...", mode=0o755)
            _add_file(t, "./usr/share/X11/xkb/rules/evdev", b"! model = keycodes\n")
            # Non-library file in a lib dir must NOT become a soname.
            _add_file(t, f"{libdir}/README", b"not a lib")

        inv = inventory_base(synth)

        check("synthetic sonames == {libfoo.so.1, libbar.so.2}",
              set(inv.sonames) == {"libfoo.so.1", "libbar.so.2"})
        check("synthetic binary 'app' found", "app" in inv.binaries)
        check("synthetic README not treated as binary", "README" not in inv.binaries)
        check("synthetic has_xkb True", inv.has_xkb is True)
        check("synthetic has_fontconfig False", inv.has_fontconfig is False)
        check("synthetic lib_dir recorded",
              "usr/lib/aarch64-linux-gnu" in inv.lib_dirs)
        check("synthetic file_count == 5", inv.file_count == 5)
        check("synthetic total_size > 0", inv.total_size > 0)

        diff = inv.diff_against({"libfoo.so.1", "libnew.so.3"})
        check("diff_against already_in_base == [libfoo.so.1]",
              diff["already_in_base"] == ["libfoo.so.1"])
        check("diff_against new == [libnew.so.3]",
              diff["new"] == ["libnew.so.3"])

        # to_json round-trips to a dict with the expected keys.
        loaded = json.loads(inv.to_json())
        check("to_json carries all dataclass fields",
              set(loaded) == set(asdict(inv)))

    # ----- real base tar -----
    real = Path("app/src/main/assets/rootfs/payloads/tiny-rootfs.tar")
    if not real.is_file():
        check(f"real base tar present at {real}", False)
        print(f"\nselftest: {failures} FAILED")
        return 1

    real_inv = inventory_base(real)
    check("real base soname count > 50", len(real_inv.sonames) > 50)
    check("real base has_xkb True", real_inv.has_xkb is True)

    print()
    print(real_inv.render())
    print()
    print(
        "real base headline: "
        f"sonames={len(real_inv.sonames)} "
        f"binaries={len(real_inv.binaries)} "
        f"has_xkb={real_inv.has_xkb} "
        f"has_fontconfig={real_inv.has_fontconfig}"
    )

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="base_inventory",
        description="Inventory what the base rootfs provides (sonames/binaries/xkb/fontconfig).",
    )
    parser.add_argument("--rootfs", help="base rootfs tar or extracted directory")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of a summary")
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.rootfs:
        parser.error("--rootfs is required (or use --selftest)")

    inv = inventory_base(args.rootfs)
    print(inv.to_json() if args.json else inv.render())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
