"""WS-5 guard: the round-4 drain#13 (v135) milestones must be recorded honestly
across the SSOT docs (gui-universality + compat-matrix + loader-feature-gaps).

Source of truth: docs/evidence/2026-06-02-round4-milestones-drain.md
  - SDL2 testdraw2 RENDERS (rendered=true frames=2217->2218),
  - qt6 analogclock SIGSEGVs in Qt init (guest child, NOT an app regression),
  - apt/dpkg install blocked on exec-re-entry (GUEST EXEC FAIL),
  - Vulkan enumerate/props marshalling backbone host-verified (device wiring pending),
  - no app regression (netsurf/foot/gtkdemo render, glmark2 ~1000+, SCREEN CUBE PASS).

These lenient cross-doc checks pin that each milestone landed in the right doc with
the right honesty (RENDERS only where device-proved; SIGSEGV/exec-re-entry/Vulkan
backbone honestly PENDING) and that the round-4 evidence is actually cited.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / "docs" / "research"
GUI = RESEARCH / "gui-universality-status.md"
MATRIX = RESEARCH / "alr-compat-matrix.md"
GAPS = RESEARCH / "loader-feature-gaps.md"
ROUND4_STEM = "2026-06-02-round4-milestones-drain"
EVIDENCE = ROOT / "docs" / "evidence" / f"{ROUND4_STEM}.md"


def _read(p: Path) -> str:
    assert p.is_file(), f"doc missing: {p}"
    return p.read_text(encoding="utf-8")


def test_round4_evidence_exists():
    assert EVIDENCE.is_file(), f"round-4 drain#13 evidence missing: {EVIDENCE}"
    body = EVIDENCE.read_text(encoding="utf-8")
    # The evidence itself must carry the milestone markers the docs cite.
    assert "sdl2gui-result: rendered=true" in body
    assert "testdraw2" in body
    assert "SIGSEGV" in body
    assert "GUEST EXEC FAIL" in body
    assert "Vulkan" in body


def test_gui_doc_promotes_sdl2_with_round4_evidence():
    text = _read(GUI)
    assert ROUND4_STEM in text, "gui-universality doc must cite the round-4 evidence"
    # SDL2 must be in the device-proven set as RENDERS with the testdraw2 proof.
    sdl_rows = [
        ln for ln in text.splitlines()
        if "SDL2" in ln and "RENDERS" in ln and "testdraw2" in ln
    ]
    assert sdl_rows, (
        "gui-universality doc must promote SDL2 (testdraw2) to RENDERS in the "
        "device-proven set"
    )
    assert any("rendered=true" in ln for ln in sdl_rows), (
        "SDL2 RENDERS row must carry the rendered=true device proof"
    )


def test_matrix_records_vulkan_backbone_host_verified():
    """Vulkan enumerate/props marshalling backbone is host-verified, device pending."""
    text = _read(MATRIX)
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if "Vulkan" in ln and ("marshal" in ln.lower() or "마샬" in ln)
    ]
    assert rows, "matrix must carry a Vulkan marshalling backbone row"
    joined = " ".join(rows)
    assert ("HOST-VERIFIED" in joined) or ("host-verified" in joined.lower()), (
        "Vulkan backbone row must record it is host-verified"
    )
    # Honest: it must NOT claim a device render/PASS yet (device wiring pending).
    assert "PASS on real Mali" not in joined, (
        "Vulkan backbone must not overclaim a real-Mali device PASS yet"
    )
    assert any(
        ("PENDING" in ln) or ("진행중" in ln) or ("pending" in ln.lower())
        for ln in lines
        if "Vulkan" in ln
    ), "Vulkan rows must keep an honest device-pending caveat"


def test_matrix_qt6_sigsegv_is_not_an_app_regression():
    """qt6 row must record the Qt-init SIGSEGV and that the app did NOT regress."""
    text = _read(MATRIX)
    lines = text.splitlines()
    rows = [ln for ln in lines if "Qt6" in ln and "SIGSEGV" in ln]
    assert rows, "matrix Qt6 row must record the Qt-init SIGSEGV"
    joined = " ".join(rows)
    assert ("회귀 아님" in joined) or ("not an app regression" in joined.lower()) or (
        "앱 회귀 아님" in joined
    ), "Qt6 SIGSEGV must be qualified as NOT an app regression (guest child crash)"


def test_loader_gaps_ties_exec_re_entry_to_apt_and_gimp():
    """The gaps SSOT must tie exec-re-entry to BOTH apt/dpkg and GIMP plugins."""
    text = _read(GAPS)
    assert "exec-re-entry" in text
    assert "apt" in text and "dpkg" in text, (
        "loader-gaps must attribute apt/dpkg install to exec-re-entry"
    )
    assert "GIMP" in text and "plugin" in text, (
        "loader-gaps must attribute GIMP plugin fork+exec to exec-re-entry"
    )


def test_sibling_docs_point_at_loader_gaps_ssot():
    """gui-universality + compat-matrix must reference the loader-gaps SSOT by name."""
    for doc in (GUI, MATRIX):
        text = _read(doc)
        assert "loader-feature-gaps.md" in text, (
            f"{doc.name} must point at the loader-feature-gaps.md SSOT instead of "
            "re-describing each gap"
        )
