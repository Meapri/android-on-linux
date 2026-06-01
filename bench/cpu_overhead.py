"""M1 — CPU-overhead model: native(adb shell) vs ALR loader for the SAME arm64 binary.

Pure math + verdict + normalized markdown/JSON emit. Source-agnostic: callers supply
two wall-clock measurements (however obtained — `adb_driver.time_native()` for the
native baseline; the ALR side from the APK report or a timed APK run). The §0 success
criterion is < 5% wall-clock overhead for syscall-light workloads; syscall-storm
workloads (chromium-class) are reported but not gated (the ptrace round-trip wall is a
known L1 architectural item, not a harness pass/fail).
"""
from __future__ import annotations

import json
from dataclasses import dataclass

# docs/research/orchestration-5session-plan.md §0: syscall-light target.
SYSCALL_LIGHT_MAX_OVERHEAD_PCT = 5.0


@dataclass(frozen=True)
class Measurement:
    label: str
    wall_ns: int
    samples: int = 1

    def __post_init__(self) -> None:
        if self.wall_ns <= 0:
            raise ValueError(f"{self.label}: wall_ns must be positive, got {self.wall_ns}")
        if self.samples <= 0:
            raise ValueError(f"{self.label}: samples must be positive, got {self.samples}")

    @property
    def per_sample_ns(self) -> float:
        return self.wall_ns / self.samples


@dataclass(frozen=True)
class OverheadResult:
    native: Measurement
    alr: Measurement
    overhead_pct: float
    ratio: float
    syscall_light: bool
    passes_target: bool
    traps: int | None = None
    rewrites: int | None = None
    binary: str = ""

    def to_dict(self) -> dict:
        return {
            "binary": self.binary,
            "native_ns_per_sample": self.native.per_sample_ns,
            "alr_ns_per_sample": self.alr.per_sample_ns,
            "overhead_pct": round(self.overhead_pct, 3),
            "ratio": round(self.ratio, 4),
            "syscall_light": self.syscall_light,
            "passes_target": self.passes_target,
            "target_max_overhead_pct": (
                SYSCALL_LIGHT_MAX_OVERHEAD_PCT if self.syscall_light else None
            ),
            "traps": self.traps,
            "rewrites": self.rewrites,
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True)

    def to_markdown(self) -> str:
        cls = "syscall-light (gated < %.0f%%)" % SYSCALL_LIGHT_MAX_OVERHEAD_PCT
        if not self.syscall_light:
            cls = "syscall-storm (reported, not gated)"
        verdict = "PASS" if self.passes_target else "FAIL"
        traps = "n/a" if self.traps is None else str(self.traps)
        rewrites = "n/a" if self.rewrites is None else str(self.rewrites)
        return (
            f"### CPU overhead — `{self.binary or 'binary'}`\n\n"
            f"| metric | value |\n"
            f"|--------|-------|\n"
            f"| native ns/sample | {self.native.per_sample_ns:,.1f} |\n"
            f"| ALR ns/sample | {self.alr.per_sample_ns:,.1f} |\n"
            f"| overhead | {self.overhead_pct:+.2f}% |\n"
            f"| ratio (ALR/native) | {self.ratio:.3f}× |\n"
            f"| class | {cls} |\n"
            f"| path-mediation traps / rewrites | {traps} / {rewrites} |\n"
            f"| verdict | **{verdict}** |\n"
        )


def compute_overhead(
    native: Measurement,
    alr: Measurement,
    *,
    syscall_light: bool = True,
    traps: int | None = None,
    rewrites: int | None = None,
    binary: str = "",
) -> OverheadResult:
    """Compare per-sample wall-clock. overhead_pct = (alr-native)/native*100.

    For syscall-light workloads, `passes_target` is overhead_pct <= 5%. For
    syscall-storm workloads, `passes_target` is always True (reported, not gated).
    """
    n = native.per_sample_ns
    a = alr.per_sample_ns
    overhead_pct = (a - n) / n * 100.0
    ratio = a / n
    passes = (overhead_pct <= SYSCALL_LIGHT_MAX_OVERHEAD_PCT) if syscall_light else True
    return OverheadResult(
        native=native,
        alr=alr,
        overhead_pct=overhead_pct,
        ratio=ratio,
        syscall_light=syscall_light,
        passes_target=passes,
        traps=traps,
        rewrites=rewrites,
        binary=binary,
    )
