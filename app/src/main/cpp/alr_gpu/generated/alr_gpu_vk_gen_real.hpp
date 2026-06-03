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

}  // namespace alr::gpu

#endif  // ALR_VK_DECODE_REAL

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_GEN_REAL_HPP
