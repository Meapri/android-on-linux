"""WS-5 drift guards for the CP-2 (GPU) and CP-3 (CPU) ratio research docs.

These two docs frame the §0 quantitative gates as native/Mali-direct *ratios*:
  * `docs/research/cp2-gpu-ratio-glmark2.md` — ALR glmark2 score (numerator,
    device-proven) over a Mali-direct baseline (denominator, PENDING_DEVICE).
  * `docs/research/cp3-cpu-overhead-ratio.md` — same-binary native-vs-ALR CPU
    overhead (compute 0%, syscall ~12%), CLOSED.

The tests are intentionally lenient (load-bearing tokens + cross-references) so
they pin the honest framing without being brittle about prose. They also keep the
CP-2 doc honest: the Mali-direct baseline must stay marked PENDING_DEVICE (no
fabricated ratio), and any docs/ path it cites must resolve.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CP2 = ROOT / "docs" / "research" / "cp2-gpu-ratio-glmark2.md"
CP3 = ROOT / "docs" / "research" / "cp3-cpu-overhead-ratio.md"

_DOCS_PATH = re.compile(r"docs/[\w./-]+\.md")


def _read(p: Path) -> str:
    assert p.is_file(), f"missing WS-5 ratio doc: {p}"
    return p.read_text(encoding="utf-8")


def _assert_docs_paths_resolve(text: str, src: Path) -> None:
    for tok in sorted(set(_DOCS_PATH.findall(text))):
        # self-references to the two ratio docs themselves are fine
        assert (ROOT / tok).is_file(), (
            f"{src.name} cites {tok!r} but no such file exists under {ROOT}"
        )


def test_cp2_doc_carries_alr_score_and_baseline_pending():
    text = _read(CP2)
    # ALR numerator is the device-proven score (build+texture, 8 MiB ring).
    assert "1163" in text, "CP-2 ratio doc must carry the ALR glmark2 score 1163"
    # software-renderer gate side already PASS (Mali, software=false).
    assert "software=false" in text or "software renderer=false" in text.lower(), (
        "CP-2 ratio doc must record the software=false (non-software renderer) gate"
    )
    # §0 GPU ratio target 0.70 must be the gate floor.
    assert "0.70" in text or "70%" in text, "CP-2 doc must state the §0 0.70 ratio floor"
    # HONEST: Mali-direct denominator must stay PENDING_DEVICE (no fabricated ratio).
    assert "PENDING_DEVICE" in text, (
        "CP-2 ratio doc must mark the Mali-direct baseline as PENDING_DEVICE"
    )


def test_cp2_doc_gives_a_measurement_procedure_for_the_baseline():
    text = _read(CP2)
    # The integration session must be able to fill the baseline: doc gives a
    # measurement procedure + the parseable Score/GL_RENDERER capture format.
    assert "GL_RENDERER" in text and "glmark2 Score" in text, (
        "CP-2 doc must specify the parseable baseline capture format"
    )
    assert "python -m bench gpu" in text, (
        "CP-2 doc must show the host bench command that evaluates the ratio"
    )


def test_cp2_doc_paths_resolve():
    _assert_docs_paths_resolve(_read(CP2), CP2)


def test_cp3_doc_carries_compute_zero_and_syscall_twelve():
    text = _read(CP3)
    # compute 0% (native speed) and syscall ~12% are the load-bearing numbers.
    assert "4.06" in text, "CP-3 doc must carry the compute ns/op (4.06 = native)"
    assert "0.00%" in text or "+0.00%" in text, "CP-3 doc must state compute +0.00%"
    assert ("11.98%" in text) or ("~12%" in text) or ("12%" in text), (
        "CP-3 doc must state the syscall ~12% overhead"
    )


def test_cp3_doc_contrasts_with_the_5pct_syscall_light_gate():
    text = _read(CP3)
    # The whole point of (c): contrast against §0 "CPU overhead < 5% (syscall-light)".
    assert ("< 5%" in text) or ("<5%" in text) or ("5%" in text), (
        "CP-3 doc must reference the §0 <5% syscall-light gate"
    )
    assert "syscall-light" in text.lower(), (
        "CP-3 doc must distinguish the syscall-light regime that the §0 gate targets"
    )
    # seccomp dispatch is the honest root cause of the residual ~12%.
    assert "seccomp" in text.lower(), (
        "CP-3 doc must attribute the residual syscall overhead to seccomp dispatch"
    )


def test_cp3_doc_paths_resolve():
    _assert_docs_paths_resolve(_read(CP3), CP3)
