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
    VkProvider as_provider() {
        VkProvider p{};
        p.create_instance = &create_instance;
        p.enumerate = &enumerate;
        p.props = &props;
        p.destroy_instance = &destroy_instance;
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

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_MARSHAL_PROBE_HPP
