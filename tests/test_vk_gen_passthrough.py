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
