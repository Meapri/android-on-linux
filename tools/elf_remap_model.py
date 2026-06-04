#!/usr/bin/env python3
# tools/elf_remap_model.py — host (device-less) verification model for the
# in-process ELF re-map mapper used by the exec-re-entry trampoline.
#
# WHY THIS EXISTS
# ===============
# The ALR exec-re-entry trampoline (app/src/main/cpp/alr_inproc_reexec.c) maps a
# guest ELF into anonymous memory and jumps to it WITHOUT execve. For a full
# static glibc binary (dpkg's zstd helper, a maintainer-script /bin/sh, ldconfig,
# chromium/GIMP static helpers) the re-mapped guest dies in its own startup
# (R12 evidence: SIGILL/SIGSEGV). The device is unavailable to this session, so
# this module is a PURE, deterministic host model of what a CORRECT re-map mapper
# must do, expressed against synthetic ELF program/section-header fixtures. It:
#
#   1. computes the single-reservation [min_vaddr, max_vaddr) span + page-rounding
#      and the per-page UNION of PT_LOAD permissions (the R11 single-span model),
#   2. identifies the PT_GNU_RELRO segment and asserts its pages END read-only,
#   3. classifies every dynamic relocation by the CORRECT AArch64 ABI constant
#      (R_AARCH64_RELATIVE=1027, R_AARCH64_IRELATIVE=1032) and flags the
#      trampoline's constant swap (it #defines IRELATIVE as 1027),
#   4. checks the auxv tag set the static glibc __libc_start_main path needs,
#   5. models the thread-pointer (TPIDR_EL0) handoff: a re-map that jumps with
#      TP=NULL (xzr) vs. a clean zeroed-TCB region (the working loader's pattern).
#
# It is NOT an emulator. It does not run guest code. It encodes the INVARIANTS a
# correct mapper must satisfy and exposes, as failing assertions, exactly where
# the current trampoline deviates from the proven-working loader
# (app/src/main/cpp/runtime_report.cpp). Fixing the model's flagged deviations in
# the C is the本체's job; this file is read-only diagnosis + a host gate.
#
# SOURCES (verified 2026-06-02):
#   * ARM AAELF64 (github.com/ARM-software/abi-aa, aaelf64): R_AARCH64_RELATIVE=1027,
#     R_AARCH64_IRELATIVE=1032, R_AARCH64_RELR=1029.
#   * glibc csu/libc-start.c __libc_start_main_impl !SHARED order (codebrowser.dev):
#     __tunables_init -> ARCH_INIT_CPU_FEATURES -> _dl_relocate_static_pie (L274)
#     -> ARCH_SETUP_IREL/apply_irel (L277) -> ARCH_SETUP_TLS/TLS_INIT_TP (L280)
#     -> _dl_setup_stack_chk_guard (L288, canary written INTO the TCB, AFTER TLS).
#   * glibc sysdeps/.../dl-early_allocate.c: __sbrk first, MMAP fallback on failure
#     (so brk-unset does NOT hard-fault on glibc >= 2.34 / Ubuntu noble 2.39).
#   * Default glibc/Ubuntu stack-protector reads the GLOBAL __stack_chk_guard
#     (adrp/ldr), not the sysreg TP-relative form (opt-in -mstack-protector-guard=sysreg).

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

# --------------------------------------------------------------------------
# AArch64 ELF ABI constants — the CANONICAL values (ARM AAELF64).
# The trampoline's bug is that it #defines R_AARCH64_IRELATIVE as 1027, which is
# actually R_AARCH64_RELATIVE. The proven-working loader (runtime_report.cpp:36)
# correctly uses 1032. These are the ground-truth values the model checks against.
# --------------------------------------------------------------------------
R_AARCH64_RELATIVE = 1027
R_AARCH64_IRELATIVE = 1032
R_AARCH64_RELR = 1029  # packed RELR relocs (DT_RELR); a static-PIE may emit these

# p_type
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_PHDR = 6
PT_TLS = 7
PT_GNU_RELRO = 0x6474E552

# p_flags
PF_X = 1
PF_W = 2
PF_R = 4

# d_tag
DT_NULL = 0
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_JMPREL = 23
DT_PLTRELSZ = 2
DT_RELR = 36
DT_RELRSZ = 35

# e_type
ET_EXEC = 2
ET_DYN = 3

# auxv tags the static glibc startup path consumes (csu/libc-start.c, _dl_aux_init)
AT_PHDR = 3
AT_PHENT = 4
AT_PHNUM = 5
AT_PAGESZ = 6
AT_BASE = 7
AT_ENTRY = 9
AT_HWCAP = 16
AT_RANDOM = 25
AT_HWCAP2 = 26

# The minimal set a re-map MUST synthesize for a static glibc binary to come up.
# AT_PHDR/PHENT/PHNUM let _dl_relocate_static_pie find its own phdrs; AT_ENTRY is
# the program entry; AT_PAGESZ sizes TLS/mmap; AT_RANDOM seeds the stack canary
# (NULL-tolerated by _dl_setup_stack_chk_guard but should be present); AT_HWCAP
# feeds IFUNC resolvers + ARCH_INIT_CPU_FEATURES. AT_BASE is required for the
# DYNAMIC (ld.so) case; for a pure static target it is 0.
REQUIRED_AUXV_STATIC = frozenset(
    {AT_PHDR, AT_PHENT, AT_PHNUM, AT_ENTRY, AT_PAGESZ, AT_RANDOM, AT_HWCAP}
)
REQUIRED_AUXV_DYNAMIC = REQUIRED_AUXV_STATIC | {AT_BASE}

