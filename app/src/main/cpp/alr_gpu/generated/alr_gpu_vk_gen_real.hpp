// alr_gpu_vk_gen_real.hpp — HAND-WRITTEN real-Mali bodies for the generated entrypoints.
//
// The generated host decoder (alr_gpu_vk_gen_decode.hpp, decode_vk_gen_op) is a thin
// dispatch: it reads each op off the wire and, under ALR_VK_DECODE_REAL, calls one
// vk_gen_real_*() from THIS file. The actual Vulkan work — virtual->real handle
// translation via VkGenTables, reconstructing each CreateInfo from the wire POD prefix +
// the allowlisted pNext chain, the same-process arena import for device memory, and the
// real vkCreate*/vkBind*/vkGet* calls on the vendor Mali libvulkan — lives here as
// auditable C++ (NOT generated), so the heavy/subtle Vulkan code is hand-reviewed while
// the mechanical per-entrypoint encode/decode/dispatch stays machine-generated.
//
// Compiled ONLY under ALR_VK_DECODE_REAL (the on-device path; runtime_report.cpp defines
// it). With no Vulkan SDK this file is empty, and the generated decoder's provider seam
// drives the host wire test instead.
//
// Handle model: all five generated handle kinds (VkCommandPool/Buffer/Image/ImageView/
// DeviceMemory) are non-dispatchable. The guest ships a virtual id (u32); VkGenTables maps
// it to the real Mali handle. A create stores the mapping; a destroy drops it; ops that
// take a handle (bind/getReqs/imageView's source image) look it up.

#ifndef ALR_GPU_GENERATED_ALR_GPU_VK_GEN_REAL_HPP
#define ALR_GPU_GENERATED_ALR_GPU_VK_GEN_REAL_HPP

#ifdef ALR_VK_DECODE_REAL

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "alr_gpu/alr_gpu_vk_decode.hpp"          // VkDecodeState
#include "alr_gpu/generated/alr_gpu_vk_arena.hpp"  // the MAP_SHARED arena

namespace alr::gpu {

// Forward decl: the generated decoder defines VkGenTables + gen_tables(st). This file is
// included by the generated decoder AFTER those are declared, so we just use them. To keep
// this header independently includable we re-declare the accessor signature.
struct VkGenTables;
VkGenTables& gen_tables(VkDecodeState& st);

// ---- allowlisted pNext relink (mirrors vk_real_create_device2's chain rebuild). Each
// struct begins with { VkStructureType sType; void* pNext; }; we keep only KNOWN sTypes
// and relink them, so a malformed/unknown sType from the wire can never make the driver
// walk a bogus chain. Returns the chain head (or null), with storage owned by `store`. ----
inline bool vk_gen_pnext_stype_allowed(uint32_t s_type) {
    switch (s_type) {
        // External-memory create-info structs ANGLE chains onto image/buffer creates.
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO:
        case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO:
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO:
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO:
            return true;
        default:
            return false;
    }
}

inline void* vk_gen_relink_pnext(const std::vector<uint32_t>& types,
                                 const std::vector<std::vector<uint8_t>>& bytes,
                                 std::vector<std::vector<uint8_t>>& store) {
    struct Hdr { VkStructureType sType; void* pNext; };
    store.clear();
    store.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size() && i < types.size(); ++i) {
        if (!vk_gen_pnext_stype_allowed(types[i])) continue;
        if (bytes[i].size() < sizeof(Hdr)) continue;
        store.push_back(bytes[i]);
    }
    void* head = nullptr;
    for (size_t i = store.size(); i-- > 0;) {
        Hdr h{};
        std::memcpy(&h, store[i].data(), sizeof(Hdr));
        h.pNext = head;
        std::memcpy(store[i].data(), &h, sizeof(Hdr));
        head = store[i].data();
    }
    return head;
}

// ---- command pool (the create_pool kind: substitute the device's gfx family like the
// hand-written coarse vk_real_create_pool does — the wire's queueFamilyIndex is advisory
// for the bring-up batch; ANGLE uses the graphics family). ----
inline VkResult vk_gen_real_create_command_pool(
    VkDecodeState& st, uint32_t vdev, uint32_t vpool, uint32_t flags,
    uint32_t queueFamilyIndex, const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = flags ? flags : VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    // Honor the requested family if the device has a queue there; else fall back to gfx.
    uint32_t fam = queueFamilyIndex;
    if (dit->second.queue_counts.find(fam) == dit->second.queue_counts.end())
        fam = dit->second.gfx_family;
    pci.queueFamilyIndex = fam;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult r = vkCreateCommandPool(dit->second.dev, &pci, nullptr, &pool);
    if (r == VK_SUCCESS) gen_tables(st).pools[vpool] = pool;
    return r;
}

inline void vk_gen_real_destroy_command_pool(VkDecodeState& st, uint32_t vdev,
                                             uint32_t vpool) {
    auto& t = gen_tables(st);
    auto pit = t.pools.find(vpool);
    auto dit = st.real_dev.find(vdev);
    if (pit != t.pools.end() && dit != st.real_dev.end() && pit->second != VK_NULL_HANDLE)
        vkDestroyCommandPool(dit->second.dev, pit->second, nullptr);
    t.pools.erase(vpool);
}

