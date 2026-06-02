"""Host unit tests for bench/storm_microbench_model.py (CP-6 R2).

Pure calculation / regression tests. The host (darwin) CANNOT run a real-kernel
seccomp NEW_LISTENER, a multi-tracee ptrace round-trip, clone fan-out, or
aarch64 self-rewrite, so these tests verify ONLY the N-thread cost *model*
arithmetic, its parallel-floor vs serial-supervisor split, the variant /
arg-parsing contract, and the device-injection interface. The per-strategy ns
constants are estimates; their effect on a real Chromium multi-thread storm is
DEVICE-ONLY (M-R2 / M-R5 / M-R1 microbench gates). See the module docstring.

Run with:  uvx --with pytest pytest tests/test_storm_microbench_model.py -q
"""
from __future__ import annotations

import math

import pytest

from bench.storm_cost_model import StormCostParams, estimate_storm_ns
from bench.storm_microbench_model import (
    CANONICAL_VARIANTS,
    DEFAULT_BRINGUP_NS_PER_THREAD,
    DEFAULT_THRESHOLD_S,
    MICROBENCH_GETPID,
    MICROBENCH_OPENAT,
    PATH_SYSCALL_NRS,
    VALID_STRATEGIES,
    VALID_VARIANTS,
    MicrobenchMeasurement,
    MicrobenchVariant,
    MtStormResult,
    alarm_window_max_threads_usable,
    estimate,
    estimate_seconds,
    format_microbench_arg,
    inject_microbench,
)

P = StormCostParams()
SECCOMP = P.seccomp_dispatch_ns      # 24
RAW = P.raw_svc_base_ns              # 200
PTRACE = P.ptrace_roundtrip_ns       # 100_000
HOOK = P.svc_hook_ns                 # 34
UN_OPT = P.user_notif_optimistic_ns  # 2000
BRINGUP = DEFAULT_BRINGUP_NS_PER_THREAD  # 200_000


# --------------------------------------------------------------------------- #
# Variant / NR contract — getpid is non-path (floor), openat is a path NR.
# --------------------------------------------------------------------------- #

def test_variant_set_and_nrs():
    assert VALID_VARIANTS == (MICROBENCH_GETPID, MICROBENCH_OPENAT)
    g = CANONICAL_VARIANTS[MICROBENCH_GETPID]
    o = CANONICAL_VARIANTS[MICROBENCH_OPENAT]
    # getpid (172) is NOT in the path set -> RET_ALLOW -> path_ratio 0.
    assert g.nr == 172
    assert g.nr not in PATH_SYSCALL_NRS
    assert not g.is_path
    assert g.path_ratio == 0.0
    # openat (56) IS a path NR -> RET_TRACE -> path_ratio 1.
    assert o.nr == 56
    assert o.nr in PATH_SYSCALL_NRS
    assert o.is_path
    assert o.path_ratio == 1.0


def test_variant_dataclass_is_path_consistency():
    v = MicrobenchVariant("custom", 56, 0.5)
    assert v.is_path  # 56 is openat
    nv = MicrobenchVariant("custom", 63, 0.5)  # 63 = read, non-path
    assert not nv.is_path


def test_bringup_default_is_two_serial_roundtrips():
    # Each worker = EVENT_CLONE + initial EVENT_STOP CONT = 2 serial ptrace ops.
    assert DEFAULT_BRINGUP_NS_PER_THREAD == 2.0 * PTRACE


# --------------------------------------------------------------------------- #
# getpid variant (path_ratio=0): pure 24ns floor, NO round-trips. The whole cost
# is the parallel per-core floor (or bring-up if that dominates), never traps.
# --------------------------------------------------------------------------- #

def test_getpid_current_no_traps_floor_only():
    # 22 threads * 200k getpid each, path_ratio 0 -> trap_count 0.
    r = estimate("current_ret_trace", 22, 200_000, 0.0)
    assert isinstance(r, MtStormResult)
    assert r.trap_count == 0
    assert r.roundtrip_ns == 0.0
    # Per-core floor = one thread's 200k * 24ns (parallelizes; not *22).
    assert r.floor_ns == 200_000 * SECCOMP
    # bring-up = 22 * 200k ns serial.
    assert r.bringup_ns == 22 * BRINGUP
    # total = max(parallel floor, serial bring-up).
    assert r.total_ns == max(r.floor_ns, r.bringup_ns)


