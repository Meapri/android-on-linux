// Host wire test for the Vulkan CMD-LOG recording band (the vkCmd* command-recording
// mechanism + vkQueueSubmit/sync). NO Vulkan SDK required.
//
// THE HARD PART OF GPU FULL-PASSTHROUGH. vkCmd* calls do NOT execute — they RECORD into a
// command buffer; the GPU work happens at vkQueueSubmit. This test proves, on the host:
//   (1) the GUEST-LOCAL RECORDER: a sequence of vkCmd* records (begin-renderpass + bind +
//       draw + end, plus a transfer + barrier + dynamic-state + dispatch) encodes into a
//       per-command-buffer byte log in the shared arena — NO ring op per call;
//   (2) the HOST REPLAY round trip: the recorded log decodes back into the SAME opcodes +
//       handles + scalar operands, in order (cmd_replay_log_wire -> a sink) — i.e. when
//       the host replays the log into a real Mali VkCommandBuffer, it sees exactly what the
//       guest recorded. (The real-Mali leg, cmd_replay_log_real, is syntax-verified against
//       the NDK vulkan.h by the pytest gate; here we prove the byte layout + handle round
//       trip with no GPU.)
//   (3) the SUBMIT RING op: vkQueueSubmit ships the submit topology (queue + cmd-buffer vid
//       list + wait/signal semaphores + fence) over the wire, found + decoded by the host
//       cmd dispatcher straight from the shared arena (the recorded BYTES never cross the
//       ring) — driven through the SAME decode_vk_batch escape hand-off the create-forwards
//       codegen uses, on a DISJOINT sub-op band (0x4000) so the two bands never collide;
//   (4) the SYNC ops: create/wait/reset fences, create semaphores, queue/device wait-idle
//       round-trip their VkResults;
//   (5) the BAND BOUNDARY: a create-forwards sub-op (low band) and a cmd-log sub-op (0x4000)
//       in the SAME batch each reach their own dispatcher (no cross-talk), and a malformed
//       cmd-log op fail-stops.
//
// Built + run by scripts/test-native-core.sh alongside native_vk_gen_passthrough_test.cpp.

#include "alr_gpu/alr_gpu_vk_cmd_dispatch.hpp"
#include "alr_gpu/alr_gpu_vk_cmdlog.hpp"
#include "alr_gpu/alr_gpu_vk_decode.hpp"
#include "alr_gpu/generated/alr_gpu_vk_arena.hpp"
#include "alr_gpu/generated/alr_gpu_vk_gen_decode.hpp"   // create-forwards band (coexistence)
#include "alr_gpu/generated/alr_gpu_vk_gen_proto.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace alr::gpu;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { printf("FAIL %s\n", what); ++failures; }
}

// ---- A recording sink that captures every decoded cmd-record so the test asserts the
// opcode + handle sequence the host would replay. ----
struct CapturedCmd {
    uint16_t op;
    uint32_t h0, h1;
    uint64_t a, b;
};
struct CmdCapture {
    std::vector<CapturedCmd> cmds;
    static void on_cmd(void* ctx, uint16_t op, uint32_t h0, uint32_t h1, uint64_t a, uint64_t b) {
        static_cast<CmdCapture*>(ctx)->cmds.push_back({op, h0, h1, a, b});
    }
    VkCmdReplaySink sink() { VkCmdReplaySink s; s.on_cmd = &on_cmd; s.ctx = this; return s; }
};

// ---- A synthetic Mali for the cmd-log RING ops (submit + sync). It decodes each submitted
// cmd-buffer's log itself (cmd_replay_log_wire) — exactly the host's real path minus the
// vkCmd* — and records what it saw, so the submit round trip is asserted. ----
struct SynthCmd {
    int submits = 0;
    int total_records = 0;
    int fences_created = 0, fences_waited = 0, fences_reset = 0;
    int sems_created = 0;
    int queue_idle = 0, device_idle = 0;
    uint32_t last_submit_queue = 0, last_submit_fence = 0;
    uint32_t last_wait_sem = 0, last_signal_sem = 0;

