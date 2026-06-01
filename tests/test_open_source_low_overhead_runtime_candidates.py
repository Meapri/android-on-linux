from pathlib import Path

DOC = Path(__file__).resolve().parents[1] / "docs" / "research" / "open-source-low-overhead-runtime-candidates.md"

REQUIRED_CANDIDATES = [
    "PRoot / Termux PRoot",
    "proot-rs",
    "fakechroot",
    "LD_PRELOAD path virtualization approaches",
    "libredirect-like path-rewrite shim",
    "bubblewrap",
    "nsjail",
    "gVisor",
    "seccomp user notification component",
]

REQUIRED_COLUMNS = [
    "License:",
    "Approach:",
    "Android non-root feasibility:",
    "Overhead expectation:",
    "Integration value:",
    "Risks:",
]


def _doc_text() -> str:
    assert DOC.exists(), f"missing research document: {DOC}"
    return DOC.read_text(encoding="utf-8")


def test_candidate_matrix_lists_required_open_source_runtime_candidates():
    text = _doc_text()

    for candidate in REQUIRED_CANDIDATES:
        assert f"Candidate: {candidate}" in text

    for column in REQUIRED_COLUMNS:
        assert column in text


def test_candidate_matrix_records_track_b_conclusions_and_recommendation():
    text = _doc_text()

    assert "No surveyed open-source project is a drop-in" in text
    assert "Top candidates for Track B follow-up" in text
    assert "LD_PRELOAD path virtualization" in text
    assert "proot-rs" in text
    assert "seccomp user notification" in text
    assert "do not replace the current PRoot backend yet" in text
    assert "bubblewrap/nsjail/gVisor as references" in text