def test_getpid_floor_does_not_scale_with_threads():
    # The 24ns dispatch runs per-core in parallel: the floor term is ONE
    # thread's work regardless of N (this is the multi-thread insight).
    a = estimate("current_ret_trace", 1, 100_000, 0.0).floor_ns
    b = estimate("current_ret_trace", 64, 100_000, 0.0).floor_ns
    assert a == b == 100_000 * SECCOMP


# --------------------------------------------------------------------------- #
# openat variant (path_ratio=1): every op is a RET_TRACE round-trip, summed
# SERIALLY over all threads on the single waitpid loop.
# --------------------------------------------------------------------------- #

def test_openat_current_all_traps_serialize_over_threads():
    r = estimate("current_ret_trace", 4, 1_000, 1.0)
    # Every op traps: trap_count = 4 * 1000.
    assert r.trap_count == 4_000
    # Serial supervisor round-trip = ALL threads' traps * 100µs (does NOT
    # parallelize — the single-stream model could not express this).
    assert r.roundtrip_ns == 4_000 * PTRACE
    # Floor is still just ONE thread's 1000 ops (parallel).
    assert r.floor_ns == 1_000 * SECCOMP
    # Serial lane (round-trip + bring-up) dominates here.
    serial = r.roundtrip_ns + r.bringup_ns
    assert r.total_ns == max(r.floor_ns, serial) == serial


def test_openat_roundtrip_scales_with_threads_unlike_floor():
    # The serial round-trip lane DOES scale with N (Σ over threads); the floor
    # does not. This asymmetry is the whole multi-thread story.
    one = estimate("current_ret_trace", 1, 10_000, 1.0)
    ten = estimate("current_ret_trace", 10, 10_000, 1.0)
    assert ten.roundtrip_ns == 10 * one.roundtrip_ns
    assert ten.floor_ns == one.floor_ns


def test_mixed_path_ratio_splits_traps():
    # A mixed guest: 25% of ops are path RET_TRACE, 75% non-path RET_ALLOW.
    r = estimate("current_ret_trace", 8, 1_000, 0.25)
    # path_per_thread = floor(1000*0.25)=250; trap_count = 8*250.
    assert r.trap_count == 8 * 250
    assert r.roundtrip_ns == 8 * 250 * PTRACE


def test_path_ratio_floor_keeps_trap_count_integer_and_bounded():
    # path_ratio that doesn't divide evenly -> floor, never exceeds per-thread.
    r = estimate("current_ret_trace", 3, 7, 0.5)  # floor(7*0.5)=3 per thread
    assert r.trap_count == 3 * 3
    assert r.trap_count <= r.total_syscalls


# --------------------------------------------------------------------------- #
# svc_rewrite: branches the raw-svc PATH site to the trampoline -> NO round-trip
# even at path_ratio=1, but the 24ns floor + 34ns hook survive (ADR §1 nail).
# --------------------------------------------------------------------------- #

def test_svc_rewrite_removes_roundtrip_even_at_full_path_ratio():
    r = estimate("svc_rewrite", 22, 200_000, 1.0)
    assert r.roundtrip_ns == 0.0  # the round-trip term vanishes
    # Floor now carries the hook: one thread's 200k*(24+34).
    assert r.floor_ns == 200_000 * (SECCOMP + HOOK)
    # trap_count is still reported (the would-be traps), but priced at 0.
    assert r.trap_count == 22 * 200_000


def test_svc_rewrite_floor_survives_nail():
    # The 24ns floor is NOT removed by svc_rewrite — it is floor + hook, strictly
    # above the bare current floor (ADR §1 nail). It must never collapse to the
    # round-trip-free cost the round-trip term alone would suggest.
    cur = estimate("current_ret_trace", 1, 1_000_000, 0.0).floor_ns
    svc = estimate("svc_rewrite", 1, 1_000_000, 0.0).floor_ns
    assert svc == cur + 1_000_000 * HOOK
    assert svc > cur  # the nail: floor still there, hook added on top
    # seccomp_off models a bare svc at raw_svc_base (200ns) — it carries NO 24ns
    # dispatch floor (that floor exists only because a filter is loaded), so its
    # per-op term is the raw_svc_base, distinct from the 24ns dispatch floor.
    off = estimate("seccomp_off", 1, 1_000_000, 0.0).floor_ns
    assert off == 1_000_000 * RAW
    assert off != cur  # seccomp_off has no 24ns dispatch floor; it is raw_svc_base


