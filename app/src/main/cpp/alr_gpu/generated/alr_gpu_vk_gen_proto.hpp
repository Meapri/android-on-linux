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
    ALR_VK_GEN_OP_CREATE_SHADER_MODULE = 18,  // vkCreateShaderModule
    ALR_VK_GEN_OP_DESTROY_SHADER_MODULE = 19,  // vkDestroyShaderModule
    ALR_VK_GEN_OP_CREATE_PIPELINE_CACHE = 20,  // vkCreatePipelineCache
    ALR_VK_GEN_OP_DESTROY_PIPELINE_CACHE = 21,  // vkDestroyPipelineCache
    ALR_VK_GEN_OP_CREATE_SAMPLER = 22,  // vkCreateSampler
    ALR_VK_GEN_OP_DESTROY_SAMPLER = 23,  // vkDestroySampler
    ALR_VK_GEN_OP_CREATE_FENCE = 24,  // vkCreateFence
    ALR_VK_GEN_OP_DESTROY_FENCE = 25,  // vkDestroyFence
    ALR_VK_GEN_OP_CREATE_SEMAPHORE = 26,  // vkCreateSemaphore
    ALR_VK_GEN_OP_DESTROY_SEMAPHORE = 27,  // vkDestroySemaphore
    ALR_VK_GEN_OP_CREATE_EVENT = 28,  // vkCreateEvent
    ALR_VK_GEN_OP_DESTROY_EVENT = 29,  // vkDestroyEvent
    ALR_VK_GEN_OP_CREATE_QUERY_POOL = 30,  // vkCreateQueryPool
    ALR_VK_GEN_OP_DESTROY_QUERY_POOL = 31,  // vkDestroyQueryPool
    ALR_VK_GEN_OP_CREATE_DESCRIPTOR_SET_LAYOUT = 32,  // vkCreateDescriptorSetLayout
    ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_SET_LAYOUT = 33,  // vkDestroyDescriptorSetLayout
    ALR_VK_GEN_OP_CREATE_PIPELINE_LAYOUT = 34,  // vkCreatePipelineLayout
    ALR_VK_GEN_OP_DESTROY_PIPELINE_LAYOUT = 35,  // vkDestroyPipelineLayout
    ALR_VK_GEN_OP_CREATE_DESCRIPTOR_POOL = 36,  // vkCreateDescriptorPool
    ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_POOL = 37,  // vkDestroyDescriptorPool
    ALR_VK_GEN_OP_ALLOCATE_DESCRIPTOR_SETS = 38,  // vkAllocateDescriptorSets
    ALR_VK_GEN_OP_FREE_DESCRIPTOR_SETS = 39,  // vkFreeDescriptorSets
    ALR_VK_GEN_OP_UPDATE_DESCRIPTOR_SETS = 40,  // vkUpdateDescriptorSets
    ALR_VK_GEN_OP_CREATE_RENDER_PASS = 41,  // vkCreateRenderPass
    ALR_VK_GEN_OP_DESTROY_RENDER_PASS = 42,  // vkDestroyRenderPass
    ALR_VK_GEN_OP_CREATE_FRAMEBUFFER = 43,  // vkCreateFramebuffer
    ALR_VK_GEN_OP_DESTROY_FRAMEBUFFER = 44,  // vkDestroyFramebuffer
    ALR_VK_GEN_OP_CREATE_GRAPHICS_PIPELINES = 45,  // vkCreateGraphicsPipelines
    ALR_VK_GEN_OP_CREATE_COMPUTE_PIPELINES = 46,  // vkCreateComputePipelines
    ALR_VK_GEN_OP_DESTROY_PIPELINE = 47,  // vkDestroyPipeline
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
    ALR_VK_GEN_REPLY_CREATE_SHADER_MODULE = 11,  // reply of vkCreateShaderModule
    ALR_VK_GEN_REPLY_CREATE_PIPELINE_CACHE = 12,  // reply of vkCreatePipelineCache
    ALR_VK_GEN_REPLY_CREATE_SAMPLER = 13,  // reply of vkCreateSampler
    ALR_VK_GEN_REPLY_CREATE_FENCE = 14,  // reply of vkCreateFence
    ALR_VK_GEN_REPLY_CREATE_SEMAPHORE = 15,  // reply of vkCreateSemaphore
    ALR_VK_GEN_REPLY_CREATE_EVENT = 16,  // reply of vkCreateEvent
    ALR_VK_GEN_REPLY_CREATE_QUERY_POOL = 17,  // reply of vkCreateQueryPool
    ALR_VK_GEN_REPLY_CREATE_DESCRIPTOR_SET_LAYOUT = 18,  // reply of vkCreateDescriptorSetLayout
    ALR_VK_GEN_REPLY_CREATE_PIPELINE_LAYOUT = 19,  // reply of vkCreatePipelineLayout
    ALR_VK_GEN_REPLY_CREATE_DESCRIPTOR_POOL = 20,  // reply of vkCreateDescriptorPool
    ALR_VK_GEN_REPLY_ALLOCATE_DESCRIPTOR_SETS = 21,  // reply of vkAllocateDescriptorSets
    ALR_VK_GEN_REPLY_CREATE_RENDER_PASS = 22,  // reply of vkCreateRenderPass
    ALR_VK_GEN_REPLY_CREATE_FRAMEBUFFER = 23,  // reply of vkCreateFramebuffer
    ALR_VK_GEN_REPLY_CREATE_GRAPHICS_PIPELINES = 24,  // reply of vkCreateGraphicsPipelines
    ALR_VK_GEN_REPLY_CREATE_COMPUTE_PIPELINES = 25,  // reply of vkCreateComputePipelines
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
// the VkCommandPoolCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_command_pool_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool, uint32_t flags, uint32_t queueFamilyIndex) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_COMMAND_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, queueFamilyIndex);
}

