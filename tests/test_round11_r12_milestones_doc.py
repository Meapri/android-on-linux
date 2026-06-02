"""WS-5 guard: the SSOT docs must reflect the round-11 (v144) SIGILL fix and the R12
6-lane in-flight arc honestly — device-proven facts intact, in-flight lanes NOT marked DONE.

This is the arc on top of the in-process-remap conquest:
  - **R11** (`docs/evidence/2026-06-02-round11-remap-sigill-fixed.md`, v144): the re-mapped
    static glibc guest SIGILL (round-10's remaining gate) is FIXED — root cause was
    map_elf_image re-mmapping each PT_LOAD with MAP_FIXED, clobbering a shared boundary page;
    fix = single-span anon mapping + per-segment memcpy + per-page union mprotect + real
    AT_HWCAP into the IRELATIVE resolver. The in-process re-map MAPPER is now correct
    (no signal 4, sp_align=ok, bss_zeroed, entry=0x400640). But `inproc` stays opt-in
    (default-OFF): global ON wedges the serialized onCreate probe sequence after PERF. So G1
    is NOT promoted to RUNS — what remains is sequence-level integration, not a mapper unknown.

  - **R12 (in-flight)** : on the v144 foundation, six lanes run in parallel —
    g1-seqint (inproc-redirect scoping + /proc/self/exe pass-through + apt fakeroot),
    g3-vk (textured/multi-draw toward ICD), g5-gles3 (GLES3 scene coverage),
    g4-input (GUI text input modifiers/repeat/keymap), apt-fakeroot (non-root dpkg superuser),
    docs. ALL six are device-pending — none promoted to a cell by device verification yet.
    The single WS-1 drain checklist + per-lane device-req gates live in the new SSOT
    `docs/research/r12-remaining-gaps-status.md`.

These lenient checks pin: the R11 SIGILL-fix facts into the evidence + loader-feature-gaps G1;
the new r12 status doc exists and lists all six lanes with device-req gates; loader-feature-gaps
carries the four `(R12 in-flight)` notes under G1/G3/G4/G5 without marking any of them DONE.
They follow the existing doc-enforcement test style (test_round9_round10_milestones_doc.py,
test_loader_feature_gaps_doc.py). The round-6 (v138) / round-7 (v139) / round-9-10 (v140-v143)
history tests are immutable — these additions are strictly about the v144 / R12 arc.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / "docs" / "research"
EVIDENCE_DIR = ROOT / "docs" / "evidence"

GAPS = RESEARCH / "loader-feature-gaps.md"
R12 = RESEARCH / "r12-remaining-gaps-status.md"

R11_STEM = "2026-06-02-round11-remap-sigill-fixed"

# The six R12 lanes this drain SSOT must enumerate (lane id per row).
R12_LANES = (
    "g1-seqint",
    "g3-vk",
    "g5-gles3",
    "g4-input",
    "apt-fakeroot",
    "docs",
)


def _read(p: Path) -> str:
    assert p.is_file(), f"SSOT doc missing: {p}"
    return p.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# 1. The R11 evidence file must exist and carry its key device facts.
# --------------------------------------------------------------------------- #

def test_r11_evidence_file_exists_and_nonempty():
    ev = EVIDENCE_DIR / f"{R11_STEM}.md"
    assert ev.is_file(), f"R11 evidence doc must exist on disk: {ev}"
    assert ev.read_text(encoding="utf-8").strip(), f"R11 evidence doc is empty: {ev}"


def test_r11_evidence_states_sigill_fixed_single_span():
    """R11: re-mapped guest SIGILL fixed via single-span mapping (no boundary clobber)."""
    text = _read(EVIDENCE_DIR / f"{R11_STEM}.md")
    assert "SIGILL" in text, "R11 must name the SIGILL it fixes"
    # The fix is real: no more signal 4, jumped entry healthy.
    assert ("signal 4" in text) or ("Illegal instruction" in text), (
        "R11 must record the signal 4 (Illegal instruction) it eliminates"
    )
    assert "entry=0x400640" in text or "0x400640" in text, (
        "R11 must record the healthy map+jump entry (0x400640)"
    )
    # Root cause: per-PT_LOAD MAP_FIXED clobbered a shared boundary page.
    assert "MAP_FIXED" in text, "R11 must name the MAP_FIXED root cause"
    assert ("clobber" in text.lower()) or ("boundary" in text.lower()), (
        "R11 must record the shared-boundary-page clobber root cause"
    )
    # The mapper is now correct.
    assert "AT_HWCAP" in text or "hwcap" in text.lower(), (
        "R11 must record threading real AT_HWCAP into the IRELATIVE resolver"
    )


def test_r11_evidence_keeps_inproc_opt_in_and_not_promoted():
    """R11 honesty: inproc stays opt-in (default-OFF); sequence integration remains."""
    text = _read(EVIDENCE_DIR / f"{R11_STEM}.md")
    assert "ALR_REEXEC_INPROC" in text, "R11 must name the opt-in env (ALR_REEXEC_INPROC)"
    assert ("opt-in" in text.lower()) or ("default-OFF" in text) or ("기본 OFF" in text), (
        "R11 must state inproc is opt-in / default-OFF (global ON wedges the probe sequence)"
    )
    # The remaining work is sequence-level integration, not a mapper unknown.
    assert ("sequence" in text.lower()) or ("시퀀스" in text), (
        "R11 must state the remaining work is sequence-level integration"
    )
    # No-regression must be recorded.
    assert ("no regression" in text.lower()) or ("회귀0" in text) or ("No regression" in text), (
        "R11 must record no-regression (v144 baseline)"
    )


# --------------------------------------------------------------------------- #
# 2. The new r12 status doc exists, is HOST-ONLY, and lists all six lanes
#    with per-lane device-req gates, as a single WS-1 drain checklist.
# --------------------------------------------------------------------------- #

def test_r12_status_doc_exists_host_only_not_bench():
    text = _read(R12)
    assert text.strip(), "r12-remaining-gaps-status doc is empty"
    assert "HOST-ONLY" in text, "r12 status doc must declare it is HOST-ONLY (WS-5)"
    assert "SSOT" in text, "r12 status doc must declare itself an SSOT"
    assert ("벤치" in text) or ("bench" in text.lower()), (
        "r12 status doc should clarify it is NOT a bench/perf doc"
    )


def test_r12_status_doc_lists_all_six_lanes():
    text = _read(R12)
    missing = [lane for lane in R12_LANES if lane not in text]
    assert not missing, (
        f"r12 status doc must enumerate all six lanes; missing: {missing}"
    )


def test_r12_status_doc_has_device_req_gate_per_lane():
    """The doc must carry a device-req gate framing + a single WS-1 drain checklist."""
    text = _read(R12)
    assert ("device-req" in text.lower()) or ("device-req 게이트" in text), (
        "r12 status doc must frame each lane by its device-req gate"
    )
    # A single WS-1 drain checklist (markdown checkboxes) covering the lanes.
    assert "WS-1" in text, "r12 status doc must be a WS-1 drain checklist"
    assert text.count("- [ ]") >= 5, (
        "r12 status doc must carry a checklist (>=5 unchecked drain items for the lanes)"
    )
    # The docs lane's device-req is N/A.
    assert "N/A" in text, "r12 status doc must state the docs lane device-req is N/A"


def test_r12_status_doc_is_device_pending_not_done():
    """ALL six lanes are device-pending — the doc must not mark them DONE/RUNS."""
    text = _read(R12)
    assert ("device-pending" in text.lower()) or ("device 게이트 대기" in text), (
        "r12 status doc must state the lanes are device-pending"
    )
    # No lane id may be tagged DONE/RUNS (mapper correctness / host-clean != cell promotion).
    for lane in R12_LANES:
        if lane == "docs":
            continue
        assert not re.search(rf"{re.escape(lane)}[^\n|]*\b(DONE|RUNS)\b", text), (
            f"r12 lane {lane} must NOT be marked DONE/RUNS — it is device-pending"
        )


def test_r12_status_doc_cites_v144_baseline_and_no_dangling_citations():
    text = _read(R12)
    # Anchored on the v144 / round-11 baseline.
    assert R11_STEM in text, "r12 status doc must cite the round-11 (v144) baseline evidence"
    assert "v144" in text, "r12 status doc must name the v144 baseline tree"
    # No dangling evidence/research citations.
    for tok in sorted(set(re.findall(r"[\w./-]+\.md", text))):
        if "/" not in tok:
            assert (RESEARCH / tok).is_file() or (ROOT / tok).is_file(), (
                f"r12 status doc references {tok!r} but no such file under docs/research/"
            )
            continue
        if any(seg in tok for seg in ("evidence/", "research/", "design/")):
            assert (ROOT / tok).is_file(), (
                f"r12 status doc references {tok!r} but no such file exists"
            )


def test_r12_status_doc_keeps_chromium_held():
    """chromium/CP-6 is user-held: the 6 lanes must not promote chromium multiprocess cells."""
    text = _read(R12)
    assert ("보류" in text) or ("held" in text.lower()), (
        "r12 status doc must keep chromium/CP-6 held (user 보류)"
    )


# --------------------------------------------------------------------------- #
# 3. loader-feature-gaps must carry the four (R12 in-flight) notes under
#    G1/G3/G4/G5 + the R11 SIGILL-fix fact, without marking anything DONE.
# --------------------------------------------------------------------------- #

def test_gaps_cites_r11_evidence_and_v144_baseline():
    text = _read(GAPS)
    assert R11_STEM in text, f"loader-feature-gaps must cite the R11 evidence ({R11_STEM}.md)"
    assert (EVIDENCE_DIR / f"{R11_STEM}.md").is_file()
    assert "v144" in text, "loader-feature-gaps must record the v144 baseline (round-11)"


def test_gaps_g1_records_r11_sigill_fixed():
    """G1 must record the R11 SIGILL fix (single-span mapping, mapper now correct)."""
    text = _read(GAPS)
    assert "SIGILL" in text, "G1 must mention the SIGILL"
    # The fix: single-span mapping, no per-PT_LOAD MAP_FIXED clobber.
    assert "MAP_FIXED" in text, "G1 must record the MAP_FIXED clobber root cause"
    assert ("span" in text.lower()) or ("clobber" in text.lower()), (
        "G1 must record the single-span fix / boundary clobber root cause"
    )
    # FIXED but NOT promoted (inproc opt-in).
    assert ("FIXED" in text) or ("해소" in text), "G1 must state the SIGILL is fixed"
    assert "ALR_REEXEC_INPROC" in text, "G1 must record inproc is opt-in (ALR_REEXEC_INPROC)"


def test_gaps_carries_four_r12_inflight_notes():
    """loader-feature-gaps must carry the (R12 in-flight) lane notes for G1/G3/G4/G5."""
    text = _read(GAPS)
    assert "(R12 in-flight)" in text, "loader-feature-gaps must carry (R12 in-flight) notes"
    # At least the four lane ids that map to G1/G3/G4/G5 must each be named in the doc.
    for lane in ("g1-seqint", "g3-vk", "g5-gles3", "g4-input"):
        assert lane in text, f"loader-feature-gaps must name the R12 lane {lane}"
    # The notes must point at the new r12 status SSOT.
    assert "r12-remaining-gaps-status.md" in text, (
        "loader-feature-gaps must point at the r12 status SSOT for the drain checklist"
    )
    assert (RESEARCH / "r12-remaining-gaps-status.md").is_file()


def test_gaps_r12_inflight_not_marked_done():
    """The (R12 in-flight) notes must NOT promote any lane/gap to DONE."""
    text = _read(GAPS)
    # The notes must explicitly say device-pending / NOT DONE.
    assert ("device-pending" in text.lower()) or ("NOT DONE" in text), (
        "loader-feature-gaps R12 notes must state the lanes are device-pending / NOT DONE"
    )
    # G1 must still not be marked DONE (mapper correct != RUNS).
    assert not re.search(r"G1[^\n|]*\bDONE\b", text), (
        "G1 must NOT be marked DONE — inproc is opt-in, sequence integration remains"
    )


# --------------------------------------------------------------------------- #
# 4. No-regression: round-6/7/9/10 history + key device facts must remain.
# --------------------------------------------------------------------------- #

def test_round6_7_9_10_history_preserved():
    """The v144/R12 update must not erase prior round history from the SSOT docs."""
    text = _read(GAPS)
    for stem in (
        "2026-06-02-round6-qt6-execreentry-vkrender",
        "2026-06-02-round7-vkrender-pass-drain",
        "2026-06-02-round9-optionS-dead-wx-execve",
        "2026-06-02-round10-step1-inproc-reexec-mechanism-proven",
        "2026-06-02-round10-step2-inproc-remap-mapjump",
    ):
        assert stem in text, (
            f"loader-feature-gaps must still cite prior history ({stem}.md)"
        )
    # The exec_events=0 finding (round-7) + Option S W^X dead (round-9) must remain.
    assert "exec_events=0" in text, "must keep the round-7 exec_events=0 finding"
    assert "Option S" in text, "must keep the round-9 Option S (W^X dead) finding"
    assert "ADR-003-v3" in text, "must keep the ADR-003-v3 (in-process re-map) lineage"