PAGE = 0x1000


def _page_down(v: int, page: int = PAGE) -> int:
    return v & ~(page - 1)


def _page_up(v: int, page: int = PAGE) -> int:
    return (v + page - 1) & ~(page - 1)


# --------------------------------------------------------------------------
# Synthetic ELF fixtures (no real ELF bytes — just the headers the mapper reads).
# --------------------------------------------------------------------------
@dataclass(frozen=True)
class Phdr:
    p_type: int
    p_flags: int
    p_vaddr: int
    p_filesz: int
    p_memsz: int
    p_offset: int = 0
    p_align: int = PAGE


@dataclass(frozen=True)
class Reloc:
    r_offset: int
    r_type: int
    r_addend: int = 0


@dataclass(frozen=True)
class ElfImage:
    e_type: int
    e_entry: int
    phdrs: tuple[Phdr, ...]
    # Dynamic relocations as they would be reached via PT_DYNAMIC DT_RELA / DT_JMPREL,
    # plus any DT_RELR-packed RELATIVE relocs. The model treats these as the ground
    # truth set the mapper is supposed to apply.
    rela: tuple[Reloc, ...] = ()
    jmprel: tuple[Reloc, ...] = ()
    relr: tuple[Reloc, ...] = ()
    page: int = PAGE

    def loads(self) -> list[Phdr]:
        return [p for p in self.phdrs if p.p_type == PT_LOAD]


# --------------------------------------------------------------------------
# (1) Single-reservation span + per-page UNION permissions (R11 model).
# --------------------------------------------------------------------------
@dataclass
class PageProt:
    vaddr: int
    prot: int  # bitmask of PF_R/PF_W/PF_X (we reuse PF_* as the "final" perms)


@dataclass
class SpanModel:
    min_v: int
    max_v: int
    span: int
    pages: list[PageProt] = field(default_factory=list)
    rwx_pages: list[int] = field(default_factory=list)  # W^X violations (PF_W & PF_X)


def model_span(img: ElfImage) -> SpanModel:
    """Single anonymous reservation [min_v, max_v), per-page UNION of covering
    PT_LOAD flags — exactly what alr_inproc_reexec.c:map_elf_image does post-R11.
    Records any page that ends up both writable AND executable (W^X reject)."""
    loads = img.loads()
    if not loads:
        return SpanModel(0, 0, 0)
    min_v = min(_page_down(p.p_vaddr, img.page) for p in loads)
    max_v = max(p.p_vaddr + p.p_memsz for p in loads)
    span = _page_up(max_v - min_v, img.page)
    m = SpanModel(min_v=min_v, max_v=max_v, span=span)
    v = min_v
    while v < _page_up(max_v, img.page):
        prot = 0
        for p in loads:
            s = _page_down(p.p_vaddr, img.page)
            e = _page_up(p.p_vaddr + p.p_memsz, img.page)
            if s <= v < e:
                prot |= p.p_flags & (PF_R | PF_W | PF_X)
        m.pages.append(PageProt(vaddr=v, prot=prot))
        if (prot & PF_W) and (prot & PF_X):
            m.rwx_pages.append(v)
        v += img.page
    return m


# --------------------------------------------------------------------------
# (1b) FIX 1 — NON-PIE ET_EXEC fixed-range reservation (execve-replacement aware).
#
# A non-PIE ET_EXEC has ABSOLUTE vaddrs (canonically a 0x400000 text base) and NO
# relocations to move it: it MUST be mapped at its fixed min_v. In the live in-process
# address space the fixed range is usually FREE (Android maps the app/bionic/loader at
# high randomized addresses), so a MAP_FIXED_NOREPLACE claim succeeds without clobber.
# But the in-process re-map EMULATES execve, and execve unconditionally REPLACES the
# whole address space. The one thing that can legitimately occupy an ET_EXEC's fixed
# low vaddr is a PREVIOUS non-PIE guest WE mapped earlier in this very process — the
# root cause of the verdict-readback bug: dpkg (non-PIE @ 0x400000) fork+execs
# dpkg-query (ALSO non-PIE @ 0x400000); the child still holds dpkg there, so the old
# NOREPLACE-then-bail returned "prog map fail" (exit 73 → empty dpkg-query stdout →
# every `dpkg --status` verdict read FALSE). The fix: when the fixed range is occupied
# by a stale prior image, EVICT it with MAP_FIXED (atomic unmap+map, exactly as execve
# would) — but ONLY after proving the range holds NONE of our live execution state
# (the trampoline .text / the guest stack we run on, both at HIGH addresses); if it
# ever did, bail (a real, diagnosable wall) rather than corrupt.

