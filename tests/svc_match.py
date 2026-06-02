"""Pure-python AArch64 ``svc`` site matcher + branch-rewrite encoder/decoder.

Host-side prototype for ADR-002 §3 (P3, M-R5-svcscan). It implements the *bit
exact* instruction analysis the on-device ``svc``-rewrite scanner needs, with NO
real execution: the device target is an aarch64 guest, and this host is darwin
(no arm64 self-rewrite / no live kernel seccomp), so only the instruction
bitfield logic is exercised here. The on-device scanner walks PT_LOAD text and
finds ``mov x8,#nr; svc #0`` sites; this module is the reusable, unit-tested core
of "is this word an svc?", "is the preceding word an x8 load?", and "what bytes
do I write to turn the svc into a BR to the trampoline?".

Everything is little-endian (AArch64 instruction stream is always LE) and every
instruction is a fixed 4 bytes.

ARM64 encodings used here (verified against ARM ARM / public references — see the
module-level ENCODING_SOURCES and the test file's docstring):

  svc #imm16        0xD4000001 | (imm16 << 5)         ; svc #0 == 0xD4000001
                    (LE bytes of svc #0: 01 00 00 d4)
  MOVZ Xd,#imm16    0xD2800000 | (hw << 21) | (imm16 << 5) | Rd     ; sf=1 opc=10
                    MOVZ x8,#imm (no shift) == 0xD2800008 | (imm16 << 5)
  MOVK Xd,#imm16    0xF2800000 | (hw << 21) | (imm16 << 5) | Rd     ; sf=1 opc=11
                    MOVK x8,#imm,LSL#(hw*16) == 0xF2800008 | (hw<<21) | (imm16<<5)
  BR Xn             0xD61F0000 | (Rn << 5)            ; mask 0xFFFFFC1F

Pure functions only; no external deps.
"""
from __future__ import annotations

from dataclasses import dataclass

# Authoritative bitfield sources (recorded for the ADR-002 notes / traceability).
ENCODING_SOURCES = (
    "https://developer.arm.com/documentation/ddi0602/latest/Base-Instructions",  # ARM ARM A64
    "https://www.scs.stanford.edu/~zyedidia/arm64/",  # A64 base-instructions tables
    "http://dinfuehr.com/blog/encoding-of-immediate-values-on-aarch64/",  # MOVZ/MOVK imm fields
    "https://devblogs.microsoft.com/oldnewthing/20220815-00/?p=106975",  # BR Xn encoding
)

INSN_SIZE = 4  # AArch64: fixed-length 32-bit instructions.

# ---- svc -------------------------------------------------------------------
_SVC_BASE = 0xD4000001          # svc #0
_SVC_IMM_MASK = 0xFFE0001F      # bits that are fixed for the svc-with-imm16 form
SVC0_WORD = 0xD4000001
SVC0_BYTES = SVC0_WORD.to_bytes(INSN_SIZE, "little")  # b"\x01\x00\x00\xd4"

# ---- wide-immediate moves (MOVZ / MOVK), 64-bit (sf=1) ---------------------
# Common fixed bits for the 64-bit move-wide group: sf=1 (bit31), bits[28:23]=100101.
_MOV_GROUP_MASK = 0x9F800000     # isolates sf + the 100101 group selector
_MOV_GROUP_VAL = 0x92800000      # sf=1, bits[28:23]=100101 (opc-agnostic)
_MOVZ_OPC = 0b10                 # opc=10 -> MOVZ
_MOVK_OPC = 0b11                 # opc=11 -> MOVK (MOVN opc=00 is NOT an x8 *load* we rewrite past)
_OPC_SHIFT = 29
_OPC_MASK = 0b11
_HW_SHIFT = 21
_HW_MASK = 0b11
_IMM16_SHIFT = 5
_IMM16_MASK = 0xFFFF
_RD_MASK = 0x1F

MOVZ_X8 = 0xD2800008             # MOVZ x8,#0 (no shift) base
MOVK_X8 = 0xF2800008             # MOVK x8,#0,LSL#0 base

# ---- BR --------------------------------------------------------------------
_BR_BASE = 0xD61F0000
_BR_MASK = 0xFFFFFC1F            # the 1-bits are fixed; Rn lives in bits[9:5]
_RN_SHIFT = 5
_RN_MASK = 0x1F


