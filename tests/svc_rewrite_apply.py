"""Host model: ACTUALLY APPLYING the multi-site AArch64 ``svc``→``br`` rewrite.

ADR-002 §2 (2a) M-R5-svcscan, the *apply* half. ``tests/svc_match.py`` is the
read-only matcher (find sites, decide rewritability, encode a single BR). This
module is the next step the on-device scanner needs: given a .text blob and the
sites the matcher found, produce the PATCHED bytes (every safe ``svc`` replaced
in-place by a 4-byte ``BR X<reg>`` to the resident trampoline), and model the
parts that make a *real* multi-threaded, dlopen-incremental rewrite correct:

  * in-place 4-byte replacement of many sites at once (``apply_svc_rewrites``),
  * RE-SCAN IDEMPOTENCY — a site already patched to a ``BR`` is never re-patched
    (so a dlopen that adds a new .so and triggers a re-scan only touches the new
    sites, never the ones already done),
  * a conservative BRANCH-TARGET safety filter (R3): a ``svc`` that is the target
    of a *direct* branch (B/BL/CBZ/CBNZ/TBZ/TBNZ) is reached with x8 possibly
    undefined, so ``preceded_by_x8_load`` alone is necessary-but-insufficient;
    we down-rank such sites to NOT rewritable. Indirect branches (BR/BLR) cannot
    be resolved statically, so any site is flagged ``maybe_indirect_target`` and
    excluded from the strictly-safe set,
  * a STOP-THE-WORLD policy model: a single aligned 4-byte store is atomic on
    ARMv8 (a remote core sees either the whole old ``svc`` or the whole new
    ``br``, never a torn half), so patching does NOT require stopping sibling
    threads; the only residual is a remote-core stale I-cache that re-executes
    the OLD ``svc`` once (benign — it just takes the normal seccomp path that one
    time) until IC IVAU + DSB ISH + ISB lands. We model that as a policy object,
    not a lock.

NO real execution and NO real arm64 self-modification: the host is darwin. This
is the pure byte/transform + policy logic the device scanner reuses; the live
cache-maintenance / real seccomp behavior is DEVICE-ONLY (M-R5 gate).

All functions are pure (no I/O). They build on ``tests.svc_match`` for the bit
exact instruction encodings.
"""
from __future__ import annotations

from dataclasses import dataclass, replace

from tests import svc_match as sm

INSN_SIZE = sm.INSN_SIZE  # 4


# --------------------------------------------------------------------------- #
# Direct-branch decoders (the R3 branch-target safety check).
#
# We only need to know, for each instruction, whether it is a PC-relative direct
# branch and — if so — what byte offset in THIS blob it targets, so we can mark
# any ``svc`` landed on by such a branch as "reached with x8 possibly undefined"
# and therefore unsafe to rewrite even when the immediately-preceding word looks
# like an x8 load (that predecessor may be skipped by the branch).
#
# Encodings (AArch64, all imm fields are in instruction units = *4 bytes):
#   B    imm26   0x14000000 | imm26                 (mask 0xFC000000 == 0x14000000)
#   BL   imm26   0x94000000 | imm26                 (mask 0xFC000000 == 0x94000000)
#   B.cond imm19 0x54000000 | (imm19<<5) | cond     (mask 0xFF000010 == 0x54000000)
#   CBZ/CBNZ imm19  0x34000000 (CBZ) / 0x35000000 (CBNZ), sf at bit31
#                   mask 0x7E000000 == 0x34000000 ; imm19 in bits[23:5]
#   TBZ/TBNZ imm14  0x36000000 (TBZ) / 0x37000000 (TBNZ)
#                   mask 0x7E000000 == 0x36000000 ; imm14 in bits[18:5]
#   BR/BLR  Xn   0xD61F0000 / 0xD63F0000            (indirect — target unknowable)
# --------------------------------------------------------------------------- #