    static bool s_submit(void* ctx, const VkCmdSubmitInfo& info, int32_t* vkr, int32_t* rr) {
        auto* s = static_cast<SynthCmd*>(ctx);
        s->submits++;
        s->last_submit_queue = info.vqueue;
        s->last_submit_fence = info.vfence;
        if (!info.waits.empty()) s->last_wait_sem = info.waits[0].first;
        if (!info.signals.empty()) s->last_signal_sem = info.signals[0];
        // Replay-decode each cmd-buffer's arena log (the real path does this then calls the
        // real vkCmd*; here we just count the records to prove the log was found + decodable).
        for (const auto& c : info.cmds) {
            if (c.log && c.log_len) {
                CmdCapture cap;
                const int n = cmd_replay_log_wire(c.log, c.log_len, cap.sink());
                if (n < 0) { *vkr = -1; *rr = ALR_VK_CMD_REPLAY_DECODE; return true; }
                s->total_records += n;
            }
        }
        *vkr = 0; *rr = ALR_VK_CMD_REPLAY_OK;
        return true;
    }
    static int s_create_fence(void* ctx, uint32_t, uint32_t, uint32_t) {
        static_cast<SynthCmd*>(ctx)->fences_created++; return 0;
    }
    static void s_destroy_fence(void*, uint32_t, uint32_t) {}
    static int s_wait_fences(void* ctx, uint32_t, uint32_t, uint64_t,
                             const std::vector<uint32_t>& f) {
        static_cast<SynthCmd*>(ctx)->fences_waited += static_cast<int>(f.size()); return 0;
    }
    static int s_reset_fences(void* ctx, uint32_t, const std::vector<uint32_t>& f) {
        static_cast<SynthCmd*>(ctx)->fences_reset += static_cast<int>(f.size()); return 0;
    }
    static int s_get_fence_status(void*, uint32_t, uint32_t) { return 0; }
    static int s_create_semaphore(void* ctx, uint32_t, uint32_t, uint32_t) {
        static_cast<SynthCmd*>(ctx)->sems_created++; return 0;
    }
    static void s_destroy_semaphore(void*, uint32_t, uint32_t) {}
    static int s_queue_wait_idle(void* ctx, uint32_t) {
        static_cast<SynthCmd*>(ctx)->queue_idle++; return 0;
    }
    static int s_device_wait_idle(void* ctx, uint32_t) {
        static_cast<SynthCmd*>(ctx)->device_idle++; return 0;
    }
    VkCmdProvider as_provider() {
        VkCmdProvider p;
        p.submit = &s_submit;
        p.create_fence = &s_create_fence;
        p.destroy_fence = &s_destroy_fence;
        p.wait_fences = &s_wait_fences;
        p.reset_fences = &s_reset_fences;
        p.get_fence_status = &s_get_fence_status;
        p.create_semaphore = &s_create_semaphore;
        p.destroy_semaphore = &s_destroy_semaphore;
        p.queue_wait_idle = &s_queue_wait_idle;
        p.device_wait_idle = &s_device_wait_idle;
        p.ctx = this;
        return p;
    }
};

// Decode the cmd-log reply stream (escape + u16 sub + payload) for the submit/sync results.
struct CmdReplies {
    std::vector<std::pair<uint32_t, std::pair<int32_t, int32_t>>> submits;  // vqueue -> (vkr, rr)
    std::vector<std::pair<uint32_t, int32_t>> results;                       // key -> result
    bool ok = true;
};
static bool decode_cmd_replies(const std::vector<uint8_t>& bytes, CmdReplies& out) {
    VkReader r(bytes.data(), bytes.size());
    for (;;) {
        uint8_t op = 0;
        if (!r.u8(op)) break;
        if (op == ALR_VK_REPLY_END) break;
        if (op != ALR_VK_REPLY_GEN_ESCAPE) { out.ok = false; return false; }
        uint16_t sub = 0;
        if (!r.u16(sub)) { out.ok = false; return false; }
        if (sub == ALR_VK_CMD_REPLY_SUBMIT) {
            uint32_t vq = 0; int32_t vkr = 0, rr = 0;
            if (!r.u32(vq) || !r.i32(vkr) || !r.i32(rr)) { out.ok = false; return false; }
            out.submits.push_back({vq, {vkr, rr}});
        } else if (sub == ALR_VK_CMD_REPLY_RESULT) {
            uint32_t key = 0; int32_t res = 0;
            if (!r.u32(key) || !r.i32(res)) { out.ok = false; return false; }
            out.results.push_back({key, res});
        } else {
            out.ok = false; return false;  // not a cmd-log reply (would be a create-band one)
        }
    }
    return out.ok;
}

