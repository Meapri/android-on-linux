/* alr_icd_cmd_entrypoints.inc — the GUEST ICD's command-buffer RECORDING entrypoints (the
 * vkCmd* / vkBeginCommandBuffer / vkEndCommandBuffer / vkQueueSubmit / fence-sync family).
 *
 * WHY THIS FILE EXISTS (the wave-8 wiring step): ANGLE's RendererVk resolves its ENTIRE
 * device dispatch table up front via vkGetDeviceProcAddr at device-init. If ANY render-
 * critical fn comes back NULL, ANGLE aborts (SIGABRT) before it ever draws. The recorder
 * machinery (alr_icd_cmd_record.h) + the host servicer (alr_gpu_vk_cmd_dispatch.hpp) were
 * already built, but the ICD had NO vkCmd / vkQueueSubmit entrypoints to hand back, so GDPA
 * returned NULL for the whole family. THIS file defines those entrypoints (each RECORDS one
 * cmd-record into its command buffer's guest-local log via alr_icd_cmd_record.h, EXACTLY in
 * the field order the host cmd_decode_one reads back), and alr_lookup registers them — so
 * GDPA returns non-NULL for every render fn ANGLE caches.
 *
 * THE RECORDING MODEL (see alr_gpu_vk_cmdlog.hpp — the source of truth): vkCmd* calls do NOT
 * round-trip; each APPENDS to a per-command-buffer byte log (cb->log). vkQueueSubmit ships
 * the log(s) ONCE over the ring (inline path); the host replays them into a real Mali
 * VkCommandBuffer + issues the real submit. Fences/semaphores are created by the generated
 * create-forwards band (vkCreateFence carries a virtual id as the VkFence handle); the
 * submit/wait ops here reference those vids via (uint32_t)(uintptr_t)handle.
 *
 * #include'd into alr_icd_vulkan.c (one TU) AFTER alr_icd_cmd_record.h, the AlrIcdDevice /
 * AlrIcdQueue / AlrIcdCommandBuffer types, alr_alloc, g_next_v* counters, alr_icd_ring_ok,
 * and alr_icd_roundtrip — all of which it uses. Defined in two passes like the generated
 * inc: ALR_ICD_CMD_DEFINE emits the functions; ALR_ICD_CMD_TABLE emits the alr_lookup rows.
 */

#ifdef ALR_ICD_CMD_DEFINE

/* ---- Vulkan struct/enum types this TU dereferences that alr_icd_vk_min.h does not already
 * define. Layouts are the official Vulkan ABI (stable); guarded so a future vk_min.h that
 * adds them does not collide. The handle/flag typedefs (VkDeviceSize, VkBuffer, VkImage,
 * VkRenderPass, VkFramebuffer, VkPipeline, VkPipelineLayout, VkDescriptorSet, VkImageLayout,
 * VkPipelineStageFlags, VkAccessFlags, VkDependencyFlags, VkShaderStageFlags, VkExtent2D/3D,
 * VkImageSubresourceRange, VkPipelineBindPoint, VkCommandBuffer, VkQueue, VkFence,
 * VkSemaphore, VkSubmitInfo) ARE in vk_min.h. ---- */

#ifndef VK_ALR_CMD_TYPES_DEFINED
#define VK_ALR_CMD_TYPES_DEFINED 1

typedef uint32_t VkCommandBufferUsageFlags;
typedef uint32_t VkCommandBufferResetFlags;
typedef uint32_t VkCommandPoolResetFlags;
typedef int32_t  VkSubpassContents;
typedef int32_t  VkIndexType;

/* VkOffset2D/3D, VkRect2D, VkViewport come from alr_icd_vk_min.h (it defines them for the
 * pipeline viewport/scissor state too — VK_ALR_GEOM_TYPES_DEFINED). If a future vk_min.h
 * drops them, define them here as a fallback. */
