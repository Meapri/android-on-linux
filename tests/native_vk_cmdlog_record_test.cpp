// native_vk_cmdlog_record_test.cpp — prove the GUEST ICD recorder (alr_icd_cmd_record.h, the
// C twin used by alr_icd_cmd_entrypoints.inc) emits a command-buffer log byte-for-byte
// compatible with the HOST decoder (alr_gpu_vk_cmdlog.hpp::cmd_decode_one). If the field
// order / sizes drift between the guest record helpers and the host replay reader, this test
// fails — which is exactly the regression the wave-8 wiring must never introduce (a drifted
// log would make the host replay garbage onto Mali).
//
// It records a representative ANGLE-style triangle pass (begin-render-pass + set viewport +
// set scissor + bind pipeline + bind vertex buffers + draw + end-render-pass) with the SAME
// recorder calls the ICD entrypoints make, then walks the log with the host cmd_decode_one
// and asserts the opcode sequence + the operands the host echoes.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// The guest recorder is pure C (extern "C" guarded). It needs AlrVkReader (gen glue) +
// AlrVkEncoder (proto) in scope first — the same order alr_icd_vulkan.c uses.
#include "alr_gpu/guest_icd/alr_icd_gen_glue.h"
#include "alr_gpu/alr_gpu_vk_proto.hpp"
#include "alr_gpu/guest_icd/alr_icd_cmd_record.h"

// The host decoder (the source of truth for the wire).
#include "alr_gpu/alr_gpu_vk_cmdlog.hpp"

using namespace alr::gpu;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail = 1; } } while (0)

// The decoded record stream the host produces (one entry per cmd_decode_one emit).
struct Rec { uint16_t op; uint32_t h0, h1; uint64_t a, b; };
static std::vector<Rec> g_recs;
static void sink_on_cmd(void* /*ctx*/, uint16_t op, uint32_t h0, uint32_t h1, uint64_t a,
                        uint64_t b) {
    g_recs.push_back(Rec{op, h0, h1, a, b});
}