// Virtual ids (disjoint, mirroring the ICD's monotonic pools).
static constexpr uint32_t kVdev = 1000, kVqueue = 2000, kVcmd = 4000;
static constexpr uint32_t kVrp = 3000001, kVfb = 3000002, kVpipe = 3000003,
                          kVlayout = 3000004, kVdescset = 3000005;
static constexpr uint32_t kVbufVtx = 7000001, kVbufIdx = 7000002, kVbufSrc = 7000003,
                          kVbufDst = 7000004, kVimg = 8000001;
static constexpr uint32_t kVfence = 4500001, kVsemWait = 4500002, kVsemSignal = 4500003;

// Record a representative frame's worth of vkCmd* into `rec`: the begin-renderpass + bind +
// draw + end the task names, plus the breadth ops, so the round trip covers them.
static void record_frame(AlrVkCmdRecorder& rec) {
    // begin render pass (1 clear value)
    rec.cmd(ALR_VK_CMD_BEGIN_RENDER_PASS);
    rec.u32(kVrp); rec.u32(kVfb);
    rec.i32(0); rec.i32(0); rec.u32(128); rec.u32(128);     // render area
    rec.u32(0 /*INLINE*/);
    rec.u32(1);                                              // 1 clear value
    { float cv[4] = {0.1f, 0.2f, 0.3f, 1.0f}; rec.bytes(cv, 16); }

    // dynamic state
    rec.cmd(ALR_VK_CMD_SET_VIEWPORT);
    rec.u32(0); rec.u32(1);
    rec.f32(0); rec.f32(0); rec.f32(128); rec.f32(128); rec.f32(0); rec.f32(1);
    rec.cmd(ALR_VK_CMD_SET_SCISSOR);
    rec.u32(0); rec.u32(1);
    rec.i32(0); rec.i32(0); rec.u32(128); rec.u32(128);

    // bind pipeline + descriptor sets + vertex/index buffers
    rec.cmd(ALR_VK_CMD_BIND_PIPELINE);
    rec.u32(0 /*GRAPHICS*/); rec.u32(kVpipe);
    rec.cmd(ALR_VK_CMD_BIND_DESCRIPTOR_SETS);
    rec.u32(0); rec.u32(kVlayout); rec.u32(0);
    rec.u32(1); rec.u32(kVdescset);   // 1 set
    rec.u32(0);                        // 0 dynamic offsets
    rec.cmd(ALR_VK_CMD_BIND_VERTEX_BUFFERS);
    rec.u32(0); rec.u32(1); rec.u32(kVbufVtx); rec.u64(0);
    rec.cmd(ALR_VK_CMD_BIND_INDEX_BUFFER);
    rec.u32(kVbufIdx); rec.u64(0); rec.u32(0 /*UINT16*/);

    // push constants
    rec.cmd(ALR_VK_CMD_PUSH_CONSTANTS);
    rec.u32(kVlayout); rec.u32(0x1 /*VERTEX*/); rec.u32(0); rec.u32(16);
    { float pc[4] = {1, 0, 0, 1}; rec.bytes(pc, 16); }

    // draws
    rec.cmd(ALR_VK_CMD_DRAW);
    rec.u32(3); rec.u32(1); rec.u32(0); rec.u32(0);
    rec.cmd(ALR_VK_CMD_DRAW_INDEXED);
    rec.u32(6); rec.u32(1); rec.u32(0); rec.i32(0); rec.u32(0);

    rec.cmd(ALR_VK_CMD_END_RENDER_PASS);

    // a transfer + barrier outside the render pass
    rec.cmd(ALR_VK_CMD_PIPELINE_BARRIER);
    rec.u32(0x1000 /*TRANSFER*/); rec.u32(0x1000); rec.u32(0);
    rec.u32(0);                                  // 0 memory barriers
    rec.u32(0);                                  // 0 buffer barriers
    rec.u32(1);                                  // 1 image barrier
    rec.u32(0); rec.u32(0x1000); rec.u32(0); rec.u32(7); rec.u32(0); rec.u32(0);
    rec.u32(kVimg); rec.u32(0x1 /*COLOR*/); rec.u32(0); rec.u32(1); rec.u32(0); rec.u32(1);

    rec.cmd(ALR_VK_CMD_COPY_BUFFER);
    rec.u32(kVbufSrc); rec.u32(kVbufDst); rec.u32(1);
    rec.u64(0); rec.u64(0); rec.u64(256);

    rec.cmd(ALR_VK_CMD_COPY_BUFFER_TO_IMAGE);
    rec.u32(kVbufSrc); rec.u32(kVimg); rec.u32(7 /*TRANSFER_DST_OPTIMAL*/); rec.u32(1);
    rec.u64(0); rec.u32(0); rec.u32(0); rec.u32(0x1); rec.u32(0); rec.u32(0); rec.u32(1);
    rec.i32(0); rec.i32(0); rec.i32(0); rec.u32(64); rec.u32(64); rec.u32(1);

    // a compute dispatch
    rec.cmd(ALR_VK_CMD_DISPATCH);
    rec.u32(8); rec.u32(8); rec.u32(1);
}

