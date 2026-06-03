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
ICD_SKIP = {"vkCreateCommandPool", "vkDestroyCommandPool",
            # vkCreate/DestroyShaderModule are hand-written in the ICD on the 218/225 band
            # (the bring-up triangle uploads its own SPIR-V through it). The generated ICD
            # function would collide on the C symbol, so suppress the generated ICD row; the
            # generated encoder + host decode (300.. band) are still emitted so the host wire
            # test + servicer cover the generated shader-module path.
            "vkCreateShaderModule", "vkDestroyShaderModule"}

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
    # =======================================================================
    # WAVE A — the create-resource forwards ANGLE's RendererVk hits right after
    # vkCreateDevice / vkGetPhysicalDeviceMemoryProperties: a shader module (the SPIR-V
    # blob), the pipeline cache, a default sampler, and the sync primitives (fence /
    # semaphore / event) + a query pool. All but the shader module are SCALAR-ONLY
    # create_handle forwards (the registry-typed POD prefix is shipped + the real Mali
    # handle returned). The shader module rides create_handle's NEW optional trailing
    # `blob_field` (codeSize/pCode SPIR-V), marshalled the same way the hand-written
    # CREATE_SHADER_MODULE (op 218) ships its blob — but generated, so it composes with the
    # generated reply band. These let ANGLE build its shader/sampler/sync objects on Mali.
    # =======================================================================
    # ---- shader module (SPIR-V blob: the heaviest WAVE-A item) ----
    {
        "name": "vkCreateShaderModule", "kind": "create_handle",
        "ci": "VkShaderModuleCreateInfo",
        # flags is the only POD scalar; the SPIR-V words ride the trailing blob (codeSize
        # bytes at pCode). The host rebuilds VkShaderModuleCreateInfo from { flags, blob }.
        "ci_fields": [("flags", "u32")],
        # blob_field: (the C member holding the byte length, the C member holding the data
        # pointer). create_handle ships it as a length-prefixed blob AFTER the scalar CI
        # fields and BEFORE the pNext chain; the host decode reads it back as (ptr,len) and
        # the real-Mali body passes it as codeSize/pCode.
        "blob_field": ("codeSize", "pCode"),
        "out": "VkShaderModule",
    },
    {
        "name": "vkDestroyShaderModule", "kind": "destroy_handle",
        "handle_param": ("shaderModule", "VkShaderModule"),
        # NOTE: vkCreate/DestroyShaderModule are ALSO hand-written on the 218/225 band (used
        # by the bring-up triangle). The generated ICD function would collide, so both are in
        # ICD_SKIP — the generated encoder + host decode still exist (for completeness + the
        # host wire test). ANGLE keeps using the generated *create* via the 300.. band only
        # because the hand-written one is what the ICD table exports; see ICD_SKIP note.
    },
    # ---- pipeline cache (scalar-only; ANGLE creates one empty cache up front) ----
    {
        "name": "vkCreatePipelineCache", "kind": "create_handle",
        "ci": "VkPipelineCacheCreateInfo",
        # initialDataSize/pInitialData (a warm cache blob) is NOT shipped for the bring-up:
        # ANGLE's first cache is empty (initialDataSize==0). A warm-cache rung would add a
        # blob_field; deferred (honest scope) — an empty cache is the create ANGLE issues.
        "ci_fields": [("flags", "u32")],
        "out": "VkPipelineCache",
    },
    {
        "name": "vkDestroyPipelineCache", "kind": "destroy_handle",
        "handle_param": ("pipelineCache", "VkPipelineCache"),
    },
    # ---- sampler (scalar-only POD prefix) ----
    {
        "name": "vkCreateSampler", "kind": "create_handle",
        "ci": "VkSamplerCreateInfo",
        "ci_fields": [
            ("flags", "u32"), ("magFilter", "u32"), ("minFilter", "u32"),
            ("mipmapMode", "u32"), ("addressModeU", "u32"), ("addressModeV", "u32"),
            ("addressModeW", "u32"), ("mipLodBias", "f32"), ("anisotropyEnable", "u32"),
            ("maxAnisotropy", "f32"), ("compareEnable", "u32"), ("compareOp", "u32"),
            ("minLod", "f32"), ("maxLod", "f32"), ("borderColor", "u32"),
            ("unnormalizedCoordinates", "u32"),
        ],
        "out": "VkSampler",
    },
    {
        "name": "vkDestroySampler", "kind": "destroy_handle",
        "handle_param": ("sampler", "VkSampler"),
    },
    # ---- fence (scalar-only: just flags) ----
    {
        "name": "vkCreateFence", "kind": "create_handle",
        "ci": "VkFenceCreateInfo",
        "ci_fields": [("flags", "u32")],
        "out": "VkFence",
    },
    {
        "name": "vkDestroyFence", "kind": "destroy_handle",
        "handle_param": ("fence", "VkFence"),
    },
    # ---- semaphore (scalar-only: flags; a timeline semaphore's pNext type is deferred) ----
    {
        "name": "vkCreateSemaphore", "kind": "create_handle",
        "ci": "VkSemaphoreCreateInfo",
        "ci_fields": [("flags", "u32")],
        "out": "VkSemaphore",
    },
    {
        "name": "vkDestroySemaphore", "kind": "destroy_handle",
        "handle_param": ("semaphore", "VkSemaphore"),
    },
    # ---- event (scalar-only: flags) ----
    {
        "name": "vkCreateEvent", "kind": "create_handle",
        "ci": "VkEventCreateInfo",
        "ci_fields": [("flags", "u32")],
        "out": "VkEvent",
    },
    {
        "name": "vkDestroyEvent", "kind": "destroy_handle",
        "handle_param": ("event", "VkEvent"),
    },
    # ---- query pool (scalar-only POD prefix) ----
    {
        "name": "vkCreateQueryPool", "kind": "create_handle",
        "ci": "VkQueryPoolCreateInfo",
        "ci_fields": [
            ("flags", "u32"), ("queryType", "u32"), ("queryCount", "u32"),
            ("pipelineStatistics", "u32"),
        ],
        "out": "VkQueryPool",
    },
    {
        "name": "vkDestroyQueryPool", "kind": "destroy_handle",
        "handle_param": ("queryPool", "VkQueryPool"),
    },
    # =======================================================================
    # WAVE B — the descriptor + layout + render-pass + framebuffer create family ANGLE's
    # RendererVk hits next (the device-iterate trap proved ANGLE reaches descriptor-set
    # management immediately after device/queue setup: its first unimplemented call is
    # vkFreeDescriptorSets). These CreateInfos carry count+array members, so they use the
    # `create_struct` kind: the SPEC declares the scalar POD prefix (`ci_fields`) PLUS named
    # `arrays`, each a { count_field, elem fields }. The generated encoder ships the scalars
    # then each array as a u32 count + count×{elem fields}; the host decode reads them back
    # and a hand-written real-Mali body (alr_gpu_vk_gen_real.hpp) rebuilds the CreateInfo +
    # its arrays (translating any handle-typed element via VkGenTables) and calls real Mali.
    # A handle element is wire-typed "vhandle" (a u32 virtual id translated host-side).
    # =======================================================================
    # ---- descriptor set layout (pBindings[]: binding/type/count/stageFlags; immutable
    #      samplers are NOT shipped for the bring-up — ANGLE uses dynamic samplers here) ----
    {
        "name": "vkCreateDescriptorSetLayout", "kind": "create_struct",
        "ci": "VkDescriptorSetLayoutCreateInfo",
        "ci_fields": [("flags", "u32")],
        "arrays": [
            {"count_field": "bindingCount", "ptr_field": "pBindings",
             "elem": "VkDescriptorSetLayoutBinding",
             "fields": [("binding", "u32"), ("descriptorType", "u32"),
                        ("descriptorCount", "u32"), ("stageFlags", "u32")],
             # pImmutableSamplers (a sampler-handle array per binding) is deferred: ANGLE's
             # texture path uses separate (non-immutable) samplers, so the binding's
             # immutable-sampler pointer is null for the bring-up. Honest scope note.
             "note": "pImmutableSamplers deferred (ANGLE uses non-immutable samplers)"},
        ],
        "out": "VkDescriptorSetLayout",
    },
    {
        "name": "vkDestroyDescriptorSetLayout", "kind": "destroy_handle",
        "handle_param": ("descriptorSetLayout", "VkDescriptorSetLayout"),
    },
    # ---- pipeline layout (pSetLayouts[]: descriptor-set-layout HANDLES; pPushConstant
    #      Ranges[]: stageFlags/offset/size) ----
    {
        "name": "vkCreatePipelineLayout", "kind": "create_struct",
        "ci": "VkPipelineLayoutCreateInfo",
        "ci_fields": [("flags", "u32")],
        "arrays": [
            {"count_field": "setLayoutCount", "ptr_field": "pSetLayouts",
             "elem": "VkDescriptorSetLayout", "handle_elem": "VkDescriptorSetLayout",
             "fields": [("@self:VkDescriptorSetLayout", "vhandle")]},
            {"count_field": "pushConstantRangeCount", "ptr_field": "pPushConstantRanges",
             "elem": "VkPushConstantRange",
             "fields": [("stageFlags", "u32"), ("offset", "u32"), ("size", "u32")]},
        ],
        "out": "VkPipelineLayout",
    },
    {
        "name": "vkDestroyPipelineLayout", "kind": "destroy_handle",
        "handle_param": ("pipelineLayout", "VkPipelineLayout"),
    },
    # ---- descriptor pool (maxSets + pPoolSizes[]: type/descriptorCount) ----
    {
        "name": "vkCreateDescriptorPool", "kind": "create_struct",
        "ci": "VkDescriptorPoolCreateInfo",
        "ci_fields": [("flags", "u32"), ("maxSets", "u32")],
        "arrays": [
            {"count_field": "poolSizeCount", "ptr_field": "pPoolSizes",
             "elem": "VkDescriptorPoolSize",
             "fields": [("type", "u32"), ("descriptorCount", "u32")]},
        ],
        "out": "VkDescriptorPool",
    },
    {
        "name": "vkDestroyDescriptorPool", "kind": "destroy_handle",
        "handle_param": ("descriptorPool", "VkDescriptorPool"),
    },
    # ---- descriptor sets: allocate N from a pool against N layouts (the alloc_sets kind:
    #      VkDescriptorSetAllocateInfo carries the pool HANDLE + a setLayout-HANDLE array; the
    #      host allocates the real sets + returns N virtual ids the guest pre-assigned). ----
    {
        "name": "vkAllocateDescriptorSets", "kind": "alloc_sets",
        "out": "VkDescriptorSet",
    },
    {
        "name": "vkFreeDescriptorSets", "kind": "free_sets",
    },
    # ---- descriptor set writes: bind buffers/images/samplers into sets (the update_sets
    #      kind: an array of VkWriteDescriptorSet, each referencing a destination set HANDLE
    #      and, per descriptor, buffer/image-view/sampler HANDLES). Copies are deferred. ----
    {
        "name": "vkUpdateDescriptorSets", "kind": "update_sets",
    },
    # =======================================================================
    # WAVE C — the render-pass / framebuffer / pipeline create family ANGLE hits once it
    # starts RENDERING (after the descriptor stage). These are the heaviest creates: a render
    # pass has nested subpasses (each an array of attachment-references), and a graphics
    # pipeline is a deep nested-state CreateInfo whose pStages reference shader modules + that
    # references a pipeline layout + render pass. Render pass + pipelines use DEDICATED kinds
    # (create_render_pass / create_pipelines) whose hand-written real bodies do the nested
    # reconstruction; framebuffer fits the create_struct kind (a flat image-view handle array
    # + a render-pass handle). NOTE: ANGLE is currently blocked UPSTREAM at device-chain
    # creation (host-service op 204), so these create-forwards are staged + ready for when
    # that unblocks — they are not yet reached on device.
    # =======================================================================
    # ---- render pass (attachments[] + subpasses[] {nested attachment-ref arrays} +
    #      dependencies[]). The nested subpass arrays make this a DEDICATED kind. ----
    {
        "name": "vkCreateRenderPass", "kind": "create_render_pass",
        "out": "VkRenderPass",
    },
    {
        "name": "vkDestroyRenderPass", "kind": "destroy_handle",
        "handle_param": ("renderPass", "VkRenderPass"),
    },
    # ---- framebuffer (renderPass HANDLE + pAttachments[]: image-view HANDLES + w/h/layers).
    #      Imageless framebuffers (VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) deferred. ----
    {
        "name": "vkCreateFramebuffer", "kind": "create_struct",
        "ci": "VkFramebufferCreateInfo",
        "ci_fields": [("flags", "u32"), ("@renderPass:VkRenderPass", "vhandle"),
                      ("width", "u32"), ("height", "u32"), ("layers", "u32")],
        "arrays": [
            {"count_field": "attachmentCount", "ptr_field": "pAttachments",
             "elem": "VkImageView", "handle_elem": "VkImageView",
             "fields": [("@self:VkImageView", "vhandle")]},
        ],
        "out": "VkFramebuffer",
    },
    {
        "name": "vkDestroyFramebuffer", "kind": "destroy_handle",
        "handle_param": ("framebuffer", "VkFramebuffer"),
    },
    # ---- graphics + compute pipelines: the HEAVIEST creates. A DEDICATED create_pipelines
    #      kind whose hand-written real body (alr_gpu_vk_gen_real.hpp) rebuilds the deep nested
    #      VkGraphicsPipelineCreateInfo / VkComputePipelineCreateInfo. The marshalling is
    #      faithful: pStages (each a VkPipelineShaderStageCreateInfo — stage, the shader-MODULE
    #      handle translated via VkGenTables, the pName entry-point string, and an optional
    #      pSpecializationInfo with its map-entry array + data blob), then EACH fixed-function
    #      sub-state shipped behind a presence flag — pVertexInputState (binding+attribute
    #      arrays), pInputAssemblyState, pTessellationState, pViewportState (viewport+scissor
    #      arrays), pRasterizationState, pMultisampleState (+ optional sample-mask words),
    #      pDepthStencilState, pColorBlendState (per-attachment array + blendConstants[4]),
    #      pDynamicState (dynamic-state array) — plus the layout / renderPass(+subpass) /
    #      basePipelineHandle HANDLES (translated via VkGenTables) and the create flags. The
    #      host calls real Mali vkCreate{Graphics,Compute}Pipelines(pipelineCache, count, ...)
    #      with the rebuilt CreateInfos + real handles, then registers each returned pipeline
    #      via cmd_register_pipeline so vkCmdBindPipeline can translate the vid. createInfoCount
    #      pipelines + an optional pipelineCache (a virtual id; 0 == VK_NULL_HANDLE) ride the
    #      wire. Compute is the same machinery with a single stage and no fixed-function state.
    {
        "name": "vkCreateGraphicsPipelines", "kind": "create_pipelines",
        "pipeline_kind": "graphics", "out": "VkPipeline",
    },
    {
        "name": "vkCreateComputePipelines", "kind": "create_pipelines",
        "pipeline_kind": "compute", "out": "VkPipeline",
    },
    {
        "name": "vkDestroyPipeline", "kind": "destroy_handle",
        "handle_param": ("pipeline", "VkPipeline"),
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
        "i32": f"alr_vk_enc_i32(e, {var});",
        "f32": f"alr_vk_enc_f32(e, {var});",
        "vhandle": f"alr_vk_enc_u32(e, {var});",
    }[wt]


