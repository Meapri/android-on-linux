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
#include "alr_icd_gen_glue.h"  /* AlrVkReader + the same-process arena glue (gen support) */
/* The GENERATED passthrough wire (the 300.. escape band): per-entrypoint encoders for the
 * memory + buffer + image + image-view render batch. Produced by
 * tools/gen_vk_passthrough.py; C-clean (extern "C"). */
#include "alr_gpu/generated/alr_gpu_vk_gen_proto.hpp"
/* The GUEST-SIDE command-buffer RECORDER + submit/sync ring encoders (the cmd-log band):
 * AlrIcdCmdLog + the per-vkCmd* record helpers + the vkQueueSubmit/fence-sync ring builders.
 * Needs AlrVkReader (alr_icd_gen_glue.h, above) + AlrVkEncoder (alr_gpu_vk_proto.hpp, above)
 * in scope, so it MUST follow both. The cmd-record VKAPI entrypoints that drive these live in
 * alr_icd_cmd_entrypoints.inc (included after the dispatch types, below). */
#include "alr_icd_cmd_record.h"

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

/* ====================================================================================
 * LEAD-1 create-call pNext FORWARDING. Every alr_vkCreate* used to hardcode
 * alr_vk_gen_pnext_count(&e, 0), DROPPING ANGLE's whole pNext chain. This helper ships the
 * POINTERLESS allowlisted structs VERBATIM (the host relinks them into the real Mali create),
 * and ALWAYS diag-logs every chained sType (FWD/DROP) under ALR_ICD_DIAG so a device run names
 * EXACTLY which extension structs ANGLE attaches to each create — ground truth for the
 * first-texture wall. POINTER-BEARING structs (binding-flags, format-list) are class (B):
 * the size table returns 0, so they are diag-logged as DROP here and handled by a dedicated
 * inline encoding in their own entrypoint (NOT shipped through this generic path, which would
 * carry a dangling guest pointer). Mirrors vkCreateDevice's proven feature-chain marshalling. */
static const char *alr_icd_pnext_stype_name(uint32_t s) {
    switch (s) {
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_VAL:  return "ExternalMemoryImageCreateInfo";
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO_VAL: return "ExternalMemoryBufferCreateInfo";
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO_VAL:    return "ImageStencilUsageCreateInfo";
        case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO_VAL:       return "ImageViewUsageCreateInfo";
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO_VAL: return "BufferOpaqueCaptureAddressCreateInfo";
        case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_VAL: return "DescriptorSetLayoutBindingFlagsCreateInfo";
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_VAL:      return "ImageFormatListCreateInfo";
        default: return "(other)";
    }
}
/* Count the POINTERLESS allowlisted pNext structs (for buffer-size planning) AND emit the
 * per-sType FWD/DROP diag for the WHOLE chain. Returns the count to ship verbatim; *out_bytes
 * accumulates their total wire payload (sType u32 + len u32 + struct bytes). */
static uint32_t alr_icd_count_create_pnext(const void *p_next, const char *who,
                                           uint32_t *out_bytes) {
    uint32_t count = 0, bytes = 0;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)p_next; p; p = p->pNext) {
        uint32_t sz = alr_icd_create_pnext_struct_size((uint32_t)p->sType);
        ALR_ICD_DIAG("%s pNext sType=%u %s %s", who, (uint32_t)p->sType,
                     alr_icd_pnext_stype_name((uint32_t)p->sType),
                     sz ? "FWD" : "DROP(pointer-bearing or not in create allowlist)");
        if (sz) { count++; bytes += 4u + 4u + sz; }
    }
    if (out_bytes) *out_bytes = bytes;
    return count;
}
/* Emit the pNext header (count) + each POINTERLESS allowlisted struct verbatim. The host
 * (vk_gen_relink_pnext) re-validates each sType against its OWN allowlist before chaining it
 * to real Mali, so an unknown sType can never make the driver walk a bogus chain. */
static void alr_icd_emit_create_pnext(AlrVkEncoder *e, const void *p_next, uint32_t count) {
    alr_vk_gen_pnext_count(e, count);
    if (!count) return;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)p_next; p; p = p->pNext) {
        uint32_t sz = alr_icd_create_pnext_struct_size((uint32_t)p->sType);
        if (sz) alr_vk_gen_pnext(e, (uint32_t)p->sType, p, sz);
    }
}

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
    AlrIcdCmdLog   log;      /* the guest-local cmd-record byte log (vkCmd* append here, ship
                              * at vkQueueSubmit). See alr_icd_cmd_record.h. */
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
/* GENERATED render-batch virtual-id pools (alr_gpu/generated/, the 300.. escape band):
 * buffers/images/image-views/device-memory. Disjoint high ranges so a stray id is
 * diagnosable. Referenced by the generated ICD entrypoints (alr_gpu_vk_gen_icd.inc). */
static uint32_t g_next_vbuf    = 7000000;
static uint32_t g_next_vimg    = 8000000;
static uint32_t g_next_vview   = 8500000;
static uint32_t g_next_vmem    = 9000000;
/* WAVE A generated create-resource virtual-id pools (shader module / pipeline cache /
 * sampler / fence / semaphore / event / query pool). Disjoint high ranges so a stray id is
 * diagnosable; referenced by the generated ICD entrypoints (alr_gpu_vk_gen_icd.inc). Note
 * g_next_vshader (5000) is the HAND-WRITTEN 218-band shader module — distinct from the
 * generated g_next_vshmod, so the two shader-module paths never alias a virtual id. */
/* g_next_vshmod: the generated shader-module create is in ICD_SKIP (hand-written on the
 * 218 band), so the generated ICD function that would consume this counter is not emitted.
 * Kept (with the other WAVE A counters) for a uniform pool layout + so a future un-skip
 * needs no new counter; marked unused so -Werror builds stay green. */
__attribute__((unused)) static uint32_t g_next_vshmod  = 10000000;
static uint32_t g_next_vpcache = 11000000;
static uint32_t g_next_vsamp   = 12000000;
static uint32_t g_next_vfence  = 13000000;
static uint32_t g_next_vsem    = 14000000;
static uint32_t g_next_vevent  = 15000000;
static uint32_t g_next_vqpool  = 16000000;
/* WAVE B generated descriptor/layout virtual-id pools. */
static uint32_t g_next_vdsl     = 17000000;
static uint32_t g_next_vplayout = 18000000;
static uint32_t g_next_vdpool   = 19000000;
static uint32_t g_next_vdset    = 20000000;
/* WAVE C generated render-pass / framebuffer virtual-id pools. */
static uint32_t g_next_vrpass   = 21000000;
static uint32_t g_next_vfb      = 22000000;
/* WAVE (pipeline) generated graphics/compute-pipeline virtual-id pool. Referenced by the
 * generated alr_vkCreate{Graphics,Compute}Pipelines (alr_gpu_vk_gen_icd.inc). */
static uint32_t g_next_vpipe    = 23000000;
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
/* Exact official-ABI sizes of the two structs the host ships as raw bytes (mirrors
 * ALR_VK_PHYS_FEATURES_BYTES / ALR_VK_PHYS_LIMITS_BYTES on the host). 64-bit arm64 on
 * both ends, so a raw memcpy of the structs is layout-correct. */
#define ALR_ICD_FEATURES_BYTES 220u  /* sizeof(VkPhysicalDeviceFeatures) = 55 * VkBool32 */
#define ALR_ICD_LIMITS_BYTES   504u  /* sizeof(VkPhysicalDeviceLimits) */
#define ALR_ICD_MEMORY_BYTES   520u  /* sizeof(VkPhysicalDeviceMemoryProperties) */
_Static_assert(sizeof(VkPhysicalDeviceFeatures) == ALR_ICD_FEATURES_BYTES,
               "VkPhysicalDeviceFeatures must be 220 bytes for the raw-bytes wire copy");
_Static_assert(sizeof(VkPhysicalDeviceLimits) == ALR_ICD_LIMITS_BYTES,
               "VkPhysicalDeviceLimits must be 504 bytes for the raw-bytes wire copy");