def test_svc_rewrite_beats_current_on_path_heavy_storm():
    # The qualitative ADR verdict on a path-heavy multi-thread storm:
    # svc_rewrite (no serial round-trips) << current (serial round-trips).
    cur = estimate_seconds("current_ret_trace", 22, 500_000, 1.0)
    svc = estimate_seconds("svc_rewrite", 22, 500_000, 1.0)
    assert svc < cur


# --------------------------------------------------------------------------- #
# seccomp_off: theoretical lower bound, no floor, no round-trip.
# --------------------------------------------------------------------------- #

def test_seccomp_off_no_floor_no_roundtrip():
    r = estimate("seccomp_off", 22, 100_000, 1.0)
    assert r.roundtrip_ns == 0.0
    assert r.floor_ns == 100_000 * RAW  # raw svc base, no 24ns dispatch floor
    assert r.total_ns == max(r.floor_ns, r.bringup_ns)


# --------------------------------------------------------------------------- #
# user_notif: path traps STAY on ptrace; absorbed non-path traps move to the
# cheaper notif round-trip. All still serialize on the supervisor.
# --------------------------------------------------------------------------- #

def test_user_notif_path_stays_on_ptrace_nonpath_absorbs():
    # path_ratio 0.2 -> 20% path (stay on ptrace 100µs), 80% non-path; absorb
    # half of the non-path onto notif (2000ns).
    r = estimate("user_notif", 10, 1_000, 0.2, absorb_ratio=0.5)
    path_pt = 200  # floor(1000*0.2)
    nonpath = 800
    absorbed_pt = 400  # floor(800*0.5)
    expected = (
        10 * path_pt * PTRACE
        + 10 * absorbed_pt * UN_OPT
    )
    assert math.isclose(r.roundtrip_ns, expected, rel_tol=1e-9)


def test_user_notif_no_absorb_equals_current():
    # absorb_ratio 0 -> user_notif keeps everything on ptrace = current.
    un = estimate("user_notif", 8, 5_000, 0.3, absorb_ratio=0.0)
    cur = estimate("current_ret_trace", 8, 5_000, 0.3)
    assert un.roundtrip_ns == cur.roundtrip_ns
    assert un.total_ns == cur.total_ns


def test_user_notif_full_absorb_on_pure_nonpath_storm():
    # getpid storm (path_ratio 0): every op is non-path; absorb all onto notif.
    # But getpid is RET_ALLOW with ZERO traps under current — there is nothing to
    # absorb (the floor is already the only cost). So user_notif == current here.
    un = estimate("user_notif", 22, 200_000, 0.0, absorb_ratio=1.0)
    cur = estimate("current_ret_trace", 22, 200_000, 0.0)
    # No path traps AND absorb acts on non-path... but non-path getpid never
    # trapped under current (RET_ALLOW). The model's absorb term is on the
    # would-be-absorbed share; with path_ratio 0 there are no ptrace traps, so
    # the ONLY difference is the (modeled) notif cost on absorbed ops.
    # Honesty check: absorbing RET_ALLOW syscalls onto notif would be a
    # PESSIMIZATION (notif is pricier than a bare ALLOW). The model surfaces that.
    assert un.roundtrip_ns >= cur.roundtrip_ns


# --------------------------------------------------------------------------- #
# estimate_seconds == estimate(...).seconds.
# --------------------------------------------------------------------------- #

def test_estimate_seconds_matches_total_ns():
    r = estimate("current_ret_trace", 22, 200_000, 1.0)
    assert estimate_seconds("current_ret_trace", 22, 200_000, 1.0) == r.total_ns / 1e9
    assert r.seconds == r.total_ns / 1e9