// Encoder for vkDestroyCommandPool (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_command_pool(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_COMMAND_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
}

// Encoder for vkAllocateMemory (same-process arena). Ships the device + virtual memory id
// + allocationSize + memoryTypeIndex; append the pNext chain after.
static inline void alr_vk_enc_gen_allocate_memory_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vmem,
                          uint64_t allocation_size, uint32_t memory_type_index) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_ALLOCATE_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmem);
    alr_vk_enc_u64(e, allocation_size);
    alr_vk_enc_u32(e, memory_type_index);
}

// Encoder for vkMapMemory (arena path: round-trips once to fetch the arena offset).
static inline void alr_vk_enc_gen_map_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vmemory,
                          uint64_t offset, uint64_t size) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_MAP_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, offset);
    alr_vk_enc_u64(e, size);
}

// Encoder for vkUnmapMemory (arena path: a coherent-flush marker, no driver unmap).
static inline void alr_vk_enc_gen_unmap_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vmemory) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_UNMAP_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmemory);
}

// Encoder for vkFlushMappedMemoryRanges (arena slabs are HOST_COHERENT; a marker for ordering).
// Ships each range's (vmemory, offset, size) so a future non-coherent arena
// could honor it; the count is bounded by the host decoder.
static inline void alr_vk_enc_gen_flush_mapped_memory_ranges_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t range_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_FLUSH_MAPPED_MEMORY_RANGES);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, range_count);
}
static inline void alr_vk_enc_gen_flush_mapped_memory_ranges_range(AlrVkEncoder *e, uint32_t vmemory,
                          uint64_t offset, uint64_t size) {
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, offset);
    alr_vk_enc_u64(e, size);
}

// Encoder for vkFreeMemory (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_free_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vmem) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_FREE_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vmem);
}

// Encoder for vkCreateBuffer. Ships the device + the guest's virtual VkBuffer id +
// the VkBufferCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_buffer_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf, uint32_t flags, uint64_t size, uint32_t usage, uint32_t sharingMode) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_BUFFER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u64(e, size);
    alr_vk_enc_u32(e, usage);
    alr_vk_enc_u32(e, sharingMode);
}

// Encoder for vkDestroyBuffer (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_buffer(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_BUFFER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
}

// Encoder for vkGetBufferMemoryRequirements (round-trips: the host returns VkMemoryRequirements).
static inline void alr_vk_enc_gen_get_buffer_memory_requirements(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_GET_BUFFER_MEMORY_REQUIREMENTS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
}

// Encoder for vkBindBufferMemory (binds the real handle to real memory at offset).
static inline void alr_vk_enc_gen_bind_buffer_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vbuf,
                          uint32_t vmemory, uint64_t memory_offset) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_BIND_BUFFER_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vbuf);
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, memory_offset);
}

// Encoder for vkCreateImage. Ships the device + the guest's virtual VkImage id +
// the VkImageCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_image_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg, uint32_t flags, uint32_t imageType, uint32_t format, uint32_t extent_width, uint32_t extent_height, uint32_t extent_depth, uint32_t mipLevels, uint32_t arrayLayers, uint32_t samples, uint32_t tiling, uint32_t usage, uint32_t sharingMode, uint32_t initialLayout) {
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
static inline void alr_vk_enc_gen_destroy_image(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_IMAGE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
}

// Encoder for vkGetImageMemoryRequirements (round-trips: the host returns VkMemoryRequirements).
static inline void alr_vk_enc_gen_get_image_memory_requirements(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_GET_IMAGE_MEMORY_REQUIREMENTS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
}

// Encoder for vkBindImageMemory (binds the real handle to real memory at offset).
static inline void alr_vk_enc_gen_bind_image_memory(AlrVkEncoder *e, uint32_t vdev, uint32_t vimg,
                          uint32_t vmemory, uint64_t memory_offset) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_BIND_IMAGE_MEMORY);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vimg);
    alr_vk_enc_u32(e, vmemory);
    alr_vk_enc_u64(e, memory_offset);
}

// Encoder for vkCreateImageView. Ships the device + the guest's virtual VkImageView id +
// the VkImageViewCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_image_view_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vview, uint32_t flags, uint32_t image, uint32_t viewType, uint32_t format, uint32_t components_r, uint32_t components_g, uint32_t components_b, uint32_t components_a, uint32_t subresourceRange_aspectMask, uint32_t subresourceRange_baseMipLevel, uint32_t subresourceRange_levelCount, uint32_t subresourceRange_baseArrayLayer, uint32_t subresourceRange_layerCount) {
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
static inline void alr_vk_enc_gen_destroy_image_view(AlrVkEncoder *e, uint32_t vdev, uint32_t vview) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_IMAGE_VIEW);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vview);
}

