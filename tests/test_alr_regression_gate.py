"""WS-5 host tests: no-regression matrix gate (bench/regression_gate.py)."""
from bench.regression_gate import GIMP_NOREGRESSION_PROBES, evaluate_text


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
