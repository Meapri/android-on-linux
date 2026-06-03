# Host gate for the Vulkan CMD-LOG recording band — the vkCmd* COMMAND-RECORDING mechanism,
# the hard part of GPU full-passthrough (memory alr-gpu-native-track).
#
# vkCmd* calls do NOT execute when called — they RECORD into a command buffer; the GPU work
# happens at vkQueueSubmit. The cmd-log mechanism makes each vkCmd* a GUEST-LOCAL append into
# a per-command-buffer byte log in the same-process MAP_SHARED arena (NO ring op per call);
# at vkQueueSubmit the guest ships the cmd-buffer vid list (+ wait/signal semaphores + fence)
# over the ring, and the host REPLAYS each cmd-buffer's arena log into a REAL Mali
# VkCommandBuffer (translate guest vids -> real handles, call the real vkCmd*) then real
# vkQueueSubmit on the owner thread.
#
# This test enforces, ON THE HOST:
#   (1) the wire round trip actually marshals — it compiles + runs the host wire test
#       (tests/native_vk_cmdlog_test.cpp) and asserts "ALL PASS": a recorded frame
#       (begin-renderpass + bind + draw + end + transfer + barrier + dispatch) decodes back
#       to the same opcodes + handles, and the submit/fence/semaphore/wait ring ops round
#       trip — all with NO Vulkan SDK (a synthetic Mali provider);
#   (2) the REAL-Mali replay path (the leg that calls the real vkCmd* on vendor libvulkan)
#       COMPILES against the NDK <vulkan/vulkan.h> with the SAME arm64-android clang the
#       device build uses (compile-only; Mali libvulkan isn't on the host) — so every vkCmd*
#       signature + VkStruct field + the fence/semaphore/submit path is type-checked; and
#   (3) the band BOUNDARY: the cmd-log ops ride the shared u8 escape (230) on a DISJOINT u16
#       sub-opcode band (0x4000), coexisting with the create-forwards codegen band (1..) via a
#       second registered dispatcher — they merge cleanly with the concurrent create-forwards
#       agent (distinct files + opcode ranges).
#
# The on-device proof (ANGLE records a frame's vkCmd* -> submit -> Mali render -> readback) is
# DEVICE-PENDING; see the device-verify plan in the task report.

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp"
GPU = CPP / "alr_gpu"
CMDLOG = GPU / "alr_gpu_vk_cmdlog.hpp"
CMDLOG_REAL = GPU / "alr_gpu_vk_cmdlog_real.hpp"
CMD_DISPATCH = GPU / "alr_gpu_vk_cmd_dispatch.hpp"
CMD_DISPATCH_REAL = GPU / "alr_gpu_vk_cmd_dispatch_real.hpp"
DECODE = GPU / "alr_gpu_vk_decode.hpp"
NATIVE_TEST = ROOT / "tests" / "native_vk_cmdlog_test.cpp"


def _cxx():
    for c in (os.environ.get("CXX"), "g++", "c++", "clang++"):
        if c and shutil.which(c):
            return c
    return None


def _ndk_clangxx():
    """The arm64-android clang++ from the pinned NDK (the device build's compiler), or None."""
    candidates = []
    home = Path.home()
    for base in (home / "Library/Android/sdk/ndk", home / "Android/Sdk/ndk",
                 Path(os.environ.get("ANDROID_NDK_HOME", "")) if os.environ.get("ANDROID_NDK_HOME") else None):
        if base and base.exists():
            candidates += list(base.glob("*/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android*-clang++"))
    # Prefer the highest API-level clang (sorted lexically is fine for our purpose).
    candidates = sorted(candidates)
    return str(candidates[-1]) if candidates else None


def test_cmdlog_headers_present():
    for f in (CMDLOG, CMDLOG_REAL, CMD_DISPATCH, CMD_DISPATCH_REAL):
        assert f.exists(), f"cmd-log header missing: {f}"
    # The real-Mali legs are hand-written + ALR_VK_DECODE_REAL-gated (SDK-free default build).
    for f in (CMDLOG_REAL, CMD_DISPATCH_REAL):
        assert "ALR_VK_DECODE_REAL" in f.read_text(), f"{f} not gated on ALR_VK_DECODE_REAL"


def test_cmdlog_band_is_disjoint_from_create_forwards():
    txt = CMDLOG.read_text()
    # The cmd-log RING ops ride the shared escape on a DISJOINT 0x4000 sub-op band (the
    # create-forwards codegen owns 1..). 0x4000 can never collide with that auto-grown band.
    assert "ALR_VK_CMD_SUBOP_BASE = 0x4000" in txt
    assert "ALR_VK_CMD_SUBOP_QUEUE_SUBMIT" in txt
    # The cmd-OPCODES (inside the per-command-buffer log) cover the core vkCmd* ANGLE records.
    for op in ("ALR_VK_CMD_BEGIN_RENDER_PASS", "ALR_VK_CMD_BIND_PIPELINE",
               "ALR_VK_CMD_BIND_DESCRIPTOR_SETS", "ALR_VK_CMD_BIND_VERTEX_BUFFERS",
               "ALR_VK_CMD_BIND_INDEX_BUFFER", "ALR_VK_CMD_DRAW", "ALR_VK_CMD_DRAW_INDEXED",
               "ALR_VK_CMD_DRAW_INDIRECT", "ALR_VK_CMD_SET_VIEWPORT", "ALR_VK_CMD_SET_SCISSOR",
               "ALR_VK_CMD_PIPELINE_BARRIER", "ALR_VK_CMD_COPY_BUFFER", "ALR_VK_CMD_COPY_IMAGE",
               "ALR_VK_CMD_COPY_BUFFER_TO_IMAGE", "ALR_VK_CMD_PUSH_CONSTANTS",
               "ALR_VK_CMD_CLEAR_ATTACHMENTS", "ALR_VK_CMD_DISPATCH", "ALR_VK_CMD_NEXT_SUBPASS"):
        assert op in txt, f"{op} missing from the cmd-opcode set"


