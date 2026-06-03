// ==========================================================================
//  GENERATED FILE — DO NOT EDIT BY HAND.
//  Produced by tools/gen_vk_passthrough.py from tools/vk_registry/vk.xml
//  (Vulkan-Headers v1.3.275, VK_HEADER_VERSION 275). Regenerate with:
//      python3 tools/gen_vk_passthrough.py
//  Edit the SPECS in that tool, not this file. The wire opcodes here are
//  APPEND-ONLY (they are the on-the-wire ABI shared with the host decoder).
// ==========================================================================

#ifndef ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP
#define ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP

#include "alr_gpu/alr_gpu_vk_proto.hpp"  // AlrVkEncoder + the hand-written 0..229 band

#ifdef __cplusplus
extern "C" {
#endif

// The single reserved u8 opcode that escapes into the generated band (first free
// slot above the hand-written 0..229 ops). Followed on the wire by a u16 sub-opcode.
enum { ALR_VK_OP_GEN_ESCAPE = 230 };
// The matching reply escape (host -> guest): a u8 escape + u16 reply sub-opcode, so
// the generated reply records never collide with the hand-written reply band.
enum { ALR_VK_REPLY_GEN_ESCAPE = 230 };

// ---- Generated request SUB-opcodes (u16; ride the escape above). APPEND-ONLY:
// an existing number is the wire and must never move. ----
enum AlrVkGenOp {
    ALR_VK_GEN_OP_CREATE_COMMAND_POOL = 1,  // vkCreateCommandPool
    ALR_VK_GEN_OP_DESTROY_COMMAND_POOL = 2,  // vkDestroyCommandPool
    ALR_VK_GEN_OP_ALLOCATE_MEMORY = 3,  // vkAllocateMemory
    ALR_VK_GEN_OP_MAP_MEMORY = 4,  // vkMapMemory
    ALR_VK_GEN_OP_UNMAP_MEMORY = 5,  // vkUnmapMemory
    ALR_VK_GEN_OP_FLUSH_MAPPED_MEMORY_RANGES = 6,  // vkFlushMappedMemoryRanges
    ALR_VK_GEN_OP_FREE_MEMORY = 7,  // vkFreeMemory
    ALR_VK_GEN_OP_CREATE_BUFFER = 8,  // vkCreateBuffer
    ALR_VK_GEN_OP_DESTROY_BUFFER = 9,  // vkDestroyBuffer
    ALR_VK_GEN_OP_GET_BUFFER_MEMORY_REQUIREMENTS = 10,  // vkGetBufferMemoryRequirements
    ALR_VK_GEN_OP_BIND_BUFFER_MEMORY = 11,  // vkBindBufferMemory
    ALR_VK_GEN_OP_CREATE_IMAGE = 12,  // vkCreateImage
    ALR_VK_GEN_OP_DESTROY_IMAGE = 13,  // vkDestroyImage
    ALR_VK_GEN_OP_GET_IMAGE_MEMORY_REQUIREMENTS = 14,  // vkGetImageMemoryRequirements
    ALR_VK_GEN_OP_BIND_IMAGE_MEMORY = 15,  // vkBindImageMemory
    ALR_VK_GEN_OP_CREATE_IMAGE_VIEW = 16,  // vkCreateImageView
    ALR_VK_GEN_OP_DESTROY_IMAGE_VIEW = 17,  // vkDestroyImageView
};

// ---- Generated reply SUB-opcodes (u16; ride the reply escape). ----
enum AlrVkGenReply {
    ALR_VK_GEN_REPLY_CREATE_COMMAND_POOL = 1,  // reply of vkCreateCommandPool
    ALR_VK_GEN_REPLY_ALLOCATE_MEMORY = 2,  // reply of vkAllocateMemory
    ALR_VK_GEN_REPLY_MAP_MEMORY = 3,  // reply of vkMapMemory
    ALR_VK_GEN_REPLY_CREATE_BUFFER = 4,  // reply of vkCreateBuffer
    ALR_VK_GEN_REPLY_GET_BUFFER_MEMORY_REQUIREMENTS = 5,  // reply of vkGetBufferMemoryRequirements
    ALR_VK_GEN_REPLY_BIND_BUFFER_MEMORY = 6,  // reply of vkBindBufferMemory
    ALR_VK_GEN_REPLY_CREATE_IMAGE = 7,  // reply of vkCreateImage
    ALR_VK_GEN_REPLY_GET_IMAGE_MEMORY_REQUIREMENTS = 8,  // reply of vkGetImageMemoryRequirements
    ALR_VK_GEN_REPLY_BIND_IMAGE_MEMORY = 9,  // reply of vkBindImageMemory
    ALR_VK_GEN_REPLY_CREATE_IMAGE_VIEW = 10,  // reply of vkCreateImageView
};

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

// ---- Per-entrypoint guest encoders. Each appends ONE op (escape + sub-opcode +
// operands) in the exact field order the host decoder (alr_gpu_vk_gen_decode.hpp)
// reads back. ----
// Encoder for vkCreateCommandPool. Ships the device + the guest's virtual VkCommandPool id +
// the VkCommandPoolCreateInfo POD prefix. Append the allowlisted pNext chain after via
// alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's feature-chain shape).
static inline void alr_vk_enc_create_command_pool_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool, uint32_t flags, uint32_t queueFamilyIndex) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_COMMAND_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, queueFamilyIndex);
}

