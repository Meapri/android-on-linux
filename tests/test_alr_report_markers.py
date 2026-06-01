"""WS-5 host tests: generic ALR marker map + compositor wl_output parsing.

Future-proofs the report parser against the growing list of `ALR <name>: PASS`
device markers, and verifies the compositor's `client bound: wl_output` line is
parsed into structured fields. Pure host test, no device required.
"""
from bench.report_parse import parse_report


SAMPLE_REPORT = """\
build: 0.4.127-ws5-marker-map
execution summary
ALR NATIVE LOADER GUEST EXEC: PASS
ALR GPU MARSHALLING HARDWARE RENDER: PASS
ALR PERF HARNESS: PASS
ALR GPU AHB RENDER (guest draw landed in AHB, direct read): PASS
ALR SOME FUTURE PROBE: FAIL
client bound: wl_output v2 (1200x1920 px, 70x111 mm, scale=2, dpi=440)
"""

_AHB_KEY = "ALR GPU AHB RENDER (guest draw landed in AHB, direct read)"


def test_alr_markers_generic_map():
    r = parse_report(SAMPLE_REPORT)
    assert r.alr_markers["ALR PERF HARNESS"] == "PASS"
    assert r.alr_markers["ALR NATIVE LOADER GUEST EXEC"] == "PASS"
    assert r.alr_markers["ALR GPU MARSHALLING HARDWARE RENDER"] == "PASS"
    # Parenthesized marker name (with internal spaces/commas) is preserved verbatim.
    assert _AHB_KEY in r.alr_markers
    assert r.alr_markers[_AHB_KEY] == "PASS"
    # FAIL status is captured too, not just PASS.
    assert r.alr_markers["ALR SOME FUTURE PROBE"] == "FAIL"


def test_wl_output_parsed():
    r = parse_report(SAMPLE_REPORT)
    assert r.wl_output is not None
    assert r.wl_output.version == 2
    assert r.wl_output.width_px == 1200
    assert r.wl_output.height_px == 1920
    assert r.wl_output.width_mm == 70
    assert r.wl_output.height_mm == 111
    assert r.wl_output.scale == 2
    assert r.wl_output.dpi == 440.0


def test_minimal_report_has_no_markers_or_wl_output():
    r = parse_report("build: 0.4.0-x\nnothing else here\n")
    assert r.alr_markers == {}
    assert r.wl_output is None
