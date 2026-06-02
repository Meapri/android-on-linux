"""WS-5 guard: the SSOT docs must reflect round-7 (v139) device truth honestly.

round-7 (`docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`, build v139,
device SM-X236N / Mali-G615 / Android 16) device-verified / device-reframed two
things that the docs must carry exactly, without overclaiming:

  - **VK-M2 render PASSES on real Mali** (G3): the round-6 clear `vkQueueSubmit`=FAIL
    is fixed — `ALR VK RENDER MARSHAL: PASS`, `submit result=VK_SUCCESS`. Root cause
    was that the marshalled VK device was created with no device extensions, so the
    AHB color-target import (`VK_ANDROID_external_memory_android_hardware_buffer`)
    was illegal; R7-A enables that device-ext + a tiler readback barrier. enumerate
    + render are both device-PASS now (remaining = ICD + textured/multi-draw).

  - **exec-re-entry (G1) is REFRAMED by hardware** — THE key finding. B-1 path-rewrite
    fires and the B-3 envp-injection decision evaluates correctly
    (`envp_reason=already`, so `envp_injected=0` is the *correct* no-op for the
    inherited-envp execs observed), but **`exec_events=0` on EVERY execve**:
    `PTRACE_EVENT_EXEC` never fires because the kernel can't complete the execve of a
    glibc-aarch64 ELF (its PT_INTERP = guest ld.so /lib/ld-linux-aarch64.so.1, which
    the Android kernel can't resolve). So B-1 + B-3 are necessary-but-NOT-sufficient
    and ADR-003's "path-rewrite + envp, NOT a loader re-map" premise is DISPROVEN: the
    real wall is the LOADER re-mapping the new ELF in-process on exec (a re-entry stub
    / loader-as-bootstrap), designed in ADR-003-v2 (parallel session). apt also needs
    full staging (`apt` top-level fails earlier with `GUEST EXEC FAIL`, traps=0).

These lenient checks pin those facts into the SSOT docs (loader-feature-gaps G1/G3,
cp6-status) + the round-7 evidence file, and pin the honesty boundaries: VK render =
VK_SUCCESS (true, device-verified), G1 = re-map wall (exec_events=0, NOT solved by
envp). They follow the existing doc-enforcement test style (test_round6_milestones_doc.py,
test_loader_feature_gaps_doc.py, test_cp6_status_doc.py). The round-6 (v138) tests are
immutable history — these additions are strictly about v139 / the G1 re-map reframe.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / "docs" / "research"
EVIDENCE_DIR = ROOT / "docs" / "evidence"
DESIGN_DIR = ROOT / "docs" / "design"

GAPS = RESEARCH / "loader-feature-gaps.md"
CP6 = RESEARCH / "cp6-status.md"

ROUND7_STEM = "2026-06-02-round7-vkrender-pass-drain"


def _read(p: Path) -> str:
    assert p.is_file(), f"SSOT doc missing: {p}"
    return p.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# 1. The round-7 evidence file must exist and carry the two device facts:
#    VK submit=VK_SUCCESS, and the exec_events=0 re-map finding.
# --------------------------------------------------------------------------- #

def test_round7_evidence_file_exists_and_nonempty():
    ev = EVIDENCE_DIR / f"{ROUND7_STEM}.md"
    assert ev.is_file(), f"round-7 evidence doc must exist on disk: {ev}"
    assert ev.read_text(encoding="utf-8").strip(), "round-7 evidence doc is empty"


def test_round7_evidence_records_vk_render_success():
    """Evidence must record VK-M2 render PASS with clear vkQueueSubmit=VK_SUCCESS."""
    text = _read(EVIDENCE_DIR / f"{ROUND7_STEM}.md")
    assert "ALR VK RENDER MARSHAL: PASS" in text, (
        "evidence must record the VK render marshal PASS marker"
    )
    assert "VK_SUCCESS" in text, (
        "evidence must record the clear vkQueueSubmit=VK_SUCCESS (the round-6 FAIL fix)"
    )
    assert "device created=yes" in text, "evidence must record VK device created=yes"
    # The AHB device-extension fix is the load-bearing root cause.
    assert "VK_ANDROID_external_memory_android_hardware_buffer" in text, (
        "evidence must name the AHB device-ext that made the clear submit legal"
    )


def test_round7_evidence_records_exec_events_zero_remap_finding():
    """Evidence must record exec_events=0 + that B-1/B-3 fire but execve never completes."""
    text = _read(EVIDENCE_DIR / f"{ROUND7_STEM}.md")
    assert "exec_events=0" in text, (
        "evidence must record exec_events=0 (PTRACE_EVENT_EXEC never fires)"
    )
    # B-3 envp decision evaluated correctly (no injection needed for inherited envp).
    assert "envp_reason=already" in text, (
        "evidence must record the B-3 envp decision (envp_reason=already, correct no-op)"
    )
    # B-1 path-rewrite still fires.
    assert "reason=rewrite" in text, "evidence must record B-1 path-rewrite still fires"
    # The honest boundary: apt still fails / chain not done.
    assert "unpacked=false" in text, (
        "evidence must keep the apt-install unpacked=false wall honest"
    )


# --------------------------------------------------------------------------- #
# 2. loader-feature-gaps: G3 = VK render PASS/VK_SUCCESS; G1 = re-map reframe.
# --------------------------------------------------------------------------- #

def test_gaps_cites_round7_evidence():
    text = _read(GAPS)
    assert ROUND7_STEM in text, (
        f"loader-feature-gaps must cite the round-7 evidence ({ROUND7_STEM}.md)"
    )
    assert (EVIDENCE_DIR / f"{ROUND7_STEM}.md").is_file()


def test_gaps_g3_reflects_vk_render_pass_success():
    """G3 must record VK-M2 render device-verified with clear submit=VK_SUCCESS."""
    text = _read(GAPS)
    assert "VK_SUCCESS" in text, (
        "G3 must record the clear vkQueueSubmit=VK_SUCCESS (round-7 render PASS)"
    )
    # It must be tied to the VK-M2 render milestone being device-verified.
    assert "VK-M2" in text and "render" in text.lower(), (
        "G3 must frame the VK_SUCCESS against the VK-M2 render milestone"
    )
    # Must not overclaim: G3 stays PARTIAL (ICD + textured/multi-draw remain).
    assert re.search(r"G3[^\n]*PARTIAL", text) or re.search(r"PARTIAL[^\n]*G3", text), (
        "G3 must stay PARTIAL — render PASSES but ICD + textured/multi-draw remain"
    )


def test_gaps_g1_states_remap_exec_events_zero_finding():
    """G1 must be reframed: B-1+B-3 fire but exec_events=0 => loader re-map is the wall."""
    text = _read(GAPS)
    assert "exec_events=0" in text, (
        "G1 must record the device finding exec_events=0 (execve never completes)"
    )
    # The reframe: the wall is an in-process re-map of the new ELF, not path+envp.
    assert ("재-맵" in text) or ("re-map" in text.lower()) or ("remap" in text.lower()), (
        "G1 must reframe the wall as an in-process re-map of the new ELF (re-entry stub)"
    )
    assert ("re-entry stub" in text) or ("loader-as-bootstrap" in text.lower()), (
        "G1 must name the re-entry stub / loader-as-bootstrap as the real mechanism"
    )
    # The kernel can't complete the execve of a glibc ELF (PT_INTERP = guest ld.so).
    assert ("PT_INTERP" in text) or ("ld-linux-aarch64" in text), (
        "G1 must record why: the kernel can't resolve the glibc ELF's guest ld.so PT_INTERP"
    )
    # B-1 + B-3 are necessary but NOT sufficient.
    assert "B-1" in text and "B-3" in text, (
        "G1 must keep B-1 (path-rewrite) and B-3 (envp) named — both fire, both insufficient"
    )
    assert ("sufficient" in text.lower()) or ("불충분" in text), (
        "G1 must state B-1+B-3 are necessary-but-NOT-sufficient"
    )
    # apt still needs full staging (honest: it is not solved by the re-map alone).
    assert "unpacked=false" in text, (
        "G1 must keep the apt-install unpacked=false wall honest"
    )
    assert ("staging" in text.lower()) or ("스테이징" in text), (
        "G1 must note apt needs full staging (apt is only minimally staged)"
    )


def test_gaps_g1_cross_references_adr_003_v2_without_dangling_citation():
    """G1 must reference ADR-003-v2 (the re-map design, owned by the parallel session)."""
    text = _read(GAPS)
    assert "ADR-003-v2" in text, (
        "G1 must cross-reference ADR-003-v2 (the loader re-map design)"
    )
    # The parallel session owns it; this doc references the path only. It may not yet
    # exist on disk — so the reference must NOT be a full *.md path token (that would
    # trip the dangling-citation guard). Guard: no adr-003-v2*.md token that fails to
    # resolve on disk.
    for tok in re.findall(r"adr-003-v2[\w./-]*\.md", text):
        assert (DESIGN_DIR / Path(tok).name).is_file(), (
            f"G1 references {tok!r} as a path but it does not resolve on disk — "
            "reference ADR-003-v2 by id (no .md path) until the parallel session lands it"
        )


# --------------------------------------------------------------------------- #
# 3. cp6-status: chromium multiprocess is gated on the same re-map.
# --------------------------------------------------------------------------- #

def test_cp6_cites_round7_evidence():
    text = _read(CP6)
    assert ROUND7_STEM in text, (
        f"cp6-status must cite the round-7 evidence ({ROUND7_STEM}.md)"
    )


def test_cp6_records_remap_finding_and_chromium_gate():
    """cp6-status must carry the exec_events=0 re-map finding + the chromium gate."""
    text = _read(CP6)
    assert "exec_events=0" in text, (
        "cp6-status must record exec_events=0 (the execve-completion wall)"
    )
    assert "ADR-003-v2" in text, (
        "cp6-status must cross-reference ADR-003-v2 (the loader re-map design)"
    )
    # chromium's multiprocess (zygote/gpu fresh-execve) is gated on the same re-map.
    assert "chromium" in text.lower(), "cp6-status must name chromium"
    assert ("재-맵" in text) or ("re-map" in text.lower()) or ("remap" in text.lower()), (
        "cp6-status must state chromium multiprocess is gated on the loader re-map"
    )
    # B-3 evaluated but did not change the outcome (honest: not the blocker).
    assert "envp_reason=already" in text or "B-3" in text, (
        "cp6-status must record the B-3 envp decision context"
    )


def test_cp6_adr003_v2_reference_has_no_dangling_md_path():
    """ADR-003-v2 may not exist on disk yet (parallel session); reference by id only."""
    text = _read(CP6)
    for tok in re.findall(r"adr-003-v2[\w./-]*\.md", text):
        assert (DESIGN_DIR / Path(tok).name).is_file(), (
            f"cp6-status references {tok!r} as a path but it does not resolve on disk"
        )


# --------------------------------------------------------------------------- #
# 4. No-regression: the round-6 (v138) facts must remain in the docs (immutable).
# --------------------------------------------------------------------------- #

def test_round6_history_preserved_in_gaps():
    """The round-7 update must not erase round-6 history (v138) from loader-feature-gaps."""
    text = _read(GAPS)
    assert "2026-06-02-round6-qt6-execreentry-vkrender" in text, (
        "loader-feature-gaps must still cite the round-6 evidence (immutable history)"
    )
    # G2 stays DONE (Qt6 wl_shm) from round-6.
    assert "wl_shm" in text and ("DONE" in text), (
        "round-6 G2 DONE (Qt6 wl_shm) must be preserved"
    )
