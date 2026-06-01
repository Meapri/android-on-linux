"""WS-5 guard: the compat matrix must record the device-confirmed glmark2 result.

`docs/research/alr-compat-matrix.md`'s GPU section used to mark the glmark2-es2
score as PENDING (WS-2 shim eglChooseConfig gap). CP-2 FINAL (drain#7) + CP-5 batch
(drain#8) closed that: glmark2 renders on real Mali, build+texture Score 1163,
software=false. This test pins that the matrix now reflects RUNS + the score and is
no longer claiming the score is PENDING — and that it links to the ratio framing.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"


def _text() -> str:
    assert MATRIX.is_file(), f"compat matrix missing: {MATRIX}"
    return MATRIX.read_text(encoding="utf-8")


def test_matrix_records_glmark2_device_score():
    text = _text()
    # CP-2 FINAL build-scene score and CP-5 build+texture score are both device-proven.
    assert "1074" in text, "matrix must record the CP-2 FINAL glmark2 build score 1074"
    assert "1163" in text, "matrix must record the CP-5 glmark2 build+texture score 1163"


def test_matrix_glmark2_score_is_no_longer_pending():
    """The specific glmark2 *score* row must not still say PENDING.

    PENDING is still legitimate elsewhere (full 14-scene aggregate, Mali-direct
    baseline), but a line that mentions a concrete glmark2 score AND 'RUNS' must
    exist — proving the score row was upgraded out of PENDING.
    """
    text = _text()
    lines = text.splitlines()
    score_run_lines = [
        ln for ln in lines
        if ("glmark2" in ln.lower())
        and ("RUNS" in ln)
        and (("1074" in ln) or ("1163" in ln))
    ]
    assert score_run_lines, (
        "matrix must carry a glmark2 row marked RUNS with a concrete score "
        "(CP-2 FINAL upgraded it out of PENDING)"
    )


def test_matrix_links_gpu_ratio_doc():
    text = _text()
    assert "cp2-gpu-ratio-glmark2.md" in text, (
        "matrix GPU section must point at the ALR-vs-Mali-direct ratio framing doc"
    )
