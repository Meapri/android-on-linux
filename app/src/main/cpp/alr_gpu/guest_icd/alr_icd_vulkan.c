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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ICD call-trace diagnostic (gated on ALR_ICD_DIAG): emits one "[alr-icd]" line per
 * traced entrypoint to stderr → the loader's guest-stdout capture / logcat. Lets a
 * device run see EXACTLY which vk* calls a real client (ANGLE) makes and which one it
 * stops at — the precise ANGLE-on-Vulkan blocker. Zero cost when the env var is unset. */
static int alr_icd_diag_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ALR_ICD_DIAG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v;
}
#define ALR_ICD_DIAG(...) do { if (alr_icd_diag_on()) { \
    fprintf(stderr, "[alr-icd] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

/* Fires when this ICD is loaded (BEFORE any vk* call). After the Part B rename our
 * SONAME is libalr_mali_icd.so, and the Khronos Vulkan-Loader dlopen()s us via the
 * alr_icd.json manifest (NOT the client directly). Under ALR_ICD_DIAG it proves
 * definitively whether the loader reached OUR ICD — if ANGLE fails to even reach
 * vkCreateInstance, this line still tells us our .so is in the address space + which
 * vk* symbols it can resolve. */
__attribute__((constructor))
static void alr_icd_ctor(void) {
    /* Under ALR_ICD_DIAG this proves the Khronos loader loaded OUR Mali ICD — if this
     * line is ABSENT from the guest output with ALR_ICD_DIAG on, the loader never loaded
     * libalr_mali_icd.so (manifest not found / wrong library_path / the host loader won).
     * The direct-SONAME guests (alr-vk-enum/tri, DT_NEEDED libalr_mali_icd.so) also hit
     * this ctor + resolve vkGetInstanceProcAddr/vkCreateInstance under the ALR loader. */
    ALR_ICD_DIAG("CTOR: ALR guest Mali ICD (libalr_mali_icd.so) loaded into client; ring_ok=%d",
                 alr_icd_ring_ok());
}

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

/* VK-M4: a command buffer IS a dispatchable handle (loader magic first), carrying the
 * virtual vcmd id + a back-pointer to its device (so record/present know vdev/vqueue). */
typedef struct AlrIcdCommandBuffer {
    VK_LOADER_DATA loader;   /* MUST be first */
    uint32_t       vcmd;     /* virtual command-buffer id on the wire */
    AlrIcdDevice*  dev;
} AlrIcdCommandBuffer;

/* ---- monotonic virtual-id allocators (start at 1; 0 reserved). The ENUM rung is
 * light; one global lock serializes id allocation + handle bookkeeping. ---- */
static pthread_mutex_t g_id_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_vinst   = 1;
static uint32_t g_next_vphys   = 100;   /* matches the probe's vphys_base convention */
static uint32_t g_next_vdev    = 1000;
static uint32_t g_next_vqueue  = 2000;
/* VK-M4 (PRESENT rung) virtual-id pools (disjoint ranges so a stray id is diagnosable). */
static uint32_t g_next_vpool   = 3000;
static uint32_t g_next_vcmd    = 4000;
static uint32_t g_next_vshader = 5000;
static uint32_t g_next_vswap   = 6000;
/* The vcmd of the most recent coarse draw-record (alrVkCmdDrawTriangleModules). The
 * single-surface bring-up records then presents, so QUEUE_PRESENT (which keys the host's
 * recorded draw by vcmd) uses this. A multi-surface breadth rung carries vcmd explicitly
 * in the present path; for the bring-up triangle this global is correct + simple. */
static _Atomic uint32_t g_last_record_vcmd = 0;
uint32_t alr_icd_last_record_vcmd(void) {
    return atomic_load_explicit(&g_last_record_vcmd, memory_order_acquire);
}

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

    ALR_ICD_DIAG("vkCreateInstance enter (ring_ok=%d)", alr_icd_ring_ok());
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
    if (inst_result != 0) { free(inst); ALR_ICD_DIAG("vkCreateInstance FAIL host_result=%d", inst_result); return (VkResult)inst_result; }

    *pInstance = (VkInstance)inst;
    ALR_ICD_DIAG("vkCreateInstance OK vinst=%u", vinst);
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
    ALR_ICD_DIAG("vkEnumeratePhysicalDevices (fill=%p ring_ok=%d)",
                 (void *)pPhysicalDevices, alr_icd_ring_ok());

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
    (void)pAllocator;
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdDevice *dev;
    uint32_t vdev, gfx_family = 0;
    int32_t dev_result = 0;
    ALR_ICD_DIAG("vkCreateDevice enter (ext_count=%u ring_ok=%d)",
                 pCreateInfo ? pCreateInfo->enabledExtensionCount : 0u, alr_icd_ring_ok());
    if (!pd || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) { ALR_ICD_DIAG("vkCreateDevice FAIL no-ring"); return VK_ERROR_INITIALIZATION_FAILED; }  /* no GPU to create on */

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
 * VK-M4 (PRESENT rung) entry points: command pool/buffer, GUEST SPIR-V shader module,
 * AHB-backed swapchain (create/images/acquire), the coarse draw-record, and present.
 * Each marshals its request over alr_icd_roundtrip; the non-dispatchable handles carry
 * the virtual id directly (uint64), the command buffer is a dispatchable object.
 * ============================================================================ */

/* Scan a reply stream for ONE present-rung record's payload, by op. Returns 1 if the
 * requested op was found and filled the out params (caller passes only the ones it
 * needs; others may be NULL). A tiny dedicated reader since alr_icd_parse_reply only
 * knows the ENUM-rung ops. */
static int alr_icd_scan_reply(const uint8_t *data, uint32_t len, uint8_t want_op,
                              int32_t *out_result, uint32_t *out_a, uint8_t *out_px) {
    AlrRd r; uint8_t op;
    r.p = data; r.n = len; r.pos = 0;
    for (;;) {
        if (!rd_u8(&r, &op)) break;
        if (op == ALR_VK_REPLY_END) break;
        if (op == ALR_VK_REPLY_SHADER) {
            uint32_t vshader; int32_t res;
            if (!rd_u32(&r, &vshader) || !rd_i32(&r, &res)) return 0;
            if (op == want_op) { if (out_result) *out_result = res; return 1; }
        } else if (op == ALR_VK_REPLY_SWAPCHAIN) {
            uint32_t vsw, cnt; int32_t res;
            if (!rd_u32(&r, &vsw) || !rd_i32(&r, &res) || !rd_u32(&r, &cnt)) return 0;
            if (op == want_op) {
                if (out_result) *out_result = res;
                if (out_a) *out_a = cnt;
                return 1;
            }
        } else if (op == ALR_VK_REPLY_ACQUIRE) {
            uint32_t vsw, idx; int32_t res;
            if (!rd_u32(&r, &vsw) || !rd_u32(&r, &idx) || !rd_i32(&r, &res)) return 0;
            if (op == want_op) {
                if (out_result) *out_result = res;
                if (out_a) *out_a = idx;
                return 1;
            }
        } else if (op == ALR_VK_REPLY_PRESENT) {
            uint32_t vsw, idx; int32_t sr, rr; uint8_t pres, px[4];
            if (!rd_u32(&r, &vsw) || !rd_u32(&r, &idx) || !rd_i32(&r, &sr) ||
                !rd_i32(&r, &rr) || !rd_u8(&r, &pres) || !rd_u8(&r, &px[0]) ||
                !rd_u8(&r, &px[1]) || !rd_u8(&r, &px[2]) || !rd_u8(&r, &px[3])) return 0;
            if (op == want_op) {
                if (out_result) *out_result = sr;
                if (out_a) *out_a = (uint32_t)pres;
                if (out_px) { out_px[0]=px[0]; out_px[1]=px[1]; out_px[2]=px[2]; out_px[3]=px[3]; }
                return 1;
            }
        } else {
            /* an ENUM-rung record we don't care about here: skip its fixed payload via
             * the full parser would be heavy; instead bail (present-rung batches don't
             * mix in ENUM records in practice). */
            return 0;
        }
    }
    return 0;
}

static VkResult VKAPI_CALL alr_vkCreateCommandPool(VkDevice device,
                                                   const VkCommandPoolCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator,
                                                   VkCommandPool *pCommandPool) {
    (void)pCreateInfo; (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vpool;
    if (!dev || !pCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    vpool = alr_alloc(&g_next_vpool, 1);
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_create_command_pool(&e, dev->vdev, vpool);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
    }
    *pCommandPool = (VkCommandPool)vpool;  /* non-dispatchable: carries the virtual id */
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
                                                const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)commandPool; (void)pAllocator;
    /* The host frees pools with the device (vk_real_destroy_device); no per-pool op. */
}

static VkResult VKAPI_CALL alr_vkAllocateCommandBuffers(VkDevice device,
                                                        const VkCommandBufferAllocateInfo *pAllocateInfo,
                                                        VkCommandBuffer *pCommandBuffers) {
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t i, count;
    if (!dev || !pAllocateInfo || !pCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    count = pAllocateInfo->commandBufferCount;
    for (i = 0; i < count; ++i) {
        AlrIcdCommandBuffer *cb = (AlrIcdCommandBuffer *)calloc(1, sizeof(AlrIcdCommandBuffer));
        if (!cb) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        alr_set_loader_magic_value(cb);
        cb->vcmd = alr_alloc(&g_next_vcmd, 1);
        cb->dev = dev;
        if (alr_icd_ring_ok()) {
            uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
            uint32_t vpool = (uint32_t)(uintptr_t)pAllocateInfo->commandPool;
            alr_vk_enc_init(&e, req, sizeof(req));
            alr_vk_enc_allocate_command_buffers(&e, dev->vdev, vpool, cb->vcmd);
            alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
            if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
        }
        pCommandBuffers[i] = (VkCommandBuffer)cb;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL alr_vkCreateShaderModule(VkDevice device,
                                                    const VkShaderModuleCreateInfo *pCreateInfo,
                                                    const VkAllocationCallbacks *pAllocator,
                                                    VkShaderModule *pShaderModule) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vshader, stage = 0;
    int32_t res = 0;
    if (!dev || !pCreateInfo || !pShaderModule || !pCreateInfo->pCode ||
        pCreateInfo->codeSize == 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    vshader = alr_alloc(&g_next_vshader, 1);
    if (alr_icd_ring_ok()) {
        /* The SPIR-V can be larger than a tiny stack buffer; size the request buffer to
         * fit the op header (13 bytes) + the blob (4-byte len + bytes). */
        uint32_t code_len = (uint32_t)pCreateInfo->codeSize;
        uint32_t need = 16 + 4 + code_len;
        uint8_t *req = (uint8_t *)malloc(need);
        uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        if (!req) return VK_ERROR_OUT_OF_HOST_MEMORY;
        AlrVkEncoder e;
        alr_vk_enc_init(&e, req, need);
        alr_vk_enc_create_shader_module(&e, dev->vdev, vshader, stage,
                                        pCreateInfo->pCode, code_len);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_scan_reply(reply, rlen, (uint8_t)ALR_VK_REPLY_SHADER,
                                               &res, NULL, NULL);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else {
            res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        }
        free(req);
    }
    if (res != 0) return (VkResult)res;
    *pShaderModule = (VkShaderModule)vshader;  /* non-dispatchable: virtual id */
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                                 const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev || shaderModule == 0 || !alr_icd_ring_ok()) return;  /* non-dispatchable: 0 == null */
    uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_destroy_shader_module(&e, dev->vdev, (uint32_t)shaderModule);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkCreateSwapchainKHR(VkDevice device,
                                                    const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                    const VkAllocationCallbacks *pAllocator,
                                                    VkSwapchainKHR *pSwapchain) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vswap, w, h, want_count, got_count = 0;
    int32_t res = 0;
    if (!dev || !pCreateInfo || !pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    vswap = alr_alloc(&g_next_vswap, 1);
    w = pCreateInfo->imageExtent.width ? pCreateInfo->imageExtent.width : 64;
    h = pCreateInfo->imageExtent.height ? pCreateInfo->imageExtent.height : 64;
    want_count = pCreateInfo->minImageCount ? pCreateInfo->minImageCount : 2;
    if (alr_icd_ring_ok()) {
        uint8_t req[48]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_create_swapchain(&e, dev->vdev, vswap, w, h, want_count);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_scan_reply(reply, rlen, (uint8_t)ALR_VK_REPLY_SWAPCHAIN,
                                               &res, &got_count, NULL);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else {
            res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (res != 0) return (VkResult)res;
    /* Stash the image count in the high bits so vkGetSwapchainImagesKHR can report it
     * without a round-trip (the count is stable). vswap fits in 24 bits (pool 6000+). */
    *pSwapchain = (VkSwapchainKHR)(((uint64_t)got_count << 32) | (uint64_t)vswap);
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                 const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    if (!dev || swapchain == 0 || !alr_icd_ring_ok()) return;  /* non-dispatchable: 0 == null */
    uint32_t vswap = (uint32_t)(swapchain & 0xffffffffu);
    uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_destroy_swapchain(&e, dev->vdev, vswap);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                       uint32_t *pSwapchainImageCount,
                                                       VkImage *pSwapchainImages) {
    (void)device;
    uint32_t count, i;
    if (!pSwapchainImageCount) return VK_ERROR_INITIALIZATION_FAILED;
    count = (uint32_t)(swapchain >> 32);  /* image count stashed in the high word */
    if (count == 0) count = 1;
    if (pSwapchainImages == NULL) { *pSwapchainImageCount = count; return VK_SUCCESS; }
    uint32_t to_write = (*pSwapchainImageCount < count) ? *pSwapchainImageCount : count;
    /* The "images" are virtual: image index i (the host owns the real AHB images). The
     * guest only ever passes the index back via acquire/present, so the handle == index. */
    for (i = 0; i < to_write; ++i) pSwapchainImages[i] = (VkImage)(uint64_t)i;
    *pSwapchainImageCount = to_write;
    return (to_write < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL alr_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                     uint64_t timeout, VkSemaphore semaphore,
                                                     VkFence fence, uint32_t *pImageIndex) {
    (void)timeout; (void)semaphore; (void)fence;
    AlrIcdDevice *dev = (AlrIcdDevice *)device;
    uint32_t vswap, idx = 0; int32_t res = 0;
    if (!dev || !pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;
    vswap = (uint32_t)(swapchain & 0xffffffffu);
    if (alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_acquire_next_image(&e, dev->vdev, vswap);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) (void)alr_icd_scan_reply(reply, rlen, (uint8_t)ALR_VK_REPLY_ACQUIRE,
                                               &res, &idx, NULL);
            else res = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    if (res != 0) return (VkResult)res;
    *pImageIndex = idx;
    return VK_SUCCESS;
}

/* Coarse draw-record: marshal CMD_BEGIN_DRAW_MODULES (host builds the renderpass +
 * pipeline from the guest's shader modules + draws into the swapchain image). */
void VKAPI_CALL alrVkCmdDrawTriangleModules(VkCommandBuffer commandBuffer,
                                            VkSwapchainKHR swapchain, uint32_t imageIndex,
                                            VkShaderModule vertModule, VkShaderModule fragModule,
                                            uint32_t width, uint32_t height, float bg_r,
                                            float bg_g, float bg_b, float bg_a) {
    AlrIcdCommandBuffer *cb = (AlrIcdCommandBuffer *)commandBuffer;
    if (!cb || !cb->dev || !alr_icd_ring_ok()) return;
    atomic_store_explicit(&g_last_record_vcmd, cb->vcmd, memory_order_release);
    uint32_t vswap = (uint32_t)(swapchain & 0xffffffffu);
    uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
    alr_vk_enc_init(&e, req, sizeof(req));
    alr_vk_enc_cmd_begin_draw_modules(&e, cb->dev->vdev, cb->vcmd, vswap, imageIndex,
                                      (uint32_t)vertModule, (uint32_t)fragModule, width,
                                      height, bg_r, bg_g, bg_b, bg_a);
    alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
    if (!e.overflow) (void)alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
}

static VkResult VKAPI_CALL alr_vkQueuePresentKHR(VkQueue queue,
                                                 const VkPresentInfoKHR *pPresentInfo) {
    AlrIcdQueue *q = (AlrIcdQueue *)queue;
    int32_t res = 0;
    if (!q || !q->dev || !pPresentInfo || pPresentInfo->swapchainCount == 0 ||
        !pPresentInfo->pSwapchains || !pPresentInfo->pImageIndices)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* The host keys the recorded draw by vcmd; QUEUE_PRESENT submits that command buffer,
     * then routes the rendered swapchain AHB to the compositor. The single-surface
     * bring-up records into one command buffer (alrVkCmdDrawTriangleModules) then
     * presents, so we use the most-recently-recorded vcmd. A multi-surface breadth rung
     * carries vcmd explicitly per swapchain. */
    uint32_t vcmd = alr_icd_last_record_vcmd();
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        uint32_t vswap = (uint32_t)(pPresentInfo->pSwapchains[i] & 0xffffffffu);
        uint32_t img = pPresentInfo->pImageIndices[i];
        if (alr_icd_ring_ok()) {
            uint8_t req[48]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
            alr_vk_enc_init(&e, req, sizeof(req));
            alr_vk_enc_queue_present(&e, q->dev->vdev, q->vqueue, vcmd, vswap, img);
            alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
            if (!e.overflow) {
                uint32_t rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
                int32_t one = 0;
                if (rlen && alr_icd_scan_reply(reply, rlen, (uint8_t)ALR_VK_REPLY_PRESENT,
                                               &one, NULL, NULL))
                    res = one;
            }
        }
        if (pPresentInfo->pResults) pPresentInfo->pResults[i] = (VkResult)res;
    }
    return (VkResult)res;
}

/* ============================================================================
 * ANGLE-init enumeration/query rung. ANGLE's RendererVk issues these READ-ONLY
 * bring-up queries (instance/device extension + version + feature + memory + format
 * enumeration) BEFORE it creates any device object. They are answered CLIENT-SIDE with
 * conservative, conformant values (no host round-trip needed during bring-up), which
 * lets ANGLE progress THROUGH vkCreateInstance/vkEnumeratePhysicalDevices/vkCreateDevice
 * to the first device-OBJECT call (vkAllocateMemory / vkCreateImage / vkCreateRenderPass)
 * — the documented boundary where the coarse marshalling wire cannot follow ANGLE's
 * fine-grained Vulkan usage. Implementing them here is real ICD breadth AND pins the
 * gap precisely at the object-creation layer rather than the (now-covered) query layer.
 * ============================================================================ */

/* Helper: copy a fixed list of extension names into the caller's array with the standard
 * two-call (count, then fill) Vulkan semantics. */
static VkResult alr_fill_ext_props(const char *const *names, const uint32_t *specs,
                                   uint32_t avail, uint32_t *pCount,
                                   VkExtensionProperties *pProps) {
    uint32_t i;
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (pProps == NULL) { *pCount = avail; return VK_SUCCESS; }
    uint32_t n = (*pCount < avail) ? *pCount : avail;
    for (i = 0; i < n; ++i) {
        memset(pProps[i].extensionName, 0, VK_MAX_EXTENSION_NAME_SIZE);
        strncpy(pProps[i].extensionName, names[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        pProps[i].specVersion = specs ? specs[i] : 1;
    }
    *pCount = n;
    return (n < avail) ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL alr_vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (pApiVersion) *pApiVersion = VK_API_VERSION_1_1;  /* we model a 1.1 ICD */
    ALR_ICD_DIAG("vkEnumerateInstanceVersion -> 1.1");
    return VK_SUCCESS;
}

VkResult VKAPI_CALL alr_vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    (void)pLayerName;
    /* The WSI surface extensions ANGLE's DisplayVk* probes for, plus the
     * get-physical-device-properties2 it always enables. The actual surface creation
     * is the host/compositor's job (the present rung); reporting them lets ANGLE's
     * RendererVk pick a path instead of bailing at instance creation. */
    static const char *const exts[] = {
        "VK_KHR_surface",
        "VK_KHR_wayland_surface",
        "VK_KHR_xcb_surface",
        "VK_EXT_headless_surface",
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
        "VK_EXT_debug_utils",
    };
    ALR_ICD_DIAG("vkEnumerateInstanceExtensionProperties (props=%p count=%u)",
                 (void *)pProperties, pProperties && pPropertyCount ? *pPropertyCount : 0u);
    return alr_fill_ext_props(exts, NULL,
                              (uint32_t)(sizeof(exts) / sizeof(exts[0])),
                              pPropertyCount, pProperties);
}

VkResult VKAPI_CALL alr_vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)pProperties;
    if (pPropertyCount) *pPropertyCount = 0;  /* no implicit layers in the guest ICD */
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL alr_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    (void)physicalDevice; (void)pLayerName;
    /* The device-level extensions ANGLE enables for a basic render+present path. */
    static const char *const exts[] = {
        "VK_KHR_swapchain",
        "VK_KHR_maintenance1",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_bind_memory2",
    };
    ALR_ICD_DIAG("vkEnumerateDeviceExtensionProperties (props=%p)", (void *)pProperties);
    return alr_fill_ext_props(exts, NULL,
                              (uint32_t)(sizeof(exts) / sizeof(exts[0])),
                              pPropertyCount, pProperties);
}

/* Deprecated (device layers are gone since Vulkan 1.0.13) but the loader still queries it
 * via the instance dispatch table; report ZERO layers — the conformant modern answer. */
static VkResult VKAPI_CALL alr_vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)physicalDevice; (void)pProperties;
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures) {
    (void)physicalDevice;
    /* Conservative: report NO optional features (all VkBool32 = 0). ANGLE treats a
     * cleared features struct as "core 1.0 only" and disables the optional code paths;
     * a richer answer would marshal the real Mali features (next rung). */
    if (pFeatures) memset(pFeatures, 0, sizeof(*pFeatures));
    ALR_ICD_DIAG("vkGetPhysicalDeviceFeatures -> all-zero");
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemProps) {
    (void)physicalDevice;
    if (!pMemProps) return;
    memset(pMemProps, 0, sizeof(*pMemProps));
    /* One device-local heap; one DEVICE_LOCAL type + one HOST_VISIBLE|COHERENT type —
     * the minimal shape a UMA mobile GPU (Mali) exposes. Real values would marshal from
     * the host's vkGetPhysicalDeviceMemoryProperties (next rung). */
    pMemProps->memoryHeapCount = 1;
    pMemProps->memoryHeaps[0].size = (VkDeviceSize)2 * 1024 * 1024 * 1024;  /* 2 GiB */
    pMemProps->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    pMemProps->memoryTypeCount = 2;
    pMemProps->memoryTypes[0].heapIndex = 0;
    pMemProps->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemProps->memoryTypes[1].heapIndex = 0;
    pMemProps->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ALR_ICD_DIAG("vkGetPhysicalDeviceMemoryProperties -> 1 heap, 2 types");
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties) {
    (void)physicalDevice; (void)format;
    /* Advertise broad support so ANGLE's format-capability probe doesn't reject the
     * basic RGBA render path. 0x7FFFFFFF = "all feature bits" (conservative-permissive;
     * a precise answer would marshal vkGetPhysicalDeviceFormatProperties — next rung). */
    if (pFormatProperties) {
        pFormatProperties->linearTilingFeatures = 0x7FFFFFFF;
        pFormatProperties->optimalTilingFeatures = 0x7FFFFFFF;
        pFormatProperties->bufferFeatures = 0x7FFFFFFF;
    }
}

/* ============================================================================
 * ANGLE-init rung, batch 2: the REQUIRED-by-the-loader image/sparse-format queries +
 * the core-1.1 "2" physical-device query family ANGLE's RendererVk::initialize calls.
 *
 * WHY THIS BATCH UNBLOCKS -9 (VK_ERROR_INCOMPATIBLE_DRIVER): the Khronos Vulkan-Loader's
 * loader_icd_init_entries() resolves a FIXED set of physical-device entry points through
 * our vk_icdGetInstanceProcAddr at ICD-scan time; if ANY required one is NULL it logs
 * "Unable to load <fn> from ICD" and REJECTS the ICD (-9). vkGetPhysicalDeviceImage
 * FormatProperties + vkGetPhysicalDeviceSparseImageFormatProperties were the two missing
 * REQUIRED entries (the others were already exported). Adding them to alr_lookup() makes
 * the loader accept us; the "2" family then satisfies ANGLE's own RendererVk queries.
 *
 * FORWARDING MODEL:
 *  - ImageFormatProperties: parameterized, so it ROUND-TRIPS to the REAL Mali
 *    vkGetPhysicalDeviceImageFormatProperties over the ring (op GET_PHYS_IMAGE_FORMAT_PROPS
 *    -> host servicer -> real Mali -> reply). Mali's verdict (incl. a valid
 *    VK_ERROR_FORMAT_NOT_SUPPORTED) flows back verbatim, so the guest mirrors the hardware.
 *  - Properties2 / Features2 / QueueFamilyProperties2 / MemoryProperties2 /
 *    FormatProperties2: the v2 struct is { sType, pNext, <v1 value> }, and the v1 value IS
 *    the real-Mali data we already marshalled (props cache) or already answer (features/
 *    memory/format). So these reuse the v1 entry points to fill the embedded v1 member and
 *    leave any chained pNext untouched (ANGLE tolerates a cleared ID/driver-props chain —
 *    it only WARNs on the driver name). This keeps the wire minimal while the data stays
 *    real-Mali-truthful where it matters (the device props/queues come from Mali).
 *  - SparseImageFormatProperties(2): report ZERO properties (no sparse support) — a
 *    conformant answer ANGLE's basic render path tolerates (it does not require sparse).
 * ============================================================================ */

/* Scan a reply stream for the ONE ALR_VK_REPLY_IMAGE_FORMAT_PROPS record, filling the
 * out params. Returns 1 if found. A dedicated reader (the ENUM-rung alr_icd_parse_reply
 * only knows props records; this record has its own fixed payload). */
static int alr_icd_scan_image_format_reply(const uint8_t *data, uint32_t len,
                                           int32_t *out_result, uint32_t *out_w,
                                           uint32_t *out_h, uint32_t *out_d,
                                           uint32_t *out_mips, uint32_t *out_layers,
                                           uint32_t *out_samples, uint64_t *out_maxsz) {
    AlrRd r; uint8_t op;
    r.p = data; r.n = len; r.pos = 0;
    for (;;) {
        if (!rd_u8(&r, &op)) break;
        if (op == ALR_VK_REPLY_END) break;
        if (op == ALR_VK_REPLY_IMAGE_FORMAT_PROPS) {
            uint32_t vphys, w, h, d, mips, layers, samples; int32_t res;
            uint8_t maxsz_b[8]; uint64_t maxsz;
            if (!rd_u32(&r, &vphys) || !rd_i32(&r, &res) || !rd_u32(&r, &w) ||
                !rd_u32(&r, &h) || !rd_u32(&r, &d) || !rd_u32(&r, &mips) ||
                !rd_u32(&r, &layers) || !rd_u32(&r, &samples)) return 0;
            /* u64 max_resource_size (no rd_u64 helper; read 8 bytes LE). */
            if (r.pos + 8 > r.n) return 0;
            memcpy(maxsz_b, r.p + r.pos, 8); r.pos += 8;
            memcpy(&maxsz, maxsz_b, 8);
            if (out_result) *out_result = res;
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            if (out_d) *out_d = d;
            if (out_mips) *out_mips = mips;
            if (out_layers) *out_layers = layers;
            if (out_samples) *out_samples = samples;
            if (out_maxsz) *out_maxsz = maxsz;
            return 1;
        }
        /* Any other record here is unexpected for this single-op batch; bail. */
        return 0;
    }
    return 0;
}

static VkResult VKAPI_CALL alr_vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties *pImageFormatProperties) {
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    int32_t res = (int32_t)VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t w = 0, h = 0, d = 0, mips = 0, layers = 0, samples = 0;
    uint64_t maxsz = 0;
    if (!pd || !pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    ALR_ICD_DIAG("vkGetPhysicalDeviceImageFormatProperties fmt=%d type=%d tiling=%d "
                 "usage=0x%x (ring_ok=%d)", (int)format, (int)type, (int)tiling,
                 (unsigned)usage, alr_icd_ring_ok());
    if (alr_icd_ring_ok()) {
        uint8_t req[64]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH]; uint32_t rlen;
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_get_phys_image_format_props(&e, pd->inst->vinst, pd->vphys,
                                               (uint32_t)format, (uint32_t)type,
                                               (uint32_t)tiling, (uint32_t)usage,
                                               (uint32_t)flags);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen)
                (void)alr_icd_scan_image_format_reply(reply, rlen, &res, &w, &h, &d, &mips,
                                                      &layers, &samples, &maxsz);
        }
    } else {
        /* Ring-less: report a generous "supported" answer so a no-host guest still
         * progresses (the same conservative-permissive stance the v1 props take). */
        res = (int32_t)VK_SUCCESS;
        w = 16384; h = 16384; d = 1; mips = 15; layers = 2048;
        samples = 0x1; maxsz = (uint64_t)1 << 31;
    }
    if (res != 0) return (VkResult)res;  /* propagate Mali's verdict (e.g. NOT_SUPPORTED) */
    pImageFormatProperties->maxExtent.width = w;
    pImageFormatProperties->maxExtent.height = h;
    pImageFormatProperties->maxExtent.depth = d;
    pImageFormatProperties->maxMipLevels = mips;
    pImageFormatProperties->maxArrayLayers = layers;
    pImageFormatProperties->sampleCounts = samples;
    pImageFormatProperties->maxResourceSize = (VkDeviceSize)maxsz;
    return VK_SUCCESS;
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
    VkSampleCountFlags samples, VkImageUsageFlags usage, VkImageTiling tiling,
    uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties) {
    (void)physicalDevice; (void)format; (void)type; (void)samples; (void)usage;
    (void)tiling; (void)pProperties;
    /* No sparse-residency support: report ZERO properties (a conformant answer; ANGLE's
     * basic render path does not require sparse). The two-call form just yields count=0. */
    if (pPropertyCount) *pPropertyCount = 0;
    ALR_ICD_DIAG("vkGetPhysicalDeviceSparseImageFormatProperties -> 0 (no sparse)");
}

/* ---- the core-1.1 "2" family: fill the embedded v1 struct from the v1 entry points,
 * leaving any chained pNext untouched (ANGLE tolerates a cleared chain). ---- */
static void VKAPI_CALL alr_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    /* Do NOT memset the whole struct: the caller chained pNext structs (ANGLE attaches
     * VkPhysicalDeviceIDProperties + DriverProperties) that we must not stomp. Only fill
     * the embedded v1 .properties (the real-Mali data, via the v1 entry point). */
    alr_vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
    ALR_ICD_DIAG("vkGetPhysicalDeviceProperties2 (pNext=%p) name=%s",
                 (void *)pProperties->pNext, pProperties->properties.deviceName);
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    alr_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
    ALR_ICD_DIAG("vkGetPhysicalDeviceFeatures2 (pNext=%p)", (void *)pFeatures->pNext);
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
    VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    if (!pQueueFamilyPropertyCount) return;
    if (pQueueFamilyProperties == NULL) {
        /* Count form: delegate to the v1 query with a NULL fill array. */
        alr_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,
                                                     pQueueFamilyPropertyCount, NULL);
        return;
    }
    /* Fill form: gather the v1 array, then copy each into the embedded v1 member of the
     * "2" structs (leaving each element's pNext chain untouched). */
    uint32_t n = *pQueueFamilyPropertyCount, i;
    VkQueueFamilyProperties tmp[16];
    if (n > 16) n = 16;
    uint32_t got = n;
    alr_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &got, tmp);
    for (i = 0; i < got; ++i) pQueueFamilyProperties[i].queueFamilyProperties = tmp[i];
    *pQueueFamilyPropertyCount = got;
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    if (!pMemoryProperties) return;
    alr_vkGetPhysicalDeviceMemoryProperties(physicalDevice,
                                            &pMemoryProperties->memoryProperties);
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties2 *pFormatProperties) {
    if (!pFormatProperties) return;
    alr_vkGetPhysicalDeviceFormatProperties(physicalDevice, format,
                                            &pFormatProperties->formatProperties);
}

