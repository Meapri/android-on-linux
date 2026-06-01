"""WS-5 host test: the DEVICE-REQ protocol + CP-status dashboard doc exists and
carries the required content (protocol terms + an embedded dashboard table)."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/research/ws5-device-req-and-cp-status.md"


def test_doc_exists() -> None:
    assert DOC.is_file(), f"missing WS-5 DEVICE-REQ doc: {DOC}"


def test_doc_has_required_content() -> None:
    text = DOC.read_text(encoding="utf-8")
    for needle in ("DEVICE-REQ", "glmark2", "baseline", "CP-2", "Checkpoint"):
        assert needle in text, f"doc missing required token: {needle!r}"
