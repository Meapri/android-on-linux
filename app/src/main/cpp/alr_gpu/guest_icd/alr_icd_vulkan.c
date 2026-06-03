/* alr_icd_vulkan.c — the ALR guest Vulkan ICD (libvulkan.so.1).
 *
 * This is the LOADER-DISCOVERABLE GUEST ENTRYPOINT for ALR's universal GPU path: a
 * glibc-aarch64 libvulkan an unmodified Vulkan client (or later ANGLE/zink) loads via
 * the standard mechanism, whose calls marshal over the SPSC ring to the app-process
 * host, where they are replayed on the REAL vendor Mali libvulkan (the exact
 * DEVICE-PROVEN path of run_vk_marshal_mali_probe, alr_gpu_vk_marshal_probe.hpp /
 * alr_gpu_vk_decode.hpp). The ICD is the Vulkan analogue of the GLES shim's
 * libGLESv2.so.2 (alr_gpu/guest_shim/) — same wire (alr_gpu_vk_proto.hpp), same ring
 * (alr_gpu_ring_c.h), same client-side virtual-handle model — but driven by the guest
 * app instead of probe code.
 *
 * SHIP-AS-SONAME decision (see guest_icd/README + the report): we ship as SONAME
 * libvulkan.so.1 DIRECTLY (the base rootfs has NO libvulkan, so ZERO conflict, exactly
 * like the GLES shim ships libGLESv2.so.2). vkGetInstanceProcAddr IS the public entry
 * an app/loader calls. We ALSO export the Khronos loader/ICD interface
 * (vk_icdNegotiateLoaderICDInterfaceVersion + vk_icdGetInstanceProcAddr) and ship an
 * ICD manifest, so a future real-Vulkan-loader app discovers us via VK_ICD_FILENAMES
 * too — at no extra cost. No external Vulkan SDK is needed (alr_icd_vk_min.h vendors
 * the ABI subset), keeping the build NEEDED-clean (only libc.so.6) for the tiny rootfs.
 *
 * SCOPE — the ENUM rung (VK-M3 first milestone): vkCreateInstance,
 * vkEnumeratePhysicalDevices, vkGetPhysicalDeviceProperties (surfacing
 * "Mali-G615 MC2"), vkGetPhysicalDeviceQueueFamilyProperties, vkCreateDevice,
 * vkGetDeviceQueue, and the matching vkDestroy*. Swapchain/WSI present, full
 * draw/compute breadth, and SPIR-V over the wire are deliberately NOT here (next rung).
 *
 * CLIENT-SIDE VIRTUAL HANDLES (the crux, mirroring the GLES shim + the proven VK wire):
 * the ICD allocates virtual ids monotonically and returns dispatchable handles whose
 * first word is the loader magic (for the loader route) and whose body carries the
 * virtual id; the host owns the virtual->real translation. The ONE place data comes
 * back is enumerate (device count) + props — that is the reply the ring carries.
 */
#include "alr_icd_vk_min.h"
#include "alr_icd_runtime.h"  /* alr_icd_roundtrip / alr_icd_ring_ok */
#include "alr_gpu_vk_proto.hpp"  /* AlrVkEncoder + AlrVkOp/AlrVkReply wire (C-clean) */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Dispatchable-handle objects. A Vulkan dispatchable handle's first word MUST be
 * the loader dispatch slot (ICD sets the loader magic). We back each handle with a
 * heap object whose first member is that slot, followed by our virtual id + context.
 * The app/loader holds the pointer opaquely and hands it back to our entry points.
 * ============================================================================ */
typedef struct AlrIcdInstance {
    VK_LOADER_DATA loader;   /* MUST be first (loader magic) */
    uint32_t       vinst;    /* virtual instance id on the wire */
    uint32_t       vphys_base;   /* base virtual id assigned to enumerated devices */
    uint32_t       phys_count;   /* device count the host reported (cached) */
} AlrIcdInstance;

typedef struct AlrIcdPhysicalDevice {
    VK_LOADER_DATA loader;   /* MUST be first */
    uint32_t       vphys;    /* virtual physical-device id on the wire */
    AlrIcdInstance* inst;    /* owning instance */
} AlrIcdPhysicalDevice;

typedef struct AlrIcdDevice {
    VK_LOADER_DATA loader;   /* MUST be first */
    uint32_t       vdev;     /* virtual device id on the wire */
    uint32_t       gfx_family;   /* graphics queue family the host chose */
    AlrIcdPhysicalDevice* phys;
} AlrIcdDevice;