// Encoder for vkCreateShaderModule. Ships the device + the guest's virtual VkShaderModule id +
// the VkShaderModuleCreateInfo POD prefix + a trailing blob. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_shader_module_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vshmod, uint32_t flags, const void *blob_data, uint32_t blob_len) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_SHADER_MODULE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vshmod);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_blob(e, blob_data, blob_len);
}

// Encoder for vkDestroyShaderModule (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_shader_module(AlrVkEncoder *e, uint32_t vdev, uint32_t vshmod) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_SHADER_MODULE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vshmod);
}

// Encoder for vkCreatePipelineCache. Ships the device + the guest's virtual VkPipelineCache id +
// the VkPipelineCacheCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_pipeline_cache_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpcache, uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_PIPELINE_CACHE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpcache);
    alr_vk_enc_u32(e, flags);
}

// Encoder for vkDestroyPipelineCache (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_pipeline_cache(AlrVkEncoder *e, uint32_t vdev, uint32_t vpcache) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_PIPELINE_CACHE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpcache);
}

// Encoder for vkCreateSampler. Ships the device + the guest's virtual VkSampler id +
// the VkSamplerCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_sampler_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vsamp, uint32_t flags, uint32_t magFilter, uint32_t minFilter, uint32_t mipmapMode, uint32_t addressModeU, uint32_t addressModeV, uint32_t addressModeW, float mipLodBias, uint32_t anisotropyEnable, float maxAnisotropy, uint32_t compareEnable, uint32_t compareOp, float minLod, float maxLod, uint32_t borderColor, uint32_t unnormalizedCoordinates) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_SAMPLER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vsamp);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, magFilter);
    alr_vk_enc_u32(e, minFilter);
    alr_vk_enc_u32(e, mipmapMode);
    alr_vk_enc_u32(e, addressModeU);
    alr_vk_enc_u32(e, addressModeV);
    alr_vk_enc_u32(e, addressModeW);
    alr_vk_enc_f32(e, mipLodBias);
    alr_vk_enc_u32(e, anisotropyEnable);
    alr_vk_enc_f32(e, maxAnisotropy);
    alr_vk_enc_u32(e, compareEnable);
    alr_vk_enc_u32(e, compareOp);
    alr_vk_enc_f32(e, minLod);
    alr_vk_enc_f32(e, maxLod);
    alr_vk_enc_u32(e, borderColor);
    alr_vk_enc_u32(e, unnormalizedCoordinates);
}

// Encoder for vkDestroySampler (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_sampler(AlrVkEncoder *e, uint32_t vdev, uint32_t vsamp) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_SAMPLER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vsamp);
}

// Encoder for vkCreateFence. Ships the device + the guest's virtual VkFence id +
// the VkFenceCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_fence_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vfence, uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_FENCE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vfence);
    alr_vk_enc_u32(e, flags);
}

// Encoder for vkDestroyFence (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_fence(AlrVkEncoder *e, uint32_t vdev, uint32_t vfence) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_FENCE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vfence);
}

// Encoder for vkCreateSemaphore. Ships the device + the guest's virtual VkSemaphore id +
// the VkSemaphoreCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_semaphore_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vsem, uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_SEMAPHORE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vsem);
    alr_vk_enc_u32(e, flags);
}

// Encoder for vkDestroySemaphore (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_semaphore(AlrVkEncoder *e, uint32_t vdev, uint32_t vsem) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_SEMAPHORE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vsem);
}

// Encoder for vkCreateEvent. Ships the device + the guest's virtual VkEvent id +
// the VkEventCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_event_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vevent, uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_EVENT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vevent);
    alr_vk_enc_u32(e, flags);
}

// Encoder for vkDestroyEvent (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_event(AlrVkEncoder *e, uint32_t vdev, uint32_t vevent) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_EVENT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vevent);
}

// Encoder for vkCreateQueryPool. Ships the device + the guest's virtual VkQueryPool id +
// the VkQueryPoolCreateInfo POD prefix. Append the allowlisted
// pNext chain after via alr_vk_gen_pnext_count/alr_vk_gen_pnext (CREATE_DEVICE2's
// feature-chain shape).
static inline void alr_vk_enc_gen_create_query_pool_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vqpool, uint32_t flags, uint32_t queryType, uint32_t queryCount, uint32_t pipelineStatistics) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_QUERY_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vqpool);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, queryType);
    alr_vk_enc_u32(e, queryCount);
    alr_vk_enc_u32(e, pipelineStatistics);
}

// Encoder for vkDestroyQueryPool (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_query_pool(AlrVkEncoder *e, uint32_t vdev, uint32_t vqpool) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_QUERY_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vqpool);
}

// Encoder for vkCreateDescriptorSetLayout (VkDescriptorSetLayoutCreateInfo with array members). _begin ships the scalar POD
// prefix; then per array call _<array>_count + _<array>_elem; then the pNext chain.
static inline void alr_vk_enc_gen_create_descriptor_set_layout_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vdsl, uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_DESCRIPTOR_SET_LAYOUT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vdsl);
    alr_vk_enc_u32(e, flags);
}
static inline void alr_vk_enc_gen_create_descriptor_set_layout_bindings_count(AlrVkEncoder *e, uint32_t count) {
    alr_vk_enc_u32(e, count);
}
static inline void alr_vk_enc_gen_create_descriptor_set_layout_bindings_elem(AlrVkEncoder *e, uint32_t binding, uint32_t descriptorType, uint32_t descriptorCount, uint32_t stageFlags) {
    alr_vk_enc_u32(e, binding);
    alr_vk_enc_u32(e, descriptorType);
    alr_vk_enc_u32(e, descriptorCount);
    alr_vk_enc_u32(e, stageFlags);
}