#ifndef VK_ALR_GEOM_TYPES_DEFINED
typedef struct VkOffset2D { int32_t x; int32_t y; } VkOffset2D;
typedef struct VkOffset3D { int32_t x; int32_t y; int32_t z; } VkOffset3D;
typedef struct VkRect2D { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct VkViewport {
    float x; float y; float width; float height; float minDepth; float maxDepth;
} VkViewport;
#endif

/* VkClearValue is a 16-byte union (4 × float color | depth+stencil). The host reads it as a
 * raw 16-byte blob, so the exact union member does not matter on the wire — only the size. */
typedef union VkClearColorValue {
    float    float32[4];
    int32_t  int32[4];
    uint32_t uint32[4];
} VkClearColorValue;
typedef struct VkClearDepthStencilValue { float depth; uint32_t stencil; } VkClearDepthStencilValue;
typedef union VkClearValue {
    VkClearColorValue           color;
    VkClearDepthStencilValue    depthStencil;
} VkClearValue;

typedef struct VkCommandBufferInheritanceInfo VkCommandBufferInheritanceInfo;  /* opaque (we drop it) */
typedef struct VkCommandBufferBeginInfo {
    VkStructureType                          sType;
    const void*                              pNext;
    VkCommandBufferUsageFlags                flags;
    const VkCommandBufferInheritanceInfo*    pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct VkRenderPassBeginInfo {
    VkStructureType        sType;
    const void*            pNext;
    VkRenderPass           renderPass;
    VkFramebuffer          framebuffer;
    VkRect2D               renderArea;
    uint32_t               clearValueCount;
    const VkClearValue*    pClearValues;
} VkRenderPassBeginInfo;

/* core-1.2 BeginRenderPass2 inherited-info reduced to contents (its pNext dropped — the
 * host band reads only `contents`, matching the bring-up scope). */
typedef struct VkSubpassBeginInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkSubpassContents  contents;
} VkSubpassBeginInfo;
typedef struct VkSubpassEndInfo {
    VkStructureType    sType;
    const void*        pNext;
} VkSubpassEndInfo;

typedef struct VkBufferCopy {
    VkDeviceSize srcOffset; VkDeviceSize dstOffset; VkDeviceSize size;
} VkBufferCopy;

typedef struct VkImageSubresourceLayers {
    VkFlags  aspectMask;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} VkImageSubresourceLayers;

typedef struct VkImageCopy {
    VkImageSubresourceLayers srcSubresource;
    VkOffset3D               srcOffset;
    VkImageSubresourceLayers dstSubresource;
    VkOffset3D               dstOffset;
    VkExtent3D               extent;
} VkImageCopy;

typedef struct VkBufferImageCopy {
    VkDeviceSize             bufferOffset;
    uint32_t                 bufferRowLength;
    uint32_t                 bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D               imageOffset;
    VkExtent3D               imageExtent;
} VkBufferImageCopy;

typedef struct VkMemoryBarrier {
    VkStructureType sType;
    const void*     pNext;
    VkAccessFlags   srcAccessMask;
    VkAccessFlags   dstAccessMask;
} VkMemoryBarrier;

typedef struct VkBufferMemoryBarrier {
    VkStructureType sType;
    const void*     pNext;
    VkAccessFlags   srcAccessMask;
    VkAccessFlags   dstAccessMask;
    uint32_t        srcQueueFamilyIndex;
    uint32_t        dstQueueFamilyIndex;
    VkBuffer        buffer;
    VkDeviceSize    offset;
    VkDeviceSize    size;
} VkBufferMemoryBarrier;

typedef struct VkImageMemoryBarrier {
    VkStructureType         sType;
    const void*             pNext;
    VkAccessFlags           srcAccessMask;
    VkAccessFlags           dstAccessMask;
    VkImageLayout           oldLayout;
    VkImageLayout           newLayout;
    uint32_t                srcQueueFamilyIndex;
    uint32_t                dstQueueFamilyIndex;
    VkImage                 image;
    VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier;

typedef struct VkClearAttachment {
    VkFlags        aspectMask;
    uint32_t       colorAttachment;
    VkClearValue   clearValue;
} VkClearAttachment;

typedef struct VkClearRect {
    VkRect2D rect;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} VkClearRect;

#endif  /* VK_ALR_CMD_TYPES_DEFINED */

/* Map a VkCommandBuffer handle to its AlrIcdCommandBuffer + its log. A NULL cb (or one with
 * no device) is a no-op for recording (the contract: recording into a bad cb does nothing,
 * the submit then ships an empty/short log). */
#define ALR_CB(commandBuffer) ((AlrIcdCommandBuffer *)(commandBuffer))
#define ALR_VID(h) ((uint32_t)(uintptr_t)(h))

/* ===========================================================================
 * Command-buffer lifecycle: begin / end / reset (+ pool reset, free).
 * =========================================================================== */
static VkResult VKAPI_CALL alr_vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                    const VkCommandBufferBeginInfo *pBeginInfo) {
    (void)pBeginInfo;
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    alr_icd_cmdlog_reset(&cb->log);   /* fresh recording */
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL alr_vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    (void)alr_icd_cmdlog_end(&cb->log);   /* cap with the END terminator */
    return cb->log.overflow ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
static VkResult VKAPI_CALL alr_vkResetCommandBuffer(VkCommandBuffer commandBuffer,
                                                    VkCommandBufferResetFlags flags) {
    (void)flags;
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    alr_icd_cmdlog_reset(&cb->log);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL alr_vkResetCommandPool(VkDevice device, VkCommandPool commandPool,
                                                  VkCommandPoolResetFlags flags) {
    /* The host owns the real pool; the guest has no per-pool list of command buffers to walk
     * here, and each cb is reset at its next vkBeginCommandBuffer. Report success (the recorded
     * logs are reset lazily on begin — ANGLE always re-begins a cb before re-recording). */
    (void)device; (void)commandPool; (void)flags;
    return VK_SUCCESS;
}
static void VKAPI_CALL alr_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                uint32_t commandBufferCount,
                                                const VkCommandBuffer *pCommandBuffers) {
    (void)device; (void)commandPool;
    if (!pCommandBuffers) return;
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        AlrIcdCommandBuffer *cb = ALR_CB(pCommandBuffers[i]);
        if (cb) { alr_icd_cmdlog_free(&cb->log); free(cb); }
    }
}

/* ===========================================================================
 * vkCmd* — each RECORDS one record into cb->log (no ring round-trip). Field order MUST equal
 * the host cmd_decode_one (alr_gpu_vk_cmdlog.hpp). Handles -> guest virtual ids via ALR_VID.
 * =========================================================================== */

static void VKAPI_CALL alr_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                const VkRenderPassBeginInfo *pRenderPassBegin,
                                                VkSubpassContents contents) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb || !pRenderPassBegin) return;
    uint32_t cc = pRenderPassBegin->clearValueCount;
    if (cc > 16) cc = 16;
    alr_icd_cmd_begin_render_pass(&cb->log,
        ALR_VID(pRenderPassBegin->renderPass), ALR_VID(pRenderPassBegin->framebuffer),
        pRenderPassBegin->renderArea.offset.x, pRenderPassBegin->renderArea.offset.y,
        pRenderPassBegin->renderArea.extent.width, pRenderPassBegin->renderArea.extent.height,
        (uint32_t)contents, cc);
    for (uint32_t i = 0; i < cc; ++i)
        alr_icd_cmdlog_raw(&cb->log, &pRenderPassBegin->pClearValues[i], 16);  /* raw VkClearValue */
}
static void VKAPI_CALL alr_vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_end_render_pass(&cb->log);
}
static void VKAPI_CALL alr_vkCmdNextSubpass(VkCommandBuffer commandBuffer,
                                            VkSubpassContents contents) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_next_subpass(&cb->log, (uint32_t)contents);
}
/* core-1.2 *2 variants: same recorded payload, different opcode. The SubpassBeginInfo/EndInfo
 * are reduced to `contents` (host band drops their pNext). */
