"""Host model + unit tests for the PC-gated seccomp path filter.

This file does NOT execute any seccomp BPF (the host is x86_64/arm64 macOS and
the filter targets an on-device aarch64 guest). Instead it models the *decision*
the interposer's PC-gated BPF program makes, exactly as specified by the shared
contract in docs/design/pcgate-seccomp.md, and asserts the soundness properties
that make the loader/interposer filter stacking safe.

Contract recap (PCGATE == 1, the fast path):
  The interposer installs a filter whose per-syscall decision is:
    if  instruction_pointer in [alr_tramp_lo, alr_tramp_hi)   -> ALLOW
    elif nr in PATH_SYSCALLS                                  -> TRACE
    else                                                      -> ALLOW

  i.e. the ONLY way a path syscall is allowed-without-tracing is to be emitted
  from the interposer's unique trampoline PC (which is only reachable *after*
  the path has already been rewritten in-process). Every other path syscall
  falls to RET_TRACE and is handled by the loader's ptrace supervisor.

Soundness properties asserted here (these are the load-bearing invariants):
  S1  An un-gated path syscall (IP outside the trampoline range) is ALWAYS
      TRACE — never silently ALLOW. The supervisor remains the safety net.
  S2  A non-path syscall is ALLOW regardless of IP (the gate does not add
      tracing the loader's reduced filter didn't ask for).
  S3  FAIL-SAFE DIRECTION: if the trampoline range is wrong/stale (e.g. the
      interposer was re-mapped after an exec re-entry so the gate window no
      longer matches the real trampoline PC), a path syscall degrades toward
      TRACE, never toward a silent ALLOW. Completeness can only get *more*
      restrictive, never less.
  S4  The ALLOW window is a half-open interval [lo, hi): lo is inside, hi is
      outside. This matches the BPF two-word [lo,hi) IP compare and prevents an
      off-by-one that would either trace the trampoline (slow but safe) or, if
      inverted, allow the byte just past it (unsafe) — we assert the safe side.
  S5  A foreign-architecture syscall (arch != AUDIT_ARCH_AARCH64) is ALLOW, as
      the loader's BPF returns RET_ALLOW for a non-matching arch before it ever
      inspects nr/IP. (Models the arch guard that precedes the gate.)

Pure-python, no external deps. Run with:  python3 -m pytest tests/ -q
"""

from pathlib import Path

import pytest


# --------------------------------------------------------------------------- #
# Constants mirrored from the contract (docs/design/pcgate-seccomp.md) and the
# loader's filter (app/src/main/cpp/runtime_report.cpp). aarch64 syscall numbers
# are stable kernel ABI, so we can hard-code them in the host model.
# --------------------------------------------------------------------------- #

AUDIT_ARCH_AARCH64 = 0xC00000B7  # <linux/audit.h>: EM_AARCH64 | __AUDIT_ARCH_64BIT | LE

# The nine path-taking syscalls the design routes through path mediation, by
# aarch64 NR. These are exactly the set the loader traces in PCGATE=0 and the
# interposer gates in PCGATE=1.
PATH_SYSCALLS = {
    56: "openat",
    437: "openat2",
    79: "newfstatat",
    291: "statx",
    48: "faccessat",
    439: "faccessat2",
    78: "readlinkat",
    34: "mkdirat",
    35: "unlinkat",
}

# A few non-path syscalls used to assert the "else -> ALLOW" arm. read/write/
# mmap/exit_group are hot and must never be traced by the PC-gate.
NON_PATH_SYSCALLS = {
    63: "read",
    64: "write",
    222: "mmap",
    94: "exit_group",
    260: "wait4",
}

# execve/execveat are NOT in the interposer's gate at all (the *loader* traces
# them in PCGATE=1 for W^X exec re-entry control). For the interposer model they
# are therefore plain non-path syscalls -> ALLOW.
EXECVE = 221
EXECVEAT = 281

ALLOW = "ALLOW"
TRACE = "TRACE"


# --------------------------------------------------------------------------- #
# The model under test: the PC-gate decision, written to match the BPF program
# the interposer constructor installs (instruction_pointer range compare, then
# nr membership). `arch` defaults to aarch64; pass a foreign arch to exercise S5.
# --------------------------------------------------------------------------- #

def pcgate_decision(nr, instruction_pointer, tramp_lo, tramp_hi,
                    arch=AUDIT_ARCH_AARCH64):
    """Return ALLOW or TRACE for one trapped syscall under the PCGATE=1 filter.

    Mirrors the contract precisely:
      arch guard first (foreign arch -> ALLOW), then
      IP in [tramp_lo, tramp_hi) -> ALLOW (trusted trampoline), then
      nr in PATH_SYSCALLS        -> TRACE, else -> ALLOW.
    The interval is HALF-OPEN: lo <= ip < hi.
    """
    if arch != AUDIT_ARCH_AARCH64:
        return ALLOW
    if tramp_lo <= instruction_pointer < tramp_hi:
        return ALLOW
    if nr in PATH_SYSCALLS:
        return TRACE
    return ALLOW


# A representative trampoline window. Values are arbitrary but fixed; the model
# is address-agnostic (only the [lo,hi) relation matters).
TRAMP_LO = 0x7F_AB00_1000
TRAMP_HI = 0x7F_AB00_1040  # 64-byte trampoline, say


# --------------------------------------------------------------------------- #
# S1: an un-gated path syscall is ALWAYS TRACE.
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("nr", sorted(PATH_SYSCALLS))
def test_s1_ungated_path_syscall_always_traces(nr):
    # IP well outside the trampoline (a normal libc/app call site).
    app_ip = 0x5500_0000_0000
    assert app_ip < TRAMP_LO or app_ip >= TRAMP_HI  # truly outside
    assert pcgate_decision(nr, app_ip, TRAMP_LO, TRAMP_HI) == TRACE


def test_s1_ungated_path_syscall_just_below_window_traces():
    # The byte immediately *below* the window is un-gated -> TRACE.
    assert pcgate_decision(56, TRAMP_LO - 1, TRAMP_LO, TRAMP_HI) == TRACE


# --------------------------------------------------------------------------- #
# S2: a non-path syscall is ALLOW regardless of IP.
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("nr", sorted(NON_PATH_SYSCALLS))
def test_s2_non_path_syscall_allows_outside_window(nr):
    app_ip = 0x5500_0000_0000
    assert pcgate_decision(nr, app_ip, TRAMP_LO, TRAMP_HI) == ALLOW


@pytest.mark.parametrize("nr", sorted(NON_PATH_SYSCALLS))
def test_s2_non_path_syscall_allows_inside_window(nr):
    inside = TRAMP_LO + 8
    assert pcgate_decision(nr, inside, TRAMP_LO, TRAMP_HI) == ALLOW


def test_s2_execve_family_not_gated_by_interposer():
    # The interposer does not trace execve/execveat (the loader does). From the
    # interposer's filter they are non-path -> ALLOW.
    app_ip = 0x5500_0000_0000
    assert pcgate_decision(EXECVE, app_ip, TRAMP_LO, TRAMP_HI) == ALLOW
    assert pcgate_decision(EXECVEAT, app_ip, TRAMP_LO, TRAMP_HI) == ALLOW


# --------------------------------------------------------------------------- #
# S3: FAIL-SAFE DIRECTION — a wrong/stale trampoline range can only make a path
# syscall *more* traced, never silently allowed.
# --------------------------------------------------------------------------- #

def test_s3_stale_window_degrades_path_syscall_to_trace():
    # Real trampoline PC, but the gate window is stale (points elsewhere, e.g.
    # the pre-exec mapping). The path syscall from the *real* (now un-gated) PC
    # falls through to TRACE — slower, but never a silent ALLOW.
    real_tramp_pc = 0x7F_CCCC_2000  # where the interposer actually re-mapped
    stale_lo, stale_hi = TRAMP_LO, TRAMP_HI  # window from the old mapping
    assert not (stale_lo <= real_tramp_pc < stale_hi)  # gate misses it
    assert pcgate_decision(56, real_tramp_pc, stale_lo, stale_hi) == TRACE


