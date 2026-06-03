// ==========================================================================
//  GENERATED FILE — DO NOT EDIT BY HAND.
//  Produced by tools/gen_vk_passthrough.py from tools/vk_registry/vk.xml
//  (Vulkan-Headers v1.3.275, VK_HEADER_VERSION 275). Regenerate with:
//      python3 tools/gen_vk_passthrough.py
//  Edit the SPECS in that tool, not this file. The wire opcodes here are
//  APPEND-ONLY (they are the on-the-wire ABI shared with the host decoder).
// ==========================================================================

// Generated ICD dispatch entries. The guest ICD (alr_icd_vulkan.c) #includes this
// twice: once with ALR_ICD_GEN_DEFINE (emit the C functions) and once with
// ALR_ICD_GEN_TABLE (emit the ALR_ENTRY rows for alr_lookup). Each function
// allocates a virtual id, marshals its request via alr_icd_roundtrip, and (for
// round-tripping ops) decodes the reply. It reuses the ICD's existing helpers:
//   AlrIcdDevice (dev->vdev), alr_alloc(&counter,1), alr_icd_ring_ok(),
//   alr_icd_roundtrip(), the generated encoders, and the generated reply scanners
//   (alr_gpu_vk_gen_icd_runtime.inc) + arena glue (alr_icd_arena_ptr/_mem_record).

// Entrypoints in ICD_SKIP are generated for the host (encoder + decode) but NOT
// emitted here, because the guest ICD already hand-writes them (a duplicate C symbol
// would collide). See ICD_SKIP in tools/gen_vk_passthrough.py.
#ifdef ALR_ICD_GEN_DEFINE
// (skipped: vkCreateCommandPool is hand-written in the ICD)
// (skipped: vkDestroyCommandPool is hand-written in the ICD)
static VkResult VKAPI_CALL alr_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                          const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vmem, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[128]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_allocate_memory_begin(&e, dev->vdev, vid,
                            (uint64_t)pAllocateInfo->allocationSize,
                            pAllocateInfo->memoryTypeIndex);
        alr_vk_gen_pnext_count(&e, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            uint64_t arena_off = (uint64_t)-1;
            if (rlen) (void)alr_icd_gen_scan_alloc(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_ALLOCATE_MEMORY, &res, &arena_off);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
            alr_icd_mem_record(vid, arena_off, (uint64_t)pAllocateInfo->allocationSize);
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pMemory = (VkDeviceMemory)(uintptr_t)vid;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL alr_vkMapMemory(VkDevice device, VkDeviceMemory memory,
                          VkDeviceSize offset, VkDeviceSize size,
                          VkMemoryMapFlags flags, void **ppData) {
    (void)flags;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vmem = (uint32_t)(uintptr_t)memory;
    if (!dev || !ppData || memory == 0) return VK_ERROR_MEMORY_MAP_FAILED;
    if (alr_icd_ring_ok()) {
        uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_map_memory(&e, dev->vdev, vmem, (uint64_t)offset, (uint64_t)size);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            uint64_t mapped_off = (uint64_t)-1; int32_t res = -1;
            if (rlen) (void)alr_icd_gen_scan_map(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_MAP_MEMORY, &res, &mapped_off);
            void *p = alr_icd_arena_ptr(mapped_off);
            if (res == 0 && p) { *ppData = p; return VK_SUCCESS; }
        }
    }
    return VK_ERROR_MEMORY_MAP_FAILED;
}

