# Host gate for the GENERATED Vulkan passthrough render batch (the codegen track).
#
# tools/gen_vk_passthrough.py parses the Vulkan registry (tools/vk_registry/vk.xml, pinned
# to Vulkan-Headers v1.3.275 == the NDK 27 VK_HEADER_VERSION) and EMITS, per entrypoint,
# the four matched halves of the same-process thin-wire marshalling: the guest encoder
# (alr_gpu/generated/alr_gpu_vk_gen_proto.hpp), the host decode-and-call-on-real-Mali
# (alr_gpu_vk_gen_decode.hpp), the ICD dispatch entry (alr_gpu_vk_gen_icd.inc +
# _icd_runtime.inc), and (the device half) the hand-written real-Mali bodies it calls
# (alr_gpu_vk_gen_real.hpp). The first render batch is device memory (the same-process
# MAP_SHARED arena -> zero-copy vkMapMemory), buffers, images, image views + reqs/bind.
#
# This test enforces, ON THE HOST with NO Vulkan SDK:
#   (1) the codegen is DETERMINISTIC + the committed generated files are in sync with the
#       tool + vk.xml (gen --check passes — a stale checkout fails the gate);
#   (2) the generated wire round trip actually marshals — it compiles + runs the host wire
#       test (tests/native_vk_gen_passthrough_test.cpp) and asserts "ALL PASS", including
#       the same-process arena handing back a real, writable vkMapMemory pointer; and
#   (3) the structural contract holds: the generated ops ride the u8 escape + u16 sub-op
#       band (never colliding with the hand-written 0..229 band), and the guest ICD wires
#       the new entrypoints (but NOT command pool, which is hand-written).
#
# The on-device proof (the generated entrypoints replayed on real Mali, driven by ANGLE
# once the WSI agent unblocks it) is DEVICE-PENDING; see the device-iterate plan in the
# codegen tool's header + the task report.

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "gen_vk_passthrough.py"
VK_XML = ROOT / "tools" / "vk_registry" / "vk.xml"
GEN = ROOT / "app/src/main/cpp/alr_gpu/generated"
GEN_PROTO = GEN / "alr_gpu_vk_gen_proto.hpp"
GEN_DECODE = GEN / "alr_gpu_vk_gen_decode.hpp"
GEN_ICD = GEN / "alr_gpu_vk_gen_icd.inc"
GEN_ICD_RT = GEN / "alr_gpu_vk_gen_icd_runtime.inc"
GEN_REAL = GEN / "alr_gpu_vk_gen_real.hpp"
GEN_ARENA = GEN / "alr_gpu_vk_arena.hpp"
NATIVE_TEST = ROOT / "tests" / "native_vk_gen_passthrough_test.cpp"


def _cxx():
    for c in (os.environ.get("CXX"), "g++", "c++", "clang++"):
        if c and shutil.which(c):
            return c
    return None


def _ndk_clangxx():
    """The arm64-android clang++ from the pinned NDK (the device build's compiler), or None.
    Used to type-check the ALR_VK_DECODE_REAL-gated real-Mali bodies against the NDK Vulkan
    headers (Mali libvulkan isn't on the host, so compile-only, no link)."""
    candidates = []
    home = Path.home()
    for base in (home / "Library/Android/sdk/ndk", home / "Android/Sdk/ndk",
                 Path(os.environ["ANDROID_NDK_HOME"]) if os.environ.get("ANDROID_NDK_HOME")
                 else None):
        if base and base.exists():
            candidates += list(base.glob(
                "*/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android*-clang++"))
    candidates = sorted(candidates)
    return str(candidates[-1]) if candidates else None


def test_tool_and_registry_present():
    assert TOOL.exists(), "codegen tool missing"
    assert VK_XML.exists(), "vendored vk.xml missing"
    # Pinned to the NDK header version (a silent registry swap would drift the ABI).
    assert "VK_HEADER_VERSION_PIN = 275" in TOOL.read_text()


def test_generated_files_exist():
    # The machine-generated files carry the GENERATED banner.
    for f in (GEN_PROTO, GEN_DECODE, GEN_ICD, GEN_ICD_RT):
        assert f.exists(), f"generated file missing: {f}"
        assert "GENERATED FILE" in f.read_text(), f"{f} not marked GENERATED"
    # The companion hand-written headers (the real-Mali bodies + the same-process arena)
    # are auditable C++, deliberately NOT generated.
    for f in (GEN_REAL, GEN_ARENA):
        assert f.exists(), f"hand-written companion missing: {f}"
        assert "HAND-WRITTEN" in f.read_text() or "arena" in f.name