def _sext(value: int, bits: int) -> int:
    """Sign-extend ``value`` (an unsigned ``bits``-wide field) to a Python int."""
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def direct_branch_target_off(word: int, off: int) -> int | None:
    """Byte offset this direct branch at ``off`` targets, or None if not one.

    Returns ``off + (imm * 4)`` for B / BL / B.cond / CBZ / CBNZ / TBZ / TBNZ.
    Indirect branches (BR/BLR) return None here (see ``is_indirect_branch``):
    their target is a register value, not statically known.
    """
    # B / BL: 26-bit imm, opcode in top 6 bits.
    if (word & 0xFC000000) in (0x14000000, 0x94000000):
        imm = _sext(word & 0x03FFFFFF, 26)
        return off + imm * INSN_SIZE
    # B.cond: 0x54......, low bit4==0 (bit4 set would be BC.cond, still imm19).
    if (word & 0xFF000010) == 0x54000000:
        imm = _sext((word >> 5) & 0x7FFFF, 19)
        return off + imm * INSN_SIZE
    # CBZ / CBNZ: sf-agnostic mask 0x7E000000 == 0x34000000, imm19 at bits[23:5].
    if (word & 0x7E000000) == 0x34000000:
        imm = _sext((word >> 5) & 0x7FFFF, 19)
        return off + imm * INSN_SIZE
    # TBZ / TBNZ: mask 0x7E000000 == 0x36000000, imm14 at bits[18:5].
    if (word & 0x7E000000) == 0x36000000:
        imm = _sext((word >> 5) & 0x3FFF, 14)
        return off + imm * INSN_SIZE
    return None


def is_indirect_branch(word: int) -> bool:
    """True for ``BR Xn`` / ``BLR Xn`` (target is a register — unknowable here)."""
    return (word & 0xFFFFFC1F) in (0xD61F0000, 0xD63F0000)


def direct_branch_targets(code: bytes) -> set[int]:
    """All in-blob byte offsets reached by a *direct* branch within ``code``.

    Offsets outside ``[0, len(code))`` are dropped (cross-section branches do not
    constrain a svc inside this blob's analysis window).
    """
    targets: set[int] = set()
    n_full = len(code) - (len(code) % INSN_SIZE)
    for off in range(0, n_full, INSN_SIZE):
        word = int.from_bytes(code[off:off + INSN_SIZE], "little")
        tgt = direct_branch_target_off(word, off)
        if tgt is not None and 0 <= tgt < len(code):
            targets.add(tgt)
    return targets


def has_indirect_branch(code: bytes) -> bool:
    """True if ``code`` contains any BR/BLR (so any site might be an indirect target)."""
    n_full = len(code) - (len(code) % INSN_SIZE)
    for off in range(0, n_full, INSN_SIZE):
        word = int.from_bytes(code[off:off + INSN_SIZE], "little")
        if is_indirect_branch(word):
            return True
    return False


# --------------------------------------------------------------------------- #
# Safe-site selection: matcher rewritability AND not a (direct) branch target.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class SafeSite:
    """A ``svc`` site annotated for the *apply* decision.

    offset                 byte offset of the svc in the blob (4-aligned)
    addr                   virtual address (base + offset)
    preceded_by_x8_load    matcher's necessary precondition (svc_match)
    is_direct_branch_target  another instruction directly branches onto this svc
                           (so x8 may be undefined here -> NOT safe)
    maybe_indirect_target  the blob contains a BR/BLR; we cannot prove this svc
                           is not reached indirectly -> excluded from the strict set
    already_br             the bytes here are ALREADY a BR (a prior rewrite); the
                           apply pass must skip it (idempotency)
    """

    offset: int
    addr: int
    preceded_by_x8_load: bool
    is_direct_branch_target: bool
    maybe_indirect_target: bool
    already_br: bool

    @property
    def strictly_safe(self) -> bool:
        """Rewrite iff x8 is established, it is NOT a direct branch target, the
        blob has no indirect branches that could land here, and it is not already
        a BR. This is the conservative ``rewritable AND not_branch_target`` rule
        R3 calls for."""
        return (
            self.preceded_by_x8_load
            and not self.is_direct_branch_target
            and not self.maybe_indirect_target
            and not self.already_br
        )