static void VKAPI_CALL alr_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vmem = (uint32_t)(uintptr_t)memory;
    if (!dev || memory == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_unmap_memory(&e, dev->vdev, vmem);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                          const VkMappedMemoryRange *pMemoryRanges) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t i;
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok() || memoryRangeCount == 0 || !pMemoryRanges) return VK_SUCCESS;
    {
        uint8_t req[16 + 64 * 20]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        uint32_t n = memoryRangeCount > 64 ? 64 : memoryRangeCount;
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_flush_mapped_memory_ranges_begin(&e, dev->vdev, n);
        for (i = 0; i < n; ++i) {
            uint32_t vmem = (uint32_t)(uintptr_t)pMemoryRanges[i].memory;
            alr_vk_enc_gen_flush_mapped_memory_ranges_range(&e, vmem, (uint64_t)pMemoryRanges[i].offset,
                                (uint64_t)pMemoryRanges[i].size);
        }
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)memory;
    if (!dev || memory == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_free_memory(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkBuffer *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vbuf, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_buffer_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint64_t)pCreateInfo->size, (uint32_t)pCreateInfo->usage, (uint32_t)pCreateInfo->sharingMode);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_BUFFER, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkBuffer)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)buffer;
    if (!dev || buffer == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_buffer(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static void VKAPI_CALL alr_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                          VkMemoryRequirements *pMemoryRequirements) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)buffer;
    if (!dev || !pMemoryRequirements) return;
    /* Conservative defaults so a ring-less ICD still returns a usable struct. */
    pMemoryRequirements->size = 0;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0xffffffffu;
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_get_buffer_memory_requirements(&e, dev->vdev, vh);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            uint64_t size = 0, align = 256; uint32_t bits = 0xffffffffu; int32_t res = -1;
            if (rlen && alr_icd_gen_scan_reqs(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_GET_BUFFER_MEMORY_REQUIREMENTS, &res, &size, &align, &bits) && res == 0) {
                pMemoryRequirements->size = size;
                pMemoryRequirements->alignment = align ? align : 256;
                pMemoryRequirements->memoryTypeBits = bits;
            }
        }
    }
}

static VkResult VKAPI_CALL alr_vkBindBufferMemory(VkDevice device, VkBuffer buffer,
                          VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)buffer;
    uint32_t vmem = (uint32_t)(uintptr_t)memory; int32_t res = 0;
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (alr_icd_ring_ok()) {
        uint8_t req[40]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_bind_buffer_memory(&e, dev->vdev, vh, vmem, (uint64_t)memoryOffset);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_BIND_BUFFER_MEMORY, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    return (VkResult)res;
}

static VkResult VKAPI_CALL alr_vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkImage *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vimg, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_image_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint32_t)pCreateInfo->imageType, (uint32_t)pCreateInfo->format, (uint32_t)pCreateInfo->extent.width, (uint32_t)pCreateInfo->extent.height, (uint32_t)pCreateInfo->extent.depth, (uint32_t)pCreateInfo->mipLevels, (uint32_t)pCreateInfo->arrayLayers, (uint32_t)pCreateInfo->samples, (uint32_t)pCreateInfo->tiling, (uint32_t)pCreateInfo->usage, (uint32_t)pCreateInfo->sharingMode, (uint32_t)pCreateInfo->initialLayout);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_IMAGE, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkImage)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyImage(VkDevice device, VkImage image,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)image;
    if (!dev || image == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_image(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static void VKAPI_CALL alr_vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                          VkMemoryRequirements *pMemoryRequirements) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)image;
    if (!dev || !pMemoryRequirements) return;
    /* Conservative defaults so a ring-less ICD still returns a usable struct. */
    pMemoryRequirements->size = 0;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0xffffffffu;
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_get_image_memory_requirements(&e, dev->vdev, vh);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            uint64_t size = 0, align = 256; uint32_t bits = 0xffffffffu; int32_t res = -1;
            if (rlen && alr_icd_gen_scan_reqs(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_GET_IMAGE_MEMORY_REQUIREMENTS, &res, &size, &align, &bits) && res == 0) {
                pMemoryRequirements->size = size;
                pMemoryRequirements->alignment = align ? align : 256;
                pMemoryRequirements->memoryTypeBits = bits;
            }
        }
    }
}

static VkResult VKAPI_CALL alr_vkBindImageMemory(VkDevice device, VkImage image,
                          VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)image;
    uint32_t vmem = (uint32_t)(uintptr_t)memory; int32_t res = 0;
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (alr_icd_ring_ok()) {
        uint8_t req[40]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_bind_image_memory(&e, dev->vdev, vh, vmem, (uint64_t)memoryOffset);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_BIND_IMAGE_MEMORY, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    return (VkResult)res;
}