typedef struct AlrIcdQueue {
    VK_LOADER_DATA loader;   /* MUST be first */
    uint32_t       vqueue;   /* virtual queue id on the wire */
    AlrIcdDevice*  dev;
} AlrIcdQueue;

/* ---- monotonic virtual-id allocators (start at 1; 0 reserved). The ENUM rung is
 * light; one global lock serializes id allocation + handle bookkeeping. ---- */
static pthread_mutex_t g_id_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_vinst   = 1;
static uint32_t g_next_vphys   = 100;   /* matches the probe's vphys_base convention */
static uint32_t g_next_vdev    = 1000;
static uint32_t g_next_vqueue  = 2000;

static uint32_t alr_alloc(uint32_t *counter, uint32_t step) {
    uint32_t v;
    pthread_mutex_lock(&g_id_lock);
    v = *counter;
    *counter += step;
    pthread_mutex_unlock(&g_id_lock);
    return v;
}

/* The deviceName the host classifies a physical device with comes back in the props
 * reply; we cache the last decoded props per VkPhysicalDevice so repeated
 * vkGetPhysicalDeviceProperties / queue-family calls don't re-marshal. Keyed by vphys. */
#define ALR_ICD_MAX_PHYS 8
typedef struct AlrIcdPhysCache {
    uint32_t vphys;            /* 0 = empty slot */
    int      have_props;
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t device_type;
    char     device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t qf_count;
    uint32_t qf_flags[16];
    uint32_t qf_qcount[16];
    uint8_t  is_software;
} AlrIcdPhysCache;
static AlrIcdPhysCache g_phys_cache[ALR_ICD_MAX_PHYS];