// Encoder for vkDestroyCommandPool (forwards a destroy; no reply).
static inline void alr_vk_enc_destroy_command_pool(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_COMMAND_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
}

// Encoder for vkAllocateMemory (same-process arena). Ships the device + virtual memory id
// + allocationSize + memoryTypeIndex; append the pNext chain after.
static inline void alr_vk_enc_allocate_memory_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vmem,
                          uint64_t allocation_size, uint32_t memory_type_index) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_ALLOCATE_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmem);
    alr_vk_enc_u64(e, allocation_size);
    alr_vk_enc_u32(e, memory_type_index);
}

// Encoder for vkMapMemory (arena path: round-trips once to fetch the arena offset).
static inline void alr_vk_enc_map_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vmemory,
                          uint64_t offset, uint64_t size) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_MAP_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, offset);
    alr_vk_enc_u64(e, size);
}

// Encoder for vkUnmapMemory (arena path: a coherent-flush marker, no driver unmap).
static inline void alr_vk_enc_unmap_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vmemory) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_UNMAP_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmemory);
}

// Encoder for vkFlushMappedMemoryRanges (arena slabs are HOST_COHERENT; a marker for ordering).
// Ships each range's (vmemory, offset, size) so a future non-coherent arena
// could honor it; the count is bounded by the host decoder.
static inline void alr_vk_enc_flush_mapped_memory_ranges_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t range_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_FLUSH_MAPPED_MEMORY_RANGES);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, range_count);
}
static inline void alr_vk_enc_flush_mapped_memory_ranges_range(AlrVkEncoder *e, uint32_t vmemory,
                          uint64_t offset, uint64_t size) {
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, offset);
    alr_vk_enc_u64(e, size);
}

// Encoder for vkFreeMemory (forwards a destroy; no reply).
static inline void alr_vk_enc_free_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vmem) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_FREE_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmem);
}

// Encoder for vkCreateBuffer. Ships the device + the guest's virtual VkBuffer id +
// the VkBufferCreateInfo POD prefix. Append the allowlisted pNext chain after via
// alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's feature-chain shape).
static inline void alr_vk_enc_create_buffer_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf, uint32_t flags, uint64_t size, uint32_t usage, uint32_t sharingMode) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_BUFFER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u64(e, size);
    alr_vk_enc_u32(e, usage);
    alr_vk_enc_u32(e, sharingMode);
}

// Encoder for vkDestroyBuffer (forwards a destroy; no reply).
static inline void alr_vk_enc_destroy_buffer(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_BUFFER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
}