/* ABI offset lock: the CALLER (ANGLE, real Vulkan headers) reads .limits at offset 296. If
 * our VkPhysicalDeviceProperties places it elsewhere, the memcpy of the real Mali limits into
 * the caller's struct lands at the wrong offset and ANGLE reads every limit shifted (the
 * DEVICE-PROVEN libGLESv2+0x1f6db4 caps-init crash). 296 = 4*4 (api/drv/vendor/devID) + 4
 * (deviceType) + 256 (deviceName) + 16 (pipelineCacheUUID) padded up to the 8-aligned limits. */
_Static_assert(offsetof(VkPhysicalDeviceProperties, limits) == 296,
               "VkPhysicalDeviceProperties.limits MUST be at offset 296 (official ABI) so the "
               "raw-Mali-limits memcpy lands where ANGLE reads it");
_Static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == ALR_ICD_MEMORY_BYTES,
               "VkPhysicalDeviceMemoryProperties must be 520 bytes for the raw-bytes wire copy");
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
    /* ANGLE caps-init rung: the REAL Mali features + limits raw bytes (host-shipped via the
     * extended PHYS_PROPS record). have_features/have_limits are set only when the host sent
     * a correctly-SIZED blob; otherwise the v1 query keeps its conservative default. */
    int      have_features;
    int      have_limits;
    uint8_t  features_raw[ALR_ICD_FEATURES_BYTES];
    uint8_t  limits_raw[ALR_ICD_LIMITS_BYTES];
    /* ANGLE memory-type-selection rung: the REAL Mali VkPhysicalDeviceMemoryProperties raw
     * bytes (host-shipped via the same extended PHYS_PROPS record). have_memprops is set only
     * when the host sent a correctly-SIZED blob; otherwise the query keeps its conservative
     * single-heap default. */
    int      have_memprops;
    uint8_t  memprops_raw[ALR_ICD_MEMORY_BYTES];
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
                /* ANGLE caps-init rung: the REAL Mali features + limits raw blobs follow
                 * is_software. Copy them into the cache only at the exact expected size, so
                 * a mismatched build can never scribble past the fixed cache arrays; a 0-length
                 * blob (synthetic-wire / no-host) just leaves have_features/have_limits = 0. */
                {
                    const uint8_t *feat_b = NULL, *lim_b = NULL, *mem_b = NULL;
                    uint32_t feat_len = 0, lim_len = 0, mem_len = 0;
                    /* features + limits + memory blobs follow is_software, in that order. The
                     * memory blob is read defensively: rd_blob fails cleanly at stream end, so
                     * an older host that didn't ship it leaves have_memprops = 0 (conservative
                     * default) rather than corrupting the parse. */
                    if (!rd_blob(&r, &feat_b, &feat_len) || !rd_blob(&r, &lim_b, &lim_len) ||
                        !rd_blob(&r, &mem_b, &mem_len)) return 0;
                    if (slot && feat_len == ALR_ICD_FEATURES_BYTES) {
                        memcpy(slot->features_raw, feat_b, ALR_ICD_FEATURES_BYTES);
                        slot->have_features = 1;
                    }
                    if (slot && lim_len == ALR_ICD_LIMITS_BYTES) {
                        memcpy(slot->limits_raw, lim_b, ALR_ICD_LIMITS_BYTES);
                        slot->have_limits = 1;
                    }
                    if (slot && mem_len == ALR_ICD_MEMORY_BYTES) {
                        memcpy(slot->memprops_raw, mem_b, ALR_ICD_MEMORY_BYTES);
                        slot->have_memprops = 1;
                    }
                }
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

    /* VERSION-SKEW FIX (LEAD 2): default the app-requested instance version to 1.3 (matching
     * the 1.3 vkEnumerateInstanceVersion + the real-Mali 1.3 device) when the client left it
     * 0, instead of clamping to 1.1. ANGLE passes its OWN apiVersion (honored verbatim), so
     * this only affects clients that pass appInfo.apiVersion == 0. */
    app_api = VK_API_VERSION_1_3;
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
    ALR_ICD_DIAG("vkEnumeratePhysicalDevices (fill=%p in_count=%u cached=%u ring_ok=%d)",
                 (void *)pPhysicalDevices, *pPhysicalDeviceCount, inst->phys_count,
                 alr_icd_ring_ok());

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
        ALR_ICD_DIAG("vkEnumeratePhysicalDevices count-query -> %u", count);
        return VK_SUCCESS;
    }

    /* Fill call: hand back up to *pPhysicalDeviceCount handles. */
    uint32_t to_write = (*pPhysicalDeviceCount < count) ? *pPhysicalDeviceCount : count;
    ALR_ICD_DIAG("vkEnumeratePhysicalDevices fill in_count=%u count=%u to_write=%u%s",
                 *pPhysicalDeviceCount, count, to_write,
                 (to_write < count) ? " -> VK_INCOMPLETE" : " -> VK_SUCCESS");
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
        /* DIAGNOSTIC (ALR_ICD_APIVER_CAP=<dotless major.minor, e.g. "11" for 1.1>): cap the
         * REPORTED device apiVersion. Tests whether ANGLE's first-glTexImage2D NULL-deref is
         * driven by ANGLE enabling Vulkan 1.2/1.3 CORE paths (whose promoted entrypoints we
         * don't all implement) off our real-Mali 1.3 apiVersion — capping to 1.1 confines
         * ANGLE to the core fns we fully wire. Default unset → real Mali version (no change). */
        {
            const char *cap = getenv("ALR_ICD_APIVER_CAP");
            if (cap && cap[0] == '1' && cap[1] == '1') pProperties->apiVersion = VK_API_VERSION_1_1;
            else if (cap && cap[0] == '1' && cap[1] == '2')
                pProperties->apiVersion = VK_MAKE_API_VERSION(0, 1, 2, 0);
            else if (cap && cap[0] == '1' && cap[1] == '0')
                pProperties->apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        }
        pProperties->driverVersion = slot->driver_version;
        pProperties->vendorID = slot->vendor_id;
        pProperties->deviceID = slot->device_id;
        pProperties->deviceType = (VkPhysicalDeviceType)slot->device_type;
        /* deviceName: the proof string ("Mali-G615 MC2") flowed through the ICD. */
        strncpy(pProperties->deviceName, slot->device_name,
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
        /* ANGLE caps-init rung: forward the FULL real-Mali VkPhysicalDeviceLimits. The
         * host shipped the struct's raw official-ABI bytes; the ICD's VkPhysicalDeviceLimits
         * is the correctly-sized opaque blob at the official offset, so a raw memcpy lands
         * every field (maxImageDimension*, maxBoundDescriptorSets, maxColorAttachments,
         * the *SampleCounts flags, ...) exactly where ANGLE's ensureCapsInitialized reads
         * them. Without this the limits were all-zero -> ANGLE std::vector::reserve(0-ish
         * bogus) -> length_error abort. */
        if (slot->have_limits) {
            memcpy(&pProperties->limits, slot->limits_raw, ALR_ICD_LIMITS_BYTES);
            ALR_ICD_DIAG("vkGetPhysicalDeviceProperties -> REAL Mali limits forwarded (%u B)",
                         (unsigned)ALR_ICD_LIMITS_BYTES);
            /* Dump the limits ANGLE most plausibly turns into a vector count/reserve during
             * caps init (a 0 or UINT32_MAX here can drive a length_error/bad_alloc grow).
             * VkPhysicalDeviceLimits is an opaque 504B blob here, so read by OFFSET against
             * the official arm64 ABI. Key uint32 fields (offset → field):
             *   0  maxImageDimension1D     4  maxImageDimension2D    8  maxImageDimension3D
             *   12 maxImageDimensionCube  16  maxImageArrayLayers   36  maxMemoryAllocationCount
             *   168 maxBoundDescriptorSets 232 maxPerStageResources  280 maxVertexInputAttributes
             *   284 maxVertexInputBindings 360 maxColorAttachments. We dump the FULL 504B as
             * 126 u32 words so NO field is missed when correlating with ANGLE's caps reads. */
            if (alr_icd_diag_on()) {
                const uint8_t *lb = (const uint8_t *)&pProperties->limits;
                uint32_t w[126]; memcpy(w, lb, 504);
                ALR_ICD_DIAG("  lim[off0..16] imgDim1D=%u 2D=%u 3D=%u Cube=%u arrLayers=%u",
                             w[0], w[1], w[2], w[3], w[4]);
                ALR_ICD_DIAG("  lim maxMemoryAllocationCount(off36)=%u maxBoundDescriptorSets(off168)=%u "
                             "maxPerStageResources(off232)=%u",
                             w[9], w[42], w[58]);
                ALR_ICD_DIAG("  lim maxVtxInAttr(off280)=%u maxVtxInBind(off284)=%u maxColorAtt(off360)=%u",
                             w[70], w[71], w[90]);
                /* full word dump in 6 chunks of 21 so a huge/zero anywhere is visible */
                for (uint32_t c = 0; c < 6; ++c)
                    ALR_ICD_DIAG("  limW[%u..]=%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                                 c*21, w[c*21+0],w[c*21+1],w[c*21+2],w[c*21+3],w[c*21+4],w[c*21+5],w[c*21+6],
                                 w[c*21+7],w[c*21+8],w[c*21+9],w[c*21+10],w[c*21+11],w[c*21+12],w[c*21+13],
                                 w[c*21+14],w[c*21+15],w[c*21+16],w[c*21+17],w[c*21+18],w[c*21+19],w[c*21+20]);
            }
        } else {
            ALR_ICD_DIAG("vkGetPhysicalDeviceProperties -> limits ABSENT (host sent none)");
        }
    } else {
        /* No host / unknown device: report a benign placeholder so callers don't NPE. Version
         * 1.3 to stay consistent with vkEnumerateInstanceVersion (LEAD 2). */
        pProperties->apiVersion = VK_API_VERSION_1_3;
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

/* FULL DEVICE PASSTHROUGH: marshal the CLIENT's REAL VkDeviceCreateInfo (queue list,
 * enabled device extensions, allowlisted pNext feature chain) to the host so it creates a
 * real Mali VkDevice whose queues/features MATCH what the client (ANGLE's RendererVk) then
 * uses. The coarse path invented its own single-queue device, which made the Android Vulkan
 * loader's vkGetDeviceQueue null-deref on the (family,index) ANGLE actually requested
 * (DEVICE-PROVEN tombstone). The request buffer is sized to the create info: header + the
 * queue/ext lists + the feature blobs (a VkPhysicalDeviceFeatures2 is ~240 bytes), so it is
 * heap-allocated rather than a fixed stack buffer. */
static VkResult VKAPI_CALL alr_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                              const VkDeviceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator,
                                              VkDevice *pDevice) {
    (void)pAllocator;
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdDevice *dev;
    uint32_t vdev, gfx_family = 0;
    int32_t dev_result = 0;
    uint32_t qci_count = (pCreateInfo ? pCreateInfo->queueCreateInfoCount : 0u);
    uint32_t ext_count = (pCreateInfo ? pCreateInfo->enabledExtensionCount : 0u);
    ALR_ICD_DIAG("vkCreateDevice enter (qci=%u ext=%u pNext=%p ring_ok=%d)",
                 qci_count, ext_count, pCreateInfo ? pCreateInfo->pNext : NULL,
                 alr_icd_ring_ok());
    if (!pd || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    if (!alr_icd_ring_ok()) { ALR_ICD_DIAG("vkCreateDevice FAIL no-ring"); return VK_ERROR_INITIALIZATION_FAILED; }  /* no GPU to create on */

    dev = (AlrIcdDevice *)calloc(1, sizeof(AlrIcdDevice));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    alr_set_loader_magic_value(dev);
    vdev = alr_alloc(&g_next_vdev, 1);
    dev->vdev = vdev;
    dev->phys = pd;

    /* Size the request buffer: fixed CREATE_DEVICE2 header (op + vinst/vphys/vdev = 13) +
     * qci list (4 + qci_count*8) + ext list (4 + sum(4+namelen)) + feature list
     * (4 + sum(4 sType + 4 len + struct bytes)) + a terminator. Walk the inputs to bound it.*/
    uint32_t need = 13 + 4 + qci_count * 8u + 4u + 4u + 8u;
    for (uint32_t i = 0; i < ext_count; ++i) {
        const char *nm = pCreateInfo->ppEnabledExtensionNames
                             ? pCreateInfo->ppEnabledExtensionNames[i] : NULL;
        need += 4u + (uint32_t)(nm ? strlen(nm) : 0u);
    }
    /* Count + size the allowlisted pNext feature structs (so the buffer fits them). */
    uint32_t feat_count = 0;
    for (const VkBaseInStructure *p = pCreateInfo ? (const VkBaseInStructure *)pCreateInfo->pNext
                                                  : NULL;
         p; p = p->pNext) {
        uint32_t sz = alr_icd_feature_struct_size((uint32_t)p->sType);
        if (sz) { feat_count++; need += 4u + 4u + sz; }
    }

    /* WAVE-7 DEVICE-CREATE DIAG: dump the EXACT VkDeviceCreateInfo ANGLE passes — every
     * enabled device extension name, and every pNext sType (flagged FWD if our allowlist
     * marshals it to the host, or DROP if it is filtered out here before the wire). This is
     * the ground truth for the host_result=-3 wall: it shows precisely which extensions /
     * feature structs ANGLE's REAL render device requires, so a -3 can be attributed to a
     * specific extension Mali lacks or a feature struct that never reached the host. Gated on
     * ALR_ICD_DIAG (zero cost otherwise). */
    if (alr_icd_diag_on()) {
        for (uint32_t i = 0; i < ext_count; ++i) {
            const char *nm = pCreateInfo->ppEnabledExtensionNames
                                 ? pCreateInfo->ppEnabledExtensionNames[i] : NULL;
            ALR_ICD_DIAG("vkCreateDevice ext[%u]=%s", i, nm ? nm : "(null)");
        }
        for (const VkBaseInStructure *p = pCreateInfo
                 ? (const VkBaseInStructure *)pCreateInfo->pNext : NULL;
             p; p = p->pNext) {
            uint32_t sz = alr_icd_feature_struct_size((uint32_t)p->sType);
            ALR_ICD_DIAG("vkCreateDevice pNext sType=%u %s", (uint32_t)p->sType,
                         sz ? "FWD" : "DROP(not in feature allowlist)");
        }
    }

    uint8_t *req = (uint8_t *)malloc(need);
    if (!req) { free(dev); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    {
        uint8_t reply[ALR_ICD_REPLY_SCRATCH]; uint32_t rlen;
        AlrVkEncoder e;
        alr_vk_enc_init(&e, req, need);
        alr_vk_enc_create_device2_begin(&e, pd->inst->vinst, pd->vphys, vdev);
        /* queue-create list (family, count) — verbatim from the client. */
        alr_vk_enc_create_device2_qci_count(&e, qci_count);
        for (uint32_t i = 0; i < qci_count; ++i) {
            const VkDeviceQueueCreateInfo *q = &pCreateInfo->pQueueCreateInfos[i];
            alr_vk_enc_create_device2_qci(&e, q->queueFamilyIndex, q->queueCount);
        }
        /* enabled device extensions — verbatim from the client. */
        alr_vk_enc_create_device2_ext_count(&e, ext_count);
        for (uint32_t i = 0; i < ext_count; ++i) {
            const char *nm = pCreateInfo->ppEnabledExtensionNames
                                 ? pCreateInfo->ppEnabledExtensionNames[i] : "";
            alr_vk_enc_create_device2_ext(&e, nm ? nm : "");
        }
        /* allowlisted pNext feature structs — whole struct (incl. its own sType/pNext). */
        alr_vk_enc_create_device2_feat_count(&e, feat_count);
        for (const VkBaseInStructure *p =
                 (const VkBaseInStructure *)pCreateInfo->pNext;
             p; p = p->pNext) {
            uint32_t sz = alr_icd_feature_struct_size((uint32_t)p->sType);
            if (sz) alr_vk_enc_create_device2_feat(&e, (uint32_t)p->sType, p, sz);
        }
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen) alr_icd_parse_reply(reply, rlen, NULL, NULL, NULL, &gfx_family, &dev_result);
            else dev_result = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        } else {
            dev_result = (int32_t)VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    free(req);
    if (dev_result != 0) { free(dev); ALR_ICD_DIAG("vkCreateDevice FAIL host_result=%d", dev_result); return (VkResult)dev_result; }
    dev->gfx_family = gfx_family;
    *pDevice = (VkDevice)dev;
    ALR_ICD_DIAG("vkCreateDevice OK vdev=%u gfx_family=%u feat=%u", vdev, gfx_family, feat_count);
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
    ALR_ICD_DIAG("vkGetDeviceQueue (family=%u index=%u)", queueFamilyIndex, queueIndex);
    q = (AlrIcdQueue *)calloc(1, sizeof(AlrIcdQueue));
    if (!q) { *pQueue = VK_NULL_HANDLE; return; }
    alr_set_loader_magic_value(q);
    vqueue = alr_alloc(&g_next_vqueue, 1);
    q->vqueue = vqueue;
    q->dev = dev;
    if (alr_icd_ring_ok()) {
        /* FULL DEVICE PASSTHROUGH: forward the client's ACTUAL (queueFamilyIndex,queueIndex)
         * so the host binds the real queue the device was created with — never an
         * un-created (family,index) that makes the driver's GetDeviceQueue null-deref. */
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH];
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_get_device_queue2(&e, dev->vdev, queueFamilyIndex, queueIndex, vqueue);
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
        alr_icd_cmdlog_init(&cb->log);   /* fresh record log (vkBeginCommandBuffer resets it) */
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
    /* VERSION-SKEW FIX (LEAD 2): report a 1.3 INSTANCE so the instance version and the
     * physical-device apiVersion AGREE. The physical device forwards REAL Mali (1.3.247),
     * so an instance pinned to 1.1 made ANGLE run a 1.1-instance / 1.3-device HYBRID:
     * ANGLE computes its effective device version as min(instanceVersion, deviceApiVersion),
     * and a 1.1 instance silently caps a 1.3 Mali to 1.1 — a mismatch against the 1.3 struct
     * sizes/promoted-entrypoint expectations elsewhere in RendererVk. Our query structs are
     * 1.3-ABI-exact (compile-time-locked) and the promoted Get*2 family is wired, so 1.3 is
     * the honest, consistent answer. (The loader still clamps this to its own max; the
     * Khronos loader on the guest path is >= 1.3.) */
    if (pApiVersion) *pApiVersion = VK_API_VERSION_1_3;
    ALR_ICD_DIAG("vkEnumerateInstanceVersion -> 1.3 (consistent with Mali device apiVersion)");
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
    /* The device-level extensions ANGLE enables for a basic render+present path, PLUS the
     * core-1.1 bind2 / dedicated-allocation family ANGLE's VMA allocator binds resources
     * through. DEVICE-CHECKED (wave-15): the first-glTexImage2D NULL-deref is invariant to
     * this list too (advertising the full SwiftShader-like KHR set did NOT clear it, and ANGLE
     * never CALLS a newly-advertised-but-unimplemented fn), so we keep the list MINIMAL —
     * exactly what our device entrypoints actually back — to avoid advertising capability we
     * can't service. (Adding more is safe re: the crash but pointless, and risks routing a
     * future ANGLE path to an unimplemented fn; revisit only when a path is proven to need
     * a specific extension.) */
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
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdPhysCache *slot;
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));
    /* ANGLE caps-init rung: forward the REAL Mali VkPhysicalDeviceFeatures. ensure_phys_props
     * round-trips GET_PHYSICAL_DEVICE_PROPERTIES, whose extended reply carries the features'
     * raw official-ABI bytes; copy them into the caller's struct (55 VkBool32, exact size).
     * Reporting Mali's real optional-feature set (vs all-zero) lets ANGLE enable the code
     * paths it needs and pass ensureCapsInitialized instead of treating us as core-1.0-only. */
    if (pd && (slot = ensure_phys_props(pd)) != NULL && slot->have_features) {
        memcpy(pFeatures, slot->features_raw, ALR_ICD_FEATURES_BYTES);
        ALR_ICD_DIAG("vkGetPhysicalDeviceFeatures -> REAL Mali features forwarded (%u B)",
                     (unsigned)ALR_ICD_FEATURES_BYTES);
    } else {
        /* No host / unknown device: keep the conservative all-zero answer (core 1.0 only). */
        ALR_ICD_DIAG("vkGetPhysicalDeviceFeatures -> all-zero (no host features)");
    }
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemProps) {
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    AlrIcdPhysCache *slot;
    if (!pMemProps) return;
    /* ANGLE memory-type-selection rung: forward the REAL Mali VkPhysicalDeviceMemoryProperties.
     * ANGLE created the REAL Mali device (real heaps/types); it then selects a memory type by
     * INDEX into the list returned here. The previous SYNTHETIC 1-heap/2-type model made ANGLE
     * pick a type index that doesn't exist on real Mali => fault right after vkGetDeviceQueue
     * (libGLESv2.so.2+0x1f6db4, before any resource create). ensure_phys_props marshals the
     * extended PHYS_PROPS reply (host queried real Mali vkGetPhysicalDeviceMemoryProperties). */
    slot = pd ? ensure_phys_props(pd) : NULL;
    if (slot && slot->have_memprops) {
        memcpy(pMemProps, slot->memprops_raw, ALR_ICD_MEMORY_BYTES);
        ALR_ICD_DIAG("vkGetPhysicalDeviceMemoryProperties -> REAL Mali forwarded "
                     "(%u heaps, %u types)",
                     pMemProps->memoryHeapCount, pMemProps->memoryTypeCount);
        /* Per-heap + per-type dump (gated on ALR_ICD_DIAG): the ground truth for an ANGLE
         * caps-init vector grow driven by a bad memory-type/heap value (a 0-size heap, an
         * out-of-range heapIndex, an unexpected DEVICE_LOCAL|HOST_VISIBLE combo). */
        if (alr_icd_diag_on()) {
            for (uint32_t h = 0; h < pMemProps->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
                ALR_ICD_DIAG("  memHeap[%u] size=%llu flags=0x%x", h,
                             (unsigned long long)pMemProps->memoryHeaps[h].size,
                             (unsigned)pMemProps->memoryHeaps[h].flags);
            for (uint32_t t = 0; t < pMemProps->memoryTypeCount && t < VK_MAX_MEMORY_TYPES; ++t)
                ALR_ICD_DIAG("  memType[%u] heapIndex=%u propertyFlags=0x%x", t,
                             pMemProps->memoryTypes[t].heapIndex,
                             (unsigned)pMemProps->memoryTypes[t].propertyFlags);
        }
        return;
    }
    /* No host / unknown device: keep the conservative single-heap shape a UMA mobile GPU
     * (Mali) minimally exposes (one DEVICE_LOCAL type + one HOST_VISIBLE|COHERENT type). */
    memset(pMemProps, 0, sizeof(*pMemProps));
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
    ALR_ICD_DIAG("vkGetPhysicalDeviceMemoryProperties -> 1 heap, 2 types (no host)");
}

/* Scan a reply stream for the ONE ALR_VK_REPLY_FORMAT_PROPS record, filling the three
 * VkFormatFeatureFlags out params. Returns 1 if found. (Mirrors the image-format scanner:
 * a single-op batch, so any other record is unexpected.) */
static int alr_icd_scan_format_props_reply(const uint8_t *data, uint32_t len,
                                           uint32_t *out_lin, uint32_t *out_opt,
                                           uint32_t *out_buf) {
    AlrRd r; uint8_t op;
    r.p = data; r.n = len; r.pos = 0;
    for (;;) {
        if (!rd_u8(&r, &op)) break;
        if (op == ALR_VK_REPLY_END) break;
        if (op == ALR_VK_REPLY_FORMAT_PROPS) {
            uint32_t vphys, lin, opt, buf;
            if (!rd_u32(&r, &vphys) || !rd_u32(&r, &lin) || !rd_u32(&r, &opt) ||
                !rd_u32(&r, &buf)) return 0;
            if (out_lin) *out_lin = lin;
            if (out_opt) *out_opt = opt;
            if (out_buf) *out_buf = buf;
            return 1;
        }
        return 0;  /* unexpected record for this single-op batch */
    }
    return 0;
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties) {
    AlrIcdPhysicalDevice *pd = (AlrIcdPhysicalDevice *)physicalDevice;
    if (!pFormatProperties) return;
    /* ANGLE format-path-selection rung: forward the REAL Mali per-format feature flags.
     * ANGLE queries these to choose render/blit/storage/sample paths for a VkFormat; the old
     * synthetic 0x7FFFFFFF (all bits) lied about Mali (claimed storage/atomic/blit on formats
     * Mali lacks) and could steer ANGLE down an unsupported path. Like ImageFormatProperties
     * this is parameterized, so it round-trips per query (op GET_PHYS_FORMAT_PROPS -> host
     * servicer -> real Mali vkGetPhysicalDeviceFormatProperties -> reply). */
    if (pd && alr_icd_ring_ok()) {
        uint8_t req[32]; AlrVkEncoder e; uint8_t reply[ALR_ICD_REPLY_SCRATCH]; uint32_t rlen;
        uint32_t lin = 0, opt = 0, buf = 0;
        alr_vk_enc_init(&e, req, sizeof(req));
        alr_vk_enc_get_phys_format_props(&e, pd->inst->vinst, pd->vphys, (uint32_t)format);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        if (!e.overflow) {
            rlen = alr_icd_roundtrip(req, (uint32_t)e.len, reply, sizeof(reply));
            if (rlen && alr_icd_scan_format_props_reply(reply, rlen, &lin, &opt, &buf)) {
                pFormatProperties->linearTilingFeatures = lin;
                pFormatProperties->optimalTilingFeatures = opt;
                pFormatProperties->bufferFeatures = buf;
                ALR_ICD_DIAG("vkGetPhysicalDeviceFormatProperties fmt=%d -> REAL Mali "
                             "(lin=0x%x opt=0x%x buf=0x%x)", (int)format, lin, opt, buf);
                return;
            }
        }
    }
    /* Ring-less / unknown device: fall back to the conservative-permissive all-bits answer so
     * a no-host guest still progresses (the same stance the ring-less image-format path takes). */
    pFormatProperties->linearTilingFeatures = 0x7FFFFFFF;
    pFormatProperties->optimalTilingFeatures = 0x7FFFFFFF;
    pFormatProperties->bufferFeatures = 0x7FFFFFFF;
    ALR_ICD_DIAG("vkGetPhysicalDeviceFormatProperties fmt=%d -> all-bits (no host)",
                 (int)format);
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
/* Mutable pNext walk (the const VkBaseInStructure is read-only; Properties2 OUT structs
 * are written, so we need a writable view). Layout matches VkBaseOutStructure. */
typedef struct AlrVkBaseOut { int32_t sType; struct AlrVkBaseOut *pNext; } AlrVkBaseOut;

static void VKAPI_CALL alr_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    /* Do NOT memset the whole struct: the caller chained pNext structs (ANGLE attaches
     * VkPhysicalDeviceIDProperties + DriverProperties) that we must not stomp. Only fill
     * the embedded v1 .properties (the real-Mali data, via the v1 entry point). */
    alr_vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
    ALR_ICD_DIAG("vkGetPhysicalDeviceProperties2 (pNext=%p) name=%s",
                 (void *)pProperties->pNext, pProperties->properties.deviceName);
    /* Fill the extended-property pNext structs ANGLE's RendererVk reads at init.
     * SUBGROUP_PROPERTIES.subgroupSize MUST be non-zero (the prior v1-only fill left it 0,
     * which is invalid — ANGLE strides/divides by it for compute/subgroup layout; SwiftShader
     * never reports 0). DRIVER_PROPERTIES.driverID lets ANGLE's RendererVk::initFeatures pick
     * its real-Mali (ARM) path instead of a generic one. Values are the real Mali-G615 ones.
     * NOTE: device-tested — these alone do NOT clear the first-glTexImage2D NULL-deref (that
     * fault is invariant to every caps VALUE the ICD reports), but a 0 subgroupSize is an
     * independent correctness bug worth fixing for compute/subgroup-using guests. The v1-only
     * fill (prior behavior) remains the FALLBACK for any sType we don't synthesize. */
    for (AlrVkBaseOut *p = (AlrVkBaseOut *)pProperties->pNext; p; p = p->pNext) {
        switch ((uint32_t)p->sType) {
            case 1000094000u: {  /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES */
                /* { sType,pNext, u32 subgroupSize, u32 supportedStages, u32 supportedOps,
                 *   VkBool32 quadOperationsInAllStages } — fill from offset 16. */
                uint32_t *f = (uint32_t *)((uint8_t *)p + 16);
                f[0] = 16u;          /* subgroupSize (Mali-G615) — NON-ZERO is the fix */
                f[1] = 0x0000007Fu;  /* supportedStages = ALL graphics+compute */
                f[2] = 0x000000FFu;  /* supportedOperations = basic..quad (all common) */
                f[3] = 1u;           /* quadOperationsInAllStages */
                break;
            }
            case 1000196000u: {  /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES */
                /* { sType,pNext, VkDriverId driverID, char driverName[256],
                 *   char driverInfo[256], VkConformanceVersion } — driverID at offset 16. */
                uint8_t *base = (uint8_t *)p + 16;
                *(uint32_t *)base = 6u;  /* VK_DRIVER_ID_ARM_PROPRIETARY */
                /* driverName / driverInfo: leave caller-zeroed (ANGLE reads driverID, not the
                 * strings, for its workaround switch); conformanceVersion left zeroed. */
                break;
            }
            default: break;  /* other chained structs keep the caller's (zeroed) init */
        }
    }
    if (alr_icd_diag_on())
        for (const VkBaseInStructure *p = (const VkBaseInStructure *)pProperties->pNext;
             p; p = p->pNext)
            ALR_ICD_DIAG("  props2 pNext sType=%u (subgroup/driver synthesized; rest v1-only)",
                         (uint32_t)p->sType);
}

static void VKAPI_CALL alr_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    alr_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
    ALR_ICD_DIAG("vkGetPhysicalDeviceFeatures2 (pNext=%p)", (void *)pFeatures->pNext);
    /* Like Properties2: the feature pNext chain (Multiview/16BitStorage/VariablePointers/
     * ProtectedMemory/SamplerYcbcr) is left as the caller initialized it. DEVICE-CHECKED:
     * reporting these features as supported did NOT move the first-texture crash. Dump-only
     * under ALR_ICD_DIAG; the orthodox fix (forward the REAL Mali Features2 chain) is
     * deferred until a path is proven to require a specific one. */
    if (alr_icd_diag_on())
        for (const VkBaseInStructure *p = (const VkBaseInStructure *)pFeatures->pNext;
             p; p = p->pNext)
            ALR_ICD_DIAG("  feat2 pNext sType=%u (v1-only; not synthesized)", (uint32_t)p->sType);
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
    /* LEAD-3 FIX: fill any chained VkFormatProperties3 from the SAME real-Mali v1 flags
     * (widened to the 64-bit VkFormatFeatureFlags2 — the legacy bits are value-compatible).
     * ANGLE's RendererVk::getFormatFeatureBits() READS FormatProperties3.optimalTilingFeatures
     * (NOT the v1 member) when the device supports VK_KHR_format_feature_flags2 (Mali does).
     * Leaving it zero (the old behavior) made ANGLE see RGBA8 as featureless -> a degenerate
     * vk::Format whose helper member is NULL -> the first-glTexImage2D deref at
     * libGLESv2+0x206db4. The v1 flags came from real Mali, so widening them is truthful. */
    const VkFormatProperties *v1 = &pFormatProperties->formatProperties;
    for (AlrVkBaseOut *p = (AlrVkBaseOut *)pFormatProperties->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_VAL) {
            VkFormatProperties3Min *fp3 = (VkFormatProperties3Min *)p;
            fp3->linearTilingFeatures  = (VkFormatFeatureFlags2Min)v1->linearTilingFeatures;
            fp3->optimalTilingFeatures = (VkFormatFeatureFlags2Min)v1->optimalTilingFeatures;
            fp3->bufferFeatures        = (VkFormatFeatureFlags2Min)v1->bufferFeatures;
            ALR_ICD_DIAG("vkGetPhysicalDeviceFormatProperties2 fmt=%u FILLED FormatProperties3 "
                         "lin=0x%llx opt=0x%llx buf=0x%llx (was zero -> RGBA8 degenerate-format fix)",
                         (unsigned)format, (unsigned long long)fp3->linearTilingFeatures,
                         (unsigned long long)fp3->optimalTilingFeatures,
                         (unsigned long long)fp3->bufferFeatures);
        } else if (alr_icd_diag_on()) {
            ALR_ICD_DIAG("vkGetPhysicalDeviceFormatProperties2 fmt=%u pNext sType=%d (left as caller set)",
                         (unsigned)format, (int)p->sType);
        }
    }
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
 * GENERATED passthrough entrypoints (the 300.. escape band). Defining ALR_ICD_GEN_DEFINE
 * emits (a) the reply scanners (alr_gpu_vk_gen_icd_runtime.inc) and (b) the per-entrypoint
 * VKAPI_CALL functions (alr_gpu_vk_gen_icd.inc). They reuse this TU's AlrIcdDevice, the
 * alr_alloc id allocator, the g_next_v counters, alr_icd_ring_ok, alr_icd_roundtrip, the
 * glue's AlrVkReader + arena helpers, and the generated encoders. Must precede alr_lookup
 * (its table references these functions). The table rows are spliced below via
 * ALR_ICD_GEN_TABLE.
 * ============================================================================ */
