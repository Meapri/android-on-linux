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
#define VK_NULL_HANDLE ((void*)0)

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

typedef enum VkResult {
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4,
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
typedef struct VkPhysicalDeviceLimits {
    uint8_t _opaque[504];  /* sizeof(VkPhysicalDeviceLimits) in the official header */
} VkPhysicalDeviceLimits;
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

#endif /* ALR_ICD_VK_MIN_H */
