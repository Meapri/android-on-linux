#!/usr/bin/env python3
# tests/test_elf_remap_model.py — host (device-less) gate for the in-process ELF
# re-map mapper diagnosis. Encodes, as deterministic assertions:
#
#   * the CORRECT mapper invariants (span/page-union, RELRO must-end-RO, ABI reloc
#     constants, auxv completeness, TP handoff), and
#   * the EXACT deviations of the current trampoline (alr_inproc_reexec.c) from the
#     proven-working loader (runtime_report.cpp), so a fix is a green diff.
#
# Run (host): cd /Users/naen/Documents/alr-static-reentry &&
#   PATH="$HOME/.local/bin:$PATH" uvx --with pytest pytest tests/test_elf_remap_model.py -q
#
# S1/S2 mapping:
#   S1 PRIMARY (TP=NULL canary/TP-relative fault, the single delta for the observed
#     ET_EXEC /bin/sh w/ IREL=0)  -> test_tp_*  + test_static_etexec_only_tp_defect
#   S1 CO-PRIMARY (IRELATIVE const swap, active on DT_RELA targets) -> test_reloc_*
#   S2 (brk-unset)  -> documented as self-healed on noble glibc 2.39 (mmap fallback);
#     test_brk_is_not_a_hard_lock_on_noble pins that conclusion.
#   RELRO low-severity -> test_relro_*

import importlib.util
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_MODEL = os.path.join(os.path.dirname(_HERE), "tools", "elf_remap_model.py")
_spec = importlib.util.spec_from_file_location("elf_remap_model", _MODEL)
M = importlib.util.module_from_spec(_spec)
sys.modules["elf_remap_model"] = M
_spec.loader.exec_module(M)


# --------------------------------------------------------------------------
# (0) Ground-truth ABI constants — the swap the trampoline made.
# --------------------------------------------------------------------------
def test_abi_relocation_constants_are_canonical():
    # ARM AAELF64: RELATIVE=1027, IRELATIVE=1032. The trampoline's bug is using
    # 1027 for IRELATIVE. Pin the truth so a regression of the model is caught.
    assert M.R_AARCH64_RELATIVE == 1027
    assert M.R_AARCH64_IRELATIVE == 1032
    assert M.R_AARCH64_RELATIVE != M.R_AARCH64_IRELATIVE


def test_trampoline_defines_irelative_as_relative():
    # The recorded trampoline config carries the actual buggy #define.
    assert M.TRAMPOLINE_CONFIG.irelative_define == M.R_AARCH64_RELATIVE
    # The working loader uses the correct constant.
    assert M.WORKING_LOADER_CONFIG.irelative_define == M.R_AARCH64_IRELATIVE


# --------------------------------------------------------------------------
# (1) Single-span + per-page UNION permissions (R11 model).
# --------------------------------------------------------------------------
def test_span_is_single_reservation_page_rounded():
    img = M.fixture_static_etexec_relro()
    span = M.model_span(img)
    # min_v page-down of the lowest PT_LOAD (0x400000), max_v covers data+bss.
    assert span.min_v == 0x400000
    assert span.span > 0  # a single contiguous reservation was computed
    assert span.span % M.PAGE == 0  # page-rounded
    # Every page that a PT_LOAD actually covers gets a non-zero union prot; pages
    # falling in an inter-segment gap stay PROT_NONE (prot==0), like a real loader.
    loads = img.loads()

    def covered(va: int) -> bool:
        return any(
            M._page_down(p.p_vaddr) <= va < M._page_up(p.p_vaddr + p.p_memsz)
            for p in loads
        )

    assert all(pp.prot != 0 for pp in span.pages if covered(pp.vaddr))
    assert all(pp.prot == 0 for pp in span.pages if not covered(pp.vaddr))
    # The fixture deliberately has a gap between RX text and RW data (separate-code),
    # so at least one PROT_NONE gap page exists — confirming gap modeling works.
    assert any(pp.prot == 0 for pp in span.pages)


