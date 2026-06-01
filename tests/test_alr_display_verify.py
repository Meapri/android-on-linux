"""WS-5 host tests: CP-1 device-exact display verification.

Covers parsing the compositor's `client bound: wl_output` line and the
rotation-agnostic / refresh-unverified verdict logic in bench.display_verify.
Pure host test, no device required.
"""
from bench.display_verify import (
    DEVICE_EXACT,
    DisplayExpectation,
    parse_wl_output_line,
    verify_display,
    verify_from_report,
)


SAMPLE_WL_OUTPUT = (
    "client bound: wl_output v2 (1200x1920 px, 70x111 mm, scale=2, dpi=440)"
)


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


def test_verify_from_report_absent():
    assert verify_from_report("nothing") is None


def test_to_markdown_smoke():
    md = verify_display(1200, 1920, refresh_mhz=90000, dpi=440.0).to_markdown()
    assert "CP-1 display verification" in md
    assert "PASS" in md
    # unverified refresh path renders without crashing
    md2 = verify_from_report(SAMPLE_WL_OUTPUT).to_markdown()
    assert "UNVERIFIED" in md2
