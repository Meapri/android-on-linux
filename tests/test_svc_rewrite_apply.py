"""Unit tests for the ``svc``→``br`` multi-site APPLY model (ADR-002 §2 2a).

``tests/svc_match.py`` (read-only matcher) is reused; this exercises
``tests/svc_rewrite_apply.py`` — the actual in-place replacement of many svc
sites with BRs, re-scan idempotency, the R3 branch-target safety filter, and the
stop-the-world / I-cache policy model. NO real execution (darwin host): pure
byte/transform + policy logic; live cache maintenance is DEVICE-ONLY.

Run:  uvx --with pytest pytest tests/test_svc_rewrite_apply.py -q
"""
from __future__ import annotations

import pytest

from tests import svc_match as sm
from tests import svc_rewrite_apply as ap

INSN = sm.INSN_SIZE
TRAMP_REG = 16  # x16: the intra-procedure-call scratch reg the trampoline parks in


# --------------------------------------------------------------------------- #
# Helpers: assemble little-endian instruction blobs from word lists.
# --------------------------------------------------------------------------- #

def blob(*words: int) -> bytes:
    return b"".join(w.to_bytes(INSN, "little") for w in words)


SVC0 = sm.SVC0_WORD                       # 0xD4000001
MOVZ_X8_56 = sm.encode_movz_x8(56)        # mov x8,#56 (openat)
MOVZ_X8_98 = sm.encode_movz_x8(98)        # mov x8,#98 (futex)
MOVZ_X8_172 = sm.encode_movz_x8(172)      # mov x8,#172 (getpid)
NOP = 0xD503201F                          # nop
BR_TRAMP = sm.encode_br(TRAMP_REG)        # br x16
RET = 0xD65F03C0                          # ret


def words_of(code: bytes) -> list[int]:
    return [int.from_bytes(code[i:i + INSN], "little") for i in range(0, len(code), INSN)]


# --------------------------------------------------------------------------- #
# 1) Single safe site: exact before/after bytes.
# --------------------------------------------------------------------------- #

def test_single_site_patches_svc_to_br_exact_bytes():
    code = blob(MOVZ_X8_56, SVC0)
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert res.rewritten_offs == (INSN,)            # the svc at offset 4
    assert res.skipped_already == ()
    assert res.skipped_unsafe == ()
    # The mov is untouched; the svc word became BR x16.
    assert words_of(res.patched) == [MOVZ_X8_56, BR_TRAMP]
    # And the patched bytes for the svc slot are exactly br x16 LE.
    assert res.patched[INSN:2 * INSN] == BR_TRAMP.to_bytes(INSN, "little")
    # Length preserved (in-place, 4-for-4).
    assert len(res.patched) == len(code)


def test_patched_br_decodes_to_the_trampoline_reg():
    code = blob(MOVZ_X8_98, SVC0)
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    patched_word = words_of(res.patched)[1]
    assert sm.is_br(patched_word)
    assert sm.decode_br(patched_word) == TRAMP_REG


# --------------------------------------------------------------------------- #
# 2) Many sites in one pass.
# --------------------------------------------------------------------------- #

def test_multi_site_patches_all_safe_svcs():
    code = blob(
        MOVZ_X8_56, SVC0,     # site A (offsets 0,4)
        NOP,                  # 8
        MOVZ_X8_98, SVC0,     # site B (12,16)
        MOVZ_X8_172, SVC0,    # site C (20,24)
        RET,                  # 28
    )
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert res.rewritten_offs == (4, 16, 24)
    out = words_of(res.patched)
    assert out[1] == BR_TRAMP and out[4] == BR_TRAMP and out[6] == BR_TRAMP
    # Non-svc words untouched.
    assert out[0] == MOVZ_X8_56 and out[2] == NOP and out[7] == RET


# --------------------------------------------------------------------------- #
# 3) Unsafe: svc with NO x8 load predecessor is left alone.
# --------------------------------------------------------------------------- #

