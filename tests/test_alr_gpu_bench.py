"""WS-5 host tests: M2 GPU bench model (bench/gpu_bench.py).

Pure parsing + ratio/verdict checks; no device, no I/O.
"""
import pytest

from bench.gpu_bench import (
    GPU_ACCEL_MIN_RATIO,
    GpuScore,
    compute_gpu_ratio,
    gpu_result_from_reports,
    parse_gl_renderer,
    parse_glmark2_renderer,
    parse_glmark2_score,
    parse_gpu_from_report,
)


# Realistic glmark2 tail: OpenGL info block + per-benchmark lines + final score.
GLMARK2_SAMPLE = """\
=======================================================
    glmark2 2021.02
=======================================================
    OpenGL Information
    GL_VENDOR:     ARM
    GL_RENDERER:   Mali-G615 (Panfrost / mt6878)
    GL_VERSION:    OpenGL ES 3.2
=======================================================
[build] use-vbo=false: FPS: 210 FrameTime: 4.762 ms
[build] use-vbo=true: FPS: 305 FrameTime: 3.279 ms
[texture] texture-filter=nearest: FPS: 290 FrameTime: 3.448 ms
=======================================================
                                  glmark2 Score: 1234
=======================================================
"""

GLMARK2_NO_SCORE = """\
=======================================================
    OpenGL Information
    GL_RENDERER:   Mali-G615 (Panfrost / mt6878)
=======================================================
[build] use-vbo=false: FPS: 210 FrameTime: 4.762 ms
"""

# The alr-gles-cube guest binary prints the renderer QUOTED on one line:
#   alr-gles-cube: EGL X.Y, GL_RENDERER="<value>", GL_VERSION="<value>", frames=N
# (see app/src/main/cpp/alr_gpu/guest_shim/alr-gles-cube.c).
ALR_CUBE_SAMPLE = (
    'alr-gles-cube: EGL 1.4, GL_RENDERER="Mali-G615 (Panfrost / mt6878)", '
    'GL_VERSION="OpenGL ES 3.2", frames=120\n'
    "alr-gles-cube: done (120 frames)\n"
)


def test_parse_score_from_realistic_sample():
    assert parse_glmark2_score(GLMARK2_SAMPLE) == 1234


def test_parse_score_bare_score_line():
    assert parse_glmark2_score("some preamble\nScore: 4096\n") == 4096


def test_parse_score_missing_returns_none():
    assert parse_glmark2_score(GLMARK2_NO_SCORE) is None
    assert parse_glmark2_score("no numbers here at all") is None


def test_parse_renderer_mali():
    renderer = parse_glmark2_renderer(GLMARK2_SAMPLE)
    assert renderer == "Mali-G615 (Panfrost / mt6878)"
    assert "Mali-G615" in renderer


def test_parse_renderer_equals_form():
    assert parse_glmark2_renderer("GL_RENDERER = Mali-G715") == "Mali-G715"


def test_parse_renderer_missing_returns_none():
    assert parse_glmark2_renderer("no renderer line") is None


def test_compute_ratio_passes():
    result = compute_gpu_ratio(
        GpuScore("ALR", 900, renderer="Mali-G615"),
        GpuScore("Mali-direct", 1000, renderer="Mali-G615"),
    )
    assert result.ratio == pytest.approx(0.9)
    assert result.software_renderer is False
    assert result.passes_target is True
    assert result.min_ratio == GPU_ACCEL_MIN_RATIO


def test_software_renderer_gate_fails_even_when_ratio_high():
    # ratio 2.0 clears the threshold, but a software rasterizer must NOT pass.
    result = compute_gpu_ratio(
        GpuScore("ALR", 2000, renderer="llvmpipe"),
        GpuScore("Mali-direct", 1000, renderer="Mali-G615"),
    )
    assert result.ratio == pytest.approx(2.0)
    assert result.software_renderer is True
    assert result.passes_target is False


def test_low_ratio_fails():
    result = compute_gpu_ratio(
        GpuScore("ALR", 500, renderer="Mali-G615"),
        GpuScore("Mali-direct", 1000, renderer="Mali-G615"),
    )
    assert result.ratio == pytest.approx(0.5)
    assert result.ratio < GPU_ACCEL_MIN_RATIO
    assert result.software_renderer is False
    assert result.passes_target is False


def test_gpuscore_rejects_nonpositive_score():
    with pytest.raises(ValueError):
        GpuScore("bad", 0)
    with pytest.raises(ValueError):
        GpuScore("bad", -5)


def test_compute_rejects_zero_mali_baseline():
    # GpuScore guards score > 0, so build a baseline via object bypass to hit the
    # compute_gpu_ratio guard directly.
    bad_baseline = object.__new__(GpuScore)
    object.__setattr__(bad_baseline, "label", "Mali-direct")
    object.__setattr__(bad_baseline, "score", 0)
    object.__setattr__(bad_baseline, "renderer", "Mali-G615")
    with pytest.raises(ValueError):
        compute_gpu_ratio(GpuScore("ALR", 900, renderer="Mali-G615"), bad_baseline)