#define ALR_ICD_GEN_DEFINE 1
#include "alr_gpu/generated/alr_gpu_vk_gen_icd_runtime.inc"
#include "alr_gpu/generated/alr_gpu_vk_gen_icd.inc"
#undef ALR_ICD_GEN_DEFINE

/* ============================================================================
 * COMMAND-BUFFER RECORDING entrypoints (the wave-8 wiring): vkBeginCommandBuffer / vkCmd* /
 * vkQueueSubmit / vkWaitForFences / ... Each RECORDS into its command buffer's cb->log (or
 * ships a submit/sync ring op). Defined here (ALR_ICD_CMD_DEFINE) BEFORE alr_lookup so the
 * table can reference them; the ALR_ENTRY rows are spliced into alr_lookup below
 * (ALR_ICD_CMD_TABLE). This is what makes vkGetDeviceProcAddr return NON-NULL for the whole
 * render/submit family ANGLE caches at device-init (without it ANGLE SIGABRTs on a NULL fn).
 * ============================================================================ */
#define ALR_ICD_CMD_DEFINE 1
#include "alr_icd_cmd_entrypoints.inc"
#undef ALR_ICD_CMD_DEFINE

/* ============================================================================
 * The core-1.1 "2" memory-requirements family. ANGLE's RendererVk (a Vulkan-1.1+ device)
 * resolves vkGetImageMemoryRequirements2 / vkGetBufferMemoryRequirements2 — NOT the v1
 * entrypoints — when it allocates the backing image/buffer for a texture/FBO. Returning NULL
 * for these made ANGLE call through a NULL fn pointer in its image-allocation path and FAULT
 * at libGLESv2.so.2+0x1f6db4 (DEVICE-PROVEN: the fault that used to hit right after
 * vkGetDeviceQueue now moves HERE, on the first FBO/texture allocation, once the real Mali
 * memory-properties fix let device-init's memory-type selection succeed).
 *
 * The "2" structs are { sType, pNext, <v1 value> }; the v1 value is exactly what the already-
 * forwarding generated v1 entrypoints (alr_vkGetImageMemoryRequirements /
 * alr_vkGetBufferMemoryRequirements, which round-trip the REAL Mali memoryTypeBits/size/align)
 * produce. So these wrappers fill the embedded v1 member from the v1 entrypoint and leave any
 * chained pNext (e.g. VkMemoryDedicatedRequirements) at its caller-initialized value — ANGLE
 * zero-inits that chain and tolerates an untouched dedicated-requirements struct. This forwards
 * Mali's true memoryTypeBits so ANGLE's memory-type selection indexes a type that EXISTS.
 * ============================================================================ */