def test_svc_without_x8_load_is_not_rewritten():
    code = blob(NOP, SVC0)  # preceded by nop, not a mov x8
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert res.rewritten_offs == ()
    assert res.skipped_unsafe == (INSN,)
    assert res.patched == code  # unchanged


def test_svc_at_offset_zero_is_not_rewritten():
    # The very first instruction has no predecessor -> never rewritable.
    code = blob(SVC0, RET)
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert res.rewritten_offs == ()
    assert res.skipped_unsafe == (0,)
    assert res.patched == code


# --------------------------------------------------------------------------- #
# 4) R3 branch-target safety — the adversarial case.
#    A svc that a DIRECT branch jumps onto is reached with x8 possibly undefined
#    (the mov x8 right before it is skipped), so it must NOT be rewritten even
#    though preceded_by_x8_load is True.
# --------------------------------------------------------------------------- #

def test_svc_that_is_a_direct_branch_target_is_excluded():
    # Layout (offsets):
    #   0: b  +12  (-> offset 12, the svc)   ; jumps PAST the mov x8
    #   4: nop
    #   8: mov x8,#56
    #  12: svc                                ; reached BOTH fall-through and via the branch
    #  16: ret
    b_to_12 = 0x14000000 | 3  # B with imm26=3 -> +12 bytes
    code = blob(b_to_12, NOP, MOVZ_X8_56, SVC0, RET)
    assert ap.direct_branch_target_off(b_to_12, 0) == 12
    sites = ap.classify_sites(code)
    svc_site = next(s for s in sites if s.offset == 12)
    assert svc_site.preceded_by_x8_load is True          # the mov IS right before
    assert svc_site.is_direct_branch_target is True       # but a branch lands on it
    assert svc_site.strictly_safe is False                # so NOT safe
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert res.rewritten_offs == ()
    assert 12 in res.skipped_unsafe
    assert res.patched == code


def test_backward_branch_target_offset_decodes_signed():
    # A backward B (negative imm) lands on an earlier svc.
    #   0: mov x8,#56
    #   4: svc           <- target of the branch at offset 8
    #   8: b -4  (imm26 = -1 -> -4 bytes -> offset 4)
    code = blob(MOVZ_X8_56, SVC0, 0x14000000 | (0x03FFFFFF & -1))
    b_word = words_of(code)[2]
    assert ap.direct_branch_target_off(b_word, 8) == 4
    sites = ap.classify_sites(code)
    svc_site = next(s for s in sites if s.offset == 4)
    assert svc_site.is_direct_branch_target is True
    assert svc_site.strictly_safe is False


@pytest.mark.parametrize(
    "branch_word, at_off, expect",
    [
        (0x14000000 | 2, 0, 8),                       # B +8
        (0x94000000 | 2, 0, 8),                       # BL +8
        (0x54000000 | (2 << 5), 0, 8),                # B.eq +8 (cond=0)
        (0x34000000 | (2 << 5), 0, 8),                # CBZ w0,+8
        (0x35000000 | (2 << 5), 0, 8),                # CBNZ w0,+8
        (0x36000000 | (2 << 5), 0, 8),                # TBZ +8
        (0x37000000 | (2 << 5), 0, 8),                # TBNZ +8
        (NOP, 0, None),                               # not a branch
        (RET, 0, None),                               # ret is not a PC-rel branch
    ],
)
def test_direct_branch_decoder_families(branch_word, at_off, expect):
    assert ap.direct_branch_target_off(branch_word, at_off) == expect


# Ground-truth encodings emitted by a REAL aarch64 assembler (clang -target
# aarch64-linux-gnu) for branches at the given offsets all targeting offset 0x24.
# These pin the decoder against the toolchain, not against my own arithmetic.
@pytest.mark.parametrize(
    "word, at_off, expect_target",
    [
        (0x14000009, 0x00, 0x24),   # b    0x24
        (0x94000008, 0x04, 0x24),   # bl   0x24
        (0x540000E0, 0x08, 0x24),   # b.eq 0x24
        (0xB40000C0, 0x0C, 0x24),   # cbz  x0, 0x24
        (0xB50000A0, 0x10, 0x24),   # cbnz x0, 0x24
        (0x36080080, 0x14, 0x24),   # tbz  w0,#1, 0x24
        (0x37080060, 0x18, 0x24),   # tbnz w0,#1, 0x24
    ],
)
def test_direct_branch_decoder_matches_real_assembler(word, at_off, expect_target):
    assert ap.direct_branch_target_off(word, at_off) == expect_target