static AlrIcdPhysCache *phys_cache_slot(uint32_t vphys) {
    int free_slot = -1, i;
    for (i = 0; i < ALR_ICD_MAX_PHYS; ++i) {
        if (g_phys_cache[i].vphys == vphys && vphys != 0) return &g_phys_cache[i];
        if (g_phys_cache[i].vphys == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        memset(&g_phys_cache[free_slot], 0, sizeof(g_phys_cache[free_slot]));
        g_phys_cache[free_slot].vphys = vphys;
        return &g_phys_cache[free_slot];
    }
    return NULL;
}

/* ============================================================================
 * Reply decode helpers — a tiny little-endian reader over the AlrVkReply stream the
 * host wrote back (same wire the host self-test's decode_vk_reply uses; reimplemented
 * here in C so the ICD stays self-contained / no C++).
 * ============================================================================ */
typedef struct AlrRd { const uint8_t *p; uint32_t n, pos; } AlrRd;
static int rd_u8(AlrRd *r, uint8_t *v)  { if (r->pos + 1 > r->n) return 0; *v = r->p[r->pos]; r->pos += 1; return 1; }
static int rd_u32(AlrRd *r, uint32_t *v){ if (r->pos + 4 > r->n) return 0; memcpy(v, r->p + r->pos, 4); r->pos += 4; return 1; }
static int rd_i32(AlrRd *r, int32_t *v) { if (r->pos + 4 > r->n) return 0; memcpy(v, r->p + r->pos, 4); r->pos += 4; return 1; }
static int rd_blob(AlrRd *r, const uint8_t **d, uint32_t *len) {
    if (!rd_u32(r, len)) return 0;
    if (r->pos + *len > r->n) return 0;
    *d = r->p + r->pos; r->pos += *len; return 1;
}

/* Parse a reply stream, filling the phys cache for any ALR_VK_REPLY_PHYS_PROPS records
 * and returning (via out params) the enumerate count/base + device gfx_family if those
 * records are present. Returns 1 on a well-formed reply. */
static int alr_icd_parse_reply(const uint8_t *data, uint32_t len,
                               uint32_t *out_count, uint32_t *out_base,
                               int32_t *out_inst_result, uint32_t *out_gfx_family,
                               int32_t *out_dev_result) {
    AlrRd r; uint8_t op;
    r.p = data; r.n = len; r.pos = 0;
    for (;;) {
        if (!rd_u8(&r, &op)) break;
        if (op == ALR_VK_REPLY_END) break;
        switch (op) {
            case ALR_VK_REPLY_INSTANCE: {
                uint32_t vinst; int32_t res;
                if (!rd_u32(&r, &vinst) || !rd_i32(&r, &res)) return 0;
                if (out_inst_result) *out_inst_result = res;
                break;
            }
            case ALR_VK_REPLY_PHYS_COUNT: {
                uint32_t vinst, base, count; int32_t res;
                if (!rd_u32(&r, &vinst) || !rd_u32(&r, &base) || !rd_u32(&r, &count) ||
                    !rd_i32(&r, &res)) return 0;
                if (out_count) *out_count = count;
                if (out_base) *out_base = base;
                break;
            }
            case ALR_VK_REPLY_PHYS_PROPS: {
                uint32_t vphys, api, drv, vend, devid, devtype, qf_count, i;
                const uint8_t *name; uint32_t name_len; uint8_t is_sw;
                AlrIcdPhysCache *slot;
                if (!rd_u32(&r, &vphys) || !rd_u32(&r, &api) || !rd_u32(&r, &drv) ||
                    !rd_u32(&r, &vend) || !rd_u32(&r, &devid) || !rd_u32(&r, &devtype) ||
                    !rd_blob(&r, &name, &name_len) || !rd_u32(&r, &qf_count)) return 0;
                slot = phys_cache_slot(vphys);
                if (slot) {
                    slot->have_props = 1;
                    slot->api_version = api;
                    slot->driver_version = drv;
                    slot->vendor_id = vend;
                    slot->device_id = devid;
                    slot->device_type = devtype;
                    uint32_t cn = name_len < (VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1)
                                      ? name_len : (VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
                    memcpy(slot->device_name, name, cn);
                    slot->device_name[cn] = '\0';
                    slot->qf_count = qf_count < 16 ? qf_count : 16;
                }
                for (i = 0; i < qf_count; ++i) {
                    uint32_t flags, qcount;
                    if (!rd_u32(&r, &flags) || !rd_u32(&r, &qcount)) return 0;
                    if (slot && i < 16) { slot->qf_flags[i] = flags; slot->qf_qcount[i] = qcount; }
                }
                if (!rd_u8(&r, &is_sw)) return 0;
                if (slot) slot->is_software = is_sw;
                break;
            }
            case ALR_VK_REPLY_DEVICE: {
                uint32_t vdev, gfx; int32_t res;
                if (!rd_u32(&r, &vdev) || !rd_i32(&r, &res) || !rd_u32(&r, &gfx)) return 0;
                if (out_dev_result) *out_dev_result = res;
                if (out_gfx_family) *out_gfx_family = gfx;
                break;
            }
            case ALR_VK_REPLY_SUBMIT: {
                /* not used by the ENUM rung; skip its fixed payload defensively */
                uint32_t vdev, vcmd; int32_t sr, rr; uint8_t px[4];
                if (!rd_u32(&r, &vdev) || !rd_u32(&r, &vcmd) || !rd_i32(&r, &sr) ||
                    !rd_i32(&r, &rr) || !rd_u8(&r, &px[0]) || !rd_u8(&r, &px[1]) ||
                    !rd_u8(&r, &px[2]) || !rd_u8(&r, &px[3])) return 0;
                (void)vdev; (void)vcmd; (void)sr; (void)rr; (void)px;
                break;
            }
            default:
                return 0;  /* unknown reply op: malformed */
        }
    }
    return 1;
}

/* A scratch reply buffer big enough for the ENUM rung's reply (instance + count +
 * props for a handful of devices). 4 KiB is generous (one Mali device ~ 90 bytes). */
#define ALR_ICD_REPLY_SCRATCH 4096

/* ============================================================================
 * Vulkan entry points — the ENUM rung. Each marshals its request via AlrVkEncoder
 * over alr_icd_roundtrip and decodes the reply. Forward-declared GIPA/GDPA below.
 * ============================================================================ */
static PFN_vkVoidFunction VKAPI_CALL alr_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
static PFN_vkVoidFunction VKAPI_CALL alr_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VkResult VKAPI_CALL alr_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkInstance *pInstance) {
    (void)pAllocator;
    AlrIcdInstance *inst;
    uint8_t req[64];
    uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    AlrVkEncoder e;
    uint32_t vinst, app_api, rlen;
    int32_t inst_result = 0;

    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;
    inst = (AlrIcdInstance *)calloc(1, sizeof(AlrIcdInstance));
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    alr_set_loader_magic_value(inst);  /* loader dispatch slot (harmless for direct use) */

    vinst = alr_alloc(&g_next_vinst, 1);
    inst->vinst = vinst;
    inst->vphys_base = 0;
    inst->phys_count = 0;

    app_api = VK_API_VERSION_1_1;
    if (pCreateInfo && pCreateInfo->pApplicationInfo &&
        pCreateInfo->pApplicationInfo->apiVersion != 0) {
        app_api = pCreateInfo->pApplicationInfo->apiVersion;
    }

    /* Ring-less (no host): still hand back a valid instance so the app can proceed to
     * vkEnumeratePhysicalDevices, which will report 0 devices — the conformant
     * "no GPU" answer. */
    if (alr_icd_ring_ok()) {
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_create_instance(&e, vinst, app_api);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) alr_icd_parse_reply(reply, rlen, NULL, NULL, &inst_result, NULL, NULL);
        }
    }
    if (inst_result != 0) { free(inst); return (VkResult)inst_result; }

    *pInstance = (VkInstance)inst;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyInstance(VkInstance instance,
                                             const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdInstance *inst = (AlrIcdInstance *)instance;
    if (!inst) return;
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_destroy_instance(&e, inst->vinst);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    }
    free(inst);
}

