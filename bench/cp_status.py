"""Keyword-heuristic checkpoint (CP) dashboard over the device-evidence corpus.

The 5-session plan defines checkpoints CP-0..CP-6 (interface, GUI baseline, GPU-native,
CPU overhead, zero-copy dmabuf, universality/toolkit, zero-overhead convergence). This
module maps each checkpoint to a tuple of lowercase keywords and tags every evidence
document (from `bench.evidence_index.scan_evidence_dir`) whose FILENAME or TITLE mentions
any of those keywords.

HEURISTIC CAVEAT — NOT AUTHORITATIVE. This is a *keyword* match over filenames/titles
only; it does NOT read document bodies, does NOT confirm a PASS/FAIL outcome, and does
NOT verify the device evidence is actually conclusive. A "yes" merely means at least one
doc *mentions* the topic. Use it as a coverage hint, never as a sign-off. The
authoritative view is the evidence documents themselves.
"""
from __future__ import annotations

from pathlib import Path

from bench.evidence_index import scan_evidence_dir

# Each CP maps to lowercase keywords matched against the doc filename OR title (lowered).
# Ordered CP-0..CP-6 for stable dashboard output.
CP_KEYWORDS: dict[str, tuple[str, ...]] = {
    "CP-0": ("interface", "contract", "gpuringhook", "ahbframesource", "header"),
    "CP-1": ("display", "90hz", "1200x1920", "widget-factory", "keymap", "xkb", "gui"),
    "CP-2": ("glmark2", "gpu native", "gpu-native", "mali score"),
    "CP-3": ("overhead", "mediation", "exec_ms", "perf", "cpu"),
    "CP-4": ("dmabuf", "zero-copy", "zerocopy", "present"),
    "CP-5": ("toolkit", "qt", "sdl", "apt", "xwayland", "netsurf"),
    "CP-6": ("chromium", "user_notif", "syscall storm", "syscall-storm"),
}


def scan_cp_status(evidence_dir: str | Path) -> dict[str, list[str]]:
    """Map each CP to the evidence filenames whose filename OR title matches a keyword.

    Helper/non-evidence files are excluded by `scan_evidence_dir` itself (names starting
    with `_`, and `CAPTURE-RUNBOOK.md`). Matching is a case-insensitive substring test of
    each CP keyword against the lowercased filename and lowercased title. Filenames are
    returned in the scan order (date, filename) and de-duplicated per CP.
    """
    docs = scan_evidence_dir(evidence_dir)
    status: dict[str, list[str]] = {cp: [] for cp in CP_KEYWORDS}
    for doc in docs:
        haystack = f"{doc.filename}\n{doc.title}".lower()
        for cp, keywords in CP_KEYWORDS.items():
            if any(kw in haystack for kw in keywords):
                status[cp].append(doc.filename)
    return status


def _escape_cell(text: str) -> str:
    """Keep a value on one table cell: drop pipes/newlines that would break the row."""
    return text.replace("|", "\\|").replace("\n", " ").strip()


def build_cp_dashboard_markdown(status: dict[str, list[str]]) -> str:
    """Render the CP status as a markdown table, ordered CP-0..CP-6.

    Columns: Checkpoint | device evidence? | count | matching docs. The
    "device evidence?" column is "yes" iff the CP's match list is non-empty, else "no".
    A heading line flags this as a keyword heuristic, not an authoritative sign-off.
    """
    lines = [
        "# Checkpoint Status Dashboard (keyword heuristic)",
        "",
        "Heuristic only: filename/title keyword match, NOT an authoritative PASS/FAIL "
        "sign-off. A 'yes' means at least one evidence doc *mentions* the topic.",
        "",
        "| Checkpoint | device evidence? | count | matching docs |",
        "| --- | --- | --- | --- |",
    ]
    for cp in CP_KEYWORDS:
        matches = status.get(cp, [])
        has = "yes" if matches else "no"
        docs_cell = _escape_cell(", ".join(matches)) if matches else ""
        lines.append(f"| {cp} | {has} | {len(matches)} | {docs_cell} |")
    return "\n".join(lines) + "\n"
