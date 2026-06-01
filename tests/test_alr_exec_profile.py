"""Tests for bench.exec_profile — the CP-3 exec_ms classification model.

Seeded by WS-1 M2 device data (light CLI ~18-23 ms, heavy lib 135-197 ms,
intended dispatch waits ~6048 ms). Pure model; no device run.
"""
from __future__ import annotations

from bench.exec_profile import (
    HEAVY_LIB,
    LIGHT_CLI,
    LONG_RUNNING,
    ExecSample,
    build_exec_profile,
    classify_exec,
    profile_from_report,
    summarize,
    to_markdown,
)


def test_classify_exec_buckets():
    assert classify_exec(19) == LIGHT_CLI       # light CLI
    assert classify_exec(135) == HEAVY_LIB      # heavy lib load+init
    assert classify_exec(6048) == LONG_RUNNING  # intended dispatch/GUI wait


def test_classify_exec_boundaries():
    assert classify_exec(30) == LIGHT_CLI       # inclusive upper bound
    assert classify_exec(31) == HEAVY_LIB
    assert classify_exec(1000) == HEAVY_LIB     # inclusive upper bound
    assert classify_exec(1001) == LONG_RUNNING


# A sample report: three gimp-probe lines carry exec_ms (light/heavy/long), and a
# fourth carries no exec_ms (predates WS-1 M2) and MUST be skipped.
_SAMPLE_REPORT = """\
build: v999-test
gimp-probe guest=/bin/dynhello exit=0 traps=0 rewrites=0 first_rewrite=- stdout_bytes=0 exec_ms=19
gimp-probe guest=/usr/bin/gimp-3.0 exit=0 traps=0 rewrites=0 first_rewrite=- stdout_bytes=0 exec_ms=135
gimp-probe guest=/usr/bin/alr-input-test exit=0 traps=0 rewrites=0 first_rewrite=- stdout_bytes=0 exec_ms=6048
gimp-probe guest=/bin/legacy-noms exit=0 traps=0 rewrites=0 first_rewrite=- stdout_bytes=0
"""


def test_profile_from_report_counts_and_categories():
    samples = profile_from_report(_SAMPLE_REPORT)
    # The no-exec_ms line is skipped -> only 3 samples.
    assert len(samples) == 3

    by_guest = {s.guest: s for s in samples}
    assert "/bin/legacy-noms" not in by_guest  # skipped (exec_ms is None)

    assert by_guest["/bin/dynhello"].exec_ms == 19
    assert by_guest["/bin/dynhello"].category == LIGHT_CLI
    assert by_guest["/usr/bin/gimp-3.0"].category == HEAVY_LIB
    assert by_guest["/usr/bin/alr-input-test"].category == LONG_RUNNING


def test_build_exec_profile_skips_none():
    class _P:
        def __init__(self, guest, exec_ms):
            self.guest = guest
            self.exec_ms = exec_ms

    probes = [_P("/bin/a", 19), _P("/bin/b", None), _P("/bin/c", 200)]
    samples = build_exec_profile(probes)
    assert [s.guest for s in samples] == ["/bin/a", "/bin/c"]
    assert isinstance(samples[0], ExecSample)


def test_summarize_light_cli_count_and_median():
    samples = [
        ExecSample("/bin/dynhello", 19, LIGHT_CLI),
        ExecSample("/bin/env", 19, LIGHT_CLI),
        ExecSample("/bin/id", 20, LIGHT_CLI),
        ExecSample("/usr/bin/gimp-3.0", 135, HEAVY_LIB),
        ExecSample("/usr/bin/alr-input-test", 6048, LONG_RUNNING),
    ]
    summary = summarize(samples)
    assert summary["counts"][LIGHT_CLI] == 3
    assert summary["counts"][HEAVY_LIB] == 1
    assert summary["counts"][LONG_RUNNING] == 1
    assert summary["light_cli_min"] == 19
    assert summary["light_cli_max"] == 20
    assert summary["light_cli_median"] == 19  # median of [19, 19, 20]


def test_summarize_empty_light_cli_is_none():
    samples = [ExecSample("/usr/bin/gimp-3.0", 135, HEAVY_LIB)]
    summary = summarize(samples)
    assert summary["counts"][LIGHT_CLI] == 0
    assert summary["light_cli_min"] is None
    assert summary["light_cli_max"] is None
    assert summary["light_cli_median"] is None


def test_to_markdown_smoke():
    samples = profile_from_report(_SAMPLE_REPORT)
    md = to_markdown(samples)
    assert "/bin/dynhello" in md       # a guest
    assert LIGHT_CLI in md             # a category
    assert "| guest | exec_ms | category |" in md
    assert "summary:" in md