# Outcomes of the ET_EXEC fixed-range claim (mirror the C diag tokens).
EXEC_FIXED_FREE = "free"          # NOREPLACE succeeded — range was free, no clobber
EXEC_FIXED_REPLACE = "replace"    # range held a stale prior guest — MAP_FIXED evicted it
EXEC_FIXED_HITS_LIVE = "hits_live"  # range overlaps our live PC/SP — bail (never corrupt)


def model_etexec_fixed_claim(
    img: ElfImage,
    occupied_ranges: tuple[tuple[int, int], ...] = (),
    live_sp: int | None = None,
    live_text: int | None = None,
) -> str:
    """Model alr_inproc_reexec.c:map_elf_image's ET_EXEC reservation decision for a
    NON-PIE image, given which [lo,hi) ranges are already mapped in the process and
    where our live SP / trampoline .text sit.

    Returns one of EXEC_FIXED_FREE / EXEC_FIXED_REPLACE / EXEC_FIXED_HITS_LIVE. Only
    meaningful for ET_EXEC (an ET_DYN image relocates to a kernel-chosen base and never
    takes this path)."""
    assert img.e_type == ET_EXEC, "fixed-range claim only applies to non-PIE ET_EXEC"
    span = model_span(img)
    lo, hi = span.min_v, span.min_v + span.span
    overlaps = any(not (r_hi <= lo or r_lo >= hi) for (r_lo, r_hi) in occupied_ranges)
    if not overlaps:
        return EXEC_FIXED_FREE  # MAP_FIXED_NOREPLACE claims the free range
    # Occupied. A real execve must replace it — UNLESS our live execution state is in it.
    if (live_sp is not None and lo <= live_sp < hi) or (
        live_text is not None and lo <= live_text < hi
    ):
        return EXEC_FIXED_HITS_LIVE
    return EXEC_FIXED_REPLACE  # evict the stale prior guest with MAP_FIXED (execve-like)


# --------------------------------------------------------------------------
# (2) PT_GNU_RELRO identification + "must end RO" invariant.
# --------------------------------------------------------------------------
@dataclass
class RelroModel:
    present: bool
    pages: list[int] = field(default_factory=list)  # page vaddrs the RELRO covers
    # After the mapper runs, is each RELRO page read-only (no PF_W)?  The current
    # trampoline NEVER mprotects RELRO RO, so model_relro(applies_relro=False) leaves
    # them writable -> not_ro_pages non-empty. With the fix, applies_relro=True.
    not_ro_pages: list[int] = field(default_factory=list)


def model_relro(img: ElfImage, span: SpanModel, applies_relro: bool) -> RelroModel:
    """Identify the PT_GNU_RELRO range and check whether its pages are finally RO.
    `applies_relro` models whether the mapper has the RELRO mprotect(RO) step.
    Note (severity): for a NON-PIE static ET_EXEC glibc, _dl_relocate_static_pie /
    _dl_protect_relro are not invoked, so a missing RELRO mprotect is a HARDENING
    gap, NOT a startup-correctness fault. The model reports it but the test suite
    classifies it as low-severity (does not by itself crash startup)."""
    relro = [p for p in img.phdrs if p.p_type == PT_GNU_RELRO]
    if not relro:
        return RelroModel(present=False)
    rm = RelroModel(present=True)
    for seg in relro:
        s = _page_down(seg.p_vaddr, img.page)
        e = _page_up(seg.p_vaddr + seg.p_memsz, img.page)
        v = s
        while v < e:
            rm.pages.append(v)
            # Find this page's final union prot from the span model.
            pp = next((x for x in span.pages if x.vaddr == v), None)
            final_w = bool(pp and (pp.prot & PF_W))
            # The mapper leaves the page writable unless it explicitly re-mprotects
            # the RELRO range RO after applying relocations.
            if final_w and not applies_relro:
                rm.not_ro_pages.append(v)
            v += img.page
    return rm


# --------------------------------------------------------------------------
# (3) Relocation classification + application-order model.
# --------------------------------------------------------------------------
@dataclass
class RelocModel:
    n_relative: int = 0
    n_irelative: int = 0
    n_relr: int = 0
    # What the CURRENT trampoline (IRELATIVE #defined as 1027) actually does:
    #   - treats every reloc whose type == 1027 as "IRELATIVE" and CALLS
    #     base+addend as an ifunc resolver — but 1027 is really RELATIVE, whose
    #     addend is a DATA bias, not a function. Calling it = jump into data.
    #   - skips real IRELATIVE (1032) entirely -> unresolved ifunc slot.
    misclassified_relative_called_as_ifunc: int = 0  # RELATIVE wrongly invoked
    irelative_skipped: int = 0  # real IRELATIVE left unresolved
    # Reads DT_JMPREL / .rela.plt (PLT relocs) at all?  The trampoline only reads
    # DT_RELA; static-PIE / DT_JMPREL-only ifuncs are missed.
    reads_jmprel: bool = False
    # Handles DT_RELR packed RELATIVE relocs?  Neither mapper does today.
    reads_relr: bool = False


def model_reloc_truth(img: ElfImage) -> RelocModel:
    """Ground-truth relocation inventory (correct ABI constants)."""
    m = RelocModel(reads_jmprel=True, reads_relr=True)
    for r in img.rela + img.jmprel:
        if r.r_type == R_AARCH64_RELATIVE:
            m.n_relative += 1
        elif r.r_type == R_AARCH64_IRELATIVE:
            m.n_irelative += 1
    for r in img.relr:
        if r.r_type == R_AARCH64_RELATIVE:
            m.n_relr += 1
    return m


