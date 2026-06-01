"""WS-5 host tests: CP-4 zero-copy present verification.

Covers parsing the device markers ``ALR AHB ZEROCOPY IMPORT: PASS`` and
``gtkdemo-result: rendered=... frames=...->...`` and the verdict logic in
bench.present_verify. Pure host test, no device required.

Device evidence: docs/evidence/2026-06-01-drain5-cp4-dmabuf-present-single-gate.md
"""
from bench.present_verify import (
    PresentVerdict,
    parse_ahb_zerocopy_import,
    parse_gtkdemo_result,
    verify_present,
)


# Real device markers (drain #5). The arrow in the real log is a unicode arrow.
SAMPLE_IMPORT_PASS = "ALR AHB ZEROCOPY IMPORT: PASS"
SAMPLE_IMPORT_FAIL = "ALR AHB ZEROCOPY IMPORT: FAIL"
SAMPLE_GTK_UNICODE = "gtkdemo-result: rendered=true frames=12→13"
SAMPLE_GTK_ASCII = "gtkdemo-result: rendered=true frames=12->13"
SAMPLE_GTK_NOADVANCE = "gtkdemo-result: rendered=true frames=13->13"


def test_parse_ahb_zerocopy_import_pass():
    assert parse_ahb_zerocopy_import(SAMPLE_IMPORT_PASS) is True


def test_parse_ahb_zerocopy_import_fail():
    assert parse_ahb_zerocopy_import(SAMPLE_IMPORT_FAIL) is False


def test_parse_ahb_zerocopy_import_absent():
    assert parse_ahb_zerocopy_import("no import marker here") is None


def test_parse_gtkdemo_result_unicode_arrow():
    parsed = parse_gtkdemo_result(SAMPLE_GTK_UNICODE)
    assert parsed is not None
    assert parsed["rendered"] is True
    assert parsed["frames_start"] == 12
    assert parsed["frames_end"] == 13


def test_parse_gtkdemo_result_ascii_arrow():
    parsed = parse_gtkdemo_result(SAMPLE_GTK_ASCII)
    assert parsed is not None
    assert parsed["rendered"] is True
    assert parsed["frames_start"] == 12
    assert parsed["frames_end"] == 13


def test_parse_gtkdemo_result_absent():
    assert parse_gtkdemo_result("no gtkdemo result") is None


def test_verify_present_both_markers_pass():
    report = (
        "some preamble\n"
        f"{SAMPLE_IMPORT_PASS}\n"
        "external-oes self-test pixel=0,0,0\n"  # info-only, ignored
        f"{SAMPLE_GTK_UNICODE}\n"
        "some trailer\n"
    )
    v = verify_present(report)
    assert v is not None
    assert v.import_ok is True
    assert v.rendered is True
    assert v.frames_advanced is True
    assert v.passed is True


def test_verify_present_frames_do_not_advance():
    report = f"{SAMPLE_IMPORT_PASS}\n{SAMPLE_GTK_NOADVANCE}\n"
    v = verify_present(report)
    assert v is not None
    assert v.import_ok is True
    assert v.rendered is True
    assert v.frames_advanced is False
    assert v.passed is False


def test_verify_present_import_fail():
    report = f"{SAMPLE_IMPORT_FAIL}\n{SAMPLE_GTK_UNICODE}\n"
    v = verify_present(report)
    assert v is not None
    assert v.import_ok is False
    assert v.passed is False


def test_verify_present_import_only_partial_not_passed():
    # AHB import PASS but gtkdemo marker absent → partial verdict, not passed.
    v = verify_present(SAMPLE_IMPORT_PASS)
    assert v is not None
    assert v.import_ok is True
    assert v.rendered is None
    assert v.frames_advanced is None
    assert v.passed is False
    assert "MISSING" in v.detail


def test_verify_present_gtk_only_partial_not_passed():
    v = verify_present(SAMPLE_GTK_UNICODE)
    assert v is not None
    assert v.import_ok is None
    assert v.rendered is True
    assert v.frames_advanced is True
    assert v.passed is False
    assert "MISSING" in v.detail


def test_verify_present_neither_marker_is_none():
    assert verify_present("nothing") is None


def test_to_markdown_smoke():
    report = f"{SAMPLE_IMPORT_PASS}\n{SAMPLE_GTK_UNICODE}\n"
    md = verify_present(report).to_markdown()
    assert "CP-4 zero-copy present verification" in md
    assert "PASS" in md
    # partial (missing) path renders without crashing
    md2 = verify_present(SAMPLE_IMPORT_PASS).to_markdown()
    assert "MISSING" in md2
    assert isinstance(verify_present(report), PresentVerdict)
