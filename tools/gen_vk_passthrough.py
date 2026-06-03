#!/usr/bin/env python3
# tools/gen_vk_passthrough.py — ALR GPU Vulkan passthrough CODEGEN.
#
# WHY THIS EXISTS
# ---------------
# ANGLE resolves ~597 device-level vk* functions against our ICD (libvulkan.so.1, the
# guest ICD in app/src/main/cpp/alr_gpu/guest_icd/). Four of them were hand-written end
# to end (vkCreateDevice2 / vkGetDeviceQueue2 / the enumerate-props path / the
# clear+draw+present rungs). Hand-writing all 597 — the guest ENCODER, the host
# DECODE-AND-CALL-ON-REAL-MALI, the virtual<->real handle translation, and the ICD
# dispatch entry, four matched halves PER entrypoint — is not viable. This tool GENERATES
# those four halves per entrypoint from the Vulkan registry (vk.xml), in the EXACT shape
# the hand-written model already proved, so the marshalling backbone stays one wire ABI.
#
# THE ARCHITECTURE IT TARGETS (the "same-process thin-wire" — memory alr-gpu-native-track):
#   * The host VkRingServicer (alr_gpu_vk_host_service.hpp) owns the REAL vendor Mali
#     libvulkan and a persistent VkDecodeState (the virtual->real handle tables).
#   * The guest ICD allocates VIRTUAL handles monotonically (client-side; never blocks on
#     a host round-trip to *create* a handle) and ships an op stream over the SPSC ring.
#   * HANDLES cross the ring (guest virtual id <-> host real handle, resolved by
#     VkDecodeState); BYTES of HOST_VISIBLE memory NEVER cross — vkMapMemory returns a
#     pointer into a pre-shared MAP_SHARED arena (alr_gpu_vk_arena.hpp), so a guest
#     write lands directly in memory the host's real VkDeviceMemory is bound to.
#   * Results (VkResult, VkMemoryRequirements, the mapped offset) come BACK in the reply.
#
# WHAT IT EMITS (into app/src/main/cpp/alr_gpu/generated/, all clearly marked GENERATED):
#   1. alr_gpu_vk_gen_proto.hpp   — the wire op enums (new 300+ band, the hand-written
#                                   200..229 band is left untouched) + the guest C
#                                   encoders (AlrVkEncoder builders), per entrypoint.
#   2. alr_gpu_vk_gen_decode.hpp  — decode_vk_gen_op(): the host half that reads each op
#                                   off the wire, translates virtual->real handles via
#                                   VkDecodeState, calls the REAL Mali entrypoint, and
#                                   appends the reply. #ifdef ALR_VK_DECODE_REAL for the
#                                   real call; a header-only wire codec otherwise.
#   3. alr_gpu_vk_gen_icd.inc     — the ICD dispatch entries (one C function per
#                                   entrypoint + an ALR_ENTRY table fragment) the guest
#                                   ICD #includes; each marshals its request + (for the
#                                   round-trip ops) decodes the reply.
#   4. alr_gpu_vk_gen_icd_runtime.inc — the ICD-side reply scanners (generated so they
#                                   never drift from the reply wire field order).
#
# The same-process MAP_SHARED arena (alr_gpu_vk_arena.hpp) is hand-written (it is shared
# infrastructure, not per-entrypoint) and lives next to the generated files; this tool
# generates the vkAllocateMemory / vkMapMemory entrypoints that USE it. The real-Mali
# bodies (vk_gen_real_*) are hand-written in alr_gpu_vk_gen_real.hpp (auditable Vulkan
# C++), called by the generated decode dispatch.
#
# HOW IT PARSES vk.xml
# --------------------
# Pure stdlib xml.etree. For each requested entrypoint it reads the <command> proto +
# <param> list, classifies each parameter by the registry type category (a non-dispatchable
# handle, a dispatchable handle, a scalar/enum/flags, a const-pointer-to-CreateInfo, an
# out-pointer-to-handle, an out-pointer-to-a-requirements struct, a length-linked array),
# and from that classification picks the matching wire encoding + host reconstruction. The
# per-entrypoint POLICY (which params are the device/handle, which CreateInfo to forward,
# whether it round-trips) is declared in SPECS below — the registry supplies the TYPES,
# the spec supplies the INTENT (the registry can't know we forward the CreateInfo's POD
# prefix + allowlisted pNext rather than deep-copying every nested array).
#
# Run:  python3 tools/gen_vk_passthrough.py            # regenerate in place
#       python3 tools/gen_vk_passthrough.py --check     # fail if out of date (CI gate)
#       python3 tools/gen_vk_passthrough.py --xml PATH  # use a different registry
#
# Reproducible: vk.xml is vendored at tools/vk_registry/vk.xml, pinned to Vulkan-Headers
# v1.3.275 (== the NDK 27.2 sysroot VK_HEADER_VERSION 275 the device build links).

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_XML = ROOT / "tools" / "vk_registry" / "vk.xml"
GEN_DIR = ROOT / "app" / "src" / "main" / "cpp" / "alr_gpu" / "generated"

# Pinned registry provenance (asserted against vk.xml so a silent registry swap is caught).
VK_HEADER_VERSION_PIN = 275

# ---------------------------------------------------------------------------
# Wire opcode band. The hand-written ops occupy 0..229 as a single BYTE opcode
# (alr_gpu_vk_proto.hpp uses alr_vk_enc_u8 for the opcode, so they must be < 256). The
# generated entrypoints would overflow a u8 (and there are eventually ~597 of them), so
# they ride an ESCAPE: the hand-written decoder reads the u8 opcode, and on the escape
# byte ALR_VK_OP_GEN_ESCAPE (a single reserved value in the u8 space) it hands off to the
# generated dispatcher, which reads a u16 generated sub-opcode + the operands. This keeps
# the hand-written band a pure u8 wire, costs ONE byte per generated op, and scales to
# 65535 generated entrypoints. The sub-opcodes are assigned in SPEC order and are STABLE
# (append-only): never renumber an existing one — it is the wire.
#
# ALR_VK_OP_GEN_ESCAPE is emitted into the generated proto header as 230 (the first free
# slot above the hand-written 0..229 band). decode_vk_batch's default case calls the
# generated dispatcher for ANY unknown u8, and the dispatcher only claims the escape byte.
GEN_ESCAPE_OP = 230
GEN_SUBOP_BASE = 1     # u16 generated sub-opcodes (1.. ; 0 reserved)
GEN_REPLY_BASE = 1     # u16 generated reply sub-opcodes (1.. ; 0 reserved)

# Entrypoints to GENERATE for the host (encoder + host decode + the wire test) but OMIT
# from the guest ICD .inc, because the ICD already hand-writes them (a duplicate
# alr_vk<Name> C symbol would collide at link). Command pool is hand-written on the 200..
# band (used by run_vk_icd_present_probe), so its generated ICD function/table-row are
# suppressed; its generated encoder + host decode still exist (the host servicer can
# replay a 300.. command-pool op, and the host wire test covers it).
ICD_SKIP = {"vkCreateCommandPool", "vkDestroyCommandPool"}