def classify_sites(code: bytes, base_addr: int = 0) -> list[SafeSite]:
    """Find every svc/already-BR site and annotate it for the apply decision.

    Reuses ``svc_match.find_svc_sites`` for the svc detection + x8-load
    precondition, then layers the branch-target safety analysis (R3) and the
    idempotency (already-BR) check on top. A site whose 4 bytes are already a
    ``BR`` is reported with ``already_br=True`` so a re-scan skips it.
    """
    targets = direct_branch_targets(code)
    indirect = has_indirect_branch(code)
    out: list[SafeSite] = []

    # 1) svc sites the matcher recognizes.
    for s in sm.find_svc_sites(code, base_addr=base_addr):
        out.append(
            SafeSite(
                offset=s.offset,
                addr=s.addr,
                preceded_by_x8_load=s.preceded_by_x8_load,
                is_direct_branch_target=s.offset in targets,
                maybe_indirect_target=indirect,
                already_br=False,
            )
        )

    # 2) already-BR sites (from a prior apply): reported for idempotency so the
    #    caller's re-scan never double-patches. We only flag a BR as an
    #    already-done site if it is preceded by an x8 load — i.e. it sits exactly
    #    where a rewritable svc would have been (a BR somewhere else is ordinary
    #    code, not our patch).
    n_full = len(code) - (len(code) % INSN_SIZE)
    seen = {s.offset for s in out}
    for off in range(INSN_SIZE, n_full, INSN_SIZE):
        word = int.from_bytes(code[off:off + INSN_SIZE], "little")
        if not sm.is_br(word):
            continue
        prev = int.from_bytes(code[off - INSN_SIZE:off], "little")
        if not sm.is_x8_load(prev):
            continue
        if off in seen:
            continue
        out.append(
            SafeSite(
                offset=off,
                addr=base_addr + off,
                preceded_by_x8_load=True,
                is_direct_branch_target=off in targets,
                maybe_indirect_target=indirect,
                already_br=True,
            )
        )

    out.sort(key=lambda s: s.offset)
    return out


def strictly_safe_sites(code: bytes, base_addr: int = 0) -> list[SafeSite]:
    """Just the sites ``apply_svc_rewrites`` would patch (``strictly_safe``)."""
    return [s for s in classify_sites(code, base_addr=base_addr) if s.strictly_safe]


# --------------------------------------------------------------------------- #
# The apply transform.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class ApplyResult:
    """Outcome of one ``apply_svc_rewrites`` pass.

    patched          the new .text bytes (same length as the input)
    rewritten_offs   offsets that were turned from svc into BR THIS pass
    skipped_already  offsets that were already a BR (idempotency skip)
    skipped_unsafe   svc offsets left as svc (no x8 load / branch target / indirect)
    """

    patched: bytes
    rewritten_offs: tuple[int, ...]
    skipped_already: tuple[int, ...]
    skipped_unsafe: tuple[int, ...]


def apply_svc_rewrites(code: bytes, reg: int, base_addr: int = 0) -> ApplyResult:
    """Replace every strictly-safe ``svc`` in ``code`` with ``BR X<reg>``.

    * In-place, 4 bytes for 4 bytes — no following code shifts.
    * Idempotent: an offset already holding a ``BR`` (a prior pass) is skipped,
      so calling this again on already-patched text is a no-op for those sites.
    * Conservative: a svc that is a direct branch target, or that sits in a blob
      containing any indirect branch, or that lacks the x8-load predecessor, is
      LEFT as svc (reported in ``skipped_unsafe``).

    ``reg`` is the register the resident trampoline address was parked in (0..30).
    """
    patch = sm.make_svc_to_br_patch(reg)  # 4 bytes: BR X<reg>
    buf = bytearray(code)
    sites = classify_sites(code, base_addr=base_addr)

    rewritten: list[int] = []
    skipped_already: list[int] = []
    skipped_unsafe: list[int] = []
    for s in sites:
        if s.already_br:
            skipped_already.append(s.offset)
            continue
        if not s.strictly_safe:
            skipped_unsafe.append(s.offset)
            continue
        buf[s.offset:s.offset + INSN_SIZE] = patch
        rewritten.append(s.offset)

    return ApplyResult(
        patched=bytes(buf),
        rewritten_offs=tuple(rewritten),
        skipped_already=tuple(skipped_already),
        skipped_unsafe=tuple(skipped_unsafe),
    )


