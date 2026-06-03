// ==========================================================================
//  GENERATED FILE — DO NOT EDIT BY HAND.
//  Produced by tools/gen_vk_passthrough.py from tools/vk_registry/vk.xml
//  (Vulkan-Headers v1.3.275, VK_HEADER_VERSION 275). Regenerate with:
//      python3 tools/gen_vk_passthrough.py
//  Edit the SPECS in that tool, not this file. The wire opcodes here are
//  APPEND-ONLY (they are the on-the-wire ABI shared with the host decoder).
// ==========================================================================

#ifndef ALR_GPU_GENERATED_ALR_GPU_VK_GEN_DECODE_HPP
#define ALR_GPU_GENERATED_ALR_GPU_VK_GEN_DECODE_HPP

// The HOST half of the generated entrypoints. decode_vk_gen_op() is called by
// decode_vk_batch() (alr_gpu_vk_decode.hpp) for any opcode in the 300.. generated
// band — it reads the op off the SAME VkReader, translates the guest's virtual
// handles to real Mali handles via VkDecodeState, calls the REAL entrypoint
// (#ifdef ALR_VK_DECODE_REAL), and appends the reply. Without ALR_VK_DECODE_REAL it
// is a header-only wire codec (handles tracked, no driver) so the host wire test
// proves the round trip with no Vulkan SDK.

#include "alr_gpu/alr_gpu_vk_decode.hpp"        // VkDecodeState, VkReader, VkReplyEncoder
#include "alr_gpu/generated/alr_gpu_vk_gen_proto.hpp"  // the op enums