# --------------------------------------------------------------------------- #
# Word <-> bytes helpers (always little-endian, 4 bytes).
# --------------------------------------------------------------------------- #

def word_at(code: bytes, offset: int) -> int:
    """Little-endian 32-bit instruction word at ``offset`` (must be 4-aligned)."""
    if offset % INSN_SIZE != 0:
        raise ValueError(f"unaligned instruction offset {offset:#x} (need 4-byte align)")
    if offset < 0 or offset + INSN_SIZE > len(code):
        raise ValueError(f"offset {offset:#x} out of range for {len(code)} bytes")
    return int.from_bytes(code[offset:offset + INSN_SIZE], "little")


def word_to_bytes(word: int) -> bytes:
    """Encode a 32-bit instruction word to its 4 little-endian bytes."""
    if not 0 <= word <= 0xFFFFFFFF:
        raise ValueError(f"word {word:#x} does not fit in 32 bits")
    return word.to_bytes(INSN_SIZE, "little")


# --------------------------------------------------------------------------- #
# svc detection.
# --------------------------------------------------------------------------- #

def is_svc(word: int) -> bool:
    """True if ``word`` is an ``svc #imm16`` (any imm16, incl. the #0 storm form)."""
    return (word & _SVC_IMM_MASK) == _SVC_BASE


def svc_imm(word: int) -> int:
    """The imm16 of an ``svc`` word. Raises if ``word`` is not an svc."""
    if not is_svc(word):
        raise ValueError(f"{word:#010x} is not an svc")
    return (word >> _IMM16_SHIFT) & _IMM16_MASK


# --------------------------------------------------------------------------- #
# x8-load detection (the predecessor that makes an svc safely rewritable).
#
# The aarch64 syscall ABI puts the syscall number in x8, so a rewritable
# storm-style site is `MOV{Z,K} x8, #nr; svc #0`. We accept MOVZ x8 (the common
# single-word small-nr case) and MOVK x8 (used to OR-in upper halves of a wider
# immediate); both write x8 (Rd==8). MOVN is deliberately NOT accepted as the
# *immediate predecessor* form here (a bare `mov x8,#nr` lowers to MOVZ).
# --------------------------------------------------------------------------- #

def _mov_opc(word: int) -> int | None:
    """Return the move-wide opc (00/10/11) if ``word`` is a 64-bit move-wide, else None."""
    if (word & _MOV_GROUP_MASK) != _MOV_GROUP_VAL:
        return None
    return (word >> _OPC_SHIFT) & _OPC_MASK


def mov_rd(word: int) -> int:
    """Destination register of a move-wide word (low 5 bits)."""
    return word & _RD_MASK


def is_movz(word: int) -> bool:
    return _mov_opc(word) == _MOVZ_OPC


def is_movk(word: int) -> bool:
    return _mov_opc(word) == _MOVK_OPC


def is_x8_load(word: int) -> bool:
    """True if ``word`` is a MOVZ/MOVK that targets x8 (Rd==8).

    This is the predecessor pattern that lets the scanner treat the following
    ``svc`` as safely rewritable: x8 (the syscall-nr register) is established by
    a self-contained wide-immediate move immediately before the svc.
    """
    opc = _mov_opc(word)
    if opc not in (_MOVZ_OPC, _MOVK_OPC):
        return False
    return mov_rd(word) == 8


# --------------------------------------------------------------------------- #
# encode / decode round-trip primitives.
# --------------------------------------------------------------------------- #

def encode_movz_x8(imm16: int, hw: int = 0) -> int:
    """Encode ``MOVZ x8, #imm16, LSL #(hw*16)``."""
    if not 0 <= imm16 <= 0xFFFF:
        raise ValueError(f"imm16 {imm16:#x} out of range")
    if hw not in (0, 1, 2, 3):
        raise ValueError(f"hw {hw} out of range (0..3)")
    return MOVZ_X8 | (hw << _HW_SHIFT) | (imm16 << _IMM16_SHIFT)


def encode_movk_x8(imm16: int, hw: int = 0) -> int:
    """Encode ``MOVK x8, #imm16, LSL #(hw*16)``."""
    if not 0 <= imm16 <= 0xFFFF:
        raise ValueError(f"imm16 {imm16:#x} out of range")
    if hw not in (0, 1, 2, 3):
        raise ValueError(f"hw {hw} out of range (0..3)")
    return MOVK_X8 | (hw << _HW_SHIFT) | (imm16 << _IMM16_SHIFT)


