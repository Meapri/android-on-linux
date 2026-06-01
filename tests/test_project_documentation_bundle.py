from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
PRODUCT = ROOT / "docs/product-requirements.md"
CLEAN_ROOM = ROOT / "docs/clean-room-protocol.md"
EXEC_SPEC = ROOT / "docs/alr-execution-backend-spec.md"
GRAPHICS_SPEC = ROOT / "docs/android-graphics-bridge-spec.md"
MILESTONES = ROOT / "docs/plans/implementation-milestones.md"
AGENT_COORDINATION = ROOT / "docs/agent-coordination.md"
PARALLEL_WORKSTREAMS = ROOT / "docs/plans/parallel-workstreams.md"
CODEX_PROMPT = ROOT / "docs/prompts/codex-clean-alr-runtime.md"
HERMES_PROMPT = ROOT / "docs/prompts/hermes-proroot-ab-and-device-evidence.md"


def test_readme_links_new_planning_bundle():
    text = README.read_text()
    for path in [
        "docs/product-requirements.md",
        "docs/clean-room-protocol.md",
        "docs/alr-execution-backend-spec.md",
        "docs/android-graphics-bridge-spec.md",
        "docs/plans/implementation-milestones.md",
        "docs/agent-coordination.md",
        "docs/plans/parallel-workstreams.md",
    ]:
        assert path in text


def test_product_requirements_define_runtime_and_gpu_target():
    text = PRODUCT.read_text()
    assert "non-root Android application runtime" in text
    assert "proroot-class behavior" in text
    assert "Android `Surface`, `EGL/GLES`, and later `Vulkan`" in text
    assert "Acceptance Ladder" in text


def test_clean_room_protocol_keeps_optional_probes_out_of_implementation():
    text = CLEAN_ROOM.read_text()
    assert "Optional external backends" in text
    assert "Disassembly-derived algorithm source" in text
    assert "black-box behavior tests" in text
    assert "not to clone a closed runtime" in text


def test_execution_backend_spec_defines_staged_alr_runtime():
    text = EXEC_SPEC.read_text()
    assert "ALR Exec v1: Preload Filesystem MVP" in text
    assert "ALR Exec v2: Process Continuity" in text
    assert "ALR Exec v3: Identity and Procfs" in text
    assert "ALR Exec v4: Package-Manager Preflight" in text
    assert "Direct Syscall Coverage" in text


def test_graphics_bridge_spec_defines_android_native_surface_path():
    text = GRAPHICS_SPEC.read_text()
    assert "Android-owned `Surface`" in text
    assert "Renderer v4: Guest GLES Shim" in text
    assert "Renderer v5: Vulkan Host Renderer" in text
    assert "Do not rely on guest Linux /dev/dri" not in text
    assert "Direct guest access to `/dev/dri`, KMS, GBM, or DRM nodes" in text


def test_implementation_milestones_define_ordered_bundles():
    text = MILESTONES.read_text()
    for heading in [
        "Bundle A: Documentation and Guardrails",
        "Bundle B: Optional proroot A/B Probe",
        "Bundle C: ALR Launcher Skeleton",
        "Bundle D: ALR Path Hook MVP",
        "Bundle E: ALR Hello and Shell Execution",
        "Bundle K: GLES Shim MVP",
        "Bundle L: Vulkan Research MVP",
    ]:
        assert heading in text
    assert "Stop Conditions" in text


def test_agent_coordination_defines_codex_hermes_split():
    text = AGENT_COORDINATION.read_text()
    assert "Codex:" in text
    assert "Hermes:" in text
    assert "docs/agent-sync.md" in text
    assert "Handoff Entry Format" in text


def test_parallel_workstreams_define_separate_write_scopes():
    text = PARALLEL_WORKSTREAMS.read_text()
    assert "Workstream A: Clean ALR Runtime" in text
    assert "Workstream B: Device Evidence and Optional Backend Probe" in text
    assert "Do not touch by default" in text
    assert "Shared Contract" in text


def test_reusable_agent_prompts_exist():
    codex = CODEX_PROMPT.read_text()
    hermes = HERMES_PROMPT.read_text()
    assert "Mission:" in codex
    assert "Clean-room rules:" in codex
    assert "Bundle C" in codex
    assert "Mission:" in hermes
    assert "Clean-room rules:" in hermes
    assert "Bundle B" in hermes
