"""Parity + contract tests for the in-interposer ``svc``-scan ROI probe.

ADR-002 §2 (2a) M-R5-svcscan, WS-1. The on-device scanner lives in C
(``app/src/main/cpp/alr_interpose/libalr_interpose.c::alr_svcscan_report``); the
*bit-exact* AArch64 instruction logic it relies on is independently unit-tested
in the pure-python oracle ``tests/svc_match.py`` (see
``tests/test_svc_rewrite_match.py``). Since the host is darwin (no arm64
self-rewrite / no live kernel seccomp), the C scanner cannot be run here. These
tests instead close the loop two ways:

  1. PARITY — extract the svc/MOVZ/MOVK/mask literals the C scanner hard-codes and
     assert they equal the python oracle's masks/bases numerically. If the C and
     python sides ever drift, the oracle's coverage stops guaranteeing the C
     behavior, so we pin them together.
  2. CONTRACT — assert the C function honors the M-R5 constraints from the ADR:
     read-only (no write/patch syscalls, no PROT_WRITE/mprotect), self-contained
     (every syscall via the trampoline ``alr_tramp_syscall``, no malloc, no libc
     string calls), side-effect-free (errno saved/restored), and that it is the
     ONLY new exported symbol (its helpers stay static).

Run:  uvx --with pytest pytest tests/test_svcscan_interpose_parity.py -q
"""
from __future__ import annotations

import re
from pathlib import Path

from tests import svc_match as sm

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "app/src/main/cpp/alr_interpose/libalr_interpose.c"


def _src() -> str:
    return SRC.read_text()


def _define_u32(text: str, name: str) -> int:
    """Read a ``#define <name> 0x....u`` 32-bit literal from the C source."""
    m = re.search(rf"#define\s+{re.escape(name)}\s+(0[xX][0-9A-Fa-f]+)u?\b", text)
    assert m, f"missing #define {name} in {SRC.name}"
    return int(m.group(1), 16)


# --------------------------------------------------------------------------- #
# 1. PARITY: C scanner masks == python oracle masks (numeric equality).
# --------------------------------------------------------------------------- #

def test_svc_mask_and_base_match_oracle():
    text = _src()
    assert _define_u32(text, "ALR_SVC_BASE") == sm.SVC0_WORD == 0xD4000001
    assert _define_u32(text, "ALR_SVC_IMM_MASK") == sm._SVC_IMM_MASK == 0xFFE0001F


def test_movwide_group_mask_matches_oracle():
    text = _src()
    assert _define_u32(text, "ALR_MOV_GROUP_MASK") == sm._MOV_GROUP_MASK == 0x9F800000
    assert _define_u32(text, "ALR_MOV_GROUP_VAL") == sm._MOV_GROUP_VAL == 0x92800000


def test_movwide_opc_and_rd_fields_match_oracle():
    text = _src()
    # opc selectors: MOVZ=0b10, MOVK=0b11; shifted at bit 29; Rd low 5 bits.
    assert _define_u32(text, "ALR_MOVZ_OPC") == sm._MOVZ_OPC == 0b10
    assert _define_u32(text, "ALR_MOVK_OPC") == sm._MOVK_OPC == 0b11
    assert int(re.search(r"#define\s+ALR_MOV_OPC_SHIFT\s+(\d+)", text).group(1)) == sm._OPC_SHIFT == 29
    assert _define_u32(text, "ALR_MOV_OPC_MASK") == sm._OPC_MASK == 0b11
    assert _define_u32(text, "ALR_MOV_RD_MASK") == sm._RD_MASK == 0x1F


