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
    ALR_VK_OP_DESTROY_INSTANCE = 209,  // u32 vinst

    // ---- VK-M2 BODY: device + queue + command-buffer + clear-submit marshalling ----
    // These extend the enumerate/props round trip to the smallest GPU-driving path: a
    // logical device, a graphics queue, a command pool + buffer, a clear-only render,
    // and a submit + fence wait — all addressed by client-side virtual handles (no
    // per-call round-trip; the host owns the virtual->real maps). The ONLY data that
    // comes back is the create/submit VkResults + the clear readback (reply blob).

    // create a logical VkDevice on virtual physical device `vphys` (must have been
    // enumerated). The host picks the graphics queue family it found for `vphys` and
    // creates one queue in it; `vdev` (guest-chosen, monotonic) names the device.
    ALR_VK_OP_CREATE_DEVICE = 210,  // u32 vinst, u32 vphys, u32 vdev

    // bind virtual queue `vqueue` to queue index `queue_index` of `vdev`'s graphics
    // family. No round-trip: vkGetDeviceQueue can't fail; the guest names it locally.
    ALR_VK_OP_GET_DEVICE_QUEUE = 211,  // u32 vdev, u32 queue_index, u32 vqueue

    // create a command pool `vpool` on `vdev` (uses the device's graphics family).
    ALR_VK_OP_CREATE_COMMAND_POOL = 212,  // u32 vdev, u32 vpool

    // allocate one PRIMARY command buffer `vcmd` from pool `vpool` on `vdev`.
    ALR_VK_OP_ALLOCATE_COMMAND_BUFFERS = 213,  // u32 vdev, u32 vpool, u32 vcmd

    // record a CLEAR-only render into an offscreen R8G8B8A8 target of `width`×`height`
    // on `vdev`, into command buffer `vcmd`. The host owns the render target (an AHB-
    // backed color attachment on device, a host-memory image off-device); the guest
    // only ships the clear color. This is the minimal render path — the heavy
    // shader/pipeline/draw ops are a later VK-M3 step. RGBA clear is f32×4.
    ALR_VK_OP_CMD_BEGIN_CLEAR = 214,
    //   u32 vdev, u32 vcmd, u32 width, u32 height, f32 r, f32 g, f32 b, f32 a

    // submit `vcmd` on `vqueue` (of `vdev`) and wait for completion (fence/queue-idle
    // on the host). After this the host reads back the clear target's center pixel and
    // returns it in the reply, so the guest can verify the GPU actually cleared it.
    ALR_VK_OP_QUEUE_SUBMIT = 215,  // u32 vdev, u32 vqueue, u32 vcmd

    // destroy the logical device `vdev` (frees queue/pool/cmd/render-target maps).
    ALR_VK_OP_DESTROY_DEVICE = 216,  // u32 vdev

    // ---- VK-M3 (render BREADTH): a real DRAW, not a bare clear. ----
    // Record a render that (1) CLEARs the offscreen R8G8B8A8 target of `width`×`height`
    // to the background `r,g,b,a`, then (2) DRAWs one filled triangle covering the
    // target center with a graphics pipeline (vert+frag SPIR-V), a bound vertex buffer
    // (NDC positions), and a single vkCmdDraw of 3 vertices. The triangle's color comes
    // from the fragment shader itself (a fixed constant baked into the host-embedded
    // SPIR-V), so the center-pixel readback after submit proves a DRAW landed (a value
    // DISTINCT from the clear background). Like CMD_BEGIN_CLEAR this only RECORDS; the
    // matching QUEUE_SUBMIT replays + reads back.
    //
    // WHY the shader/SPIR-V is NOT on the wire: the guest libvulkan ICD would normally
    // ship the app's SPIR-V via VK_OP_CREATE_SHADER_MODULE blobs; for this first breadth
    // step the probe uses ONE handcrafted triangle pipeline whose SPIR-V the host owns
    // (alr_gpu_vk_decode.hpp embeds it). The wire still carries the draw INTENT (extent
    // + background) and the host runs the full create-shader-module / pipeline-layout /
    // render-pass / graphics-pipeline / vertex-buffer / bind / draw path on real Mali.
    // A later step promotes the SPIR-V + vertex data to wire blobs (CREATE_SHADER_MODULE
    // / CREATE_BUFFER ops) — the op number space below 230 is reserved for them.
    ALR_VK_OP_CMD_BEGIN_DRAW = 217,
    //   u32 vdev, u32 vcmd, u32 width, u32 height, f32 bg_r, f32 bg_g, f32 bg_b, f32 bg_a

    // ======================================================================
    // VK-M4 (PRESENT rung): GUEST-SUPPLIED SPIR-V + an AHB-backed swapchain that
    // routes the rendered image to the in-app Wayland compositor (a wl_surface on
    // the SurfaceView). This closes the loop from "Mali ran a draw" (VK-M3) to "a
    // REAL guest Vulkan app's OWN shaders rendered AND a window appeared on screen".
    // Numbers stay in the reserved <230 SPIR-V/swapchain band (proto §VK-M3 note).
    // ======================================================================

    // Upload the guest's OWN SPIR-V blob for a shader module, bound to virtual id
    // `vshader` on `vdev`. The host vkCreateShaderModule's it on real Mali (so the
    // guest app's shaders run on the GPU — not the host-embedded triangle SPIR-V).
    // No round-trip needed to *create* (client-side virtual id); the VkResult comes
    // back in ALR_VK_REPLY_SHADER so the guest can fail a bad blob. `stage` is the
    // VkShaderStageFlagBits the module is for (advisory; the pipeline stage binds it).
    ALR_VK_OP_CREATE_SHADER_MODULE = 218,
    //   u32 vdev, u32 vshader, u32 stage, blob spirv   (spirv = u32 byte-len + bytes)

    // Create an AHB-backed swapchain `vswapchain` on `vdev` of `width`×`height` with
    // `image_count` images. Each swapchain image IS an AHB COLOR_ATTACHMENT render
    // target the host owns (the proven round7 AHB image), so a present can route the
    // AHB to the compositor zero-copy. No real VkSurface/VK_KHR_swapchain is created
    // on Mali — the ICD's swapchain is a thin host-side rotation of AHB targets whose
    // present hands the finished AHB to the in-app compositor (the device has no
    // on-screen VkSurface; the Android SurfaceView is reached via the compositor).
    ALR_VK_OP_CREATE_SWAPCHAIN = 219,
    //   u32 vdev, u32 vswapchain, u32 width, u32 height, u32 image_count

    // Destroy the shader module `vshader` (releases the host's real VkShaderModule).
    ALR_VK_OP_DESTROY_SHADER_MODULE = 225,  // u32 vdev, u32 vshader

    // Acquire the next swapchain image index from `vswapchain`. The host rotates its
    // AHB ring and returns the index in ALR_VK_REPLY_ACQUIRE (round-trip: the guest
    // needs the index before recording into that image).
    ALR_VK_OP_ACQUIRE_NEXT_IMAGE = 226,  // u32 vdev, u32 vswapchain

    // Record a clear-to-background + one-triangle DRAW that uses the GUEST'S shader
    // modules (`vvert`/`vfrag`, previously CREATE_SHADER_MODULE'd) into swapchain
    // image `image_index` of `vswapchain` on `vdev`, into command buffer `vcmd`. This
    // is the VK-M3 CMD_BEGIN_DRAW promoted to (a) the guest's own SPIR-V and (b) a
    // swapchain render target (vs. a throwaway AHB). The matching QUEUE_PRESENT routes
    // the result to the compositor. The triangle's NDC vertices come from the host's
    // proven vertex buffer (kAlrVkTriVerts) so the guest's vert shader must take a
    // vec2 at location 0 (the bring-up contract; a later rung ships vertices on wire).
    ALR_VK_OP_CMD_BEGIN_DRAW_MODULES = 227,
    //   u32 vdev, u32 vcmd, u32 vswapchain, u32 image_index, u32 vvert, u32 vfrag,
    //   u32 width, u32 height, f32 bg_r, f32 bg_g, f32 bg_b, f32 bg_a

    // Submit `vcmd` (recorded by CMD_BEGIN_DRAW_MODULES) on `vqueue`, wait for the
    // GPU, then PRESENT swapchain image `image_index` of `vswapchain`: the host routes
    // that image's AHB to the in-app Wayland compositor (alr_wayland_submit_gpu_frame),
    // which composites it onto the SurfaceView as a wl_surface. The center-pixel
    // readback still comes back (ALR_VK_REPLY_PRESENT) so a HEADLESS host self-test can
    // assert the guest's shader produced the expected color without a display.
    ALR_VK_OP_QUEUE_PRESENT = 228,
    //   u32 vdev, u32 vqueue, u32 vcmd, u32 vswapchain, u32 image_index

    // Destroy the swapchain `vswapchain` (frees its AHB image ring).
    ALR_VK_OP_DESTROY_SWAPCHAIN = 229  // u32 vdev, u32 vswapchain
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
    ALR_VK_REPLY_PHYS_PROPS = 222,
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

    // ---- VK-M2 BODY replies ----
    // result of CREATE_DEVICE: the VkResult + the graphics queue family the host used.
    ALR_VK_REPLY_DEVICE = 223,  // u32 vdev, i32 vk_result, u32 gfx_queue_family

    // result of QUEUE_SUBMIT: the submit VkResult + the readback of the clear target's
    // center pixel (so the guest verifies the GPU clear landed). The render result is
    // a separate code (0 == the whole device->submit chain succeeded; non-zero = the
    // host-side stage that failed, see AlrVkRenderResult).
    ALR_VK_REPLY_SUBMIT = 224,
    //   u32 vdev
    //   u32 vcmd
    //   i32 submit_result        (VkResult of vkQueueSubmit; 0 == VK_SUCCESS)
    //   i32 render_result        (AlrVkRenderResult; 0 == clear rendered + read back)
    //   u8  px_r, px_g, px_b, px_a  (center pixel of the cleared target, 0..255)

    // ---- VK-M4 (PRESENT rung) replies ----
    // result of CREATE_SHADER_MODULE: the VkResult of vkCreateShaderModule on Mali.
    ALR_VK_REPLY_SHADER = 225,  // u32 vshader, i32 vk_result

    // result of CREATE_SWAPCHAIN: the VkResult + the image count the host actually
    // allocated (it may clamp the request to its AHB ring capacity).
    ALR_VK_REPLY_SWAPCHAIN = 226,  // u32 vswapchain, i32 vk_result, u32 image_count

    // result of ACQUIRE_NEXT_IMAGE: the acquired image index (and VkResult). The guest
    // records into / presents this index.
    ALR_VK_REPLY_ACQUIRE = 227,  // u32 vswapchain, u32 image_index, i32 vk_result

    // result of QUEUE_PRESENT: the submit+present VkResult, the render-path outcome,
    // and the center pixel of the presented image (so a headless self-test asserts the
    // guest shader's color even with no display). `presented` is 1 if the AHB was
    // actually handed to the compositor sink (0 if no sink wired — still a valid render).
    ALR_VK_REPLY_PRESENT = 228
    //   u32 vswapchain
    //   u32 image_index
    //   i32 submit_result        (VkResult; 0 == VK_SUCCESS)
    //   i32 render_result        (AlrVkRenderResult; 0 == drew + read back)
    //   u8  presented            (1 if routed to the compositor sink)
    //   u8  px_r, px_g, px_b, px_a   (center pixel of the presented image, 0..255)
};

