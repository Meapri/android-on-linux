"""Host unit tests for bench/storm_cost_model.py (ADR-002 §3 P4).

Pure calculation / regression tests. The host (darwin, arm64 macOS) CANNOT run
a real-kernel seccomp NEW_LISTENER, ptrace round-trip, or aarch64 self-rewrite,
so these tests verify ONLY the cost *model* arithmetic and the usable verdict.
The per-strategy ns constants are estimates; their effect on a real Chromium
storm is DEVICE-ONLY (M-R2 / M-R5 / M-R1 gates). See the module docstring.

Run with:  uvx --with pytest pytest tests/test_storm_cost_model.py -q
"""
from __future__ import annotations

import math

import pytest

from bench.storm_cost_model import (
    ADR_SCENARIO_N,
    DEFAULT_THRESHOLD_S,
    PATH_SYSCALL_NRS,
    VALID_STRATEGIES,
    StormCostParams,
    StormMeasurement,
    adr_scenario_table_markdown,
    estimate_storm_ns,
    estimate_storm_seconds,
    evaluate,
    is_usable,
    judgment_table_markdown,
    split_traps_by_path,
)

# Default constants, so the formula tests read the same numbers the module uses.
P = StormCostParams()
SECCOMP = P.seccomp_dispatch_ns      # 24
RAW = P.raw_svc_base_ns              # 200
PTRACE = P.ptrace_roundtrip_ns       # 100_000
HOOK = P.svc_hook_ns                 # 34
UN_OPT = P.user_notif_optimistic_ns  # 2000
UN_PESS = P.user_notif_pessimistic_ns  # 5000


# --------------------------------------------------------------------------- #
# Constant sanity — pin the seeded ns values so a silent edit fails loudly.
# --------------------------------------------------------------------------- #

def test_default_constants_match_adr():
    assert SECCOMP == 24.0
    assert RAW == 200.0
    assert PTRACE == 100_000.0
    assert HOOK == 34.0
    assert UN_OPT == 2_000.0
    assert UN_PESS == 5_000.0
    assert DEFAULT_THRESHOLD_S == 10.0
    assert ADR_SCENARIO_N == 50_000_000


def test_path_syscall_nrs_exact_arm64_set():
    # The 9 arm64 path NRs from ADR-002 / PCGATE. Frozen — divergence is a bug.
    assert PATH_SYSCALL_NRS == frozenset({34, 35, 48, 56, 78, 79, 291, 437, 439})


# --------------------------------------------------------------------------- #
# Per-strategy formula correctness (the four ADR-002 §1 equations).
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("N,trap", [(0, 0), (1, 0), (1, 1), (1_000, 17), (5_000_000, 3)])
def test_current_ret_trace_formula(N, trap):
    got = estimate_storm_ns("current_ret_trace", N, trap)
    assert got == N * SECCOMP + trap * PTRACE


@pytest.mark.parametrize("N", [0, 1, 1_000, 50_000_000])
def test_svc_rewrite_formula_ignores_trap_count(N):
    # svc_rewrite has NO round-trip term: it does not depend on trap_count.
    a = estimate_storm_ns("svc_rewrite", N, 0)
    b = estimate_storm_ns("svc_rewrite", N, 999)
    assert a == b == N * (SECCOMP + HOOK)


def test_user_notif_formula_with_explicit_split():
    N, trap = 1_000_000, 1_000
    absorbed, path_trap = 800, 200
    got = estimate_storm_ns(
        "user_notif", N, trap, absorbed=absorbed, path_trap=path_trap
    )
    assert got == N * SECCOMP + absorbed * UN_OPT + path_trap * PTRACE


def test_user_notif_default_split_is_no_absorption_floor():
    # With no explicit split, ALL traps stay on ptrace (absorbed=0) — the honest
    # "we measured nothing" floor. It must then equal current_ret_trace.
    N, trap = 2_000_000, 500
    un = estimate_storm_ns("user_notif", N, trap)
    cur = estimate_storm_ns("current_ret_trace", N, trap)
    assert un == cur == N * SECCOMP + trap * PTRACE