static void VKAPI_CALL alr_vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                 const VkRenderPassBeginInfo *pRenderPassBegin,
                                                 const VkSubpassBeginInfo *pSubpassBeginInfo) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb || !pRenderPassBegin) return;
    uint32_t contents = pSubpassBeginInfo ? (uint32_t)pSubpassBeginInfo->contents : 0u;
    uint32_t cc = pRenderPassBegin->clearValueCount;
    if (cc > 16) cc = 16;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_BEGIN_RENDER_PASS2);
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(pRenderPassBegin->renderPass));
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(pRenderPassBegin->framebuffer));
    alr_icd_cmdlog_i32(&cb->log, pRenderPassBegin->renderArea.offset.x);
    alr_icd_cmdlog_i32(&cb->log, pRenderPassBegin->renderArea.offset.y);
    alr_icd_cmdlog_u32(&cb->log, pRenderPassBegin->renderArea.extent.width);
    alr_icd_cmdlog_u32(&cb->log, pRenderPassBegin->renderArea.extent.height);
    alr_icd_cmdlog_u32(&cb->log, contents);
    alr_icd_cmdlog_u32(&cb->log, cc);
    for (uint32_t i = 0; i < cc; ++i)
        alr_icd_cmdlog_raw(&cb->log, &pRenderPassBegin->pClearValues[i], 16);
}
static void VKAPI_CALL alr_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                               const VkSubpassEndInfo *pSubpassEndInfo) {
    (void)pSubpassEndInfo;
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_END_RENDER_PASS2);
}
static void VKAPI_CALL alr_vkCmdNextSubpass2(VkCommandBuffer commandBuffer,
                                             const VkSubpassBeginInfo *pSubpassBeginInfo,
                                             const VkSubpassEndInfo *pSubpassEndInfo) {
    (void)pSubpassEndInfo;
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    uint32_t contents = pSubpassBeginInfo ? (uint32_t)pSubpassBeginInfo->contents : 0u;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_NEXT_SUBPASS2);
    alr_icd_cmdlog_u32(&cb->log, contents);
}

