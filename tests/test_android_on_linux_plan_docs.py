from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
PRIOR_ART = ROOT / "docs/research/prior-art.md"
EXEC_ARCH = ROOT / "docs/architecture/execution-backend.md"
GPU_ARCH = ROOT / "docs/architecture/gpu-display-bridge.md"
DEVICE_EVIDENCE = ROOT / "docs/architecture/device-evidence.md"
RUNTIME_ADR = ROOT / "docs/adr/0001-runtime-model.md"
GPU_ADR = ROOT / "docs/adr/0002-gpu-public-api.md"


def test_readme_records_plib_seed_and_requested_plan_docs():
    text = README.read_text(encoding="utf-8")
    assert "Meapri/Plib" in text
    assert "ce92165fab26a5a13cbb35fd75eb0bab2959fb5a" in text
    assert "docs/research/prior-art.md" in text
    assert "docs/architecture/execution-backend.md" in text
    assert "docs/architecture/gpu-display-bridge.md" in text
    assert "app-by-app native window experience" in text


def test_prior_art_matrix_covers_required_projects_and_conclusions():
    text = PRIOR_ART.read_text(encoding="utf-8")
    for project in [
        "Plib",
        "Termux",
        "Termux:X11",
        "proot-distro",
        "UserLAnd",
        "Andronix",
        "Local Desktop",
        "FluxLinux",
        "AVF/gfxstream",
    ]:
        assert project in text
    assert "Android-public-API-first" in text
    assert "Full desktop mode" in text
    for url in [
        "https://github.com/termux/termux-app",
        "https://github.com/termux/proot-distro",
        "https://github.com/termux/termux-x11",
        "https://github.com/CypherpunkArmory/UserLAnd",
        "https://docs.andronix.app/get-started/how-does-andronix-work",
        "https://github.com/localdesktop/localdesktop",
        "https://source.android.com/docs/core/virtualization",
        "https://developer.android.com/games/develop/vulkan/overview",
    ]:
        assert url in text


def test_execution_architecture_locks_backend_order_and_policy():
    text = EXEC_ARCH.read_text(encoding="utf-8")
    assert "`PRoot baseline`" in text
    assert "`ALR launcher`" in text
    assert "`LD_PRELOAD mediation`" in text
    assert "`Clean-room syscall support`" in text
    assert "Modern Android does not allow" in text
    assert "Android host variables" in text


def test_gpu_architecture_locks_public_api_vendor_independent_path():
    text = GPU_ARCH.read_text(encoding="utf-8")
    assert "Android public graphics APIs" in text
    assert "`SurfaceView`/`ANativeWindow`" in text
    assert "Do not use `/dev/dri`, KMS, GBM, KGSL, Turnip" in text
    assert "Adreno, Mali/Immortalis, and Tensor-class" in text
    assert "Wayland-shaped ingress" in text


def test_adrs_record_runtime_and_gpu_decisions():
    runtime = RUNTIME_ADR.read_text(encoding="utf-8")
    gpu = GPU_ADR.read_text(encoding="utf-8")
    assert "Accepted" in runtime
    assert "no-root, no-VM Android APK runtime" in runtime
    assert "PRoot baseline" in runtime
    assert "ALR launcher" in runtime
    assert "Accepted" in gpu
    assert "Android public graphics APIs" in gpu
    assert "Evidence covers Adreno, Mali/Immortalis, and Tensor-class devices" in gpu


def test_device_evidence_doc_prevents_broad_gpu_claims_without_real_devices():
    text = DEVICE_EVIDENCE.read_text(encoding="utf-8")
    assert "Device evidence is the rule that keeps the project honest" in text
    assert "`adreno`" in text
    assert "`mali_immortalis`" in text
    assert "`tensor_class`" in text
    assert "Software renderers are diagnostic only" in text