static VkResult VKAPI_CALL alr_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkImageView *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vview, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_image_view_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint32_t)(uintptr_t)pCreateInfo->image, (uint32_t)pCreateInfo->viewType, (uint32_t)pCreateInfo->format, (uint32_t)pCreateInfo->components.r, (uint32_t)pCreateInfo->components.g, (uint32_t)pCreateInfo->components.b, (uint32_t)pCreateInfo->components.a, (uint32_t)pCreateInfo->subresourceRange.aspectMask, (uint32_t)pCreateInfo->subresourceRange.baseMipLevel, (uint32_t)pCreateInfo->subresourceRange.levelCount, (uint32_t)pCreateInfo->subresourceRange.baseArrayLayer, (uint32_t)pCreateInfo->subresourceRange.layerCount);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_IMAGE_VIEW, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkImageView)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyImageView(VkDevice device, VkImageView imageView,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)imageView;
    if (!dev || imageView == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_image_view(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

// (skipped: vkCreateShaderModule is hand-written in the ICD)
// (skipped: vkDestroyShaderModule is hand-written in the ICD)
static VkResult VKAPI_CALL alr_vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkPipelineCache *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vpcache, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_pipeline_cache_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_PIPELINE_CACHE, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkPipelineCache)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)pipelineCache;
    if (!dev || pipelineCache == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_pipeline_cache(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkSampler *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vsamp, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_sampler_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint32_t)pCreateInfo->magFilter, (uint32_t)pCreateInfo->minFilter, (uint32_t)pCreateInfo->mipmapMode, (uint32_t)pCreateInfo->addressModeU, (uint32_t)pCreateInfo->addressModeV, (uint32_t)pCreateInfo->addressModeW, (float)pCreateInfo->mipLodBias, (uint32_t)pCreateInfo->anisotropyEnable, (float)pCreateInfo->maxAnisotropy, (uint32_t)pCreateInfo->compareEnable, (uint32_t)pCreateInfo->compareOp, (float)pCreateInfo->minLod, (float)pCreateInfo->maxLod, (uint32_t)pCreateInfo->borderColor, (uint32_t)pCreateInfo->unnormalizedCoordinates);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_SAMPLER, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkSampler)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroySampler(VkDevice device, VkSampler sampler,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)sampler;
    if (!dev || sampler == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_sampler(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkFence *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vfence, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_fence_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_FENCE, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkFence)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyFence(VkDevice device, VkFence fence,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)fence;
    if (!dev || fence == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_fence(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkSemaphore *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vsem, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_semaphore_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_SEMAPHORE, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkSemaphore)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)semaphore;
    if (!dev || semaphore == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_semaphore(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateEvent(VkDevice device, const VkEventCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkEvent *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vevent, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_event_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_EVENT, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkEvent)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyEvent(VkDevice device, VkEvent event,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)event;
    if (!dev || event == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_event(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkQueryPool *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vqpool, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[256]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_gen_create_query_pool_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint32_t)pCreateInfo->queryType, (uint32_t)pCreateInfo->queryCount, (uint32_t)pCreateInfo->pipelineStatistics);
        alr_vk_gen_pnext_count(&e, 0);  // first batch: no pNext forwarded yet
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_QUERY_POOL, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkQueryPool)(uintptr_t)vid;  // non-dispatchable: carries the virtual id
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)queryPool;
    if (!dev || queryPool == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_query_pool(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0; uint32_t ai;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vdsl, 1);
    if (alr_icd_ring_ok()) {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        size_t cap = 256;
        cap += (size_t)pCreateInfo->bindingCount * 24;
        uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_create_descriptor_set_layout_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        { uint32_t n = pCreateInfo->bindingCount; if (n > 4096) n = 4096;
          alr_vk_enc_gen_create_descriptor_set_layout_bindings_count(&e, n);
          for (ai = 0; ai < n; ++ai) {
              alr_vk_enc_gen_create_descriptor_set_layout_bindings_elem(&e, (uint32_t)pCreateInfo->pBindings[ai].binding, (uint32_t)pCreateInfo->pBindings[ai].descriptorType, (uint32_t)pCreateInfo->pBindings[ai].descriptorCount, (uint32_t)pCreateInfo->pBindings[ai].stageFlags); } }
        alr_vk_gen_pnext_count(&e, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_DESCRIPTOR_SET_LAYOUT, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        free(req);
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkDescriptorSetLayout)(uintptr_t)vid;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout descriptorSetLayout,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)descriptorSetLayout;
    if (!dev || descriptorSetLayout == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_descriptor_set_layout(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0; uint32_t ai;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vplayout, 1);
    if (alr_icd_ring_ok()) {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        size_t cap = 256;
        cap += (size_t)pCreateInfo->setLayoutCount * 24;
        cap += (size_t)pCreateInfo->pushConstantRangeCount * 24;
        uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_create_pipeline_layout_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        { uint32_t n = pCreateInfo->setLayoutCount; if (n > 4096) n = 4096;
          alr_vk_enc_gen_create_pipeline_layout_setLayouts_count(&e, n);
          for (ai = 0; ai < n; ++ai) {
              alr_vk_enc_gen_create_pipeline_layout_setLayouts_elem(&e, (uint32_t)(uintptr_t)pCreateInfo->pSetLayouts[ai]); } }
        { uint32_t n = pCreateInfo->pushConstantRangeCount; if (n > 4096) n = 4096;
          alr_vk_enc_gen_create_pipeline_layout_pushConstantRanges_count(&e, n);
          for (ai = 0; ai < n; ++ai) {
              alr_vk_enc_gen_create_pipeline_layout_pushConstantRanges_elem(&e, (uint32_t)pCreateInfo->pPushConstantRanges[ai].stageFlags, (uint32_t)pCreateInfo->pPushConstantRanges[ai].offset, (uint32_t)pCreateInfo->pPushConstantRanges[ai].size); } }
        alr_vk_gen_pnext_count(&e, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_PIPELINE_LAYOUT, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        free(req);
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkPipelineLayout)(uintptr_t)vid;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)pipelineLayout;
    if (!dev || pipelineLayout == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_pipeline_layout(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0; uint32_t ai;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vdpool, 1);
    if (alr_icd_ring_ok()) {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        size_t cap = 256;
        cap += (size_t)pCreateInfo->poolSizeCount * 24;
        uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_create_descriptor_pool_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint32_t)pCreateInfo->maxSets);
        { uint32_t n = pCreateInfo->poolSizeCount; if (n > 4096) n = 4096;
          alr_vk_enc_gen_create_descriptor_pool_poolSizes_count(&e, n);
          for (ai = 0; ai < n; ++ai) {
              alr_vk_enc_gen_create_descriptor_pool_poolSizes_elem(&e, (uint32_t)pCreateInfo->pPoolSizes[ai].type, (uint32_t)pCreateInfo->pPoolSizes[ai].descriptorCount); } }
        alr_vk_gen_pnext_count(&e, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_DESCRIPTOR_POOL, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        free(req);
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkDescriptorPool)(uintptr_t)vid;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)descriptorPool;
    if (!dev || descriptorPool == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_descriptor_pool(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkAllocateDescriptorSets(VkDevice device,
                          const VkDescriptorSetAllocateInfo *pAllocateInfo,
                          VkDescriptorSet *pDescriptorSets) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i; int32_t res = 0;
    if (!dev || !pAllocateInfo || !pDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = pAllocateInfo->descriptorSetCount; if (n > 4096) n = 4096;
    /* Pre-assign a virtual id per set (client-side, monotonic) and write them out;
     * the host maps each to a real descriptor set allocated from the real pool. */
    for (i = 0; i < n; ++i)
        pDescriptorSets[i] = (VkDescriptorSet)(uintptr_t)alr_alloc(&g_next_vdset, 1);
    if (alr_icd_ring_ok()) {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        size_t cap = 64 + (size_t)n * 8; uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_allocate_descriptor_sets_begin(&e, dev->vdev,
                          (uint32_t)(uintptr_t)pAllocateInfo->descriptorPool, n);
        for (i = 0; i < n; ++i)
            alr_vk_enc_gen_allocate_descriptor_sets_set(&e, (uint32_t)(uintptr_t)pAllocateInfo->pSetLayouts[i],
                              (uint32_t)(uintptr_t)pDescriptorSets[i]);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_ALLOCATE_DESCRIPTOR_SETS, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        free(req);
    }
    return (VkResult)res;
}

static VkResult VKAPI_CALL alr_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i;
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok() || descriptorSetCount == 0 || !pDescriptorSets) return VK_SUCCESS;
    {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        uint32_t n = descriptorSetCount > 4096 ? 4096 : descriptorSetCount;
        size_t cap = 32 + (size_t)n * 4; uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_free_descriptor_sets_begin(&e, dev->vdev, (uint32_t)(uintptr_t)descriptorPool, n);
        for (i = 0; i < n; ++i)
            alr_vk_enc_gen_free_descriptor_sets_set(&e, (uint32_t)(uintptr_t)pDescriptorSets[i]);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
        free(req);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                          const VkWriteDescriptorSet *pDescriptorWrites,
                          uint32_t descriptorCopyCount,
                          const VkCopyDescriptorSet *pDescriptorCopies) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t i, d;
    (void)descriptorCopyCount; (void)pDescriptorCopies;  /* copies deferred */
    if (!dev || !alr_icd_ring_ok() || descriptorWriteCount == 0 || !pDescriptorWrites) return;
    {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        uint32_t wn = descriptorWriteCount > 4096 ? 4096 : descriptorWriteCount;
        size_t cap = 64; for (i = 0; i < wn; ++i) cap += 24 + (size_t)pDescriptorWrites[i].descriptorCount * 24;
        uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_update_descriptor_sets_begin(&e, dev->vdev, wn);
        for (i = 0; i < wn; ++i) {
            const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
            uint32_t dc = w->descriptorCount > 4096 ? 4096 : w->descriptorCount;
            alr_vk_enc_gen_update_descriptor_sets_write(&e, (uint32_t)(uintptr_t)w->dstSet, w->dstBinding,
                              w->dstArrayElement, (uint32_t)w->descriptorType, dc);
            int is_image = alr_vk_desc_type_is_image((uint32_t)w->descriptorType);
            for (d = 0; d < dc; ++d) {
                if (is_image) {
                    uint32_t vsamp = w->pImageInfo ? (uint32_t)(uintptr_t)w->pImageInfo[d].sampler : 0;
                    uint32_t vview = w->pImageInfo ? (uint32_t)(uintptr_t)w->pImageInfo[d].imageView : 0;
                    uint32_t lay   = w->pImageInfo ? (uint32_t)w->pImageInfo[d].imageLayout : 0;
                    alr_vk_enc_gen_update_descriptor_sets_image_info(&e, vsamp, vview, lay);
                } else {
                    uint32_t vbuf = w->pBufferInfo ? (uint32_t)(uintptr_t)w->pBufferInfo[d].buffer : 0;
                    uint64_t off  = w->pBufferInfo ? (uint64_t)w->pBufferInfo[d].offset : 0;
                    uint64_t rng  = w->pBufferInfo ? (uint64_t)w->pBufferInfo[d].range : 0;
                    alr_vk_enc_gen_update_descriptor_sets_buffer_info(&e, vbuf, off, rng);
                }
            }
        }
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
        free(req);
    }
}

static VkResult VKAPI_CALL alr_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkRenderPass *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device; uint32_t vid; int32_t res = 0; uint32_t i, j;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vrpass, 1);
    if (alr_icd_ring_ok()) {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        size_t cap = 512;
        cap += (size_t)pCreateInfo->attachmentCount * 40;
        cap += (size_t)pCreateInfo->dependencyCount * 32;
        for (i = 0; i < pCreateInfo->subpassCount; ++i) {
            const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[i];
            cap += 40 + (size_t)(sp->inputAttachmentCount + sp->colorAttachmentCount) * 8
                 + (size_t)(sp->colorAttachmentCount + sp->preserveAttachmentCount) * 8;
        }
        uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_create_render_pass_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags);
        /* attachments */
        alr_vk_enc_gen_create_render_pass_attachment_count(&e, pCreateInfo->attachmentCount);
        for (i = 0; i < pCreateInfo->attachmentCount; ++i) {
            const VkAttachmentDescription *at = &pCreateInfo->pAttachments[i];
            alr_vk_enc_gen_create_render_pass_attachment(&e, (uint32_t)at->flags, (uint32_t)at->format,
                              (uint32_t)at->samples, (uint32_t)at->loadOp, (uint32_t)at->storeOp,
                              (uint32_t)at->stencilLoadOp, (uint32_t)at->stencilStoreOp,
                              (uint32_t)at->initialLayout, (uint32_t)at->finalLayout);
        }
        /* subpasses (with nested reference arrays) */
        alr_vk_enc_gen_create_render_pass_subpass_count(&e, pCreateInfo->subpassCount);
        for (i = 0; i < pCreateInfo->subpassCount; ++i) {
            const VkSubpassDescription *sp = &pCreateInfo->pSubpasses[i];
            uint32_t hasDepth = sp->pDepthStencilAttachment ? 1u : 0u;
            uint32_t resCount = sp->pResolveAttachments ? sp->colorAttachmentCount : 0u;
            alr_vk_enc_gen_create_render_pass_subpass_begin(&e, (uint32_t)sp->flags,
                              (uint32_t)sp->pipelineBindPoint, sp->inputAttachmentCount,
                              sp->colorAttachmentCount, resCount, hasDepth,
                              sp->preserveAttachmentCount);
            for (j = 0; j < sp->inputAttachmentCount; ++j)
                alr_vk_enc_gen_create_render_pass_ref(&e, sp->pInputAttachments[j].attachment, (uint32_t)sp->pInputAttachments[j].layout);
            for (j = 0; j < sp->colorAttachmentCount; ++j)
                alr_vk_enc_gen_create_render_pass_ref(&e, sp->pColorAttachments[j].attachment, (uint32_t)sp->pColorAttachments[j].layout);
            for (j = 0; j < resCount; ++j)
                alr_vk_enc_gen_create_render_pass_ref(&e, sp->pResolveAttachments[j].attachment, (uint32_t)sp->pResolveAttachments[j].layout);
            if (hasDepth)
                alr_vk_enc_gen_create_render_pass_ref(&e, sp->pDepthStencilAttachment->attachment, (uint32_t)sp->pDepthStencilAttachment->layout);
            for (j = 0; j < sp->preserveAttachmentCount; ++j)
                alr_vk_enc_gen_create_render_pass_preserve(&e, sp->pPreserveAttachments[j]);
        }
        /* dependencies */
        alr_vk_enc_gen_create_render_pass_dependency_count(&e, pCreateInfo->dependencyCount);
        for (i = 0; i < pCreateInfo->dependencyCount; ++i) {
            const VkSubpassDependency *dp = &pCreateInfo->pDependencies[i];
            alr_vk_enc_gen_create_render_pass_dependency(&e, dp->srcSubpass, dp->dstSubpass,
                              (uint32_t)dp->srcStageMask, (uint32_t)dp->dstStageMask,
                              (uint32_t)dp->srcAccessMask, (uint32_t)dp->dstAccessMask,
                              (uint32_t)dp->dependencyFlags);
        }
        alr_vk_gen_pnext_count(&e, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_RENDER_PASS, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        free(req);
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkRenderPass)(uintptr_t)vid;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)renderPass;
    if (!dev || renderPass == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_render_pass(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator, VkFramebuffer *pHandle) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vid; int32_t res = 0; uint32_t ai;
    if (!dev || !pCreateInfo || !pHandle) return VK_ERROR_INITIALIZATION_FAILED;
    vid = alr_alloc(&g_next_vfb, 1);
    if (alr_icd_ring_ok()) {
        AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        size_t cap = 256;
        cap += (size_t)pCreateInfo->attachmentCount * 24;
        uint8_t *req = (uint8_t *)malloc(cap);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        alr_vk_enc_init(&e, req, cap);
        alr_vk_enc_gen_create_framebuffer_begin(&e, dev->vdev, vid, (uint32_t)pCreateInfo->flags, (uint32_t)(uintptr_t)pCreateInfo->renderPass, (uint32_t)pCreateInfo->width, (uint32_t)pCreateInfo->height, (uint32_t)pCreateInfo->layers);
        { uint32_t n = pCreateInfo->attachmentCount; if (n > 4096) n = 4096;
          alr_vk_enc_gen_create_framebuffer_attachments_count(&e, n);
          for (ai = 0; ai < n; ++ai) {
              alr_vk_enc_gen_create_framebuffer_attachments_elem(&e, (uint32_t)(uintptr_t)pCreateInfo->pAttachments[ai]); } }
        alr_vk_gen_pnext_count(&e, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_gen_scan_result(reply, rlen, (uint16_t)ALR_VK_GEN_REPLY_CREATE_FRAMEBUFFER, &res);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        free(req);
    }
    if (res != 0) return (VkResult)res;
    *pHandle = (VkFramebuffer)(uintptr_t)vid;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                          const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vh = (uint32_t)(uintptr_t)framebuffer;
    if (!dev || framebuffer == 0 || !alr_icd_ring_ok()) return;
    uint8_t req[24]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_gen_destroy_framebuffer(&e, dev->vdev, vh);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

#endif  // ALR_ICD_GEN_DEFINE

#ifdef ALR_ICD_GEN_TABLE
    ALR_ENTRY("vkAllocateMemory", alr_vkAllocateMemory),
    ALR_ENTRY("vkMapMemory", alr_vkMapMemory),
    ALR_ENTRY("vkUnmapMemory", alr_vkUnmapMemory),
    ALR_ENTRY("vkFlushMappedMemoryRanges", alr_vkFlushMappedMemoryRanges),
    ALR_ENTRY("vkFreeMemory", alr_vkFreeMemory),
    ALR_ENTRY("vkCreateBuffer", alr_vkCreateBuffer),
    ALR_ENTRY("vkDestroyBuffer", alr_vkDestroyBuffer),
    ALR_ENTRY("vkGetBufferMemoryRequirements", alr_vkGetBufferMemoryRequirements),
    ALR_ENTRY("vkBindBufferMemory", alr_vkBindBufferMemory),
    ALR_ENTRY("vkCreateImage", alr_vkCreateImage),
    ALR_ENTRY("vkDestroyImage", alr_vkDestroyImage),
    ALR_ENTRY("vkGetImageMemoryRequirements", alr_vkGetImageMemoryRequirements),
    ALR_ENTRY("vkBindImageMemory", alr_vkBindImageMemory),
    ALR_ENTRY("vkCreateImageView", alr_vkCreateImageView),
    ALR_ENTRY("vkDestroyImageView", alr_vkDestroyImageView),
    ALR_ENTRY("vkCreatePipelineCache", alr_vkCreatePipelineCache),
    ALR_ENTRY("vkDestroyPipelineCache", alr_vkDestroyPipelineCache),
    ALR_ENTRY("vkCreateSampler", alr_vkCreateSampler),
    ALR_ENTRY("vkDestroySampler", alr_vkDestroySampler),
    ALR_ENTRY("vkCreateFence", alr_vkCreateFence),
    ALR_ENTRY("vkDestroyFence", alr_vkDestroyFence),
    ALR_ENTRY("vkCreateSemaphore", alr_vkCreateSemaphore),
    ALR_ENTRY("vkDestroySemaphore", alr_vkDestroySemaphore),
    ALR_ENTRY("vkCreateEvent", alr_vkCreateEvent),
    ALR_ENTRY("vkDestroyEvent", alr_vkDestroyEvent),
    ALR_ENTRY("vkCreateQueryPool", alr_vkCreateQueryPool),
    ALR_ENTRY("vkDestroyQueryPool", alr_vkDestroyQueryPool),
    ALR_ENTRY("vkCreateDescriptorSetLayout", alr_vkCreateDescriptorSetLayout),
    ALR_ENTRY("vkDestroyDescriptorSetLayout", alr_vkDestroyDescriptorSetLayout),
    ALR_ENTRY("vkCreatePipelineLayout", alr_vkCreatePipelineLayout),
    ALR_ENTRY("vkDestroyPipelineLayout", alr_vkDestroyPipelineLayout),
    ALR_ENTRY("vkCreateDescriptorPool", alr_vkCreateDescriptorPool),
    ALR_ENTRY("vkDestroyDescriptorPool", alr_vkDestroyDescriptorPool),
    ALR_ENTRY("vkAllocateDescriptorSets", alr_vkAllocateDescriptorSets),
    ALR_ENTRY("vkFreeDescriptorSets", alr_vkFreeDescriptorSets),
    ALR_ENTRY("vkUpdateDescriptorSets", alr_vkUpdateDescriptorSets),
    ALR_ENTRY("vkCreateRenderPass", alr_vkCreateRenderPass),
    ALR_ENTRY("vkDestroyRenderPass", alr_vkDestroyRenderPass),
    ALR_ENTRY("vkCreateFramebuffer", alr_vkCreateFramebuffer),
    ALR_ENTRY("vkDestroyFramebuffer", alr_vkDestroyFramebuffer),
#endif  // ALR_ICD_GEN_TABLE