// Encoder for vkGetBufferMemoryRequirements (round-trips: the host returns VkMemoryRequirements).
static inline void alr_vk_enc_get_buffer_memory_requirements(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_GET_BUFFER_MEMORY_REQUIREMENTS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
}

// Encoder for vkBindBufferMemory (binds the real handle to real memory at offset).
static inline void alr_vk_enc_bind_buffer_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf,
                          uint32_t vmemory, uint64_t memory_offset) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_BIND_BUFFER_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, memory_offset);
}

// Encoder for vkCreateImage. Ships the device + the guest's virtual VkImage id +
// the VkImageCreateInfo POD prefix. Append the allowlisted pNext chain after via
// alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's feature-chain shape).
static inline void alr_vk_enc_create_image_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg, uint32_t flags, uint32_t imageType, uint32_t format, uint32_t extent_width, uint32_t extent_height, uint32_t extent_depth, uint32_t mipLevels, uint32_t arrayLayers, uint32_t samples, uint32_t tiling, uint32_t usage, uint32_t sharingMode, uint32_t initialLayout) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_IMAGE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, imageType);
    alr_vk_enc_u32(e, format);
    alr_vk_enc_u32(e, extent_width);
    alr_vk_enc_u32(e, extent_height);
    alr_vk_enc_u32(e, extent_depth);
    alr_vk_enc_u32(e, mipLevels);
    alr_vk_enc_u32(e, arrayLayers);
    alr_vk_enc_u32(e, samples);
    alr_vk_enc_u32(e, tiling);
    alr_vk_enc_u32(e, usage);
    alr_vk_enc_u32(e, sharingMode);
    alr_vk_enc_u32(e, initialLayout);
}

// Encoder for vkDestroyImage (forwards a destroy; no reply).
static inline void alr_vk_enc_destroy_image(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_IMAGE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
}

// Encoder for vkGetImageMemoryRequirements (round-trips: the host returns VkMemoryRequirements).
static inline void alr_vk_enc_get_image_memory_requirements(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_GET_IMAGE_MEMORY_REQUIREMENTS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
}

// Encoder for vkBindImageMemory (binds the real handle to real memory at offset).
static inline void alr_vk_enc_bind_image_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg,
                          uint32_t vmemory, uint64_t memory_offset) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_BIND_IMAGE_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, memory_offset);
}

// Encoder for vkCreateImageView. Ships the device + the guest's virtual VkImageView id +
// the VkImageViewCreateInfo POD prefix. Append the allowlisted pNext chain after via
// alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's feature-chain shape).
static inline void alr_vk_enc_create_image_view_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vview, uint32_t flags, uint32_t image, uint32_t viewType, uint32_t format, uint32_t components_r, uint32_t components_g, uint32_t components_b, uint32_t components_a, uint32_t subresourceRange_aspectMask, uint32_t subresourceRange_baseMipLevel, uint32_t subresourceRange_levelCount, uint32_t subresourceRange_baseArrayLayer, uint32_t subresourceRange_layerCount) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_IMAGE_VIEW);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vview);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, image);
    alr_vk_enc_u32(e, viewType);
    alr_vk_enc_u32(e, format);
    alr_vk_enc_u32(e, components_r);
    alr_vk_enc_u32(e, components_g);
    alr_vk_enc_u32(e, components_b);
    alr_vk_enc_u32(e, components_a);
    alr_vk_enc_u32(e, subresourceRange_aspectMask);
    alr_vk_enc_u32(e, subresourceRange_baseMipLevel);
    alr_vk_enc_u32(e, subresourceRange_levelCount);
    alr_vk_enc_u32(e, subresourceRange_baseArrayLayer);
    alr_vk_enc_u32(e, subresourceRange_layerCount);
}

// Encoder for vkDestroyImageView (forwards a destroy; no reply).
static inline void alr_vk_enc_destroy_image_view(AlrVkEncoder *e, uint32_t vdev, uint32_t vview) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_IMAGE_VIEW);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vview);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP
