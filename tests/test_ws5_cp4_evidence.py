"""WS-5 host test: the CP-4 present-verification evidence doc exists and carries
the required device markers + verdict tokens + the drain #5 cross-reference."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/evidence/2026-06-01-ws5-cp4-present-verified.md"
DRAIN5_REF = "2026-06-01-drain5-cp4-dmabuf-present-single-gate.md"


def test_doc_exists() -> None:
    assert DOC.is_file(), f"missing WS-5 CP-4 present evidence doc: {DOC}"


def test_doc_has_device_markers_and_verdict() -> None:
    text = DOC.read_text(encoding="utf-8")
    # Case-sensitive device markers / verdict tokens.
    for needle in ("ALR AHB ZEROCOPY IMPORT: PASS", "rendered=true", "12"):
        assert needle in text, f"doc missing required token: {needle!r}"
    # "zero-copy" may appear with any casing.
    assert "zero-copy" in text.lower(), "doc missing 'zero-copy' (case-insensitive)"


def test_doc_cross_references_drain5() -> None:
    text = DOC.read_text(encoding="utf-8")
    assert DRAIN5_REF in text, f"doc missing drain #5 cross-reference: {DRAIN5_REF!r}"
