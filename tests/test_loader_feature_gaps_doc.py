"""WS-5 guard: the loader-feature-gaps SSOT doc must enumerate the blocked big
loader features honestly, with exec-re-entry as the top-leverage item.

`docs/research/loader-feature-gaps.md` is the single place that answers "which big
loader features are currently blocked, what does each unlock, and how hard / what
does it depend on". Sibling docs (gui-universality-status.md, alr-compat-matrix.md)
point *here* instead of re-describing the same gap in every cell.

These lenient checks pin:
  - the five big gaps are each present (exec-re-entry / Qt6 closure / Vulkan render
    pipeline / netsurf network+input / GLES3 full scene),
  - exec-re-entry is called out as the highest-leverage item,
  - each gap names what it unlocks + a difficulty + a dependency,
  - the doc cites real on-disk evidence (round-4 drain#13 specifically) and has no
    dangling research/evidence citations,
  - it stays HOST-ONLY / not a bench doc,
  - chromium / CP-6 is explicitly held (user 보류), NOT listed as an active gap.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "loader-feature-gaps.md"
EVIDENCE_DIR = ROOT / "docs" / "evidence"
RESEARCH_DIR = ROOT / "docs" / "research"
ROUND4_STEM = "2026-06-02-round4-milestones-drain"

# The five blocked big features this SSOT must enumerate (substring per row).
BIG_GAPS = (
    "exec-re-entry",   # G1 — apt/dpkg + GIMP plugin fork+exec
    "Qt6",             # G2 — wayland closure
    "Vulkan",          # G3 — render pipeline
    "netsurf",         # G4 — network/input interaction
    "GLES3",           # G5 — full scene coverage
)


def _text() -> str:
    assert DOC.is_file(), f"loader-feature-gaps SSOT doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


def test_doc_exists_and_is_host_only_not_bench():
    text = _text()
    assert text.strip(), "loader-feature-gaps doc is empty"
    assert "HOST-ONLY" in text, "doc must declare it is HOST-ONLY (WS-5)"
    # Honest self-description: not a bench/perf doc.
    assert ("벤치" in text) or ("bench" in text.lower()), (
        "doc should clarify its relationship to bench/perf (it is NOT a bench doc)"
    )
    # It is an SSOT: it must say so, so sibling docs can point here.
    assert "SSOT" in text, "doc must declare itself the loader-gaps SSOT"


def test_all_five_big_gaps_present():
    text = _text()
    missing = [g for g in BIG_GAPS if g not in text]
    assert not missing, (
        f"loader-feature-gaps doc must enumerate all big gaps; missing: {missing}"
    )


def test_exec_re_entry_is_highest_leverage():
    """exec-re-entry must be flagged the top-leverage item (apt/dpkg + GIMP plugin)."""
    text = _text()
    assert "exec-re-entry" in text
    # It must be tied to the high-leverage unlocks.
    assert "apt" in text and "dpkg" in text, (
        "exec-re-entry must record that it unlocks apt/dpkg install"
    )
    assert ("GIMP plugin" in text) or ("GIMP" in text and "plugin" in text), (
        "exec-re-entry must record that it unlocks GIMP plugin fork+exec"
    )
    # It must be named the highest-leverage gap (Korean 레버리지 or English leverage).
    assert ("레버리지" in text) or ("leverage" in text.lower()), (
        "doc must frame the gaps by leverage (which one unlocks the most)"
    )
    # G1 / exec-re-entry must be listed first in the leverage ordering: its first
    # mention should precede the first mention of every other gap.
    first_exec = text.index("exec-re-entry")
    for other in ("Qt6", "Vulkan", "netsurf", "GLES3"):
        assert first_exec < text.index(other), (
            f"exec-re-entry must lead the leverage ordering, before {other}"
        )


def test_each_gap_states_unlock_difficulty_dependency():
    """The doc as a whole must carry the unlock / difficulty / dependency framing."""
    text = _text()
    assert ("잠금해제" in text) or ("unlock" in text.lower()), (
        "doc must state what each gap unlocks"
    )
    assert ("난이도" in text) or ("difficulty" in text.lower()), (
        "doc must state a difficulty for the gaps"
    )
    assert ("의존" in text) or ("depend" in text.lower()), (
        "doc must state dependencies for the gaps"
    )


def test_cites_round4_drain13_evidence():
    text = _text()
    assert ROUND4_STEM in text, (
        "loader-feature-gaps doc must cite the round-4 drain#13 evidence "
        f"({ROUND4_STEM}.md) — the device source for the exec-re-entry / qt6 walls"
    )
    assert (EVIDENCE_DIR / f"{ROUND4_STEM}.md").is_file(), (
        "the cited round-4 drain#13 evidence doc must exist on disk"
    )


def test_cites_at_least_three_real_evidence_stems():
    text = _text()
    stems = {p.stem for p in EVIDENCE_DIR.glob("*.md")}
    cited = {s for s in stems if len(s) >= 6 and s in text}
    assert len(cited) >= 3, (
        "loader-feature-gaps doc appears ungrounded: expected >=3 real evidence "
        f"stems cited, found {len(cited)}: {sorted(cited)}"
    )


def test_no_dangling_doc_citations():
    """Any evidence/research .md path token in the doc must resolve on disk."""
    text = _text()
    for tok in sorted(set(re.findall(r"[\w./-]+\.md", text))):
        # Bare filenames (e.g. `gui-universality-status.md`) resolve under research/.
        if "/" not in tok:
            assert (RESEARCH_DIR / tok).is_file() or (ROOT / tok).is_file(), (
                f"doc references {tok!r} but no such file exists under docs/research/"
            )
            continue
        if "evidence/" in tok or "research/" in tok:
            assert (ROOT / tok).is_file(), (
                f"loader-feature-gaps doc references {tok!r} but no such file exists"
            )


def test_chromium_cp6_is_held_not_an_active_gap():
    """chromium / CP-6 is user-held; it must be excluded from the active gap list."""
    text = _text()
    # The doc must acknowledge the hold explicitly.
    assert ("보류" in text) or ("held" in text.lower()), (
        "doc must explicitly mark chromium/CP-6 as held (user 보류)"
    )
    assert ("chromium" in text.lower()) or ("CP-6" in text), (
        "doc must name chromium/CP-6 when stating it is out of scope"
    )
    # The leverage table (#0) lists G1..G5; chromium must NOT be promoted into a G#.
    # Pin that no "G6" gap-id row exists (the five gaps are the scope). Match a gap-id
    # token (bold or word-boundary "G6"), not the "G6" inside "Mali-G615".
    assert not re.search(r"\*\*G6\b|\bG6\b(?!\d)", text), (
        "chromium/CP-6 must not be promoted into a sixth active gap (it is held)"
    )
    # And the five real gap ids must each appear as a section/row id.
    for gid in ("G1", "G2", "G3", "G4", "G5"):
        assert re.search(rf"\b{gid}\b", text), f"gap id {gid} must be present"