static void VKAPI_CALL alr_vkCmdBindPipeline(VkCommandBuffer commandBuffer,
                                             VkPipelineBindPoint pipelineBindPoint,
                                             VkPipeline pipeline) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_bind_pipeline(&cb->log, (uint32_t)pipelineBindPoint, ALR_VID(pipeline));
}
static void VKAPI_CALL alr_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                                   VkPipelineBindPoint pipelineBindPoint,
                                                   VkPipelineLayout layout, uint32_t firstSet,
                                                   uint32_t descriptorSetCount,
                                                   const VkDescriptorSet *pDescriptorSets,
                                                   uint32_t dynamicOffsetCount,
                                                   const uint32_t *pDynamicOffsets) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_BIND_DESCRIPTOR_SETS);
    alr_icd_cmdlog_u32(&cb->log, (uint32_t)pipelineBindPoint);
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(layout));
    alr_icd_cmdlog_u32(&cb->log, firstSet);
    alr_icd_cmdlog_u32(&cb->log, descriptorSetCount);
    for (uint32_t i = 0; i < descriptorSetCount; ++i)
        alr_icd_cmdlog_u32(&cb->log, pDescriptorSets ? ALR_VID(pDescriptorSets[i]) : 0u);
    alr_icd_cmdlog_u32(&cb->log, dynamicOffsetCount);
    for (uint32_t i = 0; i < dynamicOffsetCount; ++i)
        alr_icd_cmdlog_u32(&cb->log, pDynamicOffsets ? pDynamicOffsets[i] : 0u);
}
static void VKAPI_CALL alr_vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                                  uint32_t firstBinding, uint32_t bindingCount,
                                                  const VkBuffer *pBuffers,
                                                  const VkDeviceSize *pOffsets) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmd_bind_vertex_buffers_begin(&cb->log, firstBinding, bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i)
        alr_icd_cmd_bind_vertex_buffer_entry(&cb->log,
            pBuffers ? ALR_VID(pBuffers[i]) : 0u,
            pOffsets ? (uint64_t)pOffsets[i] : 0u);
}
static void VKAPI_CALL alr_vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                VkDeviceSize offset, VkIndexType indexType) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_bind_index_buffer(&cb->log, ALR_VID(buffer), (uint64_t)offset,
                                          (uint32_t)indexType);
}
static void VKAPI_CALL alr_vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
                                     uint32_t instanceCount, uint32_t firstVertex,
                                     uint32_t firstInstance) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_draw(&cb->log, vertexCount, instanceCount, firstVertex, firstInstance);
}
static void VKAPI_CALL alr_vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                                            uint32_t instanceCount, uint32_t firstIndex,
                                            int32_t vertexOffset, uint32_t firstInstance) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_draw_indexed(&cb->log, indexCount, instanceCount, firstIndex,
                                     vertexOffset, firstInstance);
}
static void VKAPI_CALL alr_vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                             VkDeviceSize offset, uint32_t drawCount,
                                             uint32_t stride) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_draw_indirect(&cb->log, ALR_VID(buffer), (uint64_t)offset, drawCount,
                                      stride);
}
static void VKAPI_CALL alr_vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX,
                                         uint32_t groupCountY, uint32_t groupCountZ) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (cb) alr_icd_cmd_dispatch(&cb->log, groupCountX, groupCountY, groupCountZ);
}
static void VKAPI_CALL alr_vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport,
                                            uint32_t viewportCount, const VkViewport *pViewports) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_SET_VIEWPORT);
    alr_icd_cmdlog_u32(&cb->log, firstViewport);
    alr_icd_cmdlog_u32(&cb->log, viewportCount);
    for (uint32_t i = 0; i < viewportCount; ++i) {
        const VkViewport *v = &pViewports[i];
        alr_icd_cmdlog_f32(&cb->log, v->x);        alr_icd_cmdlog_f32(&cb->log, v->y);
        alr_icd_cmdlog_f32(&cb->log, v->width);    alr_icd_cmdlog_f32(&cb->log, v->height);
        alr_icd_cmdlog_f32(&cb->log, v->minDepth); alr_icd_cmdlog_f32(&cb->log, v->maxDepth);
    }
}
static void VKAPI_CALL alr_vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor,
                                           uint32_t scissorCount, const VkRect2D *pScissors) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_SET_SCISSOR);
    alr_icd_cmdlog_u32(&cb->log, firstScissor);
    alr_icd_cmdlog_u32(&cb->log, scissorCount);
    for (uint32_t i = 0; i < scissorCount; ++i) {
        const VkRect2D *s = &pScissors[i];
        alr_icd_cmdlog_i32(&cb->log, s->offset.x); alr_icd_cmdlog_i32(&cb->log, s->offset.y);
        alr_icd_cmdlog_u32(&cb->log, s->extent.width); alr_icd_cmdlog_u32(&cb->log, s->extent.height);
    }
}
/* PipelineBarrier (+2 share the same recorded payload). The host reads three arrays:
 * memory { srcAccess,dstAccess }, buffer { srcAccess,dstAccess,srcQF,dstQF,vbuffer,off,size },
 * image { srcAccess,dstAccess,oldLayout,newLayout,srcQF,dstQF,vimage,aspect,baseMip,levelCount,
 * baseLayer,layerCount }. */
