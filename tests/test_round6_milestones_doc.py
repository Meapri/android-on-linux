"""WS-5 guard: the SSOT docs must reflect round-6 (v138) device truth honestly.

round-6 (`docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`, build v138,
device SM-X236N / Mali-G615 / Android 16) device-verified three things, each with a
distinct honesty boundary that the docs must NOT overclaim:

  - **Qt6 RENDERS** via wl_shm (rendered=true, frames 2215->2216) — the r4/r5
    Qt-init SIGSEGV (EGL hwintegration, ICD-less eglGetDisplay) was fixed by
    excluding the EGL QPA/HwIntegration plugins + QT_WAYLAND_DISABLE_HW_INTEGRATION=1.
    This is DONE: Qt6 joins the device-proven universal GUI set (now 7 toolkits).
  - **exec-re-entry B-1 device-fires** (ADR-003): execve(/bin/sh) trapped at
    EVENT_SECCOMP, program path (x0) rewritten into the rootfs
    (`alr exec x0=/bin/sh reason=rewrite`, traps=1 rewrites=1). STILL INCOMPLETE:
    `apt-install: unpacked=false` — the full dpkg fork+exec maintainer-script chain
    needs B-3 (child envp re-injection of LD_PRELOAD/ALR_ROOTFS), in flight round-7.
  - **VK-M2 render: device created=yes** on real Mali (vkCreateDevice + queue +
    cmdpool marshalled) but the clear vkQueueSubmit **FAILS** (render-pass /
    image-layout / AHB-import setup). NOT done — clear-submit fix is round-7.

These lenient checks pin those facts into the four SSOT docs and, crucially, pin the
honesty boundaries: Qt6 = RENDERS (true), VK render submit = FAILS (not done), dpkg
apt-install = still fails. They follow the existing doc-enforcement test style
(test_compat_matrix_netsurf_renders.py, test_loader_feature_gaps_doc.py, etc.).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / "docs" / "research"
EVIDENCE_DIR = ROOT / "docs" / "evidence"

MATRIX = RESEARCH / "alr-compat-matrix.md"
GAPS = RESEARCH / "loader-feature-gaps.md"
GUI = RESEARCH / "gui-universality-status.md"
CP6 = RESEARCH / "cp6-status.md"

ROUND6_STEM = "2026-06-02-round6-qt6-execreentry-vkrender"


def _read(p: Path) -> str:
    assert p.is_file(), f"SSOT doc missing: {p}"
    return p.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# 1. The round-6 evidence file must exist and carry the three device facts.
# --------------------------------------------------------------------------- #

def test_round6_evidence_file_exists():
    ev = EVIDENCE_DIR / f"{ROUND6_STEM}.md"
    assert ev.is_file(), (
        f"round-6 evidence doc must exist on disk: {ev}"
    )
    assert ev.read_text(encoding="utf-8").strip(), "round-6 evidence doc is empty"


def test_round6_evidence_records_three_milestones():
    """The evidence doc must honestly state Qt6 renders, exec B-1 fires, VK submit fails."""
    text = _read(EVIDENCE_DIR / f"{ROUND6_STEM}.md")
    # Qt6 renders via wl_shm.
    assert "rendered=true" in text and "2215" in text and "2216" in text, (
        "evidence must record Qt6 rendered=true frames 2215->2216"
    )
    # exec-re-entry B-1 path-rewrite fires.
    assert "x0=/bin/sh" in text and "reason=rewrite" in text, (
        "evidence must record the exec-re-entry x0 path-rewrite firing"
    )
    # VK render device created but submit fails (the honest boundary).
    assert "device created=yes" in text, "evidence must record VK device created=yes"
    assert ("submit result=fail" in text) or ("submit=fail" in text.lower()), (
        "evidence must record the VK clear-submit FAIL (not a render success)"
    )


# --------------------------------------------------------------------------- #
# 2. compat-matrix: Qt6 RENDERS, exec B-1/B-3, VK device-created/submit-fail.
# --------------------------------------------------------------------------- #

def test_matrix_cites_round6_evidence():
    text = _read(MATRIX)
    assert ROUND6_STEM in text, (
        f"compat matrix must cite the round-6 evidence ({ROUND6_STEM}.md)"
    )
    assert (EVIDENCE_DIR / f"{ROUND6_STEM}.md").is_file()


def test_matrix_qt6_is_renders_via_shm():
    """Qt6 row must be RENDERS with the rendered=true frame proof + wl_shm note."""
    text = _read(MATRIX)
    rows = [ln for ln in text.splitlines() if "Qt6" in ln and "|" in ln and "RENDERS" in ln]
    assert rows, "matrix must carry a Qt6 RENDERS row (round-6)"
    joined = " ".join(rows)
    assert "rendered=true" in joined, "Qt6 RENDERS row must carry rendered=true proof"
    assert ("wl_shm" in joined) or ("SHM" in joined) or ("shm" in joined), (
        "Qt6 RENDERS row must record it renders via the wl_shm backing store"
    )
    # Must not overclaim it as EGL/hardware-accelerated (it is software wl_shm).
    assert "USABLE" not in joined, (
        "Qt6 is RENDERS (analogclock demo), not yet USABLE — do not overclaim"
    )


def test_matrix_vk_render_device_created_submit_pending():
    """VK render row must say device created=yes but clear-submit FAIL — not done."""
    text = _read(MATRIX)
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if ("VK-M2" in ln or "vkCreateDevice" in ln or "VK RENDER" in ln)
        and "device created=yes" in ln
    ]
    assert rows, (
        "matrix must carry a VK-M2 render row recording device created=yes"
    )
    joined = " ".join(rows)
    # The honest boundary: the clear submit FAILS; it must NOT be marked DEVICE-VERIFIED.
    assert "FAIL" in joined or "fail" in joined, (
        "VK render row must record the clear vkQueueSubmit FAIL (not a success)"
    )
    assert "DEVICE-VERIFIED" not in joined, (
        "VK render row must not claim DEVICE-VERIFIED — the clear-submit still fails"
    )


def test_matrix_apt_install_still_fails_b3_in_flight():
    """apt install stays WALL: B-1 fires but full dpkg chain needs B-3 (round-7)."""
    text = _read(MATRIX)
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if "apt install" in ln and "exec-re-entry" in ln
    ]
    assert rows, "matrix must keep an apt-install exec-re-entry WALL row"
    joined = " ".join(rows)
    assert "B-1" in joined and "B-3" in joined, (
        "apt-install row must record B-1 device-fires + B-3 in-flight"
    )
    # Honest: it must NOT claim apt install works.
    assert "RUNS" not in joined and "USABLE" not in joined, (
        "apt install must stay a WALL — B-3 (child envp re-injection) is not done"
    )


# --------------------------------------------------------------------------- #
# 3. loader-feature-gaps: G1 PARTIAL (B-1 fires), G2 DONE, G3 submit-pending.
# --------------------------------------------------------------------------- #

def test_gaps_cites_round6_evidence():
    text = _read(GAPS)
    assert ROUND6_STEM in text, (
        f"loader-feature-gaps must cite the round-6 evidence ({ROUND6_STEM}.md)"
    )


def test_gaps_g2_qt6_marked_done():
    """G2 (Qt6 wl_shm) must be marked DONE after the round-6 device render."""
    text = _read(GAPS)
    # The G2 leverage-table row and section must carry DONE.
    assert re.search(r"G2[^\n]*DONE", text) or re.search(r"DONE[^\n]*G2", text), (
        "G2 (Qt6 wl_shm) must be marked DONE in the leverage table"
    )
    assert "wl_shm" in text, "G2 resolution must name the wl_shm backing store"


def test_gaps_g1_exec_reentry_b1_fires_b3_in_flight():
    """G1 must record B-1 execve path-rewrite device-fires + B-3 still in-flight."""
    text = _read(GAPS)
    assert "B-1" in text and "B-3" in text, (
        "G1 must distinguish B-1 (execve x0 path-rewrite) from B-3 (child envp re-inject)"
    )
    assert "reason=rewrite" in text, (
        "G1 must record the device proof that B-1 fires (alr exec x0 reason=rewrite)"
    )
    # Honest: apt-install / full chain must still be unfinished.
    assert "unpacked=false" in text, (
        "G1 must keep the apt-install unpacked=false device wall honest (B-3 pending)"
    )


def test_gaps_g3_vk_device_created_submit_fails():
    """G3 must record VK-M2 device created=yes but clear-submit FAIL (not done)."""
    text = _read(GAPS)
    assert "device created=yes" in text, (
        "G3 must record VK-M2 device created=yes on real Mali"
    )
    # The clear submit fails — G3 stays PARTIAL, not DONE.
    assert re.search(r"submit[^\n]*FAIL", text) or "vkQueueSubmit`=FAIL" in text or (
        "submit" in text.lower() and "FAIL" in text
    ), "G3 must record the clear vkQueueSubmit FAIL"
    assert re.search(r"G3[^\n]*PARTIAL", text) or re.search(r"PARTIAL[^\n]*G3", text), (
        "G3 must stay PARTIAL (device-created but submit fails), not DONE"
    )


# --------------------------------------------------------------------------- #
# 4. gui-universality + cp6-status round-6 sync.
# --------------------------------------------------------------------------- #

def test_gui_universality_lists_seven_toolkit_set():
    """The GUI doc must record the 7-toolkit set with Qt6 RENDERS."""
    text = _read(GUI)
    assert ROUND6_STEM in text, "gui-universality must cite the round-6 Qt6 evidence"
    assert "7" in text and ("toolkit" in text.lower()), (
        "gui-universality must record the universal GUI set is now 7 toolkits"
    )
    rows = [ln for ln in text.splitlines() if "Qt6" in ln and "RENDERS" in ln]
    assert rows, "gui-universality must mark Qt6 as RENDERS in the proven set"


def test_cp6_status_records_b1_device_fires():
    """cp6-status must record exec-re-entry B-1 device-fires + B-3 round-7."""
    text = _read(CP6)
    assert ROUND6_STEM in text, "cp6-status must cite the round-6 evidence"
    assert "B-1" in text and "device-fires" in text, (
        "cp6-status must record B-1 execve path-rewrite device-fires (round-6)"
    )
    assert "B-3" in text, (
        "cp6-status must keep the B-3 child envp re-injection wall honest (round-7)"
    )


def test_no_dangling_round6_citations():
    """Every doc that cites the round-6 stem must resolve it on disk (it exists)."""
    assert (EVIDENCE_DIR / f"{ROUND6_STEM}.md").is_file()
    for doc in (MATRIX, GAPS, GUI, CP6):
        text = _read(doc)
        if ROUND6_STEM in text:
            # Already asserted the file exists; this guards against typos in the stem.
            assert f"{ROUND6_STEM}.md" in text or ROUND6_STEM in text
