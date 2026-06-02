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

    if (failures == 0) {
        printf("native_vk_marshal_test: ALL PASS (vk enumerate/props marshalling round trip)\n");
        return 0;
    }
    printf("native_vk_marshal_test: %d FAILURE(S)\n", failures);
    return 1;
}