def reader_call(wt, var):
    return {"u32": f"r.u32({var})", "u64": f"r.u64({var})", "i32": f"r.i32({var})",
            "f32": f"r.f32({var})", "vhandle": f"r.u32({var})"}[wt]


def ctype_for(wt):
    return {"u32": "uint32_t", "u64": "uint64_t", "i32": "int32_t",
            "f32": "float", "vhandle": "uint32_t"}[wt]


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
        if op.get("blob_field"):
            # A trailing length-prefixed blob (e.g. shader-module SPIR-V): the data pointer +
            # byte length, shipped via alr_vk_enc_blob AFTER the scalar CI fields and BEFORE
            # the pNext chain. The host decode reads it in the same position.
            params.append("const void *blob_data")
            params.append("uint32_t blob_len")
        a(f"// Encoder for {name}. Ships the device + the guest's virtual {op['out']} id +")
        a(f"// the {op['ci']} POD prefix" +
          (" + a trailing blob" if op.get("blob_field") else "") + ". Append the allowlisted")
        a("// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's")
        a("// feature-chain shape).")
        a(f"static inline void {enc}_begin({', '.join(params)}) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a(f"    alr_vk_enc_u32(e, {op['vout']});")
        for fname, wt in op["ci_wire_fields"]:
            a(f"    {wire_enc_call(wt, wire_var(fname))}")
        if op.get("blob_field"):
            a("    alr_vk_enc_blob(e, blob_data, blob_len);")
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
    elif k == "create_struct":
        # _begin: escape + subop + vdev + vout + scalar CI fields. Then per array a
        # _<array>_count + _<array>_elem appender (same shape as flush_ranges' _begin/_range).
        # The caller appends each array's count + elements, then the pNext chain.
        params = ["AlrVkEncoder *e", "uint32_t vdev", f"uint32_t {op['vout']}"]
        for fname, wt in op["ci_wire_fields"]:
            params.append(f"{ctype_for(wt)} {wire_var(fname)}")
        a(f"// Encoder for {name} ({op['ci']} with array members). _begin ships the scalar POD")
        a("// prefix; then per array call _<array>_count + _<array>_elem; then the pNext chain.")
        a(f"static inline void {enc}_begin({', '.join(params)}) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a(f"    alr_vk_enc_u32(e, {op['vout']});")
        for fname, wt in op["ci_wire_fields"]:
            a(f"    {wire_enc_call(wt, wire_var(fname))}")
        a("}")
        for arr in op["arrays"]:
            an = arr["array_name"]
            a(f"static inline void {enc}_{an}_count(AlrVkEncoder *e, uint32_t count) {{")
            a("    alr_vk_enc_u32(e, count);")
            a("}")
            ep = ["AlrVkEncoder *e"]
            for fname, wt in arr["fields"]:
                ep.append(f"{ctype_for(wt)} {wire_var(fname)}")
            a(f"static inline void {enc}_{an}_elem({', '.join(ep)}) {{")
            for fname, wt in arr["fields"]:
                a(f"    {wire_enc_call(wt, wire_var(fname))}")
            a("}")
    elif k == "alloc_sets":
        # vkAllocateDescriptorSets: ship the device + the pool HANDLE + N (the set count) +
        # N setLayout HANDLES + N guest-assigned virtual set ids. Round-trips a per-set result.
        a(f"// Encoder for {name}. Ships the device + descriptor pool (virtual) + the set count;")
        a("// then per set call _layout (the set's layout virtual handle) and _vset (the guest's")
        a("// virtual id for that set). The host allocates the real sets + returns the result.")
        a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool,")
        a("                          uint32_t set_count) {")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, vpool);")
        a("    alr_vk_enc_u32(e, set_count);")
        a("}")
        a(f"static inline void {enc}_set(AlrVkEncoder *e, uint32_t vlayout, uint32_t vset) {{")
        a("    alr_vk_enc_u32(e, vlayout);")
        a("    alr_vk_enc_u32(e, vset);")
        a("}")
    elif k == "free_sets":
        a(f"// Encoder for {name}. Ships the device + the pool + the set count + each set's")
        a("// virtual id; the host frees the real sets back to the real pool (no reply).")
        a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool,")
        a("                          uint32_t set_count) {")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, vpool);")
        a("    alr_vk_enc_u32(e, set_count);")
        a("}")
        a(f"static inline void {enc}_set(AlrVkEncoder *e, uint32_t vset) {{")
        a("    alr_vk_enc_u32(e, vset);")
        a("}")
    elif k == "update_sets":
        # vkUpdateDescriptorSets: ship the device + write count; then per write the dst set
        # HANDLE, binding, arrayElement, descriptorType, and a descriptor count + per
        # descriptor a (vbuffer/voffset/vrange) OR (vimageview/vsampler/imagelayout) triple,
        # tagged by whether the type is a buffer or image/sampler descriptor. Copies deferred.
        a(f"// Encoder for {name}. _begin ships device + writeCount; per write call _write")
        a("// (dst set + binding + arrayElement + descriptorType + descriptorCount) then, per")
        a("// descriptor, _buffer_info OR _image_info matching the descriptor type. No reply.")
        a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t write_count) {{")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, write_count);")
        a("}")
        a(f"static inline void {enc}_write(AlrVkEncoder *e, uint32_t vdstset, uint32_t binding,")
        a("                          uint32_t array_element, uint32_t descriptor_type,")
        a("                          uint32_t descriptor_count) {")
        a("    alr_vk_enc_u32(e, vdstset);")
        a("    alr_vk_enc_u32(e, binding);")
        a("    alr_vk_enc_u32(e, array_element);")
        a("    alr_vk_enc_u32(e, descriptor_type);")
        a("    alr_vk_enc_u32(e, descriptor_count);")
        a("}")
        a(f"static inline void {enc}_buffer_info(AlrVkEncoder *e, uint32_t vbuffer,")
        a("                          uint64_t offset, uint64_t range) {")
        a("    alr_vk_enc_u32(e, vbuffer);")
        a("    alr_vk_enc_u64(e, offset);")
        a("    alr_vk_enc_u64(e, range);")
        a("}")
        a(f"static inline void {enc}_image_info(AlrVkEncoder *e, uint32_t vsampler,")
        a("                          uint32_t vimageview, uint32_t image_layout) {")
        a("    alr_vk_enc_u32(e, vsampler);")
        a("    alr_vk_enc_u32(e, vimageview);")
        a("    alr_vk_enc_u32(e, image_layout);")
        a("}")
    elif k == "create_render_pass":
        # The render pass: scalar flags, then three top-level arrays (attachments, subpasses,
        # dependencies). Subpasses themselves carry nested attachment-reference sub-arrays, so
        # the subpass encoder is split into a _subpass_begin (its scalar prefix + counts) +
        # per-reference appenders. The ICD walks the real VkRenderPassCreateInfo to drive these.
        a(f"// Encoder for {name} (DEDICATED: nested subpasses). _begin ships flags; then the")
        a("// attachments array (_attachment), the subpasses array (each _subpass_begin + its")
        a("// _ref / _preserve elements), and the dependencies array (_dependency); then pNext.")
        a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vrpass,")
        a("                          uint32_t flags) {")
        a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
        a("    alr_vk_enc_u32(e, vdev);")
        a("    alr_vk_enc_u32(e, vrpass);")
        a("    alr_vk_enc_u32(e, flags);")
        a("}")
        a(f"static inline void {enc}_attachment_count(AlrVkEncoder *e, uint32_t n) {{ alr_vk_enc_u32(e, n); }}")
        a(f"static inline void {enc}_attachment(AlrVkEncoder *e, uint32_t flags, uint32_t format,")
        a("                          uint32_t samples, uint32_t loadOp, uint32_t storeOp,")
        a("                          uint32_t stencilLoadOp, uint32_t stencilStoreOp,")
        a("                          uint32_t initialLayout, uint32_t finalLayout) {")
        a("    alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, format); alr_vk_enc_u32(e, samples);")
        a("    alr_vk_enc_u32(e, loadOp); alr_vk_enc_u32(e, storeOp);")
        a("    alr_vk_enc_u32(e, stencilLoadOp); alr_vk_enc_u32(e, stencilStoreOp);")
        a("    alr_vk_enc_u32(e, initialLayout); alr_vk_enc_u32(e, finalLayout);")
        a("}")
        a(f"static inline void {enc}_subpass_count(AlrVkEncoder *e, uint32_t n) {{ alr_vk_enc_u32(e, n); }}")
        a(f"// A subpass: scalar prefix + the 4 reference-array counts + a has-depth flag, then")
        a("// the caller appends input refs, color refs, resolve refs (if any), the depth ref")
        a("// (if any), and the preserve indices, in that fixed order.")
        a(f"static inline void {enc}_subpass_begin(AlrVkEncoder *e, uint32_t flags,")
        a("                          uint32_t pipelineBindPoint, uint32_t inputCount,")
        a("                          uint32_t colorCount, uint32_t resolveCount,")
        a("                          uint32_t hasDepth, uint32_t preserveCount) {")
        a("    alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, pipelineBindPoint);")
        a("    alr_vk_enc_u32(e, inputCount); alr_vk_enc_u32(e, colorCount);")
        a("    alr_vk_enc_u32(e, resolveCount); alr_vk_enc_u32(e, hasDepth);")
        a("    alr_vk_enc_u32(e, preserveCount);")
        a("}")
        a(f"static inline void {enc}_ref(AlrVkEncoder *e, uint32_t attachment, uint32_t layout) {{")
        a("    alr_vk_enc_u32(e, attachment); alr_vk_enc_u32(e, layout);")
        a("}")
        a(f"static inline void {enc}_preserve(AlrVkEncoder *e, uint32_t attachment) {{ alr_vk_enc_u32(e, attachment); }}")
        a(f"static inline void {enc}_dependency_count(AlrVkEncoder *e, uint32_t n) {{ alr_vk_enc_u32(e, n); }}")
        a(f"static inline void {enc}_dependency(AlrVkEncoder *e, uint32_t srcSubpass,")
        a("                          uint32_t dstSubpass, uint32_t srcStageMask,")
        a("                          uint32_t dstStageMask, uint32_t srcAccessMask,")
        a("                          uint32_t dstAccessMask, uint32_t dependencyFlags) {")
        a("    alr_vk_enc_u32(e, srcSubpass); alr_vk_enc_u32(e, dstSubpass);")
        a("    alr_vk_enc_u32(e, srcStageMask); alr_vk_enc_u32(e, dstStageMask);")
        a("    alr_vk_enc_u32(e, srcAccessMask); alr_vk_enc_u32(e, dstAccessMask);")
        a("    alr_vk_enc_u32(e, dependencyFlags);")
        a("}")
    elif k == "create_pipelines":
        a(gen_proto_encoder_pipelines(op))
    return "\n".join(L)


# ---------------------------------------------------------------------------
# WAVE D — vkCreateGraphicsPipelines / vkCreateComputePipelines: the deepest nested create.
# A DEDICATED encoder family (the create_render_pass split-encoder style, but bigger): a
# _begin (device + pipelineCache vid + pipeline count), then per pipeline a _pipeline header
# (flags + layout/renderPass/subpass/basePipeline handles + stage count), per stage a _stage
# (stage + module handle + entry-point name blob + an optional specialization-info), and —
# for GRAPHICS — each fixed-function sub-state behind a presence flag with its own element
# appenders. The ICD walks the real VkGraphics/ComputePipelineCreateInfo to drive these; the
# host decode reads them back into wire PODs; the hand-written real body rebuilds the typed
# CreateInfo + translates handles via VkGenTables and calls real Mali. Shared by both pipeline
# entrypoints (graphics emits the fixed-function appenders; compute uses only _begin/_pipeline/
# _stage and ships exactly one stage with no fixed-function state).
# ---------------------------------------------------------------------------
def gen_proto_encoder_pipelines(op):
    enc = op["enc_name"]
    name = op["name"]
    is_graphics = op["pipeline_kind"] == "graphics"
    L = []
    a = L.append
    a(f"// Encoder for {name} (DEDICATED: deep nested pipeline state). _begin ships the device,")
    a("// the pipeline-cache virtual id (0 == VK_NULL_HANDLE), and the pipeline count; then per")
    a("// pipeline _pipeline (flags + layout/renderPass/subpass/basePipeline handles + the stage")
    a("// count), per stage _stage (+ optional _stage_spec_*), and" +
      (" the fixed-function sub-states" if is_graphics else " — compute has no fixed-function") +
      ".")
    a(f"static inline void {enc}_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpcache,")
    a("                          uint32_t pipeline_count) {")
    a(f"    alr_vk_gen_op_begin(e, {op['op_enum']});")
    a("    alr_vk_enc_u32(e, vdev);")
    a("    alr_vk_enc_u32(e, vpcache);")
    a("    alr_vk_enc_u32(e, pipeline_count);")
    a("}")
    a("// One pipeline header: the guest's virtual id for THIS pipeline (vpipe; the host maps it")
    a("// to the real Mali pipeline + registers it so vkCmdBindPipeline can translate), the create")
    a("// flags, the layout / renderPass / basePipeline HANDLES (virtual ids; 0 == VK_NULL_HANDLE),")
    a("// the subpass index, the basePipelineIndex, and the stage count. (renderPass/subpass are 0")
    a("// for compute.)")
    a(f"static inline void {enc}_pipeline(AlrVkEncoder *e, uint32_t vpipe, uint32_t flags,")
    a("                          uint32_t vlayout, uint32_t vrenderpass, uint32_t subpass,")
    a("                          uint32_t vbase, int32_t base_index, uint32_t stage_count) {")
    a("    alr_vk_enc_u32(e, vpipe); alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, vlayout);")
    a("    alr_vk_enc_u32(e, vrenderpass); alr_vk_enc_u32(e, subpass); alr_vk_enc_u32(e, vbase);")
    a("    alr_vk_enc_i32(e, base_index); alr_vk_enc_u32(e, stage_count);")
    a("}")
    a("// One shader stage: the stage bit, the shader-MODULE handle (virtual id), the")
    a("// entry-point name as a length-prefixed blob, and a spec-present flag. If present, the")
    a("// caller then appends _stage_spec_begin + per-entry _stage_spec_entry + _stage_spec_data.")
    a(f"static inline void {enc}_stage(AlrVkEncoder *e, uint32_t stage, uint32_t vmodule,")
    a("                          const void *name, uint32_t name_len, uint32_t spec_present) {")
    a("    alr_vk_enc_u32(e, stage); alr_vk_enc_u32(e, vmodule);")
    a("    alr_vk_enc_blob(e, name, name_len);")
    a("    alr_vk_enc_u32(e, spec_present);")
    a("}")
    a(f"static inline void {enc}_stage_spec_begin(AlrVkEncoder *e, uint32_t map_entry_count,")
    a("                          uint32_t data_len) {")
    a("    alr_vk_enc_u32(e, map_entry_count); alr_vk_enc_u32(e, data_len);")
    a("}")
    a(f"static inline void {enc}_stage_spec_entry(AlrVkEncoder *e, uint32_t constantID,")
    a("                          uint32_t offset, uint32_t size) {")
    a("    alr_vk_enc_u32(e, constantID); alr_vk_enc_u32(e, offset); alr_vk_enc_u32(e, size);")
    a("}")
    a(f"static inline void {enc}_stage_spec_data(AlrVkEncoder *e, const void *data, uint32_t len) {{")
    a("    alr_vk_enc_blob(e, data, len);")
    a("}")
    if not is_graphics:
        return "\n".join(L)
    # ---- GRAPHICS fixed-function sub-state appenders (each behind a presence flag) ----
    a("// ---- Fixed-function sub-states (GRAPHICS). Each _<state>(present) ships a u32 presence")
    a("// flag; when present the caller then appends that state's fields/elements in order. ----")
    a(f"static inline void {enc}_vertex_input(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t binding_count, uint32_t attr_count) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (present) { alr_vk_enc_u32(e, binding_count); alr_vk_enc_u32(e, attr_count); }")
    a("}")
    a(f"static inline void {enc}_vertex_binding(AlrVkEncoder *e, uint32_t binding,")
    a("                          uint32_t stride, uint32_t inputRate) {")
    a("    alr_vk_enc_u32(e, binding); alr_vk_enc_u32(e, stride); alr_vk_enc_u32(e, inputRate);")
    a("}")
    a(f"static inline void {enc}_vertex_attr(AlrVkEncoder *e, uint32_t location,")
    a("                          uint32_t binding, uint32_t format, uint32_t offset) {")
    a("    alr_vk_enc_u32(e, location); alr_vk_enc_u32(e, binding);")
    a("    alr_vk_enc_u32(e, format); alr_vk_enc_u32(e, offset);")
    a("}")
    a(f"static inline void {enc}_input_assembly(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t topology, uint32_t primitiveRestartEnable) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (present) { alr_vk_enc_u32(e, topology); alr_vk_enc_u32(e, primitiveRestartEnable); }")
    a("}")
    a(f"static inline void {enc}_tessellation(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t patchControlPoints) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (present) alr_vk_enc_u32(e, patchControlPoints);")
    a("}")
    a(f"static inline void {enc}_viewport(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t viewport_count, uint32_t scissor_count) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (present) { alr_vk_enc_u32(e, viewport_count); alr_vk_enc_u32(e, scissor_count); }")
    a("}")
    a(f"static inline void {enc}_viewport_elem(AlrVkEncoder *e, float x, float y, float w,")
    a("                          float h, float minDepth, float maxDepth) {")
    a("    alr_vk_enc_f32(e, x); alr_vk_enc_f32(e, y); alr_vk_enc_f32(e, w);")
    a("    alr_vk_enc_f32(e, h); alr_vk_enc_f32(e, minDepth); alr_vk_enc_f32(e, maxDepth);")
    a("}")
    a(f"static inline void {enc}_scissor_elem(AlrVkEncoder *e, int32_t offX, int32_t offY,")
    a("                          uint32_t extW, uint32_t extH) {")
    a("    alr_vk_enc_i32(e, offX); alr_vk_enc_i32(e, offY);")
    a("    alr_vk_enc_u32(e, extW); alr_vk_enc_u32(e, extH);")
    a("}")
    a(f"static inline void {enc}_rasterization(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t depthClampEnable, uint32_t rasterizerDiscardEnable,")
    a("                          uint32_t polygonMode, uint32_t cullMode, uint32_t frontFace,")
    a("                          uint32_t depthBiasEnable, float depthBiasConstantFactor,")
    a("                          float depthBiasClamp, float depthBiasSlopeFactor,")
    a("                          float lineWidth) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (!present) return;")
    a("    alr_vk_enc_u32(e, depthClampEnable); alr_vk_enc_u32(e, rasterizerDiscardEnable);")
    a("    alr_vk_enc_u32(e, polygonMode); alr_vk_enc_u32(e, cullMode); alr_vk_enc_u32(e, frontFace);")
    a("    alr_vk_enc_u32(e, depthBiasEnable); alr_vk_enc_f32(e, depthBiasConstantFactor);")
    a("    alr_vk_enc_f32(e, depthBiasClamp); alr_vk_enc_f32(e, depthBiasSlopeFactor);")
    a("    alr_vk_enc_f32(e, lineWidth);")
    a("}")
    a(f"static inline void {enc}_multisample(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t rasterizationSamples, uint32_t sampleShadingEnable,")
    a("                          float minSampleShading, uint32_t sampleMaskWordCount,")
    a("                          uint32_t alphaToCoverageEnable, uint32_t alphaToOneEnable) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (!present) return;")
    a("    alr_vk_enc_u32(e, rasterizationSamples); alr_vk_enc_u32(e, sampleShadingEnable);")
    a("    alr_vk_enc_f32(e, minSampleShading); alr_vk_enc_u32(e, sampleMaskWordCount);")
    a("    alr_vk_enc_u32(e, alphaToCoverageEnable); alr_vk_enc_u32(e, alphaToOneEnable);")
    a("}")
    a(f"static inline void {enc}_sample_mask(AlrVkEncoder *e, uint32_t word) {{ alr_vk_enc_u32(e, word); }}")
    a(f"static inline void {enc}_depth_stencil(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t depthTestEnable, uint32_t depthWriteEnable,")
    a("                          uint32_t depthCompareOp, uint32_t depthBoundsTestEnable,")
    a("                          uint32_t stencilTestEnable, float minDepthBounds,")
    a("                          float maxDepthBounds) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (!present) return;")
    a("    alr_vk_enc_u32(e, depthTestEnable); alr_vk_enc_u32(e, depthWriteEnable);")
    a("    alr_vk_enc_u32(e, depthCompareOp); alr_vk_enc_u32(e, depthBoundsTestEnable);")
    a("    alr_vk_enc_u32(e, stencilTestEnable);")
    a("    alr_vk_enc_f32(e, minDepthBounds); alr_vk_enc_f32(e, maxDepthBounds);")
    a("}")
    a("// A VkStencilOpState (front/back): failOp,passOp,depthFailOp,compareOp (4×u32) +")
    a("// compareMask,writeMask,reference (3×u32). Called twice per depth-stencil (front, back).")
    a(f"static inline void {enc}_stencil_op(AlrVkEncoder *e, uint32_t failOp, uint32_t passOp,")
    a("                          uint32_t depthFailOp, uint32_t compareOp, uint32_t compareMask,")
    a("                          uint32_t writeMask, uint32_t reference) {")
    a("    alr_vk_enc_u32(e, failOp); alr_vk_enc_u32(e, passOp); alr_vk_enc_u32(e, depthFailOp);")
    a("    alr_vk_enc_u32(e, compareOp); alr_vk_enc_u32(e, compareMask);")
    a("    alr_vk_enc_u32(e, writeMask); alr_vk_enc_u32(e, reference);")
    a("}")
    a(f"static inline void {enc}_color_blend(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t logicOpEnable, uint32_t logicOp,")
    a("                          uint32_t attachment_count, float bc0, float bc1, float bc2,")
    a("                          float bc3) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (!present) return;")
    a("    alr_vk_enc_u32(e, logicOpEnable); alr_vk_enc_u32(e, logicOp);")
    a("    alr_vk_enc_u32(e, attachment_count);")
    a("    alr_vk_enc_f32(e, bc0); alr_vk_enc_f32(e, bc1); alr_vk_enc_f32(e, bc2); alr_vk_enc_f32(e, bc3);")
    a("}")
    a(f"static inline void {enc}_blend_attachment(AlrVkEncoder *e, uint32_t blendEnable,")
    a("                          uint32_t srcColorBlendFactor, uint32_t dstColorBlendFactor,")
    a("                          uint32_t colorBlendOp, uint32_t srcAlphaBlendFactor,")
    a("                          uint32_t dstAlphaBlendFactor, uint32_t alphaBlendOp,")
    a("                          uint32_t colorWriteMask) {")
    a("    alr_vk_enc_u32(e, blendEnable); alr_vk_enc_u32(e, srcColorBlendFactor);")
    a("    alr_vk_enc_u32(e, dstColorBlendFactor); alr_vk_enc_u32(e, colorBlendOp);")
    a("    alr_vk_enc_u32(e, srcAlphaBlendFactor); alr_vk_enc_u32(e, dstAlphaBlendFactor);")
    a("    alr_vk_enc_u32(e, alphaBlendOp); alr_vk_enc_u32(e, colorWriteMask);")
    a("}")
    a(f"static inline void {enc}_dynamic_state(AlrVkEncoder *e, uint32_t present,")
    a("                          uint32_t dynamic_state_count) {")
    a("    alr_vk_enc_u32(e, present);")
    a("    if (present) alr_vk_enc_u32(e, dynamic_state_count);")
    a("}")
    a(f"static inline void {enc}_dynamic_elem(AlrVkEncoder *e, uint32_t state) {{ alr_vk_enc_u32(e, state); }}")
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
    a(gen_struct_helpers(ops))
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


def gen_struct_helpers(ops):
    """Wire-side POD structs the create_struct/alloc_sets/update_sets decode reads into and
    the hand-written real bodies consume. Plain u32/u64 fields (no Vulkan types), so they
    live in block 1 (visible to both the decode cases and the real bodies). Defined for ALL
    builds (the no-SDK wire test reads into them too)."""
    L = []
    a = L.append
    a("// ---- Wire-side element/struct PODs for the array-bearing generated ops "
      "(create_struct,")
    a("// alloc_sets, update_sets). All-scalar so they need no Vulkan headers; the real-Mali")
    a("// bodies translate the virtual handle fields (vbuffer/vimageview/vsampler/...) to real")
    a("// Mali handles via VkGenTables when rebuilding the typed CreateInfo/array. ----")
    seen = set()
    for op in ops:
        if op["kind"] != "create_struct":
            continue
        for arr in op["arrays"]:
            tname = f"VkGenElem_{op['short']}_{arr['array_name']}"
            if tname in seen:
                continue
            seen.add(tname)
            a(f"struct {tname} {{  // one {arr['elem']}")
            for fname, wt in arr["fields"]:
                a(f"    {ctype_for(wt)} {wire_var(fname)} = 0;")
            a("};")
    # The descriptor-write PODs (update_sets) — fixed shape, emitted once.
    a("struct VkGenBufferInfo { uint32_t vbuffer = 0; uint64_t offset = 0; uint64_t range = 0; };")
    a("struct VkGenImageInfo  { uint32_t vsampler = 0; uint32_t vimageview = 0; "
      "uint32_t image_layout = 0; };")
    a("struct VkGenDescWrite {")
    a("    uint32_t vdstset = 0, binding = 0, array_element = 0;")
    a("    uint32_t descriptor_type = 0, descriptor_count = 0;")
    a("    std::vector<VkGenBufferInfo> buffers;  // for buffer-class descriptors")
    a("    std::vector<VkGenImageInfo> images;    // for image/sampler-class descriptors")
    a("};")
    a("// Image-class descriptor types (sampler / sampled-image / storage-image / combined /")
    a("// input-attachment) ship an image-info triple per descriptor; the rest ship a buffer")
    a("// triple. Matches the VkDescriptorType enum values (stable wire numbers).")
    a("inline bool vk_gen_desc_is_image(uint32_t t) {")
    a("    switch (t) {")
    a("        case 0:  // VK_DESCRIPTOR_TYPE_SAMPLER")
    a("        case 1:  // COMBINED_IMAGE_SAMPLER")
    a("        case 2:  // SAMPLED_IMAGE")
    a("        case 3:  // STORAGE_IMAGE")
    a("        case 10: // INPUT_ATTACHMENT")
    a("            return true;")
    a("        default:")
    a("            return false;  // UNIFORM_BUFFER / STORAGE_BUFFER / *_DYNAMIC / texel buffers")
    a("    }")
    a("}")
    # Render-pass wire PODs (create_render_pass) — emitted only if a render pass is in SPECS.
    if any(op["kind"] == "create_render_pass" for op in ops):
        a("// ---- Render-pass wire PODs (the nested create_render_pass structure). ----")
        a("struct VkGenRpAttachment {")
        a("    uint32_t flags = 0, format = 0, samples = 0, loadOp = 0, storeOp = 0;")
        a("    uint32_t stencilLoadOp = 0, stencilStoreOp = 0, initialLayout = 0, finalLayout = 0;")
        a("};")
        a("struct VkGenRpRef { uint32_t attachment = 0; uint32_t layout = 0; };")
        a("struct VkGenRpSubpass {")
        a("    uint32_t flags = 0, pipelineBindPoint = 0;")
        a("    std::vector<VkGenRpRef> input, color, resolve;")
        a("    bool has_depth = false; VkGenRpRef depth{};")
        a("    std::vector<uint32_t> preserve;")
        a("};")
        a("struct VkGenRpDependency {")
        a("    uint32_t srcSubpass = 0, dstSubpass = 0, srcStageMask = 0, dstStageMask = 0;")
        a("    uint32_t srcAccessMask = 0, dstAccessMask = 0, dependencyFlags = 0;")
        a("};")
    # Pipeline wire PODs (create_pipelines) — emitted only if a pipeline create is in SPECS.
    if any(op["kind"] == "create_pipelines" for op in ops):
        a("// ---- Pipeline wire PODs (the deep nested create_pipelines structure; WAVE D). The")
        a("// real body rebuilds the typed VkGraphics/ComputePipelineCreateInfo from these,")
        a("// translating the module / layout / renderPass / basePipeline virtual handles via")
        a("// VkGenTables. All-scalar (handles are u32 virtual ids), so no Vulkan headers here.")
        a("struct VkGenPipeSpecEntry { uint32_t constantID = 0, offset = 0, size = 0; };")
        a("struct VkGenPipeStage {")
        a("    uint32_t stage = 0, vmodule = 0;")
        a("    std::string name;                 // entry-point (\"main\" etc.)")
        a("    bool has_spec = false;")
        a("    std::vector<VkGenPipeSpecEntry> spec_entries;")
        a("    std::vector<uint8_t> spec_data;")
        a("};")
        a("struct VkGenPipeVertexBinding { uint32_t binding = 0, stride = 0, inputRate = 0; };")
        a("struct VkGenPipeVertexAttr { uint32_t location = 0, binding = 0, format = 0, offset = 0; };")
        a("struct VkGenPipeViewport { float x = 0, y = 0, w = 0, h = 0, minDepth = 0, maxDepth = 0; };")
        a("struct VkGenPipeScissor { int32_t offX = 0, offY = 0; uint32_t extW = 0, extH = 0; };")
        a("struct VkGenPipeStencilOp {")
        a("    uint32_t failOp = 0, passOp = 0, depthFailOp = 0, compareOp = 0;")
        a("    uint32_t compareMask = 0, writeMask = 0, reference = 0;")
        a("};")
        a("struct VkGenPipeBlendAttachment {")
        a("    uint32_t blendEnable = 0, srcColorBlendFactor = 0, dstColorBlendFactor = 0;")
        a("    uint32_t colorBlendOp = 0, srcAlphaBlendFactor = 0, dstAlphaBlendFactor = 0;")
        a("    uint32_t alphaBlendOp = 0, colorWriteMask = 0;")
        a("};")
        a("struct VkGenPipeline {")
        a("    uint32_t vpipe = 0;  // the guest's virtual id for this pipeline")
        a("    uint32_t flags = 0, vlayout = 0, vrenderpass = 0, subpass = 0, vbase = 0;")
        a("    int32_t base_index = 0;")
        a("    std::vector<VkGenPipeStage> stages;")
        a("    // Fixed-function sub-states (graphics). has_* mirror the optional CreateInfo ptrs.")
        a("    bool has_vertex_input = false;")
        a("    std::vector<VkGenPipeVertexBinding> vbindings;")
        a("    std::vector<VkGenPipeVertexAttr> vattrs;")
        a("    bool has_input_assembly = false; uint32_t topology = 0, primitiveRestartEnable = 0;")
        a("    bool has_tessellation = false; uint32_t patchControlPoints = 0;")
        a("    bool has_viewport = false;")
        a("    std::vector<VkGenPipeViewport> viewports;")
        a("    std::vector<VkGenPipeScissor> scissors;")
        a("    bool has_rasterization = false;")
        a("    uint32_t depthClampEnable = 0, rasterizerDiscardEnable = 0, polygonMode = 0;")
        a("    uint32_t cullMode = 0, frontFace = 0, depthBiasEnable = 0;")
        a("    float depthBiasConstantFactor = 0, depthBiasClamp = 0, depthBiasSlopeFactor = 0;")
        a("    float lineWidth = 1.0f;")
        a("    bool has_multisample = false;")
        a("    uint32_t rasterizationSamples = 1, sampleShadingEnable = 0;")
        a("    float minSampleShading = 0;")
        a("    uint32_t alphaToCoverageEnable = 0, alphaToOneEnable = 0;")
        a("    std::vector<uint32_t> sample_mask;")
        a("    bool has_depth_stencil = false;")
        a("    uint32_t depthTestEnable = 0, depthWriteEnable = 0, depthCompareOp = 0;")
        a("    uint32_t depthBoundsTestEnable = 0, stencilTestEnable = 0;")
        a("    VkGenPipeStencilOp front{}, back{};")
        a("    float minDepthBounds = 0, maxDepthBounds = 0;")
        a("    bool has_color_blend = false;")
        a("    uint32_t logicOpEnable = 0, logicOp = 0;")
        a("    float blendConstants[4] = {0, 0, 0, 0};")
        a("    std::vector<VkGenPipeBlendAttachment> blend_attachments;")
        a("    bool has_dynamic_state = false;")
        a("    std::vector<uint32_t> dynamic_states;")
        a("};")
    return "\n".join(L)


def gen_decode_state_ext():
    L = []
    a = L.append
    a("// Generated handle tables — virtual id -> real Mali handle, persistent across")
    a("// batches (like VkDecodeState's own maps). Held in a side struct keyed by the")
    a("// owning VkDecodeState's address so the generated decode finds them without")
    a("// editing the hand-written struct. One instance per VkDecodeState, via gen_tables(st).")
    a("struct VkGenTables {")
    a("#ifdef ALR_VK_DECODE_REAL")
    a("    std::map<uint32_t, VkCommandPool> pools;    // vpool   -> real")
    a("    std::map<uint32_t, VkBuffer> buffers;       // vbuf    -> real")
    a("    std::map<uint32_t, VkImage> images;         // vimg    -> real")
    a("    std::map<uint32_t, VkImageView> views;      // vview   -> real")
    a("    std::map<uint32_t, VkDeviceMemory> memory;  // vmem    -> real")
    a("    // WAVE A render-resource handle tables (virtual id -> real Mali handle).")
    a("    std::map<uint32_t, VkShaderModule> shader_modules;  // vshmod  -> real")
    a("    std::map<uint32_t, VkPipelineCache> pipeline_caches; // vpcache -> real")
    a("    std::map<uint32_t, VkSampler> samplers;     // vsamp   -> real")
    a("    std::map<uint32_t, VkFence> fences;         // vfence  -> real")
    a("    std::map<uint32_t, VkSemaphore> semaphores; // vsem    -> real")
    a("    std::map<uint32_t, VkEvent> events;         // vevent  -> real")
    a("    std::map<uint32_t, VkQueryPool> query_pools; // vqpool  -> real")
    a("    // WAVE B descriptor/layout handle tables.")
    a("    std::map<uint32_t, VkDescriptorSetLayout> dsl;      // vdsl     -> real")
    a("    std::map<uint32_t, VkPipelineLayout> pipeline_layouts; // vplayout -> real")
    a("    std::map<uint32_t, VkDescriptorPool> descriptor_pools; // vdpool  -> real")
    a("    std::map<uint32_t, VkDescriptorSet> descriptor_sets;   // vdset   -> real")
    a("    // WAVE C render-pass / framebuffer handle tables.")
    a("    std::map<uint32_t, VkRenderPass> render_passes;     // vrpass   -> real")
    a("    std::map<uint32_t, VkFramebuffer> framebuffers;     // vfb      -> real")
    a("    // WAVE D pipeline handle table (graphics + compute share one VkPipeline map).")
    a("    std::map<uint32_t, VkPipeline> pipelines;           // vpipe    -> real")
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
        if op.get("blob_field"):
            # Read the trailing blob (data ptr into the wire buffer + length) BEFORE the
            # pNext chain, matching the encoder's order. Bound it so a bogus length can't
            # make the real driver read an arbitrary blob.
            a("            const uint8_t* blob_data = nullptr; uint32_t blob_len = 0;")
            a("            if (!r.blob(blob_data, blob_len)) { st.ok = false; return true; }")
            a("            if (blob_len > (1u << 24)) { st.ok = false; return true; }  // 16MiB cap")
        a(gen_pnext_read())
        a("            int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a(f"                res = static_cast<int>(vk_gen_real_{op['short']}(")
        a(f"                    st, vdev, vhandle{(', ' + op['ci_call_args']) if op['ci_call_args'] else ''},")
        if op.get("blob_field"):
            a("                    blob_data, blob_len,")
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
    elif k == "create_struct":
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
        # Each array: read u32 count (bounded), then count×{elem fields} into a flat vector
        # of a small POD struct per element. The real body rebuilds the typed array.
        for arr in op["arrays"]:
            an = arr["array_name"]
            a(f"            uint32_t {an}_count = 0;")
            a(f"            if (!r.u32({an}_count)) {{ st.ok = false; return true; }}")
            a(f"            if ({an}_count > 4096) {{ st.ok = false; return true; }}")
            a(f"            std::vector<VkGenElem_{op['short']}_{an}> {an};")
            a(f"            {an}.reserve({an}_count);")
            a(f"            for (uint32_t i = 0; i < {an}_count; ++i) {{")
            a(f"                VkGenElem_{op['short']}_{an} el{{}};")
            er = []
            for fname, wt in arr["fields"]:
                er.append(reader_call(wt, "el." + wire_var(fname)))
            a("                if (!(" + " && ".join(er) + ")) { st.ok = false; return true; }")
            a(f"                {an}.push_back(el);")
            a("            }")
        a(gen_pnext_read())
        a("            int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        call_args = ["st", "vdev", "vhandle"]
        call_args += [wire_var(f) for f, _ in op["ci_wire_fields"]]
        call_args += [arr["array_name"] for arr in op["arrays"]]
        a(f"                res = static_cast<int>(vk_gen_real_{op['short']}(")
        a("                    " + ", ".join(call_args) + ", pnext_types, pnext_bytes));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->create_handle)")
        a(f"                res = gp->create_handle(gp->ctx, {op['op_enum']}, vdev, vhandle, 0, 0);")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vhandle);")
        a("            reply.i32(res);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "alloc_sets":
        a("            uint32_t vdev = 0, vpool = 0, set_count = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vpool) || !r.u32(set_count)) {")
        a("                st.ok = false; return true; }")
        a("            if (set_count > 4096) { st.ok = false; return true; }")
        a("            std::vector<uint32_t> vlayouts, vsets;")
        a("            vlayouts.reserve(set_count); vsets.reserve(set_count);")
        a("            for (uint32_t i = 0; i < set_count; ++i) {")
        a("                uint32_t vl = 0, vs = 0;")
        a("                if (!r.u32(vl) || !r.u32(vs)) { st.ok = false; return true; }")
        a("                vlayouts.push_back(vl); vsets.push_back(vs);")
        a("            }")
        a("            int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a("                res = static_cast<int>(vk_gen_real_allocate_descriptor_sets(")
        a("                    st, vdev, vpool, vlayouts, vsets));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->create_handle)")
        a(f"                res = gp->create_handle(gp->ctx, {op['op_enum']}, vdev,")
        a("                                        set_count ? vsets[0] : 0, set_count, vpool);")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(set_count);")
        a("            reply.i32(res);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "free_sets":
        a("            uint32_t vdev = 0, vpool = 0, set_count = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vpool) || !r.u32(set_count)) {")
        a("                st.ok = false; return true; }")
        a("            if (set_count > 4096) { st.ok = false; return true; }")
        a("            std::vector<uint32_t> vsets;")
        a("            vsets.reserve(set_count);")
        a("            for (uint32_t i = 0; i < set_count; ++i) {")
        a("                uint32_t vs = 0; if (!r.u32(vs)) { st.ok = false; return true; }")
        a("                vsets.push_back(vs);")
        a("            }")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) vk_gen_real_free_descriptor_sets(st, vdev, vpool, vsets);")
        a("#endif")
        a("            if (gp && gp->destroy_handle)")
        a(f"                gp->destroy_handle(gp->ctx, {op['op_enum']}, vdev, set_count);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "update_sets":
        a("            uint32_t vdev = 0, write_count = 0;")
        a("            if (!r.u32(vdev) || !r.u32(write_count)) { st.ok = false; return true; }")
        a("            if (write_count > 4096) { st.ok = false; return true; }")
        a("            std::vector<VkGenDescWrite> writes;")
        a("            writes.reserve(write_count);")
        a("            for (uint32_t i = 0; i < write_count; ++i) {")
        a("                VkGenDescWrite w{};")
        a("                if (!r.u32(w.vdstset) || !r.u32(w.binding) || !r.u32(w.array_element) ||")
        a("                    !r.u32(w.descriptor_type) || !r.u32(w.descriptor_count)) {")
        a("                    st.ok = false; return true; }")
        a("                if (w.descriptor_count > 4096) { st.ok = false; return true; }")
        a("                bool is_image = vk_gen_desc_is_image(w.descriptor_type);")
        a("                for (uint32_t d = 0; d < w.descriptor_count; ++d) {")
        a("                    if (is_image) {")
        a("                        VkGenImageInfo ii{};")
        a("                        if (!r.u32(ii.vsampler) || !r.u32(ii.vimageview) ||")
        a("                            !r.u32(ii.image_layout)) { st.ok = false; return true; }")
        a("                        w.images.push_back(ii);")
        a("                    } else {")
        a("                        VkGenBufferInfo bi{};")
        a("                        if (!r.u32(bi.vbuffer) || !r.u64(bi.offset) || !r.u64(bi.range)) {")
        a("                            st.ok = false; return true; }")
        a("                        w.buffers.push_back(bi);")
        a("                    }")
        a("                }")
        a("                writes.push_back(std::move(w));")
        a("            }")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) vk_gen_real_update_descriptor_sets(st, vdev, writes);")
        a("#endif")
        a("            (void)vdev;")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "create_render_pass":
        a("            uint32_t vdev = 0, vhandle = 0, flags = 0;")
        a("            if (!r.u32(vdev) || !r.u32(vhandle) || !r.u32(flags)) {")
        a("                st.ok = false; return true; }")
        a("            uint32_t att_count = 0;")
        a("            if (!r.u32(att_count) || att_count > 4096) { st.ok = false; return true; }")
        a("            std::vector<VkGenRpAttachment> attachments; attachments.reserve(att_count);")
        a("            for (uint32_t i = 0; i < att_count; ++i) {")
        a("                VkGenRpAttachment a{};")
        a("                if (!r.u32(a.flags) || !r.u32(a.format) || !r.u32(a.samples) ||")
        a("                    !r.u32(a.loadOp) || !r.u32(a.storeOp) || !r.u32(a.stencilLoadOp) ||")
        a("                    !r.u32(a.stencilStoreOp) || !r.u32(a.initialLayout) ||")
        a("                    !r.u32(a.finalLayout)) { st.ok = false; return true; }")
        a("                attachments.push_back(a);")
        a("            }")
        a("            uint32_t sub_count = 0;")
        a("            if (!r.u32(sub_count) || sub_count > 4096) { st.ok = false; return true; }")
        a("            std::vector<VkGenRpSubpass> subpasses; subpasses.reserve(sub_count);")
        a("            for (uint32_t i = 0; i < sub_count; ++i) {")
        a("                VkGenRpSubpass s{};")
        a("                uint32_t inC = 0, colC = 0, resC = 0, hasD = 0, presC = 0;")
        a("                if (!r.u32(s.flags) || !r.u32(s.pipelineBindPoint) || !r.u32(inC) ||")
        a("                    !r.u32(colC) || !r.u32(resC) || !r.u32(hasD) || !r.u32(presC)) {")
        a("                    st.ok = false; return true; }")
        a("                if (inC > 4096 || colC > 4096 || resC > 4096 || presC > 4096) {")
        a("                    st.ok = false; return true; }")
        a("                auto read_refs = [&](std::vector<VkGenRpRef>& out, uint32_t n) -> bool {")
        a("                    out.reserve(n);")
        a("                    for (uint32_t j = 0; j < n; ++j) { VkGenRpRef rf{};")
        a("                        if (!r.u32(rf.attachment) || !r.u32(rf.layout)) return false;")
        a("                        out.push_back(rf); } return true; };")
        a("                if (!read_refs(s.input, inC) || !read_refs(s.color, colC) ||")
        a("                    !read_refs(s.resolve, resC)) { st.ok = false; return true; }")
        a("                s.has_depth = hasD != 0;")
        a("                if (s.has_depth) { if (!r.u32(s.depth.attachment) ||")
        a("                    !r.u32(s.depth.layout)) { st.ok = false; return true; } }")
        a("                s.preserve.reserve(presC);")
        a("                for (uint32_t j = 0; j < presC; ++j) { uint32_t p = 0;")
        a("                    if (!r.u32(p)) { st.ok = false; return true; } s.preserve.push_back(p); }")
        a("                subpasses.push_back(std::move(s));")
        a("            }")
        a("            uint32_t dep_count = 0;")
        a("            if (!r.u32(dep_count) || dep_count > 4096) { st.ok = false; return true; }")
        a("            std::vector<VkGenRpDependency> deps; deps.reserve(dep_count);")
        a("            for (uint32_t i = 0; i < dep_count; ++i) {")
        a("                VkGenRpDependency d{};")
        a("                if (!r.u32(d.srcSubpass) || !r.u32(d.dstSubpass) || !r.u32(d.srcStageMask) ||")
        a("                    !r.u32(d.dstStageMask) || !r.u32(d.srcAccessMask) ||")
        a("                    !r.u32(d.dstAccessMask) || !r.u32(d.dependencyFlags)) {")
        a("                    st.ok = false; return true; }")
        a("                deps.push_back(d);")
        a("            }")
        a(gen_pnext_read())
        a("            int res = -1;")
        a("#ifdef ALR_VK_DECODE_REAL")
        a("            if (!gp) {")
        a("                res = static_cast<int>(vk_gen_real_create_render_pass(")
        a("                    st, vdev, vhandle, flags, attachments, subpasses, deps,")
        a("                    pnext_types, pnext_bytes));")
        a("            }")
        a("#endif")
        a("            if (gp && gp->create_handle)")
        a(f"                res = gp->create_handle(gp->ctx, {op['op_enum']}, vdev, vhandle, 0, 0);")
        a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
        a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
        a("            reply.u32(vhandle);")
        a("            reply.i32(res);")
        a("            st.decoded++;")
        a("            return true;")
    elif k == "create_pipelines":
        a(gen_decode_case_pipelines(op))
    a("        }")
    return "\n".join(L)


# Host-side decode for create_pipelines: read the full nested wire structure (the encoder
# above wrote it) into a std::vector<VkGenPipeline> and (under ALR_VK_DECODE_REAL, no
# provider) call the hand-written real body, which rebuilds the typed CreateInfos, translates
# handles via VkGenTables, and calls real Mali. Every count is bounded so a malformed wire can
# never make the host over-read or over-allocate. Reply: { u32 pipeline_count, i32 result }.
PIPE_BOUND = 4096          # max pipelines / stages / array elements per field
PIPE_NAME_CAP = 4096       # max entry-point name bytes
PIPE_SPEC_CAP = (1 << 20)  # max specialization data bytes (1 MiB)


def gen_decode_case_pipelines(op):
    is_graphics = op["pipeline_kind"] == "graphics"
    L = []
    a = L.append
    a("            uint32_t vdev = 0, vpcache = 0, pipeline_count = 0;")
    a("            if (!r.u32(vdev) || !r.u32(vpcache) || !r.u32(pipeline_count)) {")
    a("                st.ok = false; return true; }")
    a(f"            if (pipeline_count > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("            std::vector<VkGenPipeline> pipes; pipes.reserve(pipeline_count);")
    a("            for (uint32_t pi = 0; pi < pipeline_count; ++pi) {")
    a("                VkGenPipeline p{};")
    a("                uint32_t stage_count = 0;")
    a("                if (!r.u32(p.vpipe) || !r.u32(p.flags) || !r.u32(p.vlayout) ||")
    a("                    !r.u32(p.vrenderpass) || !r.u32(p.subpass) || !r.u32(p.vbase) ||")
    a("                    !r.i32(p.base_index) || !r.u32(stage_count)) { st.ok = false; return true; }")
    a(f"                if (stage_count > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("                p.stages.reserve(stage_count);")
    a("                for (uint32_t si = 0; si < stage_count; ++si) {")
    a("                    VkGenPipeStage s{};")
    a("                    const uint8_t* nm = nullptr; uint32_t nlen = 0; uint32_t spec_present = 0;")
    a("                    if (!r.u32(s.stage) || !r.u32(s.vmodule) || !r.blob(nm, nlen) ||")
    a("                        !r.u32(spec_present)) { st.ok = false; return true; }")
    a(f"                    if (nlen > {PIPE_NAME_CAP}) {{ st.ok = false; return true; }}")
    a("                    s.name.assign(reinterpret_cast<const char*>(nm), nlen);")
    a("                    if (spec_present) {")
    a("                        s.has_spec = true;")
    a("                        uint32_t me_count = 0, data_len = 0;")
    a("                        if (!r.u32(me_count) || !r.u32(data_len)) { st.ok = false; return true; }")
    a(f"                        if (me_count > {PIPE_BOUND} || data_len > {PIPE_SPEC_CAP}) {{")
    a("                            st.ok = false; return true; }")
    a("                        s.spec_entries.reserve(me_count);")
    a("                        for (uint32_t mi = 0; mi < me_count; ++mi) {")
    a("                            VkGenPipeSpecEntry me{};")
    a("                            if (!r.u32(me.constantID) || !r.u32(me.offset) || !r.u32(me.size)) {")
    a("                                st.ok = false; return true; }")
    a("                            s.spec_entries.push_back(me);")
    a("                        }")
    a("                        const uint8_t* sd = nullptr; uint32_t sdl = 0;")
    a("                        if (!r.blob(sd, sdl) || sdl != data_len) { st.ok = false; return true; }")
    a("                        s.spec_data.assign(sd, sd + sdl);")
    a("                    }")
    a("                    p.stages.push_back(std::move(s));")
    a("                }")
    if is_graphics:
        a(gen_decode_pipe_fixed_function())
    a("                pipes.push_back(std::move(p));")
    a("            }")
    a("            (void)vpcache;")
    a("            int res = -1;")
    a("#ifdef ALR_VK_DECODE_REAL")
    a("            if (!gp) {")
    a(f"                res = static_cast<int>(vk_gen_real_{op['short']}(st, vdev, vpcache, pipes));")
    a("            }")
    a("#endif")
    a("            if (gp && gp->create_handle)")
    a(f"                res = gp->create_handle(gp->ctx, {op['op_enum']}, vdev,")
    a("                                        pipeline_count ? pipes[0].flags : 0,")
    a("                                        pipeline_count, vpcache);")
    a("            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));")
    a(f"            reply.u16(static_cast<uint16_t>({op['reply_enum']}));")
    a("            reply.u32(pipeline_count);")
    a("            reply.i32(res);")
    a("            st.decoded++;")
    a("            return true;")
    return "\n".join(L)


def gen_decode_pipe_fixed_function():
    """The graphics fixed-function sub-state reads (each behind a presence flag), in the SAME
    order the encoder wrote them. Bounded the same way as the other array reads."""
    L = []
    a = L.append
    a("                // ---- fixed-function sub-states (graphics), each behind a presence flag ----")
    a("                uint32_t present = 0;")
    a("                // vertex input")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_vertex_input = true;")
    a("                    uint32_t bc = 0, ac = 0;")
    a("                    if (!r.u32(bc) || !r.u32(ac)) { st.ok = false; return true; }")
    a(f"                    if (bc > {PIPE_BOUND} || ac > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("                    p.vbindings.reserve(bc);")
    a("                    for (uint32_t i = 0; i < bc; ++i) { VkGenPipeVertexBinding b{};")
    a("                        if (!r.u32(b.binding) || !r.u32(b.stride) || !r.u32(b.inputRate)) {")
    a("                            st.ok = false; return true; } p.vbindings.push_back(b); }")
    a("                    p.vattrs.reserve(ac);")
    a("                    for (uint32_t i = 0; i < ac; ++i) { VkGenPipeVertexAttr at{};")
    a("                        if (!r.u32(at.location) || !r.u32(at.binding) || !r.u32(at.format) ||")
    a("                            !r.u32(at.offset)) { st.ok = false; return true; } p.vattrs.push_back(at); }")
    a("                }")
    a("                // input assembly")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_input_assembly = true;")
    a("                    if (!r.u32(p.topology) || !r.u32(p.primitiveRestartEnable)) {")
    a("                        st.ok = false; return true; } }")
    a("                // tessellation")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_tessellation = true;")
    a("                    if (!r.u32(p.patchControlPoints)) { st.ok = false; return true; } }")
    a("                // viewport")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_viewport = true;")
    a("                    uint32_t vc = 0, sc = 0;")
    a("                    if (!r.u32(vc) || !r.u32(sc)) { st.ok = false; return true; }")
    a(f"                    if (vc > {PIPE_BOUND} || sc > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("                    p.viewports.reserve(vc);")
    a("                    for (uint32_t i = 0; i < vc; ++i) { VkGenPipeViewport v{};")
    a("                        if (!r.f32(v.x) || !r.f32(v.y) || !r.f32(v.w) || !r.f32(v.h) ||")
    a("                            !r.f32(v.minDepth) || !r.f32(v.maxDepth)) { st.ok = false; return true; }")
    a("                        p.viewports.push_back(v); }")
    a("                    p.scissors.reserve(sc);")
    a("                    for (uint32_t i = 0; i < sc; ++i) { VkGenPipeScissor s{};")
    a("                        if (!r.i32(s.offX) || !r.i32(s.offY) || !r.u32(s.extW) || !r.u32(s.extH)) {")
    a("                            st.ok = false; return true; } p.scissors.push_back(s); }")
    a("                }")
    a("                // rasterization")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_rasterization = true;")
    a("                    if (!r.u32(p.depthClampEnable) || !r.u32(p.rasterizerDiscardEnable) ||")
    a("                        !r.u32(p.polygonMode) || !r.u32(p.cullMode) || !r.u32(p.frontFace) ||")
    a("                        !r.u32(p.depthBiasEnable) || !r.f32(p.depthBiasConstantFactor) ||")
    a("                        !r.f32(p.depthBiasClamp) || !r.f32(p.depthBiasSlopeFactor) ||")
    a("                        !r.f32(p.lineWidth)) { st.ok = false; return true; } }")
    a("                // multisample")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_multisample = true;")
    a("                    uint32_t mask_words = 0;")
    a("                    if (!r.u32(p.rasterizationSamples) || !r.u32(p.sampleShadingEnable) ||")
    a("                        !r.f32(p.minSampleShading) || !r.u32(mask_words) ||")
    a("                        !r.u32(p.alphaToCoverageEnable) || !r.u32(p.alphaToOneEnable)) {")
    a("                        st.ok = false; return true; }")
    a(f"                    if (mask_words > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("                    p.sample_mask.reserve(mask_words);")
    a("                    for (uint32_t i = 0; i < mask_words; ++i) { uint32_t w = 0;")
    a("                        if (!r.u32(w)) { st.ok = false; return true; } p.sample_mask.push_back(w); } }")
    a("                // depth stencil")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_depth_stencil = true;")
    a("                    if (!r.u32(p.depthTestEnable) || !r.u32(p.depthWriteEnable) ||")
    a("                        !r.u32(p.depthCompareOp) || !r.u32(p.depthBoundsTestEnable) ||")
    a("                        !r.u32(p.stencilTestEnable) || !r.f32(p.minDepthBounds) ||")
    a("                        !r.f32(p.maxDepthBounds)) { st.ok = false; return true; }")
    a("                    auto rd_stencil = [&](VkGenPipeStencilOp& so) -> bool {")
    a("                        return r.u32(so.failOp) && r.u32(so.passOp) && r.u32(so.depthFailOp) &&")
    a("                               r.u32(so.compareOp) && r.u32(so.compareMask) &&")
    a("                               r.u32(so.writeMask) && r.u32(so.reference); };")
    a("                    if (!rd_stencil(p.front) || !rd_stencil(p.back)) { st.ok = false; return true; } }")
    a("                // color blend")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_color_blend = true;")
    a("                    uint32_t att = 0;")
    a("                    if (!r.u32(p.logicOpEnable) || !r.u32(p.logicOp) || !r.u32(att) ||")
    a("                        !r.f32(p.blendConstants[0]) || !r.f32(p.blendConstants[1]) ||")
    a("                        !r.f32(p.blendConstants[2]) || !r.f32(p.blendConstants[3])) {")
    a("                        st.ok = false; return true; }")
    a(f"                    if (att > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("                    p.blend_attachments.reserve(att);")
    a("                    for (uint32_t i = 0; i < att; ++i) { VkGenPipeBlendAttachment ba{};")
    a("                        if (!r.u32(ba.blendEnable) || !r.u32(ba.srcColorBlendFactor) ||")
    a("                            !r.u32(ba.dstColorBlendFactor) || !r.u32(ba.colorBlendOp) ||")
    a("                            !r.u32(ba.srcAlphaBlendFactor) || !r.u32(ba.dstAlphaBlendFactor) ||")
    a("                            !r.u32(ba.alphaBlendOp) || !r.u32(ba.colorWriteMask)) {")
    a("                            st.ok = false; return true; } p.blend_attachments.push_back(ba); } }")
    a("                // dynamic state")
    a("                if (!r.u32(present)) { st.ok = false; return true; }")
    a("                if (present) { p.has_dynamic_state = true;")
    a("                    uint32_t dc = 0;")
    a("                    if (!r.u32(dc)) { st.ok = false; return true; }")
    a(f"                    if (dc > {PIPE_BOUND}) {{ st.ok = false; return true; }}")
    a("                    p.dynamic_states.reserve(dc);")
    a("                    for (uint32_t i = 0; i < dc; ++i) { uint32_t d = 0;")
    a("                        if (!r.u32(d)) { st.ok = false; return true; } p.dynamic_states.push_back(d); } }")
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
        blob = op.get("blob_field")
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, const {op['ci']} *pCreateInfo,")
        a(f"                          const VkAllocationCallbacks *pAllocator, {out_ty} *pHandle) {{")
        a("    (void)pAllocator;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t vid; int32_t res = 0;")
        a("    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;")
        a(f"    vid = alr_alloc(&{op['counter']}, 1);")
        a("    if (alr_icd_ring_ok()) {")
        if blob:
            blen, bdata = blob  # the C members: byte-length and data pointer
            # A blob create (e.g. SPIR-V) is unbounded, so the request rides a heap buffer
            # sized to the blob + a fixed header slack (scalars + pNext-count + END). The
            # roundtrip-on-heap path mirrors the stack path but frees afterward.
            a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
            a(f"        size_t blob_len = (size_t)pCreateInfo->{blen};")
            a(f"        const void *blob_data = (const void *)pCreateInfo->{bdata};")
            a("        size_t cap = blob_len + 256;  // header slack")
            a("        uint8_t *req = (uint8_t *)malloc(cap);")
            a("        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;")
            a("        alr_vk_enc_init(&e, req, cap);")
            a(f"        {op['enc_name']}_begin(&e, dev->vdev, vid{op['icd_ci_args']}, blob_data, (uint32_t)blob_len);")
            a("        alr_vk_gen_pnext_count(&e, 0);  // pNext forwarding deferred for this create")
            a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
            a("        if (!e.overflow) {")
            a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
            a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
            a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
            a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
            a("        free(req);")
        else:
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
    elif k == "create_struct":
        out_ty = op["out"]
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, const {op['ci']} *pCreateInfo,")
        a(f"                          const VkAllocationCallbacks *pAllocator, {out_ty} *pHandle) {{")
        a("    (void)pAllocator;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device;")
        a("    uint32_t vid; int32_t res = 0; uint32_t ai;")
        a("    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;")
        a(f"    vid = alr_alloc(&{op['counter']}, 1);")
        a("    if (alr_icd_ring_ok()) {")
        # Heap request buffer sized generously for the arrays (each element <= 16 bytes; the
        # counts are bounded by the host decode at 4096, but we cap our own loop too).
        a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        size_t cap = 256;")
        for arr in op["arrays"]:
            a(f"        cap += (size_t)pCreateInfo->{arr['count_field']} * 24;")
        a("        uint8_t *req = (uint8_t *)malloc(cap);")
        a("        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;")
        a("        alr_vk_enc_init(&e, req, cap);")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, vid{op['icd_ci_args']});")
        for arr in op["arrays"]:
            an = arr["array_name"]
            cf = arr["count_field"]
            pf = arr["ptr_field"]
            a(f"        {{ uint32_t n = pCreateInfo->{cf}; if (n > 4096) n = 4096;")
            a(f"          {op['enc_name']}_{an}_count(&e, n);")
            a(f"          for (ai = 0; ai < n; ++ai) {{")
            # Build the per-element encode args from the real array element.
            elem_args = []
            for fname, wt in arr["fields"]:
                if fname.startswith("@self"):
                    # The element IS a handle (e.g. pSetLayouts[ai] is a VkDescriptorSetLayout);
                    # ship its virtual id (the handle carries it in its low bits).
                    elem_args.append(f"(uint32_t)(uintptr_t)pCreateInfo->{pf}[ai]")
                else:
                    elem_args.append(f"(uint32_t)pCreateInfo->{pf}[ai].{fname}")
            a(f"              {op['enc_name']}_{an}_elem(&e, {', '.join(elem_args)}); }} }}")
        a("        alr_vk_gen_pnext_count(&e, 0);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
        a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        free(req);")
        a("    }")
        a("    if (res != 0) return (VkResult)res;")
        a(f"    *pHandle = ({out_ty})(uintptr_t)vid;")
        a("    return VK_SUCCESS;")
        a("}")
    elif k == "alloc_sets":
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device,")
        a("                          const VkDescriptorSetAllocateInfo *pAllocateInfo,")
        a("                          VkDescriptorSet *pDescriptorSets) {")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i; int32_t res = 0;")
        a("    if (!dev || !pAllocateInfo || !pDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;")
        a("    uint32_t n = pAllocateInfo->descriptorSetCount; if (n > 4096) n = 4096;")
        a("    /* Pre-assign a virtual id per set (client-side, monotonic) and write them out;")
        a("     * the host maps each to a real descriptor set allocated from the real pool. */")
        a("    for (i = 0; i < n; ++i)")
        a(f"        pDescriptorSets[i] = (VkDescriptorSet)(uintptr_t)alr_alloc(&{op['counter']}, 1);")
        a("    if (alr_icd_ring_ok()) {")
        a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        size_t cap = 64 + (size_t)n * 8; uint8_t *req = (uint8_t *)malloc(cap);")
        a("        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;")
        a("        alr_vk_enc_init(&e, req, cap);")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev,")
        a("                          (uint32_t)(uintptr_t)pAllocateInfo->descriptorPool, n);")
        a("        for (i = 0; i < n; ++i)")
        a(f"            {op['enc_name']}_set(&e, (uint32_t)(uintptr_t)pAllocateInfo->pSetLayouts[i],")
        a("                              (uint32_t)(uintptr_t)pDescriptorSets[i]);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
        a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        free(req);")
        a("    }")
        a("    return (VkResult)res;")
        a("}")
    elif k == "free_sets":
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, VkDescriptorPool descriptorPool,")
        a("                          uint32_t descriptorSetCount,")
        a("                          const VkDescriptorSet *pDescriptorSets) {")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i;")
        a("    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;")
        a("    if (!alr_icd_ring_ok() || descriptorSetCount == 0 || !pDescriptorSets) return VK_SUCCESS;")
        a("    {")
        a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        uint32_t n = descriptorSetCount > 4096 ? 4096 : descriptorSetCount;")
        a("        size_t cap = 32 + (size_t)n * 4; uint8_t *req = (uint8_t *)malloc(cap);")
        a("        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;")
        a("        alr_vk_enc_init(&e, req, cap);")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, (uint32_t)(uintptr_t)descriptorPool, n);")
        a("        for (i = 0; i < n; ++i)")
        a(f"            {op['enc_name']}_set(&e, (uint32_t)(uintptr_t)pDescriptorSets[i]);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("        free(req);")
        a("    }")
        a("    return VK_SUCCESS;")
        a("}")
    elif k == "update_sets":
        a(f"static void VKAPI_CALL {fn}(VkDevice device, uint32_t descriptorWriteCount,")
        a("                          const VkWriteDescriptorSet *pDescriptorWrites,")
        a("                          uint32_t descriptorCopyCount,")
        a("                          const VkCopyDescriptorSet *pDescriptorCopies) {")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i, d;")
        a("    (void)descriptorCopyCount; (void)pDescriptorCopies;  /* copies deferred */")
        a("    if (!dev || !alr_icd_ring_ok() || descriptorWriteCount == 0 || !pDescriptorWrites) return;")
        a("    {")
        a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        a("        uint32_t wn = descriptorWriteCount > 4096 ? 4096 : descriptorWriteCount;")
        a("        size_t cap = 64; for (i = 0; i < wn; ++i) cap += 24 + (size_t)pDescriptorWrites[i].descriptorCount * 24;")
        a("        uint8_t *req = (uint8_t *)malloc(cap);")
        a("        if (!req) return;")
        a("        alr_vk_enc_init(&e, req, cap);")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, wn);")
        a("        for (i = 0; i < wn; ++i) {")
        a("            const VkWriteDescriptorSet *w = &pDescriptorWrites[i];")
        a("            uint32_t dc = w->descriptorCount > 4096 ? 4096 : w->descriptorCount;")
        a(f"            {op['enc_name']}_write(&e, (uint32_t)(uintptr_t)w->dstSet, w->dstBinding,")
        a("                              w->dstArrayElement, (uint32_t)w->descriptorType, dc);")
        a("            int is_image = alr_vk_desc_type_is_image((uint32_t)w->descriptorType);")
        a("            for (d = 0; d < dc; ++d) {")
        a("                if (is_image) {")
        a("                    uint32_t vsamp = w->pImageInfo ? (uint32_t)(uintptr_t)w->pImageInfo[d].sampler : 0;")
        a("                    uint32_t vview = w->pImageInfo ? (uint32_t)(uintptr_t)w->pImageInfo[d].imageView : 0;")
        a("                    uint32_t lay   = w->pImageInfo ? (uint32_t)w->pImageInfo[d].imageLayout : 0;")
        a(f"                    {op['enc_name']}_image_info(&e, vsamp, vview, lay);")
        a("                } else {")
        a("                    uint32_t vbuf = w->pBufferInfo ? (uint32_t)(uintptr_t)w->pBufferInfo[d].buffer : 0;")
        a("                    uint64_t off  = w->pBufferInfo ? (uint64_t)w->pBufferInfo[d].offset : 0;")
        a("                    uint64_t rng  = w->pBufferInfo ? (uint64_t)w->pBufferInfo[d].range : 0;")
        a(f"                    {op['enc_name']}_buffer_info(&e, vbuf, off, rng);")
        a("                }")
        a("            }")
        a("        }")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a("        free(req);")
        a("    }")
        a("}")
    elif k == "create_render_pass":
        out_ty = op["out"]
        a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,")
        a(f"                          const VkAllocationCallbacks *pAllocator, {out_ty} *pHandle) {{")
        a("    (void)pAllocator;")
        a("    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t vid; int32_t res = 0; uint32_t i, j;")
        a("    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;")
        a(f"    vid = alr_alloc(&{op['counter']}, 1);")
        a("    if (alr_icd_ring_ok()) {")
        a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
        # Generous size estimate: attachments*36 + subpasses*(28 + refs*8) + deps*28 + slack.
        a("        size_t cap = 512;")
        a("        cap += (size_t)pCreateInfo->attachmentCount * 40;")
        a("        cap += (size_t)pCreateInfo->dependencyCount * 32;")
        a("        for (i = 0; i < pCreateInfo->subpassCount; ++i) {")
        a("            const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[i];")
        a("            cap += 40 + (size_t)(sp->inputAttachmentCount + sp->colorAttachmentCount) * 8")
        a("                 + (size_t)(sp->colorAttachmentCount + sp->preserveAttachmentCount) * 8;")
        a("        }")
        a("        uint8_t *req = (uint8_t *)malloc(cap);")
        a("        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;")
        a("        alr_vk_enc_init(&e, req, cap);")
        a(f"        {op['enc_name']}_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);")
        a("        /* attachments */")
        a(f"        {op['enc_name']}_attachment_count(&e, pCreateInfo->attachmentCount);")
        a("        for (i = 0; i < pCreateInfo->attachmentCount; ++i) {")
        a("            const VkAttachmentDescription *at = &pCreateInfo->pAttachments[i];")
        a(f"            {op['enc_name']}_attachment(&e, (uint32_t)at->flags, (uint32_t)at->format,")
        a("                              (uint32_t)at->samples, (uint32_t)at->loadOp, (uint32_t)at->storeOp,")
        a("                              (uint32_t)at->stencilLoadOp, (uint32_t)at->stencilStoreOp,")
        a("                              (uint32_t)at->initialLayout, (uint32_t)at->finalLayout);")
        a("        }")
        a("        /* subpasses (with nested reference arrays) */")
        a(f"        {op['enc_name']}_subpass_count(&e, pCreateInfo->subpassCount);")
        a("        for (i = 0; i < pCreateInfo->subpassCount; ++i) {")
        a("            const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[i];")
        a("            uint32_t hasDepth = sp->pDepthStencilAttachment ? 1u : 0u;")
        a("            uint32_t resCount = sp->pResolveAttachments ? sp->colorAttachmentCount : 0u;")
        a(f"            {op['enc_name']}_subpass_begin(&e, (uint32_t)sp->flags,")
        a("                              (uint32_t)sp->pipelineBindPoint, sp->inputAttachmentCount,")
        a("                              sp->colorAttachmentCount, resCount, hasDepth,")
        a("                              sp->preserveAttachmentCount);")
        a("            for (j = 0; j < sp->inputAttachmentCount; ++j)")
        a(f"                {op['enc_name']}_ref(&e, sp->pInputAttachments[j].attachment, (uint32_t)sp->pInputAttachments[j].layout);")
        a("            for (j = 0; j < sp->colorAttachmentCount; ++j)")
        a(f"                {op['enc_name']}_ref(&e, sp->pColorAttachments[j].attachment, (uint32_t)sp->pColorAttachments[j].layout);")
        a("            for (j = 0; j < resCount; ++j)")
        a(f"                {op['enc_name']}_ref(&e, sp->pResolveAttachments[j].attachment, (uint32_t)sp->pResolveAttachments[j].layout);")
        a("            if (hasDepth)")
        a(f"                {op['enc_name']}_ref(&e, sp->pDepthStencilAttachment->attachment, (uint32_t)sp->pDepthStencilAttachment->layout);")
        a("            for (j = 0; j < sp->preserveAttachmentCount; ++j)")
        a(f"                {op['enc_name']}_preserve(&e, sp->pPreserveAttachments[j]);")
        a("        }")
        a("        /* dependencies */")
        a(f"        {op['enc_name']}_dependency_count(&e, pCreateInfo->dependencyCount);")
        a("        for (i = 0; i < pCreateInfo->dependencyCount; ++i) {")
        a("            const VkSubpassDependency *dp = &pCreateInfo->pDependencies[i];")
        a(f"            {op['enc_name']}_dependency(&e, dp->srcSubpass, dp->dstSubpass,")
        a("                              (uint32_t)dp->srcStageMask, (uint32_t)dp->dstStageMask,")
        a("                              (uint32_t)dp->srcAccessMask, (uint32_t)dp->dstAccessMask,")
        a("                              (uint32_t)dp->dependencyFlags);")
        a("        }")
        a("        alr_vk_gen_pnext_count(&e, 0);")
        a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
        a("        if (!e.overflow) {")
        a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
        a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
        a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
        a("        free(req);")
        a("    }")
        a("    if (res != 0) return (VkResult)res;")
        a(f"    *pHandle = ({out_ty})(uintptr_t)vid;")
        a("    return VK_SUCCESS;")
        a("}")
    elif k == "create_pipelines":
        a(gen_icd_fn_pipelines(op))
    return "\n".join(L)


# ICD function for create_pipelines: walk the real VkGraphics/ComputePipelineCreateInfo array,
# pre-assign a virtual pipeline id per pipeline (client-side, monotonic; written into
# pPipelines), drive the generated encoder family, round-trip, and decode the { count, result }
# reply. The encoder mirrors the wire field order the host decode reads. Pure C (the guest ICD
# is C), so it walks the C structs with the real Vulkan types from <vulkan/vulkan.h>.
def gen_icd_fn_pipelines(op):
    fn = op["icd_fn"]
    enc = op["enc_name"]
    is_graphics = op["pipeline_kind"] == "graphics"
    ci_ty = "VkGraphicsPipelineCreateInfo" if is_graphics else "VkComputePipelineCreateInfo"
    L = []
    a = L.append
    a(f"static VkResult VKAPI_CALL {fn}(VkDevice device, VkPipelineCache pipelineCache,")
    a(f"                          uint32_t createInfoCount, const {ci_ty} *pCreateInfos,")
    a("                          const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {")
    a("    (void)pAllocator;")
    a("    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i; int32_t res = 0;")
    a("    if (!dev || !pCreateInfos || !pPipelines || createInfoCount == 0)")
    a("        return VK_ERROR_INITIALIZATION_FAILED;")
    a("    uint32_t n = createInfoCount > 4096 ? 4096 : createInfoCount;")
    a("    /* Pre-assign a virtual id per pipeline (the host maps each to a real Mali pipeline). */")
    a("    for (i = 0; i < n; ++i)")
    a(f"        pPipelines[i] = (VkPipeline)(uintptr_t)alr_alloc(&{op['counter']}, 1);")
    a("    if (alr_icd_ring_ok()) {")
    a("        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];")
    # Size estimate: per pipeline a header + per stage (name + spec) + the fixed-function
    # state. The fixed-function state is bounded; we size generously and bail on overflow.
    a("        size_t cap = 256;")
    a("        for (i = 0; i < n; ++i) {")
    a("            uint32_t s;")
    a("            cap += 64;")
    a(f"            for (s = 0; s < {('pCreateInfos[i].stageCount' if is_graphics else '1u')}; ++s) {{")
    if is_graphics:
        a("                const VkPipelineShaderStageCreateInfo *st = &pCreateInfos[i].pStages[s];")
    else:
        a("                const VkPipelineShaderStageCreateInfo *st = &pCreateInfos[i].stage;")
    a("                cap += 64 + (st->pName ? strlen(st->pName) : 0);")
    a("                if (st->pSpecializationInfo) cap += 32 +")
    a("                    (size_t)st->pSpecializationInfo->mapEntryCount * 12 +")
    a("                    st->pSpecializationInfo->dataSize;")
    a("            }")
    if is_graphics:
        a("            const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];")
        a("            if (ci->pVertexInputState) cap += 16 +")
        a("                (size_t)(ci->pVertexInputState->vertexBindingDescriptionCount +")
        a("                         ci->pVertexInputState->vertexAttributeDescriptionCount) * 16;")
        a("            if (ci->pViewportState) cap += 16 +")
        a("                (size_t)(ci->pViewportState->viewportCount * 24 +")
        a("                         ci->pViewportState->scissorCount * 16);")
        a("            if (ci->pColorBlendState) cap += 48 +")
        a("                (size_t)ci->pColorBlendState->attachmentCount * 32;")
        a("            if (ci->pDynamicState) cap += 8 +")
        a("                (size_t)ci->pDynamicState->dynamicStateCount * 4;")
        a("            cap += 256;  /* the fixed-size sub-states (raster/multisample/depth/etc.) */")
    a("        }")
    a("        uint8_t *req = (uint8_t *)malloc(cap);")
    a("        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;")
    a("        alr_vk_enc_init(&e, req, cap);")
    a(f"        {enc}_begin(&e, dev->vdev, (uint32_t)(uintptr_t)pipelineCache, n);")
    a("        for (i = 0; i < n; ++i) {")
    a(f"            const {ci_ty} *ci = &pCreateInfos[i];")
    a("            uint32_t vpipe = (uint32_t)(uintptr_t)pPipelines[i];  /* the pre-assigned vid */")
    if is_graphics:
        a("            uint32_t stage_count = ci->stageCount;")
        a("            uint32_t vbase = (uint32_t)(uintptr_t)ci->basePipelineHandle;")
        a(f"            {enc}_pipeline(&e, vpipe, (uint32_t)ci->flags,")
        a("                          (uint32_t)(uintptr_t)ci->layout,")
        a("                          (uint32_t)(uintptr_t)ci->renderPass, ci->subpass, vbase,")
        a("                          ci->basePipelineIndex, stage_count);")
    else:
        a("            uint32_t stage_count = 1;")
        a("            uint32_t vbase = (uint32_t)(uintptr_t)ci->basePipelineHandle;")
        a(f"            {enc}_pipeline(&e, vpipe, (uint32_t)ci->flags,")
        a("                          (uint32_t)(uintptr_t)ci->layout, 0u, 0u, vbase,")
        a("                          ci->basePipelineIndex, stage_count);")
    a("            uint32_t s;")
    a("            for (s = 0; s < stage_count; ++s) {")
    if is_graphics:
        a("                const VkPipelineShaderStageCreateInfo *ss = &ci->pStages[s];")
    else:
        a("                const VkPipelineShaderStageCreateInfo *ss = &ci->stage;")
    a("                const char *nm = ss->pName ? ss->pName : \"main\";")
    a("                uint32_t spec_present = ss->pSpecializationInfo ? 1u : 0u;")
    a(f"                {enc}_stage(&e, (uint32_t)ss->stage, (uint32_t)(uintptr_t)ss->module,")
    a("                          nm, (uint32_t)strlen(nm), spec_present);")
    a("                if (spec_present) {")
    a("                    const VkSpecializationInfo *spi = ss->pSpecializationInfo;")
    a("                    uint32_t me = spi->mapEntryCount, mi;")
    a(f"                    {enc}_stage_spec_begin(&e, me, (uint32_t)spi->dataSize);")
    a("                    for (mi = 0; mi < me; ++mi)")
    a(f"                        {enc}_stage_spec_entry(&e, spi->pMapEntries[mi].constantID,")
    a("                                          spi->pMapEntries[mi].offset,")
    a("                                          (uint32_t)spi->pMapEntries[mi].size);")
    a(f"                    {enc}_stage_spec_data(&e, spi->pData, (uint32_t)spi->dataSize);")
    a("                }")
    a("            }")
    if is_graphics:
        a(gen_icd_pipe_fixed_function(enc))
    a("        }")
    a("        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);")
    a("        if (!e.overflow) {")
    a("            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));")
    a(f"            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t){op['reply_enum']}, &res);")
    a("            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
    a("        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;")
    a("        free(req);")
    a("    }")
    a("    return (VkResult)res;")
    a("}")
    return "\n".join(L)


def gen_icd_pipe_fixed_function(enc):
    """The ICD side that walks the real graphics fixed-function sub-state pointers and drives
    the encoder, in the SAME order the host decode reads. `ci` is the VkGraphicsPipelineCreateInfo."""
    L = []
    a = L.append
    a("            /* ---- fixed-function sub-states (graphics) ---- */")
    a("            uint32_t k;")
    a("            /* vertex input */")
    a("            if (ci->pVertexInputState) {")
    a("                const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;")
    a(f"                {enc}_vertex_input(&e, 1u, vi->vertexBindingDescriptionCount,")
    a("                              vi->vertexAttributeDescriptionCount);")
    a("                for (k = 0; k < vi->vertexBindingDescriptionCount; ++k)")
    a(f"                    {enc}_vertex_binding(&e, vi->pVertexBindingDescriptions[k].binding,")
    a("                              vi->pVertexBindingDescriptions[k].stride,")
    a("                              (uint32_t)vi->pVertexBindingDescriptions[k].inputRate);")
    a("                for (k = 0; k < vi->vertexAttributeDescriptionCount; ++k)")
    a(f"                    {enc}_vertex_attr(&e, vi->pVertexAttributeDescriptions[k].location,")
    a("                              vi->pVertexAttributeDescriptions[k].binding,")
    a("                              (uint32_t)vi->pVertexAttributeDescriptions[k].format,")
    a("                              vi->pVertexAttributeDescriptions[k].offset);")
    a(f"            }} else {{ {enc}_vertex_input(&e, 0u, 0u, 0u); }}")
    a("            /* input assembly */")
    a("            if (ci->pInputAssemblyState)")
    a(f"                {enc}_input_assembly(&e, 1u, (uint32_t)ci->pInputAssemblyState->topology,")
    a("                              ci->pInputAssemblyState->primitiveRestartEnable);")
    a(f"            else {enc}_input_assembly(&e, 0u, 0u, 0u);")
    a("            /* tessellation */")
    a("            if (ci->pTessellationState)")
    a(f"                {enc}_tessellation(&e, 1u, ci->pTessellationState->patchControlPoints);")
    a(f"            else {enc}_tessellation(&e, 0u, 0u);")
    a("            /* viewport */")
    a("            if (ci->pViewportState) {")
    a("                const VkPipelineViewportStateCreateInfo *vp = ci->pViewportState;")
    a(f"                {enc}_viewport(&e, 1u, vp->viewportCount, vp->scissorCount);")
    a("                /* pViewports/pScissors may be null when the viewport/scissor is dynamic. */")
    a("                for (k = 0; k < vp->viewportCount; ++k) {")
    a("                    if (vp->pViewports)")
    a(f"                        {enc}_viewport_elem(&e, vp->pViewports[k].x, vp->pViewports[k].y,")
    a("                              vp->pViewports[k].width, vp->pViewports[k].height,")
    a("                              vp->pViewports[k].minDepth, vp->pViewports[k].maxDepth);")
    a(f"                    else {enc}_viewport_elem(&e, 0, 0, 0, 0, 0, 0);")
    a("                }")
    a("                for (k = 0; k < vp->scissorCount; ++k) {")
    a("                    if (vp->pScissors)")
    a(f"                        {enc}_scissor_elem(&e, vp->pScissors[k].offset.x, vp->pScissors[k].offset.y,")
    a("                              vp->pScissors[k].extent.width, vp->pScissors[k].extent.height);")
    a(f"                    else {enc}_scissor_elem(&e, 0, 0, 0, 0);")
    a("                }")
    a(f"            }} else {{ {enc}_viewport(&e, 0u, 0u, 0u); }}")
    a("            /* rasterization */")
    a("            if (ci->pRasterizationState) {")
    a("                const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;")
    a(f"                {enc}_rasterization(&e, 1u, rs->depthClampEnable, rs->rasterizerDiscardEnable,")
    a("                              (uint32_t)rs->polygonMode, (uint32_t)rs->cullMode,")
    a("                              (uint32_t)rs->frontFace, rs->depthBiasEnable,")
    a("                              rs->depthBiasConstantFactor, rs->depthBiasClamp,")
    a("                              rs->depthBiasSlopeFactor, rs->lineWidth);")
    a(f"            }} else {{ {enc}_rasterization(&e, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0, 0, 0, 0); }}")
    a("            /* multisample */")
    a("            if (ci->pMultisampleState) {")
    a("                const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;")
    a("                /* pSampleMask is an array of ceil(rasterizationSamples/32) sample-mask words. */")
    a("                uint32_t mw = (ms->pSampleMask && ms->rasterizationSamples)")
    a("                    ? (((uint32_t)ms->rasterizationSamples + 31u) / 32u) : 0u;")
    a(f"                {enc}_multisample(&e, 1u, (uint32_t)ms->rasterizationSamples,")
    a("                              ms->sampleShadingEnable, ms->minSampleShading, mw,")
    a("                              ms->alphaToCoverageEnable, ms->alphaToOneEnable);")
    a("                for (k = 0; k < mw; ++k)")
    a(f"                    {enc}_sample_mask(&e, ms->pSampleMask[k]);")
    a(f"            }} else {{ {enc}_multisample(&e, 0u, 0u, 0u, 0, 0u, 0u, 0u); }}")
    a("            /* depth stencil */")
    a("            if (ci->pDepthStencilState) {")
    a("                const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;")
    a(f"                {enc}_depth_stencil(&e, 1u, ds->depthTestEnable, ds->depthWriteEnable,")
    a("                              (uint32_t)ds->depthCompareOp, ds->depthBoundsTestEnable,")
    a("                              ds->stencilTestEnable, ds->minDepthBounds, ds->maxDepthBounds);")
    a(f"                {enc}_stencil_op(&e, (uint32_t)ds->front.failOp, (uint32_t)ds->front.passOp,")
    a("                              (uint32_t)ds->front.depthFailOp, (uint32_t)ds->front.compareOp,")
    a("                              ds->front.compareMask, ds->front.writeMask, ds->front.reference);")
    a(f"                {enc}_stencil_op(&e, (uint32_t)ds->back.failOp, (uint32_t)ds->back.passOp,")
    a("                              (uint32_t)ds->back.depthFailOp, (uint32_t)ds->back.compareOp,")
    a("                              ds->back.compareMask, ds->back.writeMask, ds->back.reference);")
    a(f"            }} else {{ {enc}_depth_stencil(&e, 0u, 0u, 0u, 0u, 0u, 0u, 0, 0); }}")
    a("            /* color blend */")
    a("            if (ci->pColorBlendState) {")
    a("                const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;")
    a(f"                {enc}_color_blend(&e, 1u, cb->logicOpEnable, (uint32_t)cb->logicOp,")
    a("                              cb->attachmentCount, cb->blendConstants[0], cb->blendConstants[1],")
    a("                              cb->blendConstants[2], cb->blendConstants[3]);")
    a("                for (k = 0; k < cb->attachmentCount; ++k)")
    a(f"                    {enc}_blend_attachment(&e, cb->pAttachments[k].blendEnable,")
    a("                              (uint32_t)cb->pAttachments[k].srcColorBlendFactor,")
    a("                              (uint32_t)cb->pAttachments[k].dstColorBlendFactor,")
    a("                              (uint32_t)cb->pAttachments[k].colorBlendOp,")
    a("                              (uint32_t)cb->pAttachments[k].srcAlphaBlendFactor,")
    a("                              (uint32_t)cb->pAttachments[k].dstAlphaBlendFactor,")
    a("                              (uint32_t)cb->pAttachments[k].alphaBlendOp,")
    a("                              (uint32_t)cb->pAttachments[k].colorWriteMask);")
    a(f"            }} else {{ {enc}_color_blend(&e, 0u, 0u, 0u, 0u, 0, 0, 0, 0); }}")
    a("            /* dynamic state */")
    a("            if (ci->pDynamicState) {")
    a("                const VkPipelineDynamicStateCreateInfo *dy = ci->pDynamicState;")
    a(f"                {enc}_dynamic_state(&e, 1u, dy->dynamicStateCount);")
    a("                for (k = 0; k < dy->dynamicStateCount; ++k)")
    a(f"                    {enc}_dynamic_elem(&e, (uint32_t)dy->pDynamicStates[k]);")
    a(f"            }} else {{ {enc}_dynamic_state(&e, 0u, 0u); }}")
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
    # WAVE A render-resource virtual-id pools (disjoint high ranges; see alr_icd_vulkan.c).
    "VkShaderModule": "g_next_vshmod",
    "VkPipelineCache": "g_next_vpcache",
    "VkSampler": "g_next_vsamp",
    "VkFence": "g_next_vfence",
    "VkSemaphore": "g_next_vsem",
    "VkEvent": "g_next_vevent",
    "VkQueryPool": "g_next_vqpool",
    # WAVE B descriptor/layout virtual-id pools.
    "VkDescriptorSetLayout": "g_next_vdsl",
    "VkPipelineLayout": "g_next_vplayout",
    "VkDescriptorPool": "g_next_vdpool",
    "VkDescriptorSet": "g_next_vdset",
    # WAVE C render-pass / framebuffer virtual-id pools.
    "VkRenderPass": "g_next_vrpass",
    "VkFramebuffer": "g_next_vfb",
    # WAVE D pipeline virtual-id pool (graphics + compute share one VkPipeline counter).
    "VkPipeline": "g_next_vpipe",
}
VHANDLE_VAR = {
    "VkCommandPool": "vpool",
    "VkBuffer": "vbuf",
    "VkImage": "vimg",
    "VkImageView": "vview",
    "VkDeviceMemory": "vmem",
    "VkShaderModule": "vshmod",
    "VkPipelineCache": "vpcache",
    "VkSampler": "vsamp",
    "VkFence": "vfence",
    "VkSemaphore": "vsem",
    "VkEvent": "vevent",
    "VkQueryPool": "vqpool",
    "VkDescriptorSetLayout": "vdsl",
    "VkPipelineLayout": "vplayout",
    "VkDescriptorPool": "vdpool",
    "VkDescriptorSet": "vdset",
    "VkRenderPass": "vrpass",
    "VkFramebuffer": "vfb",
    "VkPipeline": "vpipe",
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
    # WAVE A.
    "vkCreateShaderModule": "create_shader_module",
    "vkDestroyShaderModule": "destroy_shader_module",
    "vkCreatePipelineCache": "create_pipeline_cache",
    "vkDestroyPipelineCache": "destroy_pipeline_cache",
    "vkCreateSampler": "create_sampler",
    "vkDestroySampler": "destroy_sampler",
    "vkCreateFence": "create_fence",
    "vkDestroyFence": "destroy_fence",
    "vkCreateSemaphore": "create_semaphore",
    "vkDestroySemaphore": "destroy_semaphore",
    "vkCreateEvent": "create_event",
    "vkDestroyEvent": "destroy_event",
    "vkCreateQueryPool": "create_query_pool",
    "vkDestroyQueryPool": "destroy_query_pool",
    # WAVE B.
    "vkCreateDescriptorSetLayout": "create_descriptor_set_layout",
    "vkDestroyDescriptorSetLayout": "destroy_descriptor_set_layout",
    "vkCreatePipelineLayout": "create_pipeline_layout",
    "vkDestroyPipelineLayout": "destroy_pipeline_layout",
    "vkCreateDescriptorPool": "create_descriptor_pool",
    "vkDestroyDescriptorPool": "destroy_descriptor_pool",
    "vkAllocateDescriptorSets": "allocate_descriptor_sets",
    "vkFreeDescriptorSets": "free_descriptor_sets",
    "vkUpdateDescriptorSets": "update_descriptor_sets",
    # WAVE C.
    "vkCreateRenderPass": "create_render_pass",
    "vkDestroyRenderPass": "destroy_render_pass",
    "vkCreateFramebuffer": "create_framebuffer",
    "vkDestroyFramebuffer": "destroy_framebuffer",
    # WAVE D pipelines.
    "vkCreateGraphicsPipelines": "create_graphics_pipelines",
    "vkCreateComputePipelines": "create_compute_pipelines",
    "vkDestroyPipeline": "destroy_pipeline",
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
        # The generated encoder symbols live in a DISTINCT "alr_vk_enc_gen_" namespace so a
        # generated entrypoint whose snake name equals a HAND-WRITTEN 0..229 encoder (e.g.
        # vkDestroyShaderModule -> alr_vk_enc_destroy_shader_module exists on the 225 band)
        # never collides at link. These are static-inline marshalling helpers, not the wire
        # ABI (the wire is the opcodes, which are unchanged), so the name is free to namespace.
        op["enc_name"] = "alr_vk_enc_gen_" + out_snake
        op["icd_fn"] = "alr_" + name
        op["short"] = SHORT.get(name, out_snake)
        op_num += 1
        k = spec["kind"]
        has_reply = k in ("create_handle", "create_pool", "alloc_memory", "map_memory",
                          "get_reqs", "bind_memory", "create_struct", "alloc_sets",
                          "create_render_pass", "create_pipelines")
        if has_reply:
            op["reply_enum"] = "ALR_VK_GEN_REPLY_" + out_snake.upper()
            op["reply_num"] = reply_num
            reply_num += 1
            op["reply_skip"] = {
                "create_handle": REPLY_SKIP["result"], "create_pool": REPLY_SKIP["result"],
                "bind_memory": REPLY_SKIP["result"], "alloc_memory": REPLY_SKIP["alloc"],
                "map_memory": REPLY_SKIP["map"], "get_reqs": REPLY_SKIP["reqs"],
                # create_struct reply is { u32 vid, i32 result }; alloc_sets is
                # { u32 set_count, i32 result }; create_render_pass { u32 vid, i32 result };
                # create_pipelines { u32 pipeline_count, i32 result } — all the same two-field
                # shape as "result".
                "create_struct": REPLY_SKIP["result"], "alloc_sets": REPLY_SKIP["result"],
                "create_render_pass": REPLY_SKIP["result"],
                "create_pipelines": REPLY_SKIP["result"],
            }[k]
        else:
            op["reply_enum"] = None
            op["reply_num"] = None
        # The create kinds with an out handle type (counter + vout).
        if k in ("create_handle", "create_pool", "alloc_memory", "create_struct", "alloc_sets",
                 "create_render_pass", "create_pipelines"):
            out_ty = spec["out"]
            op["counter"] = COUNTERS[out_ty]
            op["vout"] = VHANDLE_VAR[out_ty]
            if k in ("create_handle", "create_pool"):
                op["ci_wire_fields"] = list(spec["ci_fields"])
                icd_args = []
                # The cast must match the encoder's per-field parameter C type (ctype_for):
                # u32->uint32_t, u64->uint64_t, i32->int32_t, f32->float, a virtual handle
                # ref->uint32_t (the handle carries its virtual id in its low bits).
                _cast = {"u32": "(uint32_t)", "u64": "(uint64_t)", "i32": "(int32_t)",
                         "f32": "(float)", "vhandle": "(uint32_t)(uintptr_t)"}
                for fname, wt in spec["ci_fields"]:
                    if fname.startswith("@"):
                        member = fname[1:].split(":", 1)[0]
                        icd_args.append(f"(uint32_t)(uintptr_t)pCreateInfo->{member}")
                    else:
                        icd_args.append(f"{_cast[wt]}pCreateInfo->{fname}")
                op["icd_ci_args"] = ("" if not icd_args else ", " + ", ".join(icd_args))
                call_args = [wire_var(f) for f, _ in spec["ci_fields"]]
                op["ci_call_args"] = ", ".join(call_args) if call_args else ""
                scal = [wire_var(f) for f, wt in spec["ci_fields"] if wt in ("u32", "u64")]
                while len(scal) < 2:
                    scal.append("0")
                op["ci_first_two"] = f"(uint64_t){scal[0]}, (uint64_t){scal[1]}"
        if k == "create_struct":
            op["ci_wire_fields"] = list(spec["ci_fields"])
            # ICD-side scalar args for _begin (same per-field cast as create_handle).
            _cast = {"u32": "(uint32_t)", "u64": "(uint64_t)", "i32": "(int32_t)",
                     "f32": "(float)", "vhandle": "(uint32_t)(uintptr_t)"}
            icd_args = []
            for fname, wt in spec["ci_fields"]:
                if fname.startswith("@"):
                    member = fname[1:].split(":", 1)[0]
                    icd_args.append(f"(uint32_t)(uintptr_t)pCreateInfo->{member}")
                else:
                    icd_args.append(f"{_cast[wt]}pCreateInfo->{fname}")
            op["icd_ci_args"] = ("" if not icd_args else ", " + ", ".join(icd_args))
            # Give each array a stable C identifier (its ptr_field minus a leading 'p').
            for arr in op["arrays"]:
                pf = arr["ptr_field"]
                arr["array_name"] = (pf[1:] if pf.startswith("p") else pf)
                arr["array_name"] = arr["array_name"][0].lower() + arr["array_name"][1:]
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