# ---------------------------------------------------------------------------
# PER-ENTRYPOINT SPECS. Each entry declares the marshalling INTENT the registry can't
# express. Field meanings:
#   kind:
#     "create_handle"  — vkCreate<X>(device, pCreateInfo, alloc, pHandle): forward the
#                        CreateInfo (POD prefix fields in `ci_fields` + allowlisted pNext
#                        blobs) to real Mali; host creates the real handle, stores it under
#                        the guest's virtual id; reply carries the VkResult. The guest
#                        picks the virtual id (client-side, monotonic).
#     "create_pool"    — same as create_handle but the pool create needs the device's gfx
#                        family substituted (the existing coarse path does this).
#     "alloc_memory"   — vkAllocateMemory: the SAME-PROCESS arena path. Host carves a slab
#                        from the MAP_SHARED arena, makes a real HOST_VISIBLE VkDeviceMemory
#                        whose contents alias the slab (import or staged), records the
#                        slab offset; reply carries VkResult + the arena offset so the
#                        guest's vkMapMemory is a local pointer add.
#     "map_memory"     — vkMapMemory: NO real driver map; resolve the arena offset for the
#                        memory's virtual id and hand back a guest pointer into the mapped
#                        arena. Round-trips once to fetch the offset (then cached).
#     "unmap_memory"   — vkUnmapMemory: arena path is a no-op flush marker (host coherent).
#     "flush_ranges"   — vkFlushMappedMemoryRanges: arena slabs are HOST_COHERENT, so this
#                        is a marker op (forwarded for ordering, no driver flush needed).
#     "get_reqs"       — vkGet<X>MemoryRequirements(device, handle, pReqs): round-trips;
#                        host calls real Mali, reply carries the VkMemoryRequirements.
#     "bind_memory"    — vkBind<X>Memory(device, handle, memory, offset): forward; host
#                        binds the real handle to the real memory at offset; reply = result.
#     "destroy_handle" — vkDestroy<X>(device, handle, alloc): forward; host destroys the
#                        real handle + drops the virtual mapping; no reply.
#   ci: the CreateInfo struct type (for create_* kinds) — its POD prefix is forwarded.
#   ci_fields: the scalar CreateInfo fields shipped explicitly on the wire (name, wire
#              type). The host rebuilds the CreateInfo from these + the pNext blobs. Arrays
#              the first batch doesn't need (e.g. pQueueFamilyIndices for EXCLUSIVE buffers)
#              are NOT shipped — documented honestly as a deferred breadth item. A field
#              "@member:VkType" is a virtual-handle reference whose value is the guest's
#              virtual id for that handle (shipped u32, translated host-side).
#   handle_param: the non-dispatchable handle the op acts on (for get_reqs/bind/destroy).
# ---------------------------------------------------------------------------
SPECS = [
    # ---- command pool ----
    # NOTE: vkCreateCommandPool / vkDestroyCommandPool are ALREADY hand-written in the ICD
    # (alr_icd_vulkan.c) on the 200.. wire band (ALR_VK_OP_CREATE_COMMAND_POOL=212), used by
    # run_vk_icd_present_probe. We still GENERATE their encoder + host decode here (so the
    # create_pool kind + the host wire test are complete + the host servicer can replay the
    # 300.. command-pool op), but they are listed in ICD_SKIP below so the generated ICD
    # .inc emits NO C function / table row for them — that would collide with the existing
    # hand-written alr_vkCreateCommandPool. The guest ICD keeps using its hand-written ones.
    {
        "name": "vkCreateCommandPool", "kind": "create_pool",
        "ci": "VkCommandPoolCreateInfo",
        "ci_fields": [("flags", "u32"), ("queueFamilyIndex", "u32")],
        "out": "VkCommandPool",
    },
    {
        "name": "vkDestroyCommandPool", "kind": "destroy_handle",
        "handle_param": ("commandPool", "VkCommandPool"),
    },
    # ---- device memory (the same-process arena) ----
    {
        "name": "vkAllocateMemory", "kind": "alloc_memory",
        "ci": "VkMemoryAllocateInfo",
        "ci_fields": [("allocationSize", "u64"), ("memoryTypeIndex", "u32")],
        "out": "VkDeviceMemory",
    },
    {
        "name": "vkMapMemory", "kind": "map_memory",
        "handle_param": ("memory", "VkDeviceMemory"),
    },
    {
        "name": "vkUnmapMemory", "kind": "unmap_memory",
        "handle_param": ("memory", "VkDeviceMemory"),
    },
    {
        "name": "vkFlushMappedMemoryRanges", "kind": "flush_ranges",
    },
    {
        "name": "vkFreeMemory", "kind": "destroy_handle",
        "handle_param": ("memory", "VkDeviceMemory"),
    },
    # ---- buffer ----
    {
        "name": "vkCreateBuffer", "kind": "create_handle",
        "ci": "VkBufferCreateInfo",
        # NOTE: pQueueFamilyIndices (CONCURRENT sharing) is NOT shipped — the first batch
        # uses EXCLUSIVE buffers (ANGLE's default). A CONCURRENT buffer would need the
        # index array on the wire; deferred (honest scope).
        "ci_fields": [("flags", "u32"), ("size", "u64"), ("usage", "u32"),
                      ("sharingMode", "u32")],
        "out": "VkBuffer",
    },
    {
        "name": "vkDestroyBuffer", "kind": "destroy_handle",
        "handle_param": ("buffer", "VkBuffer"),
    },
    {
        "name": "vkGetBufferMemoryRequirements", "kind": "get_reqs",
        "handle_param": ("buffer", "VkBuffer"), "reqs": "VkMemoryRequirements",
    },
    {
        "name": "vkBindBufferMemory", "kind": "bind_memory",
        "handle_param": ("buffer", "VkBuffer"),
    },
    # ---- image ----
    {
        "name": "vkCreateImage", "kind": "create_handle",
        "ci": "VkImageCreateInfo",
        # Image extent/format/usage/etc. POD prefix. pQueueFamilyIndices (CONCURRENT) again
        # not shipped (EXCLUSIVE images only — deferred breadth).
        "ci_fields": [
            ("flags", "u32"), ("imageType", "u32"), ("format", "u32"),
            ("extent.width", "u32"), ("extent.height", "u32"), ("extent.depth", "u32"),
            ("mipLevels", "u32"), ("arrayLayers", "u32"), ("samples", "u32"),
            ("tiling", "u32"), ("usage", "u32"), ("sharingMode", "u32"),
            ("initialLayout", "u32"),
        ],
        "out": "VkImage",
    },
    {
        "name": "vkDestroyImage", "kind": "destroy_handle",
        "handle_param": ("image", "VkImage"),
    },
    {
        "name": "vkGetImageMemoryRequirements", "kind": "get_reqs",
        "handle_param": ("image", "VkImage"), "reqs": "VkMemoryRequirements",
    },
    {
        "name": "vkBindImageMemory", "kind": "bind_memory",
        "handle_param": ("image", "VkImage"),
    },
    # ---- image view ----
    {
        "name": "vkCreateImageView", "kind": "create_handle",
        "ci": "VkImageViewCreateInfo",
        # The view references its source image (a handle, not a scalar) — shipped as a
        # virtual handle field, translated host-side. components/subresourceRange are POD.
        "ci_fields": [
            ("flags", "u32"), ("@image:VkImage", "vhandle"), ("viewType", "u32"),
            ("format", "u32"),
            ("components.r", "u32"), ("components.g", "u32"),
            ("components.b", "u32"), ("components.a", "u32"),
            ("subresourceRange.aspectMask", "u32"),
            ("subresourceRange.baseMipLevel", "u32"),
            ("subresourceRange.levelCount", "u32"),
            ("subresourceRange.baseArrayLayer", "u32"),
            ("subresourceRange.layerCount", "u32"),
        ],
        "out": "VkImageView",
    },
    {
        "name": "vkDestroyImageView", "kind": "destroy_handle",
        "handle_param": ("imageView", "VkImageView"),
    },
]

# ---------------------------------------------------------------------------
# Registry parse helpers.
# ---------------------------------------------------------------------------


