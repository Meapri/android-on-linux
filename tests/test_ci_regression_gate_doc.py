"""WS-5 guard: the CI / regression-gate doc must exist and carry its load-bearing
content (orchestration plan §3 M5).

`docs/research/ci-regression-gate.md` documents the CI architecture: what is
host-automated (pytest + NDK 4-ABI native build + zig shim/wire-check) versus what
is a manual device gate (the single shared device, serialized by the integration
session per §9), plus the no-regression criteria (GIMP/foot/gtk3demo/CLI/glmark2
must stay RUN, version-stamp pin). These lenient checks pin the doc's existence and
its essential sections so it can't silently lose them, without being brittle about
exact wording.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "ci-regression-gate.md"


def _text() -> str:
    assert DOC.is_file(), f"CI/regression-gate doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


def test_doc_exists_and_nonempty():
    text = _text()
    assert len(text) > 1000, "CI/regression-gate doc is suspiciously short"


def test_doc_distinguishes_host_auto_vs_device_manual():
    """The core purpose: separate the host-automatic gate from the device-manual one."""
    text = _text()
    low = text.lower()
    assert "host" in low and "device" in low
    # Both halves must be named explicitly.
    assert ("자동" in text) or ("auto" in low), "must mark the host gate as automatic"
    assert ("수동" in text) or ("manual" in low), "must mark the device gate as manual"


def test_doc_names_the_three_host_gate_components():
    """Host gate = uvx pytest + NDK 4-ABI native build + zig build-shim/wire-check."""
    text = _text()
    assert "uvx pytest" in text, "host gate must name the pytest entrypoint"
    # NDK 4-ABI native build.
    assert ("externalNativeBuild" in text) or ("4-ABI" in text) or ("4 ABI" in text), (
        "host gate must name the NDK native (4-ABI) build"
    )
    # zig shim build + wire-check seam.
    assert "build-shim.sh" in text and "build-wire-check.sh" in text, (
        "host gate must name the guest-shim build + wire-check scripts"
    )


def test_doc_states_device_is_single_serial_integration_owned():
    """Device gate = single shared device, serialized by the integration session (§9)."""
    text = _text()
    low = text.lower()
    assert "R5KL20B6S3X" in text, "must name the single shared device serial"
    assert ("§9" in text) or ("device lease" in low), "must reference the §9 Device Lease protocol"
    assert "DEVICE-REQ" in text, "must describe the DEVICE-REQ request protocol"


def test_doc_pins_no_regression_run_set_and_stamp():
    """No-regression: GIMP/foot/gtk3demo/CLI/glmark2 stay RUN + version-stamp pin."""
    text = _text()
    for app in ("GIMP", "foot", "gtk3demo", "glmark2"):
        assert app in text, f"no-regression set must mention {app}"
    # mediation invariant is the spine of the gate.
    assert "pcgate=1 interpose=1 traps=0 rewrites=0" in text, (
        "must state the mediation invariant (traps=0 rewrites=0)"
    )
    # version-stamp pin discipline.
    assert ("version-stamp" in text) or ("version stamp" in text) or ("stamp 핀" in text), (
        "must cover the version-stamp pin discipline"
    )


def test_doc_marks_m5_and_links_sibling_gate_doc():
    text = _text()
    assert "M5" in text, "doc should tie itself to orchestration plan §3 M5"
    # The criteria-detail sibling doc.
    assert "ws5-premerge-gate.md" in text, (
        "should cross-link the per-checkpoint gate-criteria doc"
    )