def model_reloc_trampoline(img: ElfImage, irelative_define: int) -> RelocModel:
    """Model the trampoline's reloc loop given the constant it uses for what it
    THINKS is IRELATIVE. With the bug, irelative_define == 1027 (== RELATIVE).
    It only scans DT_RELA (no DT_JMPREL, no DT_RELR)."""
    m = RelocModel(reads_jmprel=False, reads_relr=False)
    for r in img.rela:  # only DT_RELA is scanned by the trampoline
        if r.r_type == irelative_define:
            # The loop CALLS base+addend as a resolver for everything matching the
            # (mis)define. If the define is 1027, real RELATIVE relocs get called.
            if r.r_type == R_AARCH64_RELATIVE and irelative_define == R_AARCH64_RELATIVE:
                m.misclassified_relative_called_as_ifunc += 1
            m.n_irelative += 1  # what the trampoline counts as "irel"
    # Whatever real IRELATIVE relocs exist but don't match the (mis)define are skipped.
    for r in img.rela:
        if r.r_type == R_AARCH64_IRELATIVE and irelative_define != R_AARCH64_IRELATIVE:
            m.irelative_skipped += 1
    return m


# --------------------------------------------------------------------------
# (4) auxv completeness.
# --------------------------------------------------------------------------
def missing_auxv(present_tags: set[int], dynamic: bool) -> set[int]:
    required = REQUIRED_AUXV_DYNAMIC if dynamic else REQUIRED_AUXV_STATIC
    return set(required) - set(present_tags)


# --------------------------------------------------------------------------
# (5) Thread-pointer (TPIDR_EL0) handoff model — the S1 primary.
# --------------------------------------------------------------------------
@dataclass
class TpModel:
    tp_is_null: bool  # trampoline: msr tpidr_el0, xzr  -> True
    has_zeroed_tcb_region: bool  # working loader: mmap(16384)+8192 -> True
    # Does any code run with TP at the (foreign/NULL) entry value before glibc's
    # ARCH_SETUP_TLS installs its own TP?  YES on aarch64: __tunables_init,
    # _dl_relocate_static_pie, ARCH_SETUP_IREL all run BEFORE ARCH_SETUP_TLS
    # (csu/libc-start.c L267..L280). Any of those that touch the thread pointer
    # (errno, __libc_tsd, a TP-relative canary on a sysreg-guard build) fault if
    # TP==NULL. The working loader hands a zeroed, mapped 16 KiB TCB region so a
    # ±offset TP load lands on mapped-zero rather than NULL.
    code_runs_before_tls_init: bool = True


def model_tp(tp_is_null: bool, has_zeroed_tcb_region: bool) -> TpModel:
    return TpModel(tp_is_null=tp_is_null, has_zeroed_tcb_region=has_zeroed_tcb_region)


def tp_fault_risk(tp: TpModel) -> bool:
    """A re-map that jumps with TP==NULL and provides no zeroed TCB region risks a
    fault on any TP-relative access during the pre-TLS-init startup window. The
    working loader avoids it by giving a mapped zeroed 16 KiB region centered on TP.
    Returns True if the configuration is the risky one (trampoline) and False if it
    matches the proven-working loader."""
    if not tp.tp_is_null and tp.has_zeroed_tcb_region:
        return False  # working loader: TP -> middle of a mapped zeroed region
    if tp.tp_is_null and not tp.has_zeroed_tcb_region:
        return True  # trampoline: TP == NULL, nothing mapped near 0
    # Mixed/odd configs: risky unless a zeroed region backs the TP.
    return not tp.has_zeroed_tcb_region


# --------------------------------------------------------------------------
# Top-level diagnosis aggregator — one call that surfaces every deviation of a
# given mapper configuration from the correct invariants, for a given target ELF.
# --------------------------------------------------------------------------
@dataclass
class MapperConfig:
    irelative_define: int  # the constant the mapper uses for IRELATIVE
    applies_relro: bool  # does it mprotect PT_GNU_RELRO RO after relocs?
    reads_jmprel: bool  # does it scan DT_JMPREL / .rela.plt?
    reads_relr: bool  # does it apply DT_RELR packed RELATIVE relocs?
    tp_is_null: bool  # msr tpidr_el0, xzr (True) vs zeroed-TCB region (False)
    has_zeroed_tcb_region: bool
    present_auxv: frozenset[int]


# The current trampoline (alr_inproc_reexec.c) as of v150/base.
TRAMPOLINE_CONFIG = MapperConfig(
    irelative_define=1027,  # BUG: 1027 is R_AARCH64_RELATIVE; IRELATIVE is 1032
    applies_relro=False,  # no PT_GNU_RELRO handling
    reads_jmprel=False,  # only DT_RELA
    reads_relr=False,  # no DT_RELR
    tp_is_null=True,  # enter_guest: msr tpidr_el0, xzr
    has_zeroed_tcb_region=False,  # no mmap'd zeroed TCB
    present_auxv=frozenset(
        {AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY, AT_HWCAP,
         AT_HWCAP2, AT_RANDOM}
    ),
)

