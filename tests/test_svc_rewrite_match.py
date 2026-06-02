"""Unit tests for the AArch64 ``svc`` site matcher / BR-rewrite encoder.

ADR-002 §3 (P3, M-R5-svcscan) host prototype. These tests assert the *bit exact*
instruction encodings the on-device ``svc``-rewrite scanner depends on, and the
behavior of the synthetic .text walker. NO real execution: the host is darwin
(no arm64 self-rewrite, no live kernel seccomp), so this covers pure instruction
bitfield logic only.

ARM64 encoding bitfields were verified against the ARM ARM A64 base-instruction
tables and public references (recorded in ``svc_match.ENCODING_SOURCES``):

  svc #imm16    0xD4000001 | (imm16<<5)            svc #0 == 0xD4000001
                LE bytes of svc #0: 01 00 00 d4
  MOVZ Xd,#i    0xD2800000 | (hw<<21) | (i<<5) | Rd   (sf=1, opc=10)
                MOVZ x8,#i (no shift) == 0xD2800008 | (i<<5)
  MOVK Xd,#i    0xF2800000 | (hw<<21) | (i<<5) | Rd   (sf=1, opc=11)
  BR Xn         0xD61F0000 | (Rn<<5)               mask 0xFFFFFC1F

Run:  uvx --with pytest pytest tests/test_svc_rewrite_match.py -q
"""
from __future__ import annotations

import pytest

from tests import svc_match as sm


# --------------------------------------------------------------------------- #
# Reference encodings cross-checked against assembler output / the ARM ARM.
# These literals are the load-bearing ground truth; if any drifts, the whole
# matcher is wrong, so we pin them explicitly.
# --------------------------------------------------------------------------- #

def test_svc0_literal_and_le_bytes():
    assert sm.SVC0_WORD == 0xD4000001
    # svc #0 little-endian byte order, as the spec calls out: 01 00 00 d4.
    assert sm.SVC0_BYTES == b"\x01\x00\x00\xd4"
    assert sm.word_to_bytes(0xD4000001) == b"\x01\x00\x00\xd4"
    assert sm.encode_svc(0) == 0xD4000001


def test_svc_imm_shifts_into_bits_20_5():
    # svc #1 -> imm in bits[20:5]: 0xD4000001 | (1<<5) == 0xD4000021.
    assert sm.encode_svc(1) == 0xD4000021
    assert sm.encode_svc(0xFFFF) == (0xD4000001 | (0xFFFF << 5))
    assert sm.svc_imm(sm.encode_svc(0x1234)) == 0x1234


def test_movz_x8_base_and_imm_field():
    # MOVZ x8, #0 (no shift) == 0xD2800008 (sf=1 opc=10 hw=00 Rd=8).
    assert sm.MOVZ_X8 == 0xD2800008
    assert sm.encode_movz_x8(0) == 0xD2800008
    # MOVZ x8, #56 (openat nr) -> imm into bits[20:5]: 0xD2800008 | (56<<5).
    assert sm.encode_movz_x8(56) == (0xD2800008 | (56 << 5))
    assert sm.encode_movz_x8(56) == 0xD2800708


def test_movz_x8_hw_shift_field():
    # hw selects the LSL #(hw*16) slot, bits[22:21].
    assert sm.encode_movz_x8(0, hw=1) == (0xD2800008 | (1 << 21))
    assert sm.encode_movz_x8(0, hw=3) == (0xD2800008 | (3 << 21))


def test_movk_x8_base_and_opc():
    # MOVK x8, #0, LSL #0 == 0xF2800008 (sf=1 opc=11 hw=00 Rd=8).
    assert sm.MOVK_X8 == 0xF2800008
    assert sm.encode_movk_x8(0) == 0xF2800008
    assert sm.encode_movk_x8(0xABCD, hw=2) == (0xF2800008 | (2 << 21) | (0xABCD << 5))


def test_br_encoding_literal():
    # BR x16 == 0xD61F0200 (16<<5 == 0x200). A common veneer register.
    assert sm.encode_br(16) == 0xD61F0200
    # BR x0 == 0xD61F0000 (the bare base).
    assert sm.encode_br(0) == 0xD61F0000
    # BR x30 == 0xD61F03C0 (30<<5 == 0x3C0).
    assert sm.encode_br(30) == 0xD61F03C0