// Host-side render-path outcome carried in ALR_VK_REPLY_SUBMIT::render_result. 0 means
// the clear genuinely rendered into the target and the readback succeeded; the rest name
// the stage that failed so a device run is diagnosable without a debugger.
enum AlrVkRenderResult {
    ALR_VK_RENDER_OK = 0,
    ALR_VK_RENDER_NO_DEVICE = 1,         // vdev/vqueue/vcmd not known to the host
    ALR_VK_RENDER_TARGET_ALLOC = 2,      // could not allocate the offscreen/AHB target
    ALR_VK_RENDER_RECORD = 3,            // command-buffer record (begin/renderpass) failed
    ALR_VK_RENDER_SUBMIT = 4,            // vkQueueSubmit / wait failed
    ALR_VK_RENDER_READBACK = 5,          // could not read the target back on the CPU
    ALR_VK_RENDER_NO_CLEAR_RECORDED = 6, // submit with no prior CMD_BEGIN_CLEAR
    // ---- VK-M3 draw-breadth stages (the bare-clear path never returns these) ----
    ALR_VK_RENDER_PIPELINE = 7,          // shader-module / pipeline-layout / vertex-buffer
                                         // / graphics-pipeline create failed (DRAW path)
    // ---- VK-M4 present stages ----
    ALR_VK_RENDER_NO_SWAPCHAIN = 8,      // vswapchain / image_index not known to the host
    ALR_VK_RENDER_NO_SHADER = 9          // a referenced guest shader module id was unknown
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
static inline void alr_vk_enc_f32(AlrVkEncoder *e, float v)   { alr_vk_enc_raw(e, &v, 4); }
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

/* ---- VK-M2 body request builders ---- */
static inline void alr_vk_enc_create_device(AlrVkEncoder *e, uint32_t vinst,
                                            uint32_t vphys, uint32_t vdev) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CREATE_DEVICE);
    alr_vk_enc_u32(e, vinst);
    alr_vk_enc_u32(e, vphys);
    alr_vk_enc_u32(e, vdev);
}
static inline void alr_vk_enc_get_device_queue(AlrVkEncoder *e, uint32_t vdev,
                                              uint32_t queue_index, uint32_t vqueue) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_GET_DEVICE_QUEUE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, queue_index);
    alr_vk_enc_u32(e, vqueue);
}
static inline void alr_vk_enc_create_command_pool(AlrVkEncoder *e, uint32_t vdev,
                                                 uint32_t vpool) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CREATE_COMMAND_POOL);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
}
static inline void alr_vk_enc_allocate_command_buffers(AlrVkEncoder *e, uint32_t vdev,
                                                      uint32_t vpool, uint32_t vcmd) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_ALLOCATE_COMMAND_BUFFERS);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vpool);
    alr_vk_enc_u32(e, vcmd);
}
static inline void alr_vk_enc_cmd_begin_clear(AlrVkEncoder *e, uint32_t vdev,
                                             uint32_t vcmd, uint32_t width,
                                             uint32_t height, float r, float g,
                                             float b, float a) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CMD_BEGIN_CLEAR);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vcmd);
    alr_vk_enc_u32(e, width);
    alr_vk_enc_u32(e, height);
    alr_vk_enc_f32(e, r);
    alr_vk_enc_f32(e, g);
    alr_vk_enc_f32(e, b);
    alr_vk_enc_f32(e, a);
}
/* VK-M3 draw-breadth: record a clear-to-background + one-triangle DRAW into `vcmd`.
 * Same field shape as cmd_begin_clear (the f32×4 is the BACKGROUND the pass clears to);
 * the triangle color is baked into the host's fragment shader, so the wire stays small. */