# The proven-working in-process loader (runtime_report.cpp) — the reference.
WORKING_LOADER_CONFIG = MapperConfig(
    irelative_define=1032,  # correct
    applies_relro=False,  # also doesn't (hardening gap, not a crash) — matched
    reads_jmprel=False,  # uses SHT_RELA fallback instead for static (see notes)
    reads_relr=False,
    tp_is_null=False,  # alr_enter_guest: msr tpidr_el0, x2 (x2 = zeroed TCB)
    has_zeroed_tcb_region=True,  # mmap(16384)+8192
    present_auxv=frozenset(
        {AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_BASE, AT_ENTRY, AT_HWCAP,
         AT_HWCAP2, AT_RANDOM}
    ),
)


@dataclass
class Diagnosis:
    span: SpanModel
    relro: RelroModel
    reloc_truth: RelocModel
    reloc_actual: RelocModel
    missing_auxv: set[int]
    tp: TpModel
    tp_risky: bool
    # Severity-ranked findings (str), highest first.
    findings: list[str] = field(default_factory=list)


def diagnose(img: ElfImage, cfg: MapperConfig, dynamic: bool) -> Diagnosis:
    span = model_span(img)
    relro = model_relro(img, span, applies_relro=cfg.applies_relro)
    truth = model_reloc_truth(img)
    actual = model_reloc_trampoline(img, cfg.irelative_define)
    miss = missing_auxv(set(cfg.present_auxv), dynamic=dynamic)
    tp = model_tp(cfg.tp_is_null, cfg.has_zeroed_tcb_region)
    risky = tp_fault_risk(tp)

    findings: list[str] = []

    # [PRIMARY] TP handoff (the single decisive delta vs the working loader for the
    # observed ET_EXEC /bin/sh that has IREL=0 yet still crashes after the jump).
    if risky:
        findings.append(
            "PRIMARY[TP]: jumps with TPIDR_EL0=NULL (xzr) and no zeroed-TCB region; "
            "csu code (__tunables_init/_dl_relocate_static_pie/ARCH_SETUP_IREL) runs "
            "BEFORE ARCH_SETUP_TLS installs the real TP — a TP-relative access in that "
            "window faults. Working loader hands TP=mmap(16384)+8192 (zeroed)."
        )

    # [PRIMARY/CO] IRELATIVE constant swap — active whenever the target has DT_RELA
    # relocations (static-PIE / DT_RELA-bearing). Masked for plain ET_EXEC w/ no
    # PT_DYNAMIC (IREL=0), which is why the observed /bin/sh did not trip it.
    if cfg.irelative_define != R_AARCH64_IRELATIVE:
        if actual.misclassified_relative_called_as_ifunc:
            findings.append(
                f"PRIMARY[reloc]: IRELATIVE #defined as {cfg.irelative_define} "
                f"(== R_AARCH64_RELATIVE); {actual.misclassified_relative_called_as_ifunc} "
                "RELATIVE reloc(s) get CALLED as ifunc resolvers (jump into data → "
                "SIGILL/SIGSEGV). Correct value is 1032."
            )
        if actual.irelative_skipped:
            findings.append(
                f"PRIMARY[reloc]: {actual.irelative_skipped} real R_AARCH64_IRELATIVE "
                "reloc(s) skipped (never == the wrong define) → unresolved ifunc slot "
                "→ indirect call through 0/garbage."
            )

    # [SECONDARY] missing PLT/RELR coverage (only bites static-PIE / DT_JMPREL-only).
    if not cfg.reads_jmprel and (truth.n_irelative or img.jmprel):
        findings.append(
            "SECONDARY[reloc]: mapper does not scan DT_JMPREL/.rela.plt; an ifunc "
            "carried only in the PLT relocs is unresolved (static-PIE / DT_JMPREL-only "
            "targets). Working loader falls back to SHT_RELA for the static case."
        )
    if not cfg.reads_relr and img.relr:
        findings.append(
            "SECONDARY[reloc]: DT_RELR packed RELATIVE relocs are ignored; a "
            "DT_RELR-linked static-PIE leaves those RELATIVE fixups unapplied."
        )

    # [SECONDARY] brk note — modeled as informational, NOT a hard fault on noble.
    # glibc 2.34+ _dl_early_allocate falls back to mmap when __sbrk fails, so the
    # BZ2066147 brk-unset crash does NOT reproduce on Ubuntu noble glibc 2.39.

    # auxv completeness.
    if miss:
        findings.append(
            f"AUXV: missing required tags {sorted(miss)} for the "
            f"{'dynamic' if dynamic else 'static'} startup path."
        )

    # [LOW] RELRO hardening gap (NOT a startup crash for non-PIE ET_EXEC).
    if relro.present and relro.not_ro_pages:
        findings.append(
            f"LOW[relro]: PT_GNU_RELRO present but {len(relro.not_ro_pages)} page(s) "
            "stay writable (no mprotect RO). Hardening gap only; non-PIE ET_EXEC never "
            "calls _dl_protect_relro, so this does not by itself crash startup."
        )

    return Diagnosis(
        span=span,
        relro=relro,
        reloc_truth=truth,
        reloc_actual=actual,
        missing_auxv=miss,
        tp=tp,
        tp_risky=risky,
        findings=findings,
    )


