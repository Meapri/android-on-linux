"""WS-5 host tests: no-regression matrix gate (bench/regression_gate.py)."""
from bench.regression_gate import (
    GIMP_NOREGRESSION_PROBES,
    TRAPS_TOLERANCE,
    evaluate_text,
)


# Mirrors the device-proven v122 no-regression set: every probe at its expected exit,
# mediation invariant pcgate=1 interpose=1 traps=0 rewrites=0.
GOOD = """\
build: 0.4.124-seize-mt-supervisor-v124
gimp-probe guest=/bin/dynhello          exit=0  (stdout: alr-dyn-ok)
gimp-probe guest=/usr/bin/env           exit=0
gimp-probe guest=/usr/bin/id            exit=0
gimp-probe guest=/bin/dash              exit=0
gimp-probe guest=/bin/alr-png-test      exit=0
gimp-probe guest=/usr/bin/gimp-console-3.0  exit=127
gimp-probe guest=/bin/alr-wl-test       exit=0
gimp-probe guest=/bin/alr-pixman-test   exit=0
all: pcgate=1 interpose=1 traps=0 rewrites=0
"""


def test_gate_passes_on_proven_baseline():
    res = evaluate_text(GOOD)
    assert res.passed is True
    assert res.mediation_ok is True
    assert all(o.ok for o in res.outcomes)
    assert res.build_stamp == "0.4.124-seize-mt-supervisor-v124"
    # gimp-console's expected 127 must be treated as OK, not a failure.
    console = next(o for o in res.outcomes if o.guest == "/usr/bin/gimp-console-3.0")
    assert console.expected_exit == 127
    assert console.ok is True


def test_gate_fails_on_exit_regression():
    bad = GOOD.replace(
        "gimp-probe guest=/usr/bin/id            exit=0",
        "gimp-probe guest=/usr/bin/id            exit=1",
    )
    res = evaluate_text(bad)
    assert res.passed is False
    regressed = next(o for o in res.outcomes if o.guest == "/usr/bin/id")
    assert regressed.ok is False
    assert regressed.actual_exit == 1


def test_gate_fails_when_mediation_breaks():
    bad = GOOD.replace(
        "all: pcgate=1 interpose=1 traps=0 rewrites=0",
        "all: pcgate=1 interpose=1 traps=3 rewrites=2",
    )
    res = evaluate_text(bad)
    assert res.mediation_ok is False
    assert res.passed is False
    assert "traps=3" in res.mediation_detail


def test_gate_flags_absent_probe():
    bad = "\n".join(
        line for line in GOOD.splitlines() if "alr-pixman-test" not in line
    ) + "\n"
    res = evaluate_text(bad)
    assert res.passed is False
    missing = next(o for o in res.outcomes if o.guest == "/bin/alr-pixman-test")
    assert missing.actual_exit is None
    assert missing.ok is False
    assert "absent" in missing.note


def test_gate_markdown_smoke():
    md = evaluate_text(GOOD).to_markdown()
    assert "Regression gate" in md
    assert "PASS" in md
    assert "mediation invariant" in md


def test_baseline_probe_set_is_nonempty_and_includes_console():
    guests = {g for g, _ in GIMP_NOREGRESSION_PROBES}
    assert "/usr/bin/gimp-console-3.0" in guests
    assert len(GIMP_NOREGRESSION_PROBES) >= 8


# v127-style report: EVERY gimp-probe line carries per-guest traps. General guests
# run traps=0; only /usr/bin/gimp-3.0 does traps=1 (WS-1 M2 set_robust_list-class
# exception). The `all:`-aggregate still reports traps=0 rewrites=0.
V127_PER_GUEST_TRAPS = """\
build: 0.4.127-per-guest-traps-v127
gimp-probe guest=/bin/dynhello          exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=10 exec_ms=12
gimp-probe guest=/usr/bin/env           exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=0  exec_ms=11
gimp-probe guest=/usr/bin/id            exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=40 exec_ms=11
gimp-probe guest=/bin/dash              exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=0  exec_ms=13
gimp-probe guest=/bin/alr-png-test      exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=5  exec_ms=14
gimp-probe guest=/usr/bin/gimp-console-3.0  exit=127 sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=0 exec_ms=9
gimp-probe guest=/bin/alr-wl-test       exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=7  exec_ms=15
gimp-probe guest=/bin/alr-pixman-test   exit=0   sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 first_rewrite=(none) stdout_bytes=7  exec_ms=16
gimp-probe guest=/usr/bin/gimp-3.0      exit=0   sig=0 pcgate=1 interpose=1 traps=1 rewrites=0 first_rewrite=(none) stdout_bytes=99 exec_ms=820
all: pcgate=1 interpose=1 traps=0 rewrites=0
"""

# Probe set extending the device baseline with gimp-3.0 at its expected exit (0),
# so the v127 report's gimp-3.0 line is part of the gated matrix too.
V127_PROBES = GIMP_NOREGRESSION_PROBES + (("/usr/bin/gimp-3.0", 0),)


def test_gate_passes_with_per_guest_traps_and_gimp3_tolerated():
    res = evaluate_text(V127_PER_GUEST_TRAPS, probes=V127_PROBES)
    assert res.passed is True
    assert res.mediation_ok is True
    assert all(o.ok for o in res.outcomes)
    assert res.build_stamp == "0.4.127-per-guest-traps-v127"
    # gimp-3.0 traps=1 is the documented tolerated exception → no violation.
    assert res.per_guest_traps_violations == ()
    assert TRAPS_TOLERANCE["/usr/bin/gimp-3.0"] == 1
    # The markdown still renders PASS and shows no traps-violations section.
    md = res.to_markdown()
    assert "PASS" in md
    assert "per-guest traps violations" not in md


def test_gate_fails_when_general_guest_has_traps():
    # A GENERAL guest (/usr/bin/id) showing traps=2 must fail: tolerance for a
    # non-gimp-3.0 guest is 0.
    bad = V127_PER_GUEST_TRAPS.replace(
        "gimp-probe guest=/usr/bin/id            exit=0   sig=0 pcgate=1 interpose=1 traps=0",
        "gimp-probe guest=/usr/bin/id            exit=0   sig=0 pcgate=1 interpose=1 traps=2",
    )
    res = evaluate_text(bad, probes=V127_PROBES)
    assert res.passed is False
    # The aggregate mediation invariant is still satisfied — only the per-guest
    # traps check trips, proving the new check is what fails the gate.
    assert res.mediation_ok is True
    assert all(o.ok for o in res.outcomes)
    viol = next(v for v in res.per_guest_traps_violations if v.guest == "/usr/bin/id")
    assert viol.traps == 2
    assert viol.tolerated == 0
    # gimp-3.0 traps=1 is still tolerated, so it must NOT appear as a violation.
    assert all(v.guest != "/usr/bin/gimp-3.0" for v in res.per_guest_traps_violations)
    md = res.to_markdown()
    assert "FAIL" in md
    assert "per-guest traps violations" in md
    assert "/usr/bin/id" in md