def test_real_assembler_indirect_branches():
    # br x9 / blr x9 as emitted by the real assembler.
    assert ap.is_indirect_branch(0xD61F0120) is True   # br  x9
    assert ap.is_indirect_branch(0xD63F0120) is True   # blr x9
    # ...and neither is a direct branch target.
    assert ap.direct_branch_target_off(0xD61F0120, 0) is None
    assert ap.direct_branch_target_off(0xD63F0120, 0) is None


# --------------------------------------------------------------------------- #
# 5) Indirect branch in the blob -> no site is provably safe (conservative).
# --------------------------------------------------------------------------- #

def test_indirect_branch_excludes_all_sites():
    # A BR x9 somewhere in the blob: we can't prove the svc isn't an indirect
    # target, so the strict set is empty even with a clean mov x8; svc pattern.
    br_x9 = sm.encode_br(9)
    code = blob(MOVZ_X8_56, SVC0, br_x9, RET)
    assert ap.has_indirect_branch(code) is True
    sites = ap.classify_sites(code)
    svc_site = next(s for s in sites if s.offset == 4)
    assert svc_site.preceded_by_x8_load is True
    assert svc_site.maybe_indirect_target is True
    assert svc_site.strictly_safe is False
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert res.rewritten_offs == ()
    assert 4 in res.skipped_unsafe


def test_blr_counts_as_indirect():
    blr_x9 = 0xD63F0000 | (9 << 5)
    assert ap.is_indirect_branch(blr_x9) is True
    assert ap.is_indirect_branch(sm.encode_br(9)) is True
    assert ap.is_indirect_branch(NOP) is False


# --------------------------------------------------------------------------- #
# 6) Idempotency: re-applying over already-patched text is a no-op.
# --------------------------------------------------------------------------- #

def test_reapply_is_idempotent():
    code = blob(MOVZ_X8_56, SVC0, NOP, MOVZ_X8_98, SVC0, RET)
    first = ap.apply_svc_rewrites(code, TRAMP_REG)
    assert first.rewritten_offs == (4, 16)
    # Re-scan the already-patched bytes: nothing new, both sites seen as already-BR.
    second = ap.apply_svc_rewrites(first.patched, TRAMP_REG)
    assert second.rewritten_offs == ()
    assert set(second.skipped_already) == {4, 16}
    assert second.patched == first.patched  # bytes identical -> true no-op


def test_already_br_site_is_classified_not_resvc():
    # A blob where the svc slot is already a BR (preceded by a mov x8): the
    # classifier must report already_br=True, not treat it as a fresh svc.
    code = blob(MOVZ_X8_56, BR_TRAMP, RET)
    sites = ap.classify_sites(code)
    s = next(s for s in sites if s.offset == INSN)
    assert s.already_br is True
    assert s.strictly_safe is False  # already done -> not in the to-do set


def test_lone_br_not_preceded_by_x8_load_is_ignored():
    # A BR that is NOT one of our patches (no x8 load before it) is plain code;
    # it must not be reported as an already-br site.
    code = blob(NOP, BR_TRAMP, RET)
    sites = ap.classify_sites(code)
    assert all(not s.already_br for s in sites)
    assert sites == []  # nothing svc-like and no x8-preceded BR


# --------------------------------------------------------------------------- #
# 7) dlopen re-scan: base text already patched, only the new .so's sites change.
# --------------------------------------------------------------------------- #