# --------------------------------------------------------------------------- #
# is_svc / svc_imm.
# --------------------------------------------------------------------------- #

def test_is_svc_accepts_svc_imm_variants():
    assert sm.is_svc(0xD4000001)            # svc #0
    assert sm.is_svc(sm.encode_svc(0x42))   # svc #0x42
    assert sm.is_svc(sm.encode_svc(0xFFFF))


def test_is_svc_rejects_other_exception_gen_insns():
    # hvc #0 == 0xD4000002, smc #0 == 0xD4000003, brk #0 == 0xD4200000.
    # These share the exception-generation top byte but are NOT svc.
    for word in (0xD4000002, 0xD4000003, 0xD4200000, 0xD4400000):
        assert not sm.is_svc(word), f"{word:#010x} wrongly matched as svc"


def test_is_svc_rejects_movz_and_br():
    assert not sm.is_svc(sm.MOVZ_X8)
    assert not sm.is_svc(sm.encode_br(16))


def test_svc_imm_raises_on_non_svc():
    with pytest.raises(ValueError):
        sm.svc_imm(sm.MOVZ_X8)


# --------------------------------------------------------------------------- #
# x8-load detection: MOVZ x8 / MOVK x8 yes; other Rd or other ops no.
# --------------------------------------------------------------------------- #

def test_is_x8_load_movz_and_movk_to_x8():
    assert sm.is_x8_load(sm.encode_movz_x8(56))     # MOVZ x8, #56
    assert sm.is_x8_load(sm.encode_movk_x8(0x1, hw=1))  # MOVK x8, #1, LSL#16


def test_is_x8_load_rejects_other_destination_register():
    # MOVZ x0, #56 (Rd=0) must NOT count as an x8 load.
    movz_x0 = (sm.MOVZ_X8 & ~0x1F) | 0  # clear Rd, set Rd=0
    assert sm.mov_rd(movz_x0) == 0
    assert not sm.is_x8_load(movz_x0)
    # MOVZ x9, #56 (Rd=9).
    movz_x9 = (sm.MOVZ_X8 & ~0x1F) | 9
    assert sm.mov_rd(movz_x9) == 9
    assert not sm.is_x8_load(movz_x9)


def test_is_x8_load_rejects_non_move_words():
    assert not sm.is_x8_load(0xD4000001)          # svc
    assert not sm.is_x8_load(sm.encode_br(8))     # BR x8 (not a move)
    assert not sm.is_x8_load(0xD503201F)          # NOP


def test_movz_movk_classifiers_are_disjoint():
    z = sm.encode_movz_x8(7)
    k = sm.encode_movk_x8(7)
    assert sm.is_movz(z) and not sm.is_movk(z)
    assert sm.is_movk(k) and not sm.is_movz(k)


# --------------------------------------------------------------------------- #
# BR encode/decode round-trip + validation.
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("reg", list(range(0, 31)))
def test_br_round_trip_all_registers(reg):
    word = sm.encode_br(reg)
    assert sm.is_br(word)
    assert sm.decode_br(word) == reg
    # bytes round-trip too.
    assert sm.word_at(sm.word_to_bytes(word), 0) == word


def test_br_rejects_invalid_register():
    with pytest.raises(ValueError):
        sm.encode_br(31)   # x31 slot is SP/XZR — not valid for BR
    with pytest.raises(ValueError):
        sm.encode_br(-1)


def test_decode_br_raises_on_non_br():
    with pytest.raises(ValueError):
        sm.decode_br(0xD4000001)  # svc, not a BR


def test_svc_to_br_patch_is_4_bytes_and_decodes_back():
    for reg in (16, 17, 9, 0, 30):
        patch = sm.make_svc_to_br_patch(reg)
        assert len(patch) == sm.INSN_SIZE
        word = sm.word_at(patch, 0)
        assert sm.is_br(word)
        assert sm.decode_br(word) == reg