// Encoder for vkDestroyDescriptorSetLayout (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_descriptor_set_layout(AlrVkEncoder *e, uint32_t vdev, uint32_t vdsl) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_SET_LAYOUT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vdsl);
}

// Encoder for vkCreatePipelineLayout (VkPipelineLayoutCreateInfo with array members). _begin ships the scalar POD
// prefix; then per array call _<array>_count + _<array>_elem; then the pNext chain.
static inline void alr_vk_enc_gen_create_pipeline_layout_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vplayout, uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_PIPELINE_LAYOUT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vplayout);
    alr_vk_enc_u32(e, flags);
}
static inline void alr_vk_enc_gen_create_pipeline_layout_setLayouts_count(AlrVkEncoder *e, uint32_t count) {
    alr_vk_enc_u32(e, count);
}
static inline void alr_vk_enc_gen_create_pipeline_layout_setLayouts_elem(AlrVkEncoder *e, uint32_t self) {
    alr_vk_enc_u32(e, self);
}
static inline void alr_vk_enc_gen_create_pipeline_layout_pushConstantRanges_count(AlrVkEncoder *e, uint32_t count) {
    alr_vk_enc_u32(e, count);
}
static inline void alr_vk_enc_gen_create_pipeline_layout_pushConstantRanges_elem(AlrVkEncoder *e, uint32_t stageFlags, uint32_t offset, uint32_t size) {
    alr_vk_enc_u32(e, stageFlags);
    alr_vk_enc_u32(e, offset);
    alr_vk_enc_u32(e, size);
}

// Encoder for vkDestroyPipelineLayout (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_pipeline_layout(AlrVkEncoder *e, uint32_t vdev, uint32_t vplayout) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_PIPELINE_LAYOUT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vplayout);
}

// Encoder for vkCreateDescriptorPool (VkDescriptorPoolCreateInfo with array members). _begin ships the scalar POD
// prefix; then per array call _<array>_count + _<array>_elem; then the pNext chain.
static inline void alr_vk_enc_gen_create_descriptor_pool_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vdpool, uint32_t flags, uint32_t maxSets) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_DESCRIPTOR_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vdpool);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, maxSets);
}
static inline void alr_vk_enc_gen_create_descriptor_pool_poolSizes_count(AlrVkEncoder *e, uint32_t count) {
    alr_vk_enc_u32(e, count);
}
static inline void alr_vk_enc_gen_create_descriptor_pool_poolSizes_elem(AlrVkEncoder *e, uint32_t type, uint32_t descriptorCount) {
    alr_vk_enc_u32(e, type);
    alr_vk_enc_u32(e, descriptorCount);
}

// Encoder for vkDestroyDescriptorPool (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_descriptor_pool(AlrVkEncoder *e, uint32_t vdev, uint32_t vdpool) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_DESCRIPTOR_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vdpool);
}

// Encoder for vkAllocateDescriptorSets. Ships the device + descriptor pool (virtual) + the set count;
// then per set call _layout (the set's layout virtual handle) and _vset (the guest's
// virtual id for that set). The host allocates the real sets + returns the result.
static inline void alr_vk_enc_gen_allocate_descriptor_sets_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool,
                          uint32_t set_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_ALLOCATE_DESCRIPTOR_SETS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
    alr_vk_enc_u32(e, set_count);
}
static inline void alr_vk_enc_gen_allocate_descriptor_sets_set(AlrVkEncoder *e, uint32_t vlayout, uint32_t vset) {
    alr_vk_enc_u32(e, vlayout);
    alr_vk_enc_u32(e, vset);
}

// Encoder for vkFreeDescriptorSets. Ships the device + the pool + the set count + each set's
// virtual id; the host frees the real sets back to the real pool (no reply).
static inline void alr_vk_enc_gen_free_descriptor_sets_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpool,
                          uint32_t set_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_FREE_DESCRIPTOR_SETS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
    alr_vk_enc_u32(e, set_count);
}
static inline void alr_vk_enc_gen_free_descriptor_sets_set(AlrVkEncoder *e, uint32_t vset) {
    alr_vk_enc_u32(e, vset);
}