@pytest.mark.parametrize("N", [0, 1, 1_000, 50_000_000])
def test_seccomp_off_formula_drops_the_floor(N):
    # seccomp_off uses raw_svc_base and NO 24ns dispatch floor.
    assert estimate_storm_ns("seccomp_off", N, 12345) == N * RAW


def test_seconds_is_ns_over_1e9():
    N, trap = 5_000_000, 3
    ns = estimate_storm_ns("current_ret_trace", N, trap)
    assert estimate_storm_seconds("current_ret_trace", N, trap) == ns / 1e9


# --------------------------------------------------------------------------- #
# ADR-002 §5 scenario reproduction — the headline numbers this model must hit.
#   50M storm: 24ns floor only = 1.2s ; svc_rewrite(+34) ≈ 2.9s ;
#   user_notif optimistic ~100s ; pessimistic ~250s.
# --------------------------------------------------------------------------- #

def test_adr_s5_current_floor_is_1_2s():
    # trap_count=0 -> pure 24ns floor over 50M.
    s = estimate_storm_seconds("current_ret_trace", ADR_SCENARIO_N, 0)
    assert math.isclose(s, 1.2, rel_tol=1e-9)


def test_adr_s5_svc_rewrite_is_2_9s():
    s = estimate_storm_seconds("svc_rewrite", ADR_SCENARIO_N, 0)
    assert math.isclose(s, 2.9, rel_tol=1e-9)


def test_adr_s5_user_notif_optimistic_about_100s():
    # Whole storm absorbed via notif at 2000ns + 24ns floor each.
    s = estimate_storm_seconds(
        "user_notif", ADR_SCENARIO_N, ADR_SCENARIO_N,
        absorbed=ADR_SCENARIO_N, path_trap=0,
        params=P.with_user_notif(UN_OPT),
    )
    assert math.isclose(s, 101.2, rel_tol=1e-9)
    # "~100s" sanity band.
    assert 90.0 < s < 110.0


def test_adr_s5_user_notif_pessimistic_about_250s():
    s = estimate_storm_seconds(
        "user_notif", ADR_SCENARIO_N, ADR_SCENARIO_N,
        absorbed=ADR_SCENARIO_N, path_trap=0,
        params=P.with_user_notif(UN_PESS),
    )
    assert math.isclose(s, 251.2, rel_tol=1e-9)
    assert 240.0 < s < 260.0


def test_adr_s5_ordering_svc_rewrite_beats_user_notif():
    # The ADR's qualitative verdict: svc_rewrite ≪ user_notif on the same storm,
    # even after correcting R5's kill-shot arithmetic (floor survives both).
    svc = estimate_storm_seconds("svc_rewrite", ADR_SCENARIO_N, 0)
    un_opt = estimate_storm_seconds(
        "user_notif", ADR_SCENARIO_N, ADR_SCENARIO_N,
        absorbed=ADR_SCENARIO_N, path_trap=0, params=P.with_user_notif(UN_OPT),
    )
    assert svc < un_opt
    # And the floor (current, trap=0) is the cheapest seccomp-on option.
    floor = estimate_storm_seconds("current_ret_trace", ADR_SCENARIO_N, 0)
    assert floor < svc


def test_adr_s5_floor_survives_svc_rewrite_nail():
    # ADR §1 nail: the 24ns dispatch floor is NOT removed by svc_rewrite. The
    # svc_rewrite cost must therefore strictly exceed the pure floor by exactly
    # the hook term — never collapse to (or below) seccomp_off's removed floor.
    floor = estimate_storm_ns("current_ret_trace", ADR_SCENARIO_N, 0)
    svc = estimate_storm_ns("svc_rewrite", ADR_SCENARIO_N, 0)
    assert svc == floor + ADR_SCENARIO_N * HOOK
    assert svc > floor  # the nail: floor is still there, hook is added on top


# --------------------------------------------------------------------------- #
# usable verdict.
# --------------------------------------------------------------------------- #

def test_is_usable_threshold_strict_less_than():
    assert is_usable(9.999) is True
    assert is_usable(10.0) is False   # strict <, exactly at threshold is NOT usable
    assert is_usable(10.0001) is False
    assert is_usable(1.2) is True


def test_is_usable_custom_threshold():
    assert is_usable(101.2, threshold_s=120.0) is True
    assert is_usable(101.2, threshold_s=100.0) is False


