/* alr_icd_vk_min.h — a MINIMAL, self-contained Vulkan ABI subset for the ALR guest
 * Vulkan ICD's ENUM rung. Pure C; NO external Vulkan SDK needed (zig cc's sysroot
 * does not ship vulkan headers), matching the "header-only + self-contained" rule of
 * the alr_gpu tree. Every type/enum/constant below is byte/ABI-identical to official
 * <vulkan/vulkan_core.h> + <vulkan/vk_icd.h> (verified against NDK r27's copy), so an
 * unmodified Vulkan client's calls into our exported entry points have the correct
 * layout. We define ONLY what the create-instance / enumerate / props / device /
 * queue rung touches; the rest of Vulkan is deliberately absent (a later breadth rung
 * vendors more, or switches to the full SDK header when one is on the guest path).
 *
 * ABI anchors (must match the official header EXACTLY):
 *   - VKAPI_ATTR/VKAPI_CALL/VKAPI_PTR: the calling convention. On aarch64 these are
 *     all empty (the AAPCS default), as in the official header for non-Windows.
 *   - Dispatchable handles (VkInstance/VkPhysicalDevice/VkDevice/VkQueue) are pointers.
 *   - VkResult / VkStructureType / VkPhysicalDeviceType: int-sized enums, same values.
 *   - VkPhysicalDeviceProperties: deviceName is char[256] at the documented offset;
 *     we mirror the FULL struct so the app's struct and ours agree on size/offsets.
 */
#ifndef ALR_ICD_VK_MIN_H
#define ALR_ICD_VK_MIN_H

#include <stdint.h>
#include <stddef.h>

/* ---- calling-convention macros (aarch64/Linux: all empty, == official header) ---- */
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR

#define VK_API_VERSION_1_0 ((((uint32_t)0) << 29) | (((uint32_t)1) << 22) | (((uint32_t)0) << 12) | (uint32_t)0)
#define VK_API_VERSION_1_1 ((((uint32_t)0) << 29) | (((uint32_t)1) << 22) | (((uint32_t)1) << 12) | (uint32_t)0)
#define VK_MAKE_API_VERSION(variant, major, minor, patch) \
    ((((uint32_t)(variant)) << 29) | (((uint32_t)(major)) << 22) | \
     (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))

#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256U
#define VK_UUID_SIZE 16U
#define VK_MAX_MEMORY_TYPES 32U
#define VK_MAX_MEMORY_HEAPS 16U
#define VK_MAX_EXTENSION_NAME_SIZE 256U
#define VK_MAX_DESCRIPTION_SIZE 256U
#define VK_NULL_HANDLE ((void*)0)

/* VK 1.3 version macro (ANGLE may query 1.1/1.3; we cap our reported API at 1.1). */
#define VK_API_VERSION_1_3 ((((uint32_t)0) << 29) | (((uint32_t)1) << 22) | (((uint32_t)3) << 12) | (uint32_t)0)

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkSampleCountFlags;
typedef uint32_t VkQueueFlags;

/* Dispatchable handles are pointers (the loader/ICD ABI). */
typedef struct VkInstance_T*        VkInstance;
typedef struct VkPhysicalDevice_T*  VkPhysicalDevice;
typedef struct VkDevice_T*          VkDevice;
typedef struct VkQueue_T*           VkQueue;
/* VkCommandBuffer is ALSO a dispatchable handle (pointer); the rest below are NON-
 * dispatchable (uint64 handles in the official ABI on 64-bit too — they are always
 * uint64_t, never pointers, per VK_DEFINE_NON_DISPATCHABLE_HANDLE). The ALR ICD's
 * VK-M4 present rung uses these. */
typedef struct VkCommandBuffer_T*   VkCommandBuffer;

/* ---- VK-M4 (PRESENT rung) ABI additions. Non-dispatchable handles are uint64_t in the
 * official Vulkan ABI on every platform (VK_DEFINE_NON_DISPATCHABLE_HANDLE), so we
 * declare them as uint64_t — byte/ABI-identical, no pointer-size dependence. ---- */
typedef uint64_t VkCommandPool;
typedef uint64_t VkShaderModule;
typedef uint64_t VkSwapchainKHR;
typedef uint64_t VkImage;
typedef uint64_t VkSemaphore;
typedef uint64_t VkFence;
typedef uint64_t VkSurfaceKHR;
typedef uint32_t VkShaderStageFlags;
typedef uint32_t VkImageUsageFlags;
typedef uint32_t VkSwapchainCreateFlagsKHR;
typedef uint32_t VkCommandPoolCreateFlags;
typedef uint32_t VkShaderModuleCreateFlags;
typedef int32_t  VkFormat;            /* enum-sized; we pass values opaquely */
typedef int32_t  VkColorSpaceKHR;
typedef int32_t  VkPresentModeKHR;
typedef int32_t  VkSharingMode;
typedef int32_t  VkSurfaceTransformFlagBitsKHR;
typedef int32_t  VkCompositeAlphaFlagBitsKHR;
typedef int32_t  VkImageLayout;
typedef int32_t  VkCommandBufferLevel;

/* ---- WAVE A (generated create-resource forwards) ABI additions: the non-dispatchable
 * handles + their CreateInfo flag/enum fields. Non-dispatchable handles are uint64_t (the
 * official VK_DEFINE_NON_DISPATCHABLE_HANDLE ABI). The enum-typed CreateInfo fields are
 * declared int/uint sized; the generated ICD function only reads them as scalars and ships
 * them opaquely on the wire (the host rebuilds the real CreateInfo + casts back). ---- */
typedef uint64_t VkPipelineCache;
typedef uint64_t VkSampler;
typedef uint64_t VkEvent;
typedef uint64_t VkQueryPool;
typedef uint32_t VkPipelineCacheCreateFlags;
typedef uint32_t VkSamplerCreateFlags;
typedef uint32_t VkFenceCreateFlags;
typedef uint32_t VkSemaphoreCreateFlags;
typedef uint32_t VkEventCreateFlags;
typedef uint32_t VkQueryPoolCreateFlags;
typedef uint32_t VkQueryPipelineStatisticFlags;
typedef int32_t  VkFilter;
typedef int32_t  VkSamplerMipmapMode;
typedef int32_t  VkSamplerAddressMode;
typedef int32_t  VkCompareOp;
typedef int32_t  VkBorderColor;
typedef int32_t  VkQueryType;

/* ---- WAVE B (descriptor/layout create family) ABI additions. ---- */
typedef uint64_t VkDescriptorSetLayout;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkBufferView;  /* texel-buffer view (pTexelBufferView; not shipped — null) */
typedef uint32_t VkDescriptorSetLayoutCreateFlags;
typedef uint32_t VkPipelineLayoutCreateFlags;
typedef uint32_t VkDescriptorPoolCreateFlags;
typedef int32_t  VkDescriptorType;

/* ---- WAVE C (render pass / framebuffer) ABI additions. VkRenderPass is also defined for
 * the present rung elsewhere? no — add it here (non-dispatchable uint64_t). ---- */
typedef uint64_t VkRenderPass;
typedef uint64_t VkFramebuffer;
typedef uint32_t VkRenderPassCreateFlags;
typedef uint32_t VkFramebufferCreateFlags;
typedef uint32_t VkAttachmentDescriptionFlags;
typedef uint32_t VkSubpassDescriptionFlags;
typedef uint32_t VkDependencyFlags;
typedef uint32_t VkPipelineStageFlags;
typedef uint32_t VkAccessFlags;
typedef int32_t  VkAttachmentLoadOp;
typedef int32_t  VkAttachmentStoreOp;
typedef int32_t  VkPipelineBindPoint;

typedef enum VkResult {
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4,
    VK_ERROR_MEMORY_MAP_FAILED = -5,   /* used by the generated vkMapMemory */
    VK_ERROR_INCOMPATIBLE_DRIVER = -9,
    VK_RESULT_MAX_ENUM = 0x7FFFFFFF
} VkResult;

typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    /* VK-M4 (PRESENT rung) sTypes (official values). */
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000,
    VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
    VK_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} VkStructureType;

/* FULL DEVICE PASSTHROUGH: a base header for walking a pNext chain (every Vulkan struct
 * begins with { sType, pNext }). We never deref past these two words for an UNKNOWN struct
 * — we only ship structs whose sType is in our size table (alr_icd_feature_struct_size). */
typedef struct VkBaseInStructure {
    VkStructureType            sType;
    const struct VkBaseInStructure* pNext;
} VkBaseInStructure;

/* The writable twin (Properties2/Features2 pNext structs the ICD FILLS in place). Same
 * first-two-words layout as VkBaseInStructure but a non-const pNext for walking + writing. */
typedef struct VkBaseOutStructure {
    VkStructureType            sType;
    struct VkBaseOutStructure* pNext;
} VkBaseOutStructure;

