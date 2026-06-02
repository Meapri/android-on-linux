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

    if (failures == 0) {
        printf("native_vk_marshal_test: ALL PASS (vk enumerate/props + device/queue/cmd/clear-submit marshalling)\n");
        return 0;
    }
    printf("native_vk_marshal_test: %d FAILURE(S)\n", failures);
    return 1;
}
