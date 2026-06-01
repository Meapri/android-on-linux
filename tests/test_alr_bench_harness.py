"""WS-5 host tests: report parser + CPU-overhead model (bench/)."""
import math

import pytest

from bench.cpu_overhead import (
    SYSCALL_LIGHT_MAX_OVERHEAD_PCT,
    Measurement,
    compute_overhead,
)
from bench.report_parse import parse_report


PASSING_REPORT = """\
build: 0.4.124-seize-mt-supervisor-v124
execution summary
ALR NATIVE LOADER GUEST EXEC: PASS
child exit=0 signal=0
guest stdout=Chromium 147.0.7727.137
alr native loader path-mediation traps=0 rewrites=0
gimp-probe guest=/bin/dynhello          exit=0  (stdout: alr-dyn-ok)
gimp-probe guest=/usr/bin/env           exit=0
gimp-probe guest=/usr/bin/id            exit=0
gimp-probe guest=/bin/dash              exit=0
gimp-probe guest=/bin/alr-png-test      exit=0
gimp-probe guest=/usr/bin/gimp-console-3.0  exit=127
gimp-probe guest=/bin/alr-wl-test       exit=0
gimp-probe guest=/bin/alr-pixman-test   exit=0
all: pcgate=1 interpose=1 traps=0 rewrites=0
alr sc seccomp_trace_events=0
alr sc path_rewrites=0
alr gpu boundary inproc dispatch ns/op=42
alr gpu boundary socket per-cmd ns/op=1300
alr gpu boundary shmem-ring ns/op=180
ALR PERF HARNESS: PASS
"""


def test_parse_full_report():
    r = parse_report(PASSING_REPORT)
    assert r.build_stamp == "0.4.124-seize-mt-supervisor-v124"
    assert r.guest_exec is not None
    assert r.guest_exec.passed is True
    assert r.guest_exec.exit_code == 0
    assert r.guest_exec.signal == 0
    assert r.guest_exec.stdout == "Chromium 147.0.7727.137"
    assert r.path_mediation.traps == 0
    assert r.path_mediation.rewrites == 0
    assert r.path_mediation.pcgate is True
    assert r.path_mediation.interpose is True
    assert r.seccomp_trace_events == 0
    assert r.sc_path_rewrites == 0
    assert r.gpu_boundary.inproc_dispatch_ns == 42
    assert r.gpu_boundary.socket_per_cmd_ns == 1300
    assert r.gpu_boundary.shmem_ring_ns == 180
    assert r.perf_harness_pass is True
    assert len(r.guest_probes) == 8
    exits = {p.guest: p.exit_code for p in r.guest_probes}
    assert exits["/usr/bin/gimp-console-3.0"] == 127
    assert exits["/bin/dynhello"] == 0


def test_parse_missing_markers_are_none():
    r = parse_report("build: 0.4.0-x\nnothing else here\n")
    assert r.build_stamp == "0.4.0-x"
    assert r.guest_exec is None
    assert r.path_mediation.traps is None
    assert r.path_mediation.pcgate is None
    assert r.gpu_boundary.inproc_dispatch_ns is None
    assert r.seccomp_trace_events is None
    assert r.perf_harness_pass is None
    assert r.guest_probes == ()


def test_overhead_syscall_light_pass():
    native = Measurement("native", wall_ns=1_000_000, samples=1000)  # 1000 ns/sample
    alr = Measurement("alr", wall_ns=1_040_000, samples=1000)  # 1040 ns/sample
    res = compute_overhead(native, alr, syscall_light=True, traps=0, rewrites=0, binary="microbench")
    assert math.isclose(res.overhead_pct, 4.0, abs_tol=1e-9)
    assert math.isclose(res.ratio, 1.04, abs_tol=1e-9)
    assert res.passes_target is True
    assert res.binary == "microbench"


def test_overhead_syscall_light_fail():
    native = Measurement("native", wall_ns=1000)
    alr = Measurement("alr", wall_ns=1200)  # +20%
    res = compute_overhead(native, alr, syscall_light=True)
    assert math.isclose(res.overhead_pct, 20.0, abs_tol=1e-9)
    assert res.passes_target is False
    assert res.overhead_pct > SYSCALL_LIGHT_MAX_OVERHEAD_PCT


def test_overhead_syscall_storm_reported_not_gated():
    native = Measurement("native", wall_ns=1000)
    alr = Measurement("alr", wall_ns=5000)  # +400% (chromium-class)
    res = compute_overhead(native, alr, syscall_light=False)
    assert res.overhead_pct > 100
    assert res.passes_target is True  # reported, not gated


def test_measurement_validation():
    with pytest.raises(ValueError):
        Measurement("bad", wall_ns=0)
    with pytest.raises(ValueError):
        Measurement("bad", wall_ns=100, samples=0)


def test_overhead_serialization_smoke():
    res = compute_overhead(
        Measurement("native", 1000), Measurement("alr", 1030), traps=0, rewrites=0, binary="b"
    )
    d = res.to_dict()
    assert d["passes_target"] is True
    assert d["target_max_overhead_pct"] == SYSCALL_LIGHT_MAX_OVERHEAD_PCT
    assert "overhead_pct" in res.to_json()
    md = res.to_markdown()
    assert "CPU overhead" in md
    assert "PASS" in md
