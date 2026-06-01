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

import re
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

# Per-guest traps tolerance (WS-1 M2 evidence): every general guest must run
# traps=0 under ALR mediation. The SOLE documented exception is /usr/bin/gimp-3.0,
# which does traps=1 — a one-off set_robust_list-class trap on the GIMP path. Any
# guest whose per-guest `gimp-probe ... traps=N` exceeds this tolerance FAILS the gate.
TRAPS_TOLERANCE: dict[str, int] = {"/usr/bin/gimp-3.0": 1}

# Per-guest traps live on the SAME gimp-probe line, AFTER exit=, e.g.
# "gimp-probe guest=/usr/bin/id exit=0 sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 ...".
# Lines that predate this field simply don't match -> the per-guest check is skipped
# for them and the `all:`-aggregate mediation invariant stands unchanged.
_GIMP_PROBE_TRAPS = re.compile(
    r"gimp-probe guest=(\S+)\s+exit=\d+.*?\btraps=(\d+)\b"
)


@dataclass(frozen=True)
class ProbeOutcome:
    guest: str
    expected_exit: int
    actual_exit: int | None
    ok: bool
    note: str = ""


@dataclass(frozen=True)
class TrapsViolation:
    """A gimp-probe guest whose per-guest traps exceeded its TRAPS_TOLERANCE."""

    guest: str
    traps: int
    tolerated: int


@dataclass(frozen=True)
class GateResult:
    passed: bool
    outcomes: tuple[ProbeOutcome, ...]
    mediation_ok: bool
    mediation_detail: str
    build_stamp: str | None
    # NEW (WS-5, v127): per-guest traps violations parsed from gimp-probe lines that
    # carry a `traps=` field. Empty when the report predates per-guest traps (old
    # format) or when every guest is within tolerance. Optional + default → existing
    # GateResult construction/consumers are unaffected.
    per_guest_traps_violations: tuple[TrapsViolation, ...] = ()

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
        if self.per_guest_traps_violations:
            lines.append("")
            lines.append("per-guest traps violations:")
            for v in self.per_guest_traps_violations:
                lines.append(
                    f"- `{v.guest}` traps={v.traps} > tolerated {v.tolerated} — REGRESSED"
                )
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

    # Per-guest traps check (WS-1 M2): for each gimp-probe line that carries a
    # per-guest `traps=` field, flag a violation when traps exceeds the guest's
    # tolerance (0 for all general guests; 1 for /usr/bin/gimp-3.0). Lines without
    # a per-guest traps field never match the regex → skipped. If NO line carries
    # per-guest traps (old report format), this loop yields nothing and the
    # `all:`-aggregate mediation check above stands unchanged (backward compatible).
    traps_violations: list[TrapsViolation] = []
    for guest, traps_str in _GIMP_PROBE_TRAPS.findall(parsed.raw):
        traps = int(traps_str)
        tolerated = TRAPS_TOLERANCE.get(guest, 0)
        if traps > tolerated:
            traps_violations.append(TrapsViolation(guest, traps, tolerated))

    passed = (
        mediation_ok
        and all(o.ok for o in outcomes)
        and not traps_violations
    )
    return GateResult(
        passed=passed,
        outcomes=tuple(outcomes),
        mediation_ok=mediation_ok,
        mediation_detail=mediation_detail,
        build_stamp=parsed.build_stamp,
        per_guest_traps_violations=tuple(traps_violations),
    )


def evaluate_text(
    report_text: str,
    probes: tuple[tuple[str, int], ...] = GIMP_NOREGRESSION_PROBES,
) -> GateResult:
    return evaluate_report(parse_report(report_text), probes)