def test_full_svc_rewrite_sequence_round_trip():
    # Build `mov x8,#56; svc #0`, then rewrite the svc word -> `BR x16`,
    # decode the patched stream, and confirm the trampoline register survives.
    text = sm.word_to_bytes(sm.encode_movz_x8(56)) + sm.SVC0_BYTES
    svc_off = 4
    assert sm.is_svc(sm.word_at(text, svc_off))
    patch = sm.make_svc_to_br_patch(16)
    patched = text[:svc_off] + patch + text[svc_off + 4:]
    # The mov is untouched; the svc became a BR x16.
    assert sm.is_x8_load(sm.word_at(patched, 0))
    assert sm.is_br(sm.word_at(patched, svc_off))
    assert sm.decode_br(sm.word_at(patched, svc_off)) == 16
    assert not sm.is_svc(sm.word_at(patched, svc_off))


# --------------------------------------------------------------------------- #
# Alignment: word_at rejects unaligned / out-of-range offsets.
# --------------------------------------------------------------------------- #

def test_word_at_rejects_unaligned_offset():
    blob = sm.word_to_bytes(0xD4000001) * 2
    for bad in (1, 2, 3, 5, 7):
        with pytest.raises(ValueError):
            sm.word_at(blob, bad)


def test_word_at_rejects_out_of_range():
    blob = sm.SVC0_BYTES  # exactly one word
    with pytest.raises(ValueError):
        sm.word_at(blob, 4)  # one past the end
    with pytest.raises(ValueError):
        sm.word_at(blob, -4)


def test_word_to_bytes_rejects_oversized():
    with pytest.raises(ValueError):
        sm.word_to_bytes(0x1_0000_0000)


# --------------------------------------------------------------------------- #
# Synthetic .text fixture: several svc sites with x8-load-preceding,
# not-preceding, and MOVK-preceding cases.
#
# Layout (each line = 4 bytes), base_addr = 0x40_0000:
#   off 0x00  MOVZ x8,#56     (openat)        <- x8 load
#   off 0x04  svc #0          rewritable      (preceded by MOVZ x8)
#   off 0x08  NOP                             (filler, breaks adjacency)
#   off 0x0C  svc #0          NOT rewritable  (preceded by NOP)
#   off 0x10  MOVZ x0,#1                       (x0, not x8)
#   off 0x14  svc #0          NOT rewritable  (preceded by MOVZ x0)
#   off 0x18  MOVK x8,#0,LSL#16                <- x8 load (MOVK form)
#   off 0x1C  svc #0          rewritable      (preceded by MOVK x8)
#   off 0x20  MOVZ x8,#93     (exit)          <- x8 load
#   off 0x24  svc #42         rewritable, imm16=42 (non-zero svc imm)
# --------------------------------------------------------------------------- #

NOP = 0xD503201F
BASE = 0x40_0000


def _build_text() -> bytes:
    words = [
        sm.encode_movz_x8(56),               # 0x00
        sm.encode_svc(0),                    # 0x04 rewritable
        NOP,                                 # 0x08
        sm.encode_svc(0),                    # 0x0C not rewritable
        (sm.MOVZ_X8 & ~0x1F) | 0,            # 0x10 MOVZ x0 (Rd=0); imm 0 ok for the test
        sm.encode_svc(0),                    # 0x14 not rewritable
        sm.encode_movk_x8(0, hw=1),          # 0x18 MOVK x8
        sm.encode_svc(0),                    # 0x1C rewritable
        sm.encode_movz_x8(93),               # 0x20 MOVZ x8,#93
        sm.encode_svc(42),                   # 0x24 rewritable, imm16=42
    ]
    return b"".join(sm.word_to_bytes(w) for w in words)


def test_find_svc_sites_counts_and_offsets():
    sites = sm.find_svc_sites(_build_text(), base_addr=BASE)
    offs = [s.offset for s in sites]
    assert offs == [0x04, 0x0C, 0x14, 0x1C, 0x24]
    # addr == base + offset.
    assert [s.addr for s in sites] == [BASE + o for o in offs]


