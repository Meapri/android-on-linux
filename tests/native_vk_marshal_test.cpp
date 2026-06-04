// Host wire test for the Vulkan enumerate/props marshalling path (VK-M2 first step).
//
// NO Vulkan SDK required: the synthetic Mali provider answers enumerate/props, so this
// proves the REQUEST -> ring -> host-decode -> REPLY -> ring -> guest-decode round trip
// (the alr_gpu_vk_proto.hpp wire + alr_gpu_vk_decode.hpp codec) entirely on the host.
// Mirrors tests/native_gpu_ring_test.cpp for the GLES ring. Built + run by
// scripts/test-native-core.sh (the same gate the GLES ring test rides).

#include "alr_gpu/alr_gpu_vk_decode.hpp"
#include "alr_gpu/alr_gpu_vk_marshal_probe.hpp"
#include "alr_gpu/alr_gpu_vk_proto.hpp"

#include <cstdio>
#include <cstdlib>  // std::abs (clear-readback tolerance)
#include <cstring>
#include <string>
#include <vector>

using namespace alr::gpu;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("FAIL %s\n", what); ++failures; }
}

int main() {
    // 1) Direct encode/decode of the request stream (no ring): the host decoder
    //    dispatches every op and the synthetic provider answers; the reply decodes
    //    back to the expected instance/enumerate/props records.
    {
        const std::vector<uint8_t> req = build_vk_enum_request(/*vinst=*/1,
                                                               /*vphys_base=*/100);
        check(!req.empty(), "request encodes non-empty");

        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(req.data(), req.size(), st, reply, &prov);
        check(ok, "decode_vk_batch ok");
        check(st.decoded == 4, "4 request ops decoded (create/enum/props/destroy)");

        VkDecodedReply dr;
        const bool rok = decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr);
        check(rok, "reply decodes");
        check(dr.instances.size() == 1 && dr.instances[0].result == 0, "instance VK_SUCCESS");
        check(dr.enumerations.size() == 1 && dr.enumerations[0].count == 1, "1 device enumerated");
        check(dr.enumerations[0].vphys_base == 100, "vphys_base preserved");
        auto it = dr.props.find(100);
        check(it != dr.props.end(), "props for first device present");
        if (it != dr.props.end()) {
            check(it->second.device_name == "Mali-G615 (synthetic-wire)", "device name round-trips");
            check(!it->second.is_software, "device is hardware");
            check(it->second.queue_families.size() == 1, "one queue family");
            check((it->second.queue_families[0].flags & 0x1u) != 0, "graphics queue bit set");
            check((it->second.api_version >> 22) == 1 &&
                  ((it->second.api_version >> 12) & 0x3ff) == 3, "api 1.3 round-trips");
        }
    }

    // 2) Malformed stream: a truncated CREATE_INSTANCE (opcode but no operands) must
    //    fail-stop, not over-read.
    {
        uint8_t bad[1] = {(uint8_t)ALR_VK_OP_CREATE_INSTANCE};
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(bad, sizeof(bad), st, reply, nullptr);
        check(!ok, "truncated op fails cleanly");
    }

    // 3) Unknown opcode fail-stops (never silently accept an off-contract op).
    {
        uint8_t bad[1] = {199};  // below the VK op range, not a defined op
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(bad, sizeof(bad), st, reply, nullptr);
        check(!ok, "unknown opcode fails cleanly");
    }

    // 4) Full transport round trip through the SPSC ring (the actual wire path).
    {
        const std::string report = run_vk_marshal_wire_probe();
        const bool pass = report.find("ALR VK ENUM MARSHAL: PASS") != std::string::npos;
        check(pass, "ring round-trip probe PASS");
        if (!pass) printf("---- probe report ----\n%s\n", report.c_str());
    }

    // 5) Encoder overflow is detected (a tiny buffer can't hold the request).
    {
        uint8_t tiny[3];
        AlrVkEncoder e;
        alr_vk_enc_init(&e, tiny, sizeof(tiny));
        alr_vk_enc_create_instance(&e, 1, kAlrVkApi13);  // needs 9 bytes
        check(e.overflow == 1, "encoder flags overflow on short buffer");
    }

    // ---- VK-M2 BODY: device + queue + command-buffer + clear-submit marshalling ----

    // 6) Direct encode/decode of the FULL render request stream: every op dispatches,
    //    the synthetic provider creates a device + "renders" the clear, and the reply
    //    decodes back to the device + submit records with the clear color preserved.
    {
        const std::vector<uint8_t> req = build_vk_render_request();
        check(!req.empty(), "render request encodes non-empty");

        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(req.data(), req.size(), st, reply, &prov);
        check(ok, "render decode_vk_batch ok");
        // create/enum/props/createDev/getQueue/createPool/allocCmd/clear/submit/destroyDev/destroyInst = 11
        check(st.decoded == 11, "11 render request ops decoded");
        check(syn.device_created == false, "device torn down by destroy_device");

        VkDecodedReply dr;
        const bool rok = decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr);
        check(rok, "render reply decodes");
        check(dr.devices.size() == 1 && dr.devices[0].result == 0, "device VK_SUCCESS");
        check(dr.devices[0].vdev == 10, "vdev round-trips");
        check(dr.submits.size() == 1, "one submit reply");
        if (dr.submits.size() == 1) {
            const auto& s = dr.submits[0];
            check(s.vcmd == 40, "vcmd round-trips");
            check(s.submit_result == 0, "submit VK_SUCCESS");
            check(s.render_result == ALR_VK_RENDER_OK, "clear rendered OK");
            // synthetic provider returns the clear color as the center pixel.
            const int want_g = (int)(kAlrVkClearG * 255.0f + 0.5f);
            check(std::abs((int)s.px[1] - want_g) <= 1, "clear green round-trips through readback");
        }
    }

    // 7) Submit WITHOUT a prior CMD_BEGIN_CLEAR must report NO_CLEAR_RECORDED (not crash).
    {
        std::vector<uint8_t> buf(64, 0);
        AlrVkEncoder e;
        alr_vk_enc_init(&e, buf.data(), buf.size());
        alr_vk_enc_queue_submit(&e, /*vdev=*/10, /*vqueue=*/20, /*vcmd=*/40);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        buf.resize(e.len);

        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(buf.data(), buf.size(), st, reply, &prov);
        check(ok, "bare submit decodes");
        VkDecodedReply dr;
        check(decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr), "bare submit reply decodes");
        check(dr.submits.size() == 1 && dr.submits[0].render_result == ALR_VK_RENDER_NO_CLEAR_RECORDED,
              "submit with no recorded clear is flagged");
    }

    // 8) Truncated CMD_BEGIN_CLEAR (opcode + partial operands) must fail-stop, not over-read.
    {
        uint8_t bad[5] = {(uint8_t)ALR_VK_OP_CMD_BEGIN_CLEAR, 1, 0, 0, 0};  // vdev only
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(bad, sizeof(bad), st, reply, nullptr);
        check(!ok, "truncated clear op fails cleanly");
    }

    // 9) Full render transport round trip through the SPSC ring (the actual wire path).
    {
        const std::string report = run_vk_render_wire_probe();
        const bool pass = report.find("ALR VK RENDER MARSHAL: PASS") != std::string::npos;
        check(pass, "render ring round-trip probe PASS");
        if (!pass) printf("---- render probe report ----\n%s\n", report.c_str());
    }

    // ---- VK-M3 BODY: graphics pipeline + vertex buffer + vkCmdDraw (DRAW breadth) ----

    // 10) Direct encode/decode of the FULL draw request stream: every op dispatches, the
    //     synthetic provider "rasterizes" the fixed-color triangle, and the reply decodes
    //     back to the device + submit records with the TRIANGLE color (not the clear bg).
    {
        const std::vector<uint8_t> req = build_vk_draw_request();
        check(!req.empty(), "draw request encodes non-empty");

        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(req.data(), req.size(), st, reply, &prov);
        check(ok, "draw decode_vk_batch ok");
        // create/enum/props/createDev/getQueue/createPool/allocCmd/DRAW/submit/destroyDev/destroyInst = 11
        check(st.decoded == 11, "11 draw request ops decoded");

        VkDecodedReply dr;
        const bool rok = decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr);
        check(rok, "draw reply decodes");
        check(dr.devices.size() == 1 && dr.devices[0].result == 0, "draw device VK_SUCCESS");
        check(dr.submits.size() == 1, "one draw submit reply");
        if (dr.submits.size() == 1) {
            const auto& s = dr.submits[0];
            check(s.submit_result == 0, "draw submit VK_SUCCESS");
            check(s.render_result == ALR_VK_RENDER_OK, "draw rendered OK");
            const int want_r = (int)(kAlrVkTriColorR * 255.0f + 0.5f);
            const int want_g = (int)(kAlrVkTriColorG * 255.0f + 0.5f);
            const int want_b = (int)(kAlrVkTriColorB * 255.0f + 0.5f);
            check(std::abs((int)s.px[0] - want_r) <= 1 &&
                  std::abs((int)s.px[1] - want_g) <= 1 &&
                  std::abs((int)s.px[2] - want_b) <= 1, "triangle color round-trips through readback");
            // breadth proof: the draw readback differs from the clear background.
            const int bg_g = (int)(kAlrVkDrawBgG * 255.0f + 0.5f);
            check(std::abs((int)s.px[1] - bg_g) > 8, "draw center pixel is NOT the clear background");
        }
    }

    // 11) The embedded handcrafted SPIR-V is well-formed (magic + non-empty + word-aligned).
    {
        check((sizeof(kAlrVkTriVertSpv) % 4) == 0, "vert SPIR-V is word-aligned");
        check((sizeof(kAlrVkTriFragSpv) % 4) == 0, "frag SPIR-V is word-aligned");
        check(kAlrVkTriVertSpv[0] == 0x07230203u, "vert SPIR-V magic");
        check(kAlrVkTriFragSpv[0] == 0x07230203u, "frag SPIR-V magic");
        check(sizeof(kAlrVkTriVertSpv) >= 32 && sizeof(kAlrVkTriFragSpv) >= 32,
              "SPIR-V modules non-trivial");
    }

    // 12) A DRAW followed by submit must dispatch the draw seam, NOT clear_submit. Use a
    //     provider with clear_submit returning the bg and draw_submit the triangle, then
    //     assert the reply is the triangle (proves QUEUE_SUBMIT honored is_draw).
    {
        const std::vector<uint8_t> req = build_vk_draw_request();
        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        VkReplyEncoder reply;
        check(decode_vk_batch(req.data(), req.size(), st, reply, &prov), "draw-dispatch decodes");
        VkDecodedReply dr;
        check(decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr), "draw-dispatch reply decodes");
        const int want_g = (int)(kAlrVkTriColorG * 255.0f + 0.5f);
        check(dr.submits.size() == 1 && std::abs((int)dr.submits[0].px[1] - want_g) <= 1,
              "is_draw routed to draw_submit (triangle color), not clear_submit (bg)");
    }

    // 13) Full DRAW transport round trip through the SPSC ring (the actual wire path).
    {
        const std::string report = run_vk_draw_wire_probe();
        const bool pass = report.find("ALR VK DRAW MARSHAL: PASS") != std::string::npos;
        check(pass, "draw ring round-trip probe PASS");
        if (!pass) printf("---- draw probe report ----\n%s\n", report.c_str());
    }

    // 14) VK-M4 PRESENT rung: GUEST SPIR-V over the wire (CREATE_SHADER_MODULE) + an
    //     AHB-backed swapchain (CREATE_SWAPCHAIN/ACQUIRE) + QUEUE_PRESENT. The synthetic
    //     provider accepts the SPIR-V (magic check), serves a 2-image swapchain, rotates
    //     acquire, and "presents" the triangle color. Asserts the new reply records
    //     decode and the guest's shader color survives to the presented center pixel.
    {
        // A tiny but VALID SPIR-V header (magic + version + a couple words) — enough for
        // the provider's magic check (the real path vkCreateShaderModule's it on Mali).
        const uint32_t fake_spv[] = {0x07230203u, 0x00010300u, 0x0u, 0x1u};
        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;

        // Batch A: device bring-up + shader uploads + swapchain + acquire.
        std::vector<uint8_t> a(512);
        AlrVkEncoder e; alr_vk_enc_init(&e, a.data(), (uint32_t)a.size());
        alr_vk_enc_create_instance(&e, 1, kAlrVkApi13);
        alr_vk_enc_enumerate_phys(&e, 1, 100);
        alr_vk_enc_create_device(&e, 1, 100, 10);
        alr_vk_enc_get_device_queue(&e, 10, 0, 20);
        alr_vk_enc_create_command_pool(&e, 10, 30);
        alr_vk_enc_allocate_command_buffers(&e, 10, 30, 40);
        alr_vk_enc_create_shader_module(&e, 10, 50, 0x1u /*VERTEX*/,
                                        fake_spv, (uint32_t)sizeof(fake_spv));
        alr_vk_enc_create_shader_module(&e, 10, 51, 0x10u /*FRAGMENT*/,
                                        fake_spv, (uint32_t)sizeof(fake_spv));
        alr_vk_enc_create_swapchain(&e, 10, 60, 64, 64, 2);
        alr_vk_enc_acquire_next_image(&e, 10, 60);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        VkReplyEncoder ra;
        check(decode_vk_batch(a.data(), e.len, st, ra, &prov), "present batch-A decodes");
        VkDecodedReply da;
        check(decode_vk_reply(ra.bytes().data(), ra.bytes().size(), da), "present batch-A reply decodes");
        check(da.shaders.size() == 2 && da.shaders[0].result == 0 && da.shaders[1].result == 0,
              "2 guest shader modules created (SPIR-V over the wire)");
        check(da.swapchains.size() == 1 && da.swapchains[0].result == 0 &&
              da.swapchains[0].image_count == 2, "swapchain created with 2 images");
        check(da.acquires.size() == 1 && da.acquires[0].result == 0, "acquire returned an index");
        const uint32_t img = da.acquires.empty() ? 0 : da.acquires[0].image_index;

        // Batch B: record the guest-shader draw into the acquired image + present.
        std::vector<uint8_t> b(128);
        AlrVkEncoder e2; alr_vk_enc_init(&e2, b.data(), (uint32_t)b.size());
        alr_vk_enc_cmd_begin_draw_modules(&e2, 10, 40, 60, img, 50, 51, 64, 64,
                                          0.0f, 0.0f, 0.0f, 1.0f);
        alr_vk_enc_queue_present(&e2, 10, 20, 40, 60, img);
        alr_vk_enc_u8(&e2, (uint8_t)ALR_VK_OP_END);
        VkReplyEncoder rb;
        check(decode_vk_batch(b.data(), e2.len, st, rb, &prov), "present batch-B decodes");
        VkDecodedReply db;
        check(decode_vk_reply(rb.bytes().data(), rb.bytes().size(), db), "present batch-B reply decodes");
        check(db.presents.size() == 1, "one present reply");
        if (db.presents.size() == 1) {
            const auto& pr = db.presents[0];
            check(pr.render_result == ALR_VK_RENDER_OK, "present render OK");
            check(pr.presented == 1, "present routed to sink");
            const int want_r = (int)(kAlrVkTriColorR * 255.0f + 0.5f);
            check(std::abs((int)pr.px[0] - want_r) <= 1,
                  "guest frag-shader color survives to the presented center pixel");
        }
        check(syn.present_count == 1, "synthetic device presented exactly once");
    }

    // 15) ANGLE-init rung: vkGetPhysicalDeviceImageFormatProperties forward. The new op
    //     round-trips a (format,type,tiling,usage,flags) tuple to the host, which (here,
    //     synthetically; on device, real Mali) returns a VkImageFormatProperties + result.
    //     This is the op the ICD's image-format entry point marshals — the entry point whose
    //     ABSENCE made the Khronos loader reject the ICD with -9. Asserts the reply record
    //     decodes with the synthetic device's supported verdict + max extent.
    {
        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        // First create instance + enumerate so vphys 100 is a known device id.
        std::vector<uint8_t> a(256);
        AlrVkEncoder e; alr_vk_enc_init(&e, a.data(), (uint32_t)a.size());
        alr_vk_enc_create_instance(&e, 1, kAlrVkApi13);
        alr_vk_enc_enumerate_phys(&e, 1, 100);
        // R8G8B8A8_UNORM (37), 2D (0), OPTIMAL (0), COLOR_ATTACHMENT (0x10), no flags.
        alr_vk_enc_get_phys_image_format_props(&e, 1, 100, 37, 0, 0, 0x10u, 0);
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        VkReplyEncoder reply;
        check(decode_vk_batch(a.data(), e.len, st, reply, &prov), "image-format batch decodes");
        VkDecodedReply dr;
        check(decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr),
              "image-format reply decodes");
        check(dr.image_format_props.size() == 1, "one image-format-props reply record");
        if (dr.image_format_props.size() == 1) {
            const auto& ip = dr.image_format_props[0];
            check(ip.vphys == 100, "image-format vphys round-trips");
            check(ip.result == 0, "image-format result is VK_SUCCESS (supported)");
            check(ip.max_extent_w >= 1 && ip.max_extent_h >= 1, "image-format max extent set");
            check(ip.max_resource_size > 0, "image-format max resource size set");
        }
    }

    // 16) ANGLE-init rung: a truncated GET_PHYS_IMAGE_FORMAT_PROPS (opcode + partial
    //     operands) must fail-stop, not over-read.
    {
        uint8_t bad[5] = {(uint8_t)ALR_VK_OP_GET_PHYS_IMAGE_FORMAT_PROPS, 1, 0, 0, 0};  // vinst only
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(bad, sizeof(bad), st, reply, nullptr);
        check(!ok, "truncated image-format op fails cleanly");
    }

    // 17) ANGLE-init rung: GET_PHYS_FORMAT_PROPS round-trips the REAL per-format feature
    //     masks (the op that replaces the guest's synthetic all-bits answer). The synthetic
    //     provider returns a realistic color-renderable RGBA mask; assert it survives the
    //     wire verbatim (NOT all-bits, so the test proves real Mali bits — not 0x7FFFFFFF —
    //     flow back), and that the buffer feature carries VERTEX_BUFFER.
    {
        SyntheticMaliProvider syn;
        VkProvider prov = syn.as_provider();
        VkDecodeState st;
        std::vector<uint8_t> a(256);
        AlrVkEncoder e; alr_vk_enc_init(&e, a.data(), (uint32_t)a.size());
        alr_vk_enc_create_instance(&e, 1, kAlrVkApi13);
        alr_vk_enc_enumerate_phys(&e, 1, 100);
        alr_vk_enc_get_phys_format_props(&e, 1, 100, 37);  // R8G8B8A8_UNORM (37)
        alr_vk_enc_u8(&e, (uint8_t)ALR_VK_OP_END);
        VkReplyEncoder reply;
        check(decode_vk_batch(a.data(), e.len, st, reply, &prov), "format-props batch decodes");
        VkDecodedReply dr;
        check(decode_vk_reply(reply.bytes().data(), reply.bytes().size(), dr),
              "format-props reply decodes");
        check(dr.format_props.size() == 1, "one format-props reply record");
        if (dr.format_props.size() == 1) {
            const auto& fp = dr.format_props[0];
            check(fp.vphys == 100, "format-props vphys round-trips");
            check(fp.optimal_tiling_features != 0x7FFFFFFFu &&
                  fp.optimal_tiling_features != 0, "optimal feature mask is REAL (not all-bits/0)");
            check((fp.optimal_tiling_features & 0x80u) != 0, "COLOR_ATTACHMENT bit survives");
            check((fp.buffer_features & 0x40u) != 0, "VERTEX_BUFFER bit survives");
        }
    }

    // 18) ANGLE-init rung: a truncated GET_PHYS_FORMAT_PROPS (opcode + partial operands)
    //     must fail-stop, not over-read.
    {
        uint8_t bad[5] = {(uint8_t)ALR_VK_OP_GET_PHYS_FORMAT_PROPS, 1, 0, 0, 0};  // vinst only
        VkDecodeState st;
        VkReplyEncoder reply;
        const bool ok = decode_vk_batch(bad, sizeof(bad), st, reply, nullptr);
        check(!ok, "truncated format-props op fails cleanly");
    }

    if (failures == 0) {
        printf("native_vk_marshal_test: ALL PASS (vk enumerate/props + clear-submit + "
               "DRAW(pipeline/vbuf/vkCmdDraw) + PRESENT(guest SPIR-V/swapchain) + "
               "ANGLE-init image-format + format-props marshalling)\n");
        return 0;
    }
    printf("native_vk_marshal_test: %d FAILURE(S)\n", failures);
    return 1;
}
