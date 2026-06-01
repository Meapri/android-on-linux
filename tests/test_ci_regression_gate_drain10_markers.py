"""WS-5 guard: the CI/regression-gate doc must carry the drain#10 device-marker
checklist (§3.5) that the integration session checks each device drain.

drain#10 (v132) added three no-regression items to the gate:
  - GPU THROUGHPUT PASS (a new device marker, alongside LIVE + SCREEN-CUBE).
  - GLES coverage via the shim / wire-check seam (off-device round-trip assert).
  - per-guest `traps` non-increase (gtk3-widget-factory baseline now 99, down from
    135 via WS-1 CP-6 M2).

These lenient checks pin that the doc grew the per-drain device-marker checklist and
names each of the three additions, without being brittle about exact wording.
See docs/evidence/2026-06-02-breadth-fanout-drain.md.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "ci-regression-gate.md"


def _text() -> str:
    assert DOC.is_file(), f"CI/regression-gate doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


def test_doc_has_per_drain_device_marker_checklist():
    """A dedicated per-drain device-marker checklist section must exist."""
    text = _text()
    low = text.lower()
    assert "§3.5" in text or "3.5" in text, (
        "doc should add a §3.5 device-marker checklist subsection"
    )
    # The checklist is explicitly per-drain.
    assert ("드레인마다" in text) or ("each device drain" in low) or ("per-drain" in low), (
        "the marker checklist must be framed as a per-drain check"
    )


def test_doc_adds_gpu_throughput_pass_marker():
    text = _text()
    assert "ALR GPU THROUGHPUT: PASS" in text, (
        "regression-gate doc must add the GPU THROUGHPUT PASS device marker"
    )
    # Keep the existing LIVE / SCREEN-CUBE markers alongside it.
    assert "ALR GPU LIVE INTEGRATION: PASS" in text
    assert "ALR GPU SCREEN CUBE: PASS" in text


def test_doc_adds_gles_coverage_shim_wire_check():
    """GLES coverage is checked via the shim / wire-check seam."""
    text = _text()
    low = text.lower()
    assert "gles" in low, "doc must name GLES coverage as a gate item"
    assert "wire-check" in low or "build-wire-check.sh" in text, (
        "GLES coverage must be tied to the shim/wire-check seam"
    )
    # The 19-op state-setter coverage is the concrete content.
    assert "19" in text, (
        "doc should reference the 19 GLES2 state-setter coverage from drain#10"
    )


def test_doc_adds_traps_non_increase_gate():
    """per-guest traps must not increase; gtk3-widget-factory baseline = 99."""
    text = _text()
    low = text.lower()
    assert ("traps" in low) and (("비-증가" in text) or ("비증가" in text)
                                 or ("non-increase" in low) or ("not increase" in low)), (
        "doc must add a traps non-increase no-regression item"
    )
    # The new baseline (down from 135) must be recorded.
    assert "99" in text and "135" in text, (
        "doc must record the gtk3-widget-factory traps baseline 99 (down from 135)"
    )


def test_doc_cites_drain10_evidence():
    text = _text()
    assert "2026-06-02-breadth-fanout-drain" in text, (
        "regression-gate doc must cite the drain#10 evidence for the new markers"
    )
