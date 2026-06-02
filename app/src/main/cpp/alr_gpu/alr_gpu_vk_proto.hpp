// ALR GPU Vulkan command-stream wire contract — VK-M2 first step (enumerate/props).
//
// This is the Vulkan twin of alr_gpu/guest_shim/alr_gles_proto.h (the GLES op-stream
// contract). It defines the SAME little-endian wire encoding the GLES track uses
// (bare u8 opcode + back-to-back typed fields, u32/i32/f32 = 4B LE, blob = u32 len +
// bytes), so a Vulkan op stream a future guest libvulkan ICD encodes decodes on the
// committed host Vulkan decoder (alr_gpu/alr_gpu_vk_decode.hpp).
//
// SCOPE (deliberately narrow — strategy §7 "VK-M2", the enumerate/props ROUND-TRIP):
//   request ops (guest -> host):
//     VK_OP_CREATE_INSTANCE            create a vendor-libvulkan VkInstance (vhandle)
//     VK_OP_ENUMERATE_PHYSICAL_DEVICES enumerate Mali physical devices under vhandle
//     VK_OP_GET_PHYSICAL_DEVICE_PROPERTIES   props (name/api/type/vendor/queue families)
//   reply (host -> guest):  a length-prefixed reply blob the host writes back, carrying
//   the device count + each device's identity/props. Unlike the GLES draw ops (which are
//   fire-and-forget with client-side virtual ids and no round-trip), enumerate/props MUST
//   return data, so this contract pairs a request stream with a reply stream. That reply
//   is what the in-app ring (alr_gpu_ring.hpp) carries back at the sync point.
//
// CLIENT-SIDE VIRTUAL HANDLES: exactly the GLES principle. The guest allocates virtual
// VkInstance / VkPhysicalDevice handles monotonically (1,2,3,…) and never blocks on a
// host round-trip to *create* them; the host owns the virtual->real translation. The one
// place a value must come BACK is the enumerate count + the props payload — that is the
// reply, returned once per submit, not per call.
//
// Pure C99 + a tiny encoder/reader, glibc-guest-buildable (zig cc / gcc), no Vulkan
// headers needed (this is the byte protocol only — the host decoder owns <vulkan.h>).
// MUST STAY IN SYNC with alr_gpu/alr_gpu_vk_decode.hpp (the host is the source of truth).

#ifndef ALR_GPU_ALR_GPU_VK_PROTO_HPP
#define ALR_GPU_ALR_GPU_VK_PROTO_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Request opcodes (guest -> host). A distinct numeric space from the GLES
// enum AlrOp so a future single ring could carry both without ambiguity (GLES ops
// are 0..138; Vulkan ops start at 200). Do not renumber — these are the wire. ----
enum AlrVkOp {
    ALR_VK_OP_END = 0,  // shared terminator with the GLES contract (clean stop)

    // create a VkInstance on the host's vendor libvulkan, bound to virtual id `vinst`.
    // No round-trip: the guest picks `vinst` (monotonic). `app_api` is the requested
    // VkApplicationInfo::apiVersion (e.g. VK_API_VERSION_1_1 == 0x00401000).
    ALR_VK_OP_CREATE_INSTANCE = 200,  // u32 vinst, u32 app_api

    // enumerate physical devices under the instance `vinst`. The host assigns each
    // enumerated device a virtual id starting at `vphys_base` (monotonic, guest-chosen),
    // so the guest can address them in later props calls without a round-trip. The COUNT
    // comes back in the reply blob (the guest can't know it ahead of time).
    ALR_VK_OP_ENUMERATE_PHYSICAL_DEVICES = 201,  // u32 vinst, u32 vphys_base

    // fetch VkPhysicalDeviceProperties + queue-family summary for virtual device `vphys`
    // (must have been assigned by a prior enumerate under the same instance). The data
    // comes back in the reply blob, keyed by `vphys`.
    ALR_VK_OP_GET_PHYSICAL_DEVICE_PROPERTIES = 202,  // u32 vinst, u32 vphys

    // destroy the instance `vinst` (releases the host's real VkInstance + virtual maps).
    ALR_VK_OP_DESTROY_INSTANCE = 209  // u32 vinst
};

// ---- Reply record opcodes (host -> guest), carried in the reply blob. The reply is
// itself a little-endian op stream (same Reader), so the guest decodes it the same way
// the host decodes requests. Distinct space (220+) from request ops. ----
enum AlrVkReply {
    ALR_VK_REPLY_END = 0,

    // result of a CREATE_INSTANCE: the VkResult the host got (0 == VK_SUCCESS).
    ALR_VK_REPLY_INSTANCE = 220,  // u32 vinst, i32 vk_result

    // result of ENUMERATE_PHYSICAL_DEVICES: how many devices, and the [vphys_base..]
    // virtual ids the host assigned (contiguous, so just the base + count is enough).
    ALR_VK_REPLY_PHYS_COUNT = 221,  // u32 vinst, u32 vphys_base, u32 count, i32 vk_result

