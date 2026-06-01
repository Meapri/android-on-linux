"""M3 — no-regression matrix gate over a captured device report. Pure.

Seeded from device evidence (the v122 supervisor-fastpath GIMP no-regression set,
docs/evidence/2026-06-01-device-supervisor-fastpath-noregression.md): a fixed set of
guest probes with their proven exit codes, plus the `all: pcgate=1 interpose=1 traps=0
rewrites=0` mediation invariant. A later integration build that changes any expected
exit, or that no longer runs traps=0/rewrites=0 for this proven set, FAILS the gate.

gimp-console-3.0 exits 127 on device (missing deps, NOT a mediation fault) — that is
the proven baseline, so 127 is the EXPECTED value here, not a failure.
"""
from __future__ import annotations

from dataclasses import dataclass

from .report_parse import ParsedReport, parse_report

# (guest path, expected exit code) — proven on device, see evidence doc above.
GIMP_NOREGRESSION_PROBES: tuple[tuple[str, int], ...] = (
    ("/bin/dynhello", 0),
    ("/usr/bin/env", 0),
    ("/usr/bin/id", 0),
    ("/bin/dash", 0),
    ("/bin/alr-png-test", 0),
    ("/usr/bin/gimp-console-3.0", 127),  # known/expected: missing deps, not mediation
    ("/bin/alr-wl-test", 0),
    ("/bin/alr-pixman-test", 0),
)


@dataclass(frozen=True)
class ProbeOutcome:
    guest: str
    expected_exit: int
    actual_exit: int | None
    ok: bool
    note: str = ""


@dataclass(frozen=True)
class GateResult:
    passed: bool
    outcomes: tuple[ProbeOutcome, ...]
    mediation_ok: bool
    mediation_detail: str
    build_stamp: str | None

    def to_markdown(self) -> str:
        head = "PASS" if self.passed else "FAIL"
        lines = [
            f"### Regression gate — **{head}** (build: {self.build_stamp or '?'})",
            "",
            "| guest | expected | actual | result |",
            "|-------|----------|--------|--------|",
        ]
        for o in self.outcomes:
            actual = "missing" if o.actual_exit is None else str(o.actual_exit)
            mark = "ok" if o.ok else "REGRESSED"
            note = f" — {o.note}" if o.note else ""
            lines.append(f"| `{o.guest}` | {o.expected_exit} | {actual} | {mark}{note} |")
        lines.append("")
        lines.append(f"mediation invariant (pcgate=1 interpose=1 traps=0 rewrites=0): "
                     f"{'ok' if self.mediation_ok else 'FAIL'} — {self.mediation_detail}")
        return "\n".join(lines)


def evaluate_report(
    parsed: ParsedReport,
    probes: tuple[tuple[str, int], ...] = GIMP_NOREGRESSION_PROBES,
) -> GateResult:
    """Gate a parsed report against the no-regression probe set + mediation invariant."""
    actual = {p.guest: p.exit_code for p in parsed.guest_probes}
    outcomes: list[ProbeOutcome] = []
    for guest, expected in probes:
        got = actual.get(guest)
        if got is None:
            outcomes.append(ProbeOutcome(guest, expected, None, ok=False, note="probe absent"))
        else:
            outcomes.append(ProbeOutcome(guest, expected, got, ok=(got == expected)))

    pm = parsed.path_mediation
    mediation_ok = (
        pm.pcgate is True
        and pm.interpose is True
        and pm.traps == 0
        and pm.rewrites == 0
    )
    mediation_detail = (
        f"pcgate={pm.pcgate} interpose={pm.interpose} traps={pm.traps} rewrites={pm.rewrites}"
    )

    passed = mediation_ok and all(o.ok for o in outcomes)
    return GateResult(
        passed=passed,
        outcomes=tuple(outcomes),
        mediation_ok=mediation_ok,
        mediation_detail=mediation_detail,
        build_stamp=parsed.build_stamp,
    )


def evaluate_text(
    report_text: str,
    probes: tuple[tuple[str, int], ...] = GIMP_NOREGRESSION_PROBES,
) -> GateResult:
    return evaluate_report(parse_report(report_text), probes)
