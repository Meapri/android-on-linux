"""M2 — GPU bench model: glmark2 under ALR vs a native Mali-direct baseline. Pure.

Host scaffolding only. This module just PARSES glmark2 output and computes the
acceleration ratio + verdict — it has no I/O and never edits the app / GPU sources
(L1/L2/L3 ownership). The real device numbers depend on WS-2's "glmark2-on-Mali"
milestone landing; until then callers feed captured glmark2 text (or synthetic
scores) into the pure functions here.

The orchestration-5session-plan §0 GPU target is that the ALR (guest glibc driving
the real Mali GPU via the gfxstream-style boundary) glmark2 score reaches at least
~0.70 of the native Mali-direct score, AND that the reported renderer is real
hardware (NOT a software rasterizer such as swiftshader/llvmpipe/lavapipe). A score
that only clears the ratio by falling back to swrast does NOT pass the target.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass

from tools.device_evidence import renderer_is_software

# orchestration-5session-plan.md §0: GPU acceleration ratio target.
GPU_ACCEL_MIN_RATIO = 0.70

# glmark2 prints a final results block, e.g.
#   =======================================================
#                                   glmark2 Score: 1234
#   =======================================================
# Older / wrapped output sometimes prints just `Score: 1234`. Match either.
_SCORE = re.compile(r"(?:glmark2\s+)?Score:\s*(\d+)", re.IGNORECASE)
# glmark2 OpenGL Information block, e.g. `    GL_RENDERER:  Mali-G615 ...`
# Some harnesses emit `GL_RENDERER = Mali-G615 ...`. Accept `:` or `=`.
_RENDERER = re.compile(r"GL_RENDERER\s*[:=]\s*(.+)", re.IGNORECASE)


def parse_glmark2_score(text: str) -> int | None:
    """Return the integer glmark2 score, or None if no score line is present.

    Prefers the canonical `glmark2 Score:` line; falls back to a bare `Score:`.
    If multiple matches appear, the last one wins (the final summary block).
    """
    matches = _SCORE.findall(text)
    if not matches:
        return None
    return int(matches[-1])


def parse_glmark2_renderer(text: str) -> str | None:
    """Return the `GL_RENDERER` value (to end of line, stripped), or None."""
    m = _RENDERER.search(text)
    if m is None:
        return None
    return m.group(1).strip()


@dataclass(frozen=True)
class GpuScore:
    label: str
    score: int
    renderer: str = ""

    def __post_init__(self) -> None:
        if self.score <= 0:
            raise ValueError(f"{self.label}: score must be > 0, got {self.score}")


@dataclass(frozen=True)
class GpuBenchResult:
    alr: GpuScore
    mali_direct: GpuScore
    ratio: float
    min_ratio: float
    software_renderer: bool
    passes_target: bool

    def to_dict(self) -> dict:
        return {
            "alr_label": self.alr.label,
            "alr_score": self.alr.score,
            "alr_renderer": self.alr.renderer,
            "mali_direct_label": self.mali_direct.label,
            "mali_direct_score": self.mali_direct.score,
            "mali_direct_renderer": self.mali_direct.renderer,
            "ratio": round(self.ratio, 4),
            "min_ratio": self.min_ratio,
            "software_renderer": self.software_renderer,
            "passes_target": self.passes_target,
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True)

    def to_markdown(self) -> str:
        verdict = "PASS" if self.passes_target else "FAIL"
        software = "yes" if self.software_renderer else "no"
        renderer = self.alr.renderer or "(unknown)"
        return (
            "### GPU bench — glmark2 ALR vs Mali-direct\n\n"
            "| metric | value |\n"
            "|--------|-------|\n"
            f"| ALR score | {self.alr.score} |\n"
            f"| Mali-direct score | {self.mali_direct.score} |\n"
            f"| ratio | {self.ratio * 100:.1f}% (target >= {self.min_ratio * 100:.0f}%) |\n"
            f"| renderer | {renderer} |\n"
            f"| software renderer | {software} |\n"
            f"| verdict | **{verdict}** |\n"
        )


def compute_gpu_ratio(
    alr: GpuScore,
    mali_direct: GpuScore,
    *,
    min_ratio: float = GPU_ACCEL_MIN_RATIO,
) -> GpuBenchResult:
    """Compare the ALR glmark2 score against the native Mali-direct baseline.

    ratio = alr.score / mali_direct.score. The target passes when the ratio meets
    `min_ratio` AND the ALR renderer is real hardware (not a software rasterizer).
    An empty ALR renderer string is treated as not-software (renderer unknown).
    Raises ValueError if the Mali-direct baseline score is not positive.
    """
    if mali_direct.score <= 0:
        raise ValueError(
            f"mali_direct baseline score must be > 0, got {mali_direct.score}"
        )
    ratio = alr.score / mali_direct.score
    software = renderer_is_software(alr.renderer) if alr.renderer else False
    passes = (ratio >= min_ratio) and (not software)
    return GpuBenchResult(
        alr=alr,
        mali_direct=mali_direct,
        ratio=ratio,
        min_ratio=min_ratio,
        software_renderer=software,
        passes_target=passes,
    )
