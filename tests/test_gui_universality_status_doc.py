"""WS-5 guard: the GUI-universality SSOT doc must list the device-proven GUI set.

`docs/research/gui-universality-status.md` is the single place that answers
"which Linux GUI apps actually appear on the device". It is a *render* doc, not a
*bench* doc. These lenient checks pin:
  - the device-proven universal-GUI set (GIMP / gtk3-widget-factory / gtk3-demo /
    foot / netsurf-gtk) is each present with a render/usable status,
  - each cites a real on-disk evidence doc,
  - the still-pending toolkits (qt6/sdl2) are listed honestly (STAGED / 진행중),
    NOT promoted to RENDERS,
  - the doc stays a render doc (does not masquerade as a perf/bench doc).
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "gui-universality-status.md"
EVIDENCE_DIR = ROOT / "docs" / "evidence"

# The device-proven universal GUI set this doc must enumerate.
PROVEN_GUI = ("GIMP", "gtk3-widget-factory", "gtk3-demo", "foot", "netsurf")

# Still-pending toolkits that must NOT be marked RENDERS/USABLE.
PENDING_TOOLKITS = ("Qt6", "SDL2")


def _text() -> str:
    assert DOC.is_file(), f"GUI-universality SSOT doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


def _evidence_stems() -> set[str]:
    return {p.stem for p in EVIDENCE_DIR.glob("*.md")}


def test_doc_exists_and_is_render_not_bench():
    text = _text()
    assert text.strip(), "GUI-universality doc is empty"
    # It must contain the render vocabulary and not pretend to be a perf/bench doc.
    assert "RENDERS" in text, "doc must use the RENDERS status vocabulary"
    # Honest self-description: explicitly *not* a bench doc.
    assert ("벤치" in text) or ("bench" in text.lower()), (
        "doc should clarify its relationship to bench/perf (it is NOT a bench doc)"
    )


def test_doc_lists_all_proven_gui_apps():
    text = _text()
    missing = [app for app in PROVEN_GUI if app not in text]
    assert not missing, (
        f"GUI-universality doc must list the device-proven GUI set; missing: {missing}"
    )


def test_proven_gui_apps_carry_render_status():
    """Each proven app must appear on a line with a RENDERS or USABLE status."""
    text = _text()
    lines = text.splitlines()
    for app in PROVEN_GUI:
        status_lines = [
            ln for ln in lines
            if app in ln and ("RENDERS" in ln or "USABLE" in ln)
        ]
        assert status_lines, (
            f"{app} must appear on a line marked RENDERS or USABLE in the "
            "device-proven GUI set"
        )


def test_doc_cites_real_evidence_corpus():
    """The doc must cite real on-disk evidence stems (>= 4 distinct), incl. netsurf."""
    text = _text()
    stems = _evidence_stems()
    assert stems, f"no evidence docs found under {EVIDENCE_DIR}"

    cited = {s for s in stems if len(s) >= 6 and s in text}
    assert len(cited) >= 4, (
        "GUI-universality doc appears ungrounded: expected >=4 real evidence stems "
        f"cited, found {len(cited)}: {sorted(cited)}"
    )
    # The browser milestone evidence specifically must be cited.
    assert "2026-06-02-netsurf-browser-renders" in cited, (
        "doc must cite the netsurf browser render evidence (drain#12)"
    )


def test_no_dangling_evidence_citations():
    """Any evidence/research .md path token in the doc must resolve on disk."""
    import re
    text = _text()
    for tok in sorted(set(re.findall(r"[\w./-]+\.md", text))):
        if "evidence/" in tok or "research/" in tok:
            assert (ROOT / tok).is_file(), (
                f"GUI-universality doc references {tok!r} but no such file exists"
            )


def test_pending_toolkits_not_overclaimed():
    """qt6/sdl2 must be listed as STAGED / in-progress, never RENDERS/USABLE."""
    text = _text()
    lines = text.splitlines()
    for toolkit in PENDING_TOOLKITS:
        # The doc should mention them in the in-progress section.
        assert toolkit in text, f"doc should account for the pending toolkit {toolkit}"
        for ln in lines:
            if toolkit not in ln:
                continue
            # A pending toolkit line must not assert a device render.
            assert "RENDERS" not in ln and "USABLE" not in ln, (
                f"pending toolkit {toolkit} is overclaimed as RENDERS/USABLE: {ln!r}"
            )


def test_netsurf_recorded_as_browser_milestone():
    """netsurf must be present as a web browser with its 5-thread render detail."""
    text = _text()
    lines = text.splitlines()
    rows = [ln for ln in lines if "netsurf" in ln and "RENDERS" in ln]
    assert rows, "doc must mark netsurf-gtk as RENDERS"
    joined = " ".join(rows)
    assert "rendered=true" in joined, "netsurf row must carry rendered=true proof"
    assert "5" in joined and "thread" in joined.lower(), (
        "netsurf row must record the 5 guest threads (multithreaded browser)"
    )