class Registry:
    def __init__(self, xml_path: Path):
        self.tree = ET.parse(str(xml_path))
        self.root = self.tree.getroot()
        self.commands = {}      # name -> {"ret":..., "params":[...]}
        self.handles = {}       # name -> "dispatch"|"nondispatch"
        self._index()

    def _index(self):
        for ty in self.root.iter("type"):
            if ty.get("category") == "handle":
                nm = ty.findtext("name")
                kind = ty.findtext("type") or ""
                if nm:
                    self.handles[nm] = (
                        "nondispatch" if "NON_DISPATCHABLE" in kind else "dispatch")
        for c in self.root.iter("command"):
            proto = c.find("proto")
            if proto is None:
                # <command alias=...> — skip; we only generate from concrete defs.
                continue
            name = proto.findtext("name")
            ret = proto.findtext("type")
            params = []
            for prm in c.findall("param"):
                ptype = prm.findtext("type")
                pname = prm.findtext("name")
                full = "".join(prm.itertext())
                params.append({
                    "type": ptype, "name": pname, "ptr": "*" in full,
                    "const": full.strip().startswith("const"), "full": full.strip(),
                })
            self.commands[name] = {"ret": ret, "params": params}

    def header_version(self):
        for ty in self.root.iter("type"):
            if ty.get("category") == "define" and ty.findtext("name") == "VK_HEADER_VERSION":
                txt = "".join(ty.itertext()).replace("\n", " ")
                for tok in txt.split():
                    if tok.isdigit():
                        return int(tok)
        return None


# ---------------------------------------------------------------------------
# Emit helpers.
# ---------------------------------------------------------------------------

GEN_BANNER = """// ==========================================================================
//  GENERATED FILE — DO NOT EDIT BY HAND.
//  Produced by tools/gen_vk_passthrough.py from tools/vk_registry/vk.xml
//  (Vulkan-Headers v1.3.275, VK_HEADER_VERSION {ver}). Regenerate with:
//      python3 tools/gen_vk_passthrough.py
//  Edit the SPECS in that tool, not this file. The wire opcodes here are
//  APPEND-ONLY (they are the on-the-wire ABI shared with the host decoder).
// ==========================================================================
"""


def wire_enc_call(wt, var):
    return {
        "u32": f"alr_vk_enc_u32(e, {var});",
        "u64": f"alr_vk_enc_u64(e, {var});",
        "vhandle": f"alr_vk_enc_u32(e, {var});",
    }[wt]


def reader_call(wt, var):
    return {"u32": f"r.u32({var})", "u64": f"r.u64({var})", "vhandle": f"r.u32({var})"}[wt]


def ctype_for(wt):
    return {"u32": "uint32_t", "u64": "uint64_t", "vhandle": "uint32_t"}[wt]


def wire_var(fname):
    """A legal C identifier for a (possibly dotted/@-prefixed) CreateInfo field."""
    base = fname[1:].split(":", 1)[0] if fname.startswith("@") else fname
    return base.replace(".", "_")


PNEXT_HELPERS = """
// ---- Shared allowlisted-pNext encoders (the SAME shape as CREATE_DEVICE2's feature
// chain in alr_gpu_vk_proto.hpp): a count, then count x { u32 sType, blob struct_bytes }.
// The host relinks the chain from KNOWN sTypes only (skip-unknown), so a malformed pNext
// can never make the real driver walk a bogus chain. ----
static inline void alr_vk_gen_pnext_count(AlrVkEncoder *e, uint32_t count) {
    alr_vk_enc_u32(e, count);
}
static inline void alr_vk_gen_pnext(AlrVkEncoder *e, uint32_t s_type,
                                    const void *bytes, uint32_t len) {
    alr_vk_enc_u32(e, s_type);
    alr_vk_enc_blob(e, bytes, len);
}
"""

U16_HELPERS = """
// A u16 little-endian field (the generated sub-opcode width). The hand-written
// AlrVkEncoder ships u8/u32/u64/i32/f32; the generated band needs a u16 for its
// sub-opcode, added here so the hand-written wire header stays untouched.
static inline void alr_vk_enc_u16(AlrVkEncoder *e, uint16_t v) { alr_vk_enc_raw(e, &v, 2); }

// Every generated request op is: u8 ALR_VK_OP_GEN_ESCAPE, then u16 sub-opcode, then the
// operands. The escape lets the hand-written u8-opcode decoder hand off to the generated
// dispatcher without colliding with (or widening) the 0..229 hand-written band.
static inline void alr_vk_gen_op_begin(AlrVkEncoder *e, uint16_t sub_op) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_GEN_ESCAPE);
    alr_vk_enc_u16(e, sub_op);
}
"""


def gen_proto(reg, ops):
    L = []
    a = L.append
    ver = reg.header_version()
    a(GEN_BANNER.format(ver=ver))
    a("#ifndef ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP")
    a("#define ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP")
    a("")
    a('#include "alr_gpu/alr_gpu_vk_proto.hpp"  // AlrVkEncoder + the hand-written 0..229 band')
    a("")
    a("#ifdef __cplusplus")
    a('extern "C" {')
    a("#endif")
    a("")
    a("// The single reserved u8 opcode that escapes into the generated band (first free")
    a("// slot above the hand-written 0..229 ops). Followed on the wire by a u16 sub-opcode.")
    a(f"enum {{ ALR_VK_OP_GEN_ESCAPE = {GEN_ESCAPE_OP} }};")
    a("// The matching reply escape (host -> guest): a u8 escape + u16 reply sub-opcode, so")
    a("// the generated reply records never collide with the hand-written reply band.")
    a(f"enum {{ ALR_VK_REPLY_GEN_ESCAPE = {GEN_ESCAPE_OP} }};")
    a("")
    a("// ---- Generated request SUB-opcodes (u16; ride the escape above). APPEND-ONLY:")
    a("// an existing number is the wire and must never move. ----")
    a("enum AlrVkGenOp {")
    for op in ops:
        a(f"    {op['op_enum']} = {op['op_num']},  // {op['name']}")
    a("};")
    a("")
    a("// ---- Generated reply SUB-opcodes (u16; ride the reply escape). ----")
    a("enum AlrVkGenReply {")
    for op in ops:
        if op["reply_enum"]:
            a(f"    {op['reply_enum']} = {op['reply_num']},  // reply of {op['name']}")
    a("};")
    a("")
    a(U16_HELPERS.strip("\n"))
    a("")
    a(PNEXT_HELPERS.strip("\n"))
    a("")
    a("// ---- Per-entrypoint guest encoders. Each appends ONE op (escape + sub-opcode +")
    a("// operands) in the exact field order the host decoder (alr_gpu_vk_gen_decode.hpp)")
    a("// reads back. ----")
    for op in ops:
        a(gen_proto_encoder(op))
        a("")
    a("#ifdef __cplusplus")
    a('}  // extern "C"')
    a("#endif")
    a("")
    a("#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP")
    return "\n".join(L) + "\n"