static void VKAPI_CALL alr_vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    alr_vkGetImageMemoryRequirements(device, pInfo->image,
                                     &pMemoryRequirements->memoryRequirements);
    ALR_ICD_DIAG("vkGetImageMemoryRequirements2 -> REAL Mali forwarded "
                 "(size=%llu align=%llu typeBits=0x%x)",
                 (unsigned long long)pMemoryRequirements->memoryRequirements.size,
                 (unsigned long long)pMemoryRequirements->memoryRequirements.alignment,
                 pMemoryRequirements->memoryRequirements.memoryTypeBits);
}

static void VKAPI_CALL alr_vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    alr_vkGetBufferMemoryRequirements(device, pInfo->buffer,
                                      &pMemoryRequirements->memoryRequirements);
    ALR_ICD_DIAG("vkGetBufferMemoryRequirements2 -> REAL Mali forwarded "
                 "(size=%llu align=%llu typeBits=0x%x)",
                 (unsigned long long)pMemoryRequirements->memoryRequirements.size,
                 (unsigned long long)pMemoryRequirements->memoryRequirements.alignment,
                 pMemoryRequirements->memoryRequirements.memoryTypeBits);
}

/* ---- Core-1.1 bind2 (vkBindImageMemory2 / vkBindBufferMemory2) -------------------------
 * ROOT-CAUSE FIX (ANGLE first-glTexImage2D crash, libGLESv2+0x1f6db4): ANGLE's Vulkan
 * memory allocator (vk::Allocator wrapping AMD VMA) REQUIRES the bind2 entrypoints — it
 * binds every image/buffer via vkBindImageMemory2/vkBindBufferMemory2 (the dedicated-
 * allocation path), NOT the v1 vkBindImageMemory. We had only the v1 binds, so volk left
 * VMA's bind2 fn-pointers NULL; ANGLE's allocator then left a sub-object NULL and
 * dereferenced it (`ldr x12,[x8,#0x390]`, x8==NULL) on the FIRST texture's memory bind.
 * Device-proven discriminator: SwiftShader (which implements bind2) does NOT hit this
 * crash. Each VkBind*MemoryInfo just wraps (handle, memory, offset) — so we forward each
 * element to the proven v1 bind. pNext (e.g. VkBindImageMemoryDeviceGroupInfo) is ignored:
 * our device is single-GPU, so the default (whole-resource) bind is correct. The structs
 * are the official Vulkan ABI; defined locally as vk_min.h does not carry them. */