def test_boundary_page_keeps_union_perms_no_clobber():
    # Two PT_LOADs sharing a boundary page: the RX text tail and the RW data head.
    # The R11 single-span union must give that page R|W|X-union, never drop X.
    img = M.ElfImage(
        e_type=M.ET_EXEC,
        e_entry=0x400000,
        phdrs=(
            M.Phdr(M.PT_LOAD, M.PF_R | M.PF_X, 0x400000, 0x1800, 0x1800, 0x0),
            # data starts inside the same page as text's tail (0x401000 boundary):
            M.Phdr(M.PT_LOAD, M.PF_R | M.PF_W, 0x401800, 0x800, 0x800, 0x1800),
        ),
    )
    span = M.model_span(img)
    # The shared page at 0x401000 is covered by the RX seg (ends 0x401800) only;
    # 0x401000..0x402000 holds both the RX tail and the RW head → union RWX.
    page = next(p for p in span.pages if p.vaddr == 0x401000)
    assert page.prot & M.PF_X, "boundary page must keep X (no clobber)"
    assert page.prot & M.PF_W, "boundary page also needs W for the data head"
    # And the model surfaces it as a W^X violation page (the mapper must split or
    # the linker must separate-code; modern -z separate-code avoids this).
    assert 0x401000 in span.rwx_pages


def test_separated_code_has_no_wx_page():
    # The realistic static fixture uses -z separate-code (full-page gap), so no
    # page is both W and X — the W^X guard never trips.
    img = M.fixture_static_etexec_relro()
    span = M.model_span(img)
    assert span.rwx_pages == [], "separated code/data must not yield any RWX page"


# --------------------------------------------------------------------------
# (2) PT_GNU_RELRO — identified, and the trampoline leaves it RW.
# --------------------------------------------------------------------------
def test_relro_segment_identified():
    img = M.fixture_static_etexec_relro()
    span = M.model_span(img)
    relro = M.model_relro(img, span, applies_relro=False)
    assert relro.present
    assert relro.pages, "RELRO pages enumerated"


def test_trampoline_leaves_relro_writable():
    # DECISION MODEL: current mapper has no RELRO mprotect → RELRO pages stay RW.
    img = M.fixture_static_etexec_relro()
    span = M.model_span(img)
    relro = M.model_relro(img, span, applies_relro=M.TRAMPOLINE_CONFIG.applies_relro)
    assert relro.not_ro_pages, "trampoline leaves PT_GNU_RELRO pages writable"


def test_relro_fix_makes_pages_ro():
    # With the fix (applies_relro=True), no RELRO page remains writable.
    img = M.fixture_static_etexec_relro()
    span = M.model_span(img)
    relro = M.model_relro(img, span, applies_relro=True)
    assert relro.not_ro_pages == [], "RELRO mprotect RO closes the gap"


def test_relro_is_low_severity_for_non_pie():
    # The diagnosis classifies the RELRO gap as LOW (non-PIE ET_EXEC never calls
    # _dl_protect_relro), so it is NOT a startup-crash candidate by itself.
    img = M.fixture_static_etexec_relro()
    d = M.diagnose(img, M.TRAMPOLINE_CONFIG, dynamic=False)
    relro_findings = [f for f in d.findings if f.startswith("LOW[relro]")]
    assert relro_findings, "RELRO gap reported"
    # It must not be tagged PRIMARY.
    assert not any(f.startswith("PRIMARY") and "relro" in f.lower() for f in d.findings)


# --------------------------------------------------------------------------
# (3) Reloc classification — the constant swap and its masking.
# --------------------------------------------------------------------------
def test_relative_called_as_ifunc_on_dt_rela_target():
    # static-PIE with RELATIVE + IRELATIVE in DT_RELA: the buggy define (1027)
    # makes the loop CALL the RELATIVE relocs as resolvers AND skip real IRELATIVE.
    img = M.fixture_static_pie_relative_irelative()
    actual = M.model_reloc_trampoline(img, M.TRAMPOLINE_CONFIG.irelative_define)
    assert actual.misclassified_relative_called_as_ifunc == 2, (
        "both RELATIVE relocs wrongly invoked as ifunc resolvers"
    )
    assert actual.irelative_skipped == 1, "the real IRELATIVE reloc is skipped"


