"""`python -m bench <subcommand>` CLI (WS-5).

Thin argparse front-end over the pure bench models. Run from the worktree root so
the relative `from .` imports resolve as the `bench` package.

Subcommands:
- gate REPORT      — parse a captured device report and run the no-regression gate.
- verify REPORT    — full report check: gate + CP-1 display + ALR marker summary.
- overhead ...     — compute native-vs-ALR CPU overhead from two wall-clock samples.
- gpu ...          — compute glmark2 ALR-vs-Mali-direct ratio against the §0 target.
- index [--dir]    — index the docs/evidence corpus as a markdown table.
- cp-status [--dir] — print the CP-0..6 device-evidence dashboard (keyword heuristic).

main(argv) returns the process exit code (0 pass / 1 fail / 2 usage) so it is
directly unit-testable without spawning a subprocess.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .cp_status import build_cp_dashboard_markdown, scan_cp_status
from .cpu_overhead import Measurement, compute_overhead
from .evidence_index import build_index_markdown, scan_evidence_dir
from .display_verify import verify_from_report
from .gpu_bench import GPU_ACCEL_MIN_RATIO, GpuScore, compute_gpu_ratio
from .regression_gate import evaluate_report, evaluate_text
from .report_parse import parse_report

_DEFAULT_EVIDENCE_DIR = Path(__file__).resolve().parents[1] / "docs" / "evidence"


def _read_report(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def _cmd_gate(args: argparse.Namespace) -> int:
    text = _read_report(args.report)
    result = evaluate_text(text)
    print(result.to_markdown())
    return 0 if result.passed else 1


def _cmd_overhead(args: argparse.Namespace) -> int:
    native = Measurement(label="native", wall_ns=args.native_ns, samples=args.native_samples)
    alr = Measurement(label="alr", wall_ns=args.alr_ns, samples=args.alr_samples)
    result = compute_overhead(
        native,
        alr,
        syscall_light=not args.storm,
        traps=args.traps,
        rewrites=args.rewrites,
        binary=args.binary,
    )
    print(result.to_markdown())
    return 0 if result.passes_target else 1


def _cmd_gpu(args: argparse.Namespace) -> int:
    alr = GpuScore(label="alr", score=args.alr_score, renderer=args.alr_renderer)
    mali = GpuScore(label="mali-direct", score=args.mali_score)
    result = compute_gpu_ratio(alr, mali, min_ratio=args.min_ratio)
    print(result.to_markdown())
    return 0 if result.passes_target else 1


def _cmd_index(args: argparse.Namespace) -> int:
    docs = scan_evidence_dir(args.dir)
    print(build_index_markdown(docs))
    return 0


def _cmd_cp_status(args: argparse.Namespace) -> int:
    print(build_cp_dashboard_markdown(scan_cp_status(args.dir)))
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    """One-shot check of a captured device report: no-regression gate + CP-1 display
    + an ALR PASS/FAIL marker summary. Exit 0 only if the gate and (when present) the
    display verdict both pass."""
    text = _read_report(args.report)
    parsed = parse_report(text)

    gate = evaluate_report(parsed)
    print(gate.to_markdown())

    disp = verify_from_report(text)
    if disp is not None:
        print()
        print(disp.to_markdown())

    markers = parsed.alr_markers
    if markers:
        passed = sum(1 for v in markers.values() if v.upper() == "PASS")
        print()
        print(f"### ALR markers — {passed}/{len(markers)} PASS")
        non_pass = [k for k, v in markers.items() if v.upper() != "PASS"]
        if non_pass:
            print("non-PASS: " + ", ".join(sorted(non_pass)))

    ok = gate.passed and (disp is None or disp.passed)
    return 0 if ok else 1


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bench",
        description="ALR WS-5 verification/bench CLI.",
    )
    sub = parser.add_subparsers(dest="command")

    gate = sub.add_parser(
        "gate",
        help="run the no-regression gate over a captured device report",
    )
    gate.add_argument("report", help="path to a captured report, or '-' for stdin")
    gate.set_defaults(func=_cmd_gate)

    verify = sub.add_parser(
        "verify",
        help="full report check: no-regression gate + CP-1 display + ALR marker summary",
    )
    verify.add_argument("report", help="path to a captured report, or '-' for stdin")
    verify.set_defaults(func=_cmd_verify)

    over = sub.add_parser(
        "overhead",
        help="compute native-vs-ALR CPU overhead from two wall-clock samples",
    )
    over.add_argument("--native-ns", type=int, required=True, help="native total wall ns")
    over.add_argument("--native-samples", type=int, default=1, help="native sample count")
    over.add_argument("--alr-ns", type=int, required=True, help="ALR total wall ns")
    over.add_argument("--alr-samples", type=int, default=1, help="ALR sample count")
    over.add_argument("--binary", default="", help="binary label for the report")
    over.add_argument(
        "--storm",
        action="store_true",
        help="syscall-storm workload: reported, not gated (syscall_light=False)",
    )
    over.add_argument("--traps", type=int, default=None, help="path-mediation traps (optional)")
    over.add_argument(
        "--rewrites", type=int, default=None, help="path-mediation rewrites (optional)"
    )
    over.set_defaults(func=_cmd_overhead)

    gpu = sub.add_parser(
        "gpu",
        help="compute glmark2 ALR-vs-Mali-direct ratio against the §0 target",
    )
    gpu.add_argument("--alr-score", type=int, required=True, help="ALR glmark2 score")
    gpu.add_argument("--mali-score", type=int, required=True, help="native Mali-direct glmark2 score")
    gpu.add_argument("--alr-renderer", default="", help="GL_RENDERER of the ALR run (software gate)")
    gpu.add_argument(
        "--min-ratio",
        type=float,
        default=GPU_ACCEL_MIN_RATIO,
        help=f"minimum acceptable ALR/Mali ratio (default {GPU_ACCEL_MIN_RATIO})",
    )
    gpu.set_defaults(func=_cmd_gpu)

    index = sub.add_parser("index", help="index the docs/evidence corpus as a markdown table")
    index.add_argument(
        "--dir",
        default=str(_DEFAULT_EVIDENCE_DIR),
        help="evidence directory (default: docs/evidence)",
    )
    index.set_defaults(func=_cmd_index)

    cp_status = sub.add_parser(
        "cp-status",
        help="print the CP-0..6 device-evidence dashboard (keyword heuristic)",
    )
    cp_status.add_argument(
        "--dir",
        default=str(_DEFAULT_EVIDENCE_DIR),
        help="evidence directory (default: docs/evidence)",
    )
    cp_status.set_defaults(func=_cmd_cp_status)

    return parser


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    func = getattr(args, "func", None)
    if func is None:
        parser.print_help()
        return 2
    return func(args)


if __name__ == "__main__":
    import sys

    sys.exit(main())
