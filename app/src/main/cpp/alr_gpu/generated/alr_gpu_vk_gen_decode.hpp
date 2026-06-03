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
    std::map<uint32_t, VkCommandPool> pools;    // vpool  -> real
    std::map<uint32_t, VkBuffer> buffers;       // vbuf   -> real
    std::map<uint32_t, VkImage> images;         // vimg   -> real
    std::map<uint32_t, VkImageView> views;      // vview  -> real
    std::map<uint32_t, VkDeviceMemory> memory;  // vmem   -> real
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