int main() {
    // ---- 1) RECORD a triangle pass with the guest C recorder (exactly the helper calls the
    // ICD's vkCmd* entrypoints make; the inline-encoded ops mirror alr_icd_cmd_entrypoints.inc). ----
    AlrIcdCmdLog L;
    alr_icd_cmdlog_init(&L);
    alr_icd_cmdlog_reset(&L);

    // vkCmdBeginRenderPass(rp=0x11, fb=0x22, area (0,0,1920,1080), contents=0, 1 clear value)
    alr_icd_cmd_begin_render_pass(&L, 0x11u, 0x22u, 0, 0, 1920u, 1080u, 0u, 1u);
    uint8_t clear[16];
    for (int i = 0; i < 16; ++i) clear[i] = (uint8_t)(i + 1);
    alr_icd_cmdlog_raw(&L, clear, 16);  // the raw VkClearValue

    // vkCmdSetViewport(first=0, count=1, {0,0,1920,1080,0,1})  (inline, as the .inc records it)
    alr_icd_cmdlog_cmd(&L, ALR_ICD_CMD_SET_VIEWPORT);
    alr_icd_cmdlog_u32(&L, 0u); alr_icd_cmdlog_u32(&L, 1u);
    alr_icd_cmdlog_f32(&L, 0.f); alr_icd_cmdlog_f32(&L, 0.f);
    alr_icd_cmdlog_f32(&L, 1920.f); alr_icd_cmdlog_f32(&L, 1080.f);
    alr_icd_cmdlog_f32(&L, 0.f); alr_icd_cmdlog_f32(&L, 1.f);

    // vkCmdSetScissor(first=0, count=1, {(0,0),(1920,1080)})
    alr_icd_cmdlog_cmd(&L, ALR_ICD_CMD_SET_SCISSOR);
    alr_icd_cmdlog_u32(&L, 0u); alr_icd_cmdlog_u32(&L, 1u);
    alr_icd_cmdlog_i32(&L, 0); alr_icd_cmdlog_i32(&L, 0);
    alr_icd_cmdlog_u32(&L, 1920u); alr_icd_cmdlog_u32(&L, 1080u);

    // vkCmdBindPipeline(GRAPHICS=0, pipeline=0x77)
    alr_icd_cmd_bind_pipeline(&L, 0u, 0x77u);

    // vkCmdBindVertexBuffers(first=0, count=1, {buf=0x88, off=0})
    alr_icd_cmd_bind_vertex_buffers_begin(&L, 0u, 1u);
    alr_icd_cmd_bind_vertex_buffer_entry(&L, 0x88u, 0u);

    // vkCmdDraw(3,1,0,0)
    alr_icd_cmd_draw(&L, 3u, 1u, 0u, 0u);

    // vkCmdEndRenderPass
    alr_icd_cmd_end_render_pass(&L);

    uint32_t total = alr_icd_cmdlog_end(&L);   // appends ALR_VK_CMD_END
    CHECK(!L.overflow);
    CHECK(total == L.len);
    CHECK(total > 0);

    // ---- 2) DECODE with the host reader; assert the sequence + operands. ----
    VkCmdReplaySink sink; sink.on_cmd = sink_on_cmd; sink.ctx = nullptr;
    int n = cmd_replay_log_wire(L.buf, L.len, sink);
    CHECK(n == 7);                              // 7 records (END is the terminator, not a record)
    CHECK(g_recs.size() == 7u);

    if (g_recs.size() == 7u) {
        // begin render pass: h0=vrp, h1=vfb, a=contents, b=clear_count
        CHECK(g_recs[0].op == ALR_VK_CMD_BEGIN_RENDER_PASS);
        CHECK(g_recs[0].h0 == 0x11u);
        CHECK(g_recs[0].h1 == 0x22u);
        CHECK(g_recs[0].a == 0u);              // contents
        CHECK(g_recs[0].b == 1u);              // clear_count
        // set viewport: a=firstViewport, b=count
        CHECK(g_recs[1].op == ALR_VK_CMD_SET_VIEWPORT);
        CHECK(g_recs[1].a == 0u);
        CHECK(g_recs[1].b == 1u);
        // set scissor: a=firstScissor, b=count
        CHECK(g_recs[2].op == ALR_VK_CMD_SET_SCISSOR);
        CHECK(g_recs[2].a == 0u);
        CHECK(g_recs[2].b == 1u);
        // bind pipeline: h0=vpipeline, a=bindPoint
        CHECK(g_recs[3].op == ALR_VK_CMD_BIND_PIPELINE);
        CHECK(g_recs[3].h0 == 0x77u);
        CHECK(g_recs[3].a == 0u);
        // bind vertex buffers: h0=first vbuffer, a=firstBinding, b=count
        CHECK(g_recs[4].op == ALR_VK_CMD_BIND_VERTEX_BUFFERS);
        CHECK(g_recs[4].h0 == 0x88u);
        CHECK(g_recs[4].a == 0u);
        CHECK(g_recs[4].b == 1u);
        // draw: a = (vtx<<32)|inst, b = (firstVtx<<32)|firstInst
        CHECK(g_recs[5].op == ALR_VK_CMD_DRAW);
        CHECK(g_recs[5].a == ((uint64_t)3u << 32 | 1u));
        CHECK(g_recs[5].b == 0u);
        // end render pass
        CHECK(g_recs[6].op == ALR_VK_CMD_END_RENDER_PASS);
    }

    // ---- 3) A draw-indexed + bind-index variant (covers the i32 vertexOffset packing). ----
    g_recs.clear();
    alr_icd_cmdlog_reset(&L);
    alr_icd_cmd_bind_index_buffer(&L, 0x99u, 64u, 1u /*UINT32*/);
    alr_icd_cmd_draw_indexed(&L, 6u, 2u, 1u, -3 /*vertexOffset*/, 4u);
    (void)alr_icd_cmdlog_end(&L);
    n = cmd_replay_log_wire(L.buf, L.len, sink);
    CHECK(n == 2);
    if (g_recs.size() == 2u) {
        CHECK(g_recs[0].op == ALR_VK_CMD_BIND_INDEX_BUFFER);
        CHECK(g_recs[0].h0 == 0x99u);
        CHECK(g_recs[0].a == 64u);             // offset
        CHECK(g_recs[0].b == 1u);              // indexType
        CHECK(g_recs[1].op == ALR_VK_CMD_DRAW_INDEXED);
        CHECK(g_recs[1].a == ((uint64_t)6u << 32 | 2u));  // (indexCount<<32)|instanceCount
        // b = (uint32_t(vertexOffset)<<32)|firstInstance ; vertexOffset=-3 -> 0xfffffffd
        CHECK(g_recs[1].b == ((uint64_t)(uint32_t)(-3) << 32 | 4u));
    }

    alr_icd_cmdlog_free(&L);
    if (g_fail) { std::printf("native_vk_cmdlog_record_test: FAILED\n"); return 1; }
    std::printf("native_vk_cmdlog_record_test: OK (guest recorder <-> host decoder agree)\n");
    return 0;
}