static inline void alr_vk_enc_cmd_begin_draw(AlrVkEncoder *e, uint32_t vdev,
                                            uint32_t vcmd, uint32_t width,
                                            uint32_t height, float bg_r, float bg_g,
                                            float bg_b, float bg_a) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CMD_BEGIN_DRAW);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vcmd);
    alr_vk_enc_u32(e, width);
    alr_vk_enc_u32(e, height);
    alr_vk_enc_f32(e, bg_r);
    alr_vk_enc_f32(e, bg_g);
    alr_vk_enc_f32(e, bg_b);
    alr_vk_enc_f32(e, bg_a);
}
static inline void alr_vk_enc_queue_submit(AlrVkEncoder *e, uint32_t vdev,
                                          uint32_t vqueue, uint32_t vcmd) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_QUEUE_SUBMIT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vqueue);
    alr_vk_enc_u32(e, vcmd);
}
static inline void alr_vk_enc_destroy_device(AlrVkEncoder *e, uint32_t vdev) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_DESTROY_DEVICE);
    alr_vk_enc_u32(e, vdev);
}

/* ---- VK-M4 (PRESENT rung) request builders ---- */
/* Upload the guest's own SPIR-V (spirv_bytes/spirv_len) as shader module `vshader`. */
static inline void alr_vk_enc_create_shader_module(AlrVkEncoder *e, uint32_t vdev,
                                                   uint32_t vshader, uint32_t stage,
                                                   const void *spirv_bytes,
                                                   uint32_t spirv_len) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CREATE_SHADER_MODULE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vshader);
    alr_vk_enc_u32(e, stage);
    alr_vk_enc_blob(e, spirv_bytes, spirv_len);
}
static inline void alr_vk_enc_destroy_shader_module(AlrVkEncoder *e, uint32_t vdev,
                                                    uint32_t vshader) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_DESTROY_SHADER_MODULE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vshader);
}
static inline void alr_vk_enc_create_swapchain(AlrVkEncoder *e, uint32_t vdev,
                                               uint32_t vswapchain, uint32_t width,
                                               uint32_t height, uint32_t image_count) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CREATE_SWAPCHAIN);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vswapchain);
    alr_vk_enc_u32(e, width);
    alr_vk_enc_u32(e, height);
    alr_vk_enc_u32(e, image_count);
}
static inline void alr_vk_enc_destroy_swapchain(AlrVkEncoder *e, uint32_t vdev,
                                                uint32_t vswapchain) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_DESTROY_SWAPCHAIN);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vswapchain);
}
static inline void alr_vk_enc_acquire_next_image(AlrVkEncoder *e, uint32_t vdev,
                                                 uint32_t vswapchain) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_ACQUIRE_NEXT_IMAGE);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vswapchain);
}
/* Record a clear-bg + triangle DRAW using the guest's own shader modules into the
 * swapchain image `image_index`. f32×4 is the background clear color. */