static VkResult VKAPI_CALL alr_vkEnumeratePhysicalDevices(VkInstance instance,
                                                          uint32_t *pPhysicalDeviceCount,
                                                          VkPhysicalDevice *pPhysicalDevices) {
    AlrIcdInstance *inst = (AlrIcdInstance *)instance;
    uint32_t count = 0, base = 0, i;
    if (!inst || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;

    /* Marshal the enumerate the FIRST time (cache the count/base on the instance so a
     * two-call query — count, then fill — doesn't re-enumerate, matching Vulkan
     * semantics where the set is stable between the two calls). */
    if (inst->phys_count == 0 && alr_icd_ring_ok()) {
        uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH]; uint32_t rlen;
        uint32_t vphys_base = alr_alloc(&g_next_vphys, ALR_ICD_MAX_PHYS);  /* reserve a block */
        /* The host already created the real VkInstance for inst->vinst during
         * vkCreateInstance (its real_inst[vinst] map persists across batches), so we do
         * NOT re-emit CREATE_INSTANCE here — that would leak a second host VkInstance. */
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_enumerate_phys(&e, inst->vinst, vphys_base);
        /* eagerly fetch props for the first device in the SAME batch so the deviceName
         * ("Mali-G615 MC2") is cached before the app calls GetPhysicalDeviceProperties. */
        alr_vk_enc_get_phys_props(&e, inst->vinst, vphys_base);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) {
                int32_t inst_res = 0;
                alr_icd_parse_reply(reply, rlen, &count, &base, &inst_res, NULL, NULL);
            }
        }
        if (count > ALR_ICD_MAX_PHYS) count = ALR_ICD_MAX_PHYS;  /* cap to our handle table */
        inst->vphys_base = vphys_base;
        inst->phys_count = count;
    }
    count = inst->phys_count;
    base = inst->vphys_base;

    if (pPhysicalDevices == NULL) {
        *pPhysicalDeviceCount = count;  /* query-count call */
        return VK_SUCCESS;
    }

    /* Fill call: hand back up to *pPhysicalDeviceCount handles. */
    uint32_t to_write = (*pPhysicalDeviceCount < count) ? *pPhysicalDeviceCount : count;
    for (i = 0; i < to_write; ++i) {
        AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)calloc(1, sizeof(AlrIcdPhysicalDevice));
        if (!pd) { *pPhysicalDeviceCount = i; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        alr_set_loader_magic_value(pd);
        pd->vphys = base + i;
        pd->inst = inst;
        pPhysicalDevices[i] = (VkPhysicalDevice)pd;
    }
    *pPhysicalDeviceCount = to_write;
    return (to_write < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Ensure the phys cache for `pd` is populated (marshal props on demand if the eager
 * fetch in enumerate didn't cover this device). */
static AlrIcdPhysCache *ensure_phys_props(AlrIcdPhysicalDevice *pd) {
    AlrIcdPhysCache *slot = phys_cache_slot(pd->vphys);
    if (slot && slot->have_props) return slot;
    if (alr_icd_ring_ok()) {
        uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH]; uint32_t rlen;
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_get_phys_props(&e, pd->inst->vinst, pd->vphys);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) alr_icd_parse_reply(reply, rlen, NULL, NULL, NULL, NULL, NULL);
        }
        slot = phys_cache_slot(pd->vphys);
    }
    return slot;
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                         VkPhysicalDeviceProperties *pProperties) {
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdPhysCache *slot;
    if (!pd || !pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));
    slot = ensure_phys_props(pd);
    if (slot && slot->have_props) {
        pProperties->apiVersion = slot->api_version;
        pProperties->driverVersion = slot->driver_version;
        pProperties->vendorID = slot->vendor_id;
        pProperties->deviceID = slot->device_id;
        pProperties->deviceType = (VkPhysicalDeviceType)slot->device_type;
        /* deviceName: the proof string ("Mali-G615 MC2") flowed through the ICD. */
        strncpy(pProperties->deviceName, slot->device_name,
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    } else {
        /* No host / unknown device: report a benign placeholder so callers don't NPE. */
        pProperties->apiVersion = VK_API_VERSION_1_1;
        pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
        strncpy(pProperties->deviceName, "ALR (no host)", VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    }
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
    VkQueueFamilyProperties *pQueueFamilyProperties) {
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdPhysCache *slot;
    uint32_t i;
    if (!pd || !pQueueFamilyPropertyCount) return;
    slot = ensure_phys_props(pd);
    uint32_t n = (slot && slot->have_props) ? slot->qf_count : 0;
    if (pQueueFamilyProperties == NULL) {
        *pQueueFamilyPropertyCount = n;
        return;
    }
    uint32_t to_write = (*pQueueFamilyPropertyCount < n) ? *pQueueFamilyPropertyCount : n;
    for (i = 0; i < to_write; ++i) {
        memset(&pQueueFamilyProperties[i], 0, sizeof(pQueueFamilyProperties[i]));
        pQueueFamilyProperties[i].queueFlags = slot->qf_flags[i];
        pQueueFamilyProperties[i].queueCount = slot->qf_qcount[i];
        pQueueFamilyProperties[i].timestampValidBits = 64;
        pQueueFamilyProperties[i].minImageTransferGranularityWidth = 1;
        pQueueFamilyProperties[i].minImageTransferGranularityHeight = 1;
        pQueueFamilyProperties[i].minImageTransferGranularityDepth = 1;
    }
    *pQueueFamilyPropertyCount = to_write;
}

static VkResult VKAPI_CALL alr_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                              const VkDeviceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator,
                                              VkDevice *pDevice) {
    (void)pCreateInfo; (void)pAllocator;
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdDevice *dev;
    uint32_t vdev, gfx_family = 0;
    int32_t dev_result = 0;
    if (!pd || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) return VK_ERROR_INITIALIZATION_FAILED;  /* no GPU to create on */

    dev = (AlrIcdDevice *)calloc(1, sizeof(AlrIcdDevice));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    alr_set_loader_magic_value(dev);
    vdev = alr_alloc(&g_next_vdev, 1);
    dev->vdev = vdev;
    dev->phys = pd;

    {
        uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH]; uint32_t rlen;
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_create_device(&e, pd->inst->vinst, pd->vphys, vdev);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) alr_icd_parse_reply(reply, rlen, NULL, NULL, NULL, &gfx_family, &dev_result);
            else dev_result = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else {
            dev_result = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (dev_result != 0) { free(dev); return (VkResult)dev_result; }
    dev->gfx_family = gfx_family;
    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyDevice(VkDevice device,
                                           const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev) return;
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_destroy_device(&e, dev->vdev);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    }
    free(dev);
}

