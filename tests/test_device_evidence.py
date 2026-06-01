import pytest

from tools.device_evidence import (
    REQUIRED_GPU_REPORTS,
    DeviceEvidence,
    assess_vendor_independent_gpu_claim,
    device_passes_gpu_gates,
    renderer_is_software,
)


APK_SHA = "a" * 64


def _reports(status: str = "PASS") -> dict[str, str]:
    return {key: status for key in REQUIRED_GPU_REPORTS}


def _entry(family: str, model: str, renderer: str = "Hardware GPU") -> DeviceEvidence:
    return DeviceEvidence(
        device_model=model,
        android_api=35,
        gpu_family=family,
        renderer=renderer,
        apk_sha256=APK_SHA,
        reports=_reports(),
        notes="Android public Surface/EGL/GLES path",
    )


def test_device_evidence_rejects_invalid_sha256():
    with pytest.raises(ValueError, match="apk_sha256"):
        DeviceEvidence(
            device_model="Pixel",
            android_api=35,
            gpu_family="tensor",
            renderer="Mali",
            apk_sha256="bad",
            reports=_reports(),
        )


def test_software_renderers_do_not_pass_gpu_gates():
    evidence = _entry("adreno", "Emulator", renderer="Google SwiftShader")
    assert renderer_is_software(evidence.renderer)
    assert not device_passes_gpu_gates(evidence)


def test_vendor_private_paths_do_not_pass_gpu_gates():
    evidence = DeviceEvidence(
        device_model="Phone",
        android_api=35,
        gpu_family="adreno",
        renderer="Adreno 740",
        apk_sha256=APK_SHA,
        reports=_reports(),
        notes="used /dev/dri render node during smoke",
    )
    assert not device_passes_gpu_gates(evidence)


def test_vendor_independent_claim_requires_adreno_mali_and_tensor_class():
    entries = [
        _entry("adreno", "Snapdragon device", "Adreno 740"),
        _entry("mali", "Arm device", "Mali-G715"),
    ]
    assessment = assess_vendor_independent_gpu_claim(entries)
    assert not assessment.can_claim
    assert assessment.missing_families == frozenset({"tensor_class"})

    entries.append(_entry("tensor", "Pixel Tensor device", "Immortalis-G715"))
    assessment = assess_vendor_independent_gpu_claim(entries)
    assert assessment.can_claim
    assert assessment.covered_families == frozenset({"adreno", "mali_immortalis", "tensor_class"})


def test_failed_report_blocks_claim_even_when_families_are_covered():
    entries = [
        _entry("adreno", "Snapdragon device", "Adreno 740"),
        _entry("mali", "Arm device", "Mali-G715"),
        DeviceEvidence(
            device_model="Pixel Tensor device",
            android_api=35,
            gpu_family="tensor",
            renderer="Immortalis-G715",
            apk_sha256=APK_SHA,
            reports={**_reports(), "GUEST GUI GPU SURFACE EXECUTION": "FAIL"},
        ),
    ]
    assessment = assess_vendor_independent_gpu_claim(entries)
    assert not assessment.can_claim
    assert assessment.reason == "missing required GPU family evidence"
