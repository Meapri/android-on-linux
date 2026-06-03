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
#endif  // ALR_ICD_GEN_TABLE
