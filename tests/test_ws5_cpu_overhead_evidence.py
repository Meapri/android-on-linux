"""WS-5 CP-3 / M1 CPU-overhead evidence test.

Verifies WS-5's standardized quantification of WS-1's M2 CPU-mediation device
capture exists and carries the load-bearing tokens (the cold path-xlate ns/op,
the per-guest exec_ms metric, the traps=0 zero-round-trip result), and that it
honestly marks the native/PRoot baseline comparison as PENDING (so no % ratio is
fabricated). Also checks the compat matrix now records the device wall-clock.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs" / "evidence" / "2026-06-01-ws5-cpu-overhead-quantified.md"
MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"


def test_evidence_doc_exists():
    assert EVIDENCE.is_file(), f"missing WS-5 CPU-overhead evidence doc: {EVIDENCE}"


def test_evidence_carries_key_quantification_tokens():
    text = EVIDENCE.read_text(encoding="utf-8")
    # cold path-xlate ns/op
    assert "4334" in text, "missing path-xlate cold ns/op (4334) in evidence doc"
    # per-guest native-exec wall-clock metric
    assert "exec_ms" in text, "missing exec_ms metric in evidence doc"
    # path-mediation supervisor round-trip = ZERO
    assert "traps=0" in text, "missing traps=0 zero-round-trip result in evidence doc"


def test_evidence_marks_baseline_pending():
    # HONEST SCOPE: native/PRoot baseline not yet captured -> % ratio not computed.
    text = EVIDENCE.read_text(encoding="utf-8").lower()
    assert "pending" in text, "evidence doc must mark the baseline comparison as PENDING"


def test_compat_matrix_records_device_wallclock():
    text = MATRIX.read_text(encoding="utf-8")
    assert ("18-20ms" in text) or ("native-exec" in text), (
        "compat matrix must record the v127 WS-1 M2 device wall-clock"
    )
