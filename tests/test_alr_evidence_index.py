"""WS-5 host tests: device-evidence aggregator/index (bench/evidence_index.py)."""
from pathlib import Path

from bench.evidence_index import (
    RECOMMENDED_SECTIONS,
    build_index_markdown,
    scan_evidence_dir,
    validate_evidence_doc,
)

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE_DIR = ROOT / "docs/evidence"


def test_scan_real_dir_returns_expected_corpus():
    docs = scan_evidence_dir(EVIDENCE_DIR)
    assert len(docs) >= 30
    # Every doc has a non-empty title.
    assert all(d.title.strip() for d in docs)
    # Helper files (e.g. _TEMPLATE.md) and CAPTURE-RUNBOOK.md are excluded.
    assert all(not d.filename.startswith("_") for d in docs)
    assert all(d.filename != "CAPTURE-RUNBOOK.md" for d in docs)
    # At least one doc yields a parsed date and one a parsed version.
    assert any(d.date is not None for d in docs)
    assert any(d.version is not None for d in docs)


def test_scan_accepts_str_path():
    docs = scan_evidence_dir(str(EVIDENCE_DIR))
    assert len(docs) >= 30


def test_scan_is_sorted_by_date_then_filename():
    docs = scan_evidence_dir(EVIDENCE_DIR)
    keys = [(d.date or "", d.filename) for d in docs]
    assert keys == sorted(keys)


def test_build_index_markdown_has_table_header():
    docs = scan_evidence_dir(EVIDENCE_DIR)
    md = build_index_markdown(docs)
    assert "| Date | Version | Title | File |" in md
    assert "Title" in md
    assert "| --- | --- | --- | --- |" in md
    # One data row per doc, plus heading + blurb + 2 header rows.
    assert md.count("\n|") >= len(docs)


def test_validate_evidence_doc_advisory():
    only_what_changed = "# Some doc\n\n## What changed\nstuff happened\n"
    assert validate_evidence_doc(only_what_changed) == ["Honest scope"]

    both = "## What changed\nx\n\n## Honest scope\ny\n"
    assert validate_evidence_doc(both) == []

    neither = "# bare doc with no template sections\n"
    assert validate_evidence_doc(neither) == list(RECOMMENDED_SECTIONS)
