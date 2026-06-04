// ALR GPU Vulkan marshalling self-test — VK-M2 first step (host-verifiable).
//
// Pushes a Vulkan enumerate/props REQUEST stream through the SPSC command ring
// (alr_gpu_ring.hpp), drains it on the host decoder (alr_gpu_vk_decode.hpp) against
// REAL vendor Mali libvulkan, encodes a REPLY stream, pushes the reply back through a
// second ring, and decodes it guest-side — proving the full enumerate/props round trip
// over the actual transport. This is the Vulkan twin of run_ring_draw_probe()
// (alr_gpu_probe.hpp), which did the same for a GLES draw batch.
//
// Two report modes, both header-only + self-contained:
//   - run_vk_marshal_wire_probe(): NO Vulkan SDK needed. A synthetic provider injects a
//     "Mali-G615, VK 1.3, graphics queue" device so the WIRE round trip (request ->
//     ring -> decode -> reply -> ring -> decode) is verified on any host (macOS/Linux),
//     exactly so the native wire test + CI can prove the protocol with no GPU.
//   - run_vk_marshal_mali_probe(): on device (ALR_VK_DECODE_REAL), the SAME path with
//     the synthetic provider replaced by vendor libvulkan, so the enumerated device,
//     apiVersion and queue families come from the real Mali driver. Gating line
//     "ALR VK ENUM MARSHAL: PASS/FAIL" (MainActivity greps it like the other probes).
//
// The point of VK-M2 is the round trip + the client-side virtual-handle model, NOT a
// render — see alr_gpu_vk.hpp for the VK-M1 AHB-render keystone and
// docs/research/gpu-guest-accel-strategy.md §7 (VK-M2) / §10.3.

#ifndef ALR_GPU_ALR_GPU_VK_MARSHAL_PROBE_HPP
#define ALR_GPU_ALR_GPU_VK_MARSHAL_PROBE_HPP

#include <cstdint>
#include <cstdlib>  // std::abs (clear-readback tolerance check)
#include <cstring>  // std::memcpy (VK-M4 SPIR-V magic check in the synthetic provider)
#include <sstream>
#include <string>
#include <vector>

#include "alr_gpu/alr_gpu_ring.hpp"
#include "alr_gpu/alr_gpu_vk_decode.hpp"
#include "alr_gpu/alr_gpu_vk_proto.hpp"