// ---- device memory (same-process arena). For a HOST_VISIBLE alloc we carve an arena slab
// and import it as the real VkDeviceMemory (zero-copy: a guest write through the arena IS a
// write to this allocation). If the extension/import is unavailable, we fall back to plain
// HOST_VISIBLE driver memory AND still record the arena slab so vkMapMemory returns a guest
// pointer — the staged path then copies arena->driver-memory lazily (documented fallback;
// the offset==NoOffset case means "no arena pointer", and map will fail, signalling staged
// is needed). For the first batch ANGLE's host-visible staging is the target. ----
inline VkResult vk_gen_real_alloc_memory(VkDecodeState& st, uint32_t vdev, uint32_t vmem,
                                         uint64_t allocation_size,
                                         uint32_t memory_type_index, uint64_t& arena_off) {
    arena_off = kAlrVkArenaNoOffset;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkDevice dev = dit->second.dev;
    VkPhysicalDevice phys = dit->second.phys;

    // Is the requested memory type HOST_VISIBLE? Only host-visible allocations go through
    // the arena (the guest only maps host-visible memory; DEVICE_LOCAL is never mapped, so
    // it stays ordinary driver memory with no arena slab).
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const bool host_visible =
        memory_type_index < mp.memoryTypeCount &&
        (mp.memoryTypes[memory_type_index].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    if (host_visible && alr_vk_arena_ready() &&
        alr_vk_arena_host_import_available(dev)) {
        const uint64_t off = alr_vk_arena_alloc(allocation_size);
        if (off != kAlrVkArenaNoOffset) {
            // Pick a host-visible+coherent type importable for this arena pointer (it may
            // differ from the guest's requested index — the guest only needs SOME mappable
            // memory; the buffer/image's memoryTypeBits already came from our get_reqs).
            uint32_t host_type = memory_type_index;
            if (!alr_vk_arena_pick_host_type(phys, dev, off, &host_type))
                host_type = memory_type_index;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkResult r = alr_vk_arena_import_slab(dev, off, allocation_size, host_type, &mem);
            if (r == VK_SUCCESS) {
                gen_tables(st).memory[vmem] = mem;
                arena_off = off;
                return VK_SUCCESS;
            }
            // Import failed: fall through to plain driver memory below (the slab is leaked
            // back to the bump arena — acceptable; arenas are large and per-process).
        }
    }

    // Fallback (DEVICE_LOCAL, or no host-import support): ordinary driver allocation. No
    // arena slab -> vkMapMemory on this memory returns failure (the guest must use a
    // host-visible alloc to map; ANGLE allocates host-visible staging for uploads).
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = allocation_size ? allocation_size : 256;
    mai.memoryTypeIndex = memory_type_index;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = vkAllocateMemory(dev, &mai, nullptr, &mem);
    if (r == VK_SUCCESS) gen_tables(st).memory[vmem] = mem;
    return r;
}

inline void vk_gen_real_free_memory(VkDecodeState& st, uint32_t vdev, uint32_t vmem) {
    auto& t = gen_tables(st);
    auto mit = t.memory.find(vmem);
    auto dit = st.real_dev.find(vdev);
    if (mit != t.memory.end() && dit != st.real_dev.end() && mit->second != VK_NULL_HANDLE)
        vkFreeMemory(dit->second.dev, mit->second, nullptr);
    t.memory.erase(vmem);
    t.mem_arena_off.erase(vmem);
    t.mem_size.erase(vmem);
}

// ---- buffer ----
inline VkResult vk_gen_real_create_buffer(
    VkDecodeState& st, uint32_t vdev, uint32_t vbuf, uint32_t flags, uint64_t size,
    uint32_t usage, uint32_t sharingMode, const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<std::vector<uint8_t>> pnext_store;
    void* head = vk_gen_relink_pnext(pnext_types, pnext_bytes, pnext_store);
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = head;
    bci.flags = flags;
    bci.size = size;
    bci.usage = usage;
    // CONCURRENT would need pQueueFamilyIndices (not on the wire for the first batch); force
    // EXCLUSIVE so the create is well-formed even if the guest passed CONCURRENT (deferred).
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    (void)sharingMode;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(dit->second.dev, &bci, nullptr, &buf);
    if (r == VK_SUCCESS) gen_tables(st).buffers[vbuf] = buf;
    return r;
}

inline void vk_gen_real_destroy_buffer(VkDecodeState& st, uint32_t vdev, uint32_t vbuf) {
    auto& t = gen_tables(st);
    auto bit = t.buffers.find(vbuf);
    auto dit = st.real_dev.find(vdev);
    if (bit != t.buffers.end() && dit != st.real_dev.end() && bit->second != VK_NULL_HANDLE)
        vkDestroyBuffer(dit->second.dev, bit->second, nullptr);
    t.buffers.erase(vbuf);
}

inline VkResult vk_gen_real_get_buffer_reqs(VkDecodeState& st, uint32_t vdev, uint32_t vbuf,
                                            uint64_t& size, uint64_t& align, uint32_t& bits) {
    auto dit = st.real_dev.find(vdev);
    auto bit = gen_tables(st).buffers.find(vbuf);
    if (dit == st.real_dev.end() || bit == gen_tables(st).buffers.end())
        return VK_ERROR_INITIALIZATION_FAILED;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dit->second.dev, bit->second, &req);
    size = req.size;
    align = req.alignment;
    bits = req.memoryTypeBits;
    return VK_SUCCESS;
}

inline VkResult vk_gen_real_bind_buffer_memory(VkDecodeState& st, uint32_t vdev,
                                               uint32_t vbuf, uint32_t vmem, uint64_t off) {
    auto dit = st.real_dev.find(vdev);
    auto bit = gen_tables(st).buffers.find(vbuf);
    auto mit = gen_tables(st).memory.find(vmem);
    if (dit == st.real_dev.end() || bit == gen_tables(st).buffers.end() ||
        mit == gen_tables(st).memory.end())
        return VK_ERROR_INITIALIZATION_FAILED;
    return vkBindBufferMemory(dit->second.dev, bit->second, mit->second, off);
}

