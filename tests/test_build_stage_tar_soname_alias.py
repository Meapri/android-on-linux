"""build_stage_tar SONAME-alias synthesis (the libXaw.so.7 class of bug).

A few Debian shared libraries ship a REAL file whose basename encodes a different
soname than the binary's embedded DT_SONAME, reached on disk via a two-hop symlink
chain. The canonical case (which broke xcalc/xclock/xlogo/… on the ROOTFUL Xwayland
overlay) is libxaw7::

    libXaw.so.7        -> libXaw7.so.7          (symlink to a symlink)
    libXaw7.so.7       -> libXaw7.so.7.0.0      (SONAME symlink)
    libXaw7.so.7.0.0   (real file, DT_SONAME = "libXaw.so.7")

The §5-E flattener (build_stage_tar) keys off the FILENAME via parse_solib, so it
emits the real file renamed to its FILENAME-soname ``libXaw7.so.7`` and DROPS every
symlink in the group — including ``libXaw.so.7``. But every consumer DT_NEEDEDs the
*DT_SONAME* ``libXaw.so.7`` (verified against the real x11-apps binaries), so the
guest dies with "libXaw.so.7: cannot open shared object file".

The fix recovers the real DT_SONAME from the chosen ELF and, when it differs from
the flattened (filename-derived) name, re-creates the alias as a RELATIVE in-tree
symlink to the flat file. These tests prove: (1) the alias is emitted for the
mismatch case and resolves in-tree to the flat real file; (2) the resulting tar is
§5-E CONFORMANT with no guard violations against a base that lacks the soname; and
(3) the common case (filename == DT_SONAME) emits NO alias (byte-identical output).
"""

from __future__ import annotations

import struct
import tarfile
from pathlib import Path

from tools.build_stage_tar import build_stage_tar
from tools.elf_needed import read_elf_dynamic
from tools.stage_tar_spec import validate_stage_tar


# --------------------------------------------------------------------------- #
# A minimal, REAL (program-header-parseable) aarch64 ELF64-LE shared object so
# read_elf_dynamic recovers a genuine DT_SONAME — the parser walks PT_LOAD +
# PT_DYNAMIC (not section headers), so we lay everything out identity-mapped
# (p_vaddr == p_offset) in a single PT_LOAD.
# --------------------------------------------------------------------------- #
_EHDR = 64
_PHENT = 56
_NPH = 2  # PT_LOAD + PT_DYNAMIC