typedef struct AlrVkBindImageMemoryInfo {
    int32_t sType; const void *pNext;
    VkImage image; VkDeviceMemory memory; VkDeviceSize memoryOffset;
} AlrVkBindImageMemoryInfo;
typedef struct AlrVkBindBufferMemoryInfo {
    int32_t sType; const void *pNext;
    VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize memoryOffset;
} AlrVkBindBufferMemoryInfo;

static VkResult VKAPI_CALL alr_vkBindImageMemory2(
    VkDevice device, uint32_t bindInfoCount, const AlrVkBindImageMemoryInfo *pBindInfos) {
    VkResult rc = VK_SUCCESS;
    uint32_t i;
    if (!pBindInfos) return VK_ERROR_INITIALIZATION_FAILED;
    ALR_ICD_DIAG("vkBindImageMemory2 (count=%u) -> v1 bind passthrough", bindInfoCount);
    for (i = 0; i < bindInfoCount; ++i) {
        VkResult r = alr_vkBindImageMemory(device, pBindInfos[i].image,
                                           pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) rc = r;  /* report the first failure, still bind the rest */
    }
    return rc;
}

static VkResult VKAPI_CALL alr_vkBindBufferMemory2(
    VkDevice device, uint32_t bindInfoCount, const AlrVkBindBufferMemoryInfo *pBindInfos) {
    VkResult rc = VK_SUCCESS;
    uint32_t i;
    if (!pBindInfos) return VK_ERROR_INITIALIZATION_FAILED;
    ALR_ICD_DIAG("vkBindBufferMemory2 (count=%u) -> v1 bind passthrough", bindInfoCount);
    for (i = 0; i < bindInfoCount; ++i) {
        VkResult r = alr_vkBindBufferMemory(device, pBindInfos[i].buffer,
                                            pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) rc = r;
    }
    return rc;
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
        /* The core-1.1 "2" memory-requirements family ANGLE resolves to allocate texture/FBO
         * backing memory. Their ABSENCE (NULL) made ANGLE fault at libGLESv2.so.2+0x1f6db4 on
         * the first FBO allocation; these forward the REAL Mali memoryTypeBits via the v1 path
         * so ANGLE's memory-type selection indexes a type that exists. */
        ALR_ENTRY("vkGetImageMemoryRequirements2", alr_vkGetImageMemoryRequirements2),
        ALR_ENTRY("vkGetBufferMemoryRequirements2", alr_vkGetBufferMemoryRequirements2),
        ALR_ENTRY("vkGetImageMemoryRequirements2KHR", alr_vkGetImageMemoryRequirements2),
        ALR_ENTRY("vkGetBufferMemoryRequirements2KHR", alr_vkGetBufferMemoryRequirements2),
        /* Core-1.1 bind2 — ANGLE's VMA allocator binds every resource through these (the
         * first-glTexImage2D NULL-deref fix; see alr_vkBindImageMemory2). Both the core and
         * KHR-suffixed names (VMA resolves whichever the enabled version exposes). */
        ALR_ENTRY("vkBindImageMemory2", alr_vkBindImageMemory2),
        ALR_ENTRY("vkBindBufferMemory2", alr_vkBindBufferMemory2),
        ALR_ENTRY("vkBindImageMemory2KHR", alr_vkBindImageMemory2),
        ALR_ENTRY("vkBindBufferMemory2KHR", alr_vkBindBufferMemory2),
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
        /* ---- GENERATED render-batch entrypoints (the 300.. escape band): device memory
         * (same-process arena), buffers, images, image views + their reqs/bind/destroy.
         * The ALR_ENTRY rows are emitted by the codegen (tools/gen_vk_passthrough.py). ---- */
#define ALR_ICD_GEN_TABLE 1
#include "alr_gpu/generated/alr_gpu_vk_gen_icd.inc"
#undef ALR_ICD_GEN_TABLE
        /* ---- COMMAND-BUFFER RECORDING + submit/sync entrypoints (the wave-8 wiring). These
         * make vkGetDeviceProcAddr return NON-NULL for the cmd-record + queue-submit + fence
         * family ANGLE caches at device-init. Functions defined above (ALR_ICD_CMD_DEFINE). */
#define ALR_ICD_CMD_TABLE 1
#include "alr_icd_cmd_entrypoints.inc"
#undef ALR_ICD_CMD_TABLE
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

/* ---- DIAGNOSTIC TRAP POOL (gated on ALR_ICD_TRAP=1) -----------------------------------
 * ANGLE's volk resolves the ENTIRE device dispatch table up front (one vkGetDeviceProcAddr
 * per known function), so the resolution log can't tell us which UNIMPLEMENTED function it
 * actually CALLS first (it crashes on the NULL pointer with no further trace). To find the
 * real wall, under ALR_ICD_TRAP we hand back — instead of NULL — a per-name trampoline that
 * LOGS "[alr-icd] TRAP CALLED <name>" the first time it is invoked and returns 0
 * (== VK_SUCCESS for a VkResult fn; a NULL handle / no-op for the rest). That turns the
 * silent crash into an ordered trace of the device functions ANGLE genuinely uses, so the
 * next entrypoints to implement are read off directly. OFF by default (returns NULL — the
 * correct GDPA answer), so it never perturbs the no-regression path. ---- */
/* 640 slots: ANGLE's RendererVk queries ~500+ device entry points via vkGetDeviceProcAddr
 * at init; with a 128-slot pool the 129th+ unimplemented fn fell back to NULL, and a CALL to
 * that NULL fn crashed (blr x8=0) in libGLESv2 BEFORE the trap could name it. Sizing the pool
 * above the full device-fn count lets EVERY unimplemented fn ANGLE invokes log "TRAP CALLED"
 * instead, which pins the exact entrypoint the FBO/render path needs. */
#define ALR_TRAP_POOL 640
static const char *g_trap_names[ALR_TRAP_POOL];
static int         g_trap_count = 0;
static int         g_trap_fired[ALR_TRAP_POOL];
static pthread_mutex_t g_trap_lock = PTHREAD_MUTEX_INITIALIZER;

static int alr_icd_trap_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ALR_ICD_TRAP"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v;
}
/* The shared trampoline body: log this slot's name once, return 0. Variadic so it matches
 * any Vulkan calling convention on AArch64 (args are ignored; we only need the LOG). */
