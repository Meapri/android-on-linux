from __future__ import annotations

import re
from dataclasses import dataclass, field


GPU_FAMILY_ADRENO = "adreno"
GPU_FAMILY_MALI_IMMORTALIS = "mali_immortalis"
GPU_FAMILY_TENSOR_CLASS = "tensor_class"

MINIMUM_GPU_FAMILIES = frozenset(
    {
        GPU_FAMILY_ADRENO,
        GPU_FAMILY_MALI_IMMORTALIS,
        GPU_FAMILY_TENSOR_CLASS,
    }
)

REQUIRED_GPU_REPORTS = frozenset(
    {
        "HOST GPU EGL/GLES EXECUTION",
        "HOST GPU SURFACE EXECUTION",
        "GUEST GPU IPC BRIDGE EXECUTION",
        "GUEST GUI GPU SURFACE EXECUTION",
    }
)

SOFTWARE_RENDERER_MARKERS = (
    "swiftshader",
    "llvmpipe",
    "softpipe",
    "lavapipe",
    "software rasterizer",
    "mesa x11",
)

VENDOR_PRIVATE_MARKERS = (
    "/dev/dri",
    "kgsl",
    "turnip",
    "freedreno",
    "panfrost",
    "private gpu node",
    "vendor driver bundle",
)

_SHA256 = re.compile(r"^[a-fA-F0-9]{64}$")


@dataclass(frozen=True)
class DeviceEvidence:
    device_model: str
    android_api: int
    gpu_family: str
    renderer: str
    apk_sha256: str
    reports: dict[str, str]
    notes: str = ""
    used_vendor_private_path: bool = False
    software_renderer: bool | None = None
    extra: dict[str, str] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not self.device_model.strip():
            raise ValueError("device_model is required")
        if self.android_api <= 0:
            raise ValueError("android_api must be positive")
        normalized_family = normalize_gpu_family(self.gpu_family)
        object.__setattr__(self, "gpu_family", normalized_family)
        if not self.renderer.strip():
            raise ValueError("renderer is required")
        if not _SHA256.fullmatch(self.apk_sha256):
            raise ValueError("apk_sha256 must be 64 hex characters")
        normalized_reports = {key.strip().upper(): value.strip().upper() for key, value in self.reports.items()}
        object.__setattr__(self, "reports", normalized_reports)


@dataclass(frozen=True)
class GpuClaimAssessment:
    can_claim: bool
    covered_families: frozenset[str]
    missing_families: frozenset[str]
    failing_devices: tuple[str, ...]
    reason: str


def normalize_gpu_family(value: str) -> str:
    normalized = value.strip().lower().replace("-", "_").replace(" ", "_")
    aliases = {
        "qualcomm": GPU_FAMILY_ADRENO,
        "adreno_gpu": GPU_FAMILY_ADRENO,
        "mali": GPU_FAMILY_MALI_IMMORTALIS,
        "immortalis": GPU_FAMILY_MALI_IMMORTALIS,
        "arm_mali": GPU_FAMILY_MALI_IMMORTALIS,
        "google_tensor": GPU_FAMILY_TENSOR_CLASS,
        "tensor": GPU_FAMILY_TENSOR_CLASS,
    }
    return aliases.get(normalized, normalized)


def renderer_is_software(renderer: str) -> bool:
    lower = renderer.lower()
    return any(marker in lower for marker in SOFTWARE_RENDERER_MARKERS)


def notes_use_vendor_private_path(notes: str) -> bool:
    lower = notes.lower()
    return any(marker in lower for marker in VENDOR_PRIVATE_MARKERS)


def device_passes_gpu_gates(evidence: DeviceEvidence) -> bool:
    if evidence.used_vendor_private_path or notes_use_vendor_private_path(evidence.notes):
        return False
    software = evidence.software_renderer
    if software is None:
        software = renderer_is_software(evidence.renderer)
    if software:
        return False
    for report in REQUIRED_GPU_REPORTS:
        if evidence.reports.get(report) != "PASS":
            return False
    return True


def assess_vendor_independent_gpu_claim(entries: list[DeviceEvidence]) -> GpuClaimAssessment:
    passing = [entry for entry in entries if device_passes_gpu_gates(entry)]
    covered = frozenset(entry.gpu_family for entry in passing if entry.gpu_family in MINIMUM_GPU_FAMILIES)
    missing = frozenset(MINIMUM_GPU_FAMILIES - covered)
    failing = tuple(entry.device_model for entry in entries if not device_passes_gpu_gates(entry))
    if missing:
        return GpuClaimAssessment(
            can_claim=False,
            covered_families=covered,
            missing_families=missing,
            failing_devices=failing,
            reason="missing required GPU family evidence",
        )
    if failing:
        return GpuClaimAssessment(
            can_claim=False,
            covered_families=covered,
            missing_families=missing,
            failing_devices=failing,
            reason="one or more submitted device records failed GPU gates",
        )
    return GpuClaimAssessment(
        can_claim=True,
        covered_families=covered,
        missing_families=frozenset(),
        failing_devices=tuple(),
        reason="minimum GPU family evidence passed",
    )