def test_adr_s5_usability_verdicts():
    # On the 50M storm: floor 1.2s and svc_rewrite 2.9s are usable; user_notif
    # (100s/250s) is NOT.
    assert is_usable(estimate_storm_seconds("current_ret_trace", ADR_SCENARIO_N, 0))
    assert is_usable(estimate_storm_seconds("svc_rewrite", ADR_SCENARIO_N, 0))
    un = estimate_storm_seconds(
        "user_notif", ADR_SCENARIO_N, ADR_SCENARIO_N,
        absorbed=ADR_SCENARIO_N, path_trap=0, params=P.with_user_notif(UN_OPT),
    )
    assert not is_usable(un)


def test_current_strategy_becomes_unusable_with_many_traps():
    # The ptrace round-trip is the killer: even a modest trap_count at 100µs
    # blows the budget. 200k traps = 20s > 10s.
    s = estimate_storm_seconds("current_ret_trace", 1_000_000, 200_000)
    assert s > DEFAULT_THRESHOLD_S
    assert not is_usable(s)
    # Whereas svc_rewrite (no traps) on the same N stays usable.
    assert is_usable(estimate_storm_seconds("svc_rewrite", 1_000_000, 0))


# --------------------------------------------------------------------------- #
# M-R2 measured-injection interface: (N, trap_count) + trace_hist split.
# --------------------------------------------------------------------------- #

def test_split_traps_by_path():
    # trace_hist {nr: count}: 56=openat(path), 79=newfstatat(path), 63=read(non).
    hist = {56: 100, 79: 40, 63: 9999, 222: 5}  # 222=mmap non-path
    path, non_path = split_traps_by_path(hist)
    assert path == 140
    assert non_path == 9999 + 5


def test_split_traps_all_path_or_all_non_path():
    assert split_traps_by_path({56: 7, 437: 3}) == (10, 0)
    assert split_traps_by_path({63: 7, 64: 3}) == (0, 10)
    assert split_traps_by_path({}) == (0, 0)


def test_measurement_injection_drives_evaluate():
    # Inject a hypothetical M-R2 result and get a full per-strategy verdict.
    m = StormMeasurement(
        N=10_000_000, trap_count=50_000,
        absorbed=40_000, path_trap=10_000,
        label="injected M-R2 sample", measured=False,
    )
    rows = evaluate(m)
    by = {r.strategy: r for r in rows}
    assert set(by) == set(VALID_STRATEGIES)
    # current: 10M*24ns + 50k*100µs = 0.24s + 5s = 5.24s
    assert math.isclose(by["current_ret_trace"].seconds, 5.24, rel_tol=1e-9)
    # svc_rewrite: 10M*(24+34) = 0.58s
    assert math.isclose(by["svc_rewrite"].seconds, 0.58, rel_tol=1e-9)
    # user_notif optimistic: 10M*24 + 40k*2000 + 10k*100µs = 0.24+0.08+1 = 1.32s
    assert math.isclose(by["user_notif"].seconds, 1.32, rel_tol=1e-9)
    # seccomp_off: 10M*200 = 2.0s
    assert math.isclose(by["seccomp_off"].seconds, 2.0, rel_tol=1e-9)


def test_evaluate_user_notif_pessimistic_flag_uses_5000ns():
    m = StormMeasurement(N=1_000_000, trap_count=1_000, absorbed=1_000, path_trap=0)
    opt = {r.strategy: r for r in evaluate(m, user_notif_pessimistic=False)}
    pess = {r.strategy: r for r in evaluate(m, user_notif_pessimistic=True)}
    # 1M*24 + 1000*2000 = 0.024 + 0.002 = 0.026s ; pess 1M*24 + 1000*5000 = 0.029s
    assert math.isclose(opt["user_notif"].seconds, 0.026, rel_tol=1e-9)
    assert math.isclose(pess["user_notif"].seconds, 0.029, rel_tol=1e-9)
    # Non-notif rows are identical across the flag.
    assert opt["svc_rewrite"].seconds == pess["svc_rewrite"].seconds