int main() {
    // The arena: the cmd-log lives here (anonymous mmap on host; memfd on device).
    check(alr_vk_arena_create(8u << 20), "arena create");
    check(vk_cmd_dispatch() != nullptr, "cmd-log dispatcher registered into decode seam");
    check(vk_gen_dispatch() != nullptr, "create-forwards dispatcher also registered (coexist)");

    // ===== 1) GUEST-LOCAL RECORDING into the shared arena (NO ring op) =====
    const uint64_t log_off = alr_vk_arena_alloc(4096);
    check(log_off != kAlrVkArenaNoOffset, "arena slab for the cmd-log");
    uint8_t* log_buf = static_cast<uint8_t*>(alr_vk_arena_ptr(log_off));
    check(log_buf != nullptr, "cmd-log arena pointer resolves");

    AlrVkCmdRecorder rec;
    rec.begin(log_buf, 4096, log_off);
    record_frame(rec);
    const uint32_t log_len = rec.finalize();
    check(!rec.overflow(), "frame records within the arena slab");
    check(log_len > 0, "cmd-log has a non-zero length");

    // ===== 2) HOST REPLAY round trip: the log decodes to the SAME opcodes + handles =====
    CmdCapture cap;
    const int n = cmd_replay_log_wire(log_buf, log_len, cap.sink());
    check(n == 15, "all 15 recorded cmd-records decode back");  // count the records above
    if (cap.cmds.size() >= 15) {
        check(cap.cmds[0].op == ALR_VK_CMD_BEGIN_RENDER_PASS, "record[0] is begin-renderpass");
        check(cap.cmds[0].h0 == kVrp && cap.cmds[0].h1 == kVfb,
              "begin-renderpass carries the renderpass+framebuffer vids");
        check(cap.cmds[3].op == ALR_VK_CMD_BIND_PIPELINE && cap.cmds[3].h0 == kVpipe,
              "bind-pipeline carries the pipeline vid");
        check(cap.cmds[4].op == ALR_VK_CMD_BIND_DESCRIPTOR_SETS && cap.cmds[4].h0 == kVlayout,
              "bind-descriptor-sets carries the layout vid");
        check(cap.cmds[5].op == ALR_VK_CMD_BIND_VERTEX_BUFFERS && cap.cmds[5].h0 == kVbufVtx,
              "bind-vertex-buffers carries the buffer vid");
        check(cap.cmds[6].op == ALR_VK_CMD_BIND_INDEX_BUFFER && cap.cmds[6].h0 == kVbufIdx,
              "bind-index-buffer carries the buffer vid");
        // DRAW packs (vtx<<32)|inst into a, (firstVtx<<32)|firstInst into b.
        check(cap.cmds[8].op == ALR_VK_CMD_DRAW &&
              (cap.cmds[8].a >> 32) == 3 && (cap.cmds[8].a & 0xffffffff) == 1,
              "draw round-trips vertexCount=3 instanceCount=1");
        check(cap.cmds[9].op == ALR_VK_CMD_DRAW_INDEXED &&
              (cap.cmds[9].a >> 32) == 6, "draw-indexed round-trips indexCount=6");
        check(cap.cmds[10].op == ALR_VK_CMD_END_RENDER_PASS, "end-renderpass decodes");
        check(cap.cmds[11].op == ALR_VK_CMD_PIPELINE_BARRIER, "pipeline-barrier decodes");
        check(cap.cmds[12].op == ALR_VK_CMD_COPY_BUFFER && cap.cmds[12].h0 == kVbufSrc &&
              cap.cmds[12].h1 == kVbufDst, "copy-buffer carries src+dst vids");
        check(cap.cmds[13].op == ALR_VK_CMD_COPY_BUFFER_TO_IMAGE && cap.cmds[13].h0 == kVbufSrc &&
              cap.cmds[13].h1 == kVimg, "copy-buffer-to-image carries buffer+image vids");
        check(cap.cmds[14].op == ALR_VK_CMD_DISPATCH && (cap.cmds[14].a >> 32) == 8,
              "dispatch round-trips groupCountX=8");
    }

    // ===== 3) SUBMIT RING op through decode_vk_batch (the escape hand-off) =====
    SynthCmd syn;
    VkCmdProvider cp = syn.as_provider();
    set_vk_cmd_provider_seam(&cp);  // decode_vk_batch forwards this to the cmd band

    // First create the sync objects, then submit referencing them.
    std::vector<uint8_t> req(512);
    AlrVkCmdEncoder e;  // the guest-side ring encoder (below)
    e.init(req.data(), static_cast<uint32_t>(req.size()));
    e.create_fence(kVdev, kVfence, 0);
    e.create_semaphore(kVdev, kVsemWait, 0);
    e.create_semaphore(kVdev, kVsemSignal, 0);
    // vkQueueSubmit: queue + fence + 1 wait sem + 1 signal sem + 1 cmd-buffer (the log above).
    e.queue_submit_begin(kVqueue, kVfence);
    e.queue_submit_wait(kVsemWait, 0x400 /*COLOR_ATTACHMENT_OUTPUT*/);
    e.queue_submit_waits_done();
    e.queue_submit_signal(kVsemSignal);
    e.queue_submit_signals_done();
    e.queue_submit_cmd(kVcmd, log_off, log_len);
    e.queue_submit_cmds_done();
    e.end();
    check(!e.overflow(), "submit+sync request batch encodes");

    VkDecodeState st;
    VkReplyEncoder reply;
    const bool ok = decode_vk_batch(req.data(), e.len(), st, reply, /*provider=*/nullptr);
    set_vk_cmd_provider_seam(nullptr);
    check(ok, "decode_vk_batch (create-fence/sem + queue-submit through the cmd escape) ok");
    check(syn.fences_created == 1, "1 fence created via the cmd band");
    check(syn.sems_created == 2, "2 semaphores created via the cmd band");
    check(syn.submits == 1, "1 queue submit dispatched");
    check(syn.last_submit_queue == kVqueue && syn.last_submit_fence == kVfence,
          "submit carried the queue + fence vids");
    check(syn.last_wait_sem == kVsemWait && syn.last_signal_sem == kVsemSignal,
          "submit carried the wait + signal semaphore vids");
    check(syn.total_records == 15, "host decoded all 15 cmd-records out of the arena at submit");

    CmdReplies dr;
    check(decode_cmd_replies(reply.bytes(), dr), "cmd-log reply stream decodes");
    bool found_submit = false;
    for (auto& s : dr.submits)
        if (s.first == kVqueue) { found_submit = true;
            check(s.second.first == 0 && s.second.second == ALR_VK_CMD_REPLAY_OK,
                  "submit reply: VkResult ok + replay ok"); }
    check(found_submit, "submit reply present for the queue");

    // ===== 4) SYNC ops: wait + reset fences, wait-idle =====
    {
        std::vector<uint8_t> req2(256);
        AlrVkCmdEncoder e2;
        e2.init(req2.data(), static_cast<uint32_t>(req2.size()));
        e2.wait_for_fences(kVdev, /*wait_all=*/1, /*timeout=*/UINT64_MAX, {kVfence});
        e2.reset_fences(kVdev, {kVfence});
        e2.queue_wait_idle(kVqueue);
        e2.device_wait_idle(kVdev);
        e2.end();
        check(!e2.overflow(), "sync-op batch encodes");
        VkDecodeState st2;
        VkReplyEncoder reply2;
        set_vk_cmd_provider_seam(&cp);
        const bool ok2 = decode_vk_batch(req2.data(), e2.len(), st2, reply2, nullptr);
        set_vk_cmd_provider_seam(nullptr);
        check(ok2, "decode_vk_batch (wait/reset/idle) ok");
        check(syn.fences_waited == 1, "1 fence waited");
        check(syn.fences_reset == 1, "1 fence reset");
        check(syn.queue_idle == 1 && syn.device_idle == 1, "queue + device wait-idle ran");
    }

    // ===== 4b) INLINE-FALLBACK submit (the path the C ICD recorder uses): log_off ==
    //          kAlrVkArenaNoOffset + the recorded bytes shipped INLINE on the wire (for a
    //          guest with no usable arena slot). The host reads the log off the ring, not the
    //          arena, and replays it the same way. =====
    {
        // Record a tiny log into a guest-local (non-arena) buffer.
        uint8_t local[256];
        AlrVkCmdRecorder lrec;
        lrec.begin(local, sizeof(local), kAlrVkArenaNoOffset);  // NO arena offset
        lrec.cmd(ALR_VK_CMD_BIND_PIPELINE); lrec.u32(0); lrec.u32(kVpipe);
        lrec.cmd(ALR_VK_CMD_DRAW); lrec.u32(3); lrec.u32(1); lrec.u32(0); lrec.u32(0);
        const uint32_t llen = lrec.finalize();
        check(!lrec.overflow() && llen > 0, "inline log records");

        std::vector<uint8_t> req4(512);
        AlrVkCmdEncoder e4;
        e4.init(req4.data(), static_cast<uint32_t>(req4.size()));
        e4.queue_submit_begin(kVqueue, 0);
        e4.queue_submit_waits_done();
        e4.queue_submit_signals_done();
        // Inline: pass kAlrVkArenaNoOffset + the bytes (the encoder appends them on the wire).
        e4.queue_submit_cmd(kVcmd, kAlrVkArenaNoOffset, llen, local);
        e4.queue_submit_cmds_done();
        e4.end();
        check(!e4.overflow(), "inline submit encodes (log bytes on the wire)");

        const int recs_before = syn.total_records;
        VkDecodeState st4;
        VkReplyEncoder reply4;
        set_vk_cmd_provider_seam(&cp);
        const bool ok4 = decode_vk_batch(req4.data(), e4.len(), st4, reply4, nullptr);
        set_vk_cmd_provider_seam(nullptr);
        check(ok4, "inline-fallback submit decodes");
        check(syn.total_records == recs_before + 2,
              "host decoded the 2 inline cmd-records off the ring (no arena)");
    }

    // ===== 5a) BAND COEXISTENCE: a create-forwards op (low sub-op) + a cmd-log op (0x4000)
    //          in ONE batch each reach their own dispatcher (no cross-talk). =====
    {
        // The create-forwards band's synthetic provider (from the codegen header).
        VkGenProvider gp{};
        struct GenCtx { int creates = 0; } gctx;
        gp.create_handle = [](void* c, uint16_t, uint32_t, uint32_t, uint64_t, uint64_t) -> int {
            static_cast<GenCtx*>(c)->creates++; return 0; };
        gp.ctx = &gctx;
        set_vk_gen_provider(&gp);
        set_vk_cmd_provider_seam(&cp);

        std::vector<uint8_t> req3(256);
        AlrVkEncoder ge;  // the create-forwards encoder for a command-pool create (low band)
        alr_vk_enc_init(&ge, req3.data(), static_cast<uint32_t>(req3.size()));
        alr_vk_enc_gen_create_command_pool_begin(&ge, kVdev, 3000u, 0u, 0u);
        alr_vk_gen_pnext_count(&ge, 0);
        // …followed by a cmd-log queue-wait-idle (0x4000 band) in the SAME batch.
        AlrVkCmdEncoder ce;
        ce.init(req3.data() + ge.len, static_cast<uint32_t>(req3.size() - ge.len));
        ce.queue_wait_idle(kVqueue);
        ce.end();
        const uint32_t total = static_cast<uint32_t>(ge.len) + ce.len();
        check(!ge.overflow && !ce.overflow(), "mixed-band batch encodes");

        VkDecodeState st3;
        VkReplyEncoder reply3;
        const int idle_before = syn.queue_idle;
        const bool ok3 = decode_vk_batch(req3.data(), total, st3, reply3, nullptr);
        set_vk_gen_provider(nullptr);
        set_vk_cmd_provider_seam(nullptr);
        check(ok3, "mixed create-forwards + cmd-log batch decodes");
        check(gctx.creates == 1, "create-forwards op reached the gen dispatcher");
        check(syn.queue_idle == idle_before + 1, "cmd-log op reached the cmd dispatcher (no cross-talk)");
    }

    // ===== 5b) A malformed cmd-log op fail-stops (never accept off-contract). =====
    {
        // An escape + a 0x4000-band sub-op (queue-submit) truncated before its operands.
        std::vector<uint8_t> bad(8, 0);
        AlrVkCmdEncoder be;
        be.init(bad.data(), static_cast<uint32_t>(bad.size()));
        be.raw_escape(ALR_VK_CMD_SUBOP_QUEUE_SUBMIT);  // escape + sub, no operands
        const uint32_t blen = be.len();
        VkDecodeState st4;
        VkReplyEncoder reply4;
        set_vk_cmd_provider_seam(&cp);
        const bool bok = decode_vk_batch(bad.data(), blen, st4, reply4, nullptr);
        set_vk_cmd_provider_seam(nullptr);
        check(!bok, "truncated cmd-log submit fails cleanly");
    }

    // ===== 5c) An unknown 0x4000-band sub-op fail-stops. =====
    {
        std::vector<uint8_t> bad(8, 0);
        AlrVkCmdEncoder be;
        be.init(bad.data(), static_cast<uint32_t>(bad.size()));
        be.raw_escape(0x4FFFu);  // a 0x4000-band sub-op that doesn't exist
        const uint32_t blen = be.len();
        VkDecodeState st5;
        VkReplyEncoder reply5;
        const bool bok = decode_vk_batch(bad.data(), blen, st5, reply5, nullptr);
        check(!bok, "unknown cmd-log sub-op fails cleanly");
    }

    if (failures == 0) {
        printf("native_vk_cmdlog_test: ALL PASS (vkCmd* recording: guest-local arena log "
               "[begin-renderpass+bind+draw+end+transfer+barrier+dispatch] -> host replay "
               "round trip + vkQueueSubmit/fence/semaphore/wait sync over the 0x4000 escape "
               "band, coexisting with the create-forwards codegen band)\n");
        return 0;
    }
    printf("native_vk_cmdlog_test: %d FAILURE(S)\n", failures);
    return 1;
}