def test_find_svc_sites_rewritable_flags():
    sites = {s.offset: s for s in sm.find_svc_sites(_build_text(), base_addr=BASE)}
    assert sites[0x04].rewritable is True            # MOVZ x8 before
    assert sites[0x04].preceded_by_x8_load is True
    assert sites[0x0C].rewritable is False           # NOP before
    assert sites[0x14].rewritable is False           # MOVZ x0 before
    assert sites[0x1C].rewritable is True            # MOVK x8 before
    assert sites[0x24].rewritable is True            # MOVZ x8 before


def test_find_svc_sites_records_svc_imm():
    sites = {s.offset: s for s in sm.find_svc_sites(_build_text(), base_addr=BASE)}
    assert sites[0x04].imm16 == 0
    assert sites[0x24].imm16 == 42   # the non-zero svc immediate survived


def test_rewritable_ratio_matches_fixture():
    sites = sm.find_svc_sites(_build_text(), base_addr=BASE)
    rewritable = sum(1 for s in sites if s.rewritable)
    # This is exactly the "rewritable=<n>/<n>" ROI ratio the device scanner logs.
    assert (rewritable, len(sites)) == (3, 5)


def test_first_instruction_svc_has_no_predecessor():
    # An svc as the very first word has nothing before it -> not rewritable,
    # and the walker must not read out of bounds.
    text = sm.SVC0_BYTES + sm.word_to_bytes(NOP)
    sites = sm.find_svc_sites(text, base_addr=BASE)
    assert len(sites) == 1
    assert sites[0].offset == 0
    assert sites[0].preceded_by_x8_load is False


def test_find_svc_sites_ignores_trailing_partial_word():
    # A blob whose length is not a multiple of 4: the trailing 2 bytes are not a
    # full instruction and must be skipped without error.
    text = sm.word_to_bytes(sm.encode_movz_x8(56)) + sm.SVC0_BYTES + b"\x00\x00"
    sites = sm.find_svc_sites(text, base_addr=BASE)
    assert [s.offset for s in sites] == [0x04]
    assert sites[0].rewritable is True


def test_find_svc_sites_empty_and_no_svc():
    assert sm.find_svc_sites(b"", base_addr=BASE) == []
    # A blob of pure NOPs has no svc.
    assert sm.find_svc_sites(sm.word_to_bytes(NOP) * 8, base_addr=BASE) == []


def test_find_svc_sites_rejects_negative_base():
    with pytest.raises(ValueError):
        sm.find_svc_sites(sm.SVC0_BYTES, base_addr=-1)


# --------------------------------------------------------------------------- #
# Whole-pipeline patch on the fixture: rewrite every rewritable svc to a
# BR x16 trampoline veneer and confirm only those words change, the rest is
# byte-identical, and re-scanning finds the non-rewritable svc still present.
# --------------------------------------------------------------------------- #

def test_patch_all_rewritable_sites_in_fixture():
    text = bytearray(_build_text())
    sites = sm.find_svc_sites(bytes(text), base_addr=BASE)
    patch = sm.make_svc_to_br_patch(16)
    for s in sites:
        if s.rewritable:
            text[s.offset:s.offset + 4] = patch

    patched = bytes(text)
    # The 3 rewritable svc became BR x16; the 2 non-rewritable svc remain svc.
    assert sm.is_br(sm.word_at(patched, 0x04)) and sm.decode_br(sm.word_at(patched, 0x04)) == 16
    assert sm.is_br(sm.word_at(patched, 0x1C))
    assert sm.is_br(sm.word_at(patched, 0x24))
    assert sm.is_svc(sm.word_at(patched, 0x0C))
    assert sm.is_svc(sm.word_at(patched, 0x14))

    # Re-scanning: only the 2 non-rewritable svc remain, both still flagged
    # non-rewritable (their predecessors were untouched).
    rescanned = sm.find_svc_sites(patched, base_addr=BASE)
    assert [s.offset for s in rescanned] == [0x0C, 0x14]
    assert all(not s.rewritable for s in rescanned)

    # Every non-svc word (the x8 loads, NOP, MOVZ x0) is byte-identical.
    original = _build_text()
    for off in (0x00, 0x08, 0x10, 0x18, 0x20):
        assert patched[off:off + 4] == original[off:off + 4]
