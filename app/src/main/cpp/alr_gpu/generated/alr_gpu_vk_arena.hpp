// alr_gpu_vk_arena.hpp — the SAME-PROCESS MAP_SHARED arena for HOST_VISIBLE Vulkan memory.
//
// This is the keystone of the "same-process thin-wire" model (memory alr-gpu-native-track):
// a guest's vkMapMemory must return a pointer the guest can write to such that the bytes
// land DIRECTLY in the memory the host's REAL Mali VkDeviceMemory is bound to — zero copy,
// no marshalling of pixel/vertex bytes across the ring. The handles cross the ring; the
// BYTES never do.
//
// HOW (the host + guest share ONE mmap):
//   * Before fork, the host creates a memfd, ftruncate's it to the arena size, and maps it
//     MAP_SHARED. It advertises the fd to the guest (env, like the ring fds). The guest
//     maps the SAME memfd MAP_SHARED at attach. Now host_base and guest_base point at the
//     same physical pages (different virtual addresses, one backing store).
//   * vkAllocateMemory(HOST_VISIBLE) -> the host bump-allocates a SLAB [off, off+size) in
//     the arena and creates a REAL VkDeviceMemory whose contents ALIAS that slab via
//     VK_EXT_external_memory_host (vkGetMemoryHostPointerPropertiesEXT + an import of
//     host_base+off). The reply carries `off`.
//   * vkMapMemory -> the guest returns guest_base + off (a local pointer add). A guest
//     write to that pointer is immediately visible to the real VkDeviceMemory (same pages),
//     so a vkQueueSubmit that reads the buffer/image sees the guest's data with no copy.
//
// GRACEFUL DEGRADATION (IMPLEMENTED in section (C) below): if Mali does NOT expose
// VK_EXT_external_memory_host (Mali-G615 does not), the host falls back to a STAGED slab:
// it still hands the guest an arena pointer (so vkMapMemory works + the wire is identical),
// but the real VkDeviceMemory is ordinary HOST_VISIBLE driver memory, and the arena slab is
// copied INTO it lazily (on vkFlushMappedMemoryRanges, or — the backstop — right before the
// first vkQueueSubmit that consumes the allocation). The reverse copy (real -> slab) runs on
// vkInvalidateMappedMemoryRanges for read-back. The fast zero-copy path (B) is PREFERRED and
// stays byte-identical when the extension is present; the staged path (C) keeps correctness
// on drivers without it. Which path is live is recorded per-allocation (an AlrVkStagedSlab
// exists only for a staged allocation; an imported allocation has none).
//
// This header is split into:
//   (A) a pure-POSIX arena (memfd + mmap + bump allocator) usable with NO Vulkan SDK, so
//       the host wire test exercises the offset bookkeeping host-side;
//   (B) the Vulkan import glue (zero-copy host-pointer import), compiled only under
//       ALR_VK_DECODE_REAL; and
//   (C) the staged-slab copy-on-map fallback (slab<->real VkDeviceMemory memcpy), also
//       ALR_VK_DECODE_REAL-only.
//
// Header-only + self-contained (POSIX + optional NDK Vulkan), matching the alr_gpu/** rule.

#ifndef ALR_GPU_GENERATED_ALR_GPU_VK_ARENA_HPP
#define ALR_GPU_GENERATED_ALR_GPU_VK_ARENA_HPP

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <mutex>

// MAP_ANONYMOUS spelling differs across platforms (Linux: MAP_ANONYMOUS; BSD/macOS:
// MAP_ANON). Only used by the non-Linux host wire-test fallback path.
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef ALR_VK_DECODE_REAL
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#endif