// --- Block 1: the generated handle tables (VkGenTables + gen_tables). These must be
// a COMPLETE type before the real-Mali bodies below use them, so they are emitted in
// their own namespace block, the real bodies are #included at FILE scope after it
// (the bodies' own #includes must not sit inside a namespace), then block 2 resumes.
namespace alr::gpu {

// Generated handle tables — virtual id -> real Mali handle, persistent across
// batches (like VkDecodeState's own maps). Held in a side struct keyed by the
// owning VkDecodeState's address so the generated decode finds them without
// editing the hand-written struct. One instance per VkDecodeState, via gen_tables(st).
struct VkGenTables {
#ifdef ALR_VK_DECODE_REAL
    std::map<uint32_t, VkCommandPool> pools;    // vpool   -> real
    std::map<uint32_t, VkBuffer> buffers;       // vbuf    -> real
    std::map<uint32_t, VkImage> images;         // vimg    -> real
    std::map<uint32_t, VkImageView> views;      // vview   -> real
    std::map<uint32_t, VkDeviceMemory> memory;  // vmem    -> real
    // WAVE A render-resource handle tables (virtual id -> real Mali handle).
    std::map<uint32_t, VkShaderModule> shader_modules;  // vshmod  -> real
    std::map<uint32_t, VkPipelineCache> pipeline_caches; // vpcache -> real
    std::map<uint32_t, VkSampler> samplers;     // vsamp   -> real
    std::map<uint32_t, VkFence> fences;         // vfence  -> real
    std::map<uint32_t, VkSemaphore> semaphores; // vsem    -> real
    std::map<uint32_t, VkEvent> events;         // vevent  -> real
    std::map<uint32_t, VkQueryPool> query_pools; // vqpool  -> real
    // WAVE B descriptor/layout handle tables.
    std::map<uint32_t, VkDescriptorSetLayout> dsl;      // vdsl     -> real
    std::map<uint32_t, VkPipelineLayout> pipeline_layouts; // vplayout -> real
    std::map<uint32_t, VkDescriptorPool> descriptor_pools; // vdpool  -> real
    std::map<uint32_t, VkDescriptorSet> descriptor_sets;   // vdset   -> real
    // WAVE C render-pass / framebuffer handle tables.
    std::map<uint32_t, VkRenderPass> render_passes;     // vrpass   -> real
    std::map<uint32_t, VkFramebuffer> framebuffers;     // vfb      -> real
    // WAVE D pipeline handle table (graphics + compute share one VkPipeline map).
    std::map<uint32_t, VkPipeline> pipelines;           // vpipe    -> real
#endif
    // Arena offset assigned to each device-memory virtual id (HOST_VISIBLE only).
    // UINT64_MAX == not arena-backed (e.g. a DEVICE_LOCAL alloc). Tracked even in
    // wire-test mode so the map_memory round trip can be asserted with no SDK.
    std::map<uint32_t, uint64_t> mem_arena_off;
    std::map<uint32_t, uint64_t> mem_size;
};
inline VkGenTables& gen_tables(VkDecodeState& st) {
    static std::map<const VkDecodeState*, VkGenTables> g;
    return g[&st];
}

// ---- Wire-side element/struct PODs for the array-bearing generated ops (create_struct,
// alloc_sets, update_sets). All-scalar so they need no Vulkan headers; the real-Mali
// bodies translate the virtual handle fields (vbuffer/vimageview/vsampler/...) to real
// Mali handles via VkGenTables when rebuilding the typed CreateInfo/array. ----
struct VkGenElem_create_descriptor_set_layout_bindings {  // one VkDescriptorSetLayoutBinding
    uint32_t binding = 0;
    uint32_t descriptorType = 0;
    uint32_t descriptorCount = 0;
    uint32_t stageFlags = 0;
};
struct VkGenElem_create_pipeline_layout_setLayouts {  // one VkDescriptorSetLayout
    uint32_t self = 0;
};
struct VkGenElem_create_pipeline_layout_pushConstantRanges {  // one VkPushConstantRange
    uint32_t stageFlags = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};
struct VkGenElem_create_descriptor_pool_poolSizes {  // one VkDescriptorPoolSize
    uint32_t type = 0;
    uint32_t descriptorCount = 0;
};
struct VkGenElem_create_framebuffer_attachments {  // one VkImageView
    uint32_t self = 0;
};
struct VkGenBufferInfo { uint32_t vbuffer = 0; uint64_t offset = 0; uint64_t range = 0; };
struct VkGenImageInfo  { uint32_t vsampler = 0; uint32_t vimageview = 0; uint32_t image_layout = 0; };
struct VkGenDescWrite {
    uint32_t vdstset = 0, binding = 0, array_element = 0;
    uint32_t descriptor_type = 0, descriptor_count = 0;
    std::vector<VkGenBufferInfo> buffers;  // for buffer-class descriptors
    std::vector<VkGenImageInfo> images;    // for image/sampler-class descriptors
};
// Image-class descriptor types (sampler / sampled-image / storage-image / combined /
// input-attachment) ship an image-info triple per descriptor; the rest ship a buffer
// triple. Matches the VkDescriptorType enum values (stable wire numbers).
inline bool vk_gen_desc_is_image(uint32_t t) {
    switch (t) {
        case 0:  // VK_DESCRIPTOR_TYPE_SAMPLER
        case 1:  // COMBINED_IMAGE_SAMPLER
        case 2:  // SAMPLED_IMAGE
        case 3:  // STORAGE_IMAGE
        case 10: // INPUT_ATTACHMENT
            return true;
        default:
            return false;  // UNIFORM_BUFFER / STORAGE_BUFFER / *_DYNAMIC / texel buffers
    }
}
// ---- Render-pass wire PODs (the nested create_render_pass structure). ----
struct VkGenRpAttachment {
    uint32_t flags = 0, format = 0, samples = 0, loadOp = 0, storeOp = 0;
    uint32_t stencilLoadOp = 0, stencilStoreOp = 0, initialLayout = 0, finalLayout = 0;
};
struct VkGenRpRef { uint32_t attachment = 0; uint32_t layout = 0; };
struct VkGenRpSubpass {
    uint32_t flags = 0, pipelineBindPoint = 0;
    std::vector<VkGenRpRef> input, color, resolve;
    bool has_depth = false; VkGenRpRef depth{};
    std::vector<uint32_t> preserve;
};
struct VkGenRpDependency {
    uint32_t srcSubpass = 0, dstSubpass = 0, srcStageMask = 0, dstStageMask = 0;
    uint32_t srcAccessMask = 0, dstAccessMask = 0, dependencyFlags = 0;
};
// ---- Pipeline wire PODs (the deep nested create_pipelines structure; WAVE D). The
// real body rebuilds the typed VkGraphics/ComputePipelineCreateInfo from these,
// translating the module / layout / renderPass / basePipeline virtual handles via
// VkGenTables. All-scalar (handles are u32 virtual ids), so no Vulkan headers here.
struct VkGenPipeSpecEntry { uint32_t constantID = 0, offset = 0, size = 0; };
struct VkGenPipeStage {
    uint32_t stage = 0, vmodule = 0;
    std::string name;                 // entry-point ("main" etc.)
    bool has_spec = false;
    std::vector<VkGenPipeSpecEntry> spec_entries;
    std::vector<uint8_t> spec_data;
};
struct VkGenPipeVertexBinding { uint32_t binding = 0, stride = 0, inputRate = 0; };
struct VkGenPipeVertexAttr { uint32_t location = 0, binding = 0, format = 0, offset = 0; };
struct VkGenPipeViewport { float x = 0, y = 0, w = 0, h = 0, minDepth = 0, maxDepth = 0; };
struct VkGenPipeScissor { int32_t offX = 0, offY = 0; uint32_t extW = 0, extH = 0; };
struct VkGenPipeStencilOp {
    uint32_t failOp = 0, passOp = 0, depthFailOp = 0, compareOp = 0;
    uint32_t compareMask = 0, writeMask = 0, reference = 0;
};
struct VkGenPipeBlendAttachment {
    uint32_t blendEnable = 0, srcColorBlendFactor = 0, dstColorBlendFactor = 0;
    uint32_t colorBlendOp = 0, srcAlphaBlendFactor = 0, dstAlphaBlendFactor = 0;
    uint32_t alphaBlendOp = 0, colorWriteMask = 0;
};
struct VkGenPipeline {
    uint32_t vpipe = 0;  // the guest's virtual id for this pipeline
    uint32_t flags = 0, vlayout = 0, vrenderpass = 0, subpass = 0, vbase = 0;
    int32_t base_index = 0;
    std::vector<VkGenPipeStage> stages;
    // Fixed-function sub-states (graphics). has_* mirror the optional CreateInfo ptrs.
    bool has_vertex_input = false;
    std::vector<VkGenPipeVertexBinding> vbindings;
    std::vector<VkGenPipeVertexAttr> vattrs;
    bool has_input_assembly = false; uint32_t topology = 0, primitiveRestartEnable = 0;
    bool has_tessellation = false; uint32_t patchControlPoints = 0;
    bool has_viewport = false;
    std::vector<VkGenPipeViewport> viewports;
    std::vector<VkGenPipeScissor> scissors;
    bool has_rasterization = false;
    uint32_t depthClampEnable = 0, rasterizerDiscardEnable = 0, polygonMode = 0;
    uint32_t cullMode = 0, frontFace = 0, depthBiasEnable = 0;
    float depthBiasConstantFactor = 0, depthBiasClamp = 0, depthBiasSlopeFactor = 0;
    float lineWidth = 1.0f;
    bool has_multisample = false;
    uint32_t rasterizationSamples = 1, sampleShadingEnable = 0;
    float minSampleShading = 0;
    uint32_t alphaToCoverageEnable = 0, alphaToOneEnable = 0;
    std::vector<uint32_t> sample_mask;
    bool has_depth_stencil = false;
    uint32_t depthTestEnable = 0, depthWriteEnable = 0, depthCompareOp = 0;
    uint32_t depthBoundsTestEnable = 0, stencilTestEnable = 0;
    VkGenPipeStencilOp front{}, back{};
    float minDepthBounds = 0, maxDepthBounds = 0;
    bool has_color_blend = false;
    uint32_t logicOpEnable = 0, logicOp = 0;
    float blendConstants[4] = {0, 0, 0, 0};
    std::vector<VkGenPipeBlendAttachment> blend_attachments;
    bool has_dynamic_state = false;
    std::vector<uint32_t> dynamic_states;
};

}  // namespace alr::gpu (block 1)

// The hand-written real-Mali bodies (vk_gen_real_*) the decode dispatch calls under
// ALR_VK_DECODE_REAL. Included AFTER VkGenTables/gen_tables (complete type) and
// BEFORE decode_vk_gen_op (which calls the bodies). At file scope so gen_real.hpp's
// own #includes (<vulkan/vulkan.h>, <map>) are not nested in a namespace.
#ifdef ALR_VK_DECODE_REAL
#include "alr_gpu/generated/alr_gpu_vk_gen_real.hpp"
#endif

namespace alr::gpu {  // block 2

// Provider seam for the generated ops (the wire test injects a synthetic Mali here;
// null fields fall through to the real-Mali path under ALR_VK_DECODE_REAL).
struct VkGenProvider {
    // create_handle / create_pool: return VkResult-equiv (0 == ok). a,b = the first
    // two scalar CreateInfo fields (for wire-test assertions).
    int (*create_handle)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle,
                         uint64_t a, uint64_t b) = nullptr;
    // alloc_memory: *arena_off_out gets the arena offset (UINT64_MAX if none).
    int (*alloc_memory)(void* ctx, uint32_t vdev, uint32_t vmem, uint64_t size,
                        uint32_t mem_type, uint64_t* arena_off_out) = nullptr;
    bool (*map_memory)(void* ctx, uint32_t vdev, uint32_t vmem, uint64_t* off_out) = nullptr;
    int (*get_reqs)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle,
                    uint64_t* size_out, uint64_t* align_out, uint32_t* bits_out) = nullptr;
    int (*bind_memory)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle,
                       uint32_t vmem, uint64_t off) = nullptr;
    void (*destroy_handle)(void* ctx, uint16_t op, uint32_t vdev, uint32_t vhandle) = nullptr;
    void* ctx = nullptr;
};