def test_codegen_is_deterministic_and_in_sync():
    # --check writes nothing and exits 2 if any committed generated file is stale.
    r = subprocess.run([sys.executable, str(TOOL), "--check"], capture_output=True,
                       text=True, cwd=str(ROOT))
    assert r.returncode == 0, (
        "generated files are STALE — run `python3 tools/gen_vk_passthrough.py`.\n"
        f"stdout={r.stdout}\nstderr={r.stderr}")


def test_escape_band_does_not_collide_with_handwritten():
    proto = GEN_PROTO.read_text()
    # Generated ops ride a single reserved u8 escape (230, first free slot above the
    # hand-written 0..229 band) + a u16 sub-opcode — never widening / colliding the
    # hand-written u8 opcode wire.
    assert "ALR_VK_OP_GEN_ESCAPE = 230" in proto
    assert "alr_vk_gen_op_begin" in proto
    # GEN-prefixed enum names so the sub-opcodes never collide with the hand-written
    # ALR_VK_OP_* / ALR_VK_REPLY_* names.
    assert "ALR_VK_GEN_OP_ALLOCATE_MEMORY" in proto
    assert "ALR_VK_GEN_REPLY_ALLOCATE_MEMORY" in proto


def test_decode_calls_into_generated_band_via_seam():
    # The hand-written decoder gained a generated-op seam its default case consults.
    dec = (ROOT / "app/src/main/cpp/alr_gpu/alr_gpu_vk_decode.hpp").read_text()
    assert "vk_gen_dispatch()" in dec and "set_vk_gen_dispatch" in dec
    # The generated decoder self-registers into that seam.
    gen = GEN_DECODE.read_text()
    assert "set_vk_gen_dispatch(&vk_gen_dispatch_adapter)" in gen
    assert "decode_vk_gen_op" in gen


def test_same_process_arena_is_the_map_memory_path():
    # vkMapMemory must resolve to an arena pointer (the zero-copy keystone), not a driver
    # map. The generated map case computes base_off + offset and the arena resolves it.
    gen = GEN_DECODE.read_text()
    assert "ALR_VK_GEN_OP_MAP_MEMORY" in gen
    assert "mem_arena_off" in gen
    arena = GEN_ARENA.read_text()
    assert "VK_EXT_external_memory_host" in arena or \
        "vkGetMemoryHostPointerPropertiesEXT" in arena
    assert "alr_vk_arena_import_slab" in arena  # host-pointer import = zero copy


def test_icd_wires_generated_entrypoints_but_not_command_pool():
    icd = GEN_ICD.read_text()
    # The new device-memory / buffer / image / view entrypoints are wired into the ICD.
    for name in ("vkAllocateMemory", "vkMapMemory", "vkCreateBuffer", "vkCreateImage",
                 "vkCreateImageView", "vkGetBufferMemoryRequirements", "vkBindImageMemory"):
        assert f'ALR_ENTRY("{name}"' in icd, f"{name} not wired into the ICD table"
    # Command pool is hand-written (used by run_vk_icd_present_probe) — NOT re-emitted, to
    # avoid a duplicate alr_vkCreateCommandPool C symbol.
    assert 'ALR_ENTRY("vkCreateCommandPool"' not in icd
    assert "skipped: vkCreateCommandPool" in icd


@pytest.mark.skipif(_cxx() is None, reason="no host C++ compiler")
def test_generated_wire_roundtrip_passes():
    cxx = _cxx()
    out = ROOT / "build" / "test-vk-gen-passthrough.bin"
    out.parent.mkdir(exist_ok=True)
    comp = subprocess.run(
        [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Iapp/src/main/cpp",
         str(NATIVE_TEST), "-o", str(out)],
        capture_output=True, text=True, cwd=str(ROOT))
    assert comp.returncode == 0, f"compile failed:\n{comp.stderr}"
    run = subprocess.run([str(out)], capture_output=True, text=True, cwd=str(ROOT))
    assert run.returncode == 0 and "ALL PASS" in run.stdout, \
        f"generated wire test failed:\n{run.stdout}\n{run.stderr}"
    out.unlink(missing_ok=True)