/* sType values for the device-feature structs ANGLE's RendererVk may chain off
 * VkDeviceCreateInfo.pNext (official Vulkan constants). The size table below maps each to
 * its struct byte length so the guest can ship the WHOLE struct verbatim over the wire; the
 * host re-validates the sType against its own allowlist before chaining it to real Mali. */
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_VAL 1000059000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES_VAL 49
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES_VAL 51
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES_VAL 53
/* Core-promoted (1.1/1.2/1.3) INDIVIDUAL feature structs ANGLE may chain INSTEAD of (or
 * alongside) the aggregate VulkanXX_FEATURES — e.g. ANGLE on Mali enables VariablePointers
 * (1000120000), which the old 4-entry size table sized 0 -> DROPPED before the wire, so the
 * host never relinked it into the real Mali vkCreateDevice. The created device then lacked a
 * feature ANGLE had recorded its command buffer against -> ANGLE faulted at libGLESv2+0x1f6db4
 * on the first GL command. These sTypes mirror the host's vk_passthrough_feature_stype_allowed()
 * one-for-one so the ICD ships every struct the host is willing to relink (Mali only ever
 * receives features it itself reported via vkGetPhysicalDeviceFeatures, which now forwards the
 * REAL Mali set). The byte size below is the official 64-bit ABI: 16-byte header
 * { u32 sType; u32 pad; void* pNext } + N x VkBool32. */
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES_VAL 1000053001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES_VAL 1000083000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES_VAL 1000063000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES_VAL 1000120000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES_VAL 1000145001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES_VAL 1000156004
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_VAL 1000177000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_VAL 1000082000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_VAL 1000161001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_VAL 1000221000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES_VAL 1000108000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_VAL 1000253000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES_VAL 1000241000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_VAL 1000261000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_VAL 1000207000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_VAL 1000257000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_VAL 1000314007
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_VAL 1000044003
/* EXT feature structs ANGLE enables when Mali exposes the extension (the host intersects the
 * enabled-extension list against Mali's real set, so an unsupported one is never chained). */
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT_VAL 1000028000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT_VAL 1000254000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT_VAL 1000259000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT_VAL 1000265000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT_VAL 1000267000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT_VAL 1000287002
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT_VAL 1000377000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT_VAL 1000190002

/* ====================================================================================
 * CREATE-CALL pNext extension structs (LEAD-1: the create-forwards DROPPED ANGLE's pNext
 * chain — every alr_vkCreate* hardcoded pnext_count=0, so e.g. the binding-flags ANGLE
 * chains onto vkCreateDescriptorSetLayout never reached real Mali; ANGLE then built a
 * RendererVk allocator/format member against a feature the created object lacked → the
 * first-glTexImage2D NULL-deref at libGLESv2+0x206db4). Two ABI classes below:
 *   (A) POINTERLESS structs — the whole struct is inline scalars; ship it VERBATIM and let
 *       the host relink (vk_gen_relink_pnext) fix only the pNext header field.
 *   (B) POINTER-BEARING structs (BindingFlags.pBindingFlags, FormatList.pViewFormats) —
 *       the struct holds a guest pointer that is meaningless host-side. These are NOT
 *       shipped verbatim; the entrypoint inlines the pointed-to array on the wire and the
 *       host reconstructs the struct with a host-side array (see alr_vkCreateDescriptorSetLayout
 *       + vk_gen_real_create_descriptor_set_layout). The size table returns 0 for class (B)
 *       so the generic verbatim path never ships a dangling pointer. */
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_VAL 1000072001
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_VAL 1000072002
#define VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO_VAL 1000246000
#define VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO_VAL 1000117002
#define VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO_VAL 1000257002
/* Pointer-bearing (class B): given _VAL so the entrypoints can match the sType, but the
 * size table returns 0 (never shipped verbatim — handled by a dedicated inline encoding). */
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_VAL 1000161000
#define VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_VAL 1000147000

/* LEAD-3 (the first-glTexImage2D NULL-deref): the OUT-pNext struct ANGLE chains onto
 * vkGetPhysicalDeviceFormatProperties2 to read the 64-bit VkFormatFeatureFlags2. Our ICD used
 * to fill ONLY the embedded v1 formatProperties and leave this chained struct UNTOUCHED, so
 * ANGLE read all-zero optimal/linear/buffer features for RGBA8 -> it built a degenerate
 * vk::Format whose per-format helper member stayed NULL -> the deref at libGLESv2+0x206db4
 * (contextVk->getRenderer()->[+0x138]). VkFormatProperties3 = { sType, pNext,
 * linearTilingFeatures(u64), optimalTilingFeatures(u64), bufferFeatures(u64) }; the low 32
 * bits of VkFormatFeatureFlags2 are bit-compatible with the v1 VkFormatFeatureFlags, so we
 * fill it by widening the SAME real-Mali v1 flags. */
#define VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_VAL 1000360000
typedef uint64_t VkFormatFeatureFlags2Min;  /* VkFormatFeatureFlags2 (64-bit) */
typedef struct VkFormatProperties3Min {
    int32_t                    sType;
    int32_t                    _pad;
    void                      *pNext;
    VkFormatFeatureFlags2Min   linearTilingFeatures;
    VkFormatFeatureFlags2Min   optimalTilingFeatures;
    VkFormatFeatureFlags2Min   bufferFeatures;
} VkFormatProperties3Min;

/* ABI-EXACT byte size (arm64 LP64) of a POINTERLESS create-call pNext struct, so the guest
 * can ship the WHOLE struct verbatim. Each begins with the 16-byte VkBaseInStructure header
 * { u32 sType; u32 pad; void* pNext } then inline scalars; the whole struct is 8-byte aligned.
 * Returns 0 for unknown OR pointer-bearing sTypes (caller must skip / handle specially). */
static inline uint32_t alr_icd_create_pnext_struct_size(uint32_t s_type) {
    switch (s_type) {
        /* {hdr; VkExternalMemoryHandleTypeFlags handleTypes(u32)} -> 16 + 4, pad to 24. */
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_VAL:  return 24u;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_VAL: return 24u;
        /* {hdr; VkImageUsageFlags stencilUsage(u32)} -> 24. */
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO_VAL:    return 24u;
        /* {hdr; VkImageUsageFlags usage(u32)} -> 24. */
        case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO_VAL:       return 24u;
        /* {hdr; VkBool32 opaqueCaptureAddress? no — u64 opaqueCaptureAddress} -> 16 + 8 = 24. */
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO_VAL: return 24u;
        default: return 0u;  /* unknown OR pointer-bearing (class B): never ship verbatim */
    }
}

/* Selected VK-M4 enum values the guest app may set (passed opaquely to the ICD). */
#define VK_SHADER_STAGE_VERTEX_BIT 0x00000001u
#define VK_SHADER_STAGE_FRAGMENT_BIT 0x00000010u
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_PRESENT_MODE_FIFO_KHR 2
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT 0x00000010u
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002u
#define VK_SUBPASS_CONTENTS_INLINE 0

typedef enum VkPhysicalDeviceType {
    VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
    VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
    VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM = 0x7FFFFFFF
} VkPhysicalDeviceType;

typedef enum VkQueueFlagBits {
    VK_QUEUE_GRAPHICS_BIT = 0x00000001,
    VK_QUEUE_COMPUTE_BIT = 0x00000002,
    VK_QUEUE_TRANSFER_BIT = 0x00000004,
    VK_QUEUE_SPARSE_BINDING_BIT = 0x00000008,
    VK_QUEUE_FLAG_BITS_MAX_ENUM = 0x7FFFFFFF
} VkQueueFlagBits;

typedef void (VKAPI_PTR *PFN_vkVoidFunction)(void);

typedef struct VkAllocationCallbacks VkAllocationCallbacks;  /* opaque to us (ignored) */

typedef struct VkApplicationInfo {
    VkStructureType    sType;
    const void*        pNext;
    const char*        pApplicationName;
    uint32_t           applicationVersion;
    const char*        pEngineName;
    uint32_t           engineVersion;
    uint32_t           apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    VkStructureType             sType;
    const void*                 pNext;
    VkFlags                     flags;
    const VkApplicationInfo*    pApplicationInfo;
    uint32_t                    enabledLayerCount;
    const char* const*          ppEnabledLayerNames;
    uint32_t                    enabledExtensionCount;
    const char* const*          ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkDeviceQueueCreateInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkFlags            flags;
    uint32_t           queueFamilyIndex;
    uint32_t           queueCount;
    const float*       pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    VkStructureType                    sType;
    const void*                        pNext;
    VkFlags                            flags;
    uint32_t                           queueCreateInfoCount;
    const VkDeviceQueueCreateInfo*     pQueueCreateInfos;
    uint32_t                           enabledLayerCount;
    const char* const*                 ppEnabledLayerNames;
    uint32_t                           enabledExtensionCount;
    const char* const*                 ppEnabledExtensionNames;
    const void*                        pEnabledFeatures;  /* VkPhysicalDeviceFeatures* (ignored) */
} VkDeviceCreateInfo;

/* ---- VK-M4 (PRESENT rung) structs. Only the fields the ALR ICD reads are meaningful;
 * the rest are mirrored for ABI size/offset fidelity (the app fills the full struct).
 * The ICD marshals the SPIR-V blob / extent / image count and ignores the rest (it has
 * no on-screen VkSurface — the SurfaceView is reached via the in-app compositor). ---- */