static inline void alr_vk_enc_cmd_begin_draw_modules(AlrVkEncoder *e, uint32_t vdev,
                                                     uint32_t vcmd, uint32_t vswapchain,
                                                     uint32_t image_index, uint32_t vvert,
                                                     uint32_t vfrag, uint32_t width,
                                                     uint32_t height, float bg_r,
                                                     float bg_g, float bg_b, float bg_a) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_CMD_BEGIN_DRAW_MODULES);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vcmd);
    alr_vk_enc_u32(e, vswapchain);
    alr_vk_enc_u32(e, image_index);
    alr_vk_enc_u32(e, vvert);
    alr_vk_enc_u32(e, vfrag);
    alr_vk_enc_u32(e, width);
    alr_vk_enc_u32(e, height);
    alr_vk_enc_f32(e, bg_r);
    alr_vk_enc_f32(e, bg_g);
    alr_vk_enc_f32(e, bg_b);
    alr_vk_enc_f32(e, bg_a);
}
/* Submit `vcmd` + present swapchain image `image_index` (route its AHB to the compositor). */
static inline void alr_vk_enc_queue_present(AlrVkEncoder *e, uint32_t vdev, uint32_t vqueue,
                                            uint32_t vcmd, uint32_t vswapchain,
                                            uint32_t image_index) {
    alr_vk_enc_u8(e, (uint8_t)ALR_VK_OP_QUEUE_PRESENT);
    alr_vk_enc_u32(e, vdev);
    alr_vk_enc_u32(e, vqueue);
    alr_vk_enc_u32(e, vcmd);
    alr_vk_enc_u32(e, vswapchain);
    alr_vk_enc_u32(e, image_index);
}

/* aarch64-linux-gnu (the guest target) is little-endian, so the memcpy-of-native
 * encoding is the wire. If this contract is ever cross-built big-endian, both the
 * encoder here AND the host Reader must byte-swap. */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ALR_GPU_ALR_GPU_VK_PROTO_HPP */
