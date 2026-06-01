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
# The alr-gles-cube guest binary instead prints the renderer QUOTED, on a line
# like:  alr-gles-cube: EGL 1.4, GL_RENDERER="Mali-G615 ...", GL_VERSION="...".
# Match the quoted form first (so we capture only what's inside the quotes and
# don't swallow the trailing `, GL_VERSION=...`), then fall back to the bare
# `:`/`=` glmark2 form (rest of line).
_RENDERER_QUOTED = re.compile(r"GL_RENDERER\s*[:=]\s*\"([^\"]*)\"", re.IGNORECASE)
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


def parse_gl_renderer(text: str) -> str | None:
    """Return the GL_RENDERER value (unquoted, stripped), or None.

    Handles three on-the-wire forms seen in captured logcat:
      * glmark2 OpenGL info:  ``GL_RENDERER:   Mali-G615 (...)``
      * alt harness:          ``GL_RENDERER = Mali-G715``
      * alr-gles-cube guest:  ``... GL_RENDERER="Mali-G615 ...", GL_VERSION=...``

    The quoted alr-gles-cube form is matched first so only the text inside the
    quotes is returned (not the trailing ``, GL_VERSION=...``). Empty quotes
    ("") yield None. If multiple lines match, the first occurrence wins.
    """
    mq = _RENDERER_QUOTED.search(text)
    if mq is not None:
        value = mq.group(1).strip()
        return value or None
    m = _RENDERER.search(text)
    if m is None:
        return None
    # Bare form runs to end of line; trim a trailing quote if one slipped in.
    return m.group(1).strip().strip('"').strip() or None


def parse_glmark2_renderer(text: str) -> str | None:
    """Return the `GL_RENDERER` value (unquoted, stripped), or None.

    Backwards-compatible alias for :func:`parse_gl_renderer`, which also
    understands the quoted alr-gles-cube renderer line.
    """
    return parse_gl_renderer(text)


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


def parse_gpu_from_report(text: str) -> dict | None:
    """Pull a GPU score + renderer out of a captured logcat / report blob.

    Returns ``{"score": int|None, "renderer": str|None}`` when *either* a
    ``glmark2 Score:`` / bare ``Score:`` line OR a ``GL_RENDERER`` line (glmark2
    or quoted alr-gles-cube form) is present, else ``None``. Either field may be
    None individually if only one of the two markers appears in the blob.
    """
    score = parse_glmark2_score(text)
    renderer = parse_gl_renderer(text)
    if score is None and renderer is None:
        return None
    return {"score": score, "renderer": renderer}


def gpu_result_from_reports(
    alr_text: str,
    mali_text: str,
    *,
    min_ratio: float = GPU_ACCEL_MIN_RATIO,
    alr_label: str = "ALR",
    mali_label: str = "Mali-direct",
) -> GpuBenchResult:
    """Build a :class:`GpuBenchResult` from two captured report blobs.

    Parses each blob for a glmark2 / alr-gles-cube score + renderer, constructs
    a :class:`GpuScore` for each side, and runs :func:`compute_gpu_ratio`. The
    §0 GPU gate (ratio >= ``min_ratio`` AND a non-software renderer) is encoded
    by ``compute_gpu_ratio`` and surfaced via ``result.passes_target``.

    Raises :class:`ValueError` with a clear message if a score is missing from
    either blob (a bench run with no score line cannot be rated).
    """
    alr_parsed = parse_gpu_from_report(alr_text)
    mali_parsed = parse_gpu_from_report(mali_text)
    if alr_parsed is None or alr_parsed["score"] is None:
        raise ValueError(
            f"{alr_label}: no glmark2 score found in the captured report "
            "(expected a 'glmark2 Score: N' or 'Score: N' line)"
        )
    if mali_parsed is None or mali_parsed["score"] is None:
        raise ValueError(
            f"{mali_label}: no glmark2 score found in the captured report "
            "(expected a 'glmark2 Score: N' or 'Score: N' line)"
        )
    alr = GpuScore(
        label=alr_label,
        score=alr_parsed["score"],
        renderer=alr_parsed["renderer"] or "",
    )
    mali_direct = GpuScore(
        label=mali_label,
        score=mali_parsed["score"],
        renderer=mali_parsed["renderer"] or "",
    )
    return compute_gpu_ratio(alr, mali_direct, min_ratio=min_ratio)
