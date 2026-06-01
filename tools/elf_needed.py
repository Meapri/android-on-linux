"""Pure-Python ELF dynamic-section reader (WS-4 / L4 rootfs).

Why this exists
---------------
WS-4 builds the guest rootfs overlay an app needs to run. The naive way to pick
shared libraries is the Debian ``Depends:`` closure (``tools.deb_closure``), but
that over-pulls: a package's Depends drags in perl, ICU, locale data and other
transitive packages an app never actually links against. The result is a bloated
overlay tar.

The precise signal is the binary itself: the ELF ``.dynamic`` section lists the
exact ``DT_NEEDED`` SONAMEs the loader will resolve. Closing over those (each
NEEDED lib's own NEEDED, recursively) yields the MINIMAL link-reachable set —
far smaller than the Depends closure. This module is the leaf primitive for that
walk: given one ELF file, return its declared SONAME and its ordered NEEDED list.

Scope / format
--------------
Target binaries are aarch64 (``e_machine == 183``), ELF64, little-endian, both
``ET_DYN`` (.so and PIE) and ``ET_EXEC``. The parser is hand-rolled with
``struct`` (no pyelftools dependency) and only reads what the ``.dynamic``
section needs: the program-header table (to find ``PT_DYNAMIC`` and to map the
string-table vaddr through ``PT_LOAD`` segments), the dynamic array, and the
string table itself.

Robustness
----------
A real-but-weird binary must never raise — bounds are checked everywhere and a
malformed structure degrades to "return what we have so far". Only genuine I/O
errors propagate. Non-ELF input returns ``ElfDyn(is_elf=False, ...)`` with empty
fields. Non-ELF64 / big-endian input still reports ``is_elf=True`` with parsed
``ei_class``/``ei_data`` but a best-effort (often empty) ``needed`` — the rootfs
is uniformly ELF64-LE so the 32-bit/BE paths are intentionally conservative.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ELF identification indices / constants
ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1
ELFDATA2MSB = 2
EM_AARCH64 = 183

# Program-header p_type
PT_LOAD = 1
PT_DYNAMIC = 2

# Dynamic-section d_tag values we care about
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
DT_SONAME = 14
DT_RPATH = 15
DT_RUNPATH = 29

# A sane cap so a corrupt d_filesz / phnum can't spin forever or allocate wildly.
_MAX_DYN_ENTRIES = 1 << 20


@dataclass(frozen=True)
class ElfDyn:
    """The dynamic-link facts read from one ELF file.

    is_elf:    file begins with the ELF magic.
    ei_class:  1 == ELF32, 2 == ELF64.
    ei_data:   1 == little-endian, 2 == big-endian.
    e_type:    ELF type (2 == ET_EXEC, 3 == ET_DYN).
    e_machine: target ISA (183 == AArch64).
    soname:    DT_SONAME string, or None when absent (typical for executables).
    needed:    DT_NEEDED SONAMEs in dynamic-array order.
    rpath:     DT_RPATH + DT_RUNPATH search paths, ':'-split, in order.
    """

    is_elf: bool
    ei_class: int
    ei_data: int
    e_type: int
    e_machine: int
    soname: str | None
    needed: tuple[str, ...]
    rpath: tuple[str, ...] = field(default=())


_NOT_ELF = ElfDyn(
    is_elf=False,
    ei_class=0,
    ei_data=0,
    e_type=0,
    e_machine=0,
    soname=None,
    needed=(),
    rpath=(),
)


def _cstr(buf: bytes, offset: int) -> str | None:
    """NUL-terminated string at ``offset`` in a string table, or None if OOB."""
    if offset < 0 or offset >= len(buf):
        return None
    end = buf.find(b"\x00", offset)
    if end < 0:
        end = len(buf)
    return buf[offset:end].decode("utf-8", "replace")


def read_elf_dynamic(path: str | Path) -> ElfDyn:
    """Parse the ELF dynamic section of ``path``.

    Returns an :class:`ElfDyn`. Never raises on a malformed-but-readable file;
    only I/O errors (file missing/unreadable) propagate. Non-ELF input yields
    ``ElfDyn(is_elf=False, ...)``.
    """
    data = Path(path).read_bytes()

    if len(data) < 64 or data[:4] != ELF_MAGIC:
        return _NOT_ELF

    ei_class = data[4]
    ei_data = data[5]

    # We only fully parse the real case: ELF64 little-endian. For any other
    # class/endianness, report identification but a conservative empty body —
    # the rootfs is uniformly ELF64-LE, so this only guards against junk input.
    if ei_class != ELFCLASS64 or ei_data != ELFDATA2LSB:
        # e_type (off 16, u16) and e_machine (off 18, u16) live at fixed offsets
        # in the ident-independent header; read them with the declared endianness
        # for an informative-but-safe result.
        endian = "<" if ei_data == ELFDATA2LSB else ">"
        try:
            e_type, e_machine = struct.unpack_from(endian + "HH", data, 16)
        except struct.error:
            e_type = e_machine = 0
        return ElfDyn(
            is_elf=True,
            ei_class=ei_class,
            ei_data=ei_data,
            e_type=e_type,
            e_machine=e_machine,
            soname=None,
            needed=(),
            rpath=(),
        )

    # ---- ELF64 little-endian header --------------------------------------- #
    # Elf64_Ehdr layout after e_ident[16]:
    #   H e_type, H e_machine, I e_version,
    #   Q e_entry, Q e_phoff, Q e_shoff, I e_flags,
    #   H e_ehsize, H e_phentsize, H e_phnum, ...
    try:
        (
            e_type,
            e_machine,
            _e_version,
            _e_entry,
            e_phoff,
            _e_shoff,
            _e_flags,
            _e_ehsize,
            e_phentsize,
            e_phnum,
        ) = struct.unpack_from("<HHIQQQIHHH", data, 16)
    except struct.error:
        return ElfDyn(True, ei_class, ei_data, 0, 0, None, (), ())

    base = ElfDyn(True, ei_class, ei_data, e_type, e_machine, None, (), ())

    # ---- Program headers: collect PT_LOAD segments and locate PT_DYNAMIC --- #
    # Elf64_Phdr: I p_type, I p_flags, Q p_offset, Q p_vaddr, Q p_paddr,
    #             Q p_filesz, Q p_memsz, Q p_align  (56 bytes)
    if e_phoff == 0 or e_phnum == 0 or e_phentsize < 56:
        return base  # no program headers -> nothing dynamic to read

    loads: list[tuple[int, int, int]] = []  # (p_vaddr, p_filesz, p_offset)
    dyn_off = dyn_size = 0
    for i in range(e_phnum):
        ph_at = e_phoff + i * e_phentsize
        if ph_at + 56 > len(data):
            break
        (
            p_type,
            _p_flags,
            p_offset,
            p_vaddr,
            _p_paddr,
            p_filesz,
            _p_memsz,
            _p_align,
        ) = struct.unpack_from("<IIQQQQQQ", data, ph_at)
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_filesz, p_offset))
        elif p_type == PT_DYNAMIC:
            dyn_off, dyn_size = p_offset, p_filesz

    if dyn_off == 0 or dyn_size == 0:
        return base  # static binary (no PT_DYNAMIC)

    def vaddr_to_off(vaddr: int) -> int | None:
        for p_vaddr, p_filesz, p_offset in loads:
            if p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        return None

    # ---- Walk the dynamic array ------------------------------------------- #
    # Elf64_Dyn: q d_tag (signed), Q d_val (16 bytes). Collect string-table
    # OFFSETS for NEEDED/SONAME/RPATH/RUNPATH and the STRTAB vaddr + STRSZ.
    needed_offsets: list[int] = []
    soname_offset: int | None = None
    rpath_offsets: list[int] = []
    strtab_vaddr: int | None = None
    strsz = 0

    dyn_end = min(dyn_off + dyn_size, len(data))
    cursor = dyn_off
    count = 0
    while cursor + 16 <= dyn_end and count < _MAX_DYN_ENTRIES:
        d_tag, d_val = struct.unpack_from("<qQ", data, cursor)
        cursor += 16
        count += 1
        if d_tag == DT_NULL:
            break
        if d_tag == DT_NEEDED:
            needed_offsets.append(d_val)
        elif d_tag == DT_SONAME:
            soname_offset = d_val
        elif d_tag in (DT_RPATH, DT_RUNPATH):
            rpath_offsets.append(d_val)
        elif d_tag == DT_STRTAB:
            strtab_vaddr = d_val
        elif d_tag == DT_STRSZ:
            strsz = d_val

    if strtab_vaddr is None or strsz <= 0:
        return base  # cannot resolve names without a string table

    strtab_off = vaddr_to_off(strtab_vaddr)
    if strtab_off is None:
        return base
    strtab = data[strtab_off : strtab_off + strsz]
    if not strtab:
        return base

    needed: list[str] = []
    for off in needed_offsets:
        s = _cstr(strtab, off)
        if s:
            needed.append(s)

    soname: str | None = None
    if soname_offset is not None:
        soname = _cstr(strtab, soname_offset) or None

    rpath: list[str] = []
    for off in rpath_offsets:
        s = _cstr(strtab, off)
        if not s:
            continue
        for part in s.split(":"):
            if part:
                rpath.append(part)

    return ElfDyn(
        is_elf=True,
        ei_class=ei_class,
        ei_data=ei_data,
        e_type=e_type,
        e_machine=e_machine,
        soname=soname,
        needed=tuple(needed),
        rpath=tuple(rpath),
    )


def needed_of(path: str | Path) -> tuple[str, ...]:
    """The DT_NEEDED SONAMEs of ``path`` (empty tuple if none / not ELF)."""
    return read_elf_dynamic(path).needed


def soname_of(path: str | Path) -> str | None:
    """The DT_SONAME of ``path``, or None."""
    return read_elf_dynamic(path).soname


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

def _print_file(path: str) -> None:
    info = read_elf_dynamic(path)
    print(path)
    if not info.is_elf:
        print("  not an ELF file")
        return
    cls = {1: "ELF32", 2: "ELF64"}.get(info.ei_class, f"class{info.ei_class}")
    end = {1: "LE", 2: "BE"}.get(info.ei_data, f"data{info.ei_data}")
    mach = "AArch64" if info.e_machine == EM_AARCH64 else f"machine={info.e_machine}"
    print(f"  {cls} {end} e_type={info.e_type} {mach}")
    print(f"  soname: {info.soname}")
    if info.rpath:
        print(f"  rpath:  {', '.join(info.rpath)}")
    print(f"  needed ({len(info.needed)}):")
    for n in info.needed:
        print(f"    {n}")


def _selftest() -> int:
    import re
    import tarfile
    import tempfile

    failures = 0

    def check(label: str, cond: bool, extra: str = "") -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        tail = f"  {extra}" if extra else ""
        print(f"  [{status}] {label}{tail}")

    soname_re = re.compile(r"^lib.*\.so")
    # NEEDED also legitimately lists the dynamic linker (ld-linux-aarch64.so.1),
    # which is a real SONAME but does not start with "lib".
    needed_re = re.compile(r"^(lib.*|ld-.*)\.so")

    tar_path = Path("app/src/main/assets/rootfs/payloads/tiny-rootfs.tar")
    if not tar_path.is_file():
        print(f"  [FAIL] base rootfs tar present: {tar_path} (run from worktree root)")
        print("\nselftest: 1 FAILED")
        return 1

    # Real arm64 libs to extract from the deterministic base tar.
    targets = {
        "libc.so.6": "./lib/aarch64-linux-gnu/libc.so.6",
        "libharfbuzz.so.0": "./usr/lib/aarch64-linux-gnu/libharfbuzz.so.0",
        "libfreetype.so.6": "./usr/lib/aarch64-linux-gnu/libfreetype.so.6",
        "libglib-2.0.so.0": "./usr/lib/aarch64-linux-gnu/libglib-2.0.so.0",
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        extracted: dict[str, Path] = {}
        with tarfile.open(tar_path, "r") as t:
            for label, member_name in targets.items():
                try:
                    src = t.extractfile(member_name)
                except KeyError:
                    src = None
                if src is None:
                    check(f"extract {label} from base tar", False, f"({member_name} missing)")
                    continue
                out = tmp_path / label
                out.write_bytes(src.read())
                extracted[label] = out

        parsed: dict[str, ElfDyn] = {}
        for label, out in extracted.items():
            parsed[label] = read_elf_dynamic(out)

        # Every extracted lib must be a clean ELF64 LE AArch64 object.
        for label, info in parsed.items():
            check(f"{label}: is_elf", info.is_elf)
            check(f"{label}: ELF64", info.ei_class == ELFCLASS64, f"(ei_class={info.ei_class})")
            check(f"{label}: little-endian", info.ei_data == ELFDATA2LSB, f"(ei_data={info.ei_data})")
            check(f"{label}: AArch64", info.e_machine == EM_AARCH64, f"(e_machine={info.e_machine})")

        # libharfbuzz: known SONAME + known transitive NEEDED entries.
        hb = parsed.get("libharfbuzz.so.0")
        if hb is not None:
            check("libharfbuzz soname", hb.soname == "libharfbuzz.so.0", f"(got {hb.soname!r})")
            check("libharfbuzz needs libc.so.6", "libc.so.6" in hb.needed)
            check("libharfbuzz needs libfreetype.so.6", "libfreetype.so.6" in hb.needed)
            check("libharfbuzz needs libglib-2.0.so.0", "libglib-2.0.so.0" in hb.needed)

        # libfreetype: SONAME + needs libc.
        ft = parsed.get("libfreetype.so.6")
        if ft is not None:
            check("libfreetype soname", ft.soname == "libfreetype.so.6", f"(got {ft.soname!r})")
            check("libfreetype needs libc.so.6", "libc.so.6" in ft.needed)

        # Generic sanity: sonames and needed entries look like real sonames.
        for label, info in parsed.items():
            if info.soname is not None:
                check(
                    f"{label}: soname matches ^lib.*\\.so",
                    bool(soname_re.match(info.soname)),
                    f"({info.soname!r})",
                )
            check(
                f"{label}: all NEEDED look like real sonames",
                all(needed_re.match(n) for n in info.needed),
                f"({list(info.needed)})",
            )

        # At least one lib must have a non-empty NEEDED list (proves the walk ran).
        check(
            "at least one lib reports non-empty NEEDED",
            any(p.needed for p in parsed.values()),
        )

        # Convenience wrappers agree with the dataclass.
        if hb is not None:
            check("needed_of() matches", needed_of(extracted["libharfbuzz.so.0"]) == hb.needed)
            check("soname_of() matches", soname_of(extracted["libharfbuzz.so.0"]) == hb.soname)

        # Non-ELF input -> is_elf False, empty needed.
        txt = tmp_path / "not-an-elf.txt"
        txt.write_text("this is plainly not an ELF binary\n" * 4)
        nonelf = read_elf_dynamic(txt)
        check("non-ELF text: is_elf False", nonelf.is_elf is False)
        check("non-ELF text: needed == ()", nonelf.needed == ())
        check("non-ELF text: soname None", nonelf.soname is None)

        # Truncated ELF (magic only) must not raise and reports not-ELF (too short).
        short = tmp_path / "short.bin"
        short.write_bytes(ELF_MAGIC + b"\x02\x01")
        check("truncated ELF does not crash", read_elf_dynamic(short).is_elf is False)

        # Evidence dump: the real soname + a few NEEDED we actually parsed.
        if hb is not None and hb.soname:
            sample = ", ".join(hb.needed[:6])
            print(f"\n  evidence: {hb.soname} NEEDED -> {sample}"
                  + (" ..." if len(hb.needed) > 6 else ""))

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="elf_needed",
        description="Read DT_SONAME / DT_NEEDED from aarch64 ELF binaries (pure Python).",
    )
    parser.add_argument("files", nargs="*", help="ELF files to inspect")
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.files:
        parser.error("give one or more ELF files, or use --selftest")
    for f in args.files:
        _print_file(f)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