// ---- image ----
inline VkResult vk_gen_real_create_image(
    VkDecodeState& st, uint32_t vdev, uint32_t vimg, uint32_t flags, uint32_t imageType,
    uint32_t format, uint32_t extent_width, uint32_t extent_height, uint32_t extent_depth,
    uint32_t mipLevels, uint32_t arrayLayers, uint32_t samples, uint32_t tiling,
    uint32_t usage, uint32_t sharingMode, uint32_t initialLayout,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<std::vector<uint8_t>> pnext_store;
    void* head = vk_gen_relink_pnext(pnext_types, pnext_bytes, pnext_store);
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = head;
    ici.flags = flags;
    ici.imageType = static_cast<VkImageType>(imageType);
    ici.format = static_cast<VkFormat>(format);
    ici.extent = {extent_width ? extent_width : 1, extent_height ? extent_height : 1,
                  extent_depth ? extent_depth : 1};
    ici.mipLevels = mipLevels ? mipLevels : 1;
    ici.arrayLayers = arrayLayers ? arrayLayers : 1;
    ici.samples = samples ? static_cast<VkSampleCountFlagBits>(samples)
                          : VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = static_cast<VkImageTiling>(tiling);
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // CONCURRENT deferred (see buffer)
    (void)sharingMode;
    ici.initialLayout = static_cast<VkImageLayout>(initialLayout);
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(dit->second.dev, &ici, nullptr, &img);
    if (r == VK_SUCCESS) gen_tables(st).images[vimg] = img;
    return r;
}

inline void vk_gen_real_destroy_image(VkDecodeState& st, uint32_t vdev, uint32_t vimg) {
    auto& t = gen_tables(st);
    auto iit = t.images.find(vimg);
    auto dit = st.real_dev.find(vdev);
    if (iit != t.images.end() && dit != st.real_dev.end() && iit->second != VK_NULL_HANDLE)
        vkDestroyImage(dit->second.dev, iit->second, nullptr);
    t.images.erase(vimg);
}

inline VkResult vk_gen_real_get_image_reqs(VkDecodeState& st, uint32_t vdev, uint32_t vimg,
                                           uint64_t& size, uint64_t& align, uint32_t& bits) {
    auto dit = st.real_dev.find(vdev);
    auto iit = gen_tables(st).images.find(vimg);
    if (dit == st.real_dev.end() || iit == gen_tables(st).images.end())
        return VK_ERROR_INITIALIZATION_FAILED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dit->second.dev, iit->second, &req);
    size = req.size;
    align = req.alignment;
    bits = req.memoryTypeBits;
    return VK_SUCCESS;
}

inline VkResult vk_gen_real_bind_image_memory(VkDecodeState& st, uint32_t vdev,
                                              uint32_t vimg, uint32_t vmem, uint64_t off) {
    auto dit = st.real_dev.find(vdev);
    auto iit = gen_tables(st).images.find(vimg);
    auto mit = gen_tables(st).memory.find(vmem);
    if (dit == st.real_dev.end() || iit == gen_tables(st).images.end() ||
        mit == gen_tables(st).memory.end())
        return VK_ERROR_INITIALIZATION_FAILED;
    return vkBindImageMemory(dit->second.dev, iit->second, mit->second, off);
}