def _make_so(soname: str, needed: tuple[str, ...] = ()) -> bytes:
    # String table: index 0 is the mandatory NUL; then each name NUL-terminated.
    strtab = bytearray(b"\x00")
    offsets: dict[str, int] = {}
    for name in (soname, *needed):
        if name in offsets:
            continue
        offsets[name] = len(strtab)
        strtab += name.encode() + b"\x00"

    # Dynamic array: DT_NEEDED* , DT_SONAME, DT_STRTAB, DT_STRSZ, DT_NULL.
    dyn = bytearray()
    for name in needed:
        dyn += struct.pack("<qQ", 1, offsets[name])      # DT_NEEDED
    dyn += struct.pack("<qQ", 14, offsets[soname])       # DT_SONAME

    # Lay out: [ehdr][phdrs][dynamic][strtab]; identity vaddr==offset.
    ph_off = _EHDR
    dyn_off = ph_off + _NPH * _PHENT
    # DT_STRTAB/DT_STRSZ need the strtab vaddr; strtab follows the (now known)
    # dynamic array length. The dynamic array still needs two more entries
    # (STRTAB, STRSZ, NULL) appended below, so account for them first.
    dyn_len_final = len(dyn) + 3 * 16  # + DT_STRTAB + DT_STRSZ + DT_NULL
    strtab_off = dyn_off + dyn_len_final
    dyn += struct.pack("<qQ", 5, strtab_off)             # DT_STRTAB (vaddr==off)
    dyn += struct.pack("<qQ", 10, len(strtab))           # DT_STRSZ
    dyn += struct.pack("<qQ", 0, 0)                       # DT_NULL
    assert len(dyn) == dyn_len_final

    total = strtab_off + len(strtab)

    # ELF header: e_ident, e_type=ET_DYN(3), e_machine=EM_AARCH64(183).
    e_ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9
    ehdr = e_ident + struct.pack(
        "<HHIQQQIHHHHHH",
        3,        # e_type ET_DYN
        183,      # e_machine AARCH64
        1,        # e_version
        0,        # e_entry
        ph_off,   # e_phoff
        0,        # e_shoff (parser uses program headers)
        0,        # e_flags
        _EHDR,    # e_ehsize
        _PHENT,   # e_phentsize
        _NPH,     # e_phnum
        0, 0, 0,  # e_shentsize, e_shnum, e_shstrndx
    )

    # PT_LOAD covers the whole file; PT_DYNAMIC points at the dynamic array.
    ph_load = struct.pack(
        "<IIQQQQQQ", 1, 5, 0, 0, 0, total, total, 0x1000  # PT_LOAD, R+X
    )
    ph_dyn = struct.pack(
        "<IIQQQQQQ", 2, 6, dyn_off, dyn_off, dyn_off, len(dyn), len(dyn), 8
    )

    blob = bytearray(total)
    blob[0:_EHDR] = ehdr
    blob[ph_off:ph_off + _PHENT] = ph_load
    blob[ph_off + _PHENT:ph_off + 2 * _PHENT] = ph_dyn
    blob[dyn_off:dyn_off + len(dyn)] = dyn
    blob[strtab_off:strtab_off + len(strtab)] = strtab
    return bytes(blob)


def _libdir(root: Path) -> Path:
    d = root / "usr" / "lib" / "aarch64-linux-gnu"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _tar_members(tar_path: str) -> dict[str, tarfile.TarInfo]:
    with tarfile.open(tar_path, "r:*") as t:
        return {m.name: m for m in t.getmembers()}


# --------------------------------------------------------------------------- #
# the self-test of the ELF synthesizer (so a parser change can't silently make
# the alias tests vacuous)
# --------------------------------------------------------------------------- #

def test_synth_elf_parses_with_the_intended_dt_soname(tmp_path):
    p = tmp_path / "x.so"
    p.write_bytes(_make_so("libXaw.so.7", needed=("libc.so.6",)))
    ed = read_elf_dynamic(p)
    assert ed.is_elf
    assert ed.soname == "libXaw.so.7"
    assert "libc.so.6" in ed.needed


# --------------------------------------------------------------------------- #
# the bug + fix
# --------------------------------------------------------------------------- #

def _stage_libxaw(tmp_path) -> str:
    """Reproduce the on-disk libxaw7 layout and flatten it. Returns the tar path."""
    src = tmp_path / "src"
    libdir = _libdir(src)
    real = libdir / "libXaw7.so.7.0.0"
    real.write_bytes(_make_so("libXaw.so.7", needed=("libc.so.6",)))
    # Debian's two-hop chain: libXaw.so.7 -> libXaw7.so.7 -> libXaw7.so.7.0.0
    (libdir / "libXaw7.so.7").symlink_to("libXaw7.so.7.0.0")
    (libdir / "libXaw.so.7").symlink_to("libXaw7.so.7")

    out = tmp_path / "x11app-stage.tar"
    res = build_stage_tar(src, out)
    # The real lib flattens to its FILENAME soname...
    assert "usr/lib/aarch64-linux-gnu/libXaw7.so.7" in [
        m.lstrip("./") for m in _tar_members(str(out))
    ]
    # ...and the alias must be recorded.
    assert "usr/lib/aarch64-linux-gnu/libXaw.so.7" in res.soname_aliases
    return str(out)