# --------------------------------------------------------------------------
# Canonical fixtures used by the tests + any device-prep tooling.
# --------------------------------------------------------------------------
def fixture_static_etexec_relro() -> ElfImage:
    """A plausible full static glibc /bin/sh: ET_EXEC, fixed vaddr 0x400000, RX
    text + RW data + a PT_GNU_RELRO covering the RW head, NO PT_DYNAMIC (IREL=0).
    This mirrors the device-observed target (entry=0x400640, IREL=0x0) that still
    SIGILLs after the jump — so for THIS fixture the only live defect is TP."""
    base = 0x400000
    return ElfImage(
        e_type=ET_EXEC,
        e_entry=base + 0x640,
        phdrs=(
            Phdr(PT_PHDR, PF_R, base + 0x40, 0x1F8, 0x1F8, 0x40),
            Phdr(PT_LOAD, PF_R | PF_X, base, 0x9081, 0x9081, 0x0),  # RX text
            Phdr(PT_LOAD, PF_R | PF_W, base + 0xF000, 0x1100, 0x10A8 + 0x1000, 0xF000),  # RW data+bss
            Phdr(PT_GNU_RELRO, PF_R, base + 0xF000, 0x800, 0x800, 0xF000),  # RELRO head of data
        ),
        rela=(),  # no PT_DYNAMIC → no DT_RELA → IREL=0 (matches device)
    )


def fixture_static_pie_relative_irelative() -> ElfImage:
    """A static-PIE (ET_DYN, no PT_INTERP) carrying both R_AARCH64_RELATIVE and
    R_AARCH64_IRELATIVE in DT_RELA — the case where the constant swap turns active:
    the trampoline calls the RELATIVE relocs as ifuncs and skips the real IRELATIVE."""
    return ElfImage(
        e_type=ET_DYN,
        e_entry=0x1640,
        phdrs=(
            Phdr(PT_PHDR, PF_R, 0x40, 0x230, 0x230, 0x40),
            Phdr(PT_LOAD, PF_R | PF_X, 0x0, 0x9000, 0x9000, 0x0),
            Phdr(PT_LOAD, PF_R | PF_W, 0xF000, 0x1200, 0x2000, 0xF000),
            Phdr(PT_DYNAMIC, PF_R | PF_W, 0xF100, 0x100, 0x100, 0xF100),
            Phdr(PT_GNU_RELRO, PF_R, 0xF000, 0x400, 0x400, 0xF000),
        ),
        rela=(
            Reloc(0xF800, R_AARCH64_RELATIVE, 0x1234),  # data bias — must NOT be called
            Reloc(0xF808, R_AARCH64_RELATIVE, 0x5678),
            Reloc(0xF810, R_AARCH64_IRELATIVE, 0x1500),  # real ifunc resolver
        ),
    )


# --------------------------------------------------------------------------
# Shebang (#!) re-target model — the in-process binfmt_script emulation.
#
# WHY: the in-process re-map trampoline (alr_inproc_reexec.c) used to bail on any
# non-ELF target (sys_exit(72) on the ELF-magic check), so a guest that exec()s a
# `#!`-interpreter SCRIPT died. The load-bearing case is authenticated `apt-get
# update`: noble apt's gpgv method exec()s /usr/bin/apt-key — a `#!/bin/sh` script
# — to verify the InRelease, so apt-key never ran ("Unknown error executing
# apt-key" → "The repository is not signed"). The trampoline now emulates the
# kernel's fs/binfmt_script: parse `#!INTERP [ARG]`, map the rootfs-resolved
# INTERPRETER ELF, and rebuild argv = [interp, optarg?, script, orig argv[1..]].
#
# This is a PURE host model of that parse + re-target (it does NOT run guest code),
# mirroring the C in alr_inproc_reexec.c:parse_shebang + the worker's interp-path
# resolve + argv splice, so the decision is host-regression-guarded device-lessly.
# --------------------------------------------------------------------------
SHEBANG_LINE_CAP = 256  # mirrors the C cap (kernel BINPRM_BUF_SIZE-1 = 255)


def parse_shebang_line(first_bytes: bytes) -> tuple[str, str] | None:
    """Parse a `#!` first line exactly as alr_inproc_reexec.c:parse_shebang (which
    mirrors fs/binfmt_script.c + alr_exec.cpp:parse_shebang): skip "#!", trim
    leading/trailing whitespace, the first whitespace-delimited token is the
    interpreter, and the WHOLE (trimmed) remainder is a SINGLE argument (the kernel
    does not word-split). Returns (interp, arg) — arg "" if none — or None if there
    is no usable interpreter (`#!` only, or not a shebang). Honours the 255-byte cap.
    """
    if len(first_bytes) < 2 or first_bytes[0:2] != b"#!":
        return None
    # Bound to the first newline OR the kernel's 255-byte line cap (matches the C).
    line = first_bytes[2 : 2 + (SHEBANG_LINE_CAP - 1)]
    nl = min([i for i in (line.find(b"\n"), line.find(b"\r")) if i >= 0], default=-1)
    if nl >= 0:
        line = line[:nl]
    text = line.decode("utf-8", "replace")
    text = text.lstrip(" \t")
    if not text:
        return None
    sp = next((i for i, c in enumerate(text) if c in " \t"), -1)
    if sp < 0:
        interp, arg = text, ""
    else:
        interp = text[:sp]
        arg = text[sp:].lstrip(" \t").rstrip(" \t")
    interp = interp[: SHEBANG_LINE_CAP - 1]
    arg = arg[: SHEBANG_LINE_CAP - 1]
    if not interp:
        return None
    return interp, arg


