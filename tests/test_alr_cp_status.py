"""Tests for the keyword-heuristic checkpoint dashboard (bench.cp_status).

Runs against the real device-evidence corpus under docs/evidence so the keyword
heuristic stays anchored to documents that actually exist.
"""
from __future__ import annotations

from pathlib import Path

from bench.cp_status import (
    CP_KEYWORDS,
    build_cp_dashboard_markdown,
    scan_cp_status,
)

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE_DIR = ROOT / "docs/evidence"


def test_scan_cp_status_has_exactly_seven_cp_keys() -> None:
    status = scan_cp_status(EVIDENCE_DIR)
    assert set(status) == set(CP_KEYWORDS)
    assert len(status) == 7
    assert set(status) == {f"CP-{i}" for i in range(7)}


def test_cp1_gui_display_keymap_docs_exist() -> None:
    # display/90hz + xkb/keymap/gui docs are in the corpus (v126, v127).
    status = scan_cp_status(EVIDENCE_DIR)
    assert len(status["CP-1"]) >= 1


def test_cp3_cpu_mediation_overhead_doc_exists() -> None:
    # ws1-m2-cpu-mediation-overhead.md covers CPU mediation overhead.
    status = scan_cp_status(EVIDENCE_DIR)
    assert len(status["CP-3"]) >= 1


def test_dashboard_markdown_contains_headers_and_cp1() -> None:
    status = scan_cp_status(EVIDENCE_DIR)
    md = build_cp_dashboard_markdown(status)
    assert "Checkpoint" in md
    assert "CP-1" in md
    # Heuristic caveat must be surfaced, not silently dropped.
    assert "heuristic" in md.lower()