typedef struct VkExtent2D { uint32_t width; uint32_t height; } VkExtent2D;

typedef struct VkCommandPoolCreateInfo {
    VkStructureType            sType;
    const void*                pNext;
    VkCommandPoolCreateFlags   flags;
    uint32_t                   queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
    VkStructureType        sType;
    const void*            pNext;
    VkCommandPool          commandPool;
    VkCommandBufferLevel   level;
    uint32_t               commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkShaderModuleCreateInfo {
    VkStructureType              sType;
    const void*                 pNext;
    VkShaderModuleCreateFlags   flags;
    size_t                      codeSize;   /* in BYTES */
    const uint32_t*             pCode;
} VkShaderModuleCreateInfo;

typedef struct VkSwapchainCreateInfoKHR {
    VkStructureType                  sType;
    const void*                      pNext;
    VkSwapchainCreateFlagsKHR        flags;
    VkSurfaceKHR                     surface;        /* ignored by the ALR ICD */
    uint32_t                         minImageCount;
    VkFormat                         imageFormat;
    VkColorSpaceKHR                  imageColorSpace;
    VkExtent2D                       imageExtent;
    uint32_t                         imageArrayLayers;
    VkImageUsageFlags                imageUsage;
    VkSharingMode                    imageSharingMode;
    uint32_t                         queueFamilyIndexCount;
    const uint32_t*                  pQueueFamilyIndices;
    VkSurfaceTransformFlagBitsKHR    preTransform;
    VkCompositeAlphaFlagBitsKHR      compositeAlpha;
    VkPresentModeKHR                 presentMode;
    VkBool32                         clipped;
    VkSwapchainKHR                   oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef struct VkPresentInfoKHR {
    VkStructureType          sType;
    const void*              pNext;
    uint32_t                 waitSemaphoreCount;
    const VkSemaphore*       pWaitSemaphores;
    uint32_t                 swapchainCount;
    const VkSwapchainKHR*    pSwapchains;
    const uint32_t*          pImageIndices;
    VkResult*                pResults;
} VkPresentInfoKHR;

typedef struct VkSubmitInfo {
    VkStructureType                sType;
    const void*                    pNext;
    uint32_t                       waitSemaphoreCount;
    const VkSemaphore*             pWaitSemaphores;
    const VkFlags*                 pWaitDstStageMask;
    uint32_t                       commandBufferCount;
    const VkCommandBuffer*         pCommandBuffers;
    uint32_t                       signalSemaphoreCount;
    const VkSemaphore*             pSignalSemaphores;
} VkSubmitInfo;

typedef struct VkQueueFamilyProperties {
    VkQueueFlags    queueFlags;
    uint32_t        queueCount;
    uint32_t        timestampValidBits;
    /* VkExtent3D minImageTransferGranularity (3 × uint32) — mirror its 12 bytes so the
     * struct size/stride matches the official header (the app may pass an array). */
    uint32_t        minImageTransferGranularityWidth;
    uint32_t        minImageTransferGranularityHeight;
    uint32_t        minImageTransferGranularityDepth;
} VkQueueFamilyProperties;

/* VkPhysicalDeviceLimits / VkPhysicalDeviceSparseProperties are large; we mirror them
 * as correctly-SIZED opaque byte blobs so VkPhysicalDeviceProperties has the exact
 * official size + deviceName offset (the app may stack-allocate the full struct). The
 * ENUM rung only reads apiVersion/deviceType/deviceName/vendorID, all BEFORE limits. */
/* CRITICAL ABI ALIGNMENT: the official VkPhysicalDeviceLimits contains VkDeviceSize (8-byte)
 * members, so its natural alignment is 8 — and that 8-alignment is what places the `limits`
 * member of VkPhysicalDeviceProperties at offset 296 (4 bytes of padding follow the 16-byte
 * pipelineCacheUUID at offset 276..291 to 8-align limits). If we model the opaque blob as a
 * bare uint8_t[504] (alignment 1), `limits` lands at offset 292 instead — a 4-byte shift, so
 * a memcpy of the REAL Mali limits into a CALLER's (ANGLE's, real-ABI) VkPhysicalDeviceProperties
 * writes 4 bytes too early and ANGLE reads every limit shifted (a 64-bit bufferImageGranularity/
 * maxMemoryAllocationSize then reads a half-swapped huge value, sample-count masks become
 * garbage). ANGLE turns that into a bogus std::vector size during caps init and crashes on the
 * first texture (DEVICE-PROVEN: SIGSEGV libGLESv2+0x1f6db4, a vector grow, x24=0x60<<32). Force
 * the 8-byte alignment so the member offset + struct size (800) match the official ABI exactly. */
typedef struct VkPhysicalDeviceLimits {
    _Alignas(8) uint8_t _opaque[504];  /* sizeof + ALIGNMENT must match the official header (504, 8) */
} VkPhysicalDeviceLimits;
_Static_assert(_Alignof(VkPhysicalDeviceLimits) == 8,
               "VkPhysicalDeviceLimits must be 8-byte aligned so VkPhysicalDeviceProperties.limits lands at offset 296");
typedef struct VkPhysicalDeviceSparseProperties {
    VkBool32 residencyStandard2DBlockShape;
    VkBool32 residencyStandard2DMultisampleBlockShape;
    VkBool32 residencyStandard3DBlockShape;
    VkBool32 residencyAlignedMipSize;
    VkBool32 residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct VkPhysicalDeviceProperties {
    uint32_t              apiVersion;
    uint32_t              driverVersion;
    uint32_t              vendorID;
    uint32_t              deviceID;
    VkPhysicalDeviceType  deviceType;
    char                  deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t               pipelineCacheUUID[VK_UUID_SIZE];
    VkPhysicalDeviceLimits             limits;
    VkPhysicalDeviceSparseProperties   sparseProperties;
} VkPhysicalDeviceProperties;

/* ---- ANGLE-init enumeration/query structs (the read-only bring-up queries ANGLE's
 * RendererVk issues before any device-object creation). Exact official sizes/layout so
 * a caller can stack-allocate arrays of them. ---- */
typedef struct VkExtensionProperties {
    char     extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkLayerProperties {
    char     layerName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
    uint32_t implementationVersion;
    char     description[VK_MAX_DESCRIPTION_SIZE];
} VkLayerProperties;

/* VkPhysicalDeviceFeatures: 55 VkBool32 fields (Vulkan 1.0). We mirror it as a sized
 * blob of 55 VkBool32 so the size/stride is exact; the ICD memset(0)s it (a conservative
 * "no optional features" answer ANGLE tolerates by disabling those code paths). */
typedef struct VkPhysicalDeviceFeatures {
    VkBool32 features[55];
} VkPhysicalDeviceFeatures;

typedef struct VkMemoryType {
    VkFlags      propertyFlags;  /* VkMemoryPropertyFlags */
    uint32_t     heapIndex;
} VkMemoryType;
typedef struct VkMemoryHeap {
    VkDeviceSize size;
    VkFlags      flags;          /* VkMemoryHeapFlags */
} VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t      memoryTypeCount;
    VkMemoryType  memoryTypes[VK_MAX_MEMORY_TYPES];
    uint32_t      memoryHeapCount;
    VkMemoryHeap  memoryHeaps[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkFormatProperties {
    VkFlags linearTilingFeatures;
    VkFlags optimalTilingFeatures;
    VkFlags bufferFeatures;
} VkFormatProperties;

/* ---- vkGetPhysicalDeviceImageFormatProperties ABI (a REQUIRED loader entrypoint; also
 * an ANGLE format-capability query). VkImageType/Tiling are int-sized enums; the query
 * takes (format, type, tiling, usage, flags) and fills VkImageFormatProperties. ---- */
typedef int32_t VkImageType;
typedef int32_t VkImageTiling;
typedef uint32_t VkImageCreateFlags;
typedef struct VkExtent3D { uint32_t width; uint32_t height; uint32_t depth; } VkExtent3D;
typedef struct VkImageFormatProperties {
    VkExtent3D    maxExtent;
    uint32_t      maxMipLevels;
    uint32_t      maxArrayLayers;
    VkSampleCountFlags sampleCounts;
    VkDeviceSize  maxResourceSize;
} VkImageFormatProperties;

/* ---- Core 1.1 "2" query structs (sType-tagged, pNext-chained). ANGLE's RendererVk
 * uses vkGetPhysicalDeviceProperties2 (in ChoosePhysicalDevice) + the Features2 /
 * QueueFamilyProperties2 / MemoryProperties2 / FormatProperties2 family. We mirror each
 * as { sType, pNext, <v1 struct value> } — byte/ABI-identical to the official header —
 * and the ICD fills the embedded v1 value (which is the real-Mali data it already
 * marshalled) while walking past any pNext the caller chained. The "2" sType values are
 * the official Vulkan constants. ---- */
typedef struct VkPhysicalDeviceProperties2 {
    VkStructureType            sType;
    void*                      pNext;
    VkPhysicalDeviceProperties properties;
} VkPhysicalDeviceProperties2;
typedef struct VkPhysicalDeviceFeatures2 {
    VkStructureType          sType;
    void*                    pNext;
    VkPhysicalDeviceFeatures features;
} VkPhysicalDeviceFeatures2;
typedef struct VkQueueFamilyProperties2 {
    VkStructureType         sType;
    void*                   pNext;
    VkQueueFamilyProperties queueFamilyProperties;
} VkQueueFamilyProperties2;
typedef struct VkPhysicalDeviceMemoryProperties2 {
    VkStructureType                  sType;
    void*                            pNext;
    VkPhysicalDeviceMemoryProperties memoryProperties;
} VkPhysicalDeviceMemoryProperties2;
typedef struct VkFormatProperties2 {
    VkStructureType    sType;
    void*              pNext;
    VkFormatProperties formatProperties;
} VkFormatProperties2;
/* The image-format "2" pair: an INPUT info struct (format/type/tiling/usage/flags) and an
 * OUTPUT struct embedding VkImageFormatProperties. */
typedef struct VkPhysicalDeviceImageFormatInfo2 {
    VkStructureType    sType;
    const void*        pNext;
    VkFormat           format;
    VkImageType        type;
    VkImageTiling      tiling;
    VkImageUsageFlags  usage;
    VkImageCreateFlags flags;
} VkPhysicalDeviceImageFormatInfo2;
typedef struct VkImageFormatProperties2 {
    VkStructureType         sType;
    void*                   pNext;
    VkImageFormatProperties imageFormatProperties;
} VkImageFormatProperties2;
/* Sparse image-format query (a REQUIRED loader entrypoint). We report ZERO properties
 * (no sparse support) — a conformant answer ANGLE tolerates — so only the count form +
 * the struct SIZE matter for ABI. */
typedef struct VkSparseImageFormatProperties {
    VkFlags    aspectMask;        /* VkImageAspectFlags */
    VkExtent3D imageGranularity;
    VkFlags    flags;             /* VkSparseImageFormatFlags */
} VkSparseImageFormatProperties;
typedef struct VkSparseImageFormatProperties2 {
    VkStructureType               sType;
    void*                         pNext;
    VkSparseImageFormatProperties properties;
} VkSparseImageFormatProperties2;
typedef struct VkPhysicalDeviceSparseImageFormatInfo2 {
    VkStructureType    sType;
    const void*        pNext;
    VkFormat           format;
    VkImageType        type;
    VkSampleCountFlags samples;   /* VkSampleCountFlagBits */
    VkImageUsageFlags  usage;
    VkImageTiling      tiling;
} VkPhysicalDeviceSparseImageFormatInfo2;

/* FULL DEVICE PASSTHROUGH: the byte size of a device-feature struct the guest may forward
 * over the wire, by sType. Returns 0 for an unknown sType (the guest then DROPS that pNext
 * struct — the feature simply stays disabled on the real device, the conservative answer).
 * Sizes are the official 64-bit-ABI struct sizes ({sType u32, pad u32, pNext ptr}=16-byte
 * header + N×VkBool32, rounded up to 8-byte alignment). VkPhysicalDeviceFeatures2 uses the
 * guest's own exact struct size. Note: since the ICD advertises ZERO optional features in
 * vkGetPhysicalDeviceFeatures2, a conformant client (ANGLE) enables none, so in practice the
 * only struct on the wire is an all-zero VkPhysicalDeviceFeatures2 — but we size the common
 * core-version feature structs too so a client that chains them is handled, not truncated. */
/* ABI-EXACT size of a {VkStructureType sType; u32 _pad; void* pNext; VkBool32 x N} feature
 * struct on arm64-LP64. The struct's natural alignment is 8 (the void* pNext), so the WHOLE
 * struct is rounded UP to a multiple of 8 — for an ODD bool count N the trailing 4 bytes are
 * tail-padded to reach 8. The previous table used a bare `16 + N*4`, which is 4 bytes SHORT
 * for every odd-N struct (Multiview, ScalarBlockLayout, Synchronization2, …) — the SAME
 * align/tail-pad ABI class as the VkPhysicalDeviceLimits offset bug. A short size truncates
 * ANGLE's real struct by 4 B before the wire (dropping its last VkBool32) AND makes the host
 * chain a 4-B-short buffer into real Mali vkCreateDevice (Mali then over-reads the buffer).
 * ALR_FEAT_SZ(N) computes the correct padded size so an odd N can never truncate again. */
#define ALR_FEAT_SZ(n) (uint32_t)(((16u + (uint32_t)(n) * 4u) + 7u) & ~7u)
/* Compile-time lock: ALR_FEAT_SZ must equal the official NDK r27 sizeof for each struct
 * (derived from <vulkan/vulkan_core.h>; see out/abi_oracle). If a future header edit changes
 * a bool count, the matching assert fires. The numbers on the right are the OFFICIAL sizes. */
_Static_assert(ALR_FEAT_SZ(12) == 64,  "Vulkan11Features = 64");
_Static_assert(ALR_FEAT_SZ(47) == 208, "Vulkan12Features = 208");
_Static_assert(ALR_FEAT_SZ(15) == 80,  "Vulkan13Features = 80");
_Static_assert(ALR_FEAT_SZ(2)  == 24,  "VariablePointers/ShaderFloat16Int8/TransformFeedback/ProvokingVertex/CustomBorderColor/VertexAttributeDivisor = 24");
_Static_assert(ALR_FEAT_SZ(3)  == 32,  "Multiview/8BitStorage/BufferDeviceAddress/ExtendedDynamicState2 = 32");
_Static_assert(ALR_FEAT_SZ(4)  == 32,  "16BitStorage = 32");
_Static_assert(ALR_FEAT_SZ(1)  == 24,  "single-bool core/EXT feature structs = 24");
_Static_assert(ALR_FEAT_SZ(20) == 96,  "DescriptorIndexing = 96");
_Static_assert(ALR_FEAT_SZ(6)  == 40,  "LineRasterization = 40");
static inline uint32_t alr_icd_feature_struct_size(uint32_t s_type) {
    switch (s_type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_VAL:
            return (uint32_t)sizeof(VkPhysicalDeviceFeatures2);  /* exact, guest-defined (240) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES_VAL:
            return ALR_FEAT_SZ(12);   /* 12 VkBool32 -> 64 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES_VAL:
            return ALR_FEAT_SZ(47);   /* 47 VkBool32 -> 208 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES_VAL:
            return ALR_FEAT_SZ(15);   /* 15 VkBool32 -> 80 */
        /* Individual core-promoted feature structs (ANGLE chains VariablePointers on Mali;
         * the rest are sized so a chain that uses the granular structs is never truncated).
         * Byte sizes are the official 64-bit ABI: 16-byte header + N x VkBool32, the WHOLE
         * struct rounded up to its 8-byte alignment (odd N gets a 4-byte tail pad). */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES_VAL:
            return ALR_FEAT_SZ(2);    /* variablePointersStorageBuffer, variablePointers -> 24 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES_VAL:
            return ALR_FEAT_SZ(3);    /* multiview, *GeometryShader, *TessellationShader -> 32 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES_VAL:
            return ALR_FEAT_SZ(4);    /* -> 32 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_VAL:
            return ALR_FEAT_SZ(3);    /* -> 32 (was 28: 4 B short) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20: 4 B short) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_VAL:
            return ALR_FEAT_SZ(2);    /* -> 24 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_VAL:
            return ALR_FEAT_SZ(20);   /* -> 96 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_VAL:
            return ALR_FEAT_SZ(3);    /* -> 32 (was 28: 4 B short) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        /* EXT feature structs (only chained when Mali exposes the matching extension). */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(2);    /* -> 24 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(2);    /* -> 24 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(6);    /* -> 40 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(1);    /* -> 24 (was 20) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(3);    /* -> 32 (was 28: 4 B short) */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(2);    /* -> 24 */
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT_VAL:
            return ALR_FEAT_SZ(2);    /* -> 24 */
        default:
            return 0;             /* unknown: drop (feature stays off) */
    }
}

/* ========================================================================
 * GENERATED render-batch ABI (device memory / buffer / image / image view).
 * The codegen entrypoints (alr_gpu/generated/alr_gpu_vk_gen_icd.inc) take these
 * types. Every layout below is byte/ABI-identical to <vulkan/vulkan_core.h>
 * (verified against vk.xml v1.3.275 == NDK r27). Non-dispatchable handles are
 * uint64_t (VK_DEFINE_NON_DISPATCHABLE_HANDLE) exactly like VkImage above; the
 * ICD only ever carries the guest's virtual id in them, never derefs them.
 * ======================================================================== */
typedef uint64_t VkBuffer;          /* non-dispatchable (== official ABI) */
typedef uint64_t VkImageView;       /* non-dispatchable */
typedef uint64_t VkDeviceMemory;    /* non-dispatchable */

typedef VkFlags VkBufferCreateFlags;
typedef VkFlags VkBufferUsageFlags;
typedef VkFlags VkMemoryMapFlags;
typedef VkFlags VkImageViewCreateFlags;
typedef int32_t VkImageViewType;     /* int-sized enum; passed opaquely */
typedef int32_t VkComponentSwizzle;  /* int-sized enum; passed opaquely */

/* VkMemoryAllocateInfo { sType, pNext, allocationSize(u64), memoryTypeIndex(u32) }. */
typedef struct VkMemoryAllocateInfo {
    VkStructureType sType;
    const void     *pNext;
    VkDeviceSize    allocationSize;
    uint32_t        memoryTypeIndex;
} VkMemoryAllocateInfo;

/* VkMappedMemoryRange { sType, pNext, memory, offset(u64), size(u64) }. */
typedef struct VkMappedMemoryRange {
    VkStructureType sType;
    const void     *pNext;
    VkDeviceMemory  memory;
    VkDeviceSize    offset;
    VkDeviceSize    size;
} VkMappedMemoryRange;

/* VkMemoryRequirements { size(u64), alignment(u64), memoryTypeBits(u32) }. */
typedef struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t     memoryTypeBits;
} VkMemoryRequirements;

/* The core-1.1 "2" memory-requirements family (ANGLE resolves these to allocate texture/FBO
 * backing memory on a 1.1+ device). Layouts are ABI-identical to <vulkan/vulkan_core.h>.
 * sType values from the Vulkan spec (VK_VERSION_1_1). */
#define VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 1000146000
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2  1000146001
#define VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2             1000146003
typedef struct VkBufferMemoryRequirementsInfo2 {
    VkStructureType sType;
    const void     *pNext;
    VkBuffer        buffer;
} VkBufferMemoryRequirementsInfo2;
typedef struct VkImageMemoryRequirementsInfo2 {
    VkStructureType sType;
    const void     *pNext;
    VkImage         image;
} VkImageMemoryRequirementsInfo2;
typedef struct VkMemoryRequirements2 {
    VkStructureType      sType;
    void                *pNext;
    VkMemoryRequirements memoryRequirements;
} VkMemoryRequirements2;

/* VkBufferCreateInfo (the POD prefix the codegen ships; pQueueFamilyIndices is part of
 * the ABI struct but the generated forwarder uses EXCLUSIVE sharing — see the tool). */
typedef struct VkBufferCreateInfo {
    VkStructureType     sType;
    const void         *pNext;
    VkBufferCreateFlags flags;
    VkDeviceSize        size;
    VkBufferUsageFlags  usage;
    VkSharingMode       sharingMode;
    uint32_t            queueFamilyIndexCount;
    const uint32_t     *pQueueFamilyIndices;
} VkBufferCreateInfo;

/* VkImageCreateInfo — full ABI layout (matches the official header field-for-field). */
typedef struct VkImageCreateInfo {
    VkStructureType    sType;
    const void        *pNext;
    VkImageCreateFlags flags;
    VkImageType        imageType;
    VkFormat           format;
    VkExtent3D         extent;
    uint32_t           mipLevels;
    uint32_t           arrayLayers;
    VkSampleCountFlags samples;       /* VkSampleCountFlagBits, int-sized */
    VkImageTiling      tiling;
    VkImageUsageFlags  usage;
    VkSharingMode      sharingMode;
    uint32_t           queueFamilyIndexCount;
    const uint32_t    *pQueueFamilyIndices;
    VkImageLayout      initialLayout;
} VkImageCreateInfo;

typedef struct VkComponentMapping {
    VkComponentSwizzle r;
    VkComponentSwizzle g;
    VkComponentSwizzle b;
    VkComponentSwizzle a;
} VkComponentMapping;

typedef struct VkImageSubresourceRange {
    VkFlags  aspectMask;          /* VkImageAspectFlags */
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} VkImageSubresourceRange;

/* VkImageViewCreateInfo — full ABI layout. */
typedef struct VkImageViewCreateInfo {
    VkStructureType         sType;
    const void             *pNext;
    VkImageViewCreateFlags  flags;
    VkImage                 image;
    VkImageViewType         viewType;
    VkFormat                format;
    VkComponentMapping      components;
    VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;

/* ---- WAVE A CreateInfo structs (full ABI layout, field-for-field with the official
 * header). The generated ICD forwarders read the scalar fields the codegen SPEC lists and
 * ship them on the wire; the host rebuilds the real CreateInfo. Only the shader-module
 * CreateInfo (already defined above) carries a blob. ---- */
typedef struct VkPipelineCacheCreateInfo {
    VkStructureType            sType;
    const void                *pNext;
    VkPipelineCacheCreateFlags flags;
    size_t                     initialDataSize;
    const void                *pInitialData;
} VkPipelineCacheCreateInfo;

typedef struct VkSamplerCreateInfo {
    VkStructureType      sType;
    const void          *pNext;
    VkSamplerCreateFlags flags;
    VkFilter             magFilter;
    VkFilter             minFilter;
    VkSamplerMipmapMode  mipmapMode;
    VkSamplerAddressMode addressModeU;
    VkSamplerAddressMode addressModeV;
    VkSamplerAddressMode addressModeW;
    float                mipLodBias;
    VkBool32             anisotropyEnable;
    float                maxAnisotropy;
    VkBool32             compareEnable;
    VkCompareOp          compareOp;
    float                minLod;
    float                maxLod;
    VkBorderColor        borderColor;
    VkBool32             unnormalizedCoordinates;
} VkSamplerCreateInfo;

typedef struct VkFenceCreateInfo {
    VkStructureType    sType;
    const void        *pNext;
    VkFenceCreateFlags flags;
} VkFenceCreateInfo;

typedef struct VkSemaphoreCreateInfo {
    VkStructureType        sType;
    const void            *pNext;
    VkSemaphoreCreateFlags flags;
} VkSemaphoreCreateInfo;

typedef struct VkEventCreateInfo {
    VkStructureType    sType;
    const void        *pNext;
    VkEventCreateFlags flags;
} VkEventCreateInfo;

typedef struct VkQueryPoolCreateInfo {
    VkStructureType               sType;
    const void                   *pNext;
    VkQueryPoolCreateFlags        flags;
    VkQueryType                   queryType;
    uint32_t                      queryCount;
    VkQueryPipelineStatisticFlags pipelineStatistics;
} VkQueryPoolCreateInfo;

/* ---- WAVE B CreateInfo / array-element / write structs (full ABI layout). The generated
 * ICD forwarders read the array members the codegen SPEC lists + ship them on the wire;
 * the host rebuilds the real structs + translates handles. ---- */
typedef struct VkDescriptorSetLayoutBinding {
    uint32_t           binding;
    VkDescriptorType   descriptorType;
    uint32_t           descriptorCount;
    VkShaderStageFlags stageFlags;
    const VkSampler   *pImmutableSamplers;
} VkDescriptorSetLayoutBinding;

typedef struct VkDescriptorSetLayoutCreateInfo {
    VkStructureType                     sType;
    const void                         *pNext;
    VkDescriptorSetLayoutCreateFlags    flags;
    uint32_t                            bindingCount;
    const VkDescriptorSetLayoutBinding *pBindings;
} VkDescriptorSetLayoutCreateInfo;

typedef struct VkPushConstantRange {
    VkShaderStageFlags stageFlags;
    uint32_t           offset;
    uint32_t           size;
} VkPushConstantRange;

typedef struct VkPipelineLayoutCreateInfo {
    VkStructureType              sType;
    const void                 *pNext;
    VkPipelineLayoutCreateFlags flags;
    uint32_t                    setLayoutCount;
    const VkDescriptorSetLayout *pSetLayouts;
    uint32_t                    pushConstantRangeCount;
    const VkPushConstantRange  *pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct VkDescriptorPoolSize {
    VkDescriptorType type;
    uint32_t         descriptorCount;
} VkDescriptorPoolSize;

typedef struct VkDescriptorPoolCreateInfo {
    VkStructureType             sType;
    const void                *pNext;
    VkDescriptorPoolCreateFlags flags;
    uint32_t                    maxSets;
    uint32_t                    poolSizeCount;
    const VkDescriptorPoolSize *pPoolSizes;
} VkDescriptorPoolCreateInfo;

typedef struct VkDescriptorSetAllocateInfo {
    VkStructureType              sType;
    const void                 *pNext;
    VkDescriptorPool            descriptorPool;
    uint32_t                    descriptorSetCount;
    const VkDescriptorSetLayout *pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef struct VkDescriptorImageInfo {
    VkSampler     sampler;
    VkImageView   imageView;
    VkImageLayout imageLayout;
} VkDescriptorImageInfo;

typedef struct VkDescriptorBufferInfo {
    VkBuffer     buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
} VkDescriptorBufferInfo;

typedef struct VkWriteDescriptorSet {
    VkStructureType               sType;
    const void                   *pNext;
    VkDescriptorSet               dstSet;
    uint32_t                      dstBinding;
    uint32_t                      dstArrayElement;
    uint32_t                      descriptorCount;
    VkDescriptorType              descriptorType;
    const VkDescriptorImageInfo  *pImageInfo;
    const VkDescriptorBufferInfo *pBufferInfo;
    const VkBufferView           *pTexelBufferView;
} VkWriteDescriptorSet;

typedef struct VkCopyDescriptorSet {
    VkStructureType sType;
    const void     *pNext;
    VkDescriptorSet srcSet;
    uint32_t        srcBinding;
    uint32_t        srcArrayElement;
    VkDescriptorSet dstSet;
    uint32_t        dstBinding;
    uint32_t        dstArrayElement;
    uint32_t        descriptorCount;
} VkCopyDescriptorSet;

/* ---- WAVE C render-pass + framebuffer structs (full ABI layout). The generated render-pass
 * ICD forwarder walks the real VkRenderPassCreateInfo (attachments / subpasses with nested
 * attachment-reference arrays / dependencies) to drive the wire encoders. ---- */
typedef struct VkAttachmentDescription {
    VkAttachmentDescriptionFlags flags;
    VkFormat                     format;
    VkSampleCountFlags           samples;       /* VkSampleCountFlagBits */
    VkAttachmentLoadOp           loadOp;
    VkAttachmentStoreOp          storeOp;
    VkAttachmentLoadOp           stencilLoadOp;
    VkAttachmentStoreOp          stencilStoreOp;
    VkImageLayout                initialLayout;
    VkImageLayout                finalLayout;
} VkAttachmentDescription;

typedef struct VkAttachmentReference {
    uint32_t      attachment;
    VkImageLayout layout;
} VkAttachmentReference;

typedef struct VkSubpassDescription {
    VkSubpassDescriptionFlags    flags;
    VkPipelineBindPoint          pipelineBindPoint;
    uint32_t                     inputAttachmentCount;
    const VkAttachmentReference *pInputAttachments;
    uint32_t                     colorAttachmentCount;
    const VkAttachmentReference *pColorAttachments;
    const VkAttachmentReference *pResolveAttachments;
    const VkAttachmentReference *pDepthStencilAttachment;
    uint32_t                     preserveAttachmentCount;
    const uint32_t              *pPreserveAttachments;
} VkSubpassDescription;

typedef struct VkSubpassDependency {
    uint32_t             srcSubpass;
    uint32_t             dstSubpass;
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;
    VkAccessFlags        srcAccessMask;
    VkAccessFlags        dstAccessMask;
    VkDependencyFlags    dependencyFlags;
} VkSubpassDependency;

typedef struct VkRenderPassCreateInfo {
    VkStructureType                sType;
    const void                    *pNext;
    VkRenderPassCreateFlags        flags;
    uint32_t                       attachmentCount;
    const VkAttachmentDescription *pAttachments;
    uint32_t                       subpassCount;
    const VkSubpassDescription    *pSubpasses;
    uint32_t                       dependencyCount;
    const VkSubpassDependency     *pDependencies;
} VkRenderPassCreateInfo;

typedef struct VkFramebufferCreateInfo {
    VkStructureType          sType;
    const void              *pNext;
    VkFramebufferCreateFlags flags;
    VkRenderPass             renderPass;
    uint32_t                 attachmentCount;
    const VkImageView       *pAttachments;
    uint32_t                 width;
    uint32_t                 height;
    uint32_t                 layers;
} VkFramebufferCreateInfo;

/* ---- Geometry types shared by the pipeline create-info (viewport/scissor) AND the cmd-log
 * recorder entrypoints (alr_icd_cmd_entrypoints.inc). Defined HERE so both the pipeline
 * structs below and the recorder TU see the same VkViewport/VkRect2D ABI. VkExtent2D/3D are
 * defined earlier in this header. ---- */
typedef struct VkOffset2D_min { int32_t x; int32_t y; } VkOffset2D;
typedef struct VkOffset3D_min { int32_t x; int32_t y; int32_t z; } VkOffset3D;
typedef struct VkRect2D_min { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct VkViewport_min {
    float x; float y; float width; float height; float minDepth; float maxDepth;
} VkViewport;
#define VK_ALR_GEOM_TYPES_DEFINED 1   /* tells alr_icd_cmd_entrypoints.inc not to re-define */

/* ---- WAVE (graphics/compute PIPELINE create) ABI additions. The generated ICD forwarders
 * (alr_gpu/generated/alr_gpu_vk_gen_icd.inc, alr_vkCreate{Graphics,Compute}Pipelines) walk
 * the full VkGraphicsPipelineCreateInfo state graph and ship each field on the wire (the
 * host rebuilds the real structs + translates handles). These match the official Vulkan ABI
 * (the codegen was written against <vulkan/vulkan.h>); the enums are int32_t (the forwarders
 * cast each to uint32_t for the wire). ---- */
typedef uint64_t VkPipeline;                    /* non-dispatchable (carries a virtual id) */
typedef uint32_t VkPipelineCreateFlags;
typedef uint32_t VkPipelineShaderStageCreateFlags;
typedef uint32_t VkPipelineVertexInputStateCreateFlags;
typedef uint32_t VkPipelineInputAssemblyStateCreateFlags;
typedef uint32_t VkPipelineTessellationStateCreateFlags;
typedef uint32_t VkPipelineViewportStateCreateFlags;
typedef uint32_t VkPipelineRasterizationStateCreateFlags;
typedef uint32_t VkPipelineMultisampleStateCreateFlags;
typedef uint32_t VkPipelineDepthStencilStateCreateFlags;
typedef uint32_t VkPipelineColorBlendStateCreateFlags;
typedef uint32_t VkPipelineDynamicStateCreateFlags;
typedef uint32_t VkColorComponentFlags;
typedef uint32_t VkCullModeFlags;
typedef uint32_t VkSampleCountFlagBits;
typedef uint32_t VkSampleMask;
typedef int32_t  VkShaderStageFlagBits;
typedef int32_t  VkPrimitiveTopology;
typedef int32_t  VkVertexInputRate;
typedef int32_t  VkPolygonMode;
typedef int32_t  VkFrontFace;
typedef int32_t  VkStencilOp;
typedef int32_t  VkLogicOp;
typedef int32_t  VkBlendFactor;
typedef int32_t  VkBlendOp;
typedef int32_t  VkDynamicState;

typedef struct VkSpecializationMapEntry {
    uint32_t constantID;
    uint32_t offset;
    size_t   size;
} VkSpecializationMapEntry;
typedef struct VkSpecializationInfo {
    uint32_t                        mapEntryCount;
    const VkSpecializationMapEntry *pMapEntries;
    size_t                          dataSize;
    const void                     *pData;
} VkSpecializationInfo;
typedef struct VkPipelineShaderStageCreateInfo {
    VkStructureType                  sType;
    const void                      *pNext;
    VkPipelineShaderStageCreateFlags flags;
    VkShaderStageFlagBits            stage;
    VkShaderModule                   module;
    const char                      *pName;
    const VkSpecializationInfo      *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkVertexInputBindingDescription {
    uint32_t          binding;
    uint32_t          stride;
    VkVertexInputRate inputRate;
} VkVertexInputBindingDescription;
typedef struct VkVertexInputAttributeDescription {
    uint32_t location;
    uint32_t binding;
    VkFormat format;
    uint32_t offset;
} VkVertexInputAttributeDescription;
typedef struct VkPipelineVertexInputStateCreateInfo {
    VkStructureType                          sType;
    const void                              *pNext;
    VkPipelineVertexInputStateCreateFlags    flags;
    uint32_t                                 vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription   *pVertexBindingDescriptions;
    uint32_t                                 vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription *pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;

typedef struct VkPipelineInputAssemblyStateCreateInfo {
    VkStructureType                         sType;
    const void                             *pNext;
    VkPipelineInputAssemblyStateCreateFlags flags;
    VkPrimitiveTopology                     topology;
    VkBool32                                primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;

typedef struct VkPipelineTessellationStateCreateInfo {
    VkStructureType                        sType;
    const void                           *pNext;
    VkPipelineTessellationStateCreateFlags flags;
    uint32_t                              patchControlPoints;
} VkPipelineTessellationStateCreateInfo;

typedef struct VkPipelineViewportStateCreateInfo {
    VkStructureType                    sType;
    const void                       *pNext;
    VkPipelineViewportStateCreateFlags flags;
    uint32_t                          viewportCount;
    const VkViewport                 *pViewports;
    uint32_t                          scissorCount;
    const VkRect2D                   *pScissors;
} VkPipelineViewportStateCreateInfo;

typedef struct VkPipelineRasterizationStateCreateInfo {
    VkStructureType                         sType;
    const void                            *pNext;
    VkPipelineRasterizationStateCreateFlags flags;
    VkBool32                               depthClampEnable;
    VkBool32                               rasterizerDiscardEnable;
    VkPolygonMode                          polygonMode;
    VkCullModeFlags                        cullMode;
    VkFrontFace                            frontFace;
    VkBool32                               depthBiasEnable;
    float                                  depthBiasConstantFactor;
    float                                  depthBiasClamp;
    float                                  depthBiasSlopeFactor;
    float                                  lineWidth;
} VkPipelineRasterizationStateCreateInfo;

typedef struct VkPipelineMultisampleStateCreateInfo {
    VkStructureType                       sType;
    const void                          *pNext;
    VkPipelineMultisampleStateCreateFlags flags;
    VkSampleCountFlagBits                rasterizationSamples;
    VkBool32                             sampleShadingEnable;
    float                                minSampleShading;
    const VkSampleMask                  *pSampleMask;
    VkBool32                             alphaToCoverageEnable;
    VkBool32                             alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;

typedef struct VkStencilOpState {
    VkStencilOp failOp;
    VkStencilOp passOp;
    VkStencilOp depthFailOp;
    VkCompareOp compareOp;
    uint32_t    compareMask;
    uint32_t    writeMask;
    uint32_t    reference;
} VkStencilOpState;
typedef struct VkPipelineDepthStencilStateCreateInfo {
    VkStructureType                        sType;
    const void                           *pNext;
    VkPipelineDepthStencilStateCreateFlags flags;
    VkBool32                              depthTestEnable;
    VkBool32                              depthWriteEnable;
    VkCompareOp                           depthCompareOp;
    VkBool32                              depthBoundsTestEnable;
    VkBool32                              stencilTestEnable;
    VkStencilOpState                      front;
    VkStencilOpState                      back;
    float                                 minDepthBounds;
    float                                 maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;

typedef struct VkPipelineColorBlendAttachmentState {
    VkBool32              blendEnable;
    VkBlendFactor         srcColorBlendFactor;
    VkBlendFactor         dstColorBlendFactor;
    VkBlendOp             colorBlendOp;
    VkBlendFactor         srcAlphaBlendFactor;
    VkBlendFactor         dstAlphaBlendFactor;
    VkBlendOp             alphaBlendOp;
    VkColorComponentFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;
typedef struct VkPipelineColorBlendStateCreateInfo {
    VkStructureType                            sType;
    const void                               *pNext;
    VkPipelineColorBlendStateCreateFlags       flags;
    VkBool32                                   logicOpEnable;
    VkLogicOp                                  logicOp;
    uint32_t                                   attachmentCount;
    const VkPipelineColorBlendAttachmentState *pAttachments;
    float                                      blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;

typedef struct VkPipelineDynamicStateCreateInfo {
    VkStructureType                   sType;
    const void                      *pNext;
    VkPipelineDynamicStateCreateFlags flags;
    uint32_t                         dynamicStateCount;
    const VkDynamicState            *pDynamicStates;
} VkPipelineDynamicStateCreateInfo;

typedef struct VkGraphicsPipelineCreateInfo {
    VkStructureType                                sType;
    const void                                   *pNext;
    VkPipelineCreateFlags                         flags;
    uint32_t                                      stageCount;
    const VkPipelineShaderStageCreateInfo        *pStages;
    const VkPipelineVertexInputStateCreateInfo   *pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
    const VkPipelineTessellationStateCreateInfo  *pTessellationState;
    const VkPipelineViewportStateCreateInfo      *pViewportState;
    const VkPipelineRasterizationStateCreateInfo *pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo   *pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo  *pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo    *pColorBlendState;
    const VkPipelineDynamicStateCreateInfo       *pDynamicState;
    VkPipelineLayout                              layout;
    VkRenderPass                                  renderPass;
    uint32_t                                      subpass;
    VkPipeline                                    basePipelineHandle;
    int32_t                                       basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct VkComputePipelineCreateInfo {
    VkStructureType                 sType;
    const void                     *pNext;
    VkPipelineCreateFlags           flags;
    VkPipelineShaderStageCreateInfo stage;
    VkPipelineLayout                layout;
    VkPipeline                      basePipelineHandle;
    int32_t                         basePipelineIndex;
} VkComputePipelineCreateInfo;

/* The "2" sType values (official Vulkan constants). */
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 1000059000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 1000059001
#define VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 1000059002
#define VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 1000059003
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 1000059004
#define VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 1000059005
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 1000059006
#define VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2 1000059007
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2 1000059008
/* VK_ERROR_FORMAT_NOT_SUPPORTED is a VALID image-format verdict (the format/usage tuple
 * is simply unsupported), distinct from an init failure. */
#define VK_ERROR_FORMAT_NOT_SUPPORTED (-11)

/* Common VkMemoryPropertyFlagBits (only the ones a minimal allocator reports). */
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT     0x00000001
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT     0x00000002
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT    0x00000004
#define VK_MEMORY_HEAP_DEVICE_LOCAL_BIT         0x00000001

/* ---- entry-point PFN typedefs the ICD's GIPA returns ---- */
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance instance, const char* pName);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetDeviceProcAddr)(VkDevice device, const char* pName);
typedef VkResult (VKAPI_PTR *PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef void (VKAPI_PTR *PFN_vkDestroyInstance)(VkInstance, const VkAllocationCallbacks*);
typedef VkResult (VKAPI_PTR *PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
typedef VkResult (VKAPI_PTR *PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
typedef void (VKAPI_PTR *PFN_vkDestroyDevice)(VkDevice, const VkAllocationCallbacks*);
typedef void (VKAPI_PTR *PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
/* ---- ANGLE-init rung PFNs: the REQUIRED image/sparse-format queries + the core-1.1
 * "2" physical-device query family ANGLE's RendererVk::initialize uses. ---- */
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceImageFormatProperties)(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceSparseImageFormatProperties)(VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlags, VkImageUsageFlags, VkImageTiling, uint32_t*, VkSparseImageFormatProperties*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceProperties2)(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceFeatures2)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceQueueFamilyProperties2)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties2*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceMemoryProperties2)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2*);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceFormatProperties2)(VkPhysicalDevice, VkFormat, VkFormatProperties2*);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceImageFormatProperties2)(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2*, VkImageFormatProperties2*);
/* ---- VK-M4 (PRESENT rung) PFNs ---- */
typedef VkResult (VKAPI_PTR *PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*);
typedef void (VKAPI_PTR *PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const VkAllocationCallbacks*);
typedef VkResult (VKAPI_PTR *PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef VkResult (VKAPI_PTR *PFN_vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
typedef void (VKAPI_PTR *PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
typedef VkResult (VKAPI_PTR *PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef void (VKAPI_PTR *PFN_vkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
typedef VkResult (VKAPI_PTR *PFN_vkAcquireNextImageKHR)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
typedef VkResult (VKAPI_PTR *PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);

/* ---- public Vulkan function prototypes (so a client TU like alr-vk-enum.c that
 * links -lvulkan can call them by name). These bind to the ICD's exported symbols.
 * Only the ENUM-rung subset is declared (matching what the ICD exports). ---- */
#ifdef __cplusplus
extern "C" {
#endif
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue);
/* ---- ANGLE-init rung public prototypes (REQUIRED image/sparse format queries + the
 * core-1.1 "2" physical-device query family) ---- */
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pImageFormatProperties);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkSampleCountFlags samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties2 *pQueueFamilyProperties);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties);
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2 *pFormatProperties);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo, VkImageFormatProperties2 *pImageFormatProperties);
/* ---- VK-M4 (PRESENT rung) public prototypes ---- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool);
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule);
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

/* ---- ALR bring-up convenience: record the clear+triangle-draw (using the guest's own
 * vert/frag shader modules) into `commandBuffer`, targeting swapchain image `imageIndex`.
 * This folds the coarse CMD_BEGIN_DRAW_MODULES wire op (the renderpass + pipeline + draw
 * are built host-side on real Mali) into ONE call, because the ALR ENUM/PRESENT wire is
 * deliberately coarse-grained (it marshals draw INTENT, not every vkCmd*). A guest app
 * still creates instance/device/swapchain and ships its OWN SPIR-V — this is just the
 * record primitive for the coarse wire. A later breadth rung exposes fine-grained vkCmd*.
 * bg_rgba is the background the renderpass clears to (the triangle is the shader's color). */
VKAPI_ATTR void VKAPI_CALL alrVkCmdDrawTriangleModules(
    VkCommandBuffer commandBuffer, VkSwapchainKHR swapchain, uint32_t imageIndex,
    VkShaderModule vertModule, VkShaderModule fragModule, uint32_t width, uint32_t height,
    float bg_r, float bg_g, float bg_b, float bg_a);
#ifdef __cplusplus
}
#endif

/* ---- loader/ICD interface (== NDK vk_icd.h) ---- */
#define ICD_LOADER_MAGIC 0x01CDC0DE
typedef union {
    uintptr_t loaderMagic;
    void *loaderData;
} VK_LOADER_DATA;
static inline void alr_set_loader_magic_value(void *pNewObject) {
    VK_LOADER_DATA *li = (VK_LOADER_DATA *)pNewObject;
    li->loaderMagic = ICD_LOADER_MAGIC;
}

/* ============================================================================
 * EXHAUSTIVE ABI LOCK (arm64-LP64). Every struct below is one the ICD either FILLS for
 * ANGLE (a query reply) or whose bytes it ships verbatim on the wire; ANGLE/Mali read
 * them through the OFFICIAL <vulkan/vulkan_core.h> ABI, so a single wrong size/offset/
 * alignment desyncs the copy and corrupts a value ANGLE later dereferences (the proven
 * libGLESv2+0x1f6db4 vector-grow crash was exactly this: VkPhysicalDeviceLimits landing
 * 4 B early). The right-hand numbers are the official NDK r27 (vk.xml 1.3.x) sizeof/
 * alignof/offsetof, generated by out/abi_oracle.c which #includes the real header. If a
 * future edit changes any layout, the matching assert fires at COMPILE time — the whole
 * "limits bug family" is now a build error, never a device-only SIGSEGV. ---- */
#define ALR_VK_ABI_EQ(expr, want, msg) _Static_assert((expr) == (want), msg)

/* -- VkPhysicalDeviceProperties + its blobs (the vkGetPhysicalDeviceProperties reply) -- */
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceProperties), 824, "VkPhysicalDeviceProperties sizeof must be 824");
ALR_VK_ABI_EQ(_Alignof(VkPhysicalDeviceProperties), 8, "VkPhysicalDeviceProperties alignof must be 8");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceProperties, deviceType), 16, "deviceType@16");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceProperties, deviceName), 20, "deviceName@20");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceProperties, pipelineCacheUUID), 276, "pipelineCacheUUID@276");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceProperties, limits), 296, "limits@296 (the proven crash offset)");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceProperties, sparseProperties), 800, "sparseProperties@800");
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceLimits), 504, "VkPhysicalDeviceLimits sizeof must be 504");
ALR_VK_ABI_EQ(_Alignof(VkPhysicalDeviceLimits), 8, "VkPhysicalDeviceLimits alignof must be 8");
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceSparseProperties), 20, "VkPhysicalDeviceSparseProperties sizeof must be 20");