@dataclass
class ShebangRetarget:
    is_shebang: bool
    interp_guest: str = ""          # the `#!`-named interpreter (guest path, e.g. /bin/sh)
    interp_host: str = ""           # <rootfs><interp> — the ELF the trampoline maps
    argv: tuple[str, ...] = ()      # rebuilt argv for the interpreter
    error: str = ""                 # non-empty => the trampoline would sys_exit (EX_*)


def model_shebang_retarget(
    target_host: str,
    first_bytes: bytes,
    orig_argv: tuple[str, ...],
    rootfs: str,
) -> ShebangRetarget:
    """Model the trampoline's shebang re-target for a target whose mapped file
    begins `first_bytes`. `target_host` is the script's already-rootfs-resolved host
    path (the supervisor seeded it in x19); `orig_argv` is the guest's original argv
    (x20); `rootfs` is x22. Mirrors the C exactly:

      * non-`#!` -> is_shebang False (the worker falls through to the ELF path).
      * `#!` with no interpreter -> error (the C sys_exit(EX_ELF_TARGET)=72).
      * else: interp_host = <rootfs><interp>; argv = [interp, optarg?, target_host,
        orig_argv[1..]] (orig argv[0] dropped, exactly like the kernel). The script
        slot is the HOST path so the interpreter's open() needs no further mediation.
    """
    parsed = parse_shebang_line(first_bytes)
    if parsed is None:
        # Either not a shebang (ELF/other) or a malformed "#!". Distinguish: a real
        # ELF is the normal fall-through; a "#!"-prefixed-but-unparsable is the error.
        if len(first_bytes) >= 2 and first_bytes[0:2] == b"#!":
            return ShebangRetarget(is_shebang=True, error="malformed shebang")
        return ShebangRetarget(is_shebang=False)
    interp, arg = parsed
    interp_host = rootfs.rstrip("/") + interp if interp.startswith("/") else rootfs + "/" + interp
    new_argv: list[str] = [interp]
    if arg:
        new_argv.append(arg)
    new_argv.append(target_host)
    new_argv.extend(orig_argv[1:])  # drop the guest's argv[0], like binfmt_script
    return ShebangRetarget(
        is_shebang=True,
        interp_guest=interp,
        interp_host=interp_host,
        argv=tuple(new_argv),
    )


# ---------------------------------------------------------------------------
# execve FD_CLOEXEC emulation — the in-process re-map's fd-table fix.
#
# WHY: a real execve() closes every fd whose FD_CLOEXEC bit is set, at the exec
# boundary; the surviving fds become the new image's table. The in-process re-map
# (alr_inproc_reexec.c) does NO execve, so without an explicit sweep every
# CLOEXEC-marked fd the caller meant to drop stays open in the re-mapped guest.
# Two device-root-caused failures collapse to exactly this gap:
#
#   * authenticated `apt-get update`: apt's ExecGPGV/ExecFork mark all fds >= 3
#     FD_CLOEXEC (close_range(3,~0U,CLOSE_RANGE_CLOEXEC)) then dup2 the gpgv status
#     pipe's WRITE end onto fd 3 (--status-fd 3) and clear CLOEXEC on that one only.
#     After a real exec, fd 3 is the SOLE remaining write end, so when gpgv finishes
#     the pipe EOFs and apt parses GOODSIG/VALIDSIG. Under the no-execve re-map the
#     pre-dup write end (still CLOEXEC) lingers as a SECOND write end → the status
#     pipe never EOFs → the GOODSIG is never finalized → "not signed".
#   * ROOTFUL Xwayland: the X server fork()+execs /usr/bin/xkbcomp, marking its
#     sockets/fds CLOEXEC so xkbcomp does not inherit them and reading the compiled
#     keymap back through a pipe. Leaked CLOEXEC fds keep the keymap pipe's write
#     end open → the read never EOFs → "XKB: Failed to compile keymap".
#
# The model mirrors the C (close_cloexec_fds): close iff FD_CLOEXEC is set; an fd
# the caller deliberately left inheritable (NOT CLOEXEC: fd 3, stdio, the dup2'd
# status fd) is kept — exactly what the kernel does on execve.

FD_CLOEXEC = 1  # POSIX close-on-exec bit (matches the C #define)


def fd_should_close_on_exec(fd_flags: int) -> bool:
    """True iff a real execve would close this fd — i.e. FD_CLOEXEC is set. The
    in-process re-map must reproduce this for every open fd. fd NUMBER is
    irrelevant (the kernel checks the flag, not the number); stdio survives only
    because the caller does not mark it CLOEXEC, never because we special-case it.
    """
    return bool(fd_flags & FD_CLOEXEC)