static void VKAPI_CALL alr_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                            uint32_t queueIndex, VkQueue *pQueue) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    AlrIcdQueue *q;
    uint32_t vqueue;
    if (!dev || !pQueue) return;
    (void)queueFamilyIndex;  /* host binds the graphics family it chose at create_device */
    q = (AlrIcdQueue *)calloc(1, sizeof(AlrIcdQueue));
    if (!q) { *pQueue = VK_NULL_HANDLE; return; }
    alr_set_loader_magic_value(q);
    vqueue = alr_alloc(&g_next_vqueue, 1);
    q->vqueue = vqueue;
    q->dev = dev;
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_get_device_queue(&e, dev->vdev, queueIndex, vqueue);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    }
    *pQueue = (VkQueue)q;
}

/* ============================================================================
 * Dispatch — vkGetInstanceProcAddr / vkGetDeviceProcAddr. The app/loader resolves
 * every entry point through these. We return our ENUM-rung implementations and
 * vkGetInstanceProcAddr / vkGetDeviceProcAddr themselves (a global GIPA also resolves
 * the global commands vkCreateInstance / vkEnumerateInstanceVersion etc.).
 * ============================================================================ */
typedef struct { const char *name; PFN_vkVoidFunction fn; } AlrIcdProc;

#define ALR_ENTRY(n, f) { n, (PFN_vkVoidFunction)(f) }