// Encoder for vkUpdateDescriptorSets. _begin ships device + writeCount; per write call _write
// (dst set + binding + arrayElement + descriptorType + descriptorCount) then, per
// descriptor, _buffer_info OR _image_info matching the descriptor type. No reply.
static inline void alr_vk_enc_gen_update_descriptor_sets_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t write_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_UPDATE_DESCRIPTOR_SETS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, write_count);
}
static inline void alr_vk_enc_gen_update_descriptor_sets_write(AlrVkEncoder *e, uint32_t vdstset, uint32_t binding,
                          uint32_t array_element, uint32_t descriptor_type,
                          uint32_t descriptor_count) {
    alr_vk_enc_u32(e, vdstset);
    alr_vk_enc_u32(e, binding);
    alr_vk_enc_u32(e, array_element);
    alr_vk_enc_u32(e, descriptor_type);
    alr_vk_enc_u32(e, descriptor_count);
}
static inline void alr_vk_enc_gen_update_descriptor_sets_buffer_info(AlrVkEncoder *e, uint32_t vbuffer,
                          uint64_t offset, uint64_t range) {
    alr_vk_enc_u32(e, vbuffer);
    alr_vk_enc_u64(e, offset);
    alr_vk_enc_u64(e, range);
}
static inline void alr_vk_enc_gen_update_descriptor_sets_image_info(AlrVkEncoder *e, uint32_t vsampler,
                          uint32_t vimageview, uint32_t image_layout) {
    alr_vk_enc_u32(e, vsampler);
    alr_vk_enc_u32(e, vimageview);
    alr_vk_enc_u32(e, image_layout);
}

// Encoder for vkCreateRenderPass (DEDICATED: nested subpasses). _begin ships flags; then the
// attachments array (_attachment), the subpasses array (each _subpass_begin + its
// _ref / _preserve elements), and the dependencies array (_dependency); then pNext.
static inline void alr_vk_enc_gen_create_render_pass_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vrpass,
                          uint32_t flags) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_RENDER_PASS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vrpass);
    alr_vk_enc_u32(e, flags);
}
static inline void alr_vk_enc_gen_create_render_pass_attachment_count(AlrVkEncoder *e, uint32_t n) { alr_vk_enc_u32(e, n); }
static inline void alr_vk_enc_gen_create_render_pass_attachment(AlrVkEncoder *e, uint32_t flags, uint32_t format,
                          uint32_t samples, uint32_t loadOp, uint32_t storeOp,
                          uint32_t stencilLoadOp, uint32_t stencilStoreOp,
                          uint32_t initialLayout, uint32_t finalLayout) {
    alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, format); alr_vk_enc_u32(e, samples);
    alr_vk_enc_u32(e, loadOp); alr_vk_enc_u32(e, storeOp);
    alr_vk_enc_u32(e, stencilLoadOp); alr_vk_enc_u32(e, stencilStoreOp);
    alr_vk_enc_u32(e, initialLayout); alr_vk_enc_u32(e, finalLayout);
}
static inline void alr_vk_enc_gen_create_render_pass_subpass_count(AlrVkEncoder *e, uint32_t n) { alr_vk_enc_u32(e, n); }
// A subpass: scalar prefix + the 4 reference-array counts + a has-depth flag, then
// the caller appends input refs, color refs, resolve refs (if any), the depth ref
// (if any), and the preserve indices, in that fixed order.
static inline void alr_vk_enc_gen_create_render_pass_subpass_begin(AlrVkEncoder *e, uint32_t flags,
                          uint32_t pipelineBindPoint, uint32_t inputCount,
                          uint32_t colorCount, uint32_t resolveCount,
                          uint32_t hasDepth, uint32_t preserveCount) {
    alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, pipelineBindPoint);
    alr_vk_enc_u32(e, inputCount); alr_vk_enc_u32(e, colorCount);
    alr_vk_enc_u32(e, resolveCount); alr_vk_enc_u32(e, hasDepth);
    alr_vk_enc_u32(e, preserveCount);
}
static inline void alr_vk_enc_gen_create_render_pass_ref(AlrVkEncoder *e, uint32_t attachment, uint32_t layout) {
    alr_vk_enc_u32(e, attachment); alr_vk_enc_u32(e, layout);
}
static inline void alr_vk_enc_gen_create_render_pass_preserve(AlrVkEncoder *e, uint32_t attachment) { alr_vk_enc_u32(e, attachment); }
static inline void alr_vk_enc_gen_create_render_pass_dependency_count(AlrVkEncoder *e, uint32_t n) { alr_vk_enc_u32(e, n); }
static inline void alr_vk_enc_gen_create_render_pass_dependency(AlrVkEncoder *e, uint32_t srcSubpass,
                          uint32_t dstSubpass, uint32_t srcStageMask,
                          uint32_t dstStageMask, uint32_t srcAccessMask,
                          uint32_t dstAccessMask, uint32_t dependencyFlags) {
    alr_vk_enc_u32(e, srcSubpass); alr_vk_enc_u32(e, dstSubpass);
    alr_vk_enc_u32(e, srcStageMask); alr_vk_enc_u32(e, dstStageMask);
    alr_vk_enc_u32(e, srcAccessMask); alr_vk_enc_u32(e, dstAccessMask);
    alr_vk_enc_u32(e, dependencyFlags);
}

// Encoder for vkDestroyRenderPass (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_render_pass(AlrVkEncoder *e, uint32_t vdev, uint32_t vrpass) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_RENDER_PASS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vrpass);
}