def test_s3_empty_window_traces_every_path_syscall():
    # Degenerate gate (lo == hi, empty interval): nothing is trusted, so every
    # path syscall traces. This is the maximally-safe degradation and must hold.
    lo = hi = 0x7F_AB00_1000
    for nr in PATH_SYSCALLS:
        assert pcgate_decision(nr, lo, lo, hi) == TRACE
        assert pcgate_decision(nr, lo - 1, lo, hi) == TRACE
        assert pcgate_decision(nr, lo + 1, lo, hi) == TRACE


def test_s3_no_input_yields_silent_allow_for_path_syscall():
    # Exhaustive direction check: across a grid of (window, ip) configurations,
    # a path syscall is NEVER ALLOW unless the ip is genuinely inside the window.
    # If the window is wrong, the only possible outcome is TRACE. This is the
    # core soundness theorem stated operationally.
    windows = [
        (0x1000, 0x1040),
        (0x2000, 0x2000),          # empty
        (0x4000, 0x4001),          # one byte
        (0xFFFF_0000, 0xFFFF_0100),
    ]
    ips = [0x0, 0xFFF, 0x1000, 0x103F, 0x1040, 0x2000, 0x4000, 0x4001,
           0xFFFF_00FF, 0xFFFF_0100, 0xDEAD_BEEF]
    for lo, hi in windows:
        for ip in ips:
            d = pcgate_decision(56, ip, lo, hi)  # openat, a path syscall
            if d == ALLOW:
                # The only justification for ALLOW on a path syscall is a true
                # in-window IP.
                assert lo <= ip < hi, (
                    f"silent ALLOW for path syscall at ip={ip:#x} "
                    f"outside window [{lo:#x},{hi:#x})"
                )
            else:
                assert d == TRACE


# --------------------------------------------------------------------------- #
# S4: half-open interval [lo, hi) — lo inside, hi outside.
# --------------------------------------------------------------------------- #

def test_s4_window_is_half_open_lo_inside_hi_outside():
    # lo is the first trusted byte -> a path syscall there is ALLOW.
    assert pcgate_decision(56, TRAMP_LO, TRAMP_LO, TRAMP_HI) == ALLOW
    # hi is the first byte PAST the trampoline -> NOT trusted -> path TRACE.
    assert pcgate_decision(56, TRAMP_HI, TRAMP_LO, TRAMP_HI) == TRACE
    # last trusted byte (hi-1) -> ALLOW.
    assert pcgate_decision(56, TRAMP_HI - 1, TRAMP_LO, TRAMP_HI) == ALLOW


def test_s4_trampoline_pc_allows_path_syscall():
    # The whole point: a path syscall emitted FROM the trampoline (post-rewrite)
    # is allowed without a ptrace round-trip.
    mid = (TRAMP_LO + TRAMP_HI) // 2
    for nr in PATH_SYSCALLS:
        assert pcgate_decision(nr, mid, TRAMP_LO, TRAMP_HI) == ALLOW


# --------------------------------------------------------------------------- #
# S5: arch guard precedes the gate — foreign arch always ALLOW.
# --------------------------------------------------------------------------- #

def test_s5_foreign_arch_always_allows():
    foreign = 0x4000_0028  # AUDIT_ARCH_ARM (32-bit) — not our guest arch
    # Even a path syscall from a non-trampoline IP is ALLOW under a foreign arch,
    # because the BPF arch guard returns RET_ALLOW before inspecting nr/IP.
    app_ip = 0x5500_0000_0000
    assert pcgate_decision(56, app_ip, TRAMP_LO, TRAMP_HI, arch=foreign) == ALLOW
    # And aarch64 with the same inputs would TRACE — confirming the guard is what
    # changed the outcome.
    assert pcgate_decision(56, app_ip, TRAMP_LO, TRAMP_HI,
                           arch=AUDIT_ARCH_AARCH64) == TRACE


