"""§5-E stage-tar conformance validator (WS-4 / L4 rootfs).

An overlay "stage tar" is the unit the device extractor lays down on top of the
base rootfs. The §5-E convention constrains what a *conformant* overlay may
ship so that the on-device extract is safe and never silently downgrades a base
library. This module validates an overlay tar against that convention, reusing
the existing safety/structure primitives rather than re-deriving them:

  - :mod:`tools.safe_tar` — ``inspect_tar_members`` (rejects absolute paths,
    ``..`` traversal and device nodes; normalizes member kinds/paths) and
    ``UnsafeTarArchive`` for the hard failures.
  - :mod:`tools.overlay_guard` — ``scan_overlay_violations`` for the base
    downgrade ruleset and ``parse_solib`` for the flat-vs-versioned SONAME shape.

The §5-E rules validated here
-----------------------------
1. Root layout: every member path is relative (a single leading ``./`` is fine),
   never absolute, never contains ``..``, no char/block/FIFO device nodes. This
   is exactly what ``inspect_tar_members`` enforces — an ``UnsafeTarArchive`` is
   treated as a hard ERROR.
2. Symlink targets: a conformant overlay's symlinks resolve in-tree (relative,
   not starting with ``/``). An absolute or escaping target is a WARNING (the
   device extractor skips them, but a conformant overlay shouldn't ship them).
3. Flat SONAME preference: the base ships shared libs as flat real files
   ``libNAME.so.MAJOR``. An overlay SHOULD do the same. Shipping the Debian
   versioned layout instead (``libNAME.so.MAJOR -> libNAME.so.MAJOR.MINOR.PATCH``
   plus the versioned real file) is a WARNING — "non-flat SONAME; will be
   guarded against base downgrade".
4. No base downgrade: when a base is provided, ``scan_overlay_violations`` is
   composed; any "block" is an ERROR, any "warn" is a WARNING.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from tools.overlay_guard import BLOCK, WARN, scan_overlay_violations
from tools.safe_tar import TarMember, UnsafeTarArchive, inspect_tar_members
from tools.overlay_guard import parse_solib


@dataclass(frozen=True)
class StageTarReport:
    conformant: bool
    errors: list[str]
    warnings: list[str]


def _check_symlink_targets(members: list[TarMember], warnings: list[str]) -> None:
    """Rule 2 — symlink targets should resolve in-tree."""
    for m in members:
        if m.kind != "symlink":
            continue
        target = m.linkname
        if not target:
            continue
        if target.startswith("/"):
            warnings.append(
                f"symlink target escapes tree (absolute): {m.name} -> {target} "
                f"— device extractor will skip it; conformant overlays ship "
                f"in-tree relative targets"
            )
            continue
        # An in-tree relative target must not climb above the symlink's own
        # directory. inspect_tar_members tolerates absolute targets but a ".."
        # escape past the root is still non-conformant.
        parent = PurePosixPath(m.name).parent
        resolved_parts: list[str] = []
        escaped = False
        for part in (parent / target).parts:
            if part == "..":
                if not resolved_parts:
                    escaped = True
                    break
                resolved_parts.pop()
            else:
                resolved_parts.append(part)
        if escaped:
            warnings.append(
                f"symlink target escapes tree (..): {m.name} -> {target} "
                f"— device extractor will skip it; conformant overlays ship "
                f"in-tree relative targets"
            )


def _check_flat_soname(members: list[TarMember], warnings: list[str]) -> None:
    """Rule 3 — prefer the flat-SONAME layout over the Debian versioned one.

    A non-flat layout shows up as either side of the pair:
      * a symlink ``libNAME.so.MAJOR`` whose target parses to a longer version
        (``len > 1``), and/or
      * a real (or hardlink) file whose own parsed version has ``len > 1``.
    Each distinct soname is reported at most once.
    """
    flagged: set[str] = set()

    def flag(soname: str, detail: str) -> None:
        if soname in flagged:
            return
        flagged.add(soname)
        warnings.append(
            f"non-flat SONAME {soname}: {detail}; will be guarded against base "
            f"downgrade — conformant overlays ship flat real {soname}"
        )

    for m in members:
        lib = parse_solib(PurePosixPath(m.name).name)
        if lib is None:
            continue
        if m.kind == "symlink":
            target = parse_solib(PurePosixPath(m.linkname).name)
            if target is not None and len(target.version) > 1:
                flag(lib.soname, f"{m.name} -> {m.linkname} (Debian versioned layout)")
        elif m.kind in ("file", "hardlink"):
            if len(lib.version) > 1:
                flag(lib.soname, f"versioned real file {m.name}")


def _check_base_downgrade(
    overlay: str | Path, base: str | Path, errors: list[str], warnings: list[str]
) -> None:
    """Rule 4 — compose the overlay guard; map block->ERROR, warn->WARNING."""
    for v in scan_overlay_violations(base, overlay):
        if v.severity == BLOCK:
            errors.append(f"base downgrade ({v.rule}): {v.render()}")
        elif v.severity == WARN:
            warnings.append(f"base overwrite ({v.rule}): {v.render()}")


def validate_stage_tar(
    overlay: str | Path, base: str | Path | None = None
) -> StageTarReport:
    """Validate ``overlay`` against the §5-E stage-tar convention.

    Returns a :class:`StageTarReport`. ``conformant`` is True iff there are no
    errors. When ``base`` (a dir or tar) is given, base-downgrade rules are
    composed via :func:`tools.overlay_guard.scan_overlay_violations`.
    """
    errors: list[str] = []
    warnings: list[str] = []

    # Rule 1 — root layout safety. inspect_tar_members raises on absolute/`..`/
    # device members. If it does, that is a hard ERROR and we cannot reason
    # further about structure.
    try:
        members = inspect_tar_members(overlay)
    except UnsafeTarArchive as exc:
        errors.append(f"unsafe root layout: {exc}")
        return StageTarReport(conformant=False, errors=errors, warnings=warnings)
    except (OSError, ValueError) as exc:
        errors.append(f"cannot read overlay tar: {exc}")
        return StageTarReport(conformant=False, errors=errors, warnings=warnings)

    # Rule 2 — symlink targets.
    _check_symlink_targets(members, warnings)

    # Rule 3 — flat SONAME preference.
    _check_flat_soname(members, warnings)

    # Rule 4 — no base downgrade (only when a base is supplied).
    if base is not None:
        _check_base_downgrade(overlay, base, errors, warnings)

    return StageTarReport(
        conformant=not errors, errors=errors, warnings=warnings
    )


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

def _run_check(overlay: str, base: str | None, strict: bool) -> int:
    report = validate_stage_tar(overlay, base)
    for e in report.errors:
        print(f"[ERROR]   {e}")
    for w in report.warnings:
        print(f"[WARNING] {w}")
    print(
        f"\n{overlay}: {len(report.errors)} error(s), "
        f"{len(report.warnings)} warning(s) — "
        f"{'CONFORMANT' if report.conformant else 'NON-CONFORMANT'}"
    )
    if not report.conformant:
        return 1
    if strict and report.warnings:
        return 1
    return 0


def _selftest() -> int:
    import io
    import tarfile
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    def add_file(tar: tarfile.TarFile, name: str, data: bytes, mode: int = 0o644) -> None:
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.mode = mode
        tar.addfile(info, io.BytesIO(data))

    def add_symlink(tar: tarfile.TarFile, name: str, target: str) -> None:
        info = tarfile.TarInfo(name)
        info.type = tarfile.SYMTYPE
        info.linkname = target
        tar.addfile(info)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        libdir = "./usr/lib/aarch64-linux-gnu"

        # (a) clean overlay: only new files under usr/bin.
        clean = tmp_path / "clean.tar"
        with tarfile.open(clean, "w") as t:
            add_file(t, "./usr/bin/foo", b"ELF-foo")
            add_file(t, "./usr/bin/bar", b"ELF-bar")

        # (b) absolute-path overlay -> ERROR (UnsafeTarArchive).
        absbad = tmp_path / "absbad.tar"
        with tarfile.open(absbad, "w") as t:
            add_file(t, "/etc/passwd", b"root:x:0:0")

        # (c) non-flat SONAME overlay -> WARNING, conformant w/o base.
        nonflat = tmp_path / "nonflat.tar"
        with tarfile.open(nonflat, "w") as t:
            add_symlink(t, f"{libdir}/libfoo.so.1", "libfoo.so.1.2.3")
            add_file(t, f"{libdir}/libfoo.so.1.2.3", b"FOO-1.2.3" * 16)

        # (d) base owning libfoo.so.1 as a flat real file -> (c) is base downgrade.
        base = tmp_path / "base.tar"
        with tarfile.open(base, "w") as t:
            add_file(t, f"{libdir}/libfoo.so.1", b"FOO-flat-base" * 64)

        # (a) clean overlay is conformant, no errors, no warnings.
        rep_a = validate_stage_tar(clean)
        check("(a) clean overlay is conformant", rep_a.conformant)
        check("(a) clean overlay has no errors", rep_a.errors == [])
        check("(a) clean overlay has no warnings", rep_a.warnings == [])

        # (b) absolute path is an ERROR / non-conformant.
        rep_b = validate_stage_tar(absbad)
        check("(b) absolute-path overlay is non-conformant", not rep_b.conformant)
        check("(b) absolute-path overlay reports an error", len(rep_b.errors) >= 1)

        # (c) non-flat SONAME is a WARNING, still conformant without a base.
        rep_c = validate_stage_tar(nonflat)
        check("(c) non-flat overlay is conformant (no base)", rep_c.conformant)
        check("(c) non-flat overlay raises a warning", len(rep_c.warnings) >= 1)
        check(
            "(c) warning mentions non-flat SONAME",
            any("non-flat SONAME" in w for w in rep_c.warnings),
        )

        # (d) same overlay against a base that owns the flat soname -> ERROR.
        rep_d = validate_stage_tar(nonflat, base=base)
        check("(d) non-flat overlay vs flat base is non-conformant", not rep_d.conformant)
        check("(d) base downgrade reported as error", len(rep_d.errors) >= 1)
        check(
            "(d) error attributed to base downgrade",
            any("base downgrade" in e for e in rep_d.errors),
        )

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="stage_tar_spec",
        description="Validate an overlay stage tar against the §5-E convention.",
    )
    parser.add_argument("--overlay", help="overlay stage tar to validate")
    parser.add_argument("--base", help="base rootfs directory or tar (enables downgrade check)")
    parser.add_argument(
        "--strict", action="store_true",
        help="treat warnings as failures too (exit 1 on any warning)",
    )
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.overlay:
        parser.error("--overlay is required (or use --selftest)")
    return _run_check(args.overlay, args.base, args.strict)


if __name__ == "__main__":
    raise SystemExit(main())