/* -- VkPhysicalDeviceFeatures (vkGetPhysicalDeviceFeatures reply, raw-bytes wire) -- */
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceFeatures), 220, "VkPhysicalDeviceFeatures sizeof must be 220");

/* -- VkPhysicalDeviceMemoryProperties + embedded arrays (the memory-type selection path) -- */
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceMemoryProperties), 520, "VkPhysicalDeviceMemoryProperties sizeof must be 520");
ALR_VK_ABI_EQ(_Alignof(VkPhysicalDeviceMemoryProperties), 8, "VkPhysicalDeviceMemoryProperties alignof must be 8");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceMemoryProperties, memoryTypes), 4, "memoryTypes@4");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceMemoryProperties, memoryHeapCount), 260, "memoryHeapCount@260");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceMemoryProperties, memoryHeaps), 264, "memoryHeaps@264");
ALR_VK_ABI_EQ(sizeof(VkMemoryType), 8, "VkMemoryType sizeof must be 8");
ALR_VK_ABI_EQ(offsetof(VkMemoryType, heapIndex), 4, "VkMemoryType.heapIndex@4");
ALR_VK_ABI_EQ(sizeof(VkMemoryHeap), 16, "VkMemoryHeap sizeof must be 16 (8-byte VkDeviceSize size)");
ALR_VK_ABI_EQ(_Alignof(VkMemoryHeap), 8, "VkMemoryHeap alignof must be 8");
ALR_VK_ABI_EQ(offsetof(VkMemoryHeap, flags), 8, "VkMemoryHeap.flags@8");