namespace alr::gpu {

// VK_API_VERSION_1_3 packed value (major 1, minor 3) — kept local so this header
// needs no <vulkan.h>. Vulkan packs version as (major<<22)|(minor<<12)|patch.
inline constexpr uint32_t kAlrVkApi11 = (1u << 22) | (1u << 12);  // 1.1
inline constexpr uint32_t kAlrVkApi13 = (1u << 22) | (3u << 12);  // 1.3

// ---- synthetic Mali provider (host wire mode): answers enumerate/props with one
// "Mali-G615" graphics device so the round trip is verifiable without a GPU. ----
struct SyntheticMaliProvider {
    bool instance_created = false;
    uint32_t phys_base = 0;
    static int create_instance(void* ctx, uint32_t /*vinst*/, uint32_t /*api*/) {
        static_cast<SyntheticMaliProvider*>(ctx)->instance_created = true;
        return 0;  // VK_SUCCESS
    }
    static uint32_t enumerate(void* ctx, uint32_t /*vinst*/, uint32_t vphys_base) {
        static_cast<SyntheticMaliProvider*>(ctx)->phys_base = vphys_base;
        return 1;  // one device
    }
    static bool props(void* /*ctx*/, uint32_t vphys, VkPhysProps& out) {
        out.api_version = kAlrVkApi13;
        out.driver_version = 0x0001;
        out.vendor_id = 0x13B5;  // ARM
        out.device_id = 0x0;
        out.device_type = ALR_VK_PHYS_TYPE_INTEGRATED_GPU;
        out.device_name = "Mali-G615 (synthetic-wire)";
        out.is_software = false;
        out.queue_families.push_back({/*GRAPHICS|COMPUTE|TRANSFER*/ 0x7u, 1u});
        (void)vphys;
        return true;
    }
    static void destroy_instance(void* ctx, uint32_t /*vinst*/) {
        static_cast<SyntheticMaliProvider*>(ctx)->instance_created = false;
    }
    // ANGLE-init rung: answer an image-format query like a real GPU would for the common
    // render/sampled tuples. R8G8B8A8_UNORM (37) optimal COLOR_ATTACHMENT/SAMPLED is
    // "supported" with a generous max extent; everything else is reported supported too
    // (the synthetic device is permissive — the REAL Mali path returns the true verdict).
    static bool image_format_props(void* /*ctx*/, uint32_t /*vphys*/, uint32_t /*format*/,
                                   uint32_t /*type*/, uint32_t /*tiling*/, uint32_t /*usage*/,
                                   uint32_t /*flags*/, VkImageFmtProps& out) {
        out.vk_result = 0;  // VK_SUCCESS
        out.max_extent_w = 16384;
        out.max_extent_h = 16384;
        out.max_extent_d = 1;
        out.max_mip_levels = 15;
        out.max_array_layers = 2048;
        out.sample_counts = 0x1 | 0x4;  // VK_SAMPLE_COUNT_1_BIT | _4_BIT
        out.max_resource_size = static_cast<uint64_t>(1) << 31;  // 2 GiB
        return true;
    }
    // ANGLE-init rung: answer a per-format feature query with a REALISTIC (not all-bits) mask
    // so the wire round trip proves Mali's actual feature bits survive marshalling. We report
    // a typical color-renderable/sampled RGBA format's flags: SAMPLED_IMAGE (0x1) |
    // COLOR_ATTACHMENT (0x80) | COLOR_ATTACHMENT_BLEND (0x100) | BLIT_SRC (0x400) |
    // BLIT_DST (0x800) on optimal tiling; VERTEX_BUFFER (0x40) on buffers. The REAL Mali path
    // (vk_real_format_props) returns the true per-format verdict.
    static bool format_props(void* /*ctx*/, uint32_t /*vphys*/, uint32_t /*format*/,
                             VkFmtProps& out) {
        out.optimal_tiling_features = 0x1u | 0x80u | 0x100u | 0x400u | 0x800u;
        out.linear_tiling_features = 0x1u;        // SAMPLED_IMAGE on linear
        out.buffer_features = 0x40u;              // VERTEX_BUFFER
        return true;
    }
    // ---- VK-M2 body seams: the synthetic device behaves like a real one for the wire
    // round trip. create/queue/pool/cmd succeed; clear_submit "renders" by returning the
    // requested clear color as the center pixel (0..1 -> 0..255), so the round trip can
    // assert the GPU clear value survives the marshalling end to end. ----
    bool device_created = false;
    static int create_device(void* ctx, uint32_t /*vphys*/, uint32_t /*vdev*/,
                             uint32_t* gfx_family_out) {
        static_cast<SyntheticMaliProvider*>(ctx)->device_created = true;
        if (gfx_family_out) *gfx_family_out = 0;  // synthetic graphics family index
        return 0;  // VK_SUCCESS
    }
    static void get_queue(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*qi*/,
                          uint32_t /*vqueue*/) {}
    static int create_pool(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vpool*/) { return 0; }
    static int alloc_cmd(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vpool*/,
                         uint32_t /*vcmd*/) {
        return 0;
    }
    static uint8_t syn_to_u8(float f) {
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        return static_cast<uint8_t>(f * 255.0f + 0.5f);
    }
    static int clear_submit(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vqueue*/,
                            uint32_t /*vcmd*/, const VkClearRecord& rec, uint8_t px[4]) {
        if (!rec.recorded) return ALR_VK_RENDER_NO_CLEAR_RECORDED;
        px[0] = syn_to_u8(rec.clear[0]);
        px[1] = syn_to_u8(rec.clear[1]);
        px[2] = syn_to_u8(rec.clear[2]);
        px[3] = syn_to_u8(rec.clear[3]);
        return ALR_VK_RENDER_OK;
    }
    // VK-M3 draw seam: the synthetic device "rasterizes" the fixed-color triangle that
    // covers the center, so the center-pixel readback is the TRIANGLE color (not the bg).
    // This lets the wire round trip assert the draw path end to end without a GPU.
    static int draw_submit(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vqueue*/,
                           uint32_t /*vcmd*/, const VkClearRecord& rec, uint8_t px[4]) {
        if (!rec.recorded) return ALR_VK_RENDER_NO_CLEAR_RECORDED;
        px[0] = syn_to_u8(kAlrVkTriColorR);
        px[1] = syn_to_u8(kAlrVkTriColorG);
        px[2] = syn_to_u8(kAlrVkTriColorB);
        px[3] = syn_to_u8(kAlrVkTriColorA);
        return ALR_VK_RENDER_OK;
    }
    static void destroy_device(void* ctx, uint32_t /*vdev*/) {
        static_cast<SyntheticMaliProvider*>(ctx)->device_created = false;
    }
    // ---- VK-M4 (PRESENT rung) seams: the synthetic device accepts the guest's SPIR-V
    // blob (asserting only the SPIR-V magic, like the real path), serves an AHB-less
    // swapchain (2 images), rotates acquire, and "presents" by rasterizing the fixed
    // triangle color into the center pixel + counting the present. This lets the wire
    // round trip assert the SPIR-V/swapchain/present path end to end without a GPU. ----
    uint32_t swap_next = 0;
    int present_count = 0;
    static int create_shader_module(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vshader*/,
                                    const uint8_t* spirv, uint32_t spirv_len) {
        if (!spirv || spirv_len < 8 || (spirv_len % 4) != 0) return -1;
        uint32_t magic = 0; std::memcpy(&magic, spirv, 4);
        return (magic == 0x07230203u) ? 0 : -1;  // SPIR-V magic, as the real path checks
    }
    static void destroy_shader_module(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vshader*/) {}
    static int create_swapchain(void* /*ctx*/, uint32_t /*vdev*/, uint32_t /*vsw*/,
                                uint32_t /*w*/, uint32_t /*h*/, uint32_t image_count,
                                uint32_t* image_count_out) {
        uint32_t n = image_count ? image_count : 2;
        if (n > 4) n = 4;
        if (image_count_out) *image_count_out = n;
        return 0;
    }
    static void destroy_swapchain(void* /*ctx*/, uint32_t /*vsw*/) {}
    static bool acquire_next_image(void* ctx, uint32_t /*vsw*/, uint32_t* index_out) {
        SyntheticMaliProvider* s = static_cast<SyntheticMaliProvider*>(ctx);
        if (index_out) *index_out = s->swap_next;
        s->swap_next = (s->swap_next + 1u) % 2u;  // synthetic 2-image ring
        return true;
    }
    static int draw_present(void* ctx, uint32_t /*vdev*/, uint32_t /*vqueue*/,
                            uint32_t /*vcmd*/, const VkClearRecord& rec, uint8_t px[4],
                            uint8_t* presented) {
        if (!rec.recorded || !rec.is_present_draw) return ALR_VK_RENDER_NO_CLEAR_RECORDED;
        // The guest's frag shader emits the fixed triangle color (the synthetic device
        // can't run SPIR-V, but the real path produces exactly this; the wire test only
        // needs the round trip + the color to survive marshalling).
        px[0] = syn_to_u8(kAlrVkTriColorR);
        px[1] = syn_to_u8(kAlrVkTriColorG);
        px[2] = syn_to_u8(kAlrVkTriColorB);
        px[3] = syn_to_u8(kAlrVkTriColorA);
        static_cast<SyntheticMaliProvider*>(ctx)->present_count++;
        if (presented) *presented = 1;  // "routed" (a real sink would composite it)
        return ALR_VK_RENDER_OK;
    }
    VkProvider as_provider() {
        VkProvider p{};
        p.create_instance = &create_instance;
        p.enumerate = &enumerate;
        p.props = &props;
        p.image_format_props = &image_format_props;
        p.format_props = &format_props;
        p.destroy_instance = &destroy_instance;
        p.create_device = &create_device;
        p.get_queue = &get_queue;
        p.create_pool = &create_pool;
        p.alloc_cmd = &alloc_cmd;
        p.clear_submit = &clear_submit;
        p.draw_submit = &draw_submit;  // VK-M3 draw seam
        p.destroy_device = &destroy_device;
        // VK-M4 present-rung seams
        p.create_shader_module = &create_shader_module;
        p.destroy_shader_module = &destroy_shader_module;
        p.create_swapchain = &create_swapchain;
        p.destroy_swapchain = &destroy_swapchain;
        p.acquire_next_image = &acquire_next_image;
        p.draw_present = &draw_present;
        p.ctx = this;
        return p;
    }
};

// Build the standard request stream a guest libvulkan ICD would emit to enumerate +
// probe the first device: create instance v1, enumerate (assign vphys ids from 100),
// then props for the first device (v100), then destroy. Client-side virtual ids — no
// round-trip on create; the only data that comes back is the enumerate count + props.
inline std::vector<uint8_t> build_vk_enum_request(uint32_t vinst = 1,
                                                  uint32_t vphys_base = 100,
                                                  uint32_t api = kAlrVkApi13) {
    std::vector<uint8_t> buf(256, 0);
    AlrVkEncoder e;
    alr_vk_enc_init(&e, buf.data(), buf.size());
    alr_vk_enc_create_instance(&e, vinst, api);
    alr_vk_enc_enumerate_phys(&e, vinst, vphys_base);
    alr_vk_enc_get_phys_props(&e, vinst, vphys_base);  // first device = vphys_base
    alr_vk_enc_destroy_instance(&e, vinst);
    alr_vk_enc_u8(&e, static_cast<uint8_t>(ALR_VK_OP_END));
    buf.resize(e.len);
    return buf;
}

// Core round trip shared by both modes. `provider` non-null = wire mode (synthetic);
// null = device mode (real vendor libvulkan, needs ALR_VK_DECODE_REAL). Returns a
// report; sets `pass`.
inline std::string vk_marshal_roundtrip(const VkProvider* provider, const char* mode,
                                        bool& pass) {
    std::ostringstream out;
    pass = false;
    constexpr uint32_t kVinst = 1, kVphysBase = 100;

    // --- request ring (guest -> host) ---
    constexpr uint32_t kRing = 1u << 16;  // 64 KiB, ample for enumerate/props
    std::vector<uint8_t> req_region(ring_region_size(kRing), 0);
    std::vector<uint8_t> rep_region(ring_region_size(kRing), 0);
    if (!ring_init(req_region.data(), kRing) || !ring_init(rep_region.data(), kRing)) {
        out << "ALR VK ENUM MARSHAL: FAIL\nalr vk marshal error=ring-init";
        return out.str();
    }
    RingProducer req_prod(req_region.data());
    RingConsumer req_cons(req_region.data());
    RingProducer rep_prod(rep_region.data());
    RingConsumer rep_cons(rep_region.data());

    // --- guest encodes the request, pushes it, signals a sync ---
    const std::vector<uint8_t> request = build_vk_enum_request(kVinst, kVphysBase);
    const bool pushed =
        req_prod.append(request.data(), static_cast<uint32_t>(request.size()));
    req_prod.flush_and_wait(1);

    // --- host drains the request, replays on (real or synthetic) Vulkan, replies ---
    std::vector<uint8_t> req_snap(req_cons.available());
    const uint32_t req_got =
        req_cons.snapshot(req_snap.data(), static_cast<uint32_t>(req_snap.size()));
    VkDecodeState st;
    VkReplyEncoder reply;
    const bool decode_ok = decode_vk_batch(req_snap.data(), req_got, st, reply, provider);
    req_cons.advance(req_got);
    req_cons.post_reply();

    // --- host pushes the reply back; guest drains + decodes it ---
    const std::vector<uint8_t>& reply_bytes = reply.bytes();
    const bool rep_pushed =
        rep_prod.append(reply_bytes.data(), static_cast<uint32_t>(reply_bytes.size()));
    rep_prod.flush_and_wait(1);
    std::vector<uint8_t> rep_snap(rep_cons.available());
    const uint32_t rep_got =
        rep_cons.snapshot(rep_snap.data(), static_cast<uint32_t>(rep_snap.size()));
    rep_cons.advance(rep_got);
    VkDecodedReply decoded;
    const bool reply_ok = decode_vk_reply(rep_snap.data(), rep_got, decoded);

    // --- verify the round trip end to end ---
    const bool transport_ok = pushed && rep_pushed && req_got == request.size() &&
                              rep_got == reply_bytes.size() &&
                              req_cons.available() == 0 && rep_cons.available() == 0;
    const bool inst_ok = decoded.instances.size() == 1 &&
                         decoded.instances[0].vinst == kVinst &&
                         decoded.instances[0].result == 0;
    const bool enum_ok = decoded.enumerations.size() == 1 &&
                         decoded.enumerations[0].vinst == kVinst &&
                         decoded.enumerations[0].vphys_base == kVphysBase &&
                         decoded.enumerations[0].count >= 1;
    auto it = decoded.props.find(kVphysBase);
    const bool props_present = it != decoded.props.end();
    const bool props_ok =
        props_present && !it->second.device_name.empty() && !it->second.is_software &&
        !it->second.queue_families.empty() &&
        // at least one queue family advertises GRAPHICS (bit 0 of VkQueueFlags).
        [&] {
            for (const auto& qf : it->second.queue_families)
                if (qf.flags & 0x1u) return true;
            return false;
        }();

    pass = decode_ok && reply_ok && transport_ok && inst_ok && enum_ok && props_ok;

    out << "ALR VK ENUM MARSHAL: " << (pass ? "PASS" : "FAIL");
    out << "\nalr vk marshal mode=" << mode;
    out << "\nalr vk marshal request bytes=" << request.size() << " reply bytes="
        << reply_bytes.size();
    out << "\nalr vk marshal transport=" << (transport_ok ? "ok" : "bad")
        << " (req drained=" << req_got << " rep drained=" << rep_got << ")";
    out << "\nalr vk marshal ops decoded=" << st.decoded;
    out << "\nalr vk marshal instance result=" << (inst_ok ? "VK_SUCCESS" : "fail");
    if (enum_ok)
        out << "\nalr vk marshal device count=" << decoded.enumerations[0].count
            << " vphys_base=" << decoded.enumerations[0].vphys_base;
    if (props_present) {
        const auto& p = it->second;
        out << "\nalr vk marshal renderer=" << p.device_name;
        out << "\nalr vk marshal api=" << (p.api_version >> 22) << "."
            << ((p.api_version >> 12) & 0x3ff);
        out << "\nalr vk marshal vendorID=0x" << std::hex << p.vendor_id << std::dec;
        out << "\nalr vk marshal device type=" << p.device_type;
        out << "\nalr vk marshal queue families=" << p.queue_families.size();
        for (size_t i = 0; i < p.queue_families.size(); ++i)
            out << "\nalr vk marshal qf[" << i << "] flags=0x" << std::hex
                << p.queue_families[i].flags << std::dec
                << " count=" << p.queue_families[i].count;
        out << "\nalr vk marshal software renderer=" << (p.is_software ? "true" : "false");
    } else {
        out << "\nalr vk marshal props=absent";
    }
    out << "\nalr vk marshal path=guest-encode -> ring -> host-decode(vendor libvulkan)"
           " -> reply -> ring -> guest-decode (enumerate/props round trip)";
    return out.str();
}

// Host wire mode: synthetic Mali provider, no GPU. Verifiable on any host + in CI.
inline std::string run_vk_marshal_wire_probe() {
    SyntheticMaliProvider syn;
    VkProvider prov = syn.as_provider();
    bool pass = false;
    return vk_marshal_roundtrip(&prov, "wire-synthetic", pass);
}

#ifdef ALR_VK_DECODE_REAL
// Device mode: real vendor Mali libvulkan. The enumerated device, apiVersion and queue
// families come from the actual Mali driver. DEVICE-REQ verification entry point.
inline std::string run_vk_marshal_mali_probe() {
    bool pass = false;
    return vk_marshal_roundtrip(/*provider=*/nullptr, "mali-libvulkan", pass);
}
#endif

// ===========================================================================
// VK-M2 BODY — the render path: device + queue + command pool/buffer + clear submit.
//
// Builds the request stream a guest libvulkan ICD would emit to bring up a logical
// device and clear an offscreen target, pushes it through the SPSC ring, decodes it
// host-side (synthetic OR real Mali), and decodes the reply to verify the device was
// created and the GPU clear value came back. This is the Vulkan twin of the GLES
// draw-probe — it extends the enumerate/props round trip to actually drive the GPU.
// ===========================================================================

// The clear color the render probe asks for (a distinctive non-grey value so the
// readback can't accidentally match an uninitialized buffer). RGBA, 0..1.
inline constexpr float kAlrVkClearR = 0.10f;
inline constexpr float kAlrVkClearG = 0.86f;  // ~220/255 (matches the GLES probe's green)
inline constexpr float kAlrVkClearB = 0.40f;
inline constexpr float kAlrVkClearA = 1.00f;

// Build the full VK-M2-body request: create instance, enumerate, props, create device on
// the first device, get its queue, create a pool + command buffer, record a CLEAR, submit
// it, then tear down. All handles are client-side virtual ids (no per-call round-trip);
// the only data that comes back is the create/submit results + the clear readback.
inline std::vector<uint8_t> build_vk_render_request(uint32_t vinst = 1,
                                                    uint32_t vphys_base = 100,
                                                    uint32_t vdev = 10,
                                                    uint32_t vqueue = 20,
                                                    uint32_t vpool = 30,
                                                    uint32_t vcmd = 40,
                                                    uint32_t w = 64, uint32_t h = 64,
                                                    uint32_t api = kAlrVkApi13) {
    std::vector<uint8_t> buf(512, 0);
    AlrVkEncoder e;
    alr_vk_enc_init(&e, buf.data(), buf.size());
    alr_vk_enc_create_instance(&e, vinst, api);
    alr_vk_enc_enumerate_phys(&e, vinst, vphys_base);
    alr_vk_enc_get_phys_props(&e, vinst, vphys_base);
    alr_vk_enc_create_device(&e, vinst, vphys_base, vdev);
    alr_vk_enc_get_device_queue(&e, vdev, /*queue_index=*/0, vqueue);
    alr_vk_enc_create_command_pool(&e, vdev, vpool);
    alr_vk_enc_allocate_command_buffers(&e, vdev, vpool, vcmd);
    alr_vk_enc_cmd_begin_clear(&e, vdev, vcmd, w, h, kAlrVkClearR, kAlrVkClearG,
                               kAlrVkClearB, kAlrVkClearA);
    alr_vk_enc_queue_submit(&e, vdev, vqueue, vcmd);
    alr_vk_enc_destroy_device(&e, vdev);
    alr_vk_enc_destroy_instance(&e, vinst);
    alr_vk_enc_u8(&e, static_cast<uint8_t>(ALR_VK_OP_END));
    buf.resize(e.len);
    return buf;
}

// Core render round trip shared by both modes. `provider` non-null = wire mode; null =
// device mode (real Mali libvulkan, needs ALR_VK_DECODE_REAL). Returns a report; sets
// `pass`. PASS requires the device created, the submit succeeded, the clear rendered
// (render_result == OK), and the readback center pixel ~matches the requested clear.
inline std::string vk_render_roundtrip(const VkProvider* provider, const char* mode,
                                       bool& pass) {
    std::ostringstream out;
    pass = false;
    constexpr uint32_t kVdev = 10, kVcmd = 40;

    constexpr uint32_t kRing = 1u << 16;  // 64 KiB
    std::vector<uint8_t> req_region(ring_region_size(kRing), 0);
    std::vector<uint8_t> rep_region(ring_region_size(kRing), 0);
    if (!ring_init(req_region.data(), kRing) || !ring_init(rep_region.data(), kRing)) {
        out << "ALR VK RENDER MARSHAL: FAIL\nalr vk render error=ring-init";
        return out.str();
    }
    RingProducer req_prod(req_region.data());
    RingConsumer req_cons(req_region.data());
    RingProducer rep_prod(rep_region.data());
    RingConsumer rep_cons(rep_region.data());

    const std::vector<uint8_t> request = build_vk_render_request();
    const bool pushed =
        req_prod.append(request.data(), static_cast<uint32_t>(request.size()));
    req_prod.flush_and_wait(1);

    std::vector<uint8_t> req_snap(req_cons.available());
    const uint32_t req_got =
        req_cons.snapshot(req_snap.data(), static_cast<uint32_t>(req_snap.size()));
    VkDecodeState st;
    VkReplyEncoder reply;
    const bool decode_ok = decode_vk_batch(req_snap.data(), req_got, st, reply, provider);
    req_cons.advance(req_got);
    req_cons.post_reply();

    const std::vector<uint8_t>& reply_bytes = reply.bytes();
    const bool rep_pushed =
        rep_prod.append(reply_bytes.data(), static_cast<uint32_t>(reply_bytes.size()));
    rep_prod.flush_and_wait(1);
    std::vector<uint8_t> rep_snap(rep_cons.available());
    const uint32_t rep_got =
        rep_cons.snapshot(rep_snap.data(), static_cast<uint32_t>(rep_snap.size()));
    rep_cons.advance(rep_got);
    VkDecodedReply decoded;
    const bool reply_ok = decode_vk_reply(rep_snap.data(), rep_got, decoded);

    const bool transport_ok = pushed && rep_pushed && req_got == request.size() &&
                              rep_got == reply_bytes.size() &&
                              req_cons.available() == 0 && rep_cons.available() == 0;
    const bool dev_ok = decoded.devices.size() == 1 && decoded.devices[0].vdev == kVdev &&
                        decoded.devices[0].result == 0;
    const bool submit_present = decoded.submits.size() == 1;
    const auto* sub = submit_present ? &decoded.submits[0] : nullptr;
    const bool submit_ok = sub && sub->vdev == kVdev && sub->vcmd == kVcmd &&
                           sub->submit_result == 0 && sub->render_result == ALR_VK_RENDER_OK;
    // the readback center pixel must ~match the requested clear (tolerance for UNORM
    // rounding + tile resolve). Compare against the 0..255 form of the clear constants.
    auto to_u8 = [](float f) -> int {
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        return static_cast<int>(f * 255.0f + 0.5f);
    };
    const int want_r = to_u8(kAlrVkClearR), want_g = to_u8(kAlrVkClearG),
              want_b = to_u8(kAlrVkClearB);
    bool clear_ok = false;
    if (sub) {
        const int dr = std::abs(static_cast<int>(sub->px[0]) - want_r);
        const int dg = std::abs(static_cast<int>(sub->px[1]) - want_g);
        const int db = std::abs(static_cast<int>(sub->px[2]) - want_b);
        clear_ok = dr <= 8 && dg <= 8 && db <= 8;
    }

    pass = decode_ok && reply_ok && transport_ok && dev_ok && submit_ok && clear_ok;

    out << "ALR VK RENDER MARSHAL: " << (pass ? "PASS" : "FAIL");
    out << "\nalr vk render mode=" << mode;
    out << "\nalr vk render request bytes=" << request.size() << " reply bytes="
        << reply_bytes.size();
    out << "\nalr vk render transport=" << (transport_ok ? "ok" : "bad")
        << " (req drained=" << req_got << " rep drained=" << rep_got << ")";
    out << "\nalr vk render ops decoded=" << st.decoded;
    out << "\nalr vk render device created=" << (dev_ok ? "yes" : "no");
    if (dev_ok)
        out << "\nalr vk render gfx queue family=" << decoded.devices[0].gfx_family;
    if (sub) {
        out << "\nalr vk render submit result=" << (sub->submit_result == 0 ? "VK_SUCCESS" : "fail");
        out << "\nalr vk render render result=" << sub->render_result
            << " (0=OK 1=no-dev 2=target 3=record 4=submit 5=readback 6=no-clear)";
        out << "\nalr vk render center px=" << static_cast<int>(sub->px[0]) << ","
            << static_cast<int>(sub->px[1]) << "," << static_cast<int>(sub->px[2]) << ","
            << static_cast<int>(sub->px[3]) << " (expect ~" << want_r << "," << want_g << ","
            << want_b << " clear)";
        out << "\nalr vk render clear match=" << (clear_ok ? "yes" : "no");
    } else {
        out << "\nalr vk render submit=absent";
    }
    out << "\nalr vk render path=guest-encode(device/queue/pool/cmd/clear/submit) -> ring"
           " -> host-decode(vendor libvulkan, clear into AHB) -> readback -> reply -> ring"
           " -> guest-decode";
    return out.str();
}

// Host wire mode: synthetic provider "renders" the clear (returns the clear color as the
// readback pixel), so the full device/queue/cmd/clear/submit round trip is verifiable on
// any host with NO Vulkan SDK / no GPU.
inline std::string run_vk_render_wire_probe() {
    SyntheticMaliProvider syn;
    VkProvider prov = syn.as_provider();
    bool pass = false;
    return vk_render_roundtrip(&prov, "wire-synthetic", pass);
}

#ifdef ALR_VK_DECODE_REAL
// Device mode: real vendor Mali libvulkan clears an AHB-backed COLOR_ATTACHMENT on the
// actual GPU and reads it back. DEVICE-REQ verification entry point — the integration
// session wires this behind a JNI nativeAlrGpuVkRenderProbe and greps the gating line
// "ALR VK RENDER MARSHAL: PASS".
inline std::string run_vk_render_mali_probe() {
    bool pass = false;
    return vk_render_roundtrip(/*provider=*/nullptr, "mali-libvulkan", pass);
}
#endif

// ===========================================================================
// VK-M3 (render BREADTH) — the DRAW path: graphics pipeline + vertex buffer + a real
// vkCmdDraw of one triangle, beyond the bare clear. Builds the request stream a guest
// libvulkan ICD would emit to bring up a device and DRAW a triangle, pushes it through
// the SPSC ring, decodes it host-side (synthetic OR real Mali), and decodes the reply to
// verify the center pixel came back as the TRIANGLE color — distinct from the clear
// background, proving a draw (not just a clear) executed.
//
// FOR WS-1 (JNI wiring): expose run_vk_draw_mali_probe() behind a new JNI entry
// `nativeAlrGpuVkDrawProbe` exactly like nativeAlrGpuVkRenderProbe wires
// run_vk_render_mali_probe(). MainActivity should grep the gating first line
// "ALR VK DRAW MARSHAL: PASS". No JNI is added here (this header stays JNI-free).
// ===========================================================================

// The background the draw probe clears to BEFORE the triangle — deliberately FAR from the
// triangle color (kAlrVkTriColor*) so the readback unambiguously shows the draw landed.
inline constexpr float kAlrVkDrawBgR = 0.04f;
inline constexpr float kAlrVkDrawBgG = 0.04f;
inline constexpr float kAlrVkDrawBgB = 0.06f;  // near-black blue-grey
inline constexpr float kAlrVkDrawBgA = 1.00f;

// Build the full VK-M3 draw request: create instance, enumerate, props, create device,
// get queue, pool + command buffer, record a DRAW (clear bg + triangle), submit, tear
// down. Same client-side virtual-handle shape as build_vk_render_request; the only
// difference is CMD_BEGIN_DRAW instead of CMD_BEGIN_CLEAR.
inline std::vector<uint8_t> build_vk_draw_request(uint32_t vinst = 1,
                                                  uint32_t vphys_base = 100,
                                                  uint32_t vdev = 10, uint32_t vqueue = 20,
                                                  uint32_t vpool = 30, uint32_t vcmd = 40,
                                                  uint32_t w = 64, uint32_t h = 64,
                                                  uint32_t api = kAlrVkApi13) {
    std::vector<uint8_t> buf(512, 0);
    AlrVkEncoder e;
    alr_vk_enc_init(&e, buf.data(), buf.size());
    alr_vk_enc_create_instance(&e, vinst, api);
    alr_vk_enc_enumerate_phys(&e, vinst, vphys_base);
    alr_vk_enc_get_phys_props(&e, vinst, vphys_base);
    alr_vk_enc_create_device(&e, vinst, vphys_base, vdev);
    alr_vk_enc_get_device_queue(&e, vdev, /*queue_index=*/0, vqueue);
    alr_vk_enc_create_command_pool(&e, vdev, vpool);
    alr_vk_enc_allocate_command_buffers(&e, vdev, vpool, vcmd);
    alr_vk_enc_cmd_begin_draw(&e, vdev, vcmd, w, h, kAlrVkDrawBgR, kAlrVkDrawBgG,
                              kAlrVkDrawBgB, kAlrVkDrawBgA);
    alr_vk_enc_queue_submit(&e, vdev, vqueue, vcmd);
    alr_vk_enc_destroy_device(&e, vdev);
    alr_vk_enc_destroy_instance(&e, vinst);
    alr_vk_enc_u8(&e, static_cast<uint8_t>(ALR_VK_OP_END));
    buf.resize(e.len);
    return buf;
}

// Core draw round trip shared by both modes. `provider` non-null = wire mode; null =
// device mode (real Mali libvulkan, needs ALR_VK_DECODE_REAL). PASS requires the device
// created, the submit succeeded, the draw rendered (render_result == OK), the readback
// center pixel ~matches the TRIANGLE color, AND that pixel is NOT the clear background
// (so a fallback clear can't masquerade as a draw).
inline std::string vk_draw_roundtrip(const VkProvider* provider, const char* mode,
                                     bool& pass) {
    std::ostringstream out;
    pass = false;
    constexpr uint32_t kVdev = 10, kVcmd = 40;

    constexpr uint32_t kRing = 1u << 16;  // 64 KiB
    std::vector<uint8_t> req_region(ring_region_size(kRing), 0);
    std::vector<uint8_t> rep_region(ring_region_size(kRing), 0);
    if (!ring_init(req_region.data(), kRing) || !ring_init(rep_region.data(), kRing)) {
        out << "ALR VK DRAW MARSHAL: FAIL\nalr vk draw error=ring-init";
        return out.str();
    }
    RingProducer req_prod(req_region.data());
    RingConsumer req_cons(req_region.data());
    RingProducer rep_prod(rep_region.data());
    RingConsumer rep_cons(rep_region.data());

    const std::vector<uint8_t> request = build_vk_draw_request();
    const bool pushed =
        req_prod.append(request.data(), static_cast<uint32_t>(request.size()));
    req_prod.flush_and_wait(1);

    std::vector<uint8_t> req_snap(req_cons.available());
    const uint32_t req_got =
        req_cons.snapshot(req_snap.data(), static_cast<uint32_t>(req_snap.size()));
    VkDecodeState st;
    VkReplyEncoder reply;
    const bool decode_ok = decode_vk_batch(req_snap.data(), req_got, st, reply, provider);
    req_cons.advance(req_got);
    req_cons.post_reply();

    const std::vector<uint8_t>& reply_bytes = reply.bytes();
    const bool rep_pushed =
        rep_prod.append(reply_bytes.data(), static_cast<uint32_t>(reply_bytes.size()));
    rep_prod.flush_and_wait(1);
    std::vector<uint8_t> rep_snap(rep_cons.available());
    const uint32_t rep_got =
        rep_cons.snapshot(rep_snap.data(), static_cast<uint32_t>(rep_snap.size()));
    rep_cons.advance(rep_got);
    VkDecodedReply decoded;
    const bool reply_ok = decode_vk_reply(rep_snap.data(), rep_got, decoded);

    const bool transport_ok = pushed && rep_pushed && req_got == request.size() &&
                              rep_got == reply_bytes.size() &&
                              req_cons.available() == 0 && rep_cons.available() == 0;
    const bool dev_ok = decoded.devices.size() == 1 && decoded.devices[0].vdev == kVdev &&
                        decoded.devices[0].result == 0;
    const bool submit_present = decoded.submits.size() == 1;
    const auto* sub = submit_present ? &decoded.submits[0] : nullptr;
    const bool submit_ok = sub && sub->vdev == kVdev && sub->vcmd == kVcmd &&
                           sub->submit_result == 0 && sub->render_result == ALR_VK_RENDER_OK;
    auto to_u8 = [](float f) -> int {
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        return static_cast<int>(f * 255.0f + 0.5f);
    };
    const int want_r = to_u8(kAlrVkTriColorR), want_g = to_u8(kAlrVkTriColorG),
              want_b = to_u8(kAlrVkTriColorB);
    const int bg_r = to_u8(kAlrVkDrawBgR), bg_g = to_u8(kAlrVkDrawBgG),
              bg_b = to_u8(kAlrVkDrawBgB);
    bool draw_ok = false, not_bg = false;
    if (sub) {
        const int dr = std::abs(static_cast<int>(sub->px[0]) - want_r);
        const int dg = std::abs(static_cast<int>(sub->px[1]) - want_g);
        const int db = std::abs(static_cast<int>(sub->px[2]) - want_b);
        draw_ok = dr <= 12 && dg <= 12 && db <= 12;  // UNORM rounding + tile resolve
        // The readback must NOT be the clear background — that is the breadth proof.
        const int er = std::abs(static_cast<int>(sub->px[0]) - bg_r);
        const int eg = std::abs(static_cast<int>(sub->px[1]) - bg_g);
        const int eb = std::abs(static_cast<int>(sub->px[2]) - bg_b);
        not_bg = (er + eg + eb) > 24;
    }

    pass = decode_ok && reply_ok && transport_ok && dev_ok && submit_ok && draw_ok && not_bg;

    out << "ALR VK DRAW MARSHAL: " << (pass ? "PASS" : "FAIL");
    out << "\nalr vk draw mode=" << mode;
    out << "\nalr vk draw request bytes=" << request.size() << " reply bytes="
        << reply_bytes.size();
    out << "\nalr vk draw transport=" << (transport_ok ? "ok" : "bad")
        << " (req drained=" << req_got << " rep drained=" << rep_got << ")";
    out << "\nalr vk draw ops decoded=" << st.decoded;
    out << "\nalr vk draw device created=" << (dev_ok ? "yes" : "no");
    if (dev_ok)
        out << "\nalr vk draw gfx queue family=" << decoded.devices[0].gfx_family;
    if (sub) {
        out << "\nalr vk draw submit result=" << (sub->submit_result == 0 ? "VK_SUCCESS" : "fail");
        out << "\nalr vk draw render result=" << sub->render_result
            << " (0=OK 1=no-dev 2=target 3=record 4=submit 5=readback 6=no-clear 7=pipeline)";
        out << "\nalr vk draw center px=" << static_cast<int>(sub->px[0]) << ","
            << static_cast<int>(sub->px[1]) << "," << static_cast<int>(sub->px[2]) << ","
            << static_cast<int>(sub->px[3]) << " (expect ~" << want_r << "," << want_g << ","
            << want_b << " triangle)";
        out << "\nalr vk draw bg was=" << bg_r << "," << bg_g << "," << bg_b
            << " (center must DIFFER from bg)";
        out << "\nalr vk draw triangle match=" << (draw_ok ? "yes" : "no")
            << " not-background=" << (not_bg ? "yes" : "no");
    } else {
        out << "\nalr vk draw submit=absent";
    }
    out << "\nalr vk draw spirv vert words=" << (sizeof(kAlrVkTriVertSpv) / 4)
        << " frag words=" << (sizeof(kAlrVkTriFragSpv) / 4) << " (handcrafted, host-embedded)";
    out << "\nalr vk draw path=guest-encode(device/queue/pool/cmd/DRAW/submit) -> ring"
           " -> host-decode(vendor libvulkan: shader-modules+pipeline+vertex-buffer+"
           "vkCmdDraw into AHB) -> readback -> reply -> ring -> guest-decode";
    return out.str();
}

// Host wire mode: the synthetic provider "rasterizes" the fixed-color triangle (returns
// the triangle color as the readback pixel), so the full device/queue/cmd/DRAW/submit
// round trip is verifiable on any host with NO Vulkan SDK / no GPU.
inline std::string run_vk_draw_wire_probe() {
    SyntheticMaliProvider syn;
    VkProvider prov = syn.as_provider();
    bool pass = false;
    return vk_draw_roundtrip(&prov, "wire-synthetic", pass);
}

#ifdef ALR_VK_DECODE_REAL
// Device mode: real vendor Mali libvulkan builds a graphics pipeline (handcrafted SPIR-V),
// uploads a vertex buffer, and issues a real vkCmdDraw of a triangle into an AHB-backed
// COLOR_ATTACHMENT, then reads it back. DEVICE-REQ verification entry point — WS-1 wires
// this behind a JNI nativeAlrGpuVkDrawProbe and greps "ALR VK DRAW MARSHAL: PASS".
inline std::string run_vk_draw_mali_probe() {
    bool pass = false;
    return vk_draw_roundtrip(/*provider=*/nullptr, "mali-libvulkan", pass);
}
#endif

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_MARSHAL_PROBE_HPP