namespace alr::gpu {

// Default arena size: 256 MiB. Big enough for ANGLE's staging/uniform/vertex churn on a
// few surfaces; the host can override via alr_vk_arena_create(size). Page-multiple.
inline constexpr uint64_t kAlrVkArenaDefaultBytes = 256ull * 1024ull * 1024ull;

// Sentinel "this device memory is not arena-backed" offset.
inline constexpr uint64_t kAlrVkArenaNoOffset = UINT64_MAX;

// ---------------------------------------------------------------------------
// (A) The POSIX arena: a MAP_SHARED memfd + a mutex-guarded bump allocator. ONE instance
// per process; the host creates it (alr_vk_arena_create) before fork and the guest
// attaches to the inherited fd (alr_vk_arena_attach). Both then resolve offset->pointer
// locally with alr_vk_arena_ptr().
// ---------------------------------------------------------------------------
struct AlrVkArena {
    int fd = -1;            // memfd of the shared region (inheritable across fork)
    uint64_t size = 0;      // region size in bytes (page multiple)
    void* base = nullptr;   // this process's MAP_SHARED mapping of the region
    uint64_t next = 0;      // bump cursor (host-only; the guest never allocates)
    std::mutex mu;          // guards `next`
};

inline AlrVkArena& alr_vk_arena() {
    static AlrVkArena a;
    return a;
}

// memfd_create is Linux-only (the Android device target). On a non-Linux HOST build (the
// wire test on macOS), there is no memfd; alr_vk_arena_create falls back to an anonymous
// mmap so the bump-allocator + offset->pointer bookkeeping still run for the test (an
// anonymous mapping is NOT shareable across fork, which the wire test doesn't need — it
// drives both ends in one process). Returns -2 to mean "no memfd; use anon mmap".
inline int alr_vk_arena_memfd(const char* name) {
#if defined(__linux__) && defined(__NR_memfd_create)
    long fd = ::syscall(__NR_memfd_create, name, 0u);
    return fd < 0 ? -1 : static_cast<int>(fd);
#else
    (void)name;
    return -2;  // no memfd on this host; caller uses MAP_ANON
#endif
}

// Round up to a page boundary (slab alignment must satisfy the driver's nonCoherentAtomSize
// + the host-pointer import alignment; a page is a safe superset for both).
inline uint64_t alr_vk_arena_page_round(uint64_t n) {
    const uint64_t pg = 4096;
    return (n + pg - 1) & ~(pg - 1);
}

// HOST: create the arena (memfd + MAP_SHARED). Returns true on success; fills alr_vk_arena().
// Idempotent (a second call returns the existing arena).
inline bool alr_vk_arena_create(uint64_t bytes = kAlrVkArenaDefaultBytes) {
    auto& a = alr_vk_arena();
    if (a.base) return true;
    const uint64_t sz = alr_vk_arena_page_round(bytes ? bytes : kAlrVkArenaDefaultBytes);
    const int fd = alr_vk_arena_memfd("alr_vk_arena");
    if (fd == -1) return false;  // memfd available but failed
    void* p = MAP_FAILED;
    if (fd >= 0) {
        // Linux: a real MAP_SHARED memfd (inheritable across fork — the device path).
        if (::ftruncate(fd, static_cast<off_t>(sz)) != 0) { ::close(fd); return false; }
        p = ::mmap(nullptr, static_cast<size_t>(sz), PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return false; }
    } else {
        // Non-Linux host (wire test): anonymous mmap, no shareable fd. fd stays -1.
        p = ::mmap(nullptr, static_cast<size_t>(sz), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return false;
    }
    a.fd = fd >= 0 ? fd : -1;
    a.size = sz;
    a.base = p;
    a.next = 0;
    return true;
}

// GUEST: attach to the inherited arena fd (the host advertised it). Maps MAP_SHARED so the
// guest sees the SAME pages. Returns true on success.
inline bool alr_vk_arena_attach(int fd, uint64_t bytes) {
    auto& a = alr_vk_arena();
    if (a.base) return true;
    if (fd < 0 || bytes == 0) return false;
    void* p = ::mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return false;
    a.fd = fd;
    a.size = bytes;
    a.base = p;
    a.next = 0;  // the guest never bump-allocates; the host owns `next`
    return true;
}

// Resolve an arena offset to THIS process's pointer (host or guest). Returns nullptr for
// the no-offset sentinel or an out-of-range offset.
inline void* alr_vk_arena_ptr(uint64_t off) {
    auto& a = alr_vk_arena();
    if (off == kAlrVkArenaNoOffset || !a.base || off >= a.size) return nullptr;
    return static_cast<uint8_t*>(a.base) + off;
}

// HOST: bump-allocate a page-aligned slab of `bytes`. Returns the offset, or
// kAlrVkArenaNoOffset if the arena is exhausted (the caller then falls back to staged
// driver memory). Thread-safe.
inline uint64_t alr_vk_arena_alloc(uint64_t bytes) {
    auto& a = alr_vk_arena();
    if (!a.base || bytes == 0) return kAlrVkArenaNoOffset;
    const uint64_t need = alr_vk_arena_page_round(bytes);
    std::lock_guard<std::mutex> lk(a.mu);
    if (a.next + need > a.size) return kAlrVkArenaNoOffset;  // exhausted
    const uint64_t off = a.next;
    a.next += need;
    return off;
}

inline bool alr_vk_arena_ready() { return alr_vk_arena().base != nullptr; }
inline int alr_vk_arena_fd() { return alr_vk_arena().fd; }
inline uint64_t alr_vk_arena_size() { return alr_vk_arena().size; }

#ifdef ALR_VK_DECODE_REAL
// ---------------------------------------------------------------------------
// (B) Vulkan import glue — make a REAL VkDeviceMemory whose contents alias an arena slab.
//
// The clean primitive is VK_EXT_external_memory_host: import the host pointer (arena base +
// slab offset) as device memory. The driver maps OUR pages as its allocation, so a guest
// write through the shared arena IS a write to the bound buffer/image's backing store.
// ---------------------------------------------------------------------------

// Is VK_EXT_external_memory_host enabled/usable on this device? (We probe the device proc.)
inline bool alr_vk_arena_host_import_available(VkDevice dev) {
    return reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
               vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT")) != nullptr;
}

// Create a real HOST_VISIBLE VkDeviceMemory aliasing arena[off, off+size) via host-pointer
// import. `mem_type_index` must be a HOST_VISIBLE|HOST_COHERENT type whose bit is allowed
// by vkGetMemoryHostPointerPropertiesEXT for this pointer. Returns VK_SUCCESS + *out_mem on
// success. The caller picks the memory type (see alr_vk_arena_pick_host_type).
inline VkResult alr_vk_arena_import_slab(VkDevice dev, uint64_t off, uint64_t size,
                                         uint32_t mem_type_index, VkDeviceMemory* out_mem) {
    *out_mem = VK_NULL_HANDLE;
    void* host_ptr = alr_vk_arena_ptr(off);
    if (!host_ptr) return VK_ERROR_INITIALIZATION_FAILED;
    auto pGetProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!pGetProps) return VK_ERROR_FEATURE_NOT_PRESENT;

    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (pGetProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host_ptr,
                  &hp) != VK_SUCCESS)
        return VK_ERROR_INITIALIZATION_FAILED;
    // The requested type must be importable for this pointer.
    if (((hp.memoryTypeBits >> mem_type_index) & 1u) == 0)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkImportMemoryHostPointerInfoEXT import{};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = host_ptr;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &import;
    mai.allocationSize = alr_vk_arena_page_round(size);
    mai.memoryTypeIndex = mem_type_index;
    return vkAllocateMemory(dev, &mai, nullptr, out_mem);
}