def test_c_is_svc_predicate_is_oracle_identical():
    """The C predicate is `(w & ALR_SVC_IMM_MASK) == ALR_SVC_BASE`; verify that
    exact expression on the oracle's full svc-vs-not corpus (so a wrong mask in C
    would diverge here)."""
    text = _src()
    imm_mask = _define_u32(text, "ALR_SVC_IMM_MASK")
    base = _define_u32(text, "ALR_SVC_BASE")

    def c_is_svc(w: int) -> bool:
        return (w & imm_mask) == base

    # every svc imm form is accepted
    for imm in (0, 1, 0x42, 0x1234, 0xFFFF):
        assert c_is_svc(sm.encode_svc(imm)) is sm.is_svc(sm.encode_svc(imm)) is True
    # neighboring exception-gen insns (hvc/smc/brk) and unrelated ops rejected
    for w in (0xD4000002, 0xD4000003, 0xD4200000, sm.MOVZ_X8, sm.encode_br(16), 0xD503201F):
        assert c_is_svc(w) is sm.is_svc(w) is False


def test_c_is_x8_load_predicate_is_oracle_identical():
    """Reconstruct the C `alr_is_x8_load` from its #defines and check it equals
    the oracle `is_x8_load` over MOVZ/MOVK to x8, to other regs, and non-moves."""
    text = _src()
    grp_mask = _define_u32(text, "ALR_MOV_GROUP_MASK")
    grp_val = _define_u32(text, "ALR_MOV_GROUP_VAL")
    opc_shift = int(re.search(r"#define\s+ALR_MOV_OPC_SHIFT\s+(\d+)", text).group(1))
    opc_mask = _define_u32(text, "ALR_MOV_OPC_MASK")
    movz = _define_u32(text, "ALR_MOVZ_OPC")
    movk = _define_u32(text, "ALR_MOVK_OPC")
    rd_mask = _define_u32(text, "ALR_MOV_RD_MASK")

    def c_is_x8_load(w: int) -> bool:
        if (w & grp_mask) != grp_val:
            return False
        opc = (w >> opc_shift) & opc_mask
        if opc not in (movz, movk):
            return False
        return (w & rd_mask) == 8

    cases = [
        sm.encode_movz_x8(56), sm.encode_movz_x8(93), sm.encode_movk_x8(1, hw=1),
        (sm.MOVZ_X8 & ~0x1F) | 0,   # MOVZ x0
        (sm.MOVZ_X8 & ~0x1F) | 9,   # MOVZ x9
        0xD4000001, sm.encode_br(8), 0xD503201F,   # svc / BR x8 / NOP
    ]
    for w in cases:
        assert c_is_x8_load(w) == sm.is_x8_load(w), f"divergence on {w:#010x}"


def test_c_region_walk_matches_oracle_on_fixture():
    """Re-implement the C region walk (svc count + x8-load-preceded rewritable
    count, first-word-has-no-predecessor) from the C predicates and confirm it
    reproduces the oracle's find_svc_sites tallies on the shared fixture."""
    text = _src()
    imm_mask = _define_u32(text, "ALR_SVC_IMM_MASK")
    base = _define_u32(text, "ALR_SVC_BASE")
    grp_mask = _define_u32(text, "ALR_MOV_GROUP_MASK")
    grp_val = _define_u32(text, "ALR_MOV_GROUP_VAL")
    opc_shift = int(re.search(r"#define\s+ALR_MOV_OPC_SHIFT\s+(\d+)", text).group(1))
    opc_mask = _define_u32(text, "ALR_MOV_OPC_MASK")
    movz = _define_u32(text, "ALR_MOVZ_OPC")
    movk = _define_u32(text, "ALR_MOVK_OPC")
    rd_mask = _define_u32(text, "ALR_MOV_RD_MASK")

    def c_is_svc(w):
        return (w & imm_mask) == base

    def c_is_x8(w):
        if (w & grp_mask) != grp_val:
            return False
        opc = (w >> opc_shift) & opc_mask
        return opc in (movz, movk) and (w & rd_mask) == 8

    # Build a representative .text: mov x8 + svc (rewritable), NOP + svc (no),
    # movz x0 + svc (no), movk x8 + svc (yes), leading svc with no predecessor.
    NOP = 0xD503201F
    words = [
        sm.encode_movz_x8(56), sm.encode_svc(0),       # rewritable
        NOP, sm.encode_svc(0),                          # not
        (sm.MOVZ_X8 & ~0x1F) | 0, sm.encode_svc(0),     # movz x0 -> not
        sm.encode_movk_x8(0, hw=1), sm.encode_svc(42),  # rewritable, imm!=0
    ]
    blob = b"".join(w.to_bytes(4, "little") for w in words)

    # C-style walk over the blob (mirrors alr_svcscan_region: predecessor must be
    # in the SAME region; i==0 has no predecessor).
    c_sites = c_rewr = 0
    nwords = len(blob) // 4
    for i in range(nwords):
        w = int.from_bytes(blob[i * 4:i * 4 + 4], "little")
        if not c_is_svc(w):
            continue
        c_sites += 1
        if i > 0 and c_is_x8(int.from_bytes(blob[(i - 1) * 4:i * 4], "little")):
            c_rewr += 1

    oracle = sm.find_svc_sites(blob, base_addr=0x40_0000)
    o_sites = len(oracle)
    o_rewr = sum(1 for s in oracle if s.rewritable)
    assert (c_sites, c_rewr) == (o_sites, o_rewr) == (4, 2)