def test_pipeline_create_forwards_wired():
    # WAVE D — the pipeline create-forwards (the last major create entrypoints ANGLE needs to
    # render). The generated band ships graphics + compute pipeline creates + a pipeline destroy.
    proto = GEN_PROTO.read_text()
    for op in ("ALR_VK_GEN_OP_CREATE_GRAPHICS_PIPELINES",
               "ALR_VK_GEN_OP_CREATE_COMPUTE_PIPELINES", "ALR_VK_GEN_OP_DESTROY_PIPELINE"):
        assert op in proto, f"{op} missing from the generated proto"
    icd = GEN_ICD.read_text()
    for name in ("vkCreateGraphicsPipelines", "vkCreateComputePipelines", "vkDestroyPipeline"):
        assert f'ALR_ENTRY("{name}"' in icd, f"{name} not wired into the ICD table"
    # The heaviest CreateInfo's nested state must be marshalled (spot-check the sub-state
    # encoders the graphics ICD function drives).
    for enc in ("_stage_spec_entry", "_vertex_attr", "_viewport_elem", "_blend_attachment",
                "_dynamic_elem", "_stencil_op"):
        assert enc in proto, f"graphics pipeline sub-state encoder {enc} missing"


def test_pipeline_real_body_wires_cmd_register_pipeline():
    # The documented-missing SEAM CALLER: the hand-written real bodies, AFTER the real Mali
    # vkCreate{Graphics,Compute}Pipelines returns the pipeline handle(s), call
    # cmd_register_pipeline so vkCmdBindPipeline can translate the vid. This is the composition
    # seam with the cmd-log band (alr_gpu_vk_cmdlog_real.hpp declares cmd_register_pipeline).
    real = GEN_REAL.read_text()
    assert "vk_gen_real_create_graphics_pipelines" in real
    assert "vk_gen_real_create_compute_pipelines" in real
    assert real.count("cmd_register_pipeline(st,") >= 2, (
        "both pipeline real bodies must register each created pipeline via cmd_register_pipeline")
    # Handles are translated via VkGenTables (module / layout / renderPass), never passed raw.
    assert "shader_modules.find" in real and "pipeline_layouts.find" in real
    assert "render_passes.find" in real


def test_real_mali_pipeline_body_compiles_against_ndk_vulkan():
    """The real-Mali pipeline create bodies (vk_gen_real_create_{graphics,compute}_pipelines +
    vk_gen_real_destroy_pipeline) MUST type-check against the NDK <vulkan/vulkan.h> with the
    device build's own arm64-android clang. These bodies are ALR_VK_DECODE_REAL-gated (absent
    from the SDK-free host wire test), so this is THE syntax-verify of the deep nested-state
    reconstruction. Mali libvulkan isn't on the host -> compile-only (-c), no link."""
    clang = _ndk_clangxx()
    if clang is None:
        pytest.skip("no NDK arm64 clang++ found (set ANDROID_NDK_HOME)")
    src = ROOT / "build" / "gen_real_pipe_probe.cpp"
    src.parent.mkdir(exist_ok=True)
    src.write_text(
        "#define ALR_VK_DECODE_REAL 1\n"
        # cmdlog_real.hpp defines the cmd_register_pipeline seam the bodies call.
        '#include "alr_gpu/alr_gpu_vk_cmdlog_real.hpp"\n'
        '#include "alr_gpu/generated/alr_gpu_vk_gen_decode.hpp"\n'
        "using namespace alr::gpu;\n"
        "void force_pipe(VkDecodeState& st, uint32_t vdev, uint32_t vpcache,\n"
        "                const std::vector<VkGenPipeline>& pipes) {\n"
        "    (void)vk_gen_real_create_graphics_pipelines(st, vdev, vpcache, pipes);\n"
        "    (void)vk_gen_real_create_compute_pipelines(st, vdev, vpcache, pipes);\n"
        "    vk_gen_real_destroy_pipeline(st, vdev, 5);\n"
        "}\n")
    obj = ROOT / "build" / "gen_real_pipe_probe.o"
    comp = subprocess.run(
        [clang, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-c",
         "-Iapp/src/main/cpp", str(src), "-o", str(obj)],
        capture_output=True, text=True, cwd=str(ROOT))
    assert comp.returncode == 0, (
        "real-Mali pipeline create bodies do NOT compile against the NDK Vulkan headers:\n"
        f"{comp.stderr}")
    obj.unlink(missing_ok=True)
    src.unlink(missing_ok=True)