// Pick a HOST_VISIBLE|HOST_COHERENT memory type index that the arena pointer at `off` can
// be imported as. Returns true + *out_type on success.
inline bool alr_vk_arena_pick_host_type(VkPhysicalDevice phys, VkDevice dev, uint64_t off,
                                        uint32_t* out_type) {
    auto pGetProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!pGetProps) return false;
    void* host_ptr = alr_vk_arena_ptr(off);
    if (!host_ptr) return false;
    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (pGetProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host_ptr,
                  &hp) != VK_SUCCESS)
        return false;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const bool importable = ((hp.memoryTypeBits >> i) & 1u) != 0;
        const bool host_vis =
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (importable && host_vis) { *out_type = i; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// (C) STAGED-SLAB COPY-ON-MAP fallback (the documented degradation, now IMPLEMENTED).
//
// When VK_EXT_external_memory_host is NOT available (Mali-G615 does NOT expose it), the
// host cannot import an arena pointer as VkDeviceMemory, so the zero-copy alias of (B) is
// impossible. Instead of failing vkMapMemory, we keep the wire identical: the guest still
// gets an arena SLAB pointer (a SHADOW of the real allocation), so its writes succeed
// locally. The real allocation is ordinary HOST_VISIBLE driver memory. We then COPY the
// shadow slab into the real memory lazily — on vkFlushMappedMemoryRanges, or (the backstop)
// right before the first vkQueueSubmit that consumes the allocation. The reverse copy
// (real -> shadow) runs on vkInvalidateMappedMemoryRanges for read-back.
//
// This struct is the per-allocation record the HOST keeps for a staged allocation. (For an
// import / zero-copy allocation there is NO StagedSlab — that path is byte-identical to (B),
// the strict no-regression guarantee.) The host keeps these in its VkGenTables, keyed by the
// device-memory virtual id; the helpers below do the actual VkDeviceMemory map+memcpy.
struct AlrVkStagedSlab {
    VkDeviceMemory real_mem = VK_NULL_HANDLE;  // the real HOST_VISIBLE Mali allocation
    uint64_t arena_off = kAlrVkArenaNoOffset;  // shadow slab offset in the shared arena
    uint64_t size = 0;                         // allocation size (bytes the slab shadows)
    bool dirty = false;  // guest wrote the slab since the last copy-to-real (needs flush)
};

// Pick a HOST_VISIBLE memory type index (COHERENT preferred) whose bit is set in
// `allowed_bits` (the memory type a real allocation of `mem_type_index` would satisfy, i.e.
// the requested type's own bit, or — if that type is not host-visible — any host-visible
// type, so the guest still gets mappable memory). Returns true + *out_type on success.
// HOST_COHERENT is preferred so the slab->real copy needs no explicit driver flush; if only
// a non-coherent host-visible type exists, the copy helpers issue the explicit flush/invalidate.
inline bool alr_vk_arena_pick_staged_host_type(VkPhysicalDevice phys, uint32_t requested,
                                               uint32_t* out_type, bool* out_coherent) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    // 1) If the guest's requested type is itself host-visible, honor it (it already satisfies
    //    the bound resource's memoryTypeBits — ANGLE chose it for exactly that). This keeps
    //    vkBind*Memory compatible with no guesswork.
    if (requested < mp.memoryTypeCount &&
        (mp.memoryTypes[requested].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        *out_type = requested;
        *out_coherent =
            (mp.memoryTypes[requested].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        return true;
    }
    // 2) Otherwise fall back to ANY host-visible type (coherent first). (The guest only ever
    //    maps host-visible memory; a DEVICE_LOCAL-only request that nonetheless gets mapped
    //    is re-homed to a host-visible type so the shadow slab still has a real backing.)
    int best = -1; bool best_coh = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const auto f = mp.memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        const bool coh = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (best < 0 || (coh && !best_coh)) { best = static_cast<int>(i); best_coh = coh; }
        if (coh) break;
    }
    if (best < 0) return false;
    *out_type = static_cast<uint32_t>(best);
    *out_coherent = best_coh;
    return true;
}

// Copy the shadow slab -> the real HOST_VISIBLE VkDeviceMemory (the guest-write direction:
// run on flush / before a submit that consumes the allocation). Maps the real memory, memcpy
// from the arena, flushes (non-coherent) and unmaps. `off`/`size` bound the copy within the
// allocation (VK_WHOLE_SIZE-style: size==0 means to the end of the allocation). Returns
// VK_SUCCESS or the driver error. Clears `slab.dirty` on success.
inline VkResult alr_vk_arena_staged_copy_to_real(VkDevice dev, AlrVkStagedSlab& slab,
                                                 bool coherent, uint64_t off, uint64_t size) {
    if (slab.real_mem == VK_NULL_HANDLE || slab.arena_off == kAlrVkArenaNoOffset)
        return VK_ERROR_INITIALIZATION_FAILED;
    const uint8_t* src = static_cast<const uint8_t*>(alr_vk_arena_ptr(slab.arena_off));
    if (!src) return VK_ERROR_INITIALIZATION_FAILED;
    if (off >= slab.size) return VK_SUCCESS;  // nothing in range
    uint64_t n = (size == 0 || off + size > slab.size) ? (slab.size - off) : size;
    if (n == 0) return VK_SUCCESS;
    void* dst = nullptr;
    VkResult r = vkMapMemory(dev, slab.real_mem, off, n, 0, &dst);
    if (r != VK_SUCCESS || !dst) return (r == VK_SUCCESS) ? VK_ERROR_MEMORY_MAP_FAILED : r;
    std::memcpy(dst, src + off, static_cast<size_t>(n));
    if (!coherent) {
        VkMappedMemoryRange mr{};
        mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mr.memory = slab.real_mem;
        mr.offset = off;
        mr.size = VK_WHOLE_SIZE;  // off..end; the driver rounds to nonCoherentAtomSize
        vkFlushMappedMemoryRanges(dev, 1, &mr);
    }
    vkUnmapMemory(dev, slab.real_mem);
    slab.dirty = false;
    return VK_SUCCESS;
}

// Copy the real HOST_VISIBLE VkDeviceMemory -> the shadow slab (the read-back direction: run
// on vkInvalidateMappedMemoryRanges so the guest reads fresh GPU-written bytes). Returns
// VK_SUCCESS or the driver error.
inline VkResult alr_vk_arena_staged_copy_from_real(VkDevice dev, const AlrVkStagedSlab& slab,
                                                   bool coherent, uint64_t off, uint64_t size) {
    if (slab.real_mem == VK_NULL_HANDLE || slab.arena_off == kAlrVkArenaNoOffset)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint8_t* dst = static_cast<uint8_t*>(alr_vk_arena_ptr(slab.arena_off));
    if (!dst) return VK_ERROR_INITIALIZATION_FAILED;
    if (off >= slab.size) return VK_SUCCESS;
    uint64_t n = (size == 0 || off + size > slab.size) ? (slab.size - off) : size;
    if (n == 0) return VK_SUCCESS;
    void* src = nullptr;
    VkResult r = vkMapMemory(dev, slab.real_mem, off, n, 0, &src);
    if (r != VK_SUCCESS || !src) return (r == VK_SUCCESS) ? VK_ERROR_MEMORY_MAP_FAILED : r;
    if (!coherent) {
        VkMappedMemoryRange mr{};
        mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mr.memory = slab.real_mem;
        mr.offset = off;
        mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(dev, 1, &mr);
    }
    std::memcpy(dst + off, src, static_cast<size_t>(n));
    vkUnmapMemory(dev, slab.real_mem);
    return VK_SUCCESS;
}
#endif  // ALR_VK_DECODE_REAL

}  // namespace alr::gpu

#endif  // ALR_GPU_GENERATED_ALR_GPU_VK_ARENA_HPP