static VkResult VKAPI_CALL alr_vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties) {
    if (!pImageFormatInfo || !pImageFormatProperties)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Forward to the v1 path (which round-trips to real Mali), filling the embedded v1
     * member. Any pNext (e.g. VkSamplerYcbcrConversionImageFormatProperties) is left
     * as the caller initialized it — a conformant default for the bring-up. */
    return alr_vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
        pImageFormatInfo->tiling, pImageFormatInfo->usage, pImageFormatInfo->flags,
        &pImageFormatProperties->imageFormatProperties);
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
        /* ---- ANGLE-init enumeration/query rung (read-only bring-up) ---- */
        ALR_ENTRY("vkEnumerateInstanceVersion", alr_vkEnumerateInstanceVersion),
        ALR_ENTRY("vkEnumerateInstanceExtensionProperties", alr_vkEnumerateInstanceExtensionProperties),
        ALR_ENTRY("vkEnumerateInstanceLayerProperties", alr_vkEnumerateInstanceLayerProperties),
        ALR_ENTRY("vkEnumerateDeviceExtensionProperties", alr_vkEnumerateDeviceExtensionProperties),
        ALR_ENTRY("vkEnumerateDeviceLayerProperties", alr_vkEnumerateDeviceLayerProperties),
        ALR_ENTRY("vkGetPhysicalDeviceFeatures", alr_vkGetPhysicalDeviceFeatures),
        ALR_ENTRY("vkGetPhysicalDeviceMemoryProperties", alr_vkGetPhysicalDeviceMemoryProperties),
        ALR_ENTRY("vkGetPhysicalDeviceFormatProperties", alr_vkGetPhysicalDeviceFormatProperties),
        /* The two REQUIRED-by-the-Khronos-loader physical-device entry points whose
         * absence made loader_icd_init_entries reject us with -9 (the precise fix). */
        ALR_ENTRY("vkGetPhysicalDeviceImageFormatProperties", alr_vkGetPhysicalDeviceImageFormatProperties),
        ALR_ENTRY("vkGetPhysicalDeviceSparseImageFormatProperties", alr_vkGetPhysicalDeviceSparseImageFormatProperties),
        /* The core-1.1 "2" query family ANGLE's RendererVk::initialize calls (Properties2
         * is used in ChoosePhysicalDevice; the rest in the device bring-up). */
        ALR_ENTRY("vkGetPhysicalDeviceProperties2", alr_vkGetPhysicalDeviceProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceFeatures2", alr_vkGetPhysicalDeviceFeatures2),
        ALR_ENTRY("vkGetPhysicalDeviceQueueFamilyProperties2", alr_vkGetPhysicalDeviceQueueFamilyProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceMemoryProperties2", alr_vkGetPhysicalDeviceMemoryProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceFormatProperties2", alr_vkGetPhysicalDeviceFormatProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceImageFormatProperties2", alr_vkGetPhysicalDeviceImageFormatProperties2),
        /* The KHR aliases (ANGLE may resolve the VK_KHR_get_physical_device_properties2
         * names when it enables that instance extension instead of relying on core 1.1). */
        ALR_ENTRY("vkGetPhysicalDeviceProperties2KHR", alr_vkGetPhysicalDeviceProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceFeatures2KHR", alr_vkGetPhysicalDeviceFeatures2),
        ALR_ENTRY("vkGetPhysicalDeviceQueueFamilyProperties2KHR", alr_vkGetPhysicalDeviceQueueFamilyProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceMemoryProperties2KHR", alr_vkGetPhysicalDeviceMemoryProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceFormatProperties2KHR", alr_vkGetPhysicalDeviceFormatProperties2),
        ALR_ENTRY("vkGetPhysicalDeviceImageFormatProperties2KHR", alr_vkGetPhysicalDeviceImageFormatProperties2),
        ALR_ENTRY("vkCreateDevice", alr_vkCreateDevice),
        ALR_ENTRY("vkDestroyDevice", alr_vkDestroyDevice),
        ALR_ENTRY("vkGetDeviceQueue", alr_vkGetDeviceQueue),
        /* ---- VK-M4 (PRESENT rung) ---- */
        ALR_ENTRY("vkCreateCommandPool", alr_vkCreateCommandPool),
        ALR_ENTRY("vkDestroyCommandPool", alr_vkDestroyCommandPool),
        ALR_ENTRY("vkAllocateCommandBuffers", alr_vkAllocateCommandBuffers),
        ALR_ENTRY("vkCreateShaderModule", alr_vkCreateShaderModule),
        ALR_ENTRY("vkDestroyShaderModule", alr_vkDestroyShaderModule),
        ALR_ENTRY("vkCreateSwapchainKHR", alr_vkCreateSwapchainKHR),
        ALR_ENTRY("vkDestroySwapchainKHR", alr_vkDestroySwapchainKHR),
        ALR_ENTRY("vkGetSwapchainImagesKHR", alr_vkGetSwapchainImagesKHR),
        ALR_ENTRY("vkAcquireNextImageKHR", alr_vkAcquireNextImageKHR),
        ALR_ENTRY("vkQueuePresentKHR", alr_vkQueuePresentKHR),
        ALR_ENTRY("alrVkCmdDrawTriangleModules", alrVkCmdDrawTriangleModules),
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

/* vk_icdGetPhysicalDeviceProcAddr (interface v4+): the Khronos loader calls this to find
 * functions whose FIRST parameter is a VkPhysicalDevice, and a non-NULL return makes the
 * loader build a physical-device trampoline+terminator for that name. The contract (LunarG
 * LoaderDriverInterface) is that it must return NULL for any function that does NOT take a
 * VkPhysicalDevice first (global/instance/device-level), so we restrict to the
 * physical-device family rather than forwarding the whole table — returning ours for, say,
 * a VkDevice-first function (vkCreateSwapchainKHR) would mis-route it as a phys-device call.
 * The loader still resolves the non-phys functions through vk_icdGetInstanceProcAddr. */
static int alr_is_phys_device_fn(const char *n) {
    static const char *const phys_fns[] = {
        "vkGetPhysicalDeviceProperties",
        "vkGetPhysicalDeviceQueueFamilyProperties",
        "vkGetPhysicalDeviceFeatures",
        "vkGetPhysicalDeviceMemoryProperties",
        "vkGetPhysicalDeviceFormatProperties",
        "vkGetPhysicalDeviceImageFormatProperties",
        "vkGetPhysicalDeviceSparseImageFormatProperties",
        "vkGetPhysicalDeviceProperties2",
        "vkGetPhysicalDeviceFeatures2",
        "vkGetPhysicalDeviceQueueFamilyProperties2",
        "vkGetPhysicalDeviceMemoryProperties2",
        "vkGetPhysicalDeviceFormatProperties2",
        "vkGetPhysicalDeviceImageFormatProperties2",
        "vkGetPhysicalDeviceProperties2KHR",
        "vkGetPhysicalDeviceFeatures2KHR",
        "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
        "vkGetPhysicalDeviceMemoryProperties2KHR",
        "vkGetPhysicalDeviceFormatProperties2KHR",
        "vkGetPhysicalDeviceImageFormatProperties2KHR",
        "vkEnumerateDeviceExtensionProperties",
        "vkEnumerateDeviceLayerProperties",
        "vkCreateDevice",  /* takes VkPhysicalDevice first */
    };
    size_t i;
    if (!n) return 0;
    for (i = 0; i < sizeof(phys_fns) / sizeof(phys_fns[0]); ++i)
        if (strcmp(phys_fns[i], n) == 0) return 1;
    return 0;
}
__attribute__((visibility("default")))
PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    if (!alr_is_phys_device_fn(pName)) return NULL;
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
/* Global commands (NULL-instance enumerate) ANGLE may call by exported symbol, plus the
 * physical-device queries it issues during RendererVk bring-up. Thin forwarders. */
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    return alr_vkEnumerateInstanceVersion(pApiVersion);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                                           uint32_t *pPropertyCount,
                                                           VkExtensionProperties *pProperties) {
    return alr_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                       VkLayerProperties *pProperties) {
    return alr_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                         const char *pLayerName,
                                                         uint32_t *pPropertyCount,
                                                         VkExtensionProperties *pProperties) {
    return alr_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
                                                     pPropertyCount, pProperties);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                     uint32_t *pPropertyCount,
                                                     VkLayerProperties *pProperties) {
    return alr_vkEnumerateDeviceLayerProperties(physicalDevice, pPropertyCount, pProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                            VkPhysicalDeviceFeatures *pFeatures) {
    alr_vkGetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                    VkPhysicalDeviceMemoryProperties *pMemProps) {
    alr_vkGetPhysicalDeviceMemoryProperties(physicalDevice, pMemProps);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                                    VkFormat format,
                                                    VkFormatProperties *pFormatProperties) {
    alr_vkGetPhysicalDeviceFormatProperties(physicalDevice, format, pFormatProperties);
}
/* ---- ANGLE-init rung public symbols (the REQUIRED image/sparse format queries + the
 * core-1.1 "2" family). The direct-SONAME path (an app/ANGLE that dlsyms these by name)
 * binds straight to ours; the loader route resolves them via vk_icdGetInstanceProcAddr. */
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties *pImageFormatProperties) {
    return alr_vkGetPhysicalDeviceImageFormatProperties(physicalDevice, format, type, tiling,
                                                        usage, flags, pImageFormatProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
    VkSampleCountFlags samples, VkImageUsageFlags usage, VkImageTiling tiling,
    uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties) {
    alr_vkGetPhysicalDeviceSparseImageFormatProperties(physicalDevice, format, type, samples,
                                                       usage, tiling, pPropertyCount,
                                                       pProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                               VkPhysicalDeviceProperties2 *pProperties) {
    alr_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                             VkPhysicalDeviceFeatures2 *pFeatures) {
    alr_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
    VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    alr_vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pQueueFamilyPropertyCount,
                                                  pQueueFamilyProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    alr_vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties2 *pFormatProperties) {
    alr_vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties) {
    return alr_vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo,
                                                         pImageFormatProperties);
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

/* ---- VK-M4 (PRESENT rung) public symbols (a guest app linked -lvulkan calls these). ---- */
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
                                        const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool) {
    return alr_vkCreateCommandPool(device, pCreateInfo, pAllocator, pCommandPool);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
                                     const VkAllocationCallbacks *pAllocator) {
    alr_vkDestroyCommandPool(device, commandPool, pAllocator);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
                                             VkCommandBuffer *pCommandBuffers) {
    return alr_vkAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                                         const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    return alr_vkCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                      const VkAllocationCallbacks *pAllocator) {
    alr_vkDestroyShaderModule(device, shaderModule, pAllocator);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                         const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    return alr_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
}
__attribute__((visibility("default")))
void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                      const VkAllocationCallbacks *pAllocator) {
    alr_vkDestroySwapchainKHR(device, swapchain, pAllocator);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                            uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages) {
    return alr_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                          VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
    return alr_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}
__attribute__((visibility("default")))
VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    return alr_vkQueuePresentKHR(queue, pPresentInfo);
}