def encode_svc(imm16: int = 0) -> int:
    """Encode ``svc #imm16`` (default ``svc #0``)."""
    if not 0 <= imm16 <= 0xFFFF:
        raise ValueError(f"imm16 {imm16:#x} out of range")
    return _SVC_BASE | (imm16 << _IMM16_SHIFT)


def encode_br(reg: int) -> int:
    """Encode ``BR X<reg>`` — indirect branch to the address in X<reg>."""
    if not 0 <= reg <= 30:  # x31 in this slot is SP/XZR; BR uses x0..x30
        raise ValueError(f"register x{reg} invalid for BR (0..30)")
    return _BR_BASE | (reg << _RN_SHIFT)


def is_br(word: int) -> bool:
    """True if ``word`` is a ``BR Xn``."""
    return (word & _BR_MASK) == _BR_BASE


def decode_br(word: int) -> int:
    """Return the Xn register number of a ``BR Xn`` word. Raises if not a BR."""
    if not is_br(word):
        raise ValueError(f"{word:#010x} is not a BR")
    return (word >> _RN_SHIFT) & _RN_MASK


# --------------------------------------------------------------------------- #
# The svc -> trampoline BR replacement.
#
# The simplest in-place rewrite (matching the design: re-use the existing
# alr_tramp_syscall via a register the scanner has parked the trampoline address
# in) swaps the 4-byte ``svc #0`` for a single 4-byte ``BR X<reg>``. Same width,
# 4-byte aligned, so it is a pure word-for-word patch — no shifting of following
# code. This function produces those replacement bytes and is the inverse of
# ``decode_br`` for round-trip verification.
# --------------------------------------------------------------------------- #

def make_svc_to_br_patch(reg: int) -> bytes:
    """Bytes that replace a 4-byte ``svc`` with ``BR X<reg>`` (the trampoline reg)."""
    patch = word_to_bytes(encode_br(reg))
    assert len(patch) == INSN_SIZE  # same width as the svc it overwrites
    return patch


# --------------------------------------------------------------------------- #
# The site walker.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class SvcSite:
    """One ``svc`` found in a .text blob.

    offset             byte offset into the scanned blob (always 4-aligned)
    addr               virtual address = base_addr + offset
    imm16              the svc's immediate (0 for the storm form)
    preceded_by_x8_load  the immediately-preceding 4 bytes are a MOVZ/MOVK x8
    rewritable         alias of preceded_by_x8_load — the site can be safely
                       turned into a BR because x8 is established by a
                       self-contained move right before it
    """

    offset: int
    addr: int
    imm16: int
    preceded_by_x8_load: bool

    @property
    def rewritable(self) -> bool:
        return self.preceded_by_x8_load


def find_svc_sites(code: bytes, base_addr: int = 0) -> list[SvcSite]:
    """Scan a 4-byte-aligned .text blob for every ``svc`` site.

    Returns the sites in ascending offset order. ``base_addr`` is the virtual
    address the blob is mapped at, so ``site.addr`` is the real svc PC the
    on-device scanner would compare against PT_LOAD ranges. A site is
    ``rewritable`` iff the immediately-preceding instruction is a MOVZ/MOVK that
    targets x8 (the syscall-number register).

    ``len(code)`` need not be a multiple of 4 (a trailing partial word, e.g. a
    section that does not end on an instruction boundary, is simply ignored).
    """
    if base_addr < 0:
        raise ValueError("base_addr must be non-negative")
    sites: list[SvcSite] = []
    n_full = len(code) - (len(code) % INSN_SIZE)
    for off in range(0, n_full, INSN_SIZE):
        word = int.from_bytes(code[off:off + INSN_SIZE], "little")
        if not is_svc(word):
            continue
        if off >= INSN_SIZE:
            prev = int.from_bytes(code[off - INSN_SIZE:off], "little")
            preceded = is_x8_load(prev)
        else:
            preceded = False  # first instruction: nothing precedes it
        sites.append(
            SvcSite(
                offset=off,
                addr=base_addr + off,
                imm16=svc_imm(word),
                preceded_by_x8_load=preceded,
            )
        )
    return sites