/* -- VkMemoryRequirements (+ the v2 wrapper): the texture/FBO backing-alloc path -- */
ALR_VK_ABI_EQ(sizeof(VkMemoryRequirements), 24, "VkMemoryRequirements sizeof must be 24");
ALR_VK_ABI_EQ(_Alignof(VkMemoryRequirements), 8, "VkMemoryRequirements alignof must be 8");
ALR_VK_ABI_EQ(offsetof(VkMemoryRequirements, size), 0, "VkMemoryRequirements.size@0");
ALR_VK_ABI_EQ(offsetof(VkMemoryRequirements, alignment), 8, "VkMemoryRequirements.alignment@8");
ALR_VK_ABI_EQ(offsetof(VkMemoryRequirements, memoryTypeBits), 16, "VkMemoryRequirements.memoryTypeBits@16");
ALR_VK_ABI_EQ(sizeof(VkMemoryRequirements2), 40, "VkMemoryRequirements2 sizeof must be 40");
ALR_VK_ABI_EQ(offsetof(VkMemoryRequirements2, memoryRequirements), 16, "VkMemoryRequirements2.memoryRequirements@16");

/* -- VkFormatProperties / VkImageFormatProperties (the glTexImage2D format-capability path) -- */
ALR_VK_ABI_EQ(sizeof(VkFormatProperties), 12, "VkFormatProperties sizeof must be 12");
ALR_VK_ABI_EQ(sizeof(VkImageFormatProperties), 32, "VkImageFormatProperties sizeof must be 32");
ALR_VK_ABI_EQ(_Alignof(VkImageFormatProperties), 8, "VkImageFormatProperties alignof must be 8");
ALR_VK_ABI_EQ(offsetof(VkImageFormatProperties, maxMipLevels), 12, "maxMipLevels@12");
ALR_VK_ABI_EQ(offsetof(VkImageFormatProperties, maxArrayLayers), 16, "maxArrayLayers@16");
ALR_VK_ABI_EQ(offsetof(VkImageFormatProperties, sampleCounts), 20, "sampleCounts@20");
ALR_VK_ABI_EQ(offsetof(VkImageFormatProperties, maxResourceSize), 24, "maxResourceSize@24 (8-byte VkDeviceSize)");
ALR_VK_ABI_EQ(sizeof(VkSparseImageFormatProperties), 20, "VkSparseImageFormatProperties sizeof must be 20");

