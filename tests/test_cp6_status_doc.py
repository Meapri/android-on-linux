"""WS-5 guard: the CP-6 progress SSOT doc must record the round-6 CP-6 facts honestly.

`docs/research/cp6-status.md` is the single place that joins the three ADRs
(001/002/003) into one CP-6 progress flow: chromium --version device-PASS, the
M-R2 mediation-negligible verdict, the next branch (dump-dom deadlock → M-R5
svc-rewrite vs M-R1 USER_NOTIF A/B), and exec re-entry as an independent wall.

These lenient checks pin:
  - the doc exists, is HOST-ONLY, and is not a bench/perf doc,
  - it records the chromium --version device fact (Chromium 147) + M-R2 verdict
    (mediation-negligible) + the dump-dom deadlock wall,
  - it cross-references all three ADRs (001/002/003),
  - it names the next branch (M-R5 svc-rewrite vs M-R1 USER_NOTIF A/B),
  - it carries the exec re-entry / envp-propagation 급소 honestly,
  - it keeps chromium's hold honest (user 보류; implementation gated),
  - every evidence/ADR .md path token resolves on disk (no dangling citations).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "cp6-status.md"
EVIDENCE_DIR = ROOT / "docs" / "evidence"
MR2_STEM = "2026-06-02-cp6-mr2-chromium-syscall-mix"


def _text() -> str:
    assert DOC.is_file(), f"CP-6 status SSOT doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


def test_doc_exists_host_only_not_bench():
    text = _text()
    assert text.strip(), "cp6-status doc is empty"
    assert "HOST-ONLY" in text, "doc must declare it is HOST-ONLY (WS-5)"
    assert "SSOT" in text, "doc must declare itself the CP-6 progress SSOT"
    assert ("벤치" in text) or ("bench" in text.lower()), (
        "doc should clarify its relationship to bench/perf (it is NOT a bench doc)"
    )


def test_records_chromium_version_device_pass():
    text = _text()
    assert "Chromium 147" in text, "doc must record the chromium --version device fact"
    assert "--version" in text
    assert ("exit=0" in text) or ("exit 0" in text), (
        "doc must record child exit=0 for chromium --version"
    )


def test_records_mr2_mediation_negligible_verdict():
    text = _text()
    assert "M-R2" in text, "doc must name the M-R2 measurement milestone"
    assert "mediation-negligible" in text, (
        "doc must record the M-R2 verdict = mediation-negligible"
    )
    # The load-bearing numbers behind the verdict.
    assert "traps=0" in text, "verdict rests on traps=0 (zero ptrace round-trips)"


def test_records_dumpdom_deadlock_wall():
    text = _text()
    assert "--dump-dom" in text, "doc must name the render-storm trigger (--dump-dom)"
    assert ("데드락" in text) or ("deadlock" in text.lower()), (
        "doc must record the multithread-ptrace deadlock that blocks the render storm"
    )


def test_names_next_branch_svc_rewrite_vs_user_notif():
    text = _text()
    assert "M-R5" in text and "M-R1" in text, (
        "doc must name the M-R5 vs M-R1 substantive-fix branch"
    )
    assert ("svc-rewrite" in text) or ("svc rewrite" in text.lower()), (
        "doc must name the svc-rewrite candidate (M-R5)"
    )
    assert "USER_NOTIF" in text, "doc must name the USER_NOTIF candidate (M-R1)"
    assert "A/B" in text, "doc must frame the two fixes as a device A/B"


def test_cross_references_all_three_adrs():
    text = _text()
    for adr in (
        "adr-001-syscall-overhead-user-notif.md",
        "adr-002-chromium-cp6-roadmap.md",
        "adr-003-multiprocess-exec-reentry.md",
    ):
        assert adr in text, f"doc must cross-reference {adr}"
        assert (ROOT / "docs" / "design" / adr).is_file(), (
            f"cross-referenced ADR {adr} must exist on disk"
        )


def test_records_exec_reentry_as_independent_wall():
    text = _text()
    assert "exec re-entry" in text or "exec-re-entry" in text, (
        "doc must record exec re-entry"
    )
    # exec re-entry is independent of the syscall storm (ADR-003 §7).
    assert ("독립" in text) or ("independent" in text.lower()), (
        "doc must state exec re-entry is independent of the syscall storm"
    )
    # The envp-propagation 급소 (ADR-003 §4 가정-1) must be carried honestly.
    assert ("envp" in text.lower()) and ("LD_PRELOAD" in text), (
        "doc must record the execve envp-propagation 급소 (interposer re-injection)"
    )


def test_chromium_hold_is_honest():
    text = _text()
    assert "보류" in text, "doc must mark chromium as user-held (보류)"
    # The hold must not block probes, but implementation is gated.
    assert ("프로브" in text) or ("probe" in text.lower()), (
        "doc must clarify probes/measurement proceed despite the hold"
    )


def test_cites_the_mr2_evidence():
    text = _text()
    assert MR2_STEM in text, f"doc must cite the M-R2 evidence ({MR2_STEM}.md)"
    assert (EVIDENCE_DIR / f"{MR2_STEM}.md").is_file(), (
        "the cited M-R2 evidence doc must exist on disk"
    )


def test_no_dangling_doc_citations():
    """Any evidence/research/design .md path token must resolve on disk."""
    text = _text()
    for tok in sorted(set(re.findall(r"[\w./-]+\.md", text))):
        if "/" not in tok:
            # bare filename → resolve under research/ or repo root.
            assert (ROOT / "docs" / "research" / tok).is_file() or (ROOT / tok).is_file(), (
                f"doc references {tok!r} but no such file under docs/research/"
            )
            continue
        if any(seg in tok for seg in ("evidence/", "research/", "design/")):
            assert (ROOT / tok).is_file(), (
                f"cp6-status doc references {tok!r} but no such file exists"
            )