// decode_vk_gen_op — dispatch ONE generated op. `op` is the u8 already read by
// decode_vk_batch; we only claim ALR_VK_OP_GEN_ESCAPE. On the escape we read the u16
// sub-opcode + the operands off the SAME reader, translate virtual->real handles via
// `st`, call the real entrypoint (or the synthetic provider), and append the reply.
// Returns true if `op` was the generated escape (handled — even on a malformed body,
// which sets st.ok=false), false if `op` is some other (truly unknown) opcode.
inline bool decode_vk_gen_op(uint8_t op, VkReader& r, VkDecodeState& st,
                             VkReplyEncoder& reply, const VkGenProvider* gp) {
    if (op != ALR_VK_OP_GEN_ESCAPE) return false;  // not ours
    uint16_t sub = 0;
    if (!r.u16(sub)) { st.ok = false; return true; }
    switch (sub) {
        case ALR_VK_GEN_OP_CREATE_COMMAND_POOL: {  // vkCreateCommandPool
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t queueFamilyIndex = 0;
            if (!(r.u32(flags) && r.u32(queueFamilyIndex))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_command_pool(
                    st, vdev, vhandle, flags, queueFamilyIndex,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_COMMAND_POOL, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)queueFamilyIndex);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_COMMAND_POOL));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_COMMAND_POOL: {  // vkDestroyCommandPool
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_command_pool(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_COMMAND_POOL, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_ALLOCATE_MEMORY: {  // vkAllocateMemory
            uint32_t vdev = 0, vmem = 0, memory_type_index = 0;
            uint64_t allocation_size = 0;
            if (!r.u32(vdev) || !r.u32(vmem) || !r.u64(allocation_size) ||
                !r.u32(memory_type_index)) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
            uint64_t arena_off = UINT64_MAX;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_alloc_memory(
                    st, vdev, vmem, allocation_size, memory_type_index, arena_off));
            }
#endif
            if (gp && gp->alloc_memory)
                res = gp->alloc_memory(gp->ctx, vdev, vmem, allocation_size,
                                       memory_type_index, &arena_off);
            gen_tables(st).mem_arena_off[vmem] = arena_off;
            gen_tables(st).mem_size[vmem] = allocation_size;
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_ALLOCATE_MEMORY));
            reply.u32(vmem);
            reply.i32(res);
            reply.u64(arena_off);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_MAP_MEMORY: {  // vkMapMemory
            uint32_t vdev = 0, vmem = 0;
            uint64_t offset = 0, size = 0;
            if (!r.u32(vdev) || !r.u32(vmem) || !r.u64(offset) || !r.u64(size)) {
                st.ok = false; return true; }
            (void)vdev; (void)size;
            uint64_t base_off = UINT64_MAX;
            auto it = gen_tables(st).mem_arena_off.find(vmem);
            if (it != gen_tables(st).mem_arena_off.end()) base_off = it->second;
            if (gp && gp->map_memory) gp->map_memory(gp->ctx, vdev, vmem, &base_off);
            const uint64_t mapped_off =
                (base_off == UINT64_MAX) ? UINT64_MAX : base_off + offset;
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_MAP_MEMORY));
            reply.u32(vmem);
            reply.u64(mapped_off);
            reply.i32(mapped_off == UINT64_MAX ? -1 : 0);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_UNMAP_MEMORY: {  // vkUnmapMemory
            uint32_t vdev = 0, vmem = 0;
            if (!r.u32(vdev) || !r.u32(vmem)) { st.ok = false; return true; }
            (void)vdev; (void)vmem;  // arena is HOST_COHERENT: unmap is a no-op
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_FLUSH_MAPPED_MEMORY_RANGES: {  // vkFlushMappedMemoryRanges
            uint32_t vdev = 0, range_count = 0;
            if (!r.u32(vdev) || !r.u32(range_count)) { st.ok = false; return true; }
            (void)vdev;
            if (range_count > 4096) { st.ok = false; return true; }
            for (uint32_t i = 0; i < range_count; ++i) {
                uint32_t vmem = 0; uint64_t off = 0, sz = 0;
                if (!r.u32(vmem) || !r.u64(off) || !r.u64(sz)) { st.ok = false; return true; }
                (void)vmem; (void)off; (void)sz;  // coherent: nothing to flush
            }
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_FREE_MEMORY: {  // vkFreeMemory
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_free_memory(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_FREE_MEMORY, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_BUFFER: {  // vkCreateBuffer
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint64_t size = 0; uint32_t usage = 0; uint32_t sharingMode = 0;
            if (!(r.u32(flags) && r.u64(size) && r.u32(usage) && r.u32(sharingMode))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_buffer(
                    st, vdev, vhandle, flags, size, usage, sharingMode,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_BUFFER, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)size);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_BUFFER));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_BUFFER: {  // vkDestroyBuffer
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_buffer(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_BUFFER, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_GET_BUFFER_MEMORY_REQUIREMENTS: {  // vkGetBufferMemoryRequirements
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint64_t size = 0, align = 0; uint32_t bits = 0; int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_get_buffer_reqs(
                    st, vdev, vhandle, size, align, bits));
            }
#endif
            if (gp && gp->get_reqs)
                res = gp->get_reqs(gp->ctx, ALR_VK_GEN_OP_GET_BUFFER_MEMORY_REQUIREMENTS, vdev, vhandle,
                                   &size, &align, &bits);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_GET_BUFFER_MEMORY_REQUIREMENTS));
            reply.u32(vhandle);
            reply.i32(res);
            reply.u64(size);
            reply.u64(align);
            reply.u32(bits);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_BIND_BUFFER_MEMORY: {  // vkBindBufferMemory
            uint32_t vdev = 0, vhandle = 0, vmem = 0; uint64_t off = 0;
            if (!r.u32(vdev) || !r.u32(vhandle) || !r.u32(vmem) || !r.u64(off)) {
                st.ok = false; return true; }
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_bind_buffer_memory(
                    st, vdev, vhandle, vmem, off));
            }
#endif
            if (gp && gp->bind_memory)
                res = gp->bind_memory(gp->ctx, ALR_VK_GEN_OP_BIND_BUFFER_MEMORY, vdev, vhandle, vmem, off);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_BIND_BUFFER_MEMORY));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_IMAGE: {  // vkCreateImage
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t imageType = 0; uint32_t format = 0; uint32_t extent_width = 0; uint32_t extent_height = 0; uint32_t extent_depth = 0; uint32_t mipLevels = 0; uint32_t arrayLayers = 0; uint32_t samples = 0; uint32_t tiling = 0; uint32_t usage = 0; uint32_t sharingMode = 0; uint32_t initialLayout = 0;
            if (!(r.u32(flags) && r.u32(imageType) && r.u32(format) && r.u32(extent_width) && r.u32(extent_height) && r.u32(extent_depth) && r.u32(mipLevels) && r.u32(arrayLayers) && r.u32(samples) && r.u32(tiling) && r.u32(usage) && r.u32(sharingMode) && r.u32(initialLayout))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_image(
                    st, vdev, vhandle, flags, imageType, format, extent_width, extent_height, extent_depth, mipLevels, arrayLayers, samples, tiling, usage, sharingMode, initialLayout,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_IMAGE, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)imageType);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_IMAGE));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_IMAGE: {  // vkDestroyImage
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_image(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_IMAGE, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_GET_IMAGE_MEMORY_REQUIREMENTS: {  // vkGetImageMemoryRequirements
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint64_t size = 0, align = 0; uint32_t bits = 0; int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_get_image_reqs(
                    st, vdev, vhandle, size, align, bits));
            }
#endif
            if (gp && gp->get_reqs)
                res = gp->get_reqs(gp->ctx, ALR_VK_GEN_OP_GET_IMAGE_MEMORY_REQUIREMENTS, vdev, vhandle,
                                   &size, &align, &bits);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_GET_IMAGE_MEMORY_REQUIREMENTS));
            reply.u32(vhandle);
            reply.i32(res);
            reply.u64(size);
            reply.u64(align);
            reply.u32(bits);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_BIND_IMAGE_MEMORY: {  // vkBindImageMemory
            uint32_t vdev = 0, vhandle = 0, vmem = 0; uint64_t off = 0;
            if (!r.u32(vdev) || !r.u32(vhandle) || !r.u32(vmem) || !r.u64(off)) {
                st.ok = false; return true; }
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_bind_image_memory(
                    st, vdev, vhandle, vmem, off));
            }