# --------------------------------------------------------------------------- #
# Cross-check against the loader's PCGATE=0 baseline behavior. In PCGATE=0 the
# LOADER traces all path syscalls regardless of IP (no gate). We model that and
# assert the A/B difference the design predicts: PCGATE=1 collapses traces to
# (at most) the un-gated path syscalls, while PCGATE=0 traces them all.
# --------------------------------------------------------------------------- #

def loader_baseline_decision(nr, arch=AUDIT_ARCH_AARCH64):
    """PCGATE=0: loader's full 9-path-syscall TRACE filter, IP-agnostic."""
    if arch != AUDIT_ARCH_AARCH64:
        return ALLOW
    return TRACE if nr in PATH_SYSCALLS else ALLOW


def test_ab_pcgate1_traces_strictly_fewer_path_syscalls_than_baseline():
    # Model a workload: N path syscalls, of which `gated` are emitted from the
    # trampoline (post-rewrite, the common case) and the rest from un-gated PCs.
    mid = (TRAMP_LO + TRAMP_HI) // 2
    app_ip = 0x5500_0000_0000

    baseline_traces = 0
    pcgate_traces = 0
    total = 0
    for nr in PATH_SYSCALLS:
        for ip in (mid, app_ip):  # one gated, one un-gated per syscall
            total += 1
            if loader_baseline_decision(nr) == TRACE:
                baseline_traces += 1
            if pcgate_decision(nr, ip, TRAMP_LO, TRAMP_HI) == TRACE:
                pcgate_traces += 1

    # Baseline traces every path syscall; PC-gate traces only the un-gated ones.
    assert baseline_traces == total
    assert pcgate_traces < baseline_traces
    # Exactly the trampoline-emitted ones are spared.
    assert pcgate_traces == len(PATH_SYSCALLS)  # the app_ip half only


def test_ab_fully_gated_workload_collapses_traces_to_zero():
    # If EVERY path syscall is emitted from the trampoline (the design's target
    # steady state once the interposer covers the libc entry points), PC-gate
    # traces drop to zero while the baseline still traces all of them. This is
    # the "trap count collapse" the report's path_traps counter should show.
    mid = (TRAMP_LO + TRAMP_HI) // 2
    pcgate_traces = sum(
        1 for nr in PATH_SYSCALLS
        if pcgate_decision(nr, mid, TRAMP_LO, TRAMP_HI) == TRACE
    )
    baseline_traces = sum(
        1 for nr in PATH_SYSCALLS if loader_baseline_decision(nr) == TRACE
    )
    assert pcgate_traces == 0
    assert baseline_traces == len(PATH_SYSCALLS)


# --------------------------------------------------------------------------- #
# Guard: keep this host model in lockstep with the real artifacts. If the loader
# filter or the contract drifts (e.g. a syscall is added/removed), these string
# checks fail loudly so the model is updated alongside the source. They read the
# committed files but never build or run them.
# --------------------------------------------------------------------------- #

ROOT = Path(__file__).resolve().parents[1]
LOADER = ROOT / "app/src/main/cpp/runtime_report.cpp"
DESIGN = ROOT / "docs/design/pcgate-seccomp.md"


def test_loader_source_traces_the_modeled_path_syscalls():
    text = LOADER.read_text(encoding="utf-8")
    # Every path syscall this model knows about must appear by NR symbol in the
    # loader's filter, so the two cannot silently diverge.
    for name in PATH_SYSCALLS.values():
        assert f"__NR_{name}" in text, f"loader filter missing __NR_{name}"


def test_design_doc_states_the_pcgate_contract():
    text = DESIGN.read_text(encoding="utf-8")
    for needle in (
        "ALR_PCGATE",
        "instruction_pointer",
        "RESOLVE_IN_ROOT",
        "alr_tramp_lo",
        "alr_tramp_hi",
        "AUDIT_ARCH_AARCH64",
    ):
        assert needle in text, f"design doc missing: {needle}"