static void alr_icd_record_barrier(AlrIcdCmdLog *L, uint16_t op,
                                   VkPipelineStageFlags srcStageMask,
                                   VkPipelineStageFlags dstStageMask,
                                   VkDependencyFlags dependencyFlags,
                                   uint32_t memCount, const VkMemoryBarrier *mem,
                                   uint32_t bufCount, const VkBufferMemoryBarrier *buf,
                                   uint32_t imgCount, const VkImageMemoryBarrier *img) {
    alr_icd_cmdlog_cmd(L, op);
    alr_icd_cmdlog_u32(L, (uint32_t)srcStageMask);
    alr_icd_cmdlog_u32(L, (uint32_t)dstStageMask);
    alr_icd_cmdlog_u32(L, (uint32_t)dependencyFlags);
    alr_icd_cmdlog_u32(L, memCount);
    for (uint32_t i = 0; i < memCount; ++i) {
        alr_icd_cmdlog_u32(L, (uint32_t)mem[i].srcAccessMask);
        alr_icd_cmdlog_u32(L, (uint32_t)mem[i].dstAccessMask);
    }
    alr_icd_cmdlog_u32(L, bufCount);
    for (uint32_t i = 0; i < bufCount; ++i) {
        alr_icd_cmdlog_u32(L, (uint32_t)buf[i].srcAccessMask);
        alr_icd_cmdlog_u32(L, (uint32_t)buf[i].dstAccessMask);
        alr_icd_cmdlog_u32(L, buf[i].srcQueueFamilyIndex);
        alr_icd_cmdlog_u32(L, buf[i].dstQueueFamilyIndex);
        alr_icd_cmdlog_u32(L, ALR_VID(buf[i].buffer));
        alr_icd_cmdlog_u64(L, (uint64_t)buf[i].offset);
        alr_icd_cmdlog_u64(L, (uint64_t)buf[i].size);
    }
    alr_icd_cmdlog_u32(L, imgCount);
    for (uint32_t i = 0; i < imgCount; ++i) {
        alr_icd_cmdlog_u32(L, (uint32_t)img[i].srcAccessMask);
        alr_icd_cmdlog_u32(L, (uint32_t)img[i].dstAccessMask);
        alr_icd_cmdlog_u32(L, (uint32_t)img[i].oldLayout);
        alr_icd_cmdlog_u32(L, (uint32_t)img[i].newLayout);
        alr_icd_cmdlog_u32(L, img[i].srcQueueFamilyIndex);
        alr_icd_cmdlog_u32(L, img[i].dstQueueFamilyIndex);
        alr_icd_cmdlog_u32(L, ALR_VID(img[i].image));
        alr_icd_cmdlog_u32(L, img[i].subresourceRange.aspectMask);
        alr_icd_cmdlog_u32(L, img[i].subresourceRange.baseMipLevel);
        alr_icd_cmdlog_u32(L, img[i].subresourceRange.levelCount);
        alr_icd_cmdlog_u32(L, img[i].subresourceRange.baseArrayLayer);
        alr_icd_cmdlog_u32(L, img[i].subresourceRange.layerCount);
    }
}
static void VKAPI_CALL alr_vkCmdPipelineBarrier(VkCommandBuffer commandBuffer,
                                                VkPipelineStageFlags srcStageMask,
                                                VkPipelineStageFlags dstStageMask,
                                                VkDependencyFlags dependencyFlags,
                                                uint32_t memoryBarrierCount,
                                                const VkMemoryBarrier *pMemoryBarriers,
                                                uint32_t bufferMemoryBarrierCount,
                                                const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                                                uint32_t imageMemoryBarrierCount,
                                                const VkImageMemoryBarrier *pImageMemoryBarriers) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_record_barrier(&cb->log, ALR_ICD_CMD_PIPELINE_BARRIER, srcStageMask, dstStageMask,
                           dependencyFlags, memoryBarrierCount, pMemoryBarriers,
                           bufferMemoryBarrierCount, pBufferMemoryBarriers,
                           imageMemoryBarrierCount, pImageMemoryBarriers);
}
static void VKAPI_CALL alr_vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
                                           VkBuffer dstBuffer, uint32_t regionCount,
                                           const VkBufferCopy *pRegions) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_COPY_BUFFER);
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(srcBuffer));
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(dstBuffer));
    alr_icd_cmdlog_u32(&cb->log, regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) {
        alr_icd_cmdlog_u64(&cb->log, (uint64_t)pRegions[i].srcOffset);
        alr_icd_cmdlog_u64(&cb->log, (uint64_t)pRegions[i].dstOffset);
        alr_icd_cmdlog_u64(&cb->log, (uint64_t)pRegions[i].size);
    }
}
static void VKAPI_CALL alr_vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                          VkImageLayout srcImageLayout, VkImage dstImage,
                                          VkImageLayout dstImageLayout, uint32_t regionCount,
                                          const VkImageCopy *pRegions) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_COPY_IMAGE);
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(srcImage));
    alr_icd_cmdlog_u32(&cb->log, (uint32_t)srcImageLayout);
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(dstImage));
    alr_icd_cmdlog_u32(&cb->log, (uint32_t)dstImageLayout);
    alr_icd_cmdlog_u32(&cb->log, regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkImageCopy *rg = &pRegions[i];
        /* host order: u32 srcAspect,srcMip,srcBaseLayer,srcLayerCount; i32 srcX,srcY,srcZ;
         *             u32 dstAspect,dstMip,dstBaseLayer,dstLayerCount; i32 dstX,dstY,dstZ;
         *             u32 w,h,d */
        alr_icd_cmdlog_u32(&cb->log, rg->srcSubresource.aspectMask);
        alr_icd_cmdlog_u32(&cb->log, rg->srcSubresource.mipLevel);
        alr_icd_cmdlog_u32(&cb->log, rg->srcSubresource.baseArrayLayer);
        alr_icd_cmdlog_u32(&cb->log, rg->srcSubresource.layerCount);
        alr_icd_cmdlog_i32(&cb->log, rg->srcOffset.x);
        alr_icd_cmdlog_i32(&cb->log, rg->srcOffset.y);
        alr_icd_cmdlog_i32(&cb->log, rg->srcOffset.z);
        alr_icd_cmdlog_u32(&cb->log, rg->dstSubresource.aspectMask);
        alr_icd_cmdlog_u32(&cb->log, rg->dstSubresource.mipLevel);
        alr_icd_cmdlog_u32(&cb->log, rg->dstSubresource.baseArrayLayer);
        alr_icd_cmdlog_u32(&cb->log, rg->dstSubresource.layerCount);
        alr_icd_cmdlog_i32(&cb->log, rg->dstOffset.x);
        alr_icd_cmdlog_i32(&cb->log, rg->dstOffset.y);
        alr_icd_cmdlog_i32(&cb->log, rg->dstOffset.z);
        alr_icd_cmdlog_u32(&cb->log, rg->extent.width);
        alr_icd_cmdlog_u32(&cb->log, rg->extent.height);
        alr_icd_cmdlog_u32(&cb->log, rg->extent.depth);
    }
}
static void VKAPI_CALL alr_vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
                                                  VkImage dstImage, VkImageLayout dstImageLayout,
                                                  uint32_t regionCount,
                                                  const VkBufferImageCopy *pRegions) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_COPY_BUFFER_TO_IMAGE);
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(srcBuffer));
    alr_icd_cmdlog_u32(&cb->log, ALR_VID(dstImage));
    alr_icd_cmdlog_u32(&cb->log, (uint32_t)dstImageLayout);
    alr_icd_cmdlog_u32(&cb->log, regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferImageCopy *rg = &pRegions[i];
        /* host order: u64 bufferOffset; u32 bufferRowLength,bufferImageHeight,aspect,mip,
         *             baseLayer,layerCount; i32 x,y,z; u32 w,h,d */
        alr_icd_cmdlog_u64(&cb->log, (uint64_t)rg->bufferOffset);
        alr_icd_cmdlog_u32(&cb->log, rg->bufferRowLength);
        alr_icd_cmdlog_u32(&cb->log, rg->bufferImageHeight);
        alr_icd_cmdlog_u32(&cb->log, rg->imageSubresource.aspectMask);
        alr_icd_cmdlog_u32(&cb->log, rg->imageSubresource.mipLevel);
        alr_icd_cmdlog_u32(&cb->log, rg->imageSubresource.baseArrayLayer);
        alr_icd_cmdlog_u32(&cb->log, rg->imageSubresource.layerCount);
        alr_icd_cmdlog_i32(&cb->log, rg->imageOffset.x);
        alr_icd_cmdlog_i32(&cb->log, rg->imageOffset.y);
        alr_icd_cmdlog_i32(&cb->log, rg->imageOffset.z);
        alr_icd_cmdlog_u32(&cb->log, rg->imageExtent.width);
        alr_icd_cmdlog_u32(&cb->log, rg->imageExtent.height);
        alr_icd_cmdlog_u32(&cb->log, rg->imageExtent.depth);
    }
}
static void VKAPI_CALL alr_vkCmdPushConstants(VkCommandBuffer commandBuffer,
                                              VkPipelineLayout layout, VkShaderStageFlags stageFlags,
                                              uint32_t offset, uint32_t size, const void *pValues) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    alr_icd_cmd_push_constants(&cb->log, ALR_VID(layout), (uint32_t)stageFlags, offset, size,
                               pValues);
}
static void VKAPI_CALL alr_vkCmdClearAttachments(VkCommandBuffer commandBuffer,
                                                 uint32_t attachmentCount,
                                                 const VkClearAttachment *pAttachments,
                                                 uint32_t rectCount, const VkClearRect *pRects) {
    AlrIcdCommandBuffer *cb = ALR_CB(commandBuffer);
    if (!cb) return;
    /* host order: u32 att_count, att×{ u32 aspectMask, u32 colorAttachment, 16B VkClearValue };
     *             u32 rect_count, rect×{ i32 x, i32 y, u32 w, u32 h, u32 baseArrayLayer, u32 layerCount } */
    alr_icd_cmdlog_cmd(&cb->log, ALR_ICD_CMD_CLEAR_ATTACHMENTS);
    alr_icd_cmdlog_u32(&cb->log, attachmentCount);
    for (uint32_t i = 0; i < attachmentCount; ++i) {
        alr_icd_cmdlog_u32(&cb->log, pAttachments[i].aspectMask);
        alr_icd_cmdlog_u32(&cb->log, pAttachments[i].colorAttachment);
        alr_icd_cmdlog_raw(&cb->log, &pAttachments[i].clearValue, 16);
    }
    alr_icd_cmdlog_u32(&cb->log, rectCount);
    for (uint32_t i = 0; i < rectCount; ++i) {
        alr_icd_cmdlog_i32(&cb->log, pRects[i].rect.offset.x);
        alr_icd_cmdlog_i32(&cb->log, pRects[i].rect.offset.y);
        alr_icd_cmdlog_u32(&cb->log, pRects[i].rect.extent.width);
        alr_icd_cmdlog_u32(&cb->log, pRects[i].rect.extent.height);
        alr_icd_cmdlog_u32(&cb->log, pRects[i].baseArrayLayer);
        alr_icd_cmdlog_u32(&cb->log, pRects[i].layerCount);
    }
}

