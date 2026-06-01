"""Classify the per-guest native-exec wall-clock (`exec_ms`) distribution for the
CP-3 enrichment, using the data WS-1 M2 already captured on-device — no new device
run is required.

Seed data (WS-1 M2, device):
- Light CLI (~18-20 ms): dynhello 19, env 19, id 20, dash 18-19, alr-png-test 23.
- Heavy lib loads (gtk/gimp link + init): gimp-3.0 135, gtk3-widget-factory 145,
  alr-gtk3-test 197.
- Intended dispatch / GUI waits: alr-input-test / alr-interactive-test ~6048-6053 ms.

IMPORTANT: the "long-running" bucket (> 1000 ms) is an INTENDED dispatch / GUI wait
(the guest is blocked waiting for input, not the loader spinning). It is NOT
mediation overhead and must not be read as such.

This module is a pure model over `bench.report_parse` — it only READS the parsed
`exec_ms` field; it never edits the C++/Kotlin sources nor the parser.
"""
from __future__ import annotations

import statistics
from dataclasses import dataclass

from bench.report_parse import parse_report

# Classification thresholds (inclusive upper bounds), in milliseconds.
LIGHT_CLI_MAX_MS = 30   # light CLI exec: dynhello/env/id/dash/alr-png-test (~18-23)
HEAVY_LIB_MAX_MS = 1000  # heavy lib load+init: gimp/gtk (~135-197)

LIGHT_CLI = "light-cli"
HEAVY_LIB = "heavy-lib"
LONG_RUNNING = "long-running"  # intended dispatch/GUI wait, NOT overhead

_CATEGORIES = (LIGHT_CLI, HEAVY_LIB, LONG_RUNNING)


def classify_exec(exec_ms: int) -> str:
    """Bucket an exec_ms value.

    <=30   -> "light-cli"     (light CLI exec)
    <=1000 -> "heavy-lib"     (heavy lib load + init)
    else   -> "long-running"  (intended dispatch/GUI wait, NOT overhead)
    """
    if exec_ms <= LIGHT_CLI_MAX_MS:
        return LIGHT_CLI
    if exec_ms <= HEAVY_LIB_MAX_MS:
        return HEAVY_LIB
    return LONG_RUNNING


@dataclass(frozen=True)
class ExecSample:
    """One classified guest exec measurement."""

    guest: str
    exec_ms: int
    category: str


def build_exec_profile(probes) -> list[ExecSample]:
    """Classify every probe carrying an exec_ms.

    Iterates objects exposing `.guest` and `.exec_ms`; probes whose `exec_ms`
    is None (the field predates WS-1 M2) are skipped.
    """
    samples: list[ExecSample] = []
    for p in probes:
        if p.exec_ms is None:
            continue
        samples.append(
            ExecSample(
                guest=p.guest,
                exec_ms=p.exec_ms,
                category=classify_exec(p.exec_ms),
            )
        )
    return samples


def summarize(samples) -> dict:
    """Aggregate a profile: per-category counts and light-cli min/max/median.

    Returns a dict with:
    - "counts": {category: count} for every category (0 when none).
    - "light_cli_min" / "light_cli_max" / "light_cli_median": exec_ms stats over
      the light-cli group, or None when that group is empty.
    """
    counts = {cat: 0 for cat in _CATEGORIES}
    light_ms: list[int] = []
    for s in samples:
        counts[s.category] = counts.get(s.category, 0) + 1
        if s.category == LIGHT_CLI:
            light_ms.append(s.exec_ms)

    if light_ms:
        light_min: int | None = min(light_ms)
        light_max: int | None = max(light_ms)
        light_median: float | None = statistics.median(light_ms)
    else:
        light_min = light_max = light_median = None

    return {
        "counts": counts,
        "light_cli_min": light_min,
        "light_cli_max": light_max,
        "light_cli_median": light_median,
    }


def profile_from_report(text: str) -> list[ExecSample]:
    """Parse a device report and build the exec profile from its guest probes."""
    return build_exec_profile(parse_report(text).guest_probes)


def to_markdown(samples) -> str:
    """Render a profile as a Markdown table plus a one-line summary."""
    lines = [
        "| guest | exec_ms | category |",
        "| --- | --- | --- |",
    ]
    for s in samples:
        lines.append(f"| {s.guest} | {s.exec_ms} | {s.category} |")

    summary = summarize(samples)
    counts = summary["counts"]
    summary_line = (
        f"summary: {len(samples)} samples — "
        f"{LIGHT_CLI}={counts[LIGHT_CLI]}, "
        f"{HEAVY_LIB}={counts[HEAVY_LIB]}, "
        f"{LONG_RUNNING}={counts[LONG_RUNNING]} "
        f"(light-cli median={summary['light_cli_median']} ms; "
        f"long-running = intended wait, not overhead)"
    )
    lines.append("")
    lines.append(summary_line)
    return "\n".join(lines)