def test_decode_chains_two_dispatchers_by_band():
    # The hand-written decoder routes the escape between the two bands by PEEKING the u16
    # sub-op (the cmd-log band gets a SECOND registered dispatcher; the create-forwards one is
    # untouched) — so the two agents keep distinct files + opcode ranges.
    dec = DECODE.read_text()
    assert "set_vk_cmd_dispatch" in dec and "vk_cmd_dispatch()" in dec
    assert "sub >= 0x4000u" in dec, "default case must route the 0x4000 band to the cmd dispatcher"
    # The cmd dispatcher self-registers (just including its header wires the band in).
    disp = CMD_DISPATCH.read_text()
    assert "set_vk_cmd_dispatch(&vk_cmd_dispatch_adapter)" in disp


def test_submit_reads_log_from_arena_not_the_ring():
    # The recorded BYTES never cross the ring: the submit op ships each cmd-buffer's ARENA
    # OFFSET, and the host resolves it to a pointer + bounds it against the arena size.
    disp = CMD_DISPATCH.read_text()
    assert "alr_vk_arena_ptr(log_off)" in disp
    assert "log_off + log_len > alr_vk_arena_size()" in disp  # over-read guard


def test_cmd_real_translates_handles_and_calls_real_vkcmd():
    real = CMDLOG_REAL.read_text()
    # The real path translates guest vids -> real Mali handles (the composition seam with the
    # create-forwards agent: it registers handles, this consumes them) and calls the real
    # vkCmd*. Spot-check a representative set.
    for fn in ("vkCmdBeginRenderPass", "vkCmdBindPipeline", "vkCmdBindVertexBuffers",
               "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdPipelineBarrier", "vkCmdCopyBufferToImage",
               "vkCmdPushConstants", "vkCmdDispatch"):
        assert fn in real, f"real replay must call {fn}"
    # Handle resolvers fail-safe (an unresolved required handle aborts, never passes a bogus
    # handle to Mali).
    assert "ALR_VK_CMD_REPLAY_HANDLE" in real
    assert "cmd_register_pipeline" in real  # the seam the create-forwards agent fills


@pytest.mark.skipif(_cxx() is None, reason="no host C++ compiler")
def test_cmdlog_wire_roundtrip_passes():
    cxx = _cxx()
    out = ROOT / "build" / "test-vk-cmdlog.bin"
    out.parent.mkdir(exist_ok=True)
    comp = subprocess.run(
        [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Iapp/src/main/cpp",
         str(NATIVE_TEST), "-o", str(out)],
        capture_output=True, text=True, cwd=str(ROOT))
    assert comp.returncode == 0, f"compile failed:\n{comp.stderr}"
    run = subprocess.run([str(out)], capture_output=True, text=True, cwd=str(ROOT))
    assert run.returncode == 0 and "ALL PASS" in run.stdout, \
        f"cmd-log wire test failed:\n{run.stdout}\n{run.stderr}"
    out.unlink(missing_ok=True)


def test_real_mali_replay_compiles_against_ndk_vulkan():
    """The real-Mali replay leg (the vkCmd* calls) MUST type-check against the NDK Vulkan
    headers with the device build's own arm64-android clang. This is the syntax-verify of the
    real replay path (Mali libvulkan isn't on the host, so compile-only, no link)."""
    clang = _ndk_clangxx()
    if clang is None:
        pytest.skip("no NDK arm64 clang++ found (set ANDROID_NDK_HOME)")
    src = ROOT / "build" / "cmdlog_real_probe.cpp"
    src.parent.mkdir(exist_ok=True)
    src.write_text(
        "#define ALR_VK_DECODE_REAL 1\n"
        '#include "alr_gpu/alr_gpu_vk_cmd_dispatch.hpp"\n'
        '#include "alr_gpu/alr_gpu_vk_cmdlog.hpp"\n'
        "using namespace alr::gpu;\n"
        "void force_instantiate(VkDecodeState& st, VkDevice dev, VkCommandBuffer cmd,\n"
        "                       const uint8_t* log, uint32_t n) {\n"
        "    (void)cmd_replay_log_real(log, n, st, dev, cmd);\n"
        "    int rr = 0; VkCmdSubmitInfo info;\n"
        "    (void)cmd_real_queue_submit(st, info, &rr);\n"
        "    (void)cmd_real_create_fence(st, 1, 2, 0);\n"
        "    (void)cmd_real_wait_fences(st, 1, 1, 0, {});\n"
        "    (void)cmd_real_reset_fences(st, 1, {});\n"
        "    (void)cmd_real_create_semaphore(st, 1, 3, 0);\n"
        "    (void)cmd_real_queue_wait_idle(st, 4);\n"
        "    (void)cmd_real_device_wait_idle(st, 1);\n"
        "    cmd_register_pipeline(st, 5, VK_NULL_HANDLE);\n"
        "}\n")
    obj = ROOT / "build" / "cmdlog_real_probe.o"
    comp = subprocess.run(
        [clang, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-c",
         "-Iapp/src/main/cpp", str(src), "-o", str(obj)],
        capture_output=True, text=True, cwd=str(ROOT))
    assert comp.returncode == 0, (
        "real-Mali cmd-log replay does NOT compile against the NDK Vulkan headers:\n"
        f"{comp.stderr}")
    src.unlink(missing_ok=True)
    obj.unlink(missing_ok=True)
