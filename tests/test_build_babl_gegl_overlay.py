"""Host tests for tools/build_babl_gegl_overlay.py (WS-4 §10a babl/gegl modules).

All OFFLINE — exercise the overlay assembly against a synthetic extracted package
root (no network / no real .deb). The network build path is verified by hand on
the host (see the WS-4 handoff) and is the integration/device gate.

The overlay's contract (memory: ALR .so x-bit; STAGE_TAR_SPEC §10.1):
  * every babl/gegl op module ships as a real 0o755 file (ALR file-backed
    PROT_EXEC dlopen rejects a non-x .so → GIMP filters can't load it);
  * the core SONAME libraries (libbabl-0.1.so.0*, libgegl-0.4.so.0*) are NOT
    shipped — the base owns them as frozen flat SONAMEs (downgrade guard);
  * the result is §5-E conformant and overlay_guard-clean (op .so are versionless
    modules at base paths, not frozen sonames → 0 BLOCK).
"""

import io
import tarfile
from pathlib import Path

import pytest

from tools.build_babl_gegl_overlay import (
    OP_DIRS,
    _is_op_module,
    build_babl_gegl_overlay,
)
from tools.overlay_guard import BLOCK, WARN, scan_overlay_violations
from tools.stage_tar_spec import validate_stage_tar


LIBDIR = "usr/lib/aarch64-linux-gnu"


def _make_source_root(root: Path) -> None:
    """A synthetic extracted libbabl/libgegl root: 0644 op modules + core libs."""
    babl_d = root / LIBDIR / "babl-0.1"
    gegl_d = root / LIBDIR / "gegl-0.4"
    libdir = root / LIBDIR
    babl_d.mkdir(parents=True)
    gegl_d.mkdir(parents=True)
    for n in ("CIE.so", "sse2-int8.so", "oklab.so", "cairo.so"):
        f = babl_d / n
        f.write_bytes(b"\x7fELF babl " + n.encode())
        f.chmod(0o644)
    for n in ("exr-load.so", "gegl-common.so", "transformops.so"):
        f = gegl_d / n
        f.write_bytes(b"\x7fELF gegl " + n.encode())
        f.chmod(0o644)
    # core SONAME libs (frozen by base) — must be excluded from the overlay
    (libdir / "libbabl-0.1.so.0.207.1").write_bytes(b"core babl")
    (libdir / "libgegl-0.4.so.0.447.1").write_bytes(b"core gegl")
    (libdir / "libgegl-npd-0.4.so").write_bytes(b"npd helper")
    # noise that must not be shipped
    docs = root / "usr/share/doc/libgegl-common"
    docs.mkdir(parents=True)
    (docs / "copyright").write_bytes(b"GPL")


def _build(tmp_path: Path):
    root = tmp_path / "root"
    _make_source_root(root)
    out = tmp_path / "babl-gegl-stage.tar"
    res = build_babl_gegl_overlay(out, source_root=root)
    with tarfile.open(out) as t:
        members = {m.name: m for m in t.getmembers()}
    return res, out, members


def test_is_op_module_classifies_only_op_dir_so():
    assert _is_op_module(f"./{LIBDIR}/babl-0.1/CIE.so")
    assert _is_op_module(f"{LIBDIR}/gegl-0.4/exr-load.so")
    # core libs / helpers are not op-dir modules
    assert not _is_op_module(f"{LIBDIR}/libgegl-0.4.so.0")
    assert not _is_op_module(f"{LIBDIR}/libgegl-npd-0.4.so")
    # non-.so under an op dir, or .so elsewhere
    assert not _is_op_module(f"{LIBDIR}/babl-0.1/README")
    assert not _is_op_module(f"{LIBDIR}/foo.so")


def test_op_dirs_are_the_canonical_babl_gegl_module_paths():
    assert OP_DIRS == (
        f"{LIBDIR}/babl-0.1",
        f"{LIBDIR}/gegl-0.4",
    )


def test_builder_collects_all_op_modules(tmp_path: Path):
    res, _out, _members = _build(tmp_path)
    assert res.babl_ops == 4
    assert res.gegl_ops == 3
    assert res.total_ops == 7


def test_every_op_so_is_executable_0755(tmp_path: Path):
    _res, _out, members = _build(tmp_path)
    so = {n: m for n, m in members.items() if n.endswith(".so")}
    assert so, "expected op .so members"
    assert all((m.mode & 0o777) == 0o755 for m in so.values()), {
        n: oct(m.mode & 0o777) for n, m in so.items()
    }


def test_members_are_real_files_dot_rooted(tmp_path: Path):
    _res, _out, members = _build(tmp_path)
    assert all(m.isfile() for m in members.values())
    assert all(n.startswith("./") for n in members)
    # the two canonical op paths land
    assert f"./{LIBDIR}/babl-0.1/CIE.so" in members
    assert f"./{LIBDIR}/gegl-0.4/exr-load.so" in members


def test_core_soname_libs_are_never_shipped(tmp_path: Path):
    """The base owns libbabl/libgegl flat SONAMEs — shipping them would risk a
    downgrade. Only versionless op modules belong in this overlay."""
    _res, _out, members = _build(tmp_path)
    assert not any("libbabl-0.1.so" in n for n in members)
    assert not any("libgegl-0.4.so" in n for n in members)
    assert not any("libgegl-npd-0.4.so" in n for n in members)
    assert not any("copyright" in n for n in members)


def test_overlay_is_stage_tar_conformant(tmp_path: Path):
    _res, out, _members = _build(tmp_path)
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors
    assert not rep.errors


def test_overlay_guard_zero_block_against_base_owning_flat_sonames(tmp_path: Path):
    """Base ships the flat SONAME libs (frozen) + 0644 op modules at the same
    paths. The overlay overwrites only the op modules (versionless → not frozen),
    so the guard must raise no BLOCK (and no WARN — op .so aren't sonames)."""
    _res, out, _members = _build(tmp_path)

    base = tmp_path / "base.tar"
    with tarfile.open(base, "w") as bt:
        for soname, payload in (
            ("libbabl-0.1.so.0", b"base babl flat"),
            ("libgegl-0.4.so.0", b"base gegl flat"),
        ):
            ti = tarfile.TarInfo(f"./{LIBDIR}/{soname}")
            ti.size = len(payload)
            ti.mode = 0o644
            bt.addfile(ti, io.BytesIO(payload))
        for opdir, name in (("babl-0.1", "CIE.so"), ("gegl-0.4", "exr-load.so")):
            ti = tarfile.TarInfo(f"./{LIBDIR}/{opdir}/{name}")
            payload = b"\x7fELF old " + name.encode()
            ti.size = len(payload)
            ti.mode = 0o644
            bt.addfile(ti, io.BytesIO(payload))

    violations = scan_overlay_violations(base, str(out))
    blocks = [v for v in violations if v.severity == BLOCK]
    warns = [v for v in violations if v.severity == WARN]
    assert blocks == [], [v.render() for v in blocks]
    assert warns == [], [v.render() for v in warns]


def test_empty_source_root_raises(tmp_path: Path):
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(RuntimeError, match="no babl/gegl op modules"):
        build_babl_gegl_overlay(tmp_path / "x.tar", source_root=empty)


def test_builder_selftest_passes():
    from tools.build_babl_gegl_overlay import _selftest

    assert _selftest() == 0