def test_correct_constant_classifies_relocs_right():
    # With 1032, RELATIVE relocs are NOT called and the real IRELATIVE is applied.
    img = M.fixture_static_pie_relative_irelative()
    actual = M.model_reloc_trampoline(img, M.WORKING_LOADER_CONFIG.irelative_define)
    assert actual.misclassified_relative_called_as_ifunc == 0
    assert actual.irelative_skipped == 0
    truth = M.model_reloc_truth(img)
    assert truth.n_relative == 2
    assert truth.n_irelative == 1


def test_constant_bug_masked_for_plain_etexec():
    # The device-observed /bin/sh is ET_EXEC with NO PT_DYNAMIC → no DT_RELA → the
    # trampoline's reloc loop never runs → constant bug cannot fire. So for THAT
    # target the reloc swap is NOT the crash cause (it must be TP).
    img = M.fixture_static_etexec_relro()
    actual = M.model_reloc_trampoline(img, M.TRAMPOLINE_CONFIG.irelative_define)
    assert actual.misclassified_relative_called_as_ifunc == 0
    assert actual.irelative_skipped == 0
    assert actual.n_irelative == 0  # IREL=0, matches device evidence


# --------------------------------------------------------------------------
# (4) auxv completeness.
# --------------------------------------------------------------------------
def test_trampoline_auxv_complete_for_static_and_dynamic():
    # The trampoline DOES synthesize the full required tag set — auxv is NOT a
    # missing-piece for either path (rules it out as the primary lock).
    assert M.missing_auxv(set(M.TRAMPOLINE_CONFIG.present_auxv), dynamic=False) == set()
    assert M.missing_auxv(set(M.TRAMPOLINE_CONFIG.present_auxv), dynamic=True) == set()


def test_missing_at_phdr_is_flagged():
    # Sanity: a mapper that forgot AT_PHDR would be caught (static-PIE self-reloc
    # needs it). Guards the auxv check itself.
    partial = frozenset(M.TRAMPOLINE_CONFIG.present_auxv) - {M.AT_PHDR}
    assert M.AT_PHDR in M.missing_auxv(set(partial), dynamic=False)


# --------------------------------------------------------------------------
# (5) Thread-pointer handoff — S1 PRIMARY.
# --------------------------------------------------------------------------
def test_tp_null_without_tcb_is_risky():
    tp = M.model_tp(tp_is_null=True, has_zeroed_tcb_region=False)
    assert M.tp_fault_risk(tp) is True
    assert tp.code_runs_before_tls_init is True  # csu runs pre-ARCH_SETUP_TLS


def test_working_loader_tp_is_safe():
    tp = M.model_tp(tp_is_null=False, has_zeroed_tcb_region=True)
    assert M.tp_fault_risk(tp) is False


def test_trampoline_vs_working_loader_single_tp_delta():
    # The decisive contrast: the ONLY TP-handoff difference between the two mappers.
    assert M.TRAMPOLINE_CONFIG.tp_is_null is True
    assert M.TRAMPOLINE_CONFIG.has_zeroed_tcb_region is False
    assert M.WORKING_LOADER_CONFIG.tp_is_null is False
    assert M.WORKING_LOADER_CONFIG.has_zeroed_tcb_region is True
    assert M.tp_fault_risk(M.model_tp(M.TRAMPOLINE_CONFIG.tp_is_null,
                                      M.TRAMPOLINE_CONFIG.has_zeroed_tcb_region))
    assert not M.tp_fault_risk(M.model_tp(M.WORKING_LOADER_CONFIG.tp_is_null,
                                          M.WORKING_LOADER_CONFIG.has_zeroed_tcb_region))


