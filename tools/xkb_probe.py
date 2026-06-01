"""XKB keymap-data completeness probe for the ALR rootfs (WS-4 / L4).

Why this exists
---------------
A sibling session (compositor/input) hit a `wl_keyboard.keymap` NO_KEYMAP crash
and suspected the rootfs was "missing xkb-data". It is NOT: the base rootfs
already ships the full `/usr/share/X11/xkb` tree (keycodes/types/compat/symbols/
rules) plus `libxkbcommon.so.0`. The real fix for NO_KEYMAP is to point the
keymap compiler at that directory — libxkbcommon's default include root is the
compiled-in `/usr/share/X11/xkb`, which on Android resolves to the EXTRACTED
rootfs path, not a system path. Set `XKB_CONFIG_ROOT` (or an xkb_context include
path) to `<rootfs>/usr/share/X11/xkb` before compiling the keymap.

This probe turns that finding into a reusable check: given a rootfs (directory or
tar) it verifies the data needed to compile the default RMLVO keymap
(rules=evdev, model=pc105, layout=us) is present and non-empty, and prints the
canonical XKB root path to use.
"""

from __future__ import annotations

import argparse
import tarfile
from dataclasses import dataclass
from pathlib import Path

XKB_DIR = "usr/share/X11/xkb"
XKB_LIB = "usr/lib/aarch64-linux-gnu/libxkbcommon.so.0"

# Files libxkbcommon needs to compile the default evdev/pc105/us keymap.
# (geometry/* is optional — only needed for graphical keyboard layouts.)
REQUIRED_XKB_FILES = (
    "rules/evdev",
    "rules/evdev.xml",
    "keycodes/evdev",
    "types/complete",
    "compat/complete",
    "symbols/pc",
    "symbols/us",
    "symbols/inet",
)


@dataclass(frozen=True)
class XkbProbeResult:
    rootfs: str
    xkb_root: str           # the path to pass as XKB_CONFIG_ROOT (rootfs-relative shown)
    present: tuple[str, ...]
    missing: tuple[str, ...]
    empty: tuple[str, ...]
    has_libxkbcommon: bool

    @property
    def complete(self) -> bool:
        return not self.missing and not self.empty

    def render(self) -> str:
        lines = [f"rootfs: {self.rootfs}", f"xkb root (XKB_CONFIG_ROOT): {self.xkb_root}"]
        lines.append(f"libxkbcommon.so.0: {'present' if self.has_libxkbcommon else 'MISSING'}")
        if self.complete:
            lines.append(f"OK: {len(self.present)} required xkb files present — default us/pc105 keymap is compilable")
        else:
            if self.missing:
                lines.append("MISSING required xkb files:")
                lines += [f"  - {XKB_DIR}/{m}" for m in self.missing]
            if self.empty:
                lines.append("EMPTY required xkb files:")
                lines += [f"  - {XKB_DIR}/{m}" for m in self.empty]
        return "\n".join(lines)


def _normalize(name: str) -> str:
    while name.startswith("./"):
        name = name[2:]
    return name.rstrip("/")


def _sizes_from_tar(tar_path: Path) -> dict[str, int]:
    sizes: dict[str, int] = {}
    with tarfile.open(tar_path, "r:*") as tar:
        for m in tar.getmembers():
            if m.isfile():
                sizes[_normalize(m.name)] = m.size
            elif m.isdir():
                sizes.setdefault(_normalize(m.name), -1)  # dir marker
    return sizes


def _sizes_from_dir(root: Path) -> dict[str, int]:
    sizes: dict[str, int] = {}
    root = root.resolve()
    for path in root.rglob("*"):
        rel = path.relative_to(root).as_posix()
        if path.is_symlink():
            # follow only in-tree; record target size if it resolves to a file
            try:
                sizes[rel] = path.stat().st_size if path.is_file() else -1
            except OSError:
                sizes[rel] = 0
        elif path.is_file():
            sizes[rel] = path.stat().st_size
        elif path.is_dir():
            sizes[rel] = -1
    return sizes


def probe_xkb(rootfs: str | Path) -> XkbProbeResult:
    """Probe a rootfs (directory or tar) for default-keymap xkb completeness."""
    rootfs_path = Path(rootfs)
    sizes = _sizes_from_dir(rootfs_path) if rootfs_path.is_dir() else _sizes_from_tar(rootfs_path)

    present: list[str] = []
    missing: list[str] = []
    empty: list[str] = []
    for rel in REQUIRED_XKB_FILES:
        key = f"{XKB_DIR}/{rel}"
        if key not in sizes:
            missing.append(rel)
        elif sizes[key] == 0:
            empty.append(rel)
        else:
            present.append(rel)

    return XkbProbeResult(
        rootfs=str(rootfs_path),
        xkb_root=f"{rootfs_path}/{XKB_DIR}" if rootfs_path.is_dir() else f"<rootfs>/{XKB_DIR}",
        present=tuple(present),
        missing=tuple(missing),
        empty=tuple(empty),
        has_libxkbcommon=XKB_LIB in sizes,
    )


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

def _make_tar(path: Path, files: dict[str, bytes], dirs: tuple[str, ...] = ()) -> Path:
    import io

    with tarfile.open(path, "w") as tar:
        for d in dirs:
            info = tarfile.TarInfo(d.rstrip("/") + "/")
            info.type = tarfile.DIRTYPE
            tar.addfile(info)
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return path


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

        complete = {f"./{XKB_DIR}/{rel}": b"xkb-data\n" for rel in REQUIRED_XKB_FILES}
        complete[f"./{XKB_LIB}"] = b"ELF"
        good = _make_tar(tmp_path / "complete.tar", complete)
        r_good = probe_xkb(good)
        check("complete tree -> complete=True", r_good.complete)
        check("complete tree -> libxkbcommon detected", r_good.has_libxkbcommon)
        check("complete tree -> no missing", r_good.missing == ())

        partial = dict(complete)
        del partial[f"./{XKB_DIR}/symbols/us"]
        partial[f"./{XKB_DIR}/rules/evdev"] = b""  # present but empty
        bad = _make_tar(tmp_path / "partial.tar", partial)
        r_bad = probe_xkb(bad)
        check("missing symbols/us flagged", "symbols/us" in r_bad.missing)
        check("empty rules/evdev flagged", "rules/evdev" in r_bad.empty)
        check("partial tree -> complete=False", not r_bad.complete)

        # directory mode
        ddir = tmp_path / "rootfs"
        for rel in REQUIRED_XKB_FILES:
            p = ddir / XKB_DIR / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("data\n")
        r_dir = probe_xkb(ddir)
        check("directory mode -> complete=True", r_dir.complete)
        check("directory mode reports absolute xkb_root", r_dir.xkb_root.endswith(XKB_DIR))

    # Opportunistic: run against the real base rootfs tar if present.
    base = Path(__file__).resolve().parents[1] / "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    if base.is_file():
        r_base = probe_xkb(base)
        check("REAL base tiny-rootfs.tar xkb tree complete", r_base.complete)
        check("REAL base has libxkbcommon.so.0", r_base.has_libxkbcommon)
        print("    " + r_base.render().replace("\n", "\n    "))
    else:
        print("  [skip] base tiny-rootfs.tar not found (non-fatal)")

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="xkb_probe",
        description="Verify a rootfs has the xkb data to compile the default keymap.",
    )
    parser.add_argument("--rootfs", help="rootfs directory or tar to probe")
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.rootfs:
        parser.error("--rootfs is required (or use --selftest)")
    result = probe_xkb(args.rootfs)
    print(result.render())
    return 0 if result.complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