def rescan_after_dlopen(
    base_code: bytes,
    added_code: bytes,
    reg: int,
    *,
    base_addr: int = 0,
    added_addr: int = 0,
) -> tuple[ApplyResult, ApplyResult]:
    """Model a dlopen: re-apply over ALREADY-patched base text + the new .so.

    ``base_code`` is text a PRIOR pass already rewrote (its safe sites are now
    BRs). ``added_code`` is the freshly-dlopen'd .so's text. The contract this
    verifies:
      * the base re-scan rewrites NOTHING new (all its safe sites are already
        BRs -> ``rewritten_offs`` empty, ``skipped_already`` non-empty),
      * the added .so's own safe sites ARE rewritten this pass.
    Returns ``(base_result, added_result)``.
    """
    base_result = apply_svc_rewrites(base_code, reg, base_addr=base_addr)
    added_result = apply_svc_rewrites(added_code, reg, base_addr=added_addr)
    return base_result, added_result


# --------------------------------------------------------------------------- #
# Stop-the-world / cache-maintenance policy (model only; no real maintenance).
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class PatchPolicy:
    """Multi-thread safety policy for the on-device apply (modelled, not run).

    needs_stop_the_world   must sibling guest threads be stopped during patching?
                           False: a single 4-byte aligned store is atomic on
                           ARMv8 (B2.2.1 single-copy atomicity), so a concurrent
                           core observes either the whole old svc or the whole new
                           br — never a torn instruction. No lock needed.
    aligned_store_atomic   the 4-byte instruction store is naturally aligned and
                           single-copy atomic (the reason STW is unnecessary).
    stale_icache_window    a remote core may keep executing the OLD svc from its
                           I-cache until IC IVAU + DSB ISH + ISB; that one extra
                           execution is BENIGN (it just takes the normal seccomp
                           dispatch that one time), not a fault.
    icache_maint_required  the patch is incomplete until the I-cache is
                           invalidated for the patched line (IC IVAU; DSB ISH; ISB)
                           and a context-synchronizing event reached every core.
    """

    needs_stop_the_world: bool = False
    aligned_store_atomic: bool = True
    stale_icache_window: bool = True
    icache_maint_required: bool = True


def patch_policy_for(code: bytes, base_addr: int = 0) -> PatchPolicy:
    """The policy a patch over ``code`` would follow.

    Every svc we rewrite is 4-byte aligned (instructions are fixed 4 bytes and
    ``find_svc_sites`` enforces alignment), so the store is single-copy atomic and
    stop-the-world is never required regardless of site count. We still REQUIRE
    I-cache maintenance and flag the benign stale-window. (Constant today; kept a
    function so a future policy that DID need STW — e.g. spanning a page with a
    different protection story — has a single seam to express it.)
    """
    # Touch the inputs so the seam is real (and to assert our alignment premise).
    for s in strictly_safe_sites(code, base_addr=base_addr):
        assert s.offset % INSN_SIZE == 0  # premise of single-copy atomicity
    return PatchPolicy()


__all__ = [
    "INSN_SIZE",
    "direct_branch_target_off",
    "is_indirect_branch",
    "direct_branch_targets",
    "has_indirect_branch",
    "SafeSite",
    "classify_sites",
    "strictly_safe_sites",
    "ApplyResult",
    "apply_svc_rewrites",
    "rescan_after_dlopen",
    "PatchPolicy",
    "patch_policy_for",
    "replace",  # re-export for test convenience (dataclasses.replace)
]
