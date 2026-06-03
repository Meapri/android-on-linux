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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_PROTO_HPP