def test_to_markdown_smoke():
    result = compute_gpu_ratio(
        GpuScore("ALR", 900, renderer="Mali-G615"),
        GpuScore("Mali-direct", 1000, renderer="Mali-G615"),
    )
    md = result.to_markdown()
    assert ("PASS" in md) or ("FAIL" in md)
    assert "ratio" in md


def test_to_dict_and_json_roundtrip():
    import json

    result = compute_gpu_ratio(
        GpuScore("ALR", 900, renderer="Mali-G615"),
        GpuScore("Mali-direct", 1000, renderer="Mali-G615"),
    )
    d = result.to_dict()
    assert d["alr_score"] == 900
    assert d["mali_direct_score"] == 1000
    assert d["passes_target"] is True
    assert json.loads(result.to_json()) == d


# --- alr-gles-cube quoted GL_RENDERER form -------------------------------------


def test_parse_renderer_alr_cube_quoted_form():
    # The quoted value must be returned WITHOUT quotes and WITHOUT the trailing
    # `, GL_VERSION=...` text on the same line.
    renderer = parse_gl_renderer(ALR_CUBE_SAMPLE)
    assert renderer == "Mali-G615 (Panfrost / mt6878)"
    assert '"' not in renderer
    assert "GL_VERSION" not in renderer


def test_parse_glmark2_renderer_alias_handles_cube_form():
    # The legacy alias must also understand the quoted alr-gles-cube line.
    assert parse_glmark2_renderer(ALR_CUBE_SAMPLE) == "Mali-G615 (Panfrost / mt6878)"


def test_parse_gl_renderer_still_handles_glmark2_and_equals():
    assert parse_gl_renderer(GLMARK2_SAMPLE) == "Mali-G615 (Panfrost / mt6878)"
    assert parse_gl_renderer("GL_RENDERER = Mali-G715") == "Mali-G715"
    assert parse_gl_renderer("no renderer line") is None


def test_parse_gl_renderer_empty_quotes_returns_none():
    assert parse_gl_renderer('alr-gles-cube: GL_RENDERER="", frames=1') is None


# --- parse_gpu_from_report -----------------------------------------------------


def test_parse_gpu_from_report_score_and_renderer():
    blob = "preamble\nglmark2 Score: 1234\nGL_RENDERER: Mali-G615\n"
    parsed = parse_gpu_from_report(blob)
    assert parsed == {"score": 1234, "renderer": "Mali-G615"}


def test_parse_gpu_from_report_glmark2_block():
    parsed = parse_gpu_from_report(GLMARK2_SAMPLE)
    assert parsed["score"] == 1234
    assert "Mali-G615" in parsed["renderer"]


def test_parse_gpu_from_report_cube_quoted():
    # alr-gles-cube has no score line, only the quoted renderer.
    parsed = parse_gpu_from_report(ALR_CUBE_SAMPLE)
    assert parsed == {"score": None, "renderer": "Mali-G615 (Panfrost / mt6878)"}


def test_parse_gpu_from_report_score_only():
    parsed = parse_gpu_from_report("Score: 555\n")
    assert parsed == {"score": 555, "renderer": None}


def test_parse_gpu_from_report_neither_returns_none():
    assert parse_gpu_from_report("nothing relevant here") is None


# --- gpu_result_from_reports ---------------------------------------------------


def test_gpu_result_from_reports_pass():
    alr_text = "glmark2 Score: 800\nGL_RENDERER: Mali-G615 (Panfrost / mt6878)\n"
    mali_text = "glmark2 Score: 1000\nGL_RENDERER: Mali-G615 (Panfrost / mt6878)\n"
    result = gpu_result_from_reports(alr_text, mali_text)
    assert result.ratio == pytest.approx(0.8)
    assert result.ratio >= GPU_ACCEL_MIN_RATIO
    assert result.software_renderer is False
    assert result.passes_target is True
    assert result.alr.renderer == "Mali-G615 (Panfrost / mt6878)"


def test_gpu_result_from_reports_software_fails():
    # ALR fell back to a software rasterizer: must FAIL even with a fine ratio.
    alr_text = "glmark2 Score: 900\nGL_RENDERER: llvmpipe (LLVM 17)\n"
    mali_text = "glmark2 Score: 1000\nGL_RENDERER: Mali-G615\n"
    result = gpu_result_from_reports(alr_text, mali_text)
    assert result.ratio == pytest.approx(0.9)
    assert result.software_renderer is True
    assert result.passes_target is False


def test_gpu_result_from_reports_missing_alr_score_raises():
    with pytest.raises(ValueError, match="ALR: no glmark2 score"):
        gpu_result_from_reports(
            "GL_RENDERER: Mali-G615 (no score)\n",
            "glmark2 Score: 1000\nGL_RENDERER: Mali-G615\n",
        )


def test_gpu_result_from_reports_missing_mali_score_raises():
    with pytest.raises(ValueError, match="Mali-direct: no glmark2 score"):
        gpu_result_from_reports(
            "glmark2 Score: 800\nGL_RENDERER: Mali-G615\n",
            "GL_RENDERER: Mali-G615 (no score)\n",
        )
