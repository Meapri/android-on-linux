"""WS-5 guard: the SSOT docs must reflect the round-9/round-10 exec-re-entry arc honestly.

This is the in-process-remap arc that CONQUERED the G1 exec-re-entry wall conceptually
(no kernel execve), device-proven through map+jump — plus the CP-6 chromium-storm
re-diagnosis. All facts below are device-verified (evidence files already on disk).

  - **R9** (`docs/evidence/2026-06-02-round9-optionS-dead-wx-execve.md`, v140): ADR-003-v2
    Option S — kernel-execve of a rootfs-path static re-entry stub — is DEAD. untrusted_app
    W^X (targetSdk 35) forbids execve() of any app_data_file: the splice fires (spliced=1)
    but `exec_events=0` on every execve and the stub never runs, even re-pushed. This is the
    whole reason ALR maps in-process (file-backed mmap(PROT_EXEC) is allowed; execve is not).

  - **R10 step1** (`docs/evidence/2026-06-02-round10-step1-inproc-reexec-mechanism-proven.md`,
    v141): the in-process re-map MECHANISM is device-proven — at the execve seccomp-trap,
    cancel the syscall (NT_ARM_SYSTEM_CALL=-1) + PC-redirect the tracee into a RESIDENT loader
    trampoline (fork-shared .text) => resident guest code runs with ZERO kernel execve
    (`ALR-REEXEC: inproc trampoline reached`, child exit=123).

  - **R10 step2** (`docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`, v143):
    the REAL map+jump is device-proven — the trampoline maps the target ELF in-process via
    mmap(PROT_EXEC) (W^X-allowed) and jumps to its entry
    (`ALR-INPROC: mapped, jumping entry=0x400640`), NO execve, static + dynamic paths wired.
    REMAINING: the re-mapped static glibc guest SIGILLs during its own startup; /proc/self/exe
    (chromium zygote, Android binary) needs a distinct pass-through; non-root dpkg needs
    fakeroot (`requires superuser`) — independent of exec-re-entry.

  - **CP-6 chromium-storm** (`docs/design/adr-chromium-storm-deadlock.md`): the chromium
    --dump-dom "multithread deadlock" is re-diagnosed as a MISDIAGNOSIS — most likely heavy
    single-init exceeding the alarm(25s) window before the first worker clone; fix = measure-first.

These lenient checks pin those facts into the SSOT docs (loader-feature-gaps G1, cp6-status)
+ the R9/R10 evidence files + the chromium-storm ADR, and pin the honesty boundaries: the
re-map MECHANISM + map/jump are device-proven (true), but the re-mapped guest SIGILLs so G1
is NOT promoted to RUNS. They follow the existing doc-enforcement test style
(test_round7_milestones_doc.py, test_round6_milestones_doc.py). The round-6 (v138) / round-7
(v139) tests are immutable history — these additions are strictly about the v140/v141/v143 arc.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / "docs" / "research"
EVIDENCE_DIR = ROOT / "docs" / "evidence"
DESIGN_DIR = ROOT / "docs" / "design"

GAPS = RESEARCH / "loader-feature-gaps.md"
CP6 = RESEARCH / "cp6-status.md"

R9_STEM = "2026-06-02-round9-optionS-dead-wx-execve"
R10S1_STEM = "2026-06-02-round10-step1-inproc-reexec-mechanism-proven"
R10S2_STEM = "2026-06-02-round10-step2-inproc-remap-mapjump"
STORM_ADR = DESIGN_DIR / "adr-chromium-storm-deadlock.md"


def _read(p: Path) -> str:
    assert p.is_file(), f"SSOT doc missing: {p}"
    return p.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# 1. The three evidence files must exist and carry their key device facts.
# --------------------------------------------------------------------------- #

def test_r9_r10_evidence_files_exist_and_nonempty():
    for stem in (R9_STEM, R10S1_STEM, R10S2_STEM):
        ev = EVIDENCE_DIR / f"{stem}.md"
        assert ev.is_file(), f"evidence doc must exist on disk: {ev}"
        assert ev.read_text(encoding="utf-8").strip(), f"evidence doc is empty: {ev}"


def test_r9_evidence_states_optionS_wx_execve_dead():
    """R9: Option S (kernel-execve stub) is W^X-dead — exec_events=0, stub never runs."""
    text = _read(EVIDENCE_DIR / f"{R9_STEM}.md")
    # The execve never completes: PTRACE_EVENT_EXEC never fires.
    assert "exec_events=0" in text, "R9 must record exec_events=0 (execve never completes)"
    # The W^X wall: app_data_file:execute neverallow blocks execve of app-storage files.
    assert "app_data_file" in text, "R9 must name the app_data_file SELinux label"
    assert "W^X" in text or "execute" in text, (
        "R9 must state the W^X / app_data_file:execute denial"
    )
    # Option S is dead.
    assert "Option S" in text, "R9 must name Option S"
    assert ("DEAD" in text) or ("dead" in text), "R9 must state Option S is dead"
    # The splice mechanism itself was correct (spliced=1) — it's the execve that fails.
    assert "spliced=1" in text, "R9 must record the splice fired (spliced=1) but execve failed"


def test_r10_step1_evidence_states_mechanism_proven_no_execve():
    """R10 step1: inproc trampoline reached, child exit=123, zero kernel execve."""
    text = _read(EVIDENCE_DIR / f"{R10S1_STEM}.md")
    assert "ALR-REEXEC: inproc trampoline reached" in text, (
        "R10 step1 must record the trampoline-reached marker"
    )
    assert "child exit=123" in text, (
        "R10 step1 must record the probe's own exit code (child exit=123)"
    )
    # The mechanism: cancel the syscall (NT_ARM_SYSTEM_CALL=-1) + PC-redirect.
    assert "NT_ARM_SYSTEM_CALL" in text, (
        "R10 step1 must record cancelling the execve via NT_ARM_SYSTEM_CALL=-1"
    )
    # No kernel execve — the whole point.
    assert ("no execve" in text.lower()) or ("zero kernel execve" in text.lower()) or (
        "no kernel execve" in text.lower()
    ), "R10 step1 must state there is no kernel execve"


def test_r10_step2_evidence_states_mapjump_and_remaining_sigill():
    """R10 step2: map+jump (entry=...), no execve, static+dynamic wired; SIGILL remains."""
    text = _read(EVIDENCE_DIR / f"{R10S2_STEM}.md")
    # The map+jump device proof.
    assert "ALR-INPROC: mapped, jumping entry=" in text, (
        "R10 step2 must record the map+jump marker (mapped, jumping entry=...)"
    )
    assert "0x400640" in text, "R10 step2 must record the concrete jump entry (0x400640)"
    assert ("map target in-process" in text) or ("mapped its PT_LOADs" in text) or (
        "PROT_EXEC" in text
    ), "R10 step2 must record the in-process mapping of the target ELF (PT_LOADs, no execve)"
    # No kernel execve.
    assert ("NO execve" in text) or ("no execve" in text.lower()) or (
        "no kernel execve" in text.lower()
    ), "R10 step2 must state NO kernel execve"
    # The honest remaining boundary: the re-mapped static glibc guest SIGILLs.
    assert "SIGILL" in text, (
        "R10 step2 must keep the honest remaining SIGILL of the re-mapped static guest"
    )
    # The two independent edges: /proc/self/exe pass-through + non-root dpkg superuser.
    assert "/proc/self/exe" in text, (
        "R10 step2 must record the /proc/self/exe (chromium zygote) distinct path"
    )
    assert "superuser" in text, (
        "R10 step2 must record non-root dpkg requires superuser (independent of re-entry)"
    )


# --------------------------------------------------------------------------- #
# 2. loader-feature-gaps G1: the re-map wall is now device-proven (map+jump), but
#    NOT promoted to RUNS (the re-mapped guest SIGILLs).
# --------------------------------------------------------------------------- #

def test_gaps_cites_r9_r10_evidence():
    text = _read(GAPS)
    for stem in (R9_STEM, R10S1_STEM, R10S2_STEM):
        assert stem in text, f"loader-feature-gaps must cite the evidence ({stem}.md)"
        assert (EVIDENCE_DIR / f"{stem}.md").is_file()


def test_gaps_g1_states_inproc_remap_device_proven():
    """G1 must record the in-process re-map mechanism + map/jump are device-proven."""
    text = _read(GAPS)
    # The map+jump entry marker proves the device map+jump.
    assert "entry=0x400640" in text or "0x400640" in text, (
        "G1 must record the device-proven map+jump (entry=0x400640)"
    )
    # The mechanism: cancel execve + PC-redirect into a resident trampoline.
    assert "NT_ARM_SYSTEM_CALL" in text, (
        "G1 must record the cancel-execve mechanism (NT_ARM_SYSTEM_CALL=-1)"
    )
    assert ("PC-redirect" in text) or ("PC redirect" in text) or ("redirect" in text), (
        "G1 must record the PC-redirect into the resident trampoline"
    )
    # The keystone: NO kernel execve (this is what sidesteps the W^X wall).
    assert ("커널 execve 0" in text) or ("no execve" in text.lower()) or (
        "no kernel execve" in text.lower()
    ), "G1 must state the re-map happens with no kernel execve"
    # The lineage: ADR-003-v3 is the conquering design.
    assert "ADR-003-v3" in text, "G1 must name ADR-003-v3 (the in-process re-map design)"


def test_gaps_g1_keeps_optionS_wx_dead_finding():
    """G1 must keep the R9 W^X finding: Option S (kernel-execve stub) is dead."""
    text = _read(GAPS)
    assert "Option S" in text, "G1 must name Option S (the dead kernel-execve stub)"
    assert ("W^X" in text) or ("app_data_file" in text), (
        "G1 must keep the W^X / app_data_file finding (why kernel-execve is dead)"
    )
    assert ("DEAD" in text) or ("dead" in text), "G1 must state Option S is dead"


def test_gaps_g1_keeps_honest_remaining_and_not_promoted():
    """G1 mechanism is proven but the re-mapped guest SIGILLs — G1 is NOT promoted to RUNS/DONE."""
    text = _read(GAPS)
    # The honest remaining: the re-mapped guest SIGILLs during its own startup.
    assert "SIGILL" in text, (
        "G1 must keep the remaining SIGILL (re-mapped static glibc guest faults at startup)"
    )
    # The two independent edges.
    assert "/proc/self/exe" in text, "G1 must record the /proc/self/exe pass-through edge"
    assert "superuser" in text or "fakeroot" in text, (
        "G1 must record non-root dpkg (requires superuser / fakeroot) as a separate edge"
    )
    # Honesty: G1 is NOT marked DONE (mechanism proven != cell promoted).
    assert not re.search(r"G1[^\n|]*\bDONE\b", text), (
        "G1 must NOT be marked DONE — the re-mapped guest still SIGILLs (mechanism != RUNS)"
    )
    # The apt-install wall stays honest.
    assert "unpacked=false" in text, "G1 must keep the apt-install unpacked=false wall honest"


# --------------------------------------------------------------------------- #
# 3. cp6-status: chromium multiprocess is on the in-process-remap track + storm
#    re-diagnosis (misdiagnosis / measure-first).
# --------------------------------------------------------------------------- #

def test_cp6_cites_r9_r10_evidence():
    text = _read(CP6)
    for stem in (R9_STEM, R10S1_STEM, R10S2_STEM):
        assert stem in text, f"cp6-status must cite the evidence ({stem}.md)"


def test_cp6_chromium_on_inproc_remap_track():
    """cp6-status must put chromium multiprocess (fresh-execve) on the in-process-remap track."""
    text = _read(CP6)
    assert "chromium" in text.lower(), "cp6-status must name chromium"
    # The track is the in-process re-map (ADR-003-v3), device-proven map+jump.
    assert "ADR-003-v3" in text, "cp6-status must name ADR-003-v3 (the in-process re-map)"
    assert ("in-process-remap" in text) or ("in-process 재-맵" in text) or (
        "in-process re-map" in text.lower()
    ), "cp6-status must state chromium multiprocess is on the in-process-remap track"
    # The W^X dead Option S must be recorded (lineage).
    assert "Option S" in text and (("W^X" in text) or ("app_data_file" in text)), (
        "cp6-status must record Option S is W^X-dead (R9, the lineage step before v3)"
    )
    # The device map+jump proof.
    assert "0x400640" in text or "entry=0x400640" in text, (
        "cp6-status must record the device-proven map+jump (entry=0x400640)"
    )


def test_cp6_storm_adr_exists_and_says_misdiagnosis_measure_first():
    """The chromium-storm ADR must exist and re-diagnose the deadlock as a misdiagnosis."""
    assert STORM_ADR.is_file(), (
        f"the chromium-storm ADR must exist on disk: {STORM_ADR}"
    )
    adr = STORM_ADR.read_text(encoding="utf-8")
    assert adr.strip(), "chromium-storm ADR is empty"
    # misdiagnosis (오진) — the deadlock diagnosis is rejected.
    assert ("오진" in adr) or ("misdiagnos" in adr.lower()), (
        "chromium-storm ADR must call the deadlock a misdiagnosis (오진/misdiagnosis)"
    )
    # measure-first verdict.
    assert "measure-first" in adr, "chromium-storm ADR must state the measure-first verdict"
    # The three deadlock candidates are all rejected.
    assert ("clone" in adr.lower()) and ("futex" in adr.lower()), (
        "chromium-storm ADR must name the rejected deadlock candidates (clone / futex)"
    )


def test_cp6_records_storm_redagnosis_misdiagnosis_measure_first():
    """cp6-status must carry the chromium-storm re-diagnosis (misdiagnosis + measure-first)."""
    text = _read(CP6)
    # cp6-status must cross-reference the storm ADR by its on-disk filename.
    assert "adr-chromium-storm-deadlock" in text, (
        "cp6-status must cross-reference the chromium-storm ADR"
    )
    assert ("오진" in text) or ("misdiagnos" in text.lower()), (
        "cp6-status must record the deadlock is a misdiagnosis (오진/misdiagnosis)"
    )
    assert "measure-first" in text, "cp6-status must record the measure-first verdict"


def test_cp6_storm_adr_reference_resolves_on_disk():
    """Any adr-chromium-storm-deadlock*.md path token in cp6-status must resolve on disk."""
    text = _read(CP6)
    for tok in re.findall(r"adr-chromium-storm-deadlock[\w./-]*\.md", text):
        assert (DESIGN_DIR / Path(tok).name).is_file(), (
            f"cp6-status references {tok!r} as a path but it does not resolve on disk"
        )


# --------------------------------------------------------------------------- #
# 4. No-regression: round-6 (v138) and round-7 (v139) history must remain.
# --------------------------------------------------------------------------- #

def test_round6_round7_history_preserved():
    """The v140/v141/v143 update must not erase round-6/round-7 history from the SSOT docs."""
    for doc in (GAPS, CP6):
        text = _read(doc)
        assert "2026-06-02-round6-qt6-execreentry-vkrender" in text, (
            f"{doc.name} must still cite the round-6 evidence (immutable history)"
        )
        assert "2026-06-02-round7-vkrender-pass-drain" in text, (
            f"{doc.name} must still cite the round-7 evidence (immutable history)"
        )
        # The exec_events=0 finding (round-7) must remain — it is why the re-map exists.
        assert "exec_events=0" in text, (
            f"{doc.name} must keep the round-7 exec_events=0 finding (the re-map's reason)"
        )