def test_evaluate_user_notif_default_split_is_no_benefit():
    # Without absorbed/path_trap, user_notif must equal current_ret_trace exactly
    # (everything stays on ptrace) — never magically cheaper.
    m = StormMeasurement(N=3_000_000, trap_count=2_000)
    by = {r.strategy: r for r in evaluate(m)}
    assert by["user_notif"].seconds == by["current_ret_trace"].seconds
    assert "no-absorption floor" in by["user_notif"].note


# --------------------------------------------------------------------------- #
# Input validation.
# --------------------------------------------------------------------------- #

def test_unknown_strategy_raises():
    with pytest.raises(ValueError, match="unknown strategy"):
        estimate_storm_ns("ptrace_off", 10, 0)


@pytest.mark.parametrize("strategy", VALID_STRATEGIES)
def test_negative_N_raises(strategy):
    with pytest.raises(ValueError, match="N must be >= 0"):
        estimate_storm_ns(strategy, -1, 0)


def test_trap_count_exceeding_N_raises():
    with pytest.raises(ValueError, match="cannot exceed N"):
        estimate_storm_ns("current_ret_trace", 100, 101)


def test_negative_trap_count_raises():
    with pytest.raises(ValueError, match="trap_count must be >= 0"):
        estimate_storm_ns("current_ret_trace", 100, -1)


def test_user_notif_absorbed_plus_path_exceeding_N_raises():
    with pytest.raises(ValueError, match="cannot exceed N"):
        estimate_storm_ns("user_notif", 100, 100, absorbed=60, path_trap=60)


def test_zero_storm_is_zero_everywhere():
    for strat in VALID_STRATEGIES:
        assert estimate_storm_ns(strat, 0, 0) == 0.0
        assert estimate_storm_seconds(strat, 0, 0) == 0.0


# --------------------------------------------------------------------------- #
# Markdown judgment table.
# --------------------------------------------------------------------------- #

def test_judgment_table_markdown_shape_and_content():
    m = StormMeasurement(
        N=ADR_SCENARIO_N, trap_count=0,
        absorbed=ADR_SCENARIO_N, path_trap=0,
        label="50M storm", measured=False,
    )
    md = judgment_table_markdown(m)
    # Header + all four strategies present.
    assert "Storm usability — 50M storm" in md
    assert "| strategy | seconds | usable | note |" in md
    for strat in VALID_STRATEGIES:
        assert strat in md
    # The honest host-only / device-only disclaimer must be present.
    assert "DEVICE-ONLY" in md
    assert "estimated (host-injected, NOT measured)" in md  # measured=False marker
    # The §5 numbers appear in the rendered cells.
    assert "1.2" in md       # current floor
    assert "2.9" in md       # svc_rewrite
    # user_notif envelope row shows opt..pess.
    assert "opt" in md and "pess" in md


def test_measured_flag_changes_provenance_label():
    m = StormMeasurement(N=10, trap_count=0, measured=True, label="device run")
    md = judgment_table_markdown(m)
    assert "measured (device)" in md
    assert "host-injected" not in md


def test_adr_scenario_table_markdown_reproduces_headline_numbers():
    md = adr_scenario_table_markdown()
    assert "ADR-002 §5 canonical" in md
    # Floor 1.2s usable, svc_rewrite 2.9s usable, user_notif ~100s NOT usable.
    assert "1.2" in md
    assert "2.9" in md
    # 50M with thousands-separator.
    assert "50,000,000" in md
    # usable column: floor + svc_rewrite "yes", user_notif "NO" (>10s).
    assert "DEVICE-ONLY" in md


def test_table_marks_user_notif_unusable_on_adr_scenario():
    md = adr_scenario_table_markdown()
    # Find the user_notif line and confirm it is judged not-usable (NO).
    un_line = next(l for l in md.splitlines() if l.startswith("| user_notif"))
    assert "NO" in un_line


# --------------------------------------------------------------------------- #
# Honesty guard: the module must state it is model-only and device-cannot-run.
# --------------------------------------------------------------------------- #

def test_module_docstring_states_host_limitation():
    import bench.storm_cost_model as mod
    doc = mod.__doc__ or ""
    assert "DEVICE-ONLY" in doc
    assert "darwin" in doc
    # The nail must be stated.
    assert "24ns" in doc or "seccomp_dispatch" in doc