/* ===========================================================================
 * vkQueueSubmit — ship the recorded log(s) over the ring ONCE; host replays + real-submits.
 * Built with the alr_icd_cmd_submit_* helpers (inline log path). The fence/semaphore handles
 * carry their virtual ids (created by the generated create-forwards band).
 * =========================================================================== */
static VkResult VKAPI_CALL alr_vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                             const VkSubmitInfo *pSubmits, VkFence fence) {
    AlrIcdQueue *q = (AlrIcdQueue *)queue;
    if (!q || !q->dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_SUCCESS;  /* host-less bring-up: accept (no GPU work) */
    uint32_t vfence = (fence != (VkFence)0) ? ALR_VID(fence) : 0u;
    VkResult ret = VK_SUCCESS;
    /* Each VkSubmitInfo becomes one ring SUBMIT op. Size the request to fit the topology +
     * every referenced cmd-buffer's inline log. */
    for (uint32_t s = 0; s < submitCount; ++s) {
        const VkSubmitInfo *si = &pSubmits[s];
        uint32_t total_log = 0, cbc = si->commandBufferCount;
        for (uint32_t i = 0; i < cbc; ++i) {
            AlrIcdCommandBuffer *cb = ALR_CB(si->pCommandBuffers[i]);
            if (cb) total_log += cb->log.len;   /* len already includes the END terminator */
        }
        /* op header (3) + queue/fence (8) + wait list (4 + n*8) + signal list (4 + n*4) +
         * cmd list (4 + n*(4+8+4) + logs) + END (1) + slack. */
        uint32_t need = 64
            + (uint32_t)si->waitSemaphoreCount * 8u
            + (uint32_t)si->signalSemaphoreCount * 4u
            + cbc * 16u + total_log;
        uint8_t *req = (uint8_t *)malloc(need);
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        AlrVkEncoder e;
        alr_vk_enc_init(&e, req, need);
        /* On the LAST submit, attach the fence (Vulkan signals the fence once ALL submits in
         * the batch complete; serialized here, the last op carries it). */
        uint32_t this_fence = (s + 1 == submitCount) ? vfence : 0u;
        alr_icd_cmd_submit_begin(&e, q->vqueue, this_fence);
        alr_icd_cmd_submit_wait_count(&e, si->waitSemaphoreCount);
        for (uint32_t i = 0; i < si->waitSemaphoreCount; ++i) {
            uint32_t vsem = si->pWaitSemaphores ? ALR_VID(si->pWaitSemaphores[i]) : 0u;
            uint32_t stage_lo = si->pWaitDstStageMask ? (uint32_t)si->pWaitDstStageMask[i] : 0u;
            alr_icd_cmd_submit_wait(&e, vsem, stage_lo);
        }
        alr_icd_cmd_submit_signal_count(&e, si->signalSemaphoreCount);
        for (uint32_t i = 0; i < si->signalSemaphoreCount; ++i)
            alr_icd_cmd_submit_signal(&e, si->pSignalSemaphores ? ALR_VID(si->pSignalSemaphores[i]) : 0u);
        alr_icd_cmd_submit_cmd_count(&e, cbc);
        for (uint32_t i = 0; i < cbc; ++i) {
            AlrIcdCommandBuffer *cb = ALR_CB(si->pCommandBuffers[i]);
            if (cb) alr_icd_cmd_submit_cmd_inline(&e, cb->vcmd, cb->log.buf, cb->log.len);
            else    alr_icd_cmd_submit_cmd_inline(&e, 0u, NULL, 0u);
        }
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            int32_t vkr = 0, rr = 0;
            if (rlen && alr_icd_cmd_scan_submit(reply, rlen, q->vqueue, &vkr, &rr)) {
                ALR_ICD_DIAG("vkQueueSubmit: vqueue=%u cbc=%u vk=%d replay=%d", q->vqueue, cbc,
                             vkr, rr);
                if (vkr != 0) ret = (VkResult)vkr;
            } else {
                ALR_ICD_DIAG("vkQueueSubmit: vqueue=%u no reply (rlen=%u)", q->vqueue, rlen);
            }
        } else {
            ret = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        free(req);
        if (ret != VK_SUCCESS) return ret;
    }
    return ret;
}

/* ===========================================================================
 * Sync ring ops: queue/device wait-idle, fence wait/reset/status. These reference the fence
 * vids created by the generated create-forwards band; the host services them in
 * decode_vk_cmd_op against the same VkDecodeState that vkCreateFence populated.
 * =========================================================================== */
static VkResult VKAPI_CALL alr_vkQueueWaitIdle(VkQueue queue) {
    AlrIcdQueue *q = (AlrIcdQueue *)queue;
    if (!q || !q->dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_SUCCESS;
    uint8_t req[16]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_icd_cmd_queue_wait_idle(&e, q->vqueue);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (e.overflow) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    int32_t res = 0;
    if (rlen) (void)alr_icd_cmd_scan_result(reply, rlen, q->vqueue, &res);
    return (VkResult)res;
}
static VkResult VKAPI_CALL alr_vkDeviceWaitIdle(VkDevice device) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_SUCCESS;
    uint8_t req[16]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_icd_cmd_device_wait_idle(&e, dev->vdev);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (e.overflow) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    int32_t res = 0;
    if (rlen) (void)alr_icd_cmd_scan_result(reply, rlen, dev->vdev, &res);
    return (VkResult)res;
}
static VkResult VKAPI_CALL alr_vkWaitForFences(VkDevice device, uint32_t fenceCount,
                                               const VkFence *pFences, VkBool32 waitAll,
                                               uint64_t timeout) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev || !pFences) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_SUCCESS;
    uint32_t need = 32 + fenceCount * 4u;
    uint8_t *req = (uint8_t *)malloc(need);
    if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    AlrVkEncoder e;
    alr_vk_enc_init(&e, req, need);
    alr_icd_cmd_wait_for_fences_begin(&e, dev->vdev, waitAll ? 1u : 0u, timeout, fenceCount);
    for (uint32_t i = 0; i < fenceCount; ++i) alr_vk_enc_u32(&e, ALR_VID(pFences[i]));
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    int32_t res = 0;
    if (!e.overflow) {
        uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
        /* the host keys the wait reply by the FIRST fence vid (cmd_reply_result(..., vfence)). */
        if (rlen) (void)alr_icd_cmd_scan_result(reply, rlen, ALR_VID(pFences[0]), &res);
    } else res = (int32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    free(req);
    return (VkResult)res;
}
static VkResult VKAPI_CALL alr_vkResetFences(VkDevice device, uint32_t fenceCount,
                                             const VkFence *pFences) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev || !pFences) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_SUCCESS;
    uint32_t need = 24 + fenceCount * 4u;
    uint8_t *req = (uint8_t *)malloc(need);
    if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    AlrVkEncoder e;
    alr_vk_enc_init(&e, req, need);
    alr_icd_cmd_reset_fences_begin(&e, dev->vdev, fenceCount);
    for (uint32_t i = 0; i < fenceCount; ++i) alr_vk_enc_u32(&e, ALR_VID(pFences[i]));
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    int32_t res = 0;
    if (!e.overflow) {
        uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
        if (rlen) (void)alr_icd_cmd_scan_result(reply, rlen, ALR_VID(pFences[0]), &res);
    } else res = (int32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
    free(req);
    return (VkResult)res;
}
static VkResult VKAPI_CALL alr_vkGetFenceStatus(VkDevice device, VkFence fence) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_SUCCESS;
    uint8_t req[16]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_icd_cmd_get_fence_status(&e, dev->vdev, ALR_VID(fence));
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (e.overflow) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    int32_t res = 0;
    if (rlen) (void)alr_icd_cmd_scan_result(reply, rlen, ALR_VID(fence), &res);
    return (VkResult)res;
}