# --------------------------------------------------------------------------- #
# 2. CONTRACT: read-only, self-contained, side-effect-free, single export.
# --------------------------------------------------------------------------- #

def _scanner_body() -> str:
    """The text of the M-R5 scanner section (from its banner to EOF)."""
    text = _src()
    i = text.index("M-R5-svcscan: read-only")
    return text[i:]


def test_report_signature_is_stable_for_integration_wiring():
    text = _src()
    # The exact signature the integration session declares + calls.
    assert re.search(r"char\s*\*\s*alr_svcscan_report\s*\(\s*char\s*\*\s*buf\s*,"
                     r"\s*size_t\s+buflen\s*\)", text), \
        "alr_svcscan_report(char*, size_t) signature must stay stable"


def test_report_emits_stable_marker_prefix():
    body = _scanner_body()
    # The device gate greps this exact prefix.
    assert "ALR-SVCSCAN svc_sites=" in body
    assert "rewritable=" in body
    assert "exec_regions=" in body


def test_scanner_is_read_only_no_patch_syscalls():
    body = _scanner_body()
    # No write path at all: never opens maps for writing, never mprotect/mremap,
    # never emits a write/pwrite. (openat uses O_RDONLY only.)
    assert "O_RDONLY" in body
    for forbidden in ("__NR_write", "__NR_pwrite", "__NR_mprotect", "__NR_mremap",
                      "PROT_WRITE", "O_WRONLY", "O_RDWR", "O_CREAT", "memcpy("):
        assert forbidden not in body, f"scanner must be read-only: found {forbidden!r}"


def test_scanner_is_self_contained_through_trampoline():
    body = _scanner_body()
    # Every syscall the scanner issues goes through the single trusted-PC
    # trampoline (so it is PCGATE-correct and never traps the supervisor), and it
    # uses no heap and no interposable libc string routines.
    assert "alr_tramp_syscall(__NR_openat" in body
    assert "alr_tramp_syscall(__NR_read" in body
    assert "alr_tramp_syscall(__NR_close" in body
    for forbidden in ("malloc(", "calloc(", "fopen(", "getline(", "strlen(",
                      "strcpy(", "snprintf(", "sprintf(", "fprintf("):
        assert forbidden not in body, f"scanner must be self-contained: found {forbidden!r}"


def test_scanner_preserves_errno_side_effect_free():
    body = _scanner_body()
    # errno is captured on entry and restored on every return so a caller sees
    # no observable change (the ADR's "부작용 0" requirement).
    assert "int saved_errno = errno;" in body
    assert body.count("errno = saved_errno;") >= 2  # the fd-open-fail path + the normal path


def test_scanner_does_not_touch_pcgate_or_filter_state():
    body = _scanner_body()
    # It must not install/alter the seccomp filter or flip g_pcgate; it only
    # reads. (No SET_MODE_FILTER, no PR_SET_SECCOMP, no g_pcgate assignment.)
    assert "SECCOMP_SET_MODE_FILTER" not in body
    assert "PR_SET_SECCOMP" not in body
    assert "alr_install_pcgated_filter" not in body
    assert not re.search(r"\bg_pcgate\s*=", body), "scanner must not assign g_pcgate"
