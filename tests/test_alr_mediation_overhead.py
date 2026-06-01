"""Host tests for the WS-5 path-mediation overhead model (pure math, no I/O)."""
import math

import pytest

from bench.mediation_overhead import (
    MediationCost,
    MediationVerdict,
    amortized_xlate_ns,
    assess_mediation,
    to_markdown,
)

# WS-1 M2 device seed (docs/evidence/2026-06-01-ws1-m2-cpu-mediation-overhead.md).
XLATE_NS = 4334.727
SYSCALL_NS = 218.338


def test_syscall_units_matches_ws1_m2_seed():
    units = MediationCost(XLATE_NS, SYSCALL_NS).syscall_units
    assert math.isclose(units, 19.85, abs_tol=0.1)


def test_syscall_units_raises_on_nonpositive_syscall_ns():
    with pytest.raises(ValueError):
        _ = MediationCost(XLATE_NS, 0.0).syscall_units
    with pytest.raises(ValueError):
        _ = MediationCost(XLATE_NS, -1.0).syscall_units


def test_amortized_no_hits_is_cold_cost():
    assert amortized_xlate_ns(XLATE_NS, hit_ratio=0.0) == XLATE_NS


def test_amortized_all_hits_is_zero():
    assert amortized_xlate_ns(XLATE_NS, hit_ratio=1.0) == 0.0


def test_amortized_90pct_hits():
    eff = amortized_xlate_ns(XLATE_NS, hit_ratio=0.9)
    assert math.isclose(eff, 433.47, abs_tol=0.5)


def test_amortized_hit_ratio_out_of_range_raises():
    with pytest.raises(ValueError):
        amortized_xlate_ns(XLATE_NS, hit_ratio=-0.1)
    with pytest.raises(ValueError):
        amortized_xlate_ns(XLATE_NS, hit_ratio=1.1)


def test_assess_mediation_zero_traps_is_zero_roundtrip():
    v = assess_mediation(0, 0)
    assert v.supervisor_roundtrip_zero is True
    assert "zero ptrace round-trip" in v.detail


def test_assess_mediation_nonzero_traps_is_not_zero_roundtrip():
    v = assess_mediation(3, 2)
    assert v.supervisor_roundtrip_zero is False
    assert "3 supervisor round-trips" in v.detail


def test_to_markdown_smoke():
    cost = MediationCost(XLATE_NS, SYSCALL_NS)
    verdict = assess_mediation(0, 0)
    md = to_markdown(cost, hit_ratio=0.9, verdict=verdict)
    assert "syscall units" in md
    assert "supervisor round-trip" in md
    assert isinstance(verdict, MediationVerdict)