    // result of GET_PHYSICAL_DEVICE_PROPERTIES: the identity/props of one device.
    // Strings are blobs (NUL not required; the guest treats them as length-counted).
    ALR_VK_REPLY_PHYS_PROPS = 222
    //   u32 vphys
    //   u32 api_version          (VkPhysicalDeviceProperties::apiVersion, packed)
    //   u32 driver_version
    //   u32 vendor_id
    //   u32 device_id
    //   u32 device_type          (VkPhysicalDeviceType)
    //   blob device_name         (VkPhysicalDeviceProperties::deviceName)
    //   u32 queue_family_count
    //   then queue_family_count × { u32 queue_flags, u32 queue_count }
    //   u8  is_software          (1 if the host classified it as a CPU/software rasterizer)
};

// VkPhysicalDeviceType mirror (so the guest/self-test can name the type without
// including <vulkan.h>). Values are the Vulkan spec constants.
enum AlrVkPhysDeviceType {
    ALR_VK_PHYS_TYPE_OTHER = 0,
    ALR_VK_PHYS_TYPE_INTEGRATED_GPU = 1,
    ALR_VK_PHYS_TYPE_DISCRETE_GPU = 2,
    ALR_VK_PHYS_TYPE_VIRTUAL_GPU = 3,
    ALR_VK_PHYS_TYPE_CPU = 4
};

// ---------------------------------------------------------------------------
// AlrVkEncoder — the SAME little-endian byte builder shape as the GLES AlrEncoder
// (alr_gles_proto.h). Reusing the field layout keeps one wire ABI across both
// frontends. Writes into a caller-owned fixed buffer; sets overflow on a short write.
// ---------------------------------------------------------------------------
typedef struct AlrVkEncoder {
    uint8_t *buf;   /* caller-owned */
    size_t   cap;   /* capacity in bytes */
    size_t   len;   /* bytes written so far */
    int      overflow; /* set if a write didn't fit */
} AlrVkEncoder;

static inline void alr_vk_enc_init(AlrVkEncoder *e, uint8_t *buf, size_t cap) {
    e->buf = buf; e->cap = cap; e->len = 0; e->overflow = 0;
}
static inline void alr_vk_enc_raw(AlrVkEncoder *e, const void *p, size_t n) {
    if (e->overflow) return;
    if (e->len + n > e->cap) { e->overflow = 1; return; }
    if (n) memcpy(e->buf + e->len, p, n);
    e->len += n;
}
static inline void alr_vk_enc_u8(AlrVkEncoder *e, uint8_t v)  { alr_vk_enc_raw(e, &v, 1); }
static inline void alr_vk_enc_u32(AlrVkEncoder *e, uint32_t v){ alr_vk_enc_raw(e, &v, 4); }
static inline void alr_vk_enc_i32(AlrVkEncoder *e, int32_t v) { alr_vk_enc_raw(e, &v, 4); }
static inline void alr_vk_enc_blob(AlrVkEncoder *e, const void *p, uint32_t n) {
    alr_vk_enc_u32(e, n);
    if (n) alr_vk_enc_raw(e, p, n);
}
static inline void alr_vk_enc_str(AlrVkEncoder *e, const char *s) {
    alr_vk_enc_blob(e, s, (uint32_t)(s ? strlen(s) : 0));
}

/* Convenience request-stream builders (the guest ICD will emit these; the host
 * self-test uses them too). Each appends one op. */
static inline void alr_vk_enc_create_instance(AlrVkEncoder *e, uint32_t vinst,
                                              uint32_t app_api) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CREATE_INSTANCE);
    alr_vk_enc_u32(e, vinst);
    alr_vk_enc_u32(e, app_api);
}
static inline void alr_vk_enc_enumerate_phys(AlrVkEncoder *e, uint32_t vinst,
                                            uint32_t vphys_base) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_ENUMERATE_PHYSICAL_DEVICES);
    alr_vk_enc_u32(e, vinst);
    alr_vk_enc_u32(e, vphys_base);
}
static inline void alr_vk_enc_get_phys_props(AlrVkEncoder *e, uint32_t vinst,
                                            uint32_t vphys) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_GET_PHYSICAL_DEVICE_PROPERTIES);
    alr_vk_enc_u32(e, vinst);
    alr_vk_enc_u32(e, vphys);
}
static inline void alr_vk_enc_destroy_instance(AlrVkEncoder *e, uint32_t vinst) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_DESTROY_INSTANCE);
    alr_vk_enc_u32(e, vinst);
}

/* aarch64-linux-gnu (the guest target) is little-endian, so the memcpy-of-native
 * encoding is the wire. If this contract is ever cross-built big-endian, both the
 * encoder here AND the host Reader must byte-swap. */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ALR_GPU_ALR_GPU_VK_PROTO_HPP */