/* -- VkQueueFamilyProperties (+ v2): the queue-family query ANGLE enumerates -- */
ALR_VK_ABI_EQ(sizeof(VkQueueFamilyProperties), 24, "VkQueueFamilyProperties sizeof must be 24");
ALR_VK_ABI_EQ(offsetof(VkQueueFamilyProperties, timestampValidBits), 8, "timestampValidBits@8");
ALR_VK_ABI_EQ(sizeof(VkQueueFamilyProperties2), 40, "VkQueueFamilyProperties2 sizeof must be 40");
ALR_VK_ABI_EQ(offsetof(VkQueueFamilyProperties2, queueFamilyProperties), 16, "queueFamilyProperties@16");

/* -- the "2" wrapper structs the ICD fills (Properties2/Features2/MemoryProperties2/…) -- */
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceProperties2), 840, "VkPhysicalDeviceProperties2 sizeof must be 840");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceProperties2, properties), 16, "Properties2.properties@16");
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceFeatures2), 240, "VkPhysicalDeviceFeatures2 sizeof must be 240");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceFeatures2, features), 16, "Features2.features@16");
ALR_VK_ABI_EQ(sizeof(VkPhysicalDeviceMemoryProperties2), 536, "VkPhysicalDeviceMemoryProperties2 sizeof must be 536");
ALR_VK_ABI_EQ(offsetof(VkPhysicalDeviceMemoryProperties2, memoryProperties), 16, "MemoryProperties2.memoryProperties@16");
ALR_VK_ABI_EQ(sizeof(VkFormatProperties2), 32, "VkFormatProperties2 sizeof must be 32");
ALR_VK_ABI_EQ(offsetof(VkFormatProperties2, formatProperties), 16, "FormatProperties2.formatProperties@16");
ALR_VK_ABI_EQ(sizeof(VkImageFormatProperties2), 48, "VkImageFormatProperties2 sizeof must be 48");
ALR_VK_ABI_EQ(offsetof(VkImageFormatProperties2, imageFormatProperties), 16, "ImageFormatProperties2.imageFormatProperties@16");
ALR_VK_ABI_EQ(sizeof(VkSparseImageFormatProperties2), 40, "VkSparseImageFormatProperties2 sizeof must be 40");