// Encoder for vkCreateFramebuffer (VkFramebufferCreateInfo with array members). _begin ships the scalar POD
// prefix; then per array call _<array>_count + _<array>_elem; then the pNext chain.
static inline void alr_vk_enc_gen_create_framebuffer_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vfb, uint32_t flags, uint32_t renderPass, uint32_t width, uint32_t height, uint32_t layers) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_FRAMEBUFFER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vfb);
    alr_vk_enc_u32(e, flags);
    alr_vk_enc_u32(e, renderPass);
    alr_vk_enc_u32(e, width);
    alr_vk_enc_u32(e, height);
    alr_vk_enc_u32(e, layers);
}
static inline void alr_vk_enc_gen_create_framebuffer_attachments_count(AlrVkEncoder *e, uint32_t count) {
    alr_vk_enc_u32(e, count);
}
static inline void alr_vk_enc_gen_create_framebuffer_attachments_elem(AlrVkEncoder *e, uint32_t self) {
    alr_vk_enc_u32(e, self);
}

// Encoder for vkDestroyFramebuffer (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_framebuffer(AlrVkEncoder *e, uint32_t vdev, uint32_t vfb) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_FRAMEBUFFER);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vfb);
}

// Encoder for vkCreateGraphicsPipelines (DEDICATED: deep nested pipeline state). _begin ships the device,
// the pipeline-cache virtual id (0 == VK_NULL_HANDLE), and the pipeline count; then per
// pipeline _pipeline (flags + layout/renderPass/subpass/basePipeline handles + the stage
// count), per stage _stage (+ optional _stage_spec_*), and the fixed-function sub-states.
static inline void alr_vk_enc_gen_create_graphics_pipelines_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpcache,
                          uint32_t pipeline_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_GRAPHICS_PIPELINES);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpcache);
    alr_vk_enc_u32(e, pipeline_count);
}
// One pipeline header: the guest's virtual id for THIS pipeline (vpipe; the host maps it
// to the real Mali pipeline + registers it so vkCmdBindPipeline can translate), the create
// flags, the layout / renderPass / basePipeline HANDLES (virtual ids; 0 == VK_NULL_HANDLE),
// the subpass index, the basePipelineIndex, and the stage count. (renderPass/subpass are 0
// for compute.)
static inline void alr_vk_enc_gen_create_graphics_pipelines_pipeline(AlrVkEncoder *e, uint32_t vpipe, uint32_t flags,
                          uint32_t vlayout, uint32_t vrenderpass, uint32_t subpass,
                          uint32_t vbase, int32_t base_index, uint32_t stage_count) {
    alr_vk_enc_u32(e, vpipe); alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, vlayout);
    alr_vk_enc_u32(e, vrenderpass); alr_vk_enc_u32(e, subpass); alr_vk_enc_u32(e, vbase);
    alr_vk_enc_i32(e, base_index); alr_vk_enc_u32(e, stage_count);
}
// One shader stage: the stage bit, the shader-MODULE handle (virtual id), the
// entry-point name as a length-prefixed blob, and a spec-present flag. If present, the
// caller then appends _stage_spec_begin + per-entry _stage_spec_entry + _stage_spec_data.
static inline void alr_vk_enc_gen_create_graphics_pipelines_stage(AlrVkEncoder *e, uint32_t stage, uint32_t vmodule,
                          const void *name, uint32_t name_len, uint32_t spec_present) {
    alr_vk_enc_u32(e, stage); alr_vk_enc_u32(e, vmodule);
    alr_vk_enc_blob(e, name, name_len);
    alr_vk_enc_u32(e, spec_present);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_stage_spec_begin(AlrVkEncoder *e, uint32_t map_entry_count,
                          uint32_t data_len) {
    alr_vk_enc_u32(e, map_entry_count); alr_vk_enc_u32(e, data_len);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_stage_spec_entry(AlrVkEncoder *e, uint32_t constantID,
                          uint32_t offset, uint32_t size) {
    alr_vk_enc_u32(e, constantID); alr_vk_enc_u32(e, offset); alr_vk_enc_u32(e, size);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_stage_spec_data(AlrVkEncoder *e, const void *data, uint32_t len) {
    alr_vk_enc_blob(e, data, len);
}
// ---- Fixed-function sub-states (GRAPHICS). Each _<state>(present) ships a u32 presence
// flag; when present the caller then appends that state's fields/elements in order. ----
static inline void alr_vk_enc_gen_create_graphics_pipelines_vertex_input(AlrVkEncoder *e, uint32_t present,
                          uint32_t binding_count, uint32_t attr_count) {
    alr_vk_enc_u32(e, present);
    if (present) { alr_vk_enc_u32(e, binding_count); alr_vk_enc_u32(e, attr_count); }
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_vertex_binding(AlrVkEncoder *e, uint32_t binding,
                          uint32_t stride, uint32_t inputRate) {
    alr_vk_enc_u32(e, binding); alr_vk_enc_u32(e, stride); alr_vk_enc_u32(e, inputRate);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_vertex_attr(AlrVkEncoder *e, uint32_t location,
                          uint32_t binding, uint32_t format, uint32_t offset) {
    alr_vk_enc_u32(e, location); alr_vk_enc_u32(e, binding);
    alr_vk_enc_u32(e, format); alr_vk_enc_u32(e, offset);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_input_assembly(AlrVkEncoder *e, uint32_t present,
                          uint32_t topology, uint32_t primitiveRestartEnable) {
    alr_vk_enc_u32(e, present);
    if (present) { alr_vk_enc_u32(e, topology); alr_vk_enc_u32(e, primitiveRestartEnable); }
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_tessellation(AlrVkEncoder *e, uint32_t present,
                          uint32_t patchControlPoints) {
    alr_vk_enc_u32(e, present);
    if (present) alr_vk_enc_u32(e, patchControlPoints);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_viewport(AlrVkEncoder *e, uint32_t present,
                          uint32_t viewport_count, uint32_t scissor_count) {
    alr_vk_enc_u32(e, present);
    if (present) { alr_vk_enc_u32(e, viewport_count); alr_vk_enc_u32(e, scissor_count); }
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_viewport_elem(AlrVkEncoder *e, float x, float y, float w,
                          float h, float minDepth, float maxDepth) {
    alr_vk_enc_f32(e, x); alr_vk_enc_f32(e, y); alr_vk_enc_f32(e, w);
    alr_vk_enc_f32(e, h); alr_vk_enc_f32(e, minDepth); alr_vk_enc_f32(e, maxDepth);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_scissor_elem(AlrVkEncoder *e, int32_t offX, int32_t offY,
                          uint32_t extW, uint32_t extH) {
    alr_vk_enc_i32(e, offX); alr_vk_enc_i32(e, offY);
    alr_vk_enc_u32(e, extW); alr_vk_enc_u32(e, extH);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_rasterization(AlrVkEncoder *e, uint32_t present,
                          uint32_t depthClampEnable, uint32_t rasterizerDiscardEnable,
                          uint32_t polygonMode, uint32_t cullMode, uint32_t frontFace,
                          uint32_t depthBiasEnable, float depthBiasConstantFactor,
                          float depthBiasClamp, float depthBiasSlopeFactor,
                          float lineWidth) {
    alr_vk_enc_u32(e, present);
    if (!present) return;
    alr_vk_enc_u32(e, depthClampEnable); alr_vk_enc_u32(e, rasterizerDiscardEnable);
    alr_vk_enc_u32(e, polygonMode); alr_vk_enc_u32(e, cullMode); alr_vk_enc_u32(e, frontFace);
    alr_vk_enc_u32(e, depthBiasEnable); alr_vk_enc_f32(e, depthBiasConstantFactor);
    alr_vk_enc_f32(e, depthBiasClamp); alr_vk_enc_f32(e, depthBiasSlopeFactor);
    alr_vk_enc_f32(e, lineWidth);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_multisample(AlrVkEncoder *e, uint32_t present,
                          uint32_t rasterizationSamples, uint32_t sampleShadingEnable,
                          float minSampleShading, uint32_t sampleMaskWordCount,
                          uint32_t alphaToCoverageEnable, uint32_t alphaToOneEnable) {
    alr_vk_enc_u32(e, present);
    if (!present) return;
    alr_vk_enc_u32(e, rasterizationSamples); alr_vk_enc_u32(e, sampleShadingEnable);
    alr_vk_enc_f32(e, minSampleShading); alr_vk_enc_u32(e, sampleMaskWordCount);
    alr_vk_enc_u32(e, alphaToCoverageEnable); alr_vk_enc_u32(e, alphaToOneEnable);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_sample_mask(AlrVkEncoder *e, uint32_t word) { alr_vk_enc_u32(e, word); }
static inline void alr_vk_enc_gen_create_graphics_pipelines_depth_stencil(AlrVkEncoder *e, uint32_t present,
                          uint32_t depthTestEnable, uint32_t depthWriteEnable,
                          uint32_t depthCompareOp, uint32_t depthBoundsTestEnable,
                          uint32_t stencilTestEnable, float minDepthBounds,
                          float maxDepthBounds) {
    alr_vk_enc_u32(e, present);
    if (!present) return;
    alr_vk_enc_u32(e, depthTestEnable); alr_vk_enc_u32(e, depthWriteEnable);
    alr_vk_enc_u32(e, depthCompareOp); alr_vk_enc_u32(e, depthBoundsTestEnable);
    alr_vk_enc_u32(e, stencilTestEnable);
    alr_vk_enc_f32(e, minDepthBounds); alr_vk_enc_f32(e, maxDepthBounds);
}
// A VkStencilOpState (front/back): failOp,passOp,depthFailOp,compareOp (4×u32) +
// compareMask,writeMask,reference (3×u32). Called twice per depth-stencil (front, back).
static inline void alr_vk_enc_gen_create_graphics_pipelines_stencil_op(AlrVkEncoder *e, uint32_t failOp, uint32_t passOp,
                          uint32_t depthFailOp, uint32_t compareOp, uint32_t compareMask,
                          uint32_t writeMask, uint32_t reference) {
    alr_vk_enc_u32(e, failOp); alr_vk_enc_u32(e, passOp); alr_vk_enc_u32(e, depthFailOp);
    alr_vk_enc_u32(e, compareOp); alr_vk_enc_u32(e, compareMask);
    alr_vk_enc_u32(e, writeMask); alr_vk_enc_u32(e, reference);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_color_blend(AlrVkEncoder *e, uint32_t present,
                          uint32_t logicOpEnable, uint32_t logicOp,
                          uint32_t attachment_count, float bc0, float bc1, float bc2,
                          float bc3) {
    alr_vk_enc_u32(e, present);
    if (!present) return;
    alr_vk_enc_u32(e, logicOpEnable); alr_vk_enc_u32(e, logicOp);
    alr_vk_enc_u32(e, attachment_count);
    alr_vk_enc_f32(e, bc0); alr_vk_enc_f32(e, bc1); alr_vk_enc_f32(e, bc2); alr_vk_enc_f32(e, bc3);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_blend_attachment(AlrVkEncoder *e, uint32_t blendEnable,
                          uint32_t srcColorBlendFactor, uint32_t dstColorBlendFactor,
                          uint32_t colorBlendOp, uint32_t srcAlphaBlendFactor,
                          uint32_t dstAlphaBlendFactor, uint32_t alphaBlendOp,
                          uint32_t colorWriteMask) {
    alr_vk_enc_u32(e, blendEnable); alr_vk_enc_u32(e, srcColorBlendFactor);
    alr_vk_enc_u32(e, dstColorBlendFactor); alr_vk_enc_u32(e, colorBlendOp);
    alr_vk_enc_u32(e, srcAlphaBlendFactor); alr_vk_enc_u32(e, dstAlphaBlendFactor);
    alr_vk_enc_u32(e, alphaBlendOp); alr_vk_enc_u32(e, colorWriteMask);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_dynamic_state(AlrVkEncoder *e, uint32_t present,
                          uint32_t dynamic_state_count) {
    alr_vk_enc_u32(e, present);
    if (present) alr_vk_enc_u32(e, dynamic_state_count);
}
static inline void alr_vk_enc_gen_create_graphics_pipelines_dynamic_elem(AlrVkEncoder *e, uint32_t state) { alr_vk_enc_u32(e, state); }

// Encoder for vkCreateComputePipelines (DEDICATED: deep nested pipeline state). _begin ships the device,
// the pipeline-cache virtual id (0 == VK_NULL_HANDLE), and the pipeline count; then per
// pipeline _pipeline (flags + layout/renderPass/subpass/basePipeline handles + the stage
// count), per stage _stage (+ optional _stage_spec_*), and — compute has no fixed-function.
static inline void alr_vk_enc_gen_create_compute_pipelines_begin(AlrVkEncoder *e, uint32_t vdev, uint32_t vpcache,
                          uint32_t pipeline_count) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_CREATE_COMPUTE_PIPELINES);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpcache);
    alr_vk_enc_u32(e, pipeline_count);
}
// One pipeline header: the guest's virtual id for THIS pipeline (vpipe; the host maps it
// to the real Mali pipeline + registers it so vkCmdBindPipeline can translate), the create
// flags, the layout / renderPass / basePipeline HANDLES (virtual ids; 0 == VK_NULL_HANDLE),
// the subpass index, the basePipelineIndex, and the stage count. (renderPass/subpass are 0
// for compute.)
static inline void alr_vk_enc_gen_create_compute_pipelines_pipeline(AlrVkEncoder *e, uint32_t vpipe, uint32_t flags,
                          uint32_t vlayout, uint32_t vrenderpass, uint32_t subpass,
                          uint32_t vbase, int32_t base_index, uint32_t stage_count) {
    alr_vk_enc_u32(e, vpipe); alr_vk_enc_u32(e, flags); alr_vk_enc_u32(e, vlayout);
    alr_vk_enc_u32(e, vrenderpass); alr_vk_enc_u32(e, subpass); alr_vk_enc_u32(e, vbase);
    alr_vk_enc_i32(e, base_index); alr_vk_enc_u32(e, stage_count);
}
// One shader stage: the stage bit, the shader-MODULE handle (virtual id), the
// entry-point name as a length-prefixed blob, and a spec-present flag. If present, the
// caller then appends _stage_spec_begin + per-entry _stage_spec_entry + _stage_spec_data.
static inline void alr_vk_enc_gen_create_compute_pipelines_stage(AlrVkEncoder *e, uint32_t stage, uint32_t vmodule,
                          const void *name, uint32_t name_len, uint32_t spec_present) {
    alr_vk_enc_u32(e, stage); alr_vk_enc_u32(e, vmodule);
    alr_vk_enc_blob(e, name, name_len);
    alr_vk_enc_u32(e, spec_present);
}
static inline void alr_vk_enc_gen_create_compute_pipelines_stage_spec_begin(AlrVkEncoder *e, uint32_t map_entry_count,
                          uint32_t data_len) {
    alr_vk_enc_u32(e, map_entry_count); alr_vk_enc_u32(e, data_len);
}
static inline void alr_vk_enc_gen_create_compute_pipelines_stage_spec_entry(AlrVkEncoder *e, uint32_t constantID,
                          uint32_t offset, uint32_t size) {
    alr_vk_enc_u32(e, constantID); alr_vk_enc_u32(e, offset); alr_vk_enc_u32(e, size);
}
static inline void alr_vk_enc_gen_create_compute_pipelines_stage_spec_data(AlrVkEncoder *e, const void *data, uint32_t len) {
    alr_vk_enc_blob(e, data, len);
}

// Encoder for vkDestroyPipeline (forwards a destroy; no reply).
static inline void alr_vk_enc_gen_destroy_pipeline(AlrVkEncoder *e, uint32_t vdev, uint32_t vpipe) {
    alr_vk_gen_op_begin(e, ALR_VK_GEN_OP_DESTROY_PIPELINE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpipe);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP
