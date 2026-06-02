"""WS-5 host tests for ADR-002 §3 P1 (M-R2) syscall-mix parser/model.

Pure host tests against synthetic logcat fixtures — no device, no I/O, no real
seccomp. Cover: normal parsing of the 3 M-R2 lines, the path/non-path trap split
arithmetic, the round-trip back-calculation + storm classification, and the
edges (empty histogram, missing lines, zero-trap division guard, top-16 cap).
"""
from __future__ import annotations

import pytest

from bench.report_parse import (
    _parse_emul_hist,
    _parse_stime,
    _parse_trace_hist,
)
from bench.syscall_mix import (
    PATH_NRS,
    STORM_AMBIGUOUS,
    STORM_ROUNDTRIP,
    STORM_SYSCALL_WEIGHT,
    TOP_HIST,
    SyscallMix,
    classify_storm,
    parse_syscall_mix,
    summarize_markdown,
)


# --------------------------------------------------------------------------- #
# Synthetic logcat fixtures. Path nrs: {34,35,48,56,78,79,291,437,439}.
# --------------------------------------------------------------------------- #

# A round-trip-dominated storm: nonvol_ctxt == 2*(traps+emul) exactly.
# trace_hist: openat(56)=300 + newfstatat(79)=200 path; futex(98)=500 +
# epoll_wait(22)=100 non-path absorb candidates. emul: openat2(437)=40.
ROUNDTRIP_REPORT = """\
build: 0.4.200-cp6-mr2
execution summary
alr sc trace_hist 98:500 56:300 79:200 22:100
alr sc emul_hist 437:40
alr sc stime_us=900000 utime_us=120000 nonvol_ctxt=2280 traps=1100 emul=40
ALR NATIVE LOADER GUEST EXEC: PASS
child exit=0 signal=0
"""

# A syscall-weight-dominated storm: almost no context switches relative to the
# (few) round-trips -> ratio well below the syscall-weight band.
WEIGHT_REPORT = """\
build: 0.4.200-cp6-mr2
alr sc trace_hist 56:10
alr sc emul_hist
alr sc stime_us=5000000 utime_us=4000000 nonvol_ctxt=2 traps=10 emul=0
"""


def test_parse_all_three_lines():
    mix = parse_syscall_mix(ROUNDTRIP_REPORT)
    assert mix.trace_hist == {98: 500, 56: 300, 79: 200, 22: 100}
    assert mix.emul_hist == {437: 40}
    assert mix.stime == {
        "stime_us": 900000,
        "utime_us": 120000,
        "nonvol_ctxt": 2280,
        "traps": 1100,
        "emul": 40,
    }


def test_trace_hist_path_split_arithmetic():
    mix = parse_syscall_mix(ROUNDTRIP_REPORT)
    # path: 56(openat)=300 + 79(newfstatat)=200 = 500
    assert mix.path_trap == 500
    # non-path: 98(futex)=500 + 22(epoll_wait)=100 = 600 (absorb candidates)
    assert mix.nonpath_trap == 600
    assert mix.total_trap == 1100
    assert mix.path_trap_ratio == pytest.approx(500 / 1100)
    assert mix.absorb_candidate_ratio == pytest.approx(600 / 1100)
    # the two ratios partition the traps
    assert mix.path_trap_ratio + mix.absorb_candidate_ratio == pytest.approx(1.0)


def test_emul_total_and_authoritative_counts():
    mix = parse_syscall_mix(ROUNDTRIP_REPORT)
    assert mix.emul_total == 40
    # stime line is authoritative for traps/emul (not the trace_hist sum).
    assert mix.traps == 1100  # from stime, not total_trap(=1100 here, but sourced from stime)
    assert mix.emul == 40
    assert mix.nonvol_ctxt == 2280


def test_roundtrip_back_calculation():
    mix = parse_syscall_mix(ROUNDTRIP_REPORT)
    # nonvol_ctxt / max(1, traps+emul) = 2280 / 1140 = 2.0
    assert mix.ctxt_per_roundtrip == pytest.approx(2.0)
    assert classify_storm(mix) == STORM_ROUNDTRIP


def test_classify_syscall_weight_dominated():
    mix = parse_syscall_mix(WEIGHT_REPORT)
    # 2 / max(1, 10) = 0.2 <= 0.5 -> syscall-weight dominated
    assert mix.ctxt_per_roundtrip == pytest.approx(0.2)
    assert classify_storm(mix) == STORM_SYSCALL_WEIGHT


def test_classify_ambiguous_middle_band():
    # ratio == 1.0: between SYSCALL_WEIGHT_MAX(0.5) and ROUNDTRIP_MIN(1.5).
    mix = SyscallMix(
        trace_hist={56: 100},
        emul_hist={},
        stime={
            "stime_us": 1,
            "utime_us": 1,
            "nonvol_ctxt": 100,
            "traps": 100,
            "emul": 0,
        },
    )
    assert mix.ctxt_per_roundtrip == pytest.approx(1.0)
    assert classify_storm(mix) == STORM_AMBIGUOUS


# --------------------------------------------------------------------------- #
# Edge: empty histogram (present line, no pairs) parses to {}.
# --------------------------------------------------------------------------- #

def test_empty_trace_hist_parses_to_empty_dict():
    text = "alr sc trace_hist\nalr sc emul_hist 437:5\n"
    mix = parse_syscall_mix(text)
    assert mix.trace_hist == {}
    assert mix.emul_hist == {437: 5}
    assert mix.path_trap == 0
    assert mix.nonpath_trap == 0
    # zero-trap division guard: ratios are 0.0, not a ZeroDivisionError.
    assert mix.path_trap_ratio == 0.0
    assert mix.absorb_candidate_ratio == 0.0


# --------------------------------------------------------------------------- #
# Edge: missing lines stay None, independently.
# --------------------------------------------------------------------------- #

def test_missing_all_lines_yields_none():
    mix = parse_syscall_mix("build: x\nnothing relevant here\n")
    assert mix.trace_hist is None
    assert mix.emul_hist is None
    assert mix.stime is None
    # derived values fall back gracefully
    assert mix.path_trap == 0
    assert mix.total_trap == 0
    assert mix.emul_total == 0
    assert mix.nonvol_ctxt is None
    assert mix.ctxt_per_roundtrip is None
    # no stime -> traps/emul fall back to histogram sums (also 0 here)
    assert mix.traps == 0
    assert mix.emul == 0
    # no signal -> ambiguous
    assert classify_storm(mix) == STORM_AMBIGUOUS


def test_missing_stime_falls_back_to_hist_sums():
    text = "alr sc trace_hist 56:7 98:3\nalr sc emul_hist 437:4\n"
    mix = parse_syscall_mix(text)
    assert mix.stime is None
    # traps/emul derived from the histograms when stime is absent
    assert mix.traps == 10  # 7 + 3
    assert mix.emul == 4
    # but no nonvol_ctxt to divide -> ratio None -> ambiguous
    assert mix.nonvol_ctxt is None
    assert mix.ctxt_per_roundtrip is None
    assert classify_storm(mix) == STORM_AMBIGUOUS


# --------------------------------------------------------------------------- #
# Edge: zero round-trips division guard. traps+emul == 0 but a stime line exists.
# --------------------------------------------------------------------------- #

def test_zero_roundtrip_division_guard():
    text = (
        "alr sc trace_hist\n"
        "alr sc emul_hist\n"
        "alr sc stime_us=10 utime_us=10 nonvol_ctxt=0 traps=0 emul=0\n"
    )
    mix = parse_syscall_mix(text)
    # max(1, 0+0) flooring: 0 / 1 == 0.0, no ZeroDivisionError.
    assert mix.ctxt_per_roundtrip == pytest.approx(0.0)
    # zero round-trips -> nothing for a round-trip fix to remove -> ambiguous,
    # even though the raw ratio (0.0) is below the syscall-weight threshold.
    assert classify_storm(mix) == STORM_AMBIGUOUS


def test_nonzero_ctxt_zero_roundtrip_does_not_divide_by_zero():
    text = "alr sc stime_us=1 utime_us=1 nonvol_ctxt=37 traps=0 emul=0\n"
    mix = parse_syscall_mix(text)
    # 37 / max(1,0) == 37.0 (raw nonvol_ctxt, not a crash)
    assert mix.ctxt_per_roundtrip == pytest.approx(37.0)
    # still ambiguous: zero round-trips means the ratio is meaningless for the
    # round-trip-vs-weight question.
    assert classify_storm(mix) == STORM_AMBIGUOUS


# --------------------------------------------------------------------------- #
# Edge: top-16 truncation flag.
# --------------------------------------------------------------------------- #

def test_top16_truncation_flagged():
    # Build a trace_hist of exactly TOP_HIST distinct nrs.
    nrs = list(range(100, 100 + TOP_HIST))
    body = " ".join(f"{nr}:1" for nr in nrs)
    text = f"alr sc trace_hist {body}\n"
    mix = parse_syscall_mix(text)
    assert mix.trace_hist is not None
    assert len(mix.trace_hist) == TOP_HIST
    assert mix.truncated is True
    assert "top-16 cap" in summarize_markdown(mix)


def test_below_top16_not_flagged():
    text = "alr sc trace_hist 56:1 79:1\n"
    mix = parse_syscall_mix(text)
    assert mix.truncated is False
    assert "top-16 cap" not in summarize_markdown(mix)


def test_no_trace_hist_not_truncated():
    mix = parse_syscall_mix("build: x\n")
    assert mix.truncated is False


# --------------------------------------------------------------------------- #
# Markdown summary smoke + content checks.
# --------------------------------------------------------------------------- #

def test_summarize_markdown_contains_verdict_and_split():
    mix = parse_syscall_mix(ROUNDTRIP_REPORT)
    md = summarize_markdown(mix)
    assert STORM_ROUNDTRIP in md
    assert "path_trap = 500" in md
    assert "nonpath_trap = 600" in md
    # named path syscalls render with their symbol
    assert "openat=300" in md
    assert "newfstatat=200" in md
    # round-trip ratio printed
    assert "2.00" in md


def test_summarize_markdown_handles_absent_lines():
    mix = parse_syscall_mix("build: x\n")
    md = summarize_markdown(mix)
    # must not crash; reports absent lines and ambiguous verdict
    assert STORM_AMBIGUOUS in md
    assert "(line absent)" in md
    assert "n/a" in md


# --------------------------------------------------------------------------- #
# Guard: keep the path-nr set in lockstep with the roadmap's documented set.
# --------------------------------------------------------------------------- #

def test_path_nrs_match_roadmap_set():
    assert PATH_NRS == frozenset({34, 35, 48, 56, 78, 79, 291, 437, 439})


# --------------------------------------------------------------------------- #
# Direct exercise of the report_parse ADD-only helpers.
# --------------------------------------------------------------------------- #

def test_report_parse_helpers_directly():
    assert _parse_trace_hist("alr sc trace_hist 56:3 437:1\n") == {56: 3, 437: 1}
    assert _parse_trace_hist("no hist here") is None
    assert _parse_emul_hist("alr sc emul_hist 439:9\n") == {439: 9}
    assert _parse_stime(
        "alr sc stime_us=1 utime_us=2 nonvol_ctxt=3 traps=4 emul=5\n"
    ) == {"stime_us": 1, "utime_us": 2, "nonvol_ctxt": 3, "traps": 4, "emul": 5}
    assert _parse_stime("no stime") is None


def test_duplicate_nr_in_hist_accumulates():
    # Defensive: if the supervisor ever emits a duplicate nr, counts sum.
    assert _parse_trace_hist("alr sc trace_hist 56:3 56:4\n") == {56: 7}


def test_malformed_token_skipped_not_fatal():
    # A stray non-pair token between valid pairs is ignored, not fatal.
    mix = parse_syscall_mix("alr sc trace_hist 56:3 garbage 79:2\n")
    assert mix.trace_hist == {56: 3, 79: 2}