/* -- create-info structs whose ABI the codegen wire forwarders walk (image/buffer create) -- */
ALR_VK_ABI_EQ(sizeof(VkImageCreateInfo), 88, "VkImageCreateInfo sizeof must be 88");
ALR_VK_ABI_EQ(offsetof(VkImageCreateInfo, extent), 28, "VkImageCreateInfo.extent@28");
ALR_VK_ABI_EQ(offsetof(VkImageCreateInfo, initialLayout), 80, "VkImageCreateInfo.initialLayout@80");
ALR_VK_ABI_EQ(sizeof(VkBufferCreateInfo), 56, "VkBufferCreateInfo sizeof must be 56");
ALR_VK_ABI_EQ(offsetof(VkBufferCreateInfo, size), 24, "VkBufferCreateInfo.size@24");
ALR_VK_ABI_EQ(sizeof(VkImageViewCreateInfo), 80, "VkImageViewCreateInfo sizeof must be 80");
ALR_VK_ABI_EQ(sizeof(VkMemoryAllocateInfo), 32, "VkMemoryAllocateInfo sizeof must be 32");
ALR_VK_ABI_EQ(offsetof(VkMemoryAllocateInfo, allocationSize), 16, "VkMemoryAllocateInfo.allocationSize@16");

#undef ALR_VK_ABI_EQ
#endif /* ALR_ICD_VK_MIN_H */