static uintptr_t alr_trap_body(int slot) {
    if (slot >= 0 && slot < ALR_TRAP_POOL) {
        /* Log the first few invocations per name (count in g_trap_fired) so a hot loop
         * doesn't flood the log but a repeated call is still visible. */
        if (g_trap_fired[slot] < 3) {
            g_trap_fired[slot]++;
            ALR_ICD_DIAG("TRAP CALLED %s (unimplemented device fn ANGLE invoked; call #%d)",
                         g_trap_names[slot] ? g_trap_names[slot] : "?", g_trap_fired[slot]);
        }
    }
    return 0;
}
/* 128 distinct entry points, each bound to its slot index, all funneling to alr_trap_body.
 * Generated with a macro so the pool is compact + maintainable. */
#define ALR_TRAP_FN(n) static uintptr_t alr_trap_##n(void) { return alr_trap_body(n); }
#define ALR_TRAP_FN8(b) ALR_TRAP_FN(b##0) ALR_TRAP_FN(b##1) ALR_TRAP_FN(b##2) ALR_TRAP_FN(b##3) \
                        ALR_TRAP_FN(b##4) ALR_TRAP_FN(b##5) ALR_TRAP_FN(b##6) ALR_TRAP_FN(b##7)
/* slots 0..639 (80 rows of 8) */
ALR_TRAP_FN8(0) ALR_TRAP_FN8(1) ALR_TRAP_FN8(2) ALR_TRAP_FN8(3) ALR_TRAP_FN8(4) ALR_TRAP_FN8(5)
ALR_TRAP_FN8(6) ALR_TRAP_FN8(7) ALR_TRAP_FN8(8) ALR_TRAP_FN8(9) ALR_TRAP_FN8(10) ALR_TRAP_FN8(11)
ALR_TRAP_FN8(12) ALR_TRAP_FN8(13) ALR_TRAP_FN8(14) ALR_TRAP_FN8(15)
ALR_TRAP_FN8(16) ALR_TRAP_FN8(17) ALR_TRAP_FN8(18) ALR_TRAP_FN8(19) ALR_TRAP_FN8(20) ALR_TRAP_FN8(21)
ALR_TRAP_FN8(22) ALR_TRAP_FN8(23) ALR_TRAP_FN8(24) ALR_TRAP_FN8(25) ALR_TRAP_FN8(26) ALR_TRAP_FN8(27)
ALR_TRAP_FN8(28) ALR_TRAP_FN8(29) ALR_TRAP_FN8(30) ALR_TRAP_FN8(31) ALR_TRAP_FN8(32) ALR_TRAP_FN8(33)
ALR_TRAP_FN8(34) ALR_TRAP_FN8(35) ALR_TRAP_FN8(36) ALR_TRAP_FN8(37) ALR_TRAP_FN8(38) ALR_TRAP_FN8(39)
ALR_TRAP_FN8(40) ALR_TRAP_FN8(41) ALR_TRAP_FN8(42) ALR_TRAP_FN8(43) ALR_TRAP_FN8(44) ALR_TRAP_FN8(45)
ALR_TRAP_FN8(46) ALR_TRAP_FN8(47) ALR_TRAP_FN8(48) ALR_TRAP_FN8(49) ALR_TRAP_FN8(50) ALR_TRAP_FN8(51)
ALR_TRAP_FN8(52) ALR_TRAP_FN8(53) ALR_TRAP_FN8(54) ALR_TRAP_FN8(55) ALR_TRAP_FN8(56) ALR_TRAP_FN8(57)
ALR_TRAP_FN8(58) ALR_TRAP_FN8(59) ALR_TRAP_FN8(60) ALR_TRAP_FN8(61) ALR_TRAP_FN8(62) ALR_TRAP_FN8(63)
ALR_TRAP_FN8(64) ALR_TRAP_FN8(65) ALR_TRAP_FN8(66) ALR_TRAP_FN8(67) ALR_TRAP_FN8(68) ALR_TRAP_FN8(69)
ALR_TRAP_FN8(70) ALR_TRAP_FN8(71) ALR_TRAP_FN8(72) ALR_TRAP_FN8(73) ALR_TRAP_FN8(74) ALR_TRAP_FN8(75)
ALR_TRAP_FN8(76) ALR_TRAP_FN8(77) ALR_TRAP_FN8(78) ALR_TRAP_FN8(79)
#undef ALR_TRAP_FN8
#undef ALR_TRAP_FN
#define ALR_TRAP_PTR(n) (PFN_vkVoidFunction)alr_trap_##n
static const PFN_vkVoidFunction g_trap_fns[ALR_TRAP_POOL] = {
#define ALR_TRAP_ROW8(b) ALR_TRAP_PTR(b##0), ALR_TRAP_PTR(b##1), ALR_TRAP_PTR(b##2), \
    ALR_TRAP_PTR(b##3), ALR_TRAP_PTR(b##4), ALR_TRAP_PTR(b##5), ALR_TRAP_PTR(b##6), ALR_TRAP_PTR(b##7),
    ALR_TRAP_ROW8(0) ALR_TRAP_ROW8(1) ALR_TRAP_ROW8(2) ALR_TRAP_ROW8(3) ALR_TRAP_ROW8(4)
    ALR_TRAP_ROW8(5) ALR_TRAP_ROW8(6) ALR_TRAP_ROW8(7) ALR_TRAP_ROW8(8) ALR_TRAP_ROW8(9)
    ALR_TRAP_ROW8(10) ALR_TRAP_ROW8(11) ALR_TRAP_ROW8(12) ALR_TRAP_ROW8(13) ALR_TRAP_ROW8(14)
    ALR_TRAP_ROW8(15) ALR_TRAP_ROW8(16) ALR_TRAP_ROW8(17) ALR_TRAP_ROW8(18) ALR_TRAP_ROW8(19)
    ALR_TRAP_ROW8(20) ALR_TRAP_ROW8(21) ALR_TRAP_ROW8(22) ALR_TRAP_ROW8(23) ALR_TRAP_ROW8(24)
    ALR_TRAP_ROW8(25) ALR_TRAP_ROW8(26) ALR_TRAP_ROW8(27) ALR_TRAP_ROW8(28) ALR_TRAP_ROW8(29)
    ALR_TRAP_ROW8(30) ALR_TRAP_ROW8(31) ALR_TRAP_ROW8(32) ALR_TRAP_ROW8(33) ALR_TRAP_ROW8(34)
    ALR_TRAP_ROW8(35) ALR_TRAP_ROW8(36) ALR_TRAP_ROW8(37) ALR_TRAP_ROW8(38) ALR_TRAP_ROW8(39)
    ALR_TRAP_ROW8(40) ALR_TRAP_ROW8(41) ALR_TRAP_ROW8(42) ALR_TRAP_ROW8(43) ALR_TRAP_ROW8(44)
    ALR_TRAP_ROW8(45) ALR_TRAP_ROW8(46) ALR_TRAP_ROW8(47) ALR_TRAP_ROW8(48) ALR_TRAP_ROW8(49)
    ALR_TRAP_ROW8(50) ALR_TRAP_ROW8(51) ALR_TRAP_ROW8(52) ALR_TRAP_ROW8(53) ALR_TRAP_ROW8(54)
    ALR_TRAP_ROW8(55) ALR_TRAP_ROW8(56) ALR_TRAP_ROW8(57) ALR_TRAP_ROW8(58) ALR_TRAP_ROW8(59)
    ALR_TRAP_ROW8(60) ALR_TRAP_ROW8(61) ALR_TRAP_ROW8(62) ALR_TRAP_ROW8(63) ALR_TRAP_ROW8(64)
    ALR_TRAP_ROW8(65) ALR_TRAP_ROW8(66) ALR_TRAP_ROW8(67) ALR_TRAP_ROW8(68) ALR_TRAP_ROW8(69)
    ALR_TRAP_ROW8(70) ALR_TRAP_ROW8(71) ALR_TRAP_ROW8(72) ALR_TRAP_ROW8(73) ALR_TRAP_ROW8(74)
    ALR_TRAP_ROW8(75) ALR_TRAP_ROW8(76) ALR_TRAP_ROW8(77) ALR_TRAP_ROW8(78) ALR_TRAP_ROW8(79)
#undef ALR_TRAP_ROW8
#undef ALR_TRAP_PTR
};
/* Resolve (or assign) a trap trampoline for an unimplemented `pName`. The name string comes
 * from the loader's static table (stable lifetime), so we store the pointer directly. */
static PFN_vkVoidFunction alr_icd_trap_for(const char *pName) {
    int i, slot = -1;
    if (!pName) return NULL;
    pthread_mutex_lock(&g_trap_lock);
    for (i = 0; i < g_trap_count; ++i)
        if (g_trap_names[i] && strcmp(g_trap_names[i], pName) == 0) { slot = i; break; }
    if (slot < 0 && g_trap_count < ALR_TRAP_POOL) {
        slot = g_trap_count++;
        g_trap_names[slot] = pName;  /* loader table string: stable */
    }
    pthread_mutex_unlock(&g_trap_lock);
    return (slot >= 0) ? g_trap_fns[slot] : NULL;
}

static PFN_vkVoidFunction VKAPI_CALL alr_vkGetDeviceProcAddr(VkDevice device,
                                                             const char *pName) {
    (void)device;
    PFN_vkVoidFunction fn = alr_lookup(pName);
    /* Log device-fn resolution so a device run sees the LAST entrypoint ANGLE's RendererVk
     * resolves before it stops — and crucially which ones we return NULL for (the next
     * passthrough entrypoints to implement; ANGLE may call a NULL device fn). */
    if (!fn) {
        ALR_ICD_DIAG("vkGetDeviceProcAddr(%s) -> NULL (unimplemented)", pName ? pName : "?");
        /* Under ALR_ICD_TRAP, hand back a logging trampoline instead of NULL so a CALL to
         * this unimplemented fn is traced (revealing ANGLE's real call order) rather than
         * crashing silently. Strictly diagnostic; default path is unchanged (NULL). */
        if (alr_icd_trap_on()) {
            PFN_vkVoidFunction trap = alr_icd_trap_for(pName);
            if (trap) return trap;
        }
    }
    return fn;
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