def test_rescan_after_dlopen_only_touches_new_so():
    base = blob(MOVZ_X8_56, SVC0, RET)
    base_patched = ap.apply_svc_rewrites(base, TRAMP_REG).patched
    added = blob(MOVZ_X8_98, SVC0, NOP, MOVZ_X8_172, SVC0, RET)

    base_res, added_res = ap.rescan_after_dlopen(base_patched, added, TRAMP_REG)
    # Base re-scan: nothing new, the one site already a BR.
    assert base_res.rewritten_offs == ()
    assert base_res.skipped_already == (INSN,)
    assert base_res.patched == base_patched
    # New .so: its two safe svcs get patched.
    assert added_res.rewritten_offs == (4, 16)
    assert words_of(added_res.patched)[1] == BR_TRAMP
    assert words_of(added_res.patched)[4] == BR_TRAMP


# --------------------------------------------------------------------------- #
# 8) strictly_safe_sites convenience + count semantics.
# --------------------------------------------------------------------------- #

def test_strictly_safe_sites_filters_out_unsafe():
    b_to_12 = 0x14000000 | 3  # B -> offset 12
    code = blob(
        MOVZ_X8_56, SVC0,     # safe site at 4
        b_to_12,              # branch onto the svc at 12
        MOVZ_X8_98, SVC0,     # offset 16 is the svc; offset 12 is the mov... wait
        RET,
    )
    # Recompute: words at offsets 0,4,8,12,16,20.
    #   0  mov x8,#56
    #   4  svc            (safe: preceded by mov, no branch onto it)
    #   8  b +12 -> off 20 (the RET) — harmless, not onto a svc
    #  12  mov x8,#98
    #  16  svc            (safe)
    #  20  ret
    safe = ap.strictly_safe_sites(code)
    offs = [s.offset for s in safe]
    assert 4 in offs and 16 in offs


def test_count_invariant_rewritten_plus_skipped_equals_all_svcs():
    code = blob(
        MOVZ_X8_56, SVC0,   # safe (4)
        NOP, SVC0,          # unsafe: no x8 load (12)
        MOVZ_X8_98, SVC0,   # safe (20)
    )
    res = ap.apply_svc_rewrites(code, TRAMP_REG)
    all_svc_offsets = {s.offset for s in sm.find_svc_sites(code)}
    accounted = set(res.rewritten_offs) | set(res.skipped_unsafe) | set(res.skipped_already)
    assert accounted == all_svc_offsets


# --------------------------------------------------------------------------- #
# 9) Stop-the-world / cache policy model.
# --------------------------------------------------------------------------- #

def test_patch_policy_no_stop_the_world_aligned_atomic():
    code = blob(MOVZ_X8_56, SVC0, MOVZ_X8_98, SVC0, RET)
    pol = ap.patch_policy_for(code)
    assert pol.needs_stop_the_world is False      # single aligned 4B store is atomic
    assert pol.aligned_store_atomic is True
    assert pol.icache_maint_required is True       # IC IVAU + DSB ISH + ISB still needed
    assert pol.stale_icache_window is True         # benign one-shot old-svc execution


def test_all_safe_sites_are_4byte_aligned():
    # The premise that makes STW unnecessary: every site we patch is 4-aligned.
    code = blob(MOVZ_X8_56, SVC0, NOP, MOVZ_X8_98, SVC0, RET)
    for s in ap.strictly_safe_sites(code):
        assert s.offset % INSN == 0
        assert s.addr % INSN == 0


# --------------------------------------------------------------------------- #
# 10) base_addr threading: addr = base + offset.
# --------------------------------------------------------------------------- #

def test_base_addr_threads_into_site_addr():
    base = 0x7F_0000_0000
    code = blob(MOVZ_X8_56, SVC0)
    sites = ap.classify_sites(code, base_addr=base)
    svc_site = next(s for s in sites if s.offset == INSN)
    assert svc_site.addr == base + INSN


# --------------------------------------------------------------------------- #
# 11) Bad register is rejected by the underlying encoder (defensive).
# --------------------------------------------------------------------------- #

def test_apply_rejects_invalid_trampoline_reg():
    code = blob(MOVZ_X8_56, SVC0)
    with pytest.raises(ValueError):
        ap.apply_svc_rewrites(code, 31)  # x31 is SP/XZR -> invalid for BR
