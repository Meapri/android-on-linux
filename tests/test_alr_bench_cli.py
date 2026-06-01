"""Host tests for the `python -m bench` CLI (bench.__main__.main).

Exercises both subcommands through the public main(argv) entry point, asserting the
returned exit code AND that the markdown report is printed to stdout.
"""
from __future__ import annotations

from bench.__main__ import main

# The proven v122 GIMP no-regression probe set + mediation invariant. Mirrors
# bench.regression_gate.GIMP_NOREGRESSION_PROBES (gimp-console-3.0 exits 127 by design).
_PASSING_PROBES = (
    ("/bin/dynhello", 0),
    ("/usr/bin/env", 0),
    ("/usr/bin/id", 0),
    ("/bin/dash", 0),
    ("/bin/alr-png-test", 0),
    ("/usr/bin/gimp-console-3.0", 127),
    ("/bin/alr-wl-test", 0),
    ("/bin/alr-pixman-test", 0),
)


def _render_report(probes) -> str:
    lines = ["build: 0.4.124-x"]
    lines += [f"gimp-probe guest={g} exit={e}" for g, e in probes]
    lines.append("all: pcgate=1 interpose=1 traps=0 rewrites=0")
    return "\n".join(lines) + "\n"


def _write(tmp_path, name, probes):
    path = tmp_path / name
    path.write_text(_render_report(probes), encoding="utf-8")
    return path


def test_gate_passing_report(tmp_path, capsys):
    report = _write(tmp_path, "report.txt", _PASSING_PROBES)
    assert main(["gate", str(report)]) == 0
    out = capsys.readouterr().out
    assert "Regression gate" in out


def test_gate_regressed_report(tmp_path, capsys):
    # Flip /usr/bin/id from 0 -> 1: a real regression -> gate must FAIL.
    regressed = tuple(
        (g, 1 if g == "/usr/bin/id" else e) for g, e in _PASSING_PROBES
    )
    bad = _write(tmp_path, "bad.txt", regressed)
    assert main(["gate", str(bad)]) == 1


def test_overhead_under_target_passes(capsys):
    # 1000 -> 1040 ns = +4% <= 5% target -> PASS.
    assert main(["overhead", "--native-ns", "1000", "--alr-ns", "1040", "--binary", "m"]) == 0
    out = capsys.readouterr().out
    assert "CPU overhead" in out


def test_overhead_over_target_fails():
    # 1000 -> 1200 ns = +20% > 5% target -> FAIL.
    assert main(["overhead", "--native-ns", "1000", "--alr-ns", "1200"]) == 1


def test_overhead_storm_reported_not_gated():
    # 1000 -> 5000 ns = +400%, but --storm => reported, not gated => PASS.
    assert main(["overhead", "--native-ns", "1000", "--alr-ns", "5000", "--storm"]) == 0


def test_gpu_ratio_above_target_passes(capsys):
    # 900/1000 = 0.90 >= 0.70 with a hardware renderer -> PASS.
    rc = main(["gpu", "--alr-score", "900", "--mali-score", "1000", "--alr-renderer", "Mali-G615"])
    assert rc == 0
    assert "ratio" in capsys.readouterr().out


def test_gpu_ratio_below_target_fails():
    # 500/1000 = 0.50 < 0.70 -> FAIL.
    assert main(["gpu", "--alr-score", "500", "--mali-score", "1000", "--alr-renderer", "Mali-G615"]) == 1


def test_gpu_software_renderer_fails_even_when_fast():
    # ratio 2.0 but software renderer (llvmpipe) -> software gate FAIL.
    assert main(["gpu", "--alr-score", "2000", "--mali-score", "1000", "--alr-renderer", "llvmpipe"]) == 1


def test_index_default_dir_lists_corpus(capsys):
    # Default --dir resolves to the repo's docs/evidence regardless of cwd.
    assert main(["index"]) == 0
    out = capsys.readouterr().out
    assert "Title" in out


def test_no_subcommand_prints_help_returns_2():
    assert main([]) == 2


def test_cp_status_prints_dashboard(capsys):
    # Default --dir resolves to the repo's docs/evidence regardless of cwd.
    assert main(["cp-status"]) == 0
    out = capsys.readouterr().out
    assert "Checkpoint" in out
    assert "CP-1" in out


_VERIFY_REPORT = (
    "build: 0.4.127-x\n"
    + "\n".join(f"gimp-probe guest={g} exit={e}" for g, e in _PASSING_PROBES)
    + "\nall: pcgate=1 interpose=1 traps=0 rewrites=0\n"
    + "client bound: wl_output v2 (1200x1920 px, 70x111 mm, scale=2, dpi=440)\n"
    + "ALR NATIVE LOADER GUEST EXEC: PASS\n"
    + "ALR PERF HARNESS: PASS\n"
)


def test_verify_passing_report(tmp_path, capsys):
    p = tmp_path / "rep.txt"
    p.write_text(_VERIFY_REPORT, encoding="utf-8")
    assert main(["verify", str(p)]) == 0
    out = capsys.readouterr().out
    assert "Regression gate" in out
    assert "ALR markers" in out


def test_verify_regressed_report_fails(tmp_path):
    bad = _VERIFY_REPORT.replace("guest=/usr/bin/id exit=0", "guest=/usr/bin/id exit=1")
    p = tmp_path / "bad.txt"
    p.write_text(bad, encoding="utf-8")
    assert main(["verify", str(p)]) == 1