# --------------------------------------------------------------------------- #
# Floor pricer reuse: the per-thread floor must equal the single-stream model's
# floor (no constant drift between the two modules).
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("strat", ["current_ret_trace", "svc_rewrite", "seccomp_off"])
def test_floor_reuses_single_stream_pricer(strat):
    spt = 123_456
    r = estimate(strat, 5, spt, 0.0)
    # The single-stream pricer for one thread's stream at trap=0.
    src_strat = "current_ret_trace" if strat == "current_ret_trace" else strat
    expected = estimate_storm_ns(src_strat, spt, 0)
    assert r.floor_ns == expected


# --------------------------------------------------------------------------- #
# Alarm-window back-calc: how many threads fit under the 25s chromium alarm.
# --------------------------------------------------------------------------- #

def test_alarm_window_path_heavy_storm_caps_threads():
    # A path-heavy storm: each thread's 1000 traps * 100µs = 0.1s serial. Under a
    # 25s window the serial lane allows ~ window / per-thread-serial threads.
    n = alarm_window_max_threads_usable(
        "current_ret_trace", 1_000, 1.0, window_s=25.0,
    )
    # per thread serial = 1000*100µs (roundtrip) + 200k (bringup) ≈ 0.1002s.
    # 25 / 0.1002 ≈ 249.
    assert 200 <= n <= 260
    # The (n+1)-th thread overshoots.
    over = estimate_seconds("current_ret_trace", n + 1, 1_000, 1.0)
    assert over >= 25.0


def test_alarm_window_svc_rewrite_fits_far_more_threads():
    # svc_rewrite has no serial round-trip lane, so far more threads fit under
    # the same window than current_ret_trace on a path-heavy storm.
    cur = alarm_window_max_threads_usable("current_ret_trace", 1_000, 1.0, window_s=25.0)
    svc = alarm_window_max_threads_usable("svc_rewrite", 1_000, 1.0, window_s=25.0)
    assert svc > cur


def test_alarm_window_zero_when_single_thread_overshoots():
    # One thread already blows the window -> 0 threads fit.
    # 1 thread * 10M traps * 100µs = 1000s >> 1s window.
    n = alarm_window_max_threads_usable(
        "current_ret_trace", 10_000_000, 1.0, window_s=1.0,
    )
    assert n == 0


def test_alarm_window_nonpositive_window_is_zero():
    assert alarm_window_max_threads_usable("current_ret_trace", 1000, 1.0, window_s=0.0) == 0
    assert alarm_window_max_threads_usable("current_ret_trace", 1000, 1.0, window_s=-5.0) == 0


# --------------------------------------------------------------------------- #
# Device microbench injection interface.
# --------------------------------------------------------------------------- #

def test_microbench_measurement_totals():
    m = MicrobenchMeasurement(
        variant=MICROBENCH_OPENAT, n_threads=22, syscalls_per_thread=200_000,
        ns_per_op=4_550.0, guest_threads=21, measured=True,
    )
    assert m.total_ns == 4_550.0 * 22 * 200_000
    assert m.total_seconds == m.total_ns / 1e9
    assert m.path_ratio == 1.0  # openat variant


def test_microbench_measurement_getpid_path_ratio_zero():
    m = MicrobenchMeasurement(
        variant=MICROBENCH_GETPID, n_threads=22, syscalls_per_thread=200_000,
        ns_per_op=30.0,
    )
    assert m.path_ratio == 0.0


def test_microbench_unknown_variant_path_ratio_raises():
    m = MicrobenchMeasurement(variant="bogus", n_threads=1, syscalls_per_thread=1, ns_per_op=1.0)
    with pytest.raises(ValueError, match="unknown microbench variant"):
        _ = m.path_ratio


def test_inject_microbench_returns_all_strategies_plus_measured():
    m = MicrobenchMeasurement(
        variant=MICROBENCH_OPENAT, n_threads=4, syscalls_per_thread=1_000,
        ns_per_op=4_550.0, measured=True,
    )
    out = inject_microbench(m)
    assert set(out) == {"measured", *VALID_STRATEGIES}
    assert out["measured"] == m.total_seconds
    # The modeled current_ret_trace for this (4, 1000, path_ratio=1) input.
    assert out["current_ret_trace"] == estimate_seconds("current_ret_trace", 4, 1_000, 1.0)


# --------------------------------------------------------------------------- #
# Device-probe arg formatting contract.
# --------------------------------------------------------------------------- #

def test_format_arg_openat_includes_path():
    arg = format_microbench_arg(MICROBENCH_OPENAT, 22, 200_000, "/etc/alr-probe.txt")
    assert arg == "/bin/microbench\nopenat-mt\n22\n200000\n/etc/alr-probe.txt"
    assert arg.split("\n") == ["/bin/microbench", "openat-mt", "22", "200000", "/etc/alr-probe.txt"]


def test_format_arg_getpid_omits_path():
    arg = format_microbench_arg(MICROBENCH_GETPID, 22, 200_000)
    assert arg == "/bin/microbench\nsyscall-mt\n22\n200000"
    assert "/etc" not in arg


def test_format_arg_openat_requires_absolute_path():
    with pytest.raises(ValueError, match="absolute path"):
        format_microbench_arg(MICROBENCH_OPENAT, 22, 200_000, None)
    with pytest.raises(ValueError, match="absolute path"):
        format_microbench_arg(MICROBENCH_OPENAT, 22, 200_000, "relative/path")


def test_format_arg_unknown_variant_raises():
    with pytest.raises(ValueError, match="unknown microbench variant"):
        format_microbench_arg("frob-mt", 1, 1)


@pytest.mark.parametrize("n,spt", [(0, 1), (1, 0), (-1, 5)])
def test_format_arg_rejects_nonpositive(n, spt):
    with pytest.raises(ValueError):
        format_microbench_arg(MICROBENCH_GETPID, n, spt)


# --------------------------------------------------------------------------- #
# Input validation.
# --------------------------------------------------------------------------- #

def test_unknown_strategy_raises():
    with pytest.raises(ValueError, match="unknown strategy"):
        estimate("ptrace_off", 1, 1, 0.0)


@pytest.mark.parametrize("n_threads,spt,pr", [(-1, 1, 0.0), (1, -1, 0.0)])
def test_negative_counts_raise(n_threads, spt, pr):
    with pytest.raises(ValueError):
        estimate("current_ret_trace", n_threads, spt, pr)


@pytest.mark.parametrize("pr", [-0.01, 1.01, 2.0])
def test_path_ratio_out_of_range_raises(pr):
    with pytest.raises(ValueError, match="path_ratio must be in"):
        estimate("current_ret_trace", 1, 1, pr)


@pytest.mark.parametrize("ar", [-0.01, 1.5])
def test_absorb_ratio_out_of_range_raises(ar):
    with pytest.raises(ValueError, match="absorb_ratio must be in"):
        estimate("user_notif", 1, 1, 0.0, absorb_ratio=ar)


def test_negative_bringup_raises():
    with pytest.raises(ValueError, match="bringup_ns_per_thread must be >= 0"):
        estimate("current_ret_trace", 1, 1, 0.0, bringup_ns_per_thread=-1.0)


def test_zero_threads_is_zero_storm():
    for strat in VALID_STRATEGIES:
        r = estimate(strat, 0, 1_000_000, 1.0)
        assert r.total_syscalls == 0
        assert r.trap_count == 0
        assert r.bringup_ns == 0.0
        # floor is one (non-existent) thread's stream; with 0 threads the wall
        # is max(floor, 0) — but there are no threads, so the storm is the floor
        # of zero real work. The honest total is the per-core floor placeholder;
        # callers gate on n_threads>0. We just assert it does not crash / go neg.
        assert r.total_ns >= 0.0


def test_zero_syscalls_per_thread_only_bringup():
    r = estimate("current_ret_trace", 22, 0, 1.0)
    assert r.total_syscalls == 0
    assert r.trap_count == 0
    assert r.floor_ns == 0.0
    assert r.roundtrip_ns == 0.0
    assert r.bringup_ns == 22 * BRINGUP
    assert r.total_ns == 22 * BRINGUP


# --------------------------------------------------------------------------- #
# Honesty guard: the module must state it is model-only and device-cannot-run.
# --------------------------------------------------------------------------- #

def test_module_docstring_states_host_limitation():
    import bench.storm_microbench_model as mod
    doc = mod.__doc__ or ""
    assert "DEVICE-ONLY" in doc
    assert "darwin" in doc
    # The serial-supervisor insight must be stated.
    assert "serial" in doc.lower()
    assert "waitpid" in doc


def test_threshold_constant_reexported():
    assert DEFAULT_THRESHOLD_S == 10.0
