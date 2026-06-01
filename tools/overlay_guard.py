"""Overlay tar version-consistency guard (WS-4 / L4 rootfs).

Background — the harfbuzz regression class
------------------------------------------
The base rootfs follows the §5-E "flat SONAME" convention: a shared library is
shipped as a *real regular file* named exactly ``libNAME.so.MAJOR`` (e.g. the
base ``libharfbuzz.so.0`` is the real harfbuzz 8.3.0 binary, not a symlink).

A non-conformant overlay (e.g. a raw .deb dump like ``chromium-stage.tar``)
instead ships the Debian layout::

    libharfbuzz.so.0          -> libharfbuzz.so.0.60000.0   (symlink)
    libharfbuzz.so.0.60000.0                                (real file, hb 6.0.0)

Extracting that overlay on top of the base *deletes* the real flat
``libharfbuzz.so.0`` (8.3.0) and replaces it with a symlink to the older
6.0.0 binary — a silent downgrade that breaks every consumer compiled against
8.3.0 (pango/GTK). This guard refuses that class of change.

Why structural, not arithmetic
------------------------------
Filename version arithmetic is unreliable here: the base flattened its real
version into the SONAME name, so the base file is just ``libharfbuzz.so.0``
(filename version ``(0,)``) while the overlay advertises ``(0, 60000, 0)`` —
numerically *larger*, yet actually older. There is no dpkg DB in the base to
consult (``var/lib/dpkg/status`` is empty). So the rule is structural:

    A SONAME for which the base provides a real library file is FROZEN.
    An overlay may not repoint it (symlink) nor ship a versioned variant
    (``libNAME.so.MAJOR.MINOR...``) for it. The base library wins.

Numeric comparison is only applied when BOTH sides are versioned (len > 1),
where the filename order is meaningful (e.g. base ``...0.80300.0`` vs overlay
``...0.60000.0`` -> downgrade).
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import tarfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from tools.safe_tar import TarMember, inspect_tar_members

# verdict severities
BLOCK = "block"   # certain or structurally-forced downgrade -> reject
WARN = "warn"     # ambiguous in-place overwrite of a base library -> review
OK = "ok"


@dataclass(frozen=True)
class SoLib:
    stem: str           # "libharfbuzz.so"
    soname: str         # "libharfbuzz.so.0"
    version: tuple      # (0,) for a flat soname, (0, 60000, 0) for a versioned file

    @property
    def is_flat(self) -> bool:
        return len(self.version) == 1


def parse_solib(basename: str) -> SoLib | None:
    """Parse a shared-library basename, or return None if it is not a versioned .so.

    >>> parse_solib("libharfbuzz.so.0").version
    (0,)
    >>> parse_solib("libharfbuzz.so.0.60000.0").soname
    'libharfbuzz.so.0'
    >>> parse_solib("libharfbuzz.so") is None
    True
    """
    name = PurePosixPath(basename).name
    marker = ".so."
    idx = name.find(marker)
    if idx < 0:
        return None
    stem = name[: idx + len(".so")]
    ver_text = name[idx + len(marker) :]
    if not ver_text:
        return None
    parts = ver_text.split(".")
    if not all(p.isdigit() for p in parts):
        return None
    version = tuple(int(p) for p in parts)
    return SoLib(stem=stem, soname=f"{stem}.{version[0]}", version=version)


def _soname_key(rel_path: str, lib: SoLib) -> str:
    """Directory-scoped soname key so multiarch dirs never collide."""
    parent = PurePosixPath(rel_path).parent.as_posix()
    return f"{parent}|{lib.soname}"


def _ver_le(a: tuple, b: tuple) -> bool:
    """a <= b with zero-padding so (0,5) and (0,5,0) compare equal."""
    width = max(len(a), len(b))
    return a + (0,) * (width - len(a)) <= b + (0,) * (width - len(b))


@dataclass(frozen=True)
class BaseSoname:
    key: str
    soname: str
    rel_path: str       # the real library file path in the base
    version: tuple      # filename-derived version of the base real file
    true_version: tuple | None = None  # real upstream version, when a lib-versions.json sidecar declares it


def _iter_dir_members(base_dir: Path):
    """Yield (rel_path, kind, linkname) for a base rootfs directory."""
    base_dir = base_dir.resolve()
    for root, _dirs, files in os.walk(base_dir):
        for fname in files:
            full = Path(root) / fname
            rel = full.relative_to(base_dir).as_posix()
            if full.is_symlink():
                yield rel, "symlink", os.readlink(full)
            else:
                yield rel, "file", ""


def _iter_tar_members(tar_path: Path):
    for m in inspect_tar_members(tar_path):
        yield m.name, m.kind, m.linkname


def _load_versions_sidecar(target: str | Path) -> dict[str, tuple]:
    """Load a ``<tar>.lib-versions.json`` / ``<dir>/lib-versions.json`` sidecar.

    The sidecar is emitted by ``tools.build_stage_tar`` and maps ``"<dir>/<soname>"``
    to a ``"MAJOR.MINOR.PATCH"`` string — the REAL upstream version a flattened
    flat-SONAME file came from. We re-key it to the internal ``"<dir>|<soname>"``
    form so flat-over-flat overwrites can be compared numerically (otherwise the
    flattened filename has lost the minor/patch). Returns {} when absent/unreadable.
    """
    path = Path(target)
    sidecar = path / "lib-versions.json" if path.is_dir() else Path(f"{path}.lib-versions.json")
    if not sidecar.is_file():
        return {}
    try:
        raw = json.loads(sidecar.read_text())
    except (ValueError, OSError):
        return {}
    out: dict[str, tuple] = {}
    for sidecar_key, ver in raw.items():
        directory, _, soname = sidecar_key.rpartition("/")
        try:
            out[f"{directory}|{soname}"] = tuple(int(x) for x in str(ver).split("."))
        except ValueError:
            continue
    return out


def build_base_soname_index(base: str | Path) -> dict[str, BaseSoname]:
    """Map each SONAME the base provides as a *real library file* to its version.

    Accepts either a base rootfs directory or a base tar. Only real files (not
    symlinks) define a frozen soname — the file the soname ultimately resolves
    to. When several real files share a soname (versioned base layout) the
    highest version wins.
    """
    base_path = Path(base)
    if base_path.is_dir():
        iterator = _iter_dir_members(base_path)
    else:
        iterator = _iter_tar_members(base_path)

    index: dict[str, BaseSoname] = {}
    for rel_path, kind, _linkname in iterator:
        if kind != "file":
            continue
        lib = parse_solib(PurePosixPath(rel_path).name)
        if lib is None:
            continue
        key = _soname_key(rel_path, lib)
        existing = index.get(key)
        if existing is None or not _ver_le(lib.version, existing.version):
            index[key] = BaseSoname(
                key=key,
                soname=lib.soname,
                rel_path=rel_path,
                version=lib.version,
            )

    # Augment with real upstream versions from a lib-versions.json sidecar, if the
    # base carries one. This only ADDS a true_version for flat-over-flat numeric
    # comparison; it never changes the filename-derived `version` the structural
    # frozen-flat rules rely on.
    for sidecar_key, true_ver in _load_versions_sidecar(base).items():
        entry = index.get(sidecar_key)
        if entry is not None:
            index[sidecar_key] = BaseSoname(
                key=entry.key,
                soname=entry.soname,
                rel_path=entry.rel_path,
                version=entry.version,
                true_version=true_ver,
            )
    return index


@dataclass(frozen=True)
class Violation:
    severity: str       # BLOCK | WARN
    rule: str
    overlay_path: str
    soname: str
    detail: str

    def render(self) -> str:
        return f"[{self.severity.upper():5}] {self.rule}: {self.overlay_path} ({self.soname}) — {self.detail}"


def scan_overlay_violations(
    base: str | Path, overlay_tar: str | Path
) -> list[Violation]:
    """Return downgrade/structure violations an overlay would inflict on the base."""
    index = build_base_soname_index(base)
    overlay_versions = _load_versions_sidecar(overlay_tar)
    violations: list[Violation] = []

    for member in inspect_tar_members(overlay_tar):
        lib = parse_solib(PurePosixPath(member.name).name)
        if lib is None:
            continue
        key = _soname_key(member.name, lib)
        base_entry = index.get(key)
        if base_entry is None:
            continue  # base does not own this soname — overlay may add it freely

        base_ver = base_entry.version

        if member.kind == "symlink":
            target = parse_solib(PurePosixPath(member.linkname).name)
            target_ver = target.version if target else None
            if base_entry.version and len(base_ver) == 1:
                violations.append(Violation(
                    BLOCK, "symlink-over-frozen-soname", member.name, lib.soname,
                    f"overlay repoints a base flat-SONAME real library to symlink "
                    f"-> {member.linkname}; base file {base_entry.rel_path} would be lost",
                ))
            elif target_ver is not None and not _ver_le(base_ver, target_ver):
                violations.append(Violation(
                    BLOCK, "symlink-downgrade", member.name, lib.soname,
                    f"symlink target {member.linkname} ({_fmt(target_ver)}) "
                    f"< base {_fmt(base_ver)}",
                ))
            continue

        # real file (or hardlink) carrying a version for a base-owned soname
        if not lib.is_flat:
            # versioned variant (libX.so.MAJOR.MINOR...) for a base soname
            if len(base_ver) == 1:
                violations.append(Violation(
                    BLOCK, "versioned-over-frozen-soname", member.name, lib.soname,
                    f"overlay ships a versioned variant {_fmt(lib.version)} for a soname "
                    f"the base provides as a flat real file ({base_entry.rel_path}); "
                    f"would shadow/downgrade the base library",
                ))
            elif not _ver_le(base_ver, lib.version):
                violations.append(Violation(
                    BLOCK, "versioned-downgrade", member.name, lib.soname,
                    f"overlay {_fmt(lib.version)} < base {_fmt(base_ver)}",
                ))
        else:
            # flat-over-flat at the same soname path: in-place overwrite, same
            # major. The flattened filename has lost the minor/patch, so the
            # decision depends on lib-versions.json sidecars:
            base_true = base_entry.true_version
            overlay_true = overlay_versions.get(key)
            if base_true is not None and overlay_true is not None:
                # Both sides declare a real version — a reliable numeric decision.
                if not _ver_le(base_true, overlay_true):
                    violations.append(Violation(
                        BLOCK, "flat-downgrade", member.name, lib.soname,
                        f"overlay {_fmt(overlay_true)} < base {_fmt(base_true)} "
                        f"(from lib-versions.json sidecars)",
                    ))
                # else: upgrade or equal — allowed, no violation.
            else:
                # No sidecar(s): filename can't prove the minor order, so we can
                # certify neither a downgrade nor that it is safe. The device guard
                # APPLIES it (a flat overlay is the conformant §5-E shape and may be
                # a legitimate restore/upgrade — e.g. harfbuzz-fix-stage.tar), but it
                # is flagged for review. Non-blocking by default; --strict blocks.
                violations.append(Violation(
                    WARN, "flat-overwrite-ambiguous", member.name, lib.soname,
                    f"overlay overwrites base real library {base_entry.rel_path} in "
                    f"place; version unverifiable from filename ({_fmt(lib.version)} "
                    f"vs base {_fmt(base_ver)}) — applied, review recommended "
                    f"(emit lib-versions.json via build_stage_tar to make this exact)",
                ))

    return violations


def _fmt(version: tuple) -> str:
    return ".".join(str(v) for v in version)


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

def _run_check(base: str, overlay: str, strict: bool) -> int:
    violations = scan_overlay_violations(base, overlay)
    blocks = [v for v in violations if v.severity == BLOCK]
    warns = [v for v in violations if v.severity == WARN]
    for v in violations:
        print(v.render())
    if not violations:
        print(f"OK: overlay {overlay} introduces no base-lib downgrade")
    else:
        print(f"\n{len(blocks)} blocking, {len(warns)} warning violation(s)")
    return 1 if blocks or (strict and warns) else 0


def _add_file(tar: tarfile.TarFile, name: str, data: bytes, mode: int = 0o644) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    tar.addfile(info, io.BytesIO(data))


def _add_symlink(tar: tarfile.TarFile, name: str, target: str) -> None:
    info = tarfile.TarInfo(name)
    info.type = tarfile.SYMTYPE
    info.linkname = target
    tar.addfile(info)


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
        libdir = "./usr/lib/aarch64-linux-gnu"

        # Base: flat SONAME real files (the §5-E convention).
        base = tmp_path / "base.tar"
        with tarfile.open(base, "w") as t:
            _add_file(t, f"{libdir}/libharfbuzz.so.0", b"HB-8.3.0" * 64)
            _add_file(t, f"{libdir}/libpango-1.0.so.0", b"PANGO" * 16)

        # Overlay A: the harfbuzz incident — symlink + versioned 6.0.0 real file.
        bad = tmp_path / "bad-overlay.tar"
        with tarfile.open(bad, "w") as t:
            _add_symlink(t, f"{libdir}/libharfbuzz.so.0", "libharfbuzz.so.0.60000.0")
            _add_file(t, f"{libdir}/libharfbuzz.so.0.60000.0", b"HB-6.0.0" * 32)
            _add_file(t, "./usr/lib/chromium/chrome", b"ELF...")  # unrelated payload

        # Overlay B: conformant — only adds a new soname, no base collision.
        good = tmp_path / "good-overlay.tar"
        with tarfile.open(good, "w") as t:
            _add_file(t, f"{libdir}/libwebp.so.7", b"WEBP" * 16)
            _add_file(t, "./usr/lib/chromium/chrome", b"ELF...")

        # Overlay C: versioned-vs-versioned genuine downgrade.
        vbase = tmp_path / "vbase.tar"
        with tarfile.open(vbase, "w") as t:
            _add_symlink(t, f"{libdir}/libfoo.so.1", "libfoo.so.1.80300.0")
            _add_file(t, f"{libdir}/libfoo.so.1.80300.0", b"FOO-new" * 16)
        vdown = tmp_path / "vdown.tar"
        with tarfile.open(vdown, "w") as t:
            _add_symlink(t, f"{libdir}/libfoo.so.1", "libfoo.so.1.60000.0")
            _add_file(t, f"{libdir}/libfoo.so.1.60000.0", b"FOO-old" * 16)

        bad_v = scan_overlay_violations(base, bad)
        bad_blocks = [v for v in bad_v if v.severity == BLOCK]
        check("harfbuzz overlay flagged as BLOCK", len(bad_blocks) >= 1)
        check(
            "harfbuzz symlink + versioned file both blocked",
            {v.rule for v in bad_blocks}
            >= {"symlink-over-frozen-soname", "versioned-over-frozen-soname"},
        )

        good_v = scan_overlay_violations(base, good)
        check("conformant overlay has no violations", good_v == [])

        vdown_v = scan_overlay_violations(vbase, vdown)
        check(
            "versioned-vs-versioned downgrade blocked",
            any(v.severity == BLOCK for v in vdown_v),
        )
        vup_v = scan_overlay_violations(vdown, vbase)
        check("versioned-vs-versioned upgrade allowed", vup_v == [])

        # Flat-over-flat with lib-versions.json sidecars (build_stage_tar output).
        # Base + overlay both ship a flat real libbar.so.2 — filename can't tell
        # order, but the sidecars declare the real versions.
        fbase = tmp_path / "flatbase.tar"
        with tarfile.open(fbase, "w") as t:
            _add_file(t, f"{libdir}/libbar.so.2", b"BAR-2.0.14" * 16)
        (tmp_path / "flatbase.tar.lib-versions.json").write_text(
            '{"usr/lib/aarch64-linux-gnu/libbar.so.2": "2.0.14"}'
        )
        fdown = tmp_path / "flatdown.tar"
        with tarfile.open(fdown, "w") as t:
            _add_file(t, f"{libdir}/libbar.so.2", b"BAR-2.0.8" * 16)
        (tmp_path / "flatdown.tar.lib-versions.json").write_text(
            '{"usr/lib/aarch64-linux-gnu/libbar.so.2": "2.0.8"}'
        )
        fup = tmp_path / "flatup.tar"
        with tarfile.open(fup, "w") as t:
            _add_file(t, f"{libdir}/libbar.so.2", b"BAR-2.0.20" * 16)
        (tmp_path / "flatup.tar.lib-versions.json").write_text(
            '{"usr/lib/aarch64-linux-gnu/libbar.so.2": "2.0.20"}'
        )
        fnos = tmp_path / "flatnosidecar.tar"
        with tarfile.open(fnos, "w") as t:
            _add_file(t, f"{libdir}/libbar.so.2", b"BAR-x" * 16)

        down_v = scan_overlay_violations(fbase, fdown)
        check(
            "flat-over-flat downgrade blocked via sidecars",
            any(v.severity == BLOCK and v.rule == "flat-downgrade" for v in down_v),
        )
        up_v = scan_overlay_violations(fbase, fup)
        check("flat-over-flat upgrade allowed via sidecars", up_v == [])
        nos_v = scan_overlay_violations(fbase, fnos)
        check(
            "flat-over-flat WARNs (not blocks) when overlay has no sidecar",
            nos_v and all(v.severity == WARN for v in nos_v),
        )

        # parse_solib unit checks
        check("parse_solib flat", parse_solib("libharfbuzz.so.0").version == (0,))
        check(
            "parse_solib versioned",
            parse_solib("libharfbuzz.so.0.60000.0").version == (0, 60000, 0),
        )
        check("parse_solib dev-symlink None", parse_solib("libharfbuzz.so") is None)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="overlay_guard",
        description="Reject overlay tars that would downgrade base rootfs libraries.",
    )
    parser.add_argument("--base", help="base rootfs directory or tar")
    parser.add_argument("--overlay", help="overlay tar to check")
    parser.add_argument(
        "--strict", action="store_true",
        help="treat ambiguous in-place overwrites (WARN) as failures too",
    )
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.base or not args.overlay:
        parser.error("--base and --overlay are required (or use --selftest)")
    return _run_check(args.base, args.overlay, args.strict)


if __name__ == "__main__":
    raise SystemExit(main())