// ---- image view (references its source image — a virtual handle on the wire). ----
inline VkResult vk_gen_real_create_image_view(
    VkDecodeState& st, uint32_t vdev, uint32_t vview, uint32_t flags, uint32_t image,
    uint32_t viewType, uint32_t format, uint32_t comp_r, uint32_t comp_g, uint32_t comp_b,
    uint32_t comp_a, uint32_t aspectMask, uint32_t baseMipLevel, uint32_t levelCount,
    uint32_t baseArrayLayer, uint32_t layerCount, const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    // Translate the source image's virtual id -> real VkImage.
    auto iit = gen_tables(st).images.find(image);
    if (iit == gen_tables(st).images.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<std::vector<uint8_t>> pnext_store;
    void* head = vk_gen_relink_pnext(pnext_types, pnext_bytes, pnext_store);
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.pNext = head;
    vci.flags = flags;
    vci.image = iit->second;
    vci.viewType = static_cast<VkImageViewType>(viewType);
    vci.format = static_cast<VkFormat>(format);
    vci.components.r = static_cast<VkComponentSwizzle>(comp_r);
    vci.components.g = static_cast<VkComponentSwizzle>(comp_g);
    vci.components.b = static_cast<VkComponentSwizzle>(comp_b);
    vci.components.a = static_cast<VkComponentSwizzle>(comp_a);
    vci.subresourceRange.aspectMask =
        aspectMask ? aspectMask : VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = baseMipLevel;
    vci.subresourceRange.levelCount = levelCount ? levelCount : 1;
    vci.subresourceRange.baseArrayLayer = baseArrayLayer;
    vci.subresourceRange.layerCount = layerCount ? layerCount : 1;
    VkImageView view = VK_NULL_HANDLE;
    VkResult r = vkCreateImageView(dit->second.dev, &vci, nullptr, &view);
    if (r == VK_SUCCESS) gen_tables(st).views[vview] = view;
    return r;
}

inline void vk_gen_real_destroy_image_view(VkDecodeState& st, uint32_t vdev,
                                           uint32_t vview) {
    auto& t = gen_tables(st);
    auto vit = t.views.find(vview);
    auto dit = st.real_dev.find(vdev);
    if (vit != t.views.end() && dit != st.real_dev.end() && vit->second != VK_NULL_HANDLE)
        vkDestroyImageView(dit->second.dev, vit->second, nullptr);
    t.views.erase(vview);
}

// ===========================================================================
// WAVE A — shader module / pipeline cache / sampler / fence / semaphore / event /
// query pool. These are the create-resource objects ANGLE's RendererVk builds right after
// device creation. All are non-dispatchable handles stored in VkGenTables; the create
// forwards the registry-typed POD prefix (rebuilt from the wire scalars) to real Mali and
// stores the handle, the destroy looks it up + releases it. Only the shader module carries
// a blob (the SPIR-V words); the rest are pure scalar forwards.
// ===========================================================================

// ---- shader module (the SPIR-V blob rides the wire; codeSize/pCode rebuilt from it) ----
inline VkResult vk_gen_real_create_shader_module(
    VkDecodeState& st, uint32_t vdev, uint32_t vshmod, uint32_t flags,
    const uint8_t* spirv, uint32_t spirv_len, const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    // SPIR-V is a stream of 32-bit words; the byte length must be a non-zero multiple of 4.
    // A malformed blob is rejected here so the real driver never parses garbage.
    if (!spirv || spirv_len == 0 || (spirv_len & 3u) != 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.flags = flags;
    sci.codeSize = spirv_len;  // codeSize is in BYTES (Vulkan spec)
    sci.pCode = reinterpret_cast<const uint32_t*>(spirv);
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(dit->second.dev, &sci, nullptr, &mod);
    if (r == VK_SUCCESS) gen_tables(st).shader_modules[vshmod] = mod;
    return r;
}

inline void vk_gen_real_destroy_shader_module(VkDecodeState& st, uint32_t vdev,
                                              uint32_t vshmod) {
    auto& t = gen_tables(st);
    auto it = t.shader_modules.find(vshmod);
    auto dit = st.real_dev.find(vdev);
    if (it != t.shader_modules.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyShaderModule(dit->second.dev, it->second, nullptr);
    t.shader_modules.erase(vshmod);
}

// ---- pipeline cache (empty cache: initialDataSize 0 for the bring-up) ----
inline VkResult vk_gen_real_create_pipeline_cache(
    VkDecodeState& st, uint32_t vdev, uint32_t vpcache, uint32_t flags,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineCacheCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    pci.flags = flags;
    pci.initialDataSize = 0;  // warm-cache (pInitialData) deferred — ANGLE's first is empty
    pci.pInitialData = nullptr;
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineCache(dit->second.dev, &pci, nullptr, &cache);
    if (r == VK_SUCCESS) gen_tables(st).pipeline_caches[vpcache] = cache;
    return r;
}

inline void vk_gen_real_destroy_pipeline_cache(VkDecodeState& st, uint32_t vdev,
                                               uint32_t vpcache) {
    auto& t = gen_tables(st);
    auto it = t.pipeline_caches.find(vpcache);
    auto dit = st.real_dev.find(vdev);
    if (it != t.pipeline_caches.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyPipelineCache(dit->second.dev, it->second, nullptr);
    t.pipeline_caches.erase(vpcache);
}

// ---- sampler (full scalar POD prefix; the f32 fields ride the wire as floats) ----
inline VkResult vk_gen_real_create_sampler(
    VkDecodeState& st, uint32_t vdev, uint32_t vsamp, uint32_t flags, uint32_t magFilter,
    uint32_t minFilter, uint32_t mipmapMode, uint32_t addressModeU, uint32_t addressModeV,
    uint32_t addressModeW, float mipLodBias, uint32_t anisotropyEnable, float maxAnisotropy,
    uint32_t compareEnable, uint32_t compareOp, float minLod, float maxLod,
    uint32_t borderColor, uint32_t unnormalizedCoordinates,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.flags = flags;
    sci.magFilter = static_cast<VkFilter>(magFilter);
    sci.minFilter = static_cast<VkFilter>(minFilter);
    sci.mipmapMode = static_cast<VkSamplerMipmapMode>(mipmapMode);
    sci.addressModeU = static_cast<VkSamplerAddressMode>(addressModeU);
    sci.addressModeV = static_cast<VkSamplerAddressMode>(addressModeV);
    sci.addressModeW = static_cast<VkSamplerAddressMode>(addressModeW);
    sci.mipLodBias = mipLodBias;
    sci.anisotropyEnable = anisotropyEnable ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = maxAnisotropy;
    sci.compareEnable = compareEnable ? VK_TRUE : VK_FALSE;
    sci.compareOp = static_cast<VkCompareOp>(compareOp);
    sci.minLod = minLod;
    sci.maxLod = maxLod;
    sci.borderColor = static_cast<VkBorderColor>(borderColor);
    sci.unnormalizedCoordinates = unnormalizedCoordinates ? VK_TRUE : VK_FALSE;
    VkSampler samp = VK_NULL_HANDLE;
    VkResult r = vkCreateSampler(dit->second.dev, &sci, nullptr, &samp);
    if (r == VK_SUCCESS) gen_tables(st).samplers[vsamp] = samp;
    return r;
}

inline void vk_gen_real_destroy_sampler(VkDecodeState& st, uint32_t vdev, uint32_t vsamp) {
    auto& t = gen_tables(st);
    auto it = t.samplers.find(vsamp);
    auto dit = st.real_dev.find(vdev);
    if (it != t.samplers.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroySampler(dit->second.dev, it->second, nullptr);
    t.samplers.erase(vsamp);
}

// ---- fence (flags only; VK_FENCE_CREATE_SIGNALED_BIT is the one meaningful flag) ----
inline VkResult vk_gen_real_create_fence(
    VkDecodeState& st, uint32_t vdev, uint32_t vfence, uint32_t flags,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = flags;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r = vkCreateFence(dit->second.dev, &fci, nullptr, &fence);
    if (r == VK_SUCCESS) gen_tables(st).fences[vfence] = fence;
    return r;
}

inline void vk_gen_real_destroy_fence(VkDecodeState& st, uint32_t vdev, uint32_t vfence) {
    auto& t = gen_tables(st);
    auto it = t.fences.find(vfence);
    auto dit = st.real_dev.find(vdev);
    if (it != t.fences.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyFence(dit->second.dev, it->second, nullptr);
    t.fences.erase(vfence);
}

// ---- semaphore (flags only; a timeline semaphore's type pNext is deferred) ----
inline VkResult vk_gen_real_create_semaphore(
    VkDecodeState& st, uint32_t vdev, uint32_t vsem, uint32_t flags,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.flags = flags;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult r = vkCreateSemaphore(dit->second.dev, &sci, nullptr, &sem);
    if (r == VK_SUCCESS) gen_tables(st).semaphores[vsem] = sem;
    return r;
}

inline void vk_gen_real_destroy_semaphore(VkDecodeState& st, uint32_t vdev, uint32_t vsem) {
    auto& t = gen_tables(st);
    auto it = t.semaphores.find(vsem);
    auto dit = st.real_dev.find(vdev);
    if (it != t.semaphores.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroySemaphore(dit->second.dev, it->second, nullptr);
    t.semaphores.erase(vsem);
}

// ---- event (flags only) ----
inline VkResult vk_gen_real_create_event(
    VkDecodeState& st, uint32_t vdev, uint32_t vevent, uint32_t flags,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkEventCreateInfo eci{};
    eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    eci.flags = flags;
    VkEvent ev = VK_NULL_HANDLE;
    VkResult r = vkCreateEvent(dit->second.dev, &eci, nullptr, &ev);
    if (r == VK_SUCCESS) gen_tables(st).events[vevent] = ev;
    return r;
}

inline void vk_gen_real_destroy_event(VkDecodeState& st, uint32_t vdev, uint32_t vevent) {
    auto& t = gen_tables(st);
    auto it = t.events.find(vevent);
    auto dit = st.real_dev.find(vdev);
    if (it != t.events.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyEvent(dit->second.dev, it->second, nullptr);
    t.events.erase(vevent);
}

// ---- query pool (scalar POD prefix) ----
inline VkResult vk_gen_real_create_query_pool(
    VkDecodeState& st, uint32_t vdev, uint32_t vqpool, uint32_t flags, uint32_t queryType,
    uint32_t queryCount, uint32_t pipelineStatistics,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkQueryPoolCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qci.flags = flags;
    qci.queryType = static_cast<VkQueryType>(queryType);
    qci.queryCount = queryCount ? queryCount : 1;
    qci.pipelineStatistics = pipelineStatistics;
    VkQueryPool pool = VK_NULL_HANDLE;
    VkResult r = vkCreateQueryPool(dit->second.dev, &qci, nullptr, &pool);
    if (r == VK_SUCCESS) gen_tables(st).query_pools[vqpool] = pool;
    return r;
}

inline void vk_gen_real_destroy_query_pool(VkDecodeState& st, uint32_t vdev,
                                           uint32_t vqpool) {
    auto& t = gen_tables(st);
    auto it = t.query_pools.find(vqpool);
    auto dit = st.real_dev.find(vdev);
    if (it != t.query_pools.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyQueryPool(dit->second.dev, it->second, nullptr);
    t.query_pools.erase(vqpool);
}

// ===========================================================================
// WAVE B — descriptor set layout / pipeline layout / descriptor pool + allocate/free/update
// descriptor sets. These reconstruct an array-bearing CreateInfo (or write/alloc info) from
// the wire element vectors the generated decode read, TRANSLATING every handle-typed element
// (set-layout / image-view / sampler / buffer / descriptor-set virtual id) to its real Mali
// handle via VkGenTables, then call real Mali. Device-iterate target: ANGLE's RendererVk
// reaches descriptor-set management immediately after device setup (the trap proved its first
// unimplemented call is vkFreeDescriptorSets).
// ===========================================================================

// ---- descriptor set layout (pBindings: binding/type/count/stageFlags; immutable samplers
//      deferred — the binding's pImmutableSamplers is null) ----
inline VkResult vk_gen_real_create_descriptor_set_layout(
    VkDecodeState& st, uint32_t vdev, uint32_t vdsl, uint32_t flags,
    const std::vector<VkGenElem_create_descriptor_set_layout_bindings>& bindings,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkDescriptorSetLayoutBinding> vb;
    vb.reserve(bindings.size());
    for (const auto& b : bindings) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = b.binding;
        lb.descriptorType = static_cast<VkDescriptorType>(b.descriptorType);
        lb.descriptorCount = b.descriptorCount;
        lb.stageFlags = b.stageFlags;
        lb.pImmutableSamplers = nullptr;  // non-immutable samplers (ANGLE's path)
        vb.push_back(lb);
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.flags = flags;
    ci.bindingCount = static_cast<uint32_t>(vb.size());
    ci.pBindings = vb.empty() ? nullptr : vb.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorSetLayout(dit->second.dev, &ci, nullptr, &layout);
    if (r == VK_SUCCESS) gen_tables(st).dsl[vdsl] = layout;
    return r;
}

inline void vk_gen_real_destroy_descriptor_set_layout(VkDecodeState& st, uint32_t vdev,
                                                      uint32_t vdsl) {
    auto& t = gen_tables(st);
    auto it = t.dsl.find(vdsl);
    auto dit = st.real_dev.find(vdev);
    if (it != t.dsl.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dit->second.dev, it->second, nullptr);
    t.dsl.erase(vdsl);
}

// ---- pipeline layout (pSetLayouts: set-layout HANDLES translated via VkGenTables;
//      pPushConstantRanges: stageFlags/offset/size) ----
inline VkResult vk_gen_real_create_pipeline_layout(
    VkDecodeState& st, uint32_t vdev, uint32_t vplayout, uint32_t flags,
    const std::vector<VkGenElem_create_pipeline_layout_setLayouts>& setLayouts,
    const std::vector<VkGenElem_create_pipeline_layout_pushConstantRanges>& pushConstantRanges,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    auto& t = gen_tables(st);
    std::vector<VkDescriptorSetLayout> real_sets;
    real_sets.reserve(setLayouts.size());
    for (const auto& s : setLayouts) {
        auto it = t.dsl.find(s.self);
        if (it == t.dsl.end()) return VK_ERROR_INITIALIZATION_FAILED;  // unknown layout id
        real_sets.push_back(it->second);
    }
    std::vector<VkPushConstantRange> ranges;
    ranges.reserve(pushConstantRanges.size());
    for (const auto& p : pushConstantRanges) {
        VkPushConstantRange pr{};
        pr.stageFlags = p.stageFlags;
        pr.offset = p.offset;
        pr.size = p.size;
        ranges.push_back(pr);
    }
    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.flags = flags;
    ci.setLayoutCount = static_cast<uint32_t>(real_sets.size());
    ci.pSetLayouts = real_sets.empty() ? nullptr : real_sets.data();
    ci.pushConstantRangeCount = static_cast<uint32_t>(ranges.size());
    ci.pPushConstantRanges = ranges.empty() ? nullptr : ranges.data();
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineLayout(dit->second.dev, &ci, nullptr, &layout);
    if (r == VK_SUCCESS) gen_tables(st).pipeline_layouts[vplayout] = layout;
    return r;
}

inline void vk_gen_real_destroy_pipeline_layout(VkDecodeState& st, uint32_t vdev,
                                                uint32_t vplayout) {
    auto& t = gen_tables(st);
    auto it = t.pipeline_layouts.find(vplayout);
    auto dit = st.real_dev.find(vdev);
    if (it != t.pipeline_layouts.end() && dit != st.real_dev.end() &&
        it->second != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dit->second.dev, it->second, nullptr);
    t.pipeline_layouts.erase(vplayout);
}

// ---- descriptor pool (maxSets + pPoolSizes: type/descriptorCount). FREE_DESCRIPTOR_SET_BIT
//      is forced on so our generated vkFreeDescriptorSets can return sets to the pool. ----
inline VkResult vk_gen_real_create_descriptor_pool(
    VkDecodeState& st, uint32_t vdev, uint32_t vdpool, uint32_t flags, uint32_t maxSets,
    const std::vector<VkGenElem_create_descriptor_pool_poolSizes>& poolSizes,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(poolSizes.size());
    for (const auto& p : poolSizes) {
        VkDescriptorPoolSize ps{};
        ps.type = static_cast<VkDescriptorType>(p.type);
        ps.descriptorCount = p.descriptorCount ? p.descriptorCount : 1;
        sizes.push_back(ps);
    }
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // Honor the guest's flags + ensure FREE_DESCRIPTOR_SET so per-set free works (ANGLE may
    // or may not set it; our free path needs it and it is always valid to enable).
    ci.flags = flags | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets = maxSets ? maxSets : 1;
    ci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    ci.pPoolSizes = sizes.empty() ? nullptr : sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorPool(dit->second.dev, &ci, nullptr, &pool);
    if (r == VK_SUCCESS) gen_tables(st).descriptor_pools[vdpool] = pool;
    return r;
}

inline void vk_gen_real_destroy_descriptor_pool(VkDecodeState& st, uint32_t vdev,
                                                uint32_t vdpool) {
    auto& t = gen_tables(st);
    auto it = t.descriptor_pools.find(vdpool);
    auto dit = st.real_dev.find(vdev);
    if (it != t.descriptor_pools.end() && dit != st.real_dev.end() &&
        it->second != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dit->second.dev, it->second, nullptr);
    t.descriptor_pools.erase(vdpool);
    // The sets allocated from this pool are implicitly freed by the driver; drop any of our
    // virtual-id mappings that pointed into it would require a reverse index — instead we
    // leave stale vset entries (harmless: a freed set's id is never reused, monotonic).
}

// ---- allocate descriptor sets: N sets from the (virtual) pool against N (virtual) layouts;
//      store each real set under the guest's pre-assigned virtual id. ----
inline VkResult vk_gen_real_allocate_descriptor_sets(
    VkDecodeState& st, uint32_t vdev, uint32_t vpool,
    const std::vector<uint32_t>& vlayouts, const std::vector<uint32_t>& vsets) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    auto& t = gen_tables(st);
    auto pit = t.descriptor_pools.find(vpool);
    if (pit == t.descriptor_pools.end()) return VK_ERROR_INITIALIZATION_FAILED;
    if (vlayouts.size() != vsets.size() || vsets.empty()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkDescriptorSetLayout> real_layouts;
    real_layouts.reserve(vlayouts.size());
    for (uint32_t vl : vlayouts) {
        auto lit = t.dsl.find(vl);
        if (lit == t.dsl.end()) return VK_ERROR_INITIALIZATION_FAILED;
        real_layouts.push_back(lit->second);
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pit->second;
    ai.descriptorSetCount = static_cast<uint32_t>(real_layouts.size());
    ai.pSetLayouts = real_layouts.data();
    std::vector<VkDescriptorSet> real_sets(real_layouts.size(), VK_NULL_HANDLE);
    VkResult r = vkAllocateDescriptorSets(dit->second.dev, &ai, real_sets.data());
    if (r != VK_SUCCESS) return r;
    for (size_t i = 0; i < vsets.size(); ++i)
        t.descriptor_sets[vsets[i]] = real_sets[i];
    return VK_SUCCESS;
}

// ---- free descriptor sets back to their pool ----
inline void vk_gen_real_free_descriptor_sets(VkDecodeState& st, uint32_t vdev, uint32_t vpool,
                                             const std::vector<uint32_t>& vsets) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return;
    auto& t = gen_tables(st);
    auto pit = t.descriptor_pools.find(vpool);
    if (pit == t.descriptor_pools.end()) return;
    std::vector<VkDescriptorSet> real_sets;
    real_sets.reserve(vsets.size());
    for (uint32_t vs : vsets) {
        auto sit = t.descriptor_sets.find(vs);
        if (sit != t.descriptor_sets.end() && sit->second != VK_NULL_HANDLE)
            real_sets.push_back(sit->second);
    }
    if (!real_sets.empty())
        vkFreeDescriptorSets(dit->second.dev, pit->second,
                             static_cast<uint32_t>(real_sets.size()), real_sets.data());
    for (uint32_t vs : vsets) t.descriptor_sets.erase(vs);
}

// ---- update descriptor sets: bind buffers/images/samplers into the (virtual) destination
//      sets. Each write's per-descriptor handles are translated to real Mali handles. ----
inline void vk_gen_real_update_descriptor_sets(VkDecodeState& st, uint32_t vdev,
                                               const std::vector<VkGenDescWrite>& writes) {
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return;
    auto& t = gen_tables(st);
    // Storage for the per-write info arrays must outlive the vkUpdateDescriptorSets call.
    std::vector<VkWriteDescriptorSet> vw;
    std::vector<std::vector<VkDescriptorBufferInfo>> buf_store;
    std::vector<std::vector<VkDescriptorImageInfo>> img_store;
    vw.reserve(writes.size());
    buf_store.reserve(writes.size());
    img_store.reserve(writes.size());
    for (const auto& w : writes) {
        auto dsit = t.descriptor_sets.find(w.vdstset);
        if (dsit == t.descriptor_sets.end() || dsit->second == VK_NULL_HANDLE)
            continue;  // unknown destination set: skip (don't feed the driver a bad handle)
        VkWriteDescriptorSet wd{};
        wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wd.dstSet = dsit->second;
        wd.dstBinding = w.binding;
        wd.dstArrayElement = w.array_element;
        wd.descriptorType = static_cast<VkDescriptorType>(w.descriptor_type);
        if (!w.images.empty()) {
            std::vector<VkDescriptorImageInfo> infos;
            infos.reserve(w.images.size());
            for (const auto& ii : w.images) {
                VkDescriptorImageInfo di{};
                if (ii.vsampler) {
                    auto sit = t.samplers.find(ii.vsampler);
                    di.sampler = (sit != t.samplers.end()) ? sit->second : VK_NULL_HANDLE;
                }
                if (ii.vimageview) {
                    auto vit = t.views.find(ii.vimageview);
                    di.imageView = (vit != t.views.end()) ? vit->second : VK_NULL_HANDLE;
                }
                di.imageLayout = static_cast<VkImageLayout>(ii.image_layout);
                infos.push_back(di);
            }
            img_store.push_back(std::move(infos));
            wd.descriptorCount = static_cast<uint32_t>(img_store.back().size());
            wd.pImageInfo = img_store.back().data();
        } else {
            std::vector<VkDescriptorBufferInfo> infos;
            infos.reserve(w.buffers.size());
            for (const auto& bi : w.buffers) {
                VkDescriptorBufferInfo db{};
                if (bi.vbuffer) {
                    auto bit = t.buffers.find(bi.vbuffer);
                    db.buffer = (bit != t.buffers.end()) ? bit->second : VK_NULL_HANDLE;
                }
                db.offset = bi.offset;
                db.range = bi.range ? bi.range : VK_WHOLE_SIZE;
                infos.push_back(db);
            }
            buf_store.push_back(std::move(infos));
            wd.descriptorCount = static_cast<uint32_t>(buf_store.back().size());
            wd.pBufferInfo = buf_store.back().data();
        }
        if (wd.descriptorCount) vw.push_back(wd);
    }
    if (!vw.empty())
        vkUpdateDescriptorSets(dit->second.dev, static_cast<uint32_t>(vw.size()), vw.data(),
                               0, nullptr);
}

// ===========================================================================
// WAVE C — render pass (nested subpasses) + framebuffer. The render-pass real body rebuilds
// VkRenderPassCreateInfo from the wire attachment/subpass/dependency vectors, materializing
// each subpass's nested attachment-reference arrays into stable storage that outlives the
// vkCreateRenderPass call. Framebuffer translates its render-pass + image-view handles via
// VkGenTables. (Reachable once the device-init host-service wall — op 204 — is cleared.)
// ===========================================================================
inline VkResult vk_gen_real_create_render_pass(
    VkDecodeState& st, uint32_t vdev, uint32_t vrpass, uint32_t flags,
    const std::vector<VkGenRpAttachment>& attachments,
    const std::vector<VkGenRpSubpass>& subpasses,
    const std::vector<VkGenRpDependency>& dependencies,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkAttachmentDescription> att;
    att.reserve(attachments.size());
    for (const auto& a : attachments) {
        VkAttachmentDescription d{};
        d.flags = a.flags;
        d.format = static_cast<VkFormat>(a.format);
        d.samples = a.samples ? static_cast<VkSampleCountFlagBits>(a.samples)
                              : VK_SAMPLE_COUNT_1_BIT;
        d.loadOp = static_cast<VkAttachmentLoadOp>(a.loadOp);
        d.storeOp = static_cast<VkAttachmentStoreOp>(a.storeOp);
        d.stencilLoadOp = static_cast<VkAttachmentLoadOp>(a.stencilLoadOp);
        d.stencilStoreOp = static_cast<VkAttachmentStoreOp>(a.stencilStoreOp);
        d.initialLayout = static_cast<VkImageLayout>(a.initialLayout);
        d.finalLayout = static_cast<VkImageLayout>(a.finalLayout);
        att.push_back(d);
    }
    // The reference arrays each subpass points at must outlive vkCreateRenderPass, so they
    // are held in per-subpass storage vectors here (stable addresses while we build + call).
    std::vector<std::vector<VkAttachmentReference>> in_refs(subpasses.size());
    std::vector<std::vector<VkAttachmentReference>> col_refs(subpasses.size());
    std::vector<std::vector<VkAttachmentReference>> res_refs(subpasses.size());
    std::vector<VkAttachmentReference> depth_refs(subpasses.size());
    std::vector<std::vector<uint32_t>> pres(subpasses.size());
    std::vector<VkSubpassDescription> subs;
    subs.reserve(subpasses.size());
    auto to_refs = [](const std::vector<VkGenRpRef>& src,
                      std::vector<VkAttachmentReference>& dst) {
        dst.reserve(src.size());
        for (const auto& r : src) {
            VkAttachmentReference ar{};
            ar.attachment = r.attachment;
            ar.layout = static_cast<VkImageLayout>(r.layout);
            dst.push_back(ar);
        }
    };
    for (size_t i = 0; i < subpasses.size(); ++i) {
        const auto& s = subpasses[i];
        to_refs(s.input, in_refs[i]);
        to_refs(s.color, col_refs[i]);
        to_refs(s.resolve, res_refs[i]);
        pres[i] = s.preserve;
        VkSubpassDescription sd{};
        sd.flags = s.flags;
        sd.pipelineBindPoint = static_cast<VkPipelineBindPoint>(s.pipelineBindPoint);
        sd.inputAttachmentCount = static_cast<uint32_t>(in_refs[i].size());
        sd.pInputAttachments = in_refs[i].empty() ? nullptr : in_refs[i].data();
        sd.colorAttachmentCount = static_cast<uint32_t>(col_refs[i].size());
        sd.pColorAttachments = col_refs[i].empty() ? nullptr : col_refs[i].data();
        // pResolveAttachments, if present, must have colorAttachmentCount entries.
        sd.pResolveAttachments =
            (!res_refs[i].empty() && res_refs[i].size() == col_refs[i].size())
                ? res_refs[i].data() : nullptr;
        if (s.has_depth) {
            depth_refs[i].attachment = s.depth.attachment;
            depth_refs[i].layout = static_cast<VkImageLayout>(s.depth.layout);
            sd.pDepthStencilAttachment = &depth_refs[i];
        }
        sd.preserveAttachmentCount = static_cast<uint32_t>(pres[i].size());
        sd.pPreserveAttachments = pres[i].empty() ? nullptr : pres[i].data();
        subs.push_back(sd);
    }
    std::vector<VkSubpassDependency> deps;
    deps.reserve(dependencies.size());
    for (const auto& d : dependencies) {
        VkSubpassDependency sd{};
        sd.srcSubpass = d.srcSubpass;
        sd.dstSubpass = d.dstSubpass;
        sd.srcStageMask = d.srcStageMask;
        sd.dstStageMask = d.dstStageMask;
        sd.srcAccessMask = d.srcAccessMask;
        sd.dstAccessMask = d.dstAccessMask;
        sd.dependencyFlags = d.dependencyFlags;
        deps.push_back(sd);
    }
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.flags = flags;
    ci.attachmentCount = static_cast<uint32_t>(att.size());
    ci.pAttachments = att.empty() ? nullptr : att.data();
    ci.subpassCount = static_cast<uint32_t>(subs.size());
    ci.pSubpasses = subs.empty() ? nullptr : subs.data();
    ci.dependencyCount = static_cast<uint32_t>(deps.size());
    ci.pDependencies = deps.empty() ? nullptr : deps.data();
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = vkCreateRenderPass(dit->second.dev, &ci, nullptr, &rp);
    if (r == VK_SUCCESS) gen_tables(st).render_passes[vrpass] = rp;
    return r;
}

inline void vk_gen_real_destroy_render_pass(VkDecodeState& st, uint32_t vdev, uint32_t vrpass) {
    auto& t = gen_tables(st);
    auto it = t.render_passes.find(vrpass);
    auto dit = st.real_dev.find(vdev);
    if (it != t.render_passes.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyRenderPass(dit->second.dev, it->second, nullptr);
    t.render_passes.erase(vrpass);
}

// ---- framebuffer (renderPass + image-view HANDLES translated via VkGenTables) ----
inline VkResult vk_gen_real_create_framebuffer(
    VkDecodeState& st, uint32_t vdev, uint32_t vfb, uint32_t flags, uint32_t renderPass,
    uint32_t width, uint32_t height, uint32_t layers,
    const std::vector<VkGenElem_create_framebuffer_attachments>& attachments,
    const std::vector<uint32_t>& pnext_types,
    const std::vector<std::vector<uint8_t>>& pnext_bytes) {
    (void)pnext_types; (void)pnext_bytes;
    auto dit = st.real_dev.find(vdev);
    if (dit == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    auto& t = gen_tables(st);
    auto rpit = t.render_passes.find(renderPass);
    if (rpit == t.render_passes.end()) return VK_ERROR_INITIALIZATION_FAILED;  // unknown rpass
    std::vector<VkImageView> views;
    views.reserve(attachments.size());
    for (const auto& a : attachments) {
        auto vit = t.views.find(a.self);
        if (vit == t.views.end()) return VK_ERROR_INITIALIZATION_FAILED;  // unknown view id
        views.push_back(vit->second);
    }
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.flags = flags;
    ci.renderPass = rpit->second;
    ci.attachmentCount = static_cast<uint32_t>(views.size());
    ci.pAttachments = views.empty() ? nullptr : views.data();
    ci.width = width ? width : 1;
    ci.height = height ? height : 1;
    ci.layers = layers ? layers : 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = vkCreateFramebuffer(dit->second.dev, &ci, nullptr, &fb);
    if (r == VK_SUCCESS) gen_tables(st).framebuffers[vfb] = fb;
    return r;
}

inline void vk_gen_real_destroy_framebuffer(VkDecodeState& st, uint32_t vdev, uint32_t vfb) {
    auto& t = gen_tables(st);
    auto it = t.framebuffers.find(vfb);
    auto dit = st.real_dev.find(vdev);
    if (it != t.framebuffers.end() && dit != st.real_dev.end() && it->second != VK_NULL_HANDLE)
        vkDestroyFramebuffer(dit->second.dev, it->second, nullptr);
    t.framebuffers.erase(vfb);
}

}  // namespace alr::gpu

#endif  // ALR_VK_DECODE_REAL

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_REAL_HPP
