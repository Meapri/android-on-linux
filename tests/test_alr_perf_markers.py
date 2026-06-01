"""WS-5 host tests: WS-1 M2 CPU-mediation perf markers.

Covers the three perf microbench lines (`run_perf_comparison`, surfaced to
logcat) and the per-guest `exec_ms` (fork->reap native-exec wall-clock) that
WS-1 M2 added to the `gimp-probe guest=... exit=... exec_ms=K` line.

The perf-line spacing here is the verbatim device-captured form (multiple
spaces, spaces around `=`) from
docs/evidence/2026-06-01-ws1-m2-cpu-mediation-overhead.md. The parser regexes
use \\s+ / \\s*=\\s* so they also tolerate the source's compact `ns/op=` form.

Pure host test, no device required.
"""
from bench.report_parse import parse_report


# Verbatim device-captured perf lines + a gimp-probe line carrying exec_ms in the
# same field layout runtime_report.cpp emits (~L2401: "... stdout_bytes=K exec_ms=K").
SAMPLE_REPORT = """\
build: 0.4.127-ws5-perf-markers
execution summary
ALR PERF SYSCALL ROUNDTRIP MEASURED: PASS
alr perf alr xlate     ns/op = 4334.727
alr perf syscall getppid ns/op = 218.338
gimp-probe guest=/bin/dynhello exit=0 sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=14 exec_ms=19
gimp-probe guest=/usr/bin/gimp-console-3.0 exit=0 sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=128 exec_ms=48
"""

# A report missing all WS-1 M2 perf markers (and exec_ms) — negative case.
NO_PERF_REPORT = """\
build: 0.4.0-x
execution summary
gimp-probe guest=/bin/dynhello exit=0
nothing perf-related here
"""


def test_perf_markers_parsed():
    r = parse_report(SAMPLE_REPORT)
    assert r.perf_xlate_ns == 4334.727
    assert r.perf_syscall_ns == 218.338
    assert r.perf_roundtrip_measured is True


def test_exec_ms_tied_to_guest():
    r = parse_report(SAMPLE_REPORT)
    by_guest = {p.guest: p.exec_ms for p in r.guest_probes}
    assert by_guest["/bin/dynhello"] == 19
    assert by_guest["/usr/bin/gimp-console-3.0"] == 48
    # exec_ms is an int (parsed from %lld), not a float/string.
    assert all(isinstance(v, int) for v in by_guest.values())


def test_compact_ns_op_form_also_parses():
    """The source emits `ns/op=4334.727` (no spaces around `=`); regexes must
    accept that compact form too, not just the pretty-printed device capture."""
    compact = (
        "ALR PERF SYSCALL ROUNDTRIP MEASURED: PASS\n"
        "alr perf alr xlate ns/op=4334.727\n"
        "alr perf syscall getppid ns/op=218.338\n"
    )
    r = parse_report(compact)
    assert r.perf_xlate_ns == 4334.727
    assert r.perf_syscall_ns == 218.338
    assert r.perf_roundtrip_measured is True


def test_missing_perf_markers_are_none():
    r = parse_report(NO_PERF_REPORT)
    assert r.perf_xlate_ns is None
    assert r.perf_syscall_ns is None
    assert r.perf_roundtrip_measured is None
    # A probe line without exec_ms leaves exec_ms None (back-compat with old reports).
    assert r.guest_probes[0].guest == "/bin/dynhello"
    assert r.guest_probes[0].exec_ms is None