#endif  /* ALR_ICD_CMD_DEFINE */

/* ===========================================================================
 * The alr_lookup table rows (spliced into alr_lookup via ALR_ICD_CMD_TABLE). Every render-
 * critical fn ANGLE caches at device-init resolves to a NON-NULL recorder/submit entrypoint.
 * =========================================================================== */
#ifdef ALR_ICD_CMD_TABLE
        ALR_ENTRY("vkBeginCommandBuffer", alr_vkBeginCommandBuffer),
        ALR_ENTRY("vkEndCommandBuffer", alr_vkEndCommandBuffer),
        ALR_ENTRY("vkResetCommandBuffer", alr_vkResetCommandBuffer),
        ALR_ENTRY("vkResetCommandPool", alr_vkResetCommandPool),
        ALR_ENTRY("vkFreeCommandBuffers", alr_vkFreeCommandBuffers),
        ALR_ENTRY("vkCmdBeginRenderPass", alr_vkCmdBeginRenderPass),
        ALR_ENTRY("vkCmdEndRenderPass", alr_vkCmdEndRenderPass),
        ALR_ENTRY("vkCmdNextSubpass", alr_vkCmdNextSubpass),
        ALR_ENTRY("vkCmdBeginRenderPass2", alr_vkCmdBeginRenderPass2),
        ALR_ENTRY("vkCmdBeginRenderPass2KHR", alr_vkCmdBeginRenderPass2),
        ALR_ENTRY("vkCmdEndRenderPass2", alr_vkCmdEndRenderPass2),
        ALR_ENTRY("vkCmdEndRenderPass2KHR", alr_vkCmdEndRenderPass2),
        ALR_ENTRY("vkCmdNextSubpass2", alr_vkCmdNextSubpass2),
        ALR_ENTRY("vkCmdNextSubpass2KHR", alr_vkCmdNextSubpass2),
        ALR_ENTRY("vkCmdBindPipeline", alr_vkCmdBindPipeline),
        ALR_ENTRY("vkCmdBindDescriptorSets", alr_vkCmdBindDescriptorSets),
        ALR_ENTRY("vkCmdBindVertexBuffers", alr_vkCmdBindVertexBuffers),
        ALR_ENTRY("vkCmdBindIndexBuffer", alr_vkCmdBindIndexBuffer),
        ALR_ENTRY("vkCmdDraw", alr_vkCmdDraw),
        ALR_ENTRY("vkCmdDrawIndexed", alr_vkCmdDrawIndexed),
        ALR_ENTRY("vkCmdDrawIndirect", alr_vkCmdDrawIndirect),
        ALR_ENTRY("vkCmdDispatch", alr_vkCmdDispatch),
        ALR_ENTRY("vkCmdSetViewport", alr_vkCmdSetViewport),
        ALR_ENTRY("vkCmdSetScissor", alr_vkCmdSetScissor),
        ALR_ENTRY("vkCmdPipelineBarrier", alr_vkCmdPipelineBarrier),
        ALR_ENTRY("vkCmdCopyBuffer", alr_vkCmdCopyBuffer),
        ALR_ENTRY("vkCmdCopyImage", alr_vkCmdCopyImage),
        ALR_ENTRY("vkCmdCopyBufferToImage", alr_vkCmdCopyBufferToImage),
        ALR_ENTRY("vkCmdPushConstants", alr_vkCmdPushConstants),
        ALR_ENTRY("vkCmdClearAttachments", alr_vkCmdClearAttachments),
        ALR_ENTRY("vkQueueSubmit", alr_vkQueueSubmit),
        ALR_ENTRY("vkQueueWaitIdle", alr_vkQueueWaitIdle),
        ALR_ENTRY("vkDeviceWaitIdle", alr_vkDeviceWaitIdle),
        ALR_ENTRY("vkWaitForFences", alr_vkWaitForFences),
        ALR_ENTRY("vkResetFences", alr_vkResetFences),
        ALR_ENTRY("vkGetFenceStatus", alr_vkGetFenceStatus),
#endif  /* ALR_ICD_CMD_TABLE */
