"""WS-5 host tests: CP-1 device-exact display verification.

Covers parsing the compositor's `client bound: wl_output` line and the
rotation-agnostic / refresh-unverified verdict logic in bench.display_verify.
Pure host test, no device required.
"""
from bench.display_verify import (
    DEVICE_EXACT,
    DisplayExpectation,
    parse_display_marker,
    parse_wl_output_line,
    verify_display,
    verify_from_report,
)


SAMPLE_WL_OUTPUT = (
    "client bound: wl_output v2 (1200x1920 px, 70x111 mm, scale=2, dpi=440)"
)

# Real WS-3/integration device marker (v126 evidence):
#   docs/evidence/2026-06-01-v126-harfbuzz-fix-display-90hz.md
SAMPLE_DISPLAY_MARKER = "display: 1920x1200 @ 90000mHz density=213"


def test_device_exact_expectation():
    assert DEVICE_EXACT == DisplayExpectation(1200, 1920, 90)


def test_parse_wl_output_line():
    parsed = parse_wl_output_line(SAMPLE_WL_OUTPUT)
    assert parsed is not None
    assert parsed["version"] == 2
    assert parsed["width_px"] == 1200
    assert parsed["height_px"] == 1920
    assert parsed["width_mm"] == 70
    assert parsed["height_mm"] == 111
    assert parsed["scale"] == 2
    assert parsed["dpi"] == 440.0


def test_parse_wl_output_line_absent():
    assert parse_wl_output_line("no output line here") is None


def test_verify_exact_with_refresh():
    v = verify_display(1200, 1920, refresh_mhz=90000)
    assert v.resolution_ok is True
    assert v.refresh_ok is True
    assert v.refresh_verified is True
    assert v.passed is True


def test_verify_rotation_agnostic():
    v = verify_display(1920, 1200, refresh_mhz=90000)
    assert v.resolution_ok is True
    assert v.passed is True


def test_verify_wrong_resolution():
    v = verify_display(1080, 1920, refresh_mhz=90000)
    assert v.resolution_ok is False
    assert v.passed is False


def test_verify_no_refresh_is_unverified_not_failed():
    v = verify_display(1200, 1920)
    assert v.refresh_verified is False
    assert v.refresh_ok is True
    assert v.resolution_ok is True
    assert v.passed is True
    assert "unverified" in v.detail.lower()


def test_verify_wrong_refresh():
    v = verify_display(1200, 1920, refresh_mhz=60000)
    assert v.refresh_verified is True
    assert v.refresh_ok is False
    assert v.passed is False


def test_verify_from_report_resolution_only():
    v = verify_from_report(SAMPLE_WL_OUTPUT)
    assert v is not None
    assert v.resolution_ok is True
    assert v.refresh_verified is False
    # no refresh marker in the log → unverified, so resolution alone decides
    assert v.passed is True
    assert v.dpi == 440.0


def test_parse_display_marker_real_line():
    parsed = parse_display_marker(SAMPLE_DISPLAY_MARKER)
    assert parsed is not None
    assert parsed["width_px"] == 1920
    assert parsed["height_px"] == 1200
    assert parsed["refresh_mhz"] == 90000
    assert parsed["density"] == 213


def test_parse_display_marker_no_density():
    parsed = parse_display_marker("display: 1920x1200 @ 90000mHz")
    assert parsed is not None
    assert parsed["width_px"] == 1920
    assert parsed["height_px"] == 1200
    assert parsed["refresh_mhz"] == 90000
    assert parsed["density"] is None


def test_parse_display_marker_absent():
    assert parse_display_marker("no display marker here") is None


def test_verify_from_report_display_marker_refresh_verified():
    # The display: marker carries refresh, so refresh is VERIFIED here.
    report = (
        "some preamble\n"
        f"{SAMPLE_DISPLAY_MARKER}\n"
        "some trailer\n"
    )
    v = verify_from_report(report)
    assert v is not None
    assert v.resolution_ok is True  # 1920x1200 == device-exact 1200x1920 (rotation-agnostic)
    assert v.refresh_ok is True
    assert v.refresh_verified is True
    assert v.passed is True


def test_verify_from_report_prefers_display_marker_over_wl_output():
    # When BOTH markers are present, the display: marker wins → refresh verified.
    report = f"{SAMPLE_WL_OUTPUT}\n{SAMPLE_DISPLAY_MARKER}\n"
    v = verify_from_report(report)
    assert v is not None
    assert v.refresh_verified is True
    assert v.passed is True


def test_verify_from_report_absent():
    assert verify_from_report("nothing") is None


def test_to_markdown_smoke():
    md = verify_display(1200, 1920, refresh_mhz=90000, dpi=440.0).to_markdown()
    assert "CP-1 display verification" in md
    assert "PASS" in md
    # unverified refresh path renders without crashing
    md2 = verify_from_report(SAMPLE_WL_OUTPUT).to_markdown()
    assert "UNVERIFIED" in md2
