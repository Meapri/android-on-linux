"""WS-5 guard: the chromium BROWSER run-plan + GPU-path SSOT docs must exist and be honest.

This pins the path from "chromium runs `--version`" (already device-proven,
docs/evidence/2026-06-01-device-SM-X236N-chromium-runs-inprocess.md) to "browser
renders a page" as the CR-1..CR-5 ladder, plus the GPU flag ladder for CR-3.

  - chromium-run-plan.md — milestones CR-1..CR-5, each with its exact chromium flag
    set, blockers, and a device-drain gate (what log proves it). CR-1 = single-process
    headless --dump-dom of a data:/file: page (engine renders, DOM dumped); CR-2 +=
    https URL (net); CR-3 += GPU; CR-4 = a GUI window via --ozone-platform=wayland;
    CR-5 = drop --single-process (multiprocess zygote). HONEST: only CR-1 is near-term;
    CR-5 is gated on G1 (in-process re-map exec re-entry, in-flight).
  - chromium-gpu-path.md — the GPU flag ladder for CR-3: --disable-gpu (software) ->
    --use-gl=angle --use-angle=swiftshader (software Vulkan) -> --use-gl=angle
    --use-angle=gles-egl (ANGLE -> our libEGL.so.1/libGLESv2.so.2 GLES shim -> Mali)
    -> (later) our VK marshal. Which approach needs which staged libs.
  - cp6-status.md — a §0-a section linking chromium-run-plan as the path from
    "--version runs" to "browser running", with PR #2 measure-first (alarm window) +
    the --single-process sidestep of G1.

These lenient checks follow the existing doc-enforcement style
(test_round9_round10_milestones_doc.py, test_round11_r12_milestones_doc.py). They do
NOT promote any CR cell to RUNS/PASS — chromium is user-held and only CR-1 is near-term.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / "docs" / "research"
EVIDENCE_DIR = ROOT / "docs" / "evidence"

RUN_PLAN = RESEARCH / "chromium-run-plan.md"
GPU_PATH = RESEARCH / "chromium-gpu-path.md"
CP6 = RESEARCH / "cp6-status.md"

CR_MILESTONES = ("CR-1", "CR-2", "CR-3", "CR-4", "CR-5")
VERSION_RUNS_EVIDENCE = "2026-06-01-device-SM-X236N-chromium-runs-inprocess"


def _read(p: Path) -> str:
    assert p.is_file(), f"SSOT doc missing: {p}"
    return p.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# 1. The two new docs exist, are HOST-ONLY, and are not bench docs.
# --------------------------------------------------------------------------- #

def test_run_plan_and_gpu_path_docs_exist_nonempty():
    for p in (RUN_PLAN, GPU_PATH):
        assert p.is_file(), f"SSOT doc must exist on disk: {p}"
        assert p.read_text(encoding="utf-8").strip(), f"SSOT doc is empty: {p}"


def test_run_plan_is_host_only_ssot_not_bench():
    text = _read(RUN_PLAN)
    assert "HOST-ONLY" in text, "run-plan must declare it is HOST-ONLY (WS-5)"
    assert "SSOT" in text, "run-plan must declare itself an SSOT"
    assert ("벤치" in text) or ("bench" in text.lower()), (
        "run-plan must clarify it is NOT a bench/perf doc"
    )


def test_gpu_path_is_host_only_ssot_not_bench():
    text = _read(GPU_PATH)
    assert "HOST-ONLY" in text, "gpu-path must declare it is HOST-ONLY (WS-5)"
    assert "SSOT" in text, "gpu-path must declare itself an SSOT"
    assert ("벤치" in text) or ("bench" in text.lower()), (
        "gpu-path must clarify it is NOT a bench/perf doc"
    )


# --------------------------------------------------------------------------- #
# 2. The run-plan lists CR-1..CR-5, each with a flag set + a device-drain gate.
# --------------------------------------------------------------------------- #

def test_run_plan_lists_all_five_milestones():
    text = _read(RUN_PLAN)
    missing = [cr for cr in CR_MILESTONES if cr not in text]
    assert not missing, f"run-plan must enumerate all CR milestones; missing: {missing}"


def test_run_plan_cr1_exact_flag_set():
    """CR-1 = single-process headless --dump-dom of a data:/file: page, GPU/net/multiproc 0."""
    text = _read(RUN_PLAN)
    for flag in (
        "--single-process",
        "--no-zygote",
        "--no-sandbox",
        "--disable-gpu",
        "--dump-dom",
    ):
        assert flag in text, f"CR-1 flag set must include {flag}"
    # The target is a local data:/file: page (no network).
    assert ("data:" in text) or ("file:" in text), (
        "CR-1 must target a local data:/file: page (no network)"
    )


def test_run_plan_each_milestone_has_flag_and_gate():
    """Every CR milestone must carry a flag set and a device-drain gate (what log proves it)."""
    text = _read(RUN_PLAN)
    # A flag set framing + a device-drain gate framing must both be present.
    assert text.count("flag set") >= 3, "run-plan must give a flag set per milestone"
    assert ("device-drain gate" in text) or ("drain gate" in text), (
        "run-plan must frame each milestone by its device-drain gate"
    )
    # CR-2 += real https URL.
    assert "https://" in text, "CR-2 must target a real https URL"
    # CR-3 += GPU.
    assert ("GPU" in text) and ("--use-gl" in text or "use-angle" in text), (
        "CR-3 must bring GPU (the --use-gl/--use-angle ladder)"
    )
    # CR-4 = GUI window via ozone-platform=wayland.
    assert "--ozone-platform=wayland" in text, (
        "CR-4 must use --ozone-platform=wayland (GUI window to the compositor)"
    )
    # The wl interfaces the compositor must advertise.
    assert "xdg_wm_base" in text and "wl_shm" in text, (
        "CR-4 must name the mandatory wl interfaces (xdg_wm_base + wl_shm)"
    )


def test_run_plan_cr1_drain_gate_is_concrete():
    """CR-1 drain gate: child exit=0 + a serialized DOM in guest stdout = page rendered."""
    text = _read(RUN_PLAN)
    assert "child exit=0" in text, "CR-1 gate must require child exit=0"
    assert ("guest stdout=" in text), (
        "CR-1 gate must key on the loader's guest stdout= line (the dumped DOM)"
    )
    assert "</html>" in text, (
        "CR-1 gate must require a serialized DOM (terminating </html>) as render proof"
    )


def test_run_plan_has_ws1_handoff_flag_string():
    """WS-1 handoff: a precise CR-1 flag string + the 'rendered the page' drain gate."""
    text = _read(RUN_PLAN)
    assert "WS-1" in text, "run-plan must carry a WS-1 handoff"
    # The newline-delimited argv string the loader parses.
    assert r"\n--single-process" in text or "--single-process\\n" in text, (
        "WS-1 handoff must give the newline-delimited CR-1 argv flag string"
    )
    # The drain gate must define 'Chromium rendered the page'.
    assert "rendered the page" in text, (
        "WS-1 handoff must define what logcat line = 'Chromium rendered the page'"
    )


# --------------------------------------------------------------------------- #
# 3. Honesty: CR-5 is gated on G1 (in-flight); only CR-1 is near-term.
# --------------------------------------------------------------------------- #

def test_run_plan_cr5_drops_single_process_multiprocess():
    """CR-5 = drop --single-process (the multiprocess zygote)."""
    text = _read(RUN_PLAN)
    assert ("multiprocess" in text.lower()) or ("멀티프로세스" in text), (
        "CR-5 must be the multiprocess (zygote) milestone"
    )
    # It is explicitly the act of dropping --single-process.
    assert re.search(r"drop[^\n]*--single-process", text) or ("--single-process` 제거" in text) or (
        "single-process` **제거**" in text
    ), "CR-5 must state it drops --single-process"


def test_run_plan_cr5_gated_on_g1():
    """HONESTY: CR-5 depends on G1 (re-mapped-guest exec re-entry, in-flight) — not near-term."""
    text = _read(RUN_PLAN)
    assert "G1" in text, "run-plan must name G1 as CR-5's gate"
    # G1 is in-flight, not RUNS.
    assert ("in-flight" in text.lower()) or ("RUNS 아님" in text) or ("not RUNS" in text), (
        "run-plan must state G1 is in-flight / not yet RUNS"
    )
    # CR-5 must be tagged long-term / G1-gated, not near-term.
    assert ("장기" in text) or ("long-term" in text.lower()) or ("G1-gated" in text), (
        "run-plan must mark CR-5 as long-term / G1-gated"
    )
    # The honest in-process-remap lineage + the remaining edges.
    assert ("in-process" in text.lower()) and ("SIGILL" in text), (
        "run-plan must keep the honest in-process re-map lineage + the remaining SIGILL"
    )
    assert "/proc/self/exe" in text, "run-plan must keep the /proc/self/exe pass-through edge"


def test_run_plan_only_cr1_is_near_term():
    """HONESTY: only CR-1 is near-term; the doc must say so."""
    text = _read(RUN_PLAN)
    assert ("near-term" in text.lower()) or ("near term" in text.lower()), (
        "run-plan must use the near-term framing"
    )
    # CR-1 is the near-term one; CR-5 is NOT near-term (it is G1-gated/long-term).
    assert not re.search(r"CR-5[^\n|]*near-term", text), (
        "CR-5 must NOT be marked near-term — it is G1-gated/long-term"
    )
    # No CR milestone may be promoted to RUNS/PASS (chromium is held).
    for cr in CR_MILESTONES:
        assert not re.search(rf"{re.escape(cr)}[^\n|]*\b(RUNS|PASS)\b", text), (
            f"{cr} must NOT be promoted to RUNS/PASS — chromium is user-held, device-pending"
        )


def test_run_plan_keeps_chromium_held_and_measure_first():
    """The run-plan must keep chromium user-held and carry the measure-first (alarm window)."""
    text = _read(RUN_PLAN)
    assert ("보류" in text) or ("held" in text.lower()), (
        "run-plan must keep chromium user-held (보류)"
    )
    # PR #2 measure-first: the alarm window, not a deadlock.
    assert "measure-first" in text, "run-plan must record the measure-first verdict for CR-1"
    assert ("ALR_GUEST_ALARM_S" in text) or ("alarm" in text.lower()), (
        "run-plan must reference the alarm measurement window (PR #2)"
    )
    assert ("오진" in text) or ("misdiagnos" in text.lower()), (
        "run-plan must record the deadlock re-diagnosis (misdiagnosis)"
    )


# --------------------------------------------------------------------------- #
# 4. The GPU-path doc: a flag ladder + which approach needs which staged libs.
# --------------------------------------------------------------------------- #

def test_gpu_path_has_the_flag_ladder():
    text = _read(GPU_PATH)
    # The four-rung ladder.
    assert "--disable-gpu" in text, "gpu-path rung ① must be --disable-gpu (software raster)"
    assert "swiftshader" in text.lower(), (
        "gpu-path rung ② must be SwiftShader (software Vulkan)"
    )
    assert "gles-egl" in text, "gpu-path rung ③ must be --use-angle=gles-egl (ANGLE -> our shim)"
    # ANGLE is the chromium-side GL frontend.
    assert ("--use-gl=angle" in text) or ("use-angle" in text), (
        "gpu-path must reference --use-gl=angle / --use-angle"
    )


def test_gpu_path_names_shim_sonames_and_staged_libs():
    """gpu-path must say which approach needs which staged libs (esp. our shim sonames)."""
    text = _read(GPU_PATH)
    # Our GLES shim discovered via LD_LIBRARY_PATH with exact sonames.
    assert "libEGL.so.1" in text and "libGLESv2.so.2" in text, (
        "gpu-path must name our shim sonames (libEGL.so.1 / libGLESv2.so.2)"
    )
    # SwiftShader is a chromium-bundled lib.
    assert ("libvk_swiftshader" in text) or ("vk_swiftshader" in text), (
        "gpu-path must name the chromium-bundled SwiftShader lib"
    )
    # The shim stage tar lineage.
    assert "gpushim-stage.tar" in text, (
        "gpu-path must reference the gpushim-stage.tar (our shim staged lib)"
    )
    # dmabuf/dev/dri stays UN-satisfied (non-root) -> wl_shm swap.
    assert ("dmabuf" in text) and ("wl_shm" in text), (
        "gpu-path must record dmabuf stays un-satisfied (swap stays wl_shm)"
    )


def test_gpu_path_recommends_a_ladder_honestly():
    text = _read(GPU_PATH)
    assert ("권장" in text) or ("recommend" in text.lower()), (
        "gpu-path must give a recommended flag ladder"
    )
    # Honesty: only rung ① is device-safe today; the rest are device-pending.
    assert "device-pending" in text.lower(), (
        "gpu-path must mark the accelerated rungs device-pending"
    )
    # The CP-2 lineage (our shim already drove Mali for glmark2).
    assert "cp2-gpu-ratio-glmark2" in text or "CP-2" in text, (
        "gpu-path must cite the CP-2 lineage (shim -> Mali, glmark2)"
    )


# --------------------------------------------------------------------------- #
# 5. cp6-status links the run-plan as the "--version -> browser" path.
# --------------------------------------------------------------------------- #

def test_cp6_links_run_plan_as_version_to_browser_path():
    text = _read(CP6)
    assert "chromium-run-plan.md" in text, (
        "cp6-status must link chromium-run-plan as the --version -> browser path"
    )
    assert (RESEARCH / "chromium-run-plan.md").is_file()
    # The CR ladder must be referenced.
    for cr in ("CR-1", "CR-5"):
        assert cr in text, f"cp6-status must reference {cr} from the run-plan ladder"
    # The measure-first (alarm window) + the --single-process sidestep of G1.
    assert "measure-first" in text, "cp6-status must carry PR #2 measure-first (alarm window)"
    assert ("--single-process" in text) and ("G1" in text), (
        "cp6-status must note the --single-process sidestep of G1 (multiprocess)"
    )


def test_cp6_links_gpu_path_doc():
    text = _read(CP6)
    assert "chromium-gpu-path.md" in text, "cp6-status must link the chromium-gpu-path doc"
    assert (RESEARCH / "chromium-gpu-path.md").is_file()


# --------------------------------------------------------------------------- #
# 6. No dangling citations + the --version baseline evidence is real.
# --------------------------------------------------------------------------- #

def test_run_plan_cites_version_runs_baseline():
    text = _read(RUN_PLAN)
    assert VERSION_RUNS_EVIDENCE in text, (
        "run-plan must cite the chromium --version device baseline evidence"
    )
    assert (EVIDENCE_DIR / f"{VERSION_RUNS_EVIDENCE}.md").is_file()


def test_docs_have_no_dangling_md_citations():
    for doc in (RUN_PLAN, GPU_PATH):
        text = _read(doc)
        for tok in sorted(set(re.findall(r"[\w./-]+\.md", text))):
            if "/" not in tok:
                assert (RESEARCH / tok).is_file() or (ROOT / tok).is_file(), (
                    f"{doc.name} references {tok!r} but no such file under docs/research/"
                )
                continue
            if any(seg in tok for seg in ("evidence/", "research/", "design/")):
                assert (ROOT / tok).is_file(), (
                    f"{doc.name} references {tok!r} but no such file exists"
                )