def close_cloexec_fds_model(fd_table: dict[int, int]) -> tuple[dict[int, int], set[int]]:
    """Model alr_inproc_reexec.c:close_cloexec_fds over an fd->flags table.

    Returns (surviving_table, closed_fds): every fd whose flags carry FD_CLOEXEC is
    removed (closed), the rest are retained verbatim — the post-execve fd table the
    re-mapped guest must see. Deterministic and order-independent (the C closes each
    independently; closing one never resurrects another).
    """
    surviving: dict[int, int] = {}
    closed: set[int] = set()
    for fd, flags in fd_table.items():
        if fd_should_close_on_exec(flags):
            closed.add(fd)
        else:
            surviving[fd] = flags
    return surviving, closed


def apt_status_fd_survives_remap(fd_table: dict[int, int], status_fd: int = 3) -> bool:
    """The load-bearing apt invariant: after the CLOEXEC sweep, the status fd (3)
    that apt dup2'd (CLOEXEC-cleared) must REMAIN open AND no OTHER write end of the
    same status pipe may remain. Modeled as: status_fd survives, and every CLOEXEC
    fd (the leaked pre-dup write ends) is closed. Given a table where fd 3 is the
    dup2'd end (flags 0) and some higher fd is the original write end (FD_CLOEXEC),
    this returns True only once the sweep is applied — the regression guard for the
    'repository is not signed' device failure.
    """
    surviving, closed = close_cloexec_fds_model(fd_table)
    return status_fd in surviving and all(
        fd in closed for fd, flags in fd_table.items() if (flags & FD_CLOEXEC)
    )


def inheritable_status_fds_after_remap(fd_table: dict[int, int]) -> int:
    """Mirror of alr_inproc_reexec.c:audit_status_fd's survivor count: the number
    of OPEN, non-CLOEXEC fds with number >= 3 that REMAIN after the CLOEXEC sweep.

    These are exactly the fds a re-mapped gpgv inherits as candidate write ends of
    the apt status pipe. The load-bearing invariant for authenticated apt is that
    this count is EXACTLY 1 (only the status fd itself — the dup2'd, CLOEXEC-cleared
    status write end). A count of 0 means the status fd was wrongly closed (gpgv's
    `--status-fd N` write EBADFs -> no GOODSIG); > 1 means a stray inheritable fd
    lingered besides it (the status pipe never EOFs -> apt reports "not signed"). The
    C audit is a device-drain diagnostic; this models the same number for a host test.
    """
    surviving, _closed = close_cloexec_fds_model(fd_table)
    return sum(1 for fd, flags in surviving.items()
               if fd >= 3 and not (flags & FD_CLOEXEC))


# ---------------------------------------------------------------------------
# FIX 2 — apt/gpgv `--status-fd N` parse + protect (the fd-3 survival fix).
#
# WHY: apt does NOT hard-wire fd 3 for the gpgv status pipe — it uses the pipe's own
# fd number and passes it as `--status-fd N` (or `--status-fd=N`). A real kernel-
# exec'd gpgv inherits fd N because apt cleared CLOEXEC on it before the exec. Under
# the no-execve in-process re-map, if any process in the deep apt → apt-key(sh) →
# gpgv chain leaves fd N CLOEXEC (or a leaked supervisor capture-pipe fd perturbs the
# numbering so the status pipe lands above the native fd 3), the CLOEXEC sweep would
# WRONGLY drop it → gpgv's `--status-fd N` write EBADFs → no GOODSIG → "the
# repository is not signed". The trampoline therefore (1) parses the REAL N from the
# guest argv and (2) clears CLOEXEC on fd N before the sweep, replicating apt's own
# pre-exec clear. These mirror alr_inproc_reexec.c:find_status_fd / protect_status_fd.


def find_status_fd_model(argv: list[str]) -> int:
    """Parse `--status-fd N` / `--status-fd=N` from a guest argv exactly as
    alr_inproc_reexec.c:find_status_fd. Returns the fd number, or -1 if absent or the
    value is missing/non-numeric. Only the FIRST occurrence is honoured (gpgv passes
    it once). A bounded, allocation-free scan in the C; a list scan here."""
    for i, a in enumerate(argv):
        if a == "--status-fd":
            if i + 1 < len(argv):
                v = argv[i + 1]
                if v.isdigit():
                    return int(v)
            return -1
        if a.startswith("--status-fd="):
            v = a[len("--status-fd="):]
            if v.isdigit():
                return int(v)
            return -1
    return -1


def protect_status_fd_model(
    fd_table: dict[int, int], status_fd: int
) -> dict[int, int]:
    """Model alr_inproc_reexec.c:protect_status_fd. If `status_fd` is named (>= 0) AND
    open in the table, clear its FD_CLOEXEC bit so the subsequent CLOEXEC sweep KEEPS
    it (replicating apt's pre-exec clear). Returns a NEW table (the input is not
    mutated). A no-op when status_fd is -1, not open, or already non-CLOEXEC — and it
    NEVER touches any other fd (no blind whitelist; only the guest-named status fd)."""
    if status_fd < 0 or status_fd not in fd_table:
        return dict(fd_table)
    out = dict(fd_table)
    out[status_fd] = out[status_fd] & ~FD_CLOEXEC
    return out