# --------------------------------------------------------------------------
# End-to-end diagnosis on the two key targets.
# --------------------------------------------------------------------------
def test_static_etexec_only_tp_defect_is_primary():
    # For the device-observed ET_EXEC /bin/sh (IREL=0, no DT_RELA): the ONLY live
    # PRIMARY finding must be the TP handoff — the reloc swap is masked, RELRO is
    # low, auxv is complete. This is the model's core claim for that target.
    img = M.fixture_static_etexec_relro()
    d = M.diagnose(img, M.TRAMPOLINE_CONFIG, dynamic=False)
    primaries = [f for f in d.findings if f.startswith("PRIMARY")]
    assert len(primaries) == 1, f"expected exactly one PRIMARY, got {primaries}"
    assert primaries[0].startswith("PRIMARY[TP]")
    assert d.tp_risky is True


def test_static_pie_has_both_tp_and_reloc_primaries():
    # For a static-PIE with DT_RELA: BOTH the TP handoff and the reloc swap fire.
    img = M.fixture_static_pie_relative_irelative()
    d = M.diagnose(img, M.TRAMPOLINE_CONFIG, dynamic=False)
    primaries = [f for f in d.findings if f.startswith("PRIMARY")]
    assert any(f.startswith("PRIMARY[TP]") for f in primaries)
    assert any(f.startswith("PRIMARY[reloc]") for f in primaries)


def test_working_loader_config_has_no_primary_findings():
    # Applying the proven-working loader's config to either target yields NO PRIMARY
    # findings — proving the two fixes (1032 + zeroed-TCB) are exactly what closes them.
    for img in (M.fixture_static_etexec_relro(),
                M.fixture_static_pie_relative_irelative()):
        d = M.diagnose(img, M.WORKING_LOADER_CONFIG, dynamic=False)
        primaries = [f for f in d.findings if f.startswith("PRIMARY")]
        assert primaries == [], f"working loader should clear all PRIMARYs, got {primaries}"


# --------------------------------------------------------------------------
# S2 — brk: documented as NOT a hard lock on Ubuntu noble (glibc 2.39).
# --------------------------------------------------------------------------
def test_brk_is_not_a_hard_lock_on_noble():
    # glibc >= 2.34 _dl_early_allocate falls back to mmap(MAP_ANONYMOUS) when
    # __sbrk fails, so a re-mapped guest with an arbitrary program break still
    # allocates its TCB. The model therefore does NOT raise brk as a PRIMARY/
    # SECONDARY crash lock for noble. This test pins that conclusion: no finding
    # mentions brk as a fault.
    img = M.fixture_static_etexec_relro()
    d = M.diagnose(img, M.TRAMPOLINE_CONFIG, dynamic=False)
    assert not any("brk" in f.lower() for f in d.findings), (
        "brk must not be reported as a crash lock on noble glibc 2.39 (mmap fallback)"
    )


# --------------------------------------------------------------------------
# Fix-ordering claim: the two-line patch (1032 + zeroed-TCB) is sufficient for
# the model to clear all PRIMARYs on both targets.
# --------------------------------------------------------------------------
def test_two_line_fix_clears_all_primaries():
    fixed = M.MapperConfig(
        irelative_define=M.R_AARCH64_IRELATIVE,  # (a) 1027 -> 1032
        applies_relro=M.TRAMPOLINE_CONFIG.applies_relro,  # RELRO unchanged (low)
        reads_jmprel=M.TRAMPOLINE_CONFIG.reads_jmprel,
        reads_relr=M.TRAMPOLINE_CONFIG.reads_relr,
        tp_is_null=False,  # (b) zeroed-TCB region instead of xzr
        has_zeroed_tcb_region=True,
        present_auxv=M.TRAMPOLINE_CONFIG.present_auxv,
    )
    for img in (M.fixture_static_etexec_relro(),
                M.fixture_static_pie_relative_irelative()):
        d = M.diagnose(img, fixed, dynamic=False)
        assert [f for f in d.findings if f.startswith("PRIMARY")] == []


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-q"]))