static PFN_vkVoidFunction alr_lookup(const char *pName) {
    static const AlrIcdProc table[] = {
        ALR_ENTRY("vkGetInstanceProcAddr", alr_vkGetInstanceProcAddr),
        ALR_ENTRY("vkGetDeviceProcAddr", alr_vkGetDeviceProcAddr),
        ALR_ENTRY("vkCreateInstance", alr_vkCreateInstance),
        ALR_ENTRY("vkDestroyInstance", alr_vkDestroyInstance),
        ALR_ENTRY("vkEnumeratePhysicalDevices", alr_vkEnumeratePhysicalDevices),
        ALR_ENTRY("vkGetPhysicalDeviceProperties", alr_vkGetPhysicalDeviceProperties),
        ALR_ENTRY("vkGetPhysicalDeviceQueueFamilyProperties", alr_vkGetPhysicalDeviceQueueFamilyProperties),
        ALR_ENTRY("vkCreateDevice", alr_vkCreateDevice),
        ALR_ENTRY("vkDestroyDevice", alr_vkDestroyDevice),
        ALR_ENTRY("vkGetDeviceQueue", alr_vkGetDeviceQueue),
    };
    size_t i;
    if (!pName) return NULL;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (strcmp(table[i].name, pName) == 0) return table[i].fn;
    return NULL;  /* unimplemented entry point (next rung): NULL is the correct GIPA answer */
}

static PFN_vkVoidFunction VKAPI_CALL alr_vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *pName) {
    (void)instance;  /* our dispatch is global (single ICD), so instance is advisory */
    return alr_lookup(pName);
}

static PFN_vkVoidFunction VKAPI_CALL alr_vkGetDeviceProcAddr(VkDevice device,
                                                             const char *pName) {
    (void)device;
    return alr_lookup(pName);
}

/* ============================================================================
 * EXPORTED ABI. Two equivalent discovery paths (we ship both — see the decision note):
 *   (1) DIRECT SONAME: an app/ANGLE that dlopen("libvulkan.so.1") + dlsym the public
 *       "vkGetInstanceProcAddr" goes straight to ours. (Also the plain Vulkan symbols
 *       below for an app that linked -lvulkan and calls vkCreateInstance directly.)
 *   (2) KHRONOS LOADER: vk_icdNegotiateLoaderICDInterfaceVersion +
 *       vk_icdGetInstanceProcAddr let the real Vulkan loader bind us via an ICD
 *       manifest (VK_ICD_FILENAMES). Implemented for free atop the same dispatch.
 * All have default visibility (the build script keeps the TU's exports as-is).
 * ============================================================================ */

/* ---- (2) Khronos loader/ICD interface ---- */
__attribute__((visibility("default")))
VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    /* We support up to interface version 5 semantics (GIPA-based). Clamp to the
     * loader's request but never below our minimum; report the negotiated value. */
    if (!pVersion) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t want = *pVersion;
    uint32_t ours = 5;
    *pVersion = (want < ours) ? want : ours;
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return alr_vkGetInstanceProcAddr(instance, pName);
}

/* vk_icdGetPhysicalDeviceProcAddr (interface v4+): our phys-device functions resolve
 * through the normal GIPA, so just forward. */
__attribute__((visibility("default")))
PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    return alr_lookup(pName);
}

/* ---- (1) Direct public Vulkan symbols. An app that links -lvulkan and calls these
 * by name (rather than through GIPA) binds straight to ours. Thin forwarders. ---- */
__attribute__((visibility("default")))
PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return alr_vkGetInstanceProcAddr(instance, pName);
}
__attribute__((visibility("default")))
PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    return alr_vkGetDeviceProcAddr(device, pName);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator,
                                     VkInstance *pInstance) {
    return alr_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    alr_vkDestroyInstance(instance, pAllocator);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pCount,
                                               VkPhysicalDevice *pDevices) {
    return alr_vkEnumeratePhysicalDevices(instance, pCount, pDevices);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                              VkPhysicalDeviceProperties *pProperties) {
    alr_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                                         uint32_t *pCount,
                                                         VkQueueFamilyProperties *pProps) {
    alr_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, pProps);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice,
                                   const VkDeviceCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    return alr_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    alr_vkDestroyDevice(device, pAllocator);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                 uint32_t queueIndex, VkQueue *pQueue) {
    alr_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}