def gen_proto_encoder(op):
    name = op["name"]
    enc = op["enc_name"]
    L = []
    a = L.append
    k = op["kind"]
    if k in ("create_handle", "create_pool"):
        params = ["AlrVkEncoder *e", "uint32_t vdev", f"uint32_t {op['vout']}"]
        for fname, wt in op["ci_wire_fields"]:
            params.append(f"{ctype_for(wt)} {wire_var(fname)}")
        a(f"// Encoder for {name}. Ships the device + the guest's virtual {op['out']} id +")
        a(f"// the {op['ci']} POD prefix. Append the allowlisted pNext chain after via")
        a("// alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's feature-chain shape).")
        a(f"static inline void {enc}_begin({', '.join(params)}) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a(f"    alr_vk_enc_u32(e, {op['vout']});")
        for fname, wt in op["ci_wire_fields"]:
            a(f"    {wire_enc_call(wt, wire_var(fname))}")
        a("}")
    elif k == "alloc_memory":
        a(f"// Encoder for {name} (same-process arena). Ships the device + virtual memory id")
        a("// + allocationSize + memoryTypeIndex; append the pNext chain after.")
        a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vmem,")
        a("                          uint64_t allocation_size, uint32_t memory_type_index) {")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, vmem);")
        a("    alr_vk_enc_u64(e, allocation_size);")
        a("    alr_vk_enc_u32(e, memory_type_index);")
        a("}")
    elif k == "get_reqs":
        a(f"// Encoder for {name} (round-trips: the host returns VkMemoryRequirements).")
        a(f"static inline void {enc}(AlrVkEncoder *e, uint32_t vdev, uint32_t {op['vhandle']}) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a(f"    alr_vk_enc_u32(e, {op['vhandle']});")
        a("}")
    elif k == "bind_memory":
        a(f"// Encoder for {name} (binds the real handle to real memory at offset).")
        a(f"static inline void {enc}(AlrVkEncoder *e, uint32_t vdev, uint32_t {op['vhandle']},")
        a("                          uint32_t vmemory, uint64_t memory_offset) {")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a(f"    alr_vk_enc_u32(e, {op['vhandle']});")
        a("    alr_vk_enc_u32(e, vmemory);")
        a("    alr_vk_enc_u64(e, memory_offset);")
        a("}")
    elif k == "map_memory":
        a(f"// Encoder for {name} (arena path: round-trips once to fetch the arena offset).")
        a(f"static inline void {enc}(AlrVkEncoder *e, uint32_t vdev, uint32_t vmemory,")
        a("                          uint64_t offset, uint64_t size) {")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, vmemory);")
        a("    alr_vk_enc_u64(e, offset);")
        a("    alr_vk_enc_u64(e, size);")
        a("}")
    elif k == "unmap_memory":
        a(f"// Encoder for {name} (arena path: a coherent-flush marker, no driver unmap).")
        a(f"static inline void {enc}(AlrVkEncoder *e, uint32_t vdev, uint32_t vmemory) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, vmemory);")
        a("}")
    elif k == "flush_ranges":
        a(f"// Encoder for {name} (arena slabs are HOST_COHERENT; a marker for ordering).")
        a("// Ships each range's (vmemory, offset, size) so a future non-coherent arena")
        a("// could honor it; the count is bounded by the host decoder.")
        a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t range_count) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, range_count);")
        a("}")
        a(f"static inline void {enc}_range(AlrVkEncoder *e, uint32_t vmemory,")
        a("                          uint64_t offset, uint64_t size) {")
        a("    alr_vk_enc_u32(e, vmemory);")
        a("    alr_vk_enc_u64(e, offset);")
        a("    alr_vk_enc_u64(e, size);")
        a("}")
    elif k == "destroy_handle":
        a(f"// Encoder for {name} (forwards a destroy; no reply).")
        a(f"static inline void {enc}(AlrVkEncoder *e, uint32_t vdev, uint32_t {op['vhandle']}) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a(f"    alr_vk_enc_u32(e, {op['vhandle']});")
        a("}")
    return "\n".join(L)


def gen_decode(reg, ops):
    L = []
    a = L.append
    ver = reg.header_version()
    a(GEN_BANNER.format(ver=ver))
    a("#ifndef ALR_GPU_GENERATED_ALR_GPU_VK_GEN_DECODE_HPP")
    a("#define ALR_GPU_GENERATED_ALR_GPU_VK_GEN_DECODE_HPP")
    a("")
    a("// The HOST half of the generated entrypoints. decode_vk_gen_op() is called by")
    a("// decode_vk_batch() (alr_gpu_vk_decode.hpp) for any opcode in the 300.. generated")
    a("// band — it reads the op off the SAME VkReader, translates the guest's virtual")
    a("// handles to real Mali handles via VkDecodeState, calls the REAL entrypoint")
    a("// (#ifdef ALR_VK_DECODE_REAL), and appends the reply. Without ALR_VK_DECODE_REAL it")
    a("// is a header-only wire codec (handles tracked, no driver) so the host wire test")
    a("// proves the round trip with no Vulkan SDK.")
    a("")
    a('#include "alr_gpu/alr_gpu_vk_decode.hpp"        // VkDecodeState, VkReader, VkReplyEncoder')
    a('#include "alr_gpu/generated/alr_gpu_vk_gen_proto.hpp"  // the op enums')
    a("")
    a("// --- Block 1: the generated handle tables (VkGenTables + gen_tables). These must be")
    a("// a COMPLETE type before the real-Mali bodies below use them, so they are emitted in")
    a("// their own namespace block, the real bodies are #included at FILE scope after it")
    a("// (the bodies' own #includes must not sit inside a namespace), then block 2 resumes.")
    a("namespace alr::gpu {")
    a("")
    a(gen_decode_state_ext())
    a("")
    a("}  // namespace alr::gpu (block 1)")
    a("")
    a("// The hand-written real-Mali bodies (vk_gen_real_*) the decode dispatch calls under")
    a("// ALR_VK_DECODE_REAL. Included AFTER VkGenTables/gen_tables (complete type) and")
    a("// BEFORE decode_vk_gen_op (which calls the bodies). At file scope so gen_real.hpp's")
    a("// own #includes (<vulkan/vulkan.h>, <map>) are not nested in a namespace.")
    a("#ifdef ALR_VK_DECODE_REAL")
    a('#include "alr_gpu/generated/alr_gpu_vk_gen_real.hpp"')
    a("#endif")
    a("")
    a("namespace alr::gpu {  // block 2")
    a("")
    a("// Provider seam for the generated ops (the wire test injects a synthetic Mali here;")
    a("// null fields fall through to the real-Mali path under ALR_VK_DECODE_REAL).")
    a(gen_provider_struct())
    a("")
    a("// decode_vk_gen_op — dispatch ONE generated op. `op` is the u8 already read by")
    a("// decode_vk_batch; we only claim ALR_VK_OP_GEN_ESCAPE. On the escape we read the u16")
    a("// sub-opcode + the operands off the SAME reader, translate virtual->real handles via")
    a("// `st`, call the real entrypoint (or the synthetic provider), and append the reply.")
    a("// Returns true if `op` was the generated escape (handled — even on a malformed body,")
    a("// which sets st.ok=false), false if `op` is some other (truly unknown) opcode.")
    a("inline bool decode_vk_gen_op(uint8_t op, VkReader& r, VkDecodeState& st,")
    a("                             VkReplyEncoder& reply, const VkGenProvider* gp) {")
    a("    if (op != ALR_VK_OP_GEN_ESCAPE) return false;  // not ours")
    a("    uint16_t sub = 0;")
    a("    if (!r.u16(sub)) { st.ok = false; return true; }")
    a("    switch (sub) {")
    for op in ops:
        a(gen_decode_case(op))
    a("        default:")
    a("            st.ok = false;  // escape with an unknown sub-opcode: fail-stop")
    a("            return true;")
    a("    }")
    a("    return true;")
    a("}")
    a("")
    a("// ---- Self-registration into decode_vk_batch's generated-op seam. The opaque")
    a("// `gen_provider` from decode_vk_batch is the VkGenProvider* the caller set (null on")
    a("// device -> real-Mali path). A function-local static registrar runs at first use of")
    a("// this TU so simply #including this header wires the 300.. band into decode_vk_batch.")
    a("inline bool vk_gen_dispatch_adapter(uint8_t op, VkReader& r, VkDecodeState& st,")
    a("                                    VkReplyEncoder& reply, const void* gen_provider) {")
    a("    return decode_vk_gen_op(op, r, st, reply,")
    a("                            static_cast<const VkGenProvider*>(gen_provider));")
    a("}")
    a("inline bool vk_gen_register_dispatch() {")
    a("    set_vk_gen_dispatch(&vk_gen_dispatch_adapter);")
    a("    return true;")
    a("}")
    a("inline const bool kVkGenDispatchRegistered = vk_gen_register_dispatch();")
    a("")
    a("}  // namespace alr::gpu")
    a("")
    a("#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_DECODE_HPP")
    return "\n".join(L) + "\n"


def gen_decode_state_ext():
    L = []
    a = L.append
    a("// Generated handle tables — virtual id -> real Mali handle, persistent across")
    a("// batches (like VkDecodeState's own maps). Held in a side struct keyed by the")
    a("// owning VkDecodeState's address so the generated decode finds them without")
    a("// editing the hand-written struct. One instance per VkDecodeState, via gen_tables(st).")
    a("struct VkGenTables {")
    a("#ifdef ALR_VK_DECODE_REAL")
    a("    std::map<uint32_t, VkCommandPool> pools;    // vpool  -> real")
    a("    std::map<uint32_t, VkBuffer> buffers;       // vbuf   -> real")
    a("    std::map<uint32_t, VkImage> images;         // vimg   -> real")
    a("    std::map<uint32_t, VkImageView> views;      // vview  -> real")
    a("    std::map<uint32_t, VkDeviceMemory> memory;  // vmem   -> real")
    a("#endif")
    a("    // Arena offset assigned to each device-memory virtual id (HOST_VISIBLE only).")
    a("    // UINT64_MAX == not arena-backed (e.g. a DEVICE_LOCAL alloc). Tracked even in")
    a("    // wire-test mode so the map_memory round trip can be asserted with no SDK.")
    a("    std::map<uint32_t, uint64_t> mem_arena_off;")
    a("    std::map<uint32_t, uint64_t> mem_size;")
    a("};")
    a("inline VkGenTables& gen_tables(VkDecodeState& st) {")
    a("    static std::map<const VkDecodeState*, VkGenTables> g;")
    a("    return g[&st];")
    a("}")
    return "\n".join(L)


def gen_provider_struct():
    L = []
    a = L.append
    a("struct VkGenProvider {")
    a("    // create_handle / create_pool: return VkResult-equiv (0 == ok). a,b = the first")
    a("    // two scalar CreateInfo fields (for wire-test assertions).")
    a("    int (*create_handle)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle,")
    a("                         uint64_t a, uint64_t b) = nullptr;")
    a("    // alloc_memory: *arena_off_out gets the arena offset (UINT64_MAX if none).")
    a("    int (*alloc_memory)(void* ctx, uint32_t vdev, uint32_t vmem, uint64_t size,")
    a("                        uint32_t mem_type, uint64_t* arena_off_out) = nullptr;")
    a("    bool (*map_memory)(void* ctx, uint32_t vdev, uint32_t vmem, uint64_t* off_out) = nullptr;")
    a("    int (*get_reqs)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle,")
    a("                    uint64_t* size_out, uint64_t* align_out, uint32_t* bits_out) = nullptr;")
    a("    int (*bind_memory)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle,")
    a("                       uint32_t vmem, uint64_t off) = nullptr;")
    a("    void (*destroy_handle)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle) = nullptr;")
    a("    void* ctx = nullptr;")
    a("};")
    return "\n".join(L)


def gen_pnext_read():
    L = []
    a = L.append
    a("            uint32_t pnext_count = 0;")
    a("            std::vector<uint32_t> pnext_types;")
    a("            std::vector<std::vector<uint8_t>> pnext_bytes;")
    a("            if (!r.u32(pnext_count)) { st.ok = false; return true; }")
    a("            if (pnext_count > 32) { st.ok = false; return true; }")
    a("            for (uint32_t i = 0; i < pnext_count; ++i) {")
    a("                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;")
    a("                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }")
    a("                if (n > 1024) { st.ok = false; return true; }")
    a("                pnext_types.push_back(stype);")
    a("                pnext_bytes.emplace_back(d, d + n);")
    a("            }")
    a("            (void)pnext_count;")
    return "\n".join(L)


def gen_decode_case(op):
    k = op["kind"]
    L = []
    a = L.append
    a(f"        case {op['op_enum']}: {{  // {op['name']}")
    if k in ("create_handle", "create_pool"):
        a("            uint32_t vdev = 0, vhandle = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }")
        decls, reads = [], []
        for fname, wt in op["ci_wire_fields"]:
            v = wire_var(fname)
            decls.append(f"{ctype_for(wt)} {v} = 0;")
            reads.append(reader_call(wt, v))
        if decls:
            a("            " + " ".join(decls))
            a("            if (!(" + " && ".join(reads) + ")) { st.ok = false; return true; }")
        a(gen_pnext_read())
        a("            int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a(f"                res = static_cast<int>(vk_gen_real_{op['short']}(")
        a(f"                    st, vdev, vhandle{(', ' + op['ci_call_args']) if op['ci_call_args'] else ''},")
        a("                    pnext_types, pnext_bytes));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->create_handle)")
        a(f"                res = gp->create_handle(gp->ctx, {op['op_enum']}, vdev, vhandle,")
        a(f"                                        {op['ci_first_two']});")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vhandle);")
        a("            reply.i32(res);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "alloc_memory":
        a("            uint32_t vdev = 0, vmem = 0, memory_type_index = 0;")
        a("            uint64_t allocation_size = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vmem) || !r.u64(allocation_size) ||")
        a("                !r.u32(memory_type_index)) { st.ok = false; return true; }")
        a(gen_pnext_read())
        a("            int res = -1;")
        a("            uint64_t arena_off = UINT64_MAX;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a("                res = static_cast<int>(vk_gen_real_alloc_memory(")
        a("                    st, vdev, vmem, allocation_size, memory_type_index, arena_off));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->alloc_memory)")
        a("                res = gp->alloc_memory(gp->ctx, vdev, vmem, allocation_size,")
        a("                                       memory_type_index, &arena_off);")
        a("            gen_tables(st).mem_arena_off[vmem] = arena_off;")
        a("            gen_tables(st).mem_size[vmem] = allocation_size;")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vmem);")
        a("            reply.i32(res);")
        a("            reply.u64(arena_off);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "map_memory":
        a("            uint32_t vdev = 0, vmem = 0;")
        a("            uint64_t offset = 0, size = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vmem) || !r.u64(offset) || !r.u64(size)) {")
        a("                st.ok = false; return true; }")
        a("            (void)vdev; (void)size;")
        a("            uint64_t base_off = UINT64_MAX;")
        a("            auto it = gen_tables(st).mem_arena_off.find(vmem);")
        a("            if (it != gen_tables(st).mem_arena_off.end()) base_off = it->second;")
        a("            if (gp && gp->map_memory) gp->map_memory(gp->ctx, vdev, vmem, &base_off);")
        a("            const uint64_t mapped_off =")
        a("                (base_off == UINT64_MAX) ? UINT64_MAX : base_off + offset;")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vmem);")
        a("            reply.u64(mapped_off);")
        a("            reply.i32(mapped_off == UINT64_MAX ? -1 : 0);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "unmap_memory":
        a("            uint32_t vdev = 0, vmem = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vmem)) { st.ok = false; return true; }")
        a("            (void)vdev; (void)vmem;  // arena is HOST_COHERENT: unmap is a no-op")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "flush_ranges":
        a("            uint32_t vdev = 0, range_count = 0;")
        a("            if (!r.u32(vdev) || !r.u32(range_count)) { st.ok = false; return true; }")
        a("            (void)vdev;")
        a("            if (range_count > 4096) { st.ok = false; return true; }")
        a("            for (uint32_t i = 0; i < range_count; ++i) {")
        a("                uint32_t vmem = 0; uint64_t off = 0, sz = 0;")
        a("                if (!r.u32(vmem) || !r.u64(off) || !r.u64(sz)) { st.ok = false; return true; }")
        a("                (void)vmem; (void)off; (void)sz;  // coherent: nothing to flush")
        a("            }")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "get_reqs":
        a("            uint32_t vdev = 0, vhandle = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }")
        a("            uint64_t size = 0, align = 0; uint32_t bits = 0; int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a(f"                res = static_cast<int>(vk_gen_real_{op['short']}(")
        a("                    st, vdev, vhandle, size, align, bits));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->get_reqs)")
        a(f"                res = gp->get_reqs(gp->ctx, {op['op_enum']}, vdev, vhandle,")
        a("                                   &size, &align, &bits);")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vhandle);")
        a("            reply.i32(res);")
        a("            reply.u64(size);")
        a("            reply.u64(align);")
        a("            reply.u32(bits);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "bind_memory":
        a("            uint32_t vdev = 0, vhandle = 0, vmem = 0; uint64_t off = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vhandle) || !r.u32(vmem) || !r.u64(off)) {")
        a("                st.ok = false; return true; }")
        a("            int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a(f"                res = static_cast<int>(vk_gen_real_{op['short']}(")
        a("                    st, vdev, vhandle, vmem, off));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->bind_memory)")
        a(f"                res = gp->bind_memory(gp->ctx, {op['op_enum']}, vdev, vhandle, vmem, off);")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vhandle);")
        a("            reply.i32(res);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "destroy_handle":
        a("            uint32_t vdev = 0, vhandle = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }")
        a("            (void)vdev;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a(f"                vk_gen_real_{op['short']}(st, vdev, vhandle);")
        a("            }")
        a("#endif")
        a("            if (gp && gp->destroy_handle)")
        a(f"                gp->destroy_handle(gp->ctx, {op['op_enum']}, vdev, vhandle);")
        a("            st.decoded++;")
        a("            return true;")
    a("        }")
    return "\n".join(L)


def gen_icd(reg, ops):
    L = []
    a = L.append
    ver = reg.header_version()
    a(GEN_BANNER.format(ver=ver))
    a("// Generated ICD dispatch entries. The guest ICD (alr_icd_vulkan.c) #includes this")
    a("// twice: once with ALR_ICD_GEN_DEFINE (emit the C functions) and once with")
    a("// ALR_ICD_GEN_TABLE (emit the ALR_ENTRY rows for alr_lookup). Each function")
    a("// allocates a virtual id, marshals its request via alr_icd_roundtrip, and (for")
    a("// round-tripping ops) decodes the reply. It reuses the ICD's existing helpers:")
    a("//   AlrIcdDevice (dev->vdev), alr_alloc(&counter,1), alr_icd_ring_ok(),")
    a("//   alr_icd_roundtrip(), the generated encoders, and the generated reply scanners")
    a("//   (alr_gpu_vk_gen_icd_runtime.inc) + arena glue (alr_icd_arena_ptr/_mem_record).")
    a("")
    a("// Entrypoints in ICD_SKIP are generated for the host (encoder + decode) but NOT")
    a("// emitted here, because the guest ICD already hand-writes them (a duplicate C symbol")
    a("// would collide). See ICD_SKIP in tools/gen_vk_passthrough.py.")
    a("#ifdef ALR_ICD_GEN_DEFINE")
    for op in ops:
        if op["name"] in ICD_SKIP:
            a(f"// (skipped: {op['name']} is hand-written in the ICD)")
            continue
        a(gen_icd_fn(op))
        a("")
    a("#endif  // ALR_ICD_GEN_DEFINE")
    a("")
    a("#ifdef ALR_ICD_GEN_TABLE")
    for op in ops:
        if op["name"] in ICD_SKIP:
            continue
        a(f'    ALR_ENTRY("{op["name"]}", {op["icd_fn"]}),')
    a("#endif  // ALR_ICD_GEN_TABLE")
    return "\n".join(L) + "\n"


def gen_icd_fn(op):
    k = op["kind"]
    name = op["name"]
    fn = op["icd_fn"]
    L = []
    a = L.append
    if k in ("create_handle", "create_pool"):
        out_ty = op["out"]
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, const {op['ci']} *pCreateInfo,")
        a(f"                          const VkAllocationCallbacks *pAllocator, {out_ty} *pHandle) {{")
        a("    (void)pAllocator;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t vid; int32_t res = 0;")
        a("    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;")
        a(f"    vid = alr_alloc(&{op['counter']}, 1);")
        a("    if (alr_icd_ring_ok()) {")
        a("        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, vid{op['icd_ci_args']});")
        a("        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
        a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("    }")
        a("    if (res != 0) return (VkResult)res;")
        a(f"    *pHandle = ({out_ty})(uintptr_t)vid;  // non-dispatchable: carries the virtual id")
        a("    return VK_SUCCESS;")
        a("}")
    elif k == "alloc_memory":
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,")
        a("                          const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {")
        a("    (void)pAllocator;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t vid; int32_t res = 0;")
        a("    if (!dev || !pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;")
        a(f"    vid = alr_alloc(&{op['counter']}, 1);")
        a("    if (alr_icd_ring_ok()) {")
        a("        uint8_t req[128]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, vid,")
        a("                            (uint64_t)pAllocateInfo->allocationSize,")
        a("                            pAllocateInfo->memoryTypeIndex);")
        a("        alr_vk_gen_pnext_count(&e, 0);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("            uint64_t arena_off = (uint64_t)-1;")
        a(f"            if (rlen) (void)alr_icd_gen_scan_alloc(reply, rlen, (uint16_t){op['reply_enum']}, &res, &arena_off);")
        a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("            alr_icd_mem_record(vid, arena_off, (uint64_t)pAllocateInfo->allocationSize);")
        a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("    }")
        a("    if (res != 0) return (VkResult)res;")
        a("    *pMemory = (VkDeviceMemory)(uintptr_t)vid;")
        a("    return VK_SUCCESS;")
        a("}")
    elif k == "map_memory":
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, VkDeviceMemory memory,")
        a("                          VkDeviceSize offset, VkDeviceSize size,")
        a("                          VkMemoryMapFlags flags, void **ppData) {")
        a("    (void)flags;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t vmem = (uint32_t)(uintptr_t)memory;")
        a("    if (!dev || !ppData || memory == 0) return VK_ERROR_MEMORY_MAP_FAILED;")
        a("    if (alr_icd_ring_ok()) {")
        a("        uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"        {op['enc_name']}(&e, dev->vdev, vmem, (uint64_t)offset, (uint64_t)size);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("            uint64_t mapped_off = (uint64_t)-1; int32_t res = -1;")
        a(f"            if (rlen) (void)alr_icd_gen_scan_map(reply, rlen, (uint16_t){op['reply_enum']}, &res, &mapped_off);")
        a("            void *p = alr_icd_arena_ptr(mapped_off);")
        a("            if (res == 0 && p) { *ppData = p; return VK_SUCCESS; }")
        a("        }")
        a("    }")
        a("    return VK_ERROR_MEMORY_MAP_FAILED;")
        a("}")
    elif k == "unmap_memory":
        a(f"static void VKAPI_CALL {fn}(VkDevice device, VkDeviceMemory memory) {{")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t vmem = (uint32_t)(uintptr_t)memory;")
        a("    if (!dev || memory == 0 || !alr_icd_ring_ok()) return;")
        a("    uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("    alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"    {op['enc_name']}(&e, dev->vdev, vmem);")
        a("    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("}")
    elif k == "flush_ranges":
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, uint32_t memoryRangeCount,")
        a("                          const VkMappedMemoryRange *pMemoryRanges) {")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t i;")
        a("    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;")
        a("    if (!alr_icd_ring_ok() || memoryRangeCount == 0 || !pMemoryRanges) return VK_SUCCESS;")
        a("    {")
        a("        uint8_t req[16 + 64 * 20]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        uint32_t n = memoryRangeCount > 64 ? 64 : memoryRangeCount;")
        a("        alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, n);")
        a("        for (i = 0; i < n; ++i) {")
        a("            uint32_t vmem = (uint32_t)(uintptr_t)pMemoryRanges[i].memory;")
        a(f"            {op['enc_name']}_range(&e, vmem, (uint64_t)pMemoryRanges[i].offset,")
        a("                                (uint64_t)pMemoryRanges[i].size);")
        a("        }")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("    }")
        a("    return VK_SUCCESS;")
        a("}")
    elif k == "get_reqs":
        hp_name, hp_ty = op["handle_param"]
        a(f"static void VKAPI_CALL {fn}(VkDevice device, {hp_ty} {hp_name},")
        a("                          VkMemoryRequirements *pMemoryRequirements) {")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a(f"    uint32_t vh = (uint32_t)(uintptr_t){hp_name};")
        a("    if (!dev || !pMemoryRequirements) return;")
        a("    /* Conservative defaults so a ring-less ICD still returns a usable struct. */")
        a("    pMemoryRequirements->size = 0;")
        a("    pMemoryRequirements->alignment = 256;")
        a("    pMemoryRequirements->memoryTypeBits = 0xffffffffu;")
        a("    if (alr_icd_ring_ok()) {")
        a("        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"        {op['enc_name']}(&e, dev->vdev, vh);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("            uint64_t size = 0, align = 256; uint32_t bits = 0xffffffffu; int32_t res = -1;")
        a(f"            if (rlen && alr_icd_gen_scan_reqs(reply, rlen, (uint16_t){op['reply_enum']}, &res, &size, &align, &bits) && res == 0) {{")
        a("                pMemoryRequirements->size = size;")
        a("                pMemoryRequirements->alignment = align ? align : 256;")
        a("                pMemoryRequirements->memoryTypeBits = bits;")
        a("            }")
        a("        }")
        a("    }")
        a("}")
    elif k == "bind_memory":
        hp_name, hp_ty = op["handle_param"]
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, {hp_ty} {hp_name},")
        a("                          VkDeviceMemory memory, VkDeviceSize memoryOffset) {")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a(f"    uint32_t vh = (uint32_t)(uintptr_t){hp_name};")
        a("    uint32_t vmem = (uint32_t)(uintptr_t)memory; int32_t res = 0;")
        a("    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;")
        a("    if (alr_icd_ring_ok()) {")
        a("        uint8_t req[40]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"        {op['enc_name']}(&e, dev->vdev, vh, vmem, (uint64_t)memoryOffset);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
        a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("    }")
        a("    return (VkResult)res;")
        a("}")
    elif k == "destroy_handle":
        hp_name, hp_ty = op["handle_param"]
        a(f"static void VKAPI_CALL {fn}(VkDevice device, {hp_ty} {hp_name},")
        a("                          const VkAllocationCallbacks *pAllocator) {")
        a("    (void)pAllocator;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a(f"    uint32_t vh = (uint32_t)(uintptr_t){hp_name};")
        a(f"    if (!dev || {hp_name} == 0 || !alr_icd_ring_ok()) return;")
        a("    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("    alr_vk_enc_init(&e, req, sizeof(req));")
        a(f"    {op['enc_name']}(&e, dev->vdev, vh);")
        a("    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("}")
    return "\n".join(L)


# ---------------------------------------------------------------------------
# ICD reply-scan helpers (generated so the field order can never drift from the host
# reply encoder above). Pure C; included by the ICD under ALR_ICD_GEN_DEFINE.
# ---------------------------------------------------------------------------

def gen_icd_runtime(reg, ops):
    ver = reg.header_version()
    L = []
    a = L.append
    a(GEN_BANNER.format(ver=ver))
    a("// Generated ICD-side reply scanners. They walk the generated reply stream the host")
    a("// wrote (each record is u8 ALR_VK_REPLY_GEN_ESCAPE + u16 sub-opcode + payload,")
    a("// terminated by u8 ALR_VK_REPLY_END) and pull the fields each round-tripping op")
    a("// needs. Generated so the field order can never drift from the host encoder. They")
    a("// use the ICD's AlrVkReader (a tiny LE reader the ICD defines).")
    a("#ifdef ALR_ICD_GEN_DEFINE")
    a("")
    a("/* Forward decls (the scanners below call skip/next, which are defined after them). */")
    a("static int alr_icd_gen_skip(AlrVkReader *rd, uint16_t sub);")
    a("static int alr_icd_gen_next(AlrVkReader *rd, uint16_t *sub);")
    a("")
    a("/* Advance `rd` past ONE generated reply record we are not collecting (its sub-op is")
    a(" * `sub`), so the scanner can keep walking. Byte sizes mirror the host reply encoder. */")
    a("static int alr_icd_gen_skip(AlrVkReader *rd, uint16_t sub) {")
    a("    uint32_t u; int32_t i; uint64_t q;")
    a("    switch (sub) {")
    for op in ops:
        if not op["reply_enum"]:
            continue
        sk = op["reply_skip"]
        a(f"        case (uint16_t){op['reply_enum']}: " + sk + " return 1;")
    a("        default: return 0;  /* unknown record: stop (don't guess a size) */")
    a("    }")
    a("    (void)u; (void)i; (void)q;")
    a("}")
    a("")
    a("/* Read the next generated reply record's sub-opcode into *sub. Returns 1 if a record")
    a(" * is present (escape byte consumed + sub read), 0 at ALR_VK_REPLY_END / on malformed")
    a(" * input. A non-escape, non-END byte is treated as end-of-generated-records (a")
    a(" * hand-written reply record the generated scanner doesn't own). */")
    a("static int alr_icd_gen_next(AlrVkReader *rd, uint16_t *sub) {")
    a("    uint8_t op;")
    a("    if (!alr_vk_reader_u8(rd, &op)) return 0;")
    a("    if (op == (uint8_t)ALR_VK_REPLY_END) return 0;")
    a("    if (op != (uint8_t)ALR_VK_REPLY_GEN_ESCAPE) return 0;")
    a("    return alr_vk_reader_u16(rd, sub);")
    a("}")
    a("")
    a("/* { u32 vid, i32 result }. */")
    a("static int alr_icd_gen_scan_result(const uint8_t *buf, uint32_t len, uint16_t want,")
    a("                                   int32_t *res_out) {")
    a("    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;")
    a("    while (alr_icd_gen_next(&rd, &sub)) {")
    a("        if (sub == want) { uint32_t vid; int32_t res;")
    a("            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_i32(&rd, &res)) return 0;")
    a("            if (res_out) *res_out = res; return 1; }")
    a("        if (!alr_icd_gen_skip(&rd, sub)) return 0; }")
    a("    return 0;")
    a("}")
    a("")
    a("/* { u32 vid, i32 result, u64 arena_off }. */")
    a("static int alr_icd_gen_scan_alloc(const uint8_t *buf, uint32_t len, uint16_t want,")
    a("                                  int32_t *res_out, uint64_t *arena_out) {")
    a("    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;")
    a("    while (alr_icd_gen_next(&rd, &sub)) {")
    a("        if (sub == want) { uint32_t vid; int32_t res; uint64_t off;")
    a("            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_i32(&rd, &res) ||")
    a("                !alr_vk_reader_u64(&rd, &off)) return 0;")
    a("            if (res_out) *res_out = res; if (arena_out) *arena_out = off; return 1; }")
    a("        if (!alr_icd_gen_skip(&rd, sub)) return 0; }")
    a("    return 0;")
    a("}")
    a("")
    a("/* { u32 vid, u64 mapped_off, i32 result }. */")
    a("static int alr_icd_gen_scan_map(const uint8_t *buf, uint32_t len, uint16_t want,")
    a("                                int32_t *res_out, uint64_t *off_out) {")
    a("    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;")
    a("    while (alr_icd_gen_next(&rd, &sub)) {")
    a("        if (sub == want) { uint32_t vid; uint64_t off; int32_t res;")
    a("            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_u64(&rd, &off) ||")
    a("                !alr_vk_reader_i32(&rd, &res)) return 0;")
    a("            if (off_out) *off_out = off; if (res_out) *res_out = res; return 1; }")
    a("        if (!alr_icd_gen_skip(&rd, sub)) return 0; }")
    a("    return 0;")
    a("}")
    a("")
    a("/* { u32 vid, i32 result, u64 size, u64 align, u32 bits }. */")
    a("static int alr_icd_gen_scan_reqs(const uint8_t *buf, uint32_t len, uint16_t want,")
    a("                                 int32_t *res_out, uint64_t *size_out,")
    a("                                 uint64_t *align_out, uint32_t *bits_out) {")
    a("    AlrVkReader rd; alr_vk_reader_init(&rd, buf, len); uint16_t sub;")
    a("    while (alr_icd_gen_next(&rd, &sub)) {")
    a("        if (sub == want) { uint32_t vid; int32_t res; uint64_t size, align; uint32_t bits;")
    a("            if (!alr_vk_reader_u32(&rd, &vid) || !alr_vk_reader_i32(&rd, &res) ||")
    a("                !alr_vk_reader_u64(&rd, &size) || !alr_vk_reader_u64(&rd, &align) ||")
    a("                !alr_vk_reader_u32(&rd, &bits)) return 0;")
    a("            if (res_out) *res_out = res; if (size_out) *size_out = size;")
    a("            if (align_out) *align_out = align; if (bits_out) *bits_out = bits; return 1; }")
    a("        if (!alr_icd_gen_skip(&rd, sub)) return 0; }")
    a("    return 0;")
    a("}")
    a("")
    a("#endif  // ALR_ICD_GEN_DEFINE")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# Spec -> op model resolution.
# ---------------------------------------------------------------------------

COUNTERS = {
    "VkCommandPool": "g_next_vpool",
    "VkBuffer": "g_next_vbuf",
    "VkImage": "g_next_vimg",
    "VkImageView": "g_next_vview",
    "VkDeviceMemory": "g_next_vmem",
}
VHANDLE_VAR = {
    "VkCommandPool": "vpool",
    "VkBuffer": "vbuf",
    "VkImage": "vimg",
    "VkImageView": "vview",
    "VkDeviceMemory": "vmem",
}
SHORT = {
    "vkCreateCommandPool": "create_command_pool",
    "vkCreateBuffer": "create_buffer",
    "vkCreateImage": "create_image",
    "vkCreateImageView": "create_image_view",
    "vkGetBufferMemoryRequirements": "get_buffer_reqs",
    "vkGetImageMemoryRequirements": "get_image_reqs",
    "vkBindBufferMemory": "bind_buffer_memory",
    "vkBindImageMemory": "bind_image_memory",
    "vkDestroyCommandPool": "destroy_command_pool",
    "vkDestroyBuffer": "destroy_buffer",
    "vkDestroyImage": "destroy_image",
    "vkDestroyImageView": "destroy_image_view",
    "vkFreeMemory": "free_memory",
}
# Reply-record payload sizes (after the u8 opcode) for the ICD skip table.
REPLY_SKIP = {
    "result": "{ if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)) return 0; }",
    "alloc":  "{ if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)||!alr_vk_reader_u64(rd,&q)) return 0; }",
    "map":    "{ if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_i32(rd,&i)) return 0; }",
    "reqs":   "{ if(!alr_vk_reader_u32(rd,&u)||!alr_vk_reader_i32(rd,&i)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_u64(rd,&q)||!alr_vk_reader_u32(rd,&u)) return 0; }",
}


def resolve_ops(reg):
    ops = []
    op_num = GEN_SUBOP_BASE
    reply_num = GEN_REPLY_BASE
    for spec in SPECS:
        name = spec["name"]
        if name not in reg.commands:
            raise SystemExit(f"[gen] {name} not found in vk.xml — registry mismatch")
        op = dict(spec)
        snake = name[2:]
        out_snake = "".join(
            ("_" + ch.lower()) if ch.isupper() else ch for ch in snake).lstrip("_")
        # GEN-prefixed enum names so the u16 sub-opcodes never collide with the hand-written
        # u8 op/reply enums (which already use names like ALR_VK_OP_CREATE_COMMAND_POOL).
        op["op_enum"] = "ALR_VK_GEN_OP_" + out_snake.upper()
        op["op_num"] = op_num
        op["enc_name"] = "alr_vk_enc_" + out_snake
        op["icd_fn"] = "alr_" + name
        op["short"] = SHORT.get(name, out_snake)
        op_num += 1
        k = spec["kind"]
        has_reply = k in ("create_handle", "create_pool", "alloc_memory", "map_memory",
                          "get_reqs", "bind_memory")
        if has_reply:
            op["reply_enum"] = "ALR_VK_GEN_REPLY_" + out_snake.upper()
            op["reply_num"] = reply_num
            reply_num += 1
            op["reply_skip"] = {
                "create_handle": REPLY_SKIP["result"], "create_pool": REPLY_SKIP["result"],
                "bind_memory": REPLY_SKIP["result"], "alloc_memory": REPLY_SKIP["alloc"],
                "map_memory": REPLY_SKIP["map"], "get_reqs": REPLY_SKIP["reqs"],
            }[k]
        else:
            op["reply_enum"] = None
            op["reply_num"] = None
        if k in ("create_handle", "create_pool", "alloc_memory"):
            out_ty = spec["out"]
            op["counter"] = COUNTERS[out_ty]
            op["vout"] = VHANDLE_VAR[out_ty]
            if k in ("create_handle", "create_pool"):
                op["ci_wire_fields"] = list(spec["ci_fields"])
                icd_args = []
                for fname, wt in spec["ci_fields"]:
                    if fname.startswith("@"):
                        member = fname[1:].split(":", 1)[0]
                        icd_args.append(f"(uint32_t)(uintptr_t)pCreateInfo->{member}")
                    else:
                        icd_args.append(
                            f"(uint32_t)pCreateInfo->{fname}" if wt == "u32"
                            else f"(uint64_t)pCreateInfo->{fname}")
                op["icd_ci_args"] = ("" if not icd_args else ", " + ", ".join(icd_args))
                call_args = [wire_var(f) for f, _ in spec["ci_fields"]]
                op["ci_call_args"] = ", ".join(call_args) if call_args else ""
                scal = [wire_var(f) for f, wt in spec["ci_fields"] if wt in ("u32", "u64")]
                while len(scal) < 2:
                    scal.append("0")
                op["ci_first_two"] = f"(uint64_t){scal[0]}, (uint64_t){scal[1]}"
        if k in ("get_reqs", "bind_memory", "destroy_handle", "map_memory", "unmap_memory"):
            if "handle_param" in spec:
                hp_name, hp_ty = spec["handle_param"]
                op["vhandle"] = VHANDLE_VAR.get(hp_ty, "vh")
        ops.append(op)
    return ops


def main():
    ap = argparse.ArgumentParser(description="Generate ALR Vulkan passthrough marshalling.")
    ap.add_argument("--xml", default=str(DEFAULT_XML), help="path to vk.xml")
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 2) if any generated file is stale, write nothing")
    args = ap.parse_args()

    reg = Registry(Path(args.xml))
    hv = reg.header_version()
    if hv != VK_HEADER_VERSION_PIN:
        print(f"[gen] WARNING: vk.xml VK_HEADER_VERSION={hv} != pinned {VK_HEADER_VERSION_PIN}",
              file=sys.stderr)

    ops = resolve_ops(reg)

    proto = gen_proto(reg, ops)
    decode = gen_decode(reg, ops)
    icd = gen_icd(reg, ops)
    icd_runtime = gen_icd_runtime(reg, ops)

    outputs = {
        GEN_DIR / "alr_gpu_vk_gen_proto.hpp": proto,
        GEN_DIR / "alr_gpu_vk_gen_decode.hpp": decode,
        GEN_DIR / "alr_gpu_vk_gen_icd.inc": icd,
        GEN_DIR / "alr_gpu_vk_gen_icd_runtime.inc": icd_runtime,
    }

    stale = []
    for path, content in outputs.items():
        existing = path.read_text() if path.exists() else None
        if existing != content:
            stale.append(path)
        if not args.check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

    if args.check:
        if stale:
            print("[gen] STALE (regenerate with python3 tools/gen_vk_passthrough.py):",
                  file=sys.stderr)
            for p in stale:
                print("   ", p.relative_to(ROOT), file=sys.stderr)
            sys.exit(2)
        print(f"[gen] up to date ({len(ops)} entrypoints, {len(outputs)} files)")
    else:
        print(f"[gen] wrote {len(outputs)} files for {len(ops)} entrypoints "
              f"(escape op {GEN_ESCAPE_OP}, sub-ops "
              f"{GEN_SUBOP_BASE}..{GEN_SUBOP_BASE + len(ops) - 1})")
        for path in outputs:
            print("   ", path.relative_to(ROOT))


if __name__ == "__main__":
    main()