def test_libxaw_alias_symlink_is_synthesized_and_resolves_in_tree(tmp_path):
    out = _stage_libxaw(tmp_path)
    members = _tar_members(out)
    norm = {m.lstrip("./"): info for m, info in members.items()}

    alias = "usr/lib/aarch64-linux-gnu/libXaw.so.7"
    flat = "usr/lib/aarch64-linux-gnu/libXaw7.so.7"
    assert alias in norm, "the DT_SONAME alias libXaw.so.7 must be shipped"
    info = norm[alias]
    assert info.issym(), "the alias must be a symlink, not a copy"
    # Relative, same-directory target pointing at the flat real lib.
    assert info.linkname == "libXaw7.so.7"
    assert flat in norm and norm[flat].isreg(), "flat real lib must exist as target"


def test_libxaw_overlay_is_conformant_with_no_guard_violations(tmp_path):
    out = _stage_libxaw(tmp_path)

    # A base that does NOT provide libXaw (only libc) — the realistic case: the
    # ALR base ships the X client libs but not libXaw7. The alias must not trip
    # any §5-E rule (it is in-tree, relative, single-version target, soname not
    # owned by the base).
    base_root = tmp_path / "base"
    _libdir(base_root).joinpath("libc.so.6").write_bytes(
        _make_so("libc.so.6")
    )

    rep = validate_stage_tar(out, base=base_root)
    assert rep.conformant, f"expected CONFORMANT, errors={rep.errors}"
    assert not rep.errors


def test_no_alias_when_filename_matches_dt_soname(tmp_path):
    """The overwhelming-majority case: a normal lib whose filename already equals
    its DT_SONAME gets NO alias — output is the plain flat file only."""
    src = tmp_path / "src"
    libdir = _libdir(src)
    # libfoo.so.1.2.3 with DT_SONAME libfoo.so.1 + the Debian SONAME link.
    real = libdir / "libfoo.so.1.2.3"
    real.write_bytes(_make_so("libfoo.so.1", needed=("libc.so.6",)))
    (libdir / "libfoo.so.1").symlink_to("libfoo.so.1.2.3")

    out = tmp_path / "stage.tar"
    res = build_stage_tar(src, out)
    assert res.soname_aliases == (), "no alias when filename == DT_SONAME"
    norm = {m.lstrip("./") for m in _tar_members(str(out))}
    assert "usr/lib/aarch64-linux-gnu/libfoo.so.1" in norm
    # No spurious extra symlink members beyond the single flat real file.
    so_members = [m for m in norm if "libfoo" in m]
    assert so_members == ["usr/lib/aarch64-linux-gnu/libfoo.so.1"], so_members


def test_alias_not_emitted_when_dt_soname_group_already_present(tmp_path):
    """If the overlay ALSO ships a real lib whose own flat name equals the alias
    target soname, we must not double-emit / collide. (Defensive: a hypothetical
    package shipping both libXaw7.so.* and a real libXaw.so.7.)"""
    src = tmp_path / "src"
    libdir = _libdir(src)
    libdir.joinpath("libXaw7.so.7.0.0").write_bytes(_make_so("libXaw.so.7"))
    (libdir / "libXaw7.so.7").symlink_to("libXaw7.so.7.0.0")
    # A real file that flattens to exactly libXaw.so.7 (the alias target name).
    libdir.joinpath("libXaw.so.7").write_bytes(_make_so("libXaw.so.7"))

    out = tmp_path / "stage.tar"
    res = build_stage_tar(src, out)
    # libXaw.so.7 is provided by a real group already -> no alias symlink added
    # for it (would clash with the real member).
    assert "usr/lib/aarch64-linux-gnu/libXaw.so.7" not in res.soname_aliases
    norm = {m.lstrip("./"): i for m, i in _tar_members(str(out)).items()}
    assert norm["usr/lib/aarch64-linux-gnu/libXaw.so.7"].isreg()
