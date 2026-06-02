# Host gate for the VK-M3 render BREADTH (DRAW) marshalling path.
#
# G3-VK lane (auto/r12-g3-vk): the marshalled Vulkan render path was extended beyond a
# bare clear to a real DRAW — a graphics pipeline (handcrafted SPIR-V vert+frag), a
# vertex buffer, and one vkCmdDraw of a triangle, decoded host-side and (on device)
# replayed on real Mali libvulkan. This test enforces, ON THE HOST with NO Vulkan SDK:
#   (1) the wire contract for the new draw op/seam stays in sync across the three
#       alr_gpu_vk_*.hpp headers (proto opcode, decode case + real path, probe entry), and
#   (2) the synthetic wire round trip actually DRAWS — it compiles a tiny driver against
#       the headers and asserts "ALR VK DRAW MARSHAL: PASS" + that the read-back center
#       pixel is the triangle color and NOT the clear background (the breadth proof).
#
# The on-device proof (real Mali pipeline + vkCmdDraw into an AHB) is DEVICE-PENDING and
# rides run_vk_draw_mali_probe()/the gating line "ALR VK DRAW MARSHAL: PASS" once WS-1
# wires the JNI entry (this test does not add JNI — see the header comment for WS-1).

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
GPU = ROOT / "app/src/main/cpp/alr_gpu"
PROTO = GPU / "alr_gpu_vk_proto.hpp"
DECODE = GPU / "alr_gpu_vk_decode.hpp"
PROBE = GPU / "alr_gpu_vk_marshal_probe.hpp"
NATIVE_TEST = ROOT / "tests/native_vk_marshal_test.cpp"


def _cxx():
    for c in (os.environ.get("CXX"), "g++", "c++", "clang++"):
        if c and shutil.which(c):
            return c
    return None


def test_proto_declares_draw_op_and_builder():
    text = PROTO.read_text()
    # new draw opcode wires into the existing 200+ Vulkan op space, after the clear path.
    assert "ALR_VK_OP_CMD_BEGIN_DRAW = 217" in text
    assert "alr_vk_enc_cmd_begin_draw" in text
    # a pipeline-stage failure code for the draw path (clear path never returns it).
    assert "ALR_VK_RENDER_PIPELINE = 7" in text


def test_decode_has_draw_record_seam_and_real_path():
    text = DECODE.read_text()
    # the record carries a draw flag + the fixed triangle color constants.
    assert "bool is_draw" in text
    for c in ("kAlrVkTriColorR", "kAlrVkTriColorG", "kAlrVkTriColorB"):
        assert c in text
    # handcrafted SPIR-V is embedded (vert + frag) with a documented glslc recipe.
    assert "kAlrVkTriVertSpv" in text and "kAlrVkTriFragSpv" in text
    assert "glslc" in text and "--target-env=vulkan1.1" in text
    # the provider gained a draw_submit seam, and QUEUE_SUBMIT dispatches on is_draw.
    assert "draw_submit" in text
    assert "ALR_VK_OP_CMD_BEGIN_DRAW" in text
    # the real Mali path actually builds a pipeline + vertex buffer + draws.
    assert "vk_real_draw_submit" in text
    for call in (
        "vkCreateShaderModule",
        "vkCreatePipelineLayout",
        "vkCreateGraphicsPipelines",
        "vkCmdBindVertexBuffers",
        "vkCmdDraw(cmd, 3",
    ):
        assert call in text, call


def test_spirv_blobs_are_wellformed_words():
    """The embedded SPIR-V must start with the SPIR-V magic word (0x07230203)."""
    text = DECODE.read_text()
    assert text.count("0x07230203u") >= 2  # vert + frag both begin with the magic


def test_probe_exposes_draw_entrypoints_for_ws1():
    text = PROBE.read_text()
    assert "build_vk_draw_request" in text
    assert "run_vk_draw_wire_probe" in text
    assert "run_vk_draw_mali_probe" in text  # device-mode entry (ALR_VK_DECODE_REAL)
    assert "ALR VK DRAW MARSHAL:" in text
    # WS-1 handoff comment is present (no JNI added in this header).
    assert "nativeAlrGpuVkDrawProbe" in text


def test_native_test_drives_the_draw_path():
    text = NATIVE_TEST.read_text()
    assert "build_vk_draw_request" in text
    assert "run_vk_draw_wire_probe" in text
    assert "ALR VK DRAW MARSHAL: PASS" in text
    assert "NOT the clear background" in text  # the breadth assertion


def test_wire_draw_roundtrip_passes_when_compiled():
    """Compile a tiny driver against the headers (NO Vulkan SDK) and run the synthetic
    DRAW round trip; it must PASS and the center pixel must be the triangle, not the bg."""
    cxx = _cxx()
    if cxx is None:
        pytest.skip("no host C++ compiler available")
    driver = r"""
#include "alr_gpu/alr_gpu_vk_marshal_probe.hpp"
#include <cstdio>
#include <string>
using namespace alr::gpu;
int main() {
    const std::string r = run_vk_draw_wire_probe();
    const bool pass = r.find("ALR VK DRAW MARSHAL: PASS") != std::string::npos;
    // must report a real triangle pixel that is not the near-black background.
    const bool not_bg = r.find("not-background=yes") != std::string::npos;
    const bool match = r.find("triangle match=yes") != std::string::npos;
    if (!(pass && not_bg && match)) { printf("DRIVER-FAIL\n%s\n", r.c_str()); return 1; }
    printf("DRIVER-OK\n");
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "drv.cpp"
        src.write_text(driver)
        exe = Path(td) / "drv"
        cp = subprocess.run(
            [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "app/src/main/cpp"), str(src), "-o", str(exe)],
            capture_output=True, text=True,
        )
        assert cp.returncode == 0, f"compile failed:\n{cp.stderr}"
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        assert run.returncode == 0, f"draw round trip failed:\n{run.stdout}\n{run.stderr}"
        assert "DRIVER-OK" in run.stdout