#endif
            if (gp && gp->bind_memory)
                res = gp->bind_memory(gp->ctx, ALR_VK_GEN_OP_BIND_IMAGE_MEMORY, vdev, vhandle, vmem, off);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_BIND_IMAGE_MEMORY));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_IMAGE_VIEW: {  // vkCreateImageView
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t image = 0; uint32_t viewType = 0; uint32_t format = 0; uint32_t components_r = 0; uint32_t components_g = 0; uint32_t components_b = 0; uint32_t components_a = 0; uint32_t subresourceRange_aspectMask = 0; uint32_t subresourceRange_baseMipLevel = 0; uint32_t subresourceRange_levelCount = 0; uint32_t subresourceRange_baseArrayLayer = 0; uint32_t subresourceRange_layerCount = 0;
            if (!(r.u32(flags) && r.u32(image) && r.u32(viewType) && r.u32(format) && r.u32(components_r) && r.u32(components_g) && r.u32(components_b) && r.u32(components_a) && r.u32(subresourceRange_aspectMask) && r.u32(subresourceRange_baseMipLevel) && r.u32(subresourceRange_levelCount) && r.u32(subresourceRange_baseArrayLayer) && r.u32(subresourceRange_layerCount))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_image_view(
                    st, vdev, vhandle, flags, image, viewType, format, components_r, components_g, components_b, components_a, subresourceRange_aspectMask, subresourceRange_baseMipLevel, subresourceRange_levelCount, subresourceRange_baseArrayLayer, subresourceRange_layerCount,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_IMAGE_VIEW, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)viewType);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_IMAGE_VIEW));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_IMAGE_VIEW: {  // vkDestroyImageView
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_image_view(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_IMAGE_VIEW, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_SHADER_MODULE: {  // vkCreateShaderModule
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            const uint8_t* blob_data = nullptr; uint32_t blob_len = 0;
            if (!r.blob(blob_data, blob_len)) { st.ok = false; return true; }
            if (blob_len > (1u << 24)) { st.ok = false; return true; }  // 16MiB cap
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_shader_module(
                    st, vdev, vhandle, flags,
                    blob_data, blob_len,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_SHADER_MODULE, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_SHADER_MODULE));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_SHADER_MODULE: {  // vkDestroyShaderModule
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_shader_module(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_SHADER_MODULE, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_PIPELINE_CACHE: {  // vkCreatePipelineCache
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_pipeline_cache(
                    st, vdev, vhandle, flags,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_PIPELINE_CACHE, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_PIPELINE_CACHE));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_PIPELINE_CACHE: {  // vkDestroyPipelineCache
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_pipeline_cache(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_PIPELINE_CACHE, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_SAMPLER: {  // vkCreateSampler
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t magFilter = 0; uint32_t minFilter = 0; uint32_t mipmapMode = 0; uint32_t addressModeU = 0; uint32_t addressModeV = 0; uint32_t addressModeW = 0; float mipLodBias = 0; uint32_t anisotropyEnable = 0; float maxAnisotropy = 0; uint32_t compareEnable = 0; uint32_t compareOp = 0; float minLod = 0; float maxLod = 0; uint32_t borderColor = 0; uint32_t unnormalizedCoordinates = 0;
            if (!(r.u32(flags) && r.u32(magFilter) && r.u32(minFilter) && r.u32(mipmapMode) && r.u32(addressModeU) && r.u32(addressModeV) && r.u32(addressModeW) && r.f32(mipLodBias) && r.u32(anisotropyEnable) && r.f32(maxAnisotropy) && r.u32(compareEnable) && r.u32(compareOp) && r.f32(minLod) && r.f32(maxLod) && r.u32(borderColor) && r.u32(unnormalizedCoordinates))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_sampler(
                    st, vdev, vhandle, flags, magFilter, minFilter, mipmapMode, addressModeU, addressModeV, addressModeW, mipLodBias, anisotropyEnable, maxAnisotropy, compareEnable, compareOp, minLod, maxLod, borderColor, unnormalizedCoordinates,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_SAMPLER, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)magFilter);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_SAMPLER));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_SAMPLER: {  // vkDestroySampler
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_sampler(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_SAMPLER, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_FENCE: {  // vkCreateFence
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_fence(
                    st, vdev, vhandle, flags,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_FENCE, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_FENCE));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_FENCE: {  // vkDestroyFence
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_fence(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_FENCE, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_SEMAPHORE: {  // vkCreateSemaphore
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_semaphore(
                    st, vdev, vhandle, flags,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_SEMAPHORE, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_SEMAPHORE));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_SEMAPHORE: {  // vkDestroySemaphore
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_semaphore(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_SEMAPHORE, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_EVENT: {  // vkCreateEvent
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_event(
                    st, vdev, vhandle, flags,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_EVENT, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_EVENT));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_EVENT: {  // vkDestroyEvent
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_event(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_EVENT, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_QUERY_POOL: {  // vkCreateQueryPool
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t queryType = 0; uint32_t queryCount = 0; uint32_t pipelineStatistics = 0;
            if (!(r.u32(flags) && r.u32(queryType) && r.u32(queryCount) && r.u32(pipelineStatistics))) { st.ok = false; return true; }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_query_pool(
                    st, vdev, vhandle, flags, queryType, queryCount, pipelineStatistics,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_QUERY_POOL, vdev, vhandle,
                                        (uint64_t)flags, (uint64_t)queryType);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_QUERY_POOL));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_QUERY_POOL: {  // vkDestroyQueryPool
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_query_pool(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_QUERY_POOL, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_DESCRIPTOR_SET_LAYOUT: {  // vkCreateDescriptorSetLayout
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            uint32_t bindings_count = 0;
            if (!r.u32(bindings_count)) { st.ok = false; return true; }
            if (bindings_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenElem_create_descriptor_set_layout_bindings> bindings;
            bindings.reserve(bindings_count);
            for (uint32_t i = 0; i < bindings_count; ++i) {
                VkGenElem_create_descriptor_set_layout_bindings el{};
                if (!(r.u32(el.binding) && r.u32(el.descriptorType) && r.u32(el.descriptorCount) && r.u32(el.stageFlags))) { st.ok = false; return true; }
                bindings.push_back(el);
            }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_descriptor_set_layout(
                    st, vdev, vhandle, flags, bindings, pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_DESCRIPTOR_SET_LAYOUT, vdev, vhandle, 0, 0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_DESCRIPTOR_SET_LAYOUT));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_SET_LAYOUT: {  // vkDestroyDescriptorSetLayout
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_descriptor_set_layout(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_SET_LAYOUT, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_PIPELINE_LAYOUT: {  // vkCreatePipelineLayout
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0;
            if (!(r.u32(flags))) { st.ok = false; return true; }
            uint32_t setLayouts_count = 0;
            if (!r.u32(setLayouts_count)) { st.ok = false; return true; }
            if (setLayouts_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenElem_create_pipeline_layout_setLayouts> setLayouts;
            setLayouts.reserve(setLayouts_count);
            for (uint32_t i = 0; i < setLayouts_count; ++i) {
                VkGenElem_create_pipeline_layout_setLayouts el{};
                if (!(r.u32(el.self))) { st.ok = false; return true; }
                setLayouts.push_back(el);
            }
            uint32_t pushConstantRanges_count = 0;
            if (!r.u32(pushConstantRanges_count)) { st.ok = false; return true; }
            if (pushConstantRanges_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenElem_create_pipeline_layout_pushConstantRanges> pushConstantRanges;
            pushConstantRanges.reserve(pushConstantRanges_count);
            for (uint32_t i = 0; i < pushConstantRanges_count; ++i) {
                VkGenElem_create_pipeline_layout_pushConstantRanges el{};
                if (!(r.u32(el.stageFlags) && r.u32(el.offset) && r.u32(el.size))) { st.ok = false; return true; }
                pushConstantRanges.push_back(el);
            }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_pipeline_layout(
                    st, vdev, vhandle, flags, setLayouts, pushConstantRanges, pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_PIPELINE_LAYOUT, vdev, vhandle, 0, 0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_PIPELINE_LAYOUT));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_PIPELINE_LAYOUT: {  // vkDestroyPipelineLayout
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_pipeline_layout(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_PIPELINE_LAYOUT, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_DESCRIPTOR_POOL: {  // vkCreateDescriptorPool
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t maxSets = 0;
            if (!(r.u32(flags) && r.u32(maxSets))) { st.ok = false; return true; }
            uint32_t poolSizes_count = 0;
            if (!r.u32(poolSizes_count)) { st.ok = false; return true; }
            if (poolSizes_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenElem_create_descriptor_pool_poolSizes> poolSizes;
            poolSizes.reserve(poolSizes_count);
            for (uint32_t i = 0; i < poolSizes_count; ++i) {
                VkGenElem_create_descriptor_pool_poolSizes el{};
                if (!(r.u32(el.type) && r.u32(el.descriptorCount))) { st.ok = false; return true; }
                poolSizes.push_back(el);
            }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_descriptor_pool(
                    st, vdev, vhandle, flags, maxSets, poolSizes, pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_DESCRIPTOR_POOL, vdev, vhandle, 0, 0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_DESCRIPTOR_POOL));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_POOL: {  // vkDestroyDescriptorPool
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_descriptor_pool(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_POOL, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_ALLOCATE_DESCRIPTOR_SETS: {  // vkAllocateDescriptorSets
            uint32_t vdev = 0, vpool = 0, set_count = 0;
            if (!r.u32(vdev) || !r.u32(vpool) || !r.u32(set_count)) {
                st.ok = false; return true; }
            if (set_count > 4096) { st.ok = false; return true; }
            std::vector<uint32_t> vlayouts, vsets;
            vlayouts.reserve(set_count); vsets.reserve(set_count);
            for (uint32_t i = 0; i < set_count; ++i) {
                uint32_t vl = 0, vs = 0;
                if (!r.u32(vl) || !r.u32(vs)) { st.ok = false; return true; }
                vlayouts.push_back(vl); vsets.push_back(vs);
            }
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_allocate_descriptor_sets(
                    st, vdev, vpool, vlayouts, vsets));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_ALLOCATE_DESCRIPTOR_SETS, vdev,
                                        set_count ? vsets[0] : 0, set_count, vpool);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_ALLOCATE_DESCRIPTOR_SETS));
            reply.u32(set_count);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_FREE_DESCRIPTOR_SETS: {  // vkFreeDescriptorSets
            uint32_t vdev = 0, vpool = 0, set_count = 0;
            if (!r.u32(vdev) || !r.u32(vpool) || !r.u32(set_count)) {
                st.ok = false; return true; }
            if (set_count > 4096) { st.ok = false; return true; }
            std::vector<uint32_t> vsets;
            vsets.reserve(set_count);
            for (uint32_t i = 0; i < set_count; ++i) {
                uint32_t vs = 0; if (!r.u32(vs)) { st.ok = false; return true; }
                vsets.push_back(vs);
            }
#ifdef ALR_VK_DECODE_REAL
            if (!gp) vk_gen_real_free_descriptor_sets(st, vdev, vpool, vsets);
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_FREE_DESCRIPTOR_SETS, vdev, set_count);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_UPDATE_DESCRIPTOR_SETS: {  // vkUpdateDescriptorSets
            uint32_t vdev = 0, write_count = 0;
            if (!r.u32(vdev) || !r.u32(write_count)) { st.ok = false; return true; }
            if (write_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenDescWrite> writes;
            writes.reserve(write_count);
            for (uint32_t i = 0; i < write_count; ++i) {
                VkGenDescWrite w{};
                if (!r.u32(w.vdstset) || !r.u32(w.binding) || !r.u32(w.array_element) ||
                    !r.u32(w.descriptor_type) || !r.u32(w.descriptor_count)) {
                    st.ok = false; return true; }
                if (w.descriptor_count > 4096) { st.ok = false; return true; }
                bool is_image = vk_gen_desc_is_image(w.descriptor_type);
                for (uint32_t d = 0; d < w.descriptor_count; ++d) {
                    if (is_image) {
                        VkGenImageInfo ii{};
                        if (!r.u32(ii.vsampler) || !r.u32(ii.vimageview) ||
                            !r.u32(ii.image_layout)) { st.ok = false; return true; }
                        w.images.push_back(ii);
                    } else {
                        VkGenBufferInfo bi{};
                        if (!r.u32(bi.vbuffer) || !r.u64(bi.offset) || !r.u64(bi.range)) {
                            st.ok = false; return true; }
                        w.buffers.push_back(bi);
                    }
                }
                writes.push_back(std::move(w));
            }
#ifdef ALR_VK_DECODE_REAL
            if (!gp) vk_gen_real_update_descriptor_sets(st, vdev, writes);
#endif
            (void)vdev;
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_RENDER_PASS: {  // vkCreateRenderPass
            uint32_t vdev = 0, vhandle = 0, flags = 0;
            if (!r.u32(vdev) || !r.u32(vhandle) || !r.u32(flags)) {
                st.ok = false; return true; }
            uint32_t att_count = 0;
            if (!r.u32(att_count) || att_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenRpAttachment> attachments; attachments.reserve(att_count);
            for (uint32_t i = 0; i < att_count; ++i) {
                VkGenRpAttachment a{};
                if (!r.u32(a.flags) || !r.u32(a.format) || !r.u32(a.samples) ||
                    !r.u32(a.loadOp) || !r.u32(a.storeOp) || !r.u32(a.stencilLoadOp) ||
                    !r.u32(a.stencilStoreOp) || !r.u32(a.initialLayout) ||
                    !r.u32(a.finalLayout)) { st.ok = false; return true; }
                attachments.push_back(a);
            }
            uint32_t sub_count = 0;
            if (!r.u32(sub_count) || sub_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenRpSubpass> subpasses; subpasses.reserve(sub_count);
            for (uint32_t i = 0; i < sub_count; ++i) {
                VkGenRpSubpass s{};
                uint32_t inC = 0, colC = 0, resC = 0, hasD = 0, presC = 0;
                if (!r.u32(s.flags) || !r.u32(s.pipelineBindPoint) || !r.u32(inC) ||
                    !r.u32(colC) || !r.u32(resC) || !r.u32(hasD) || !r.u32(presC)) {
                    st.ok = false; return true; }
                if (inC > 4096 || colC > 4096 || resC > 4096 || presC > 4096) {
                    st.ok = false; return true; }
                auto read_refs = [&](std::vector<VkGenRpRef>& out, uint32_t n) -> bool {
                    out.reserve(n);
                    for (uint32_t j = 0; j < n; ++j) { VkGenRpRef rf{};
                        if (!r.u32(rf.attachment) || !r.u32(rf.layout)) return false;
                        out.push_back(rf); } return true; };
                if (!read_refs(s.input, inC) || !read_refs(s.color, colC) ||
                    !read_refs(s.resolve, resC)) { st.ok = false; return true; }
                s.has_depth = hasD != 0;
                if (s.has_depth) { if (!r.u32(s.depth.attachment) ||
                    !r.u32(s.depth.layout)) { st.ok = false; return true; } }
                s.preserve.reserve(presC);
                for (uint32_t j = 0; j < presC; ++j) { uint32_t p = 0;
                    if (!r.u32(p)) { st.ok = false; return true; } s.preserve.push_back(p); }
                subpasses.push_back(std::move(s));
            }
            uint32_t dep_count = 0;
            if (!r.u32(dep_count) || dep_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenRpDependency> deps; deps.reserve(dep_count);
            for (uint32_t i = 0; i < dep_count; ++i) {
                VkGenRpDependency d{};
                if (!r.u32(d.srcSubpass) || !r.u32(d.dstSubpass) || !r.u32(d.srcStageMask) ||
                    !r.u32(d.dstStageMask) || !r.u32(d.srcAccessMask) ||
                    !r.u32(d.dstAccessMask) || !r.u32(d.dependencyFlags)) {
                    st.ok = false; return true; }
                deps.push_back(d);
            }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_render_pass(
                    st, vdev, vhandle, flags, attachments, subpasses, deps,
                    pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_RENDER_PASS, vdev, vhandle, 0, 0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_RENDER_PASS));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_RENDER_PASS: {  // vkDestroyRenderPass
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_render_pass(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_RENDER_PASS, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_FRAMEBUFFER: {  // vkCreateFramebuffer
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            uint32_t flags = 0; uint32_t renderPass = 0; uint32_t width = 0; uint32_t height = 0; uint32_t layers = 0;
            if (!(r.u32(flags) && r.u32(renderPass) && r.u32(width) && r.u32(height) && r.u32(layers))) { st.ok = false; return true; }
            uint32_t attachments_count = 0;
            if (!r.u32(attachments_count)) { st.ok = false; return true; }
            if (attachments_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenElem_create_framebuffer_attachments> attachments;
            attachments.reserve(attachments_count);
            for (uint32_t i = 0; i < attachments_count; ++i) {
                VkGenElem_create_framebuffer_attachments el{};
                if (!(r.u32(el.self))) { st.ok = false; return true; }
                attachments.push_back(el);
            }
            uint32_t pnext_count = 0;
            std::vector<uint32_t> pnext_types;
            std::vector<std::vector<uint8_t>> pnext_bytes;
            if (!r.u32(pnext_count)) { st.ok = false; return true; }
            if (pnext_count > 32) { st.ok = false; return true; }
            for (uint32_t i = 0; i < pnext_count; ++i) {
                uint32_t stype = 0; const uint8_t* d = nullptr; uint32_t n = 0;
                if (!r.u32(stype) || !r.blob(d, n)) { st.ok = false; return true; }
                if (n > 1024) { st.ok = false; return true; }
                pnext_types.push_back(stype);
                pnext_bytes.emplace_back(d, d + n);
            }
            (void)pnext_count;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_framebuffer(
                    st, vdev, vhandle, flags, renderPass, width, height, layers, attachments, pnext_types, pnext_bytes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_FRAMEBUFFER, vdev, vhandle, 0, 0);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_FRAMEBUFFER));
            reply.u32(vhandle);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_FRAMEBUFFER: {  // vkDestroyFramebuffer
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_framebuffer(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_FRAMEBUFFER, vdev, vhandle);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_GRAPHICS_PIPELINES: {  // vkCreateGraphicsPipelines
            uint32_t vdev = 0, vpcache = 0, pipeline_count = 0;
            if (!r.u32(vdev) || !r.u32(vpcache) || !r.u32(pipeline_count)) {
                st.ok = false; return true; }
            if (pipeline_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenPipeline> pipes; pipes.reserve(pipeline_count);
            for (uint32_t pi = 0; pi < pipeline_count; ++pi) {
                VkGenPipeline p{};
                uint32_t stage_count = 0;
                if (!r.u32(p.vpipe) || !r.u32(p.flags) || !r.u32(p.vlayout) ||
                    !r.u32(p.vrenderpass) || !r.u32(p.subpass) || !r.u32(p.vbase) ||
                    !r.i32(p.base_index) || !r.u32(stage_count)) { st.ok = false; return true; }
                if (stage_count > 4096) { st.ok = false; return true; }
                p.stages.reserve(stage_count);
                for (uint32_t si = 0; si < stage_count; ++si) {
                    VkGenPipeStage s{};
                    const uint8_t* nm = nullptr; uint32_t nlen = 0; uint32_t spec_present = 0;
                    if (!r.u32(s.stage) || !r.u32(s.vmodule) || !r.blob(nm, nlen) ||
                        !r.u32(spec_present)) { st.ok = false; return true; }
                    if (nlen > 4096) { st.ok = false; return true; }
                    s.name.assign(reinterpret_cast<const char*>(nm), nlen);
                    if (spec_present) {
                        s.has_spec = true;
                        uint32_t me_count = 0, data_len = 0;
                        if (!r.u32(me_count) || !r.u32(data_len)) { st.ok = false; return true; }
                        if (me_count > 4096 || data_len > 1048576) {
                            st.ok = false; return true; }
                        s.spec_entries.reserve(me_count);
                        for (uint32_t mi = 0; mi < me_count; ++mi) {
                            VkGenPipeSpecEntry me{};
                            if (!r.u32(me.constantID) || !r.u32(me.offset) || !r.u32(me.size)) {
                                st.ok = false; return true; }
                            s.spec_entries.push_back(me);
                        }
                        const uint8_t* sd = nullptr; uint32_t sdl = 0;
                        if (!r.blob(sd, sdl) || sdl != data_len) { st.ok = false; return true; }
                        s.spec_data.assign(sd, sd + sdl);
                    }
                    p.stages.push_back(std::move(s));
                }
                // ---- fixed-function sub-states (graphics), each behind a presence flag ----
                uint32_t present = 0;
                // vertex input
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_vertex_input = true;
                    uint32_t bc = 0, ac = 0;
                    if (!r.u32(bc) || !r.u32(ac)) { st.ok = false; return true; }
                    if (bc > 4096 || ac > 4096) { st.ok = false; return true; }
                    p.vbindings.reserve(bc);
                    for (uint32_t i = 0; i < bc; ++i) { VkGenPipeVertexBinding b{};
                        if (!r.u32(b.binding) || !r.u32(b.stride) || !r.u32(b.inputRate)) {
                            st.ok = false; return true; } p.vbindings.push_back(b); }
                    p.vattrs.reserve(ac);
                    for (uint32_t i = 0; i < ac; ++i) { VkGenPipeVertexAttr at{};
                        if (!r.u32(at.location) || !r.u32(at.binding) || !r.u32(at.format) ||
                            !r.u32(at.offset)) { st.ok = false; return true; } p.vattrs.push_back(at); }
                }
                // input assembly
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_input_assembly = true;
                    if (!r.u32(p.topology) || !r.u32(p.primitiveRestartEnable)) {
                        st.ok = false; return true; } }
                // tessellation
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_tessellation = true;
                    if (!r.u32(p.patchControlPoints)) { st.ok = false; return true; } }
                // viewport
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_viewport = true;
                    uint32_t vc = 0, sc = 0;
                    if (!r.u32(vc) || !r.u32(sc)) { st.ok = false; return true; }
                    if (vc > 4096 || sc > 4096) { st.ok = false; return true; }
                    p.viewports.reserve(vc);
                    for (uint32_t i = 0; i < vc; ++i) { VkGenPipeViewport v{};
                        if (!r.f32(v.x) || !r.f32(v.y) || !r.f32(v.w) || !r.f32(v.h) ||
                            !r.f32(v.minDepth) || !r.f32(v.maxDepth)) { st.ok = false; return true; }
                        p.viewports.push_back(v); }
                    p.scissors.reserve(sc);
                    for (uint32_t i = 0; i < sc; ++i) { VkGenPipeScissor s{};
                        if (!r.i32(s.offX) || !r.i32(s.offY) || !r.u32(s.extW) || !r.u32(s.extH)) {
                            st.ok = false; return true; } p.scissors.push_back(s); }
                }
                // rasterization
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_rasterization = true;
                    if (!r.u32(p.depthClampEnable) || !r.u32(p.rasterizerDiscardEnable) ||
                        !r.u32(p.polygonMode) || !r.u32(p.cullMode) || !r.u32(p.frontFace) ||
                        !r.u32(p.depthBiasEnable) || !r.f32(p.depthBiasConstantFactor) ||
                        !r.f32(p.depthBiasClamp) || !r.f32(p.depthBiasSlopeFactor) ||
                        !r.f32(p.lineWidth)) { st.ok = false; return true; } }
                // multisample
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_multisample = true;
                    uint32_t mask_words = 0;
                    if (!r.u32(p.rasterizationSamples) || !r.u32(p.sampleShadingEnable) ||
                        !r.f32(p.minSampleShading) || !r.u32(mask_words) ||
                        !r.u32(p.alphaToCoverageEnable) || !r.u32(p.alphaToOneEnable)) {
                        st.ok = false; return true; }
                    if (mask_words > 4096) { st.ok = false; return true; }
                    p.sample_mask.reserve(mask_words);
                    for (uint32_t i = 0; i < mask_words; ++i) { uint32_t w = 0;
                        if (!r.u32(w)) { st.ok = false; return true; } p.sample_mask.push_back(w); } }
                // depth stencil
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_depth_stencil = true;
                    if (!r.u32(p.depthTestEnable) || !r.u32(p.depthWriteEnable) ||
                        !r.u32(p.depthCompareOp) || !r.u32(p.depthBoundsTestEnable) ||
                        !r.u32(p.stencilTestEnable) || !r.f32(p.minDepthBounds) ||
                        !r.f32(p.maxDepthBounds)) { st.ok = false; return true; }
                    auto rd_stencil = [&](VkGenPipeStencilOp& so) -> bool {
                        return r.u32(so.failOp) && r.u32(so.passOp) && r.u32(so.depthFailOp) &&
                               r.u32(so.compareOp) && r.u32(so.compareMask) &&
                               r.u32(so.writeMask) && r.u32(so.reference); };
                    if (!rd_stencil(p.front) || !rd_stencil(p.back)) { st.ok = false; return true; } }
                // color blend
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_color_blend = true;
                    uint32_t att = 0;
                    if (!r.u32(p.logicOpEnable) || !r.u32(p.logicOp) || !r.u32(att) ||
                        !r.f32(p.blendConstants[0]) || !r.f32(p.blendConstants[1]) ||
                        !r.f32(p.blendConstants[2]) || !r.f32(p.blendConstants[3])) {
                        st.ok = false; return true; }
                    if (att > 4096) { st.ok = false; return true; }
                    p.blend_attachments.reserve(att);
                    for (uint32_t i = 0; i < att; ++i) { VkGenPipeBlendAttachment ba{};
                        if (!r.u32(ba.blendEnable) || !r.u32(ba.srcColorBlendFactor) ||
                            !r.u32(ba.dstColorBlendFactor) || !r.u32(ba.colorBlendOp) ||
                            !r.u32(ba.srcAlphaBlendFactor) || !r.u32(ba.dstAlphaBlendFactor) ||
                            !r.u32(ba.alphaBlendOp) || !r.u32(ba.colorWriteMask)) {
                            st.ok = false; return true; } p.blend_attachments.push_back(ba); } }
                // dynamic state
                if (!r.u32(present)) { st.ok = false; return true; }
                if (present) { p.has_dynamic_state = true;
                    uint32_t dc = 0;
                    if (!r.u32(dc)) { st.ok = false; return true; }
                    if (dc > 4096) { st.ok = false; return true; }
                    p.dynamic_states.reserve(dc);
                    for (uint32_t i = 0; i < dc; ++i) { uint32_t d = 0;
                        if (!r.u32(d)) { st.ok = false; return true; } p.dynamic_states.push_back(d); } }
                pipes.push_back(std::move(p));
            }
            (void)vpcache;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_graphics_pipelines(st, vdev, vpcache, pipes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_GRAPHICS_PIPELINES, vdev,
                                        pipeline_count ? pipes[0].flags : 0,
                                        pipeline_count, vpcache);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_GRAPHICS_PIPELINES));
            reply.u32(pipeline_count);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_CREATE_COMPUTE_PIPELINES: {  // vkCreateComputePipelines
            uint32_t vdev = 0, vpcache = 0, pipeline_count = 0;
            if (!r.u32(vdev) || !r.u32(vpcache) || !r.u32(pipeline_count)) {
                st.ok = false; return true; }
            if (pipeline_count > 4096) { st.ok = false; return true; }
            std::vector<VkGenPipeline> pipes; pipes.reserve(pipeline_count);
            for (uint32_t pi = 0; pi < pipeline_count; ++pi) {
                VkGenPipeline p{};
                uint32_t stage_count = 0;
                if (!r.u32(p.vpipe) || !r.u32(p.flags) || !r.u32(p.vlayout) ||
                    !r.u32(p.vrenderpass) || !r.u32(p.subpass) || !r.u32(p.vbase) ||
                    !r.i32(p.base_index) || !r.u32(stage_count)) { st.ok = false; return true; }
                if (stage_count > 4096) { st.ok = false; return true; }
                p.stages.reserve(stage_count);
                for (uint32_t si = 0; si < stage_count; ++si) {
                    VkGenPipeStage s{};
                    const uint8_t* nm = nullptr; uint32_t nlen = 0; uint32_t spec_present = 0;
                    if (!r.u32(s.stage) || !r.u32(s.vmodule) || !r.blob(nm, nlen) ||
                        !r.u32(spec_present)) { st.ok = false; return true; }
                    if (nlen > 4096) { st.ok = false; return true; }
                    s.name.assign(reinterpret_cast<const char*>(nm), nlen);
                    if (spec_present) {
                        s.has_spec = true;
                        uint32_t me_count = 0, data_len = 0;
                        if (!r.u32(me_count) || !r.u32(data_len)) { st.ok = false; return true; }
                        if (me_count > 4096 || data_len > 1048576) {
                            st.ok = false; return true; }
                        s.spec_entries.reserve(me_count);
                        for (uint32_t mi = 0; mi < me_count; ++mi) {
                            VkGenPipeSpecEntry me{};
                            if (!r.u32(me.constantID) || !r.u32(me.offset) || !r.u32(me.size)) {
                                st.ok = false; return true; }
                            s.spec_entries.push_back(me);
                        }
                        const uint8_t* sd = nullptr; uint32_t sdl = 0;
                        if (!r.blob(sd, sdl) || sdl != data_len) { st.ok = false; return true; }
                        s.spec_data.assign(sd, sd + sdl);
                    }
                    p.stages.push_back(std::move(s));
                }
                pipes.push_back(std::move(p));
            }
            (void)vpcache;
            int res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                res = static_cast<int>(vk_gen_real_create_compute_pipelines(st, vdev, vpcache, pipes));
            }
#endif
            if (gp && gp->create_handle)
                res = gp->create_handle(gp->ctx, ALR_VK_GEN_OP_CREATE_COMPUTE_PIPELINES, vdev,
                                        pipeline_count ? pipes[0].flags : 0,
                                        pipeline_count, vpcache);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_GEN_REPLY_CREATE_COMPUTE_PIPELINES));
            reply.u32(pipeline_count);
            reply.i32(res);
            st.decoded++;
            return true;
        }
        case ALR_VK_GEN_OP_DESTROY_PIPELINE: {  // vkDestroyPipeline
            uint32_t vdev = 0, vhandle = 0;
            if (!r.u32(vdev) || !r.u32(vhandle)) { st.ok = false; return true; }
            (void)vdev;
#ifdef ALR_VK_DECODE_REAL
            if (!gp) {
                vk_gen_real_destroy_pipeline(st, vdev, vhandle);
            }
#endif
            if (gp && gp->destroy_handle)
                gp->destroy_handle(gp->ctx, ALR_VK_GEN_OP_DESTROY_PIPELINE, vdev, vhandle);
            st.decoded++;
            return true;
        }
        default:
            st.ok = false;  // escape with an unknown sub-opcode: fail-stop
            return true;
    }
    return true;
}

// ---- Self-registration into decode_vk_batch's generated-op seam. The opaque
// `gen_provider` from decode_vk_batch is the VkGenProvider* the caller set (null on
// device -> real-Mali path). A function-local static registrar runs at first use of
// this TU so simply #including this header wires the 300.. band into decode_vk_batch.
inline bool vk_gen_dispatch_adapter(uint8_t op, VkReader& r, VkDecodeState& st,
                                    VkReplyEncoder& reply, const void* gen_provider) {
    return decode_vk_gen_op(op, r, st, reply,
                            static_cast<const VkGenProvider*>(gen_provider));
}
inline bool vk_gen_register_dispatch() {
    set_vk_gen_dispatch(&vk_gen_dispatch_adapter);
    return true;
}
inline const bool kVkGenDispatchRegistered = vk_gen_register_dispatch();

}  // namespace alr::gpu

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_DECODE_HPP
