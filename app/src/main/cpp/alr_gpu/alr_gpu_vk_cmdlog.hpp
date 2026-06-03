// ALR GPU Vulkan COMMAND-BUFFER RECORDING ("cmd-log") wire contract + host replay.
//
// THE HARD PART OF GPU FULL-PASSTHROUGH (memory alr-gpu-native-track + the a709aef8
// design transcript). Unlike vkCreate*/vkBind* (which RUN immediately and round-trip a
// VkResult — the create-forwards codegen band, alr_gpu/generated/alr_gpu_vk_gen_*), the
// vkCmd* family does NOT execute when called. They RECORD into a VkCommandBuffer; the GPU
// work happens later at vkQueueSubmit. Marshalling each vkCmd* as its own ring op +
// round-trip would be catastrophically slow (a frame is thousands of vkCmd* calls).
//
// THE DESIGN (guest-local record, ship-once at submit, host replay):
//   * vkBeginCommandBuffer allocates a per-command-buffer RECORD LOG in the SAME-PROCESS
//     MAP_SHARED arena (alr_gpu_vk_arena.hpp), keyed by the guest VkCommandBuffer's vid.
//   * Each vkCmd* APPENDS one record { u16 cmd-opcode, fixed args (handles as guest vids),
//     inline data, arena offsets for large pointers } to that log — PURELY GUEST-LOCAL, a
//     memcpy into the arena, NO ring op, NO host round-trip.
//   * vkEndCommandBuffer finalizes the log length. vkResetCommandBuffer / pool reset clears
//     it (length := 0).
//   * vkQueueSubmit ships ONE ring op { queue vid, [cmd-buffer vids], [wait sem vids],
//     [signal sem vids], fence vid } (ALR_VK_OP_GEN escape, the SUBMIT sub-op). The host
//     reads each cmd-buffer's log straight out of the shared arena (zero-copy — the bytes
//     never cross the ring), REPLAYS it into a REAL Mali VkCommandBuffer (decode each
//     cmd-opcode -> translate guest vids to real handles via VkDecodeState/VkGenTables ->
//     call the REAL vkCmd*), then issues the real vkQueueSubmit on the owner thread.
//
// OPCODE BAND (coexists with the create-forwards codegen — keep distinct ranges so the two
// agents merge cleanly): the cmd-log + submit/sync ops ride the SAME u8 escape byte
// ALR_VK_OP_GEN_ESCAPE (230) the codegen uses, but on a DISJOINT u16 sub-opcode band
// starting at ALR_VK_CMD_SUBOP_BASE = 0x4000. The create-forwards band is 1.. (auto-grown
// from vk.xml; currently ~17). 0x4000 leaves the entire low band to the generator and can
// never collide. Two dispatchers chain off decode_vk_batch's default case: the generated
// decoder (decode_vk_gen_op, low sub-ops) and THIS one (decode_vk_cmd_op, 0x4000 sub-ops);
// each claims only its own sub-opcodes.
//
// THE CMD-OPCODES (u16, a SEPARATE enum from the ring sub-opcodes — these live INSIDE the
// per-command-buffer log, not on the ring): the core set ANGLE's RendererVk emits. Stable,
// append-only (they are the recorded-log ABI). See AlrVkCmd below.
//
// Header-only + self-contained: pure C++/POSIX for the WIRE codec (no Vulkan SDK — the host
// wire test records a log + replay-decodes it with a synthetic Mali, proving opcodes/handles
// round-trip). The REAL-Mali replay (translate handles + call vkCmd* on vendor libvulkan) is
// compiled only under ALR_VK_DECODE_REAL. Matches the alr_gpu/** rule.

#ifndef ALR_GPU_ALR_GPU_VK_CMDLOG_HPP
#define ALR_GPU_ALR_GPU_VK_CMDLOG_HPP

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "alr_gpu/alr_gpu_vk_decode.hpp"          // VkReader, VkDecodeState, VkReplyEncoder
#include "alr_gpu/generated/alr_gpu_vk_arena.hpp"  // the MAP_SHARED arena (cmd-log storage)

namespace alr::gpu {

// ===========================================================================
// (1) RING sub-opcodes for the cmd-log band (ride ALR_VK_OP_GEN_ESCAPE; u16; 0x4000+).
// DISJOINT from the create-forwards codegen band (1..). APPEND-ONLY — the wire.
// ===========================================================================
enum AlrVkCmdSubOp {
    ALR_VK_CMD_SUBOP_BASE = 0x4000,

    // vkQueueSubmit: ship the submit topology (NOT the recorded bytes — those live in the
    // arena, found via each cmd-buffer's vid). The host replays each cmd-buffer's arena log
    // into a real Mali VkCommandBuffer, then real vkQueueSubmit on the owner thread.
    //   u32 vqueue, u32 vfence (0 == VK_NULL_HANDLE),
    //   u32 wait_count,   wait_count   × { u32 vsem, u32 stage_mask_lo }   (wait semaphores)
    //   u32 signal_count, signal_count × { u32 vsem }                      (signal semaphores)
    //   u32 cmd_count,    cmd_count    × { u32 vcmd, u64 log_off, u32 log_len }
    //       log_off = the cmd-buffer's record-log arena offset (kAlrVkArenaNoOffset if the
    //       log was guest-local, see ALR_VK_CMD_LOG_INLINE); log_len = recorded bytes.
    // Round-trips: reply carries the submit VkResult (ALR_VK_CMD_REPLY_SUBMIT).
    ALR_VK_CMD_SUBOP_QUEUE_SUBMIT = 0x4000,

    // vkQueueWaitIdle(vqueue) — round-trips the VkResult.
    ALR_VK_CMD_SUBOP_QUEUE_WAIT_IDLE = 0x4001,
    // vkDeviceWaitIdle(vdev) — round-trips the VkResult.
    ALR_VK_CMD_SUBOP_DEVICE_WAIT_IDLE = 0x4002,

    // ---- Fences (sync). Created/destroyed here (NOT the create-forwards band) because
    // the submit + wait path owns them. ----
    ALR_VK_CMD_SUBOP_CREATE_FENCE = 0x4010,   // u32 vdev, u32 vfence, u32 flags  -> result
    ALR_VK_CMD_SUBOP_DESTROY_FENCE = 0x4011,  // u32 vdev, u32 vfence             (no reply)
    // vkWaitForFences: u32 vdev, u32 wait_all, u64 timeout, u32 count, count×u32 vfence
    //   -> result (VK_SUCCESS / VK_TIMEOUT).
    ALR_VK_CMD_SUBOP_WAIT_FOR_FENCES = 0x4012,
    // vkResetFences: u32 vdev, u32 count, count×u32 vfence  -> result.
    ALR_VK_CMD_SUBOP_RESET_FENCES = 0x4013,
    // vkGetFenceStatus: u32 vdev, u32 vfence  -> result (VK_SUCCESS signalled / VK_NOT_READY).
    ALR_VK_CMD_SUBOP_GET_FENCE_STATUS = 0x4014,

    // ---- Semaphores (sync). Same rationale as fences. ----
    ALR_VK_CMD_SUBOP_CREATE_SEMAPHORE = 0x4020,   // u32 vdev, u32 vsem, u32 flags -> result
    ALR_VK_CMD_SUBOP_DESTROY_SEMAPHORE = 0x4021,  // u32 vdev, u32 vsem            (no reply)
};

// Reply sub-opcodes (ride ALR_VK_REPLY_GEN_ESCAPE == 230; u16; 0x4000+ to stay disjoint
// from the create-forwards reply band 1..). APPEND-ONLY.
enum AlrVkCmdReply {
    ALR_VK_CMD_REPLY_SUBMIT = 0x4000,       // u32 vqueue, i32 vk_result, i32 replay_result
    ALR_VK_CMD_REPLY_RESULT = 0x4001,       // u32 key, i32 vk_result  (waits / create / status)
};

// Host-side cmd-log replay outcome (carried in ALR_VK_CMD_REPLY_SUBMIT::replay_result). 0 ==
// every cmd-buffer's log decoded + replayed into a real VkCommandBuffer and the submit ran.
enum AlrVkCmdReplayResult {
    ALR_VK_CMD_REPLAY_OK = 0,
    ALR_VK_CMD_REPLAY_NO_QUEUE = 1,       // vqueue unknown to the host
    ALR_VK_CMD_REPLAY_NO_CMD = 2,         // a vcmd had no real Mali command buffer
    ALR_VK_CMD_REPLAY_LOG_UNREADABLE = 3, // a cmd-buffer's arena log offset/len was invalid
    ALR_VK_CMD_REPLAY_DECODE = 4,         // a record in a log was malformed (bad opcode/trunc)
    ALR_VK_CMD_REPLAY_BEGIN = 5,          // vkBeginCommandBuffer on the real buffer failed
    ALR_VK_CMD_REPLAY_END = 6,            // vkEndCommandBuffer on the real buffer failed
    ALR_VK_CMD_REPLAY_HANDLE = 7,         // a referenced guest vid had no real handle
    ALR_VK_CMD_REPLAY_SUBMIT = 8,         // vkQueueSubmit itself failed
};

// ===========================================================================
// (2) The CMD-OPCODES that live INSIDE a command-buffer's record log (u16). A SEPARATE
// enum from the ring sub-opcodes above — these are never seen on the ring; the host reads
// them out of the arena log during replay. The core set ANGLE's RendererVk records.
// APPEND-ONLY (the recorded-log ABI).
// ===========================================================================
enum AlrVkCmd {
    ALR_VK_CMD_END = 0,  // log terminator (vkEndCommandBuffer writes it)

    // ---- render-pass scoping ----
    ALR_VK_CMD_BEGIN_RENDER_PASS = 1,
    //   u32 vrenderpass, u32 vframebuffer,
    //   i32 rx, i32 ry, u32 rw, u32 rh,            (render area)
    //   u32 contents,                              (VkSubpassContents)
    //   u32 clear_count, clear_count × { 16 bytes VkClearValue raw }
    ALR_VK_CMD_END_RENDER_PASS = 2,                 // (no args)
    ALR_VK_CMD_NEXT_SUBPASS = 3,                     // u32 contents
    // KHR/core-1.3 dynamic variants (BeginRenderPass2 / EndRenderPass2). The
    // VkSubpassBeginInfo/VkSubpassEndInfo are reduced to their contents field (their
    // pNext is dropped on the bring-up wire — honest scope).
    ALR_VK_CMD_BEGIN_RENDER_PASS2 = 4,               // same payload as BEGIN_RENDER_PASS
    ALR_VK_CMD_END_RENDER_PASS2 = 5,                 // (no args)
    ALR_VK_CMD_NEXT_SUBPASS2 = 6,                    // u32 contents

    // ---- pipeline + binding ----
    ALR_VK_CMD_BIND_PIPELINE = 10,                   // u32 bindPoint, u32 vpipeline
    ALR_VK_CMD_BIND_DESCRIPTOR_SETS = 11,
    //   u32 bindPoint, u32 vlayout, u32 firstSet,
    //   u32 set_count,     set_count     × u32 vdescset,
    //   u32 dynamic_count, dynamic_count × u32 dynamic_offset
    ALR_VK_CMD_BIND_VERTEX_BUFFERS = 12,
    //   u32 firstBinding, u32 count, count × { u32 vbuffer, u64 offset }
    ALR_VK_CMD_BIND_INDEX_BUFFER = 13,               // u32 vbuffer, u64 offset, u32 indexType

    // ---- draw / dispatch ----
    ALR_VK_CMD_DRAW = 20,                            // u32 vtx, u32 inst, u32 firstVtx, u32 firstInst
    ALR_VK_CMD_DRAW_INDEXED = 21,
    //   u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance
    ALR_VK_CMD_DRAW_INDIRECT = 22,                   // u32 vbuffer, u64 offset, u32 drawCount, u32 stride
    ALR_VK_CMD_DISPATCH = 23,                        // u32 gx, u32 gy, u32 gz

    // ---- dynamic state ----
    ALR_VK_CMD_SET_VIEWPORT = 30,
    //   u32 firstViewport, u32 count, count × { f32 x,y,w,h,minDepth,maxDepth }
    ALR_VK_CMD_SET_SCISSOR = 31,
    //   u32 firstScissor, u32 count, count × { i32 x, i32 y, u32 w, u32 h }

    // ---- pipeline barriers ----
    ALR_VK_CMD_PIPELINE_BARRIER = 40,
    //   u32 srcStage, u32 dstStage, u32 depFlags,
    //   u32 mem_count,    mem_count    × { u32 srcAccess, u32 dstAccess }
    //   u32 buf_count,    buf_count    × { u32 srcAccess, u32 dstAccess, u32 srcQF, u32 dstQF,
    //                                      u32 vbuffer, u64 offset, u64 size }
    //   u32 img_count,    img_count    × { u32 srcAccess, u32 dstAccess, u32 oldLayout,
    //                                      u32 newLayout, u32 srcQF, u32 dstQF, u32 vimage,
    //                                      u32 aspect, u32 baseMip, u32 levelCount,
    //                                      u32 baseLayer, u32 layerCount }
    // KHR/core-1.3 synchronization2 variant + the event variants share this op for the
    // bring-up (the dependency-info is reduced to the same triple of arrays; stage/access
    // are the 32-bit values — the 64-bit sync2 masks are truncated to 32 bits, which covers
    // ANGLE's actual usage on this band — honest scope).
    ALR_VK_CMD_PIPELINE_BARRIER2 = 41,               // same payload as PIPELINE_BARRIER

    // ---- transfers ----
    ALR_VK_CMD_COPY_BUFFER = 50,
    //   u32 vsrc, u32 vdst, u32 region_count, region_count × { u64 srcOff, u64 dstOff, u64 size }
    ALR_VK_CMD_COPY_IMAGE = 51,
    //   u32 vsrc, u32 srcLayout, u32 vdst, u32 dstLayout,
    //   u32 region_count, region_count × {
    //       u32 srcAspect, u32 srcMip, u32 srcBaseLayer, u32 srcLayerCount,
    //       i32 srcX, i32 srcY, i32 srcZ,
    //       u32 dstAspect, u32 dstMip, u32 dstBaseLayer, u32 dstLayerCount,
    //       i32 dstX, i32 dstY, i32 dstZ, u32 w, u32 h, u32 d }
    ALR_VK_CMD_COPY_BUFFER_TO_IMAGE = 52,
    //   u32 vsrcbuf, u32 vdstimg, u32 dstLayout,
    //   u32 region_count, region_count × {
    //       u64 bufferOffset, u32 bufferRowLength, u32 bufferImageHeight,
    //       u32 aspect, u32 mip, u32 baseLayer, u32 layerCount,
    //       i32 x, i32 y, i32 z, u32 w, u32 h, u32 d }

    // ---- push constants ----
    ALR_VK_CMD_PUSH_CONSTANTS = 60,
    //   u32 vlayout, u32 stageFlags, u32 offset, u32 size, size bytes (inline)

    // ---- clears ----
    ALR_VK_CMD_CLEAR_ATTACHMENTS = 70,
    //   u32 att_count, att_count × { u32 aspectMask, u32 colorAttachment, 16 bytes VkClearValue }
    //   u32 rect_count, rect_count × { i32 x, i32 y, u32 w, u32 h, u32 baseArrayLayer, u32 layerCount }
};

// ---------------------------------------------------------------------------
// (3) The GUEST-LOCAL RECORDER. A thin append-only byte builder over a region of the
// SAME-PROCESS arena (or a guest-local fallback buffer if the arena is unavailable). The
// guest ICD owns one AlrVkCmdRecorder per VkCommandBuffer between vkBeginCommandBuffer and
// vkEndCommandBuffer. It is a header-only C++ helper here (the ICD is C, so the ICD wraps
// the same byte ops in C — see alr_gpu/guest_icd/alr_icd_cmd_record.h, which mirrors THIS
// encoding exactly; this C++ version is what the host wire test records with).
//
// The recorder is a pure little-endian appender (same field encoding as AlrVkEncoder): u16
// cmd-opcode, then the fixed args, inline data, and arena offsets, EXACTLY in the order the
// host replay reader (cmd_replay_log) reads them back. No ring ops happen here.
// ---------------------------------------------------------------------------
class AlrVkCmdRecorder {
public:
    AlrVkCmdRecorder() = default;

    // Begin recording into `buf` (caller-owned; arena-resident on the device path). After
    // begin the recorder appends until finalize(). `arena_off` is the buffer's arena offset
    // (so the submit op can tell the host where to read), or kAlrVkArenaNoOffset for a
    // guest-local buffer the submit will inline.
    void begin(uint8_t* buf, size_t cap, uint64_t arena_off) {
        buf_ = buf; cap_ = cap; len_ = 0; arena_off_ = arena_off; overflow_ = false;
    }

    // Append helpers (the recorded-log encoding).
    void u8(uint8_t v) { raw(&v, 1); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void bytes(const void* p, uint32_t n) { if (n) raw(p, n); }

    // Open one record: write its u16 cmd-opcode. The caller then appends the args.
    void cmd(uint16_t opcode) { u16(opcode); }

    // Finalize: append the END terminator. Returns the recorded length (0 on overflow).
    uint32_t finalize() {
        u16(static_cast<uint16_t>(ALR_VK_CMD_END));
        return overflow_ ? 0 : static_cast<uint32_t>(len_);
    }

    uint32_t length() const { return static_cast<uint32_t>(len_); }
    uint64_t arena_off() const { return arena_off_; }
    bool overflow() const { return overflow_; }
    const uint8_t* data() const { return buf_; }

private:
    void raw(const void* p, size_t n) {
        if (overflow_) return;
        if (len_ + n > cap_) { overflow_ = true; return; }
        if (n) std::memcpy(buf_ + len_, p, n);
        len_ += n;
    }
    uint8_t* buf_ = nullptr;
    size_t cap_ = 0;
    size_t len_ = 0;
    uint64_t arena_off_ = kAlrVkArenaNoOffset;
    bool overflow_ = false;
};

// ---------------------------------------------------------------------------
// (3b) The GUEST-SIDE RING ENCODER for the cmd-log RING ops (vkQueueSubmit + the sync ops).
// Unlike the per-cmd recorder above (which writes the arena log, NO ring), THESE ops cross
// the ring. This is the C++ builder the host wire test uses; the guest ICD (C) mirrors the
// exact same encoding (alr_gpu/guest_icd/alr_icd_cmd_record.h). Each op is the shared escape
// byte (ALR_VK_OP_GEN_ESCAPE == 230, the value mirrored as kVkGenEscapeOp in the decoder) +
// a u16 sub-opcode (0x4000 band) + operands, matching decode_vk_cmd_op's field order.
// ---------------------------------------------------------------------------
class AlrVkCmdEncoder {
public:
    void init(uint8_t* buf, uint32_t cap) { buf_ = buf; cap_ = cap; len_ = 0; overflow_ = false; }
    uint32_t len() const { return len_; }
    bool overflow() const { return overflow_; }

    // Open an op: the shared escape byte + the u16 sub-opcode. (Public so the wire test can
    // craft a malformed "escape + sub, no operands" op via raw_escape.)
    void raw_escape(uint16_t sub_op) { u8(kVkGenEscapeOp); u16(sub_op); }

    // ---- create / destroy sync objects ----
    void create_fence(uint32_t vdev, uint32_t vfence, uint32_t flags) {
        raw_escape(ALR_VK_CMD_SUBOP_CREATE_FENCE); u32(vdev); u32(vfence); u32(flags);
    }
    void destroy_fence(uint32_t vdev, uint32_t vfence) {
        raw_escape(ALR_VK_CMD_SUBOP_DESTROY_FENCE); u32(vdev); u32(vfence);
    }
    void create_semaphore(uint32_t vdev, uint32_t vsem, uint32_t flags) {
        raw_escape(ALR_VK_CMD_SUBOP_CREATE_SEMAPHORE); u32(vdev); u32(vsem); u32(flags);
    }
    void destroy_semaphore(uint32_t vdev, uint32_t vsem) {
        raw_escape(ALR_VK_CMD_SUBOP_DESTROY_SEMAPHORE); u32(vdev); u32(vsem);
    }

    // ---- vkQueueSubmit (built incrementally: the wait/signal/cmd lists are variable). ----
    void queue_submit_begin(uint32_t vqueue, uint32_t vfence) {
        raw_escape(ALR_VK_CMD_SUBOP_QUEUE_SUBMIT); u32(vqueue); u32(vfence);
        wait_n_ = signal_n_ = cmd_n_ = 0;
        wait_count_pos_ = len_; u32(0);  // patched by queue_submit_waits_done
    }
    void queue_submit_wait(uint32_t vsem, uint32_t stage_mask_lo) {
        u32(vsem); u32(stage_mask_lo); ++wait_n_;
    }
    void queue_submit_waits_done() {
        patch_u32(wait_count_pos_, wait_n_);
        signal_count_pos_ = len_; u32(0);
    }
    void queue_submit_signal(uint32_t vsem) { u32(vsem); ++signal_n_; }
    void queue_submit_signals_done() {
        patch_u32(signal_count_pos_, signal_n_);
        cmd_count_pos_ = len_; u32(0);
    }
    // Add one cmd-buffer to the submit. The PRIMARY path passes its arena `log_off` (the
    // host reads the log zero-copy from the shared arena; `log` may be null). The INLINE
    // FALLBACK passes log_off == kAlrVkArenaNoOffset + a non-null `log` of `log_len` bytes,
    // which are appended on the wire (a copy, for guests without a usable arena slot).
    void queue_submit_cmd(uint32_t vcmd, uint64_t log_off, uint32_t log_len,
                          const void* log = nullptr) {
        u32(vcmd); u64(log_off); u32(log_len);
        if (log_off == kAlrVkArenaNoOffset && log && log_len) raw(log, log_len);
        ++cmd_n_;
    }
    void queue_submit_cmds_done() { patch_u32(cmd_count_pos_, cmd_n_); }

    // ---- waits ----
    void wait_for_fences(uint32_t vdev, uint32_t wait_all, uint64_t timeout,
                         const std::vector<uint32_t>& vfences) {
        raw_escape(ALR_VK_CMD_SUBOP_WAIT_FOR_FENCES);
        u32(vdev); u32(wait_all); u64(timeout);
        u32(static_cast<uint32_t>(vfences.size()));
        for (uint32_t f : vfences) u32(f);
    }
    void reset_fences(uint32_t vdev, const std::vector<uint32_t>& vfences) {
        raw_escape(ALR_VK_CMD_SUBOP_RESET_FENCES);
        u32(vdev); u32(static_cast<uint32_t>(vfences.size()));
        for (uint32_t f : vfences) u32(f);
    }
    void get_fence_status(uint32_t vdev, uint32_t vfence) {
        raw_escape(ALR_VK_CMD_SUBOP_GET_FENCE_STATUS); u32(vdev); u32(vfence);
    }
    void queue_wait_idle(uint32_t vqueue) {
        raw_escape(ALR_VK_CMD_SUBOP_QUEUE_WAIT_IDLE); u32(vqueue);
    }
    void device_wait_idle(uint32_t vdev) {
        raw_escape(ALR_VK_CMD_SUBOP_DEVICE_WAIT_IDLE); u32(vdev);
    }

    // The shared op-stream terminator (same as ALR_VK_OP_END == 0).
    void end() { u8(0); }

    void u8(uint8_t v) { raw(&v, 1); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }

private:
    void raw(const void* p, size_t n) {
        if (overflow_) return;
        if (len_ + n > cap_) { overflow_ = true; return; }
        if (n) std::memcpy(buf_ + len_, p, n);
        len_ += static_cast<uint32_t>(n);
    }
    void patch_u32(uint32_t pos, uint32_t v) {
        if (pos + 4 <= len_) std::memcpy(buf_ + pos, &v, 4);
    }
    uint8_t* buf_ = nullptr;
    uint32_t cap_ = 0;
    uint32_t len_ = 0;
    bool overflow_ = false;
    uint32_t wait_n_ = 0, signal_n_ = 0, cmd_n_ = 0;
    uint32_t wait_count_pos_ = 0, signal_count_pos_ = 0, cmd_count_pos_ = 0;
};

// ===========================================================================
// (4) The HOST REPLAY. cmd_replay_log() reads ONE command-buffer's record log (raw bytes,
// already located via its arena offset) and replays each record into a real Mali
// VkCommandBuffer (translating guest vids -> real handles via VkGenTables), under
// ALR_VK_DECODE_REAL. Without it, cmd_replay_log_wire() is the no-SDK codec the host wire
// test drives (it decodes every record + counts/echoes them through a synthetic sink,
// proving the opcodes/handles round-trip with no GPU).
// ===========================================================================

// A synthetic replay sink for the wire test: each call records that the host decoded one
// cmd-record with its translated-or-virtual operands. The wire test asserts the sequence.
struct VkCmdReplaySink {
    // Called once per decoded record. `op` is the AlrVkCmd opcode. h0/h1 are the first one
    // or two handle operands (guest vids in wire mode), a/b two scalar operands, for the
    // test to assert the round trip without a full struct per op. ctx is opaque.
    void (*on_cmd)(void* ctx, uint16_t op, uint32_t h0, uint32_t h1, uint64_t a,
                   uint64_t b) = nullptr;
    void* ctx = nullptr;
};

// Decode ONE record off the reader (the u16 opcode already read into `op`), consuming its
// operands, and (in wire mode) emit it to the sink. Returns false on a malformed record.
// SHARED by the wire codec and the real-Mali replay (the real path overrides the per-op
// body under ALR_VK_DECODE_REAL — see cmd_replay_one_real below — but the FIELD ORDER is
// defined ONCE here so the two can never drift).
//
// `emit2` lets the real path reuse the exact same field-reading sequence: this function
// reads the fields into locals and calls `emit2(op, h0, h1, a, b, r)` giving the caller the
// reader positioned right after the fixed operands (so the real path can read the same
// variable arrays). For the wire codec, emit2 is the sink echo.
template <typename EmitFn>
inline bool cmd_decode_one(uint16_t op, VkReader& r, EmitFn&& emit) {
    switch (op) {
        case ALR_VK_CMD_BEGIN_RENDER_PASS:
        case ALR_VK_CMD_BEGIN_RENDER_PASS2: {
            uint32_t vrp = 0, vfb = 0, contents = 0, rw = 0, rh = 0, clear_count = 0;
            int32_t rx = 0, ry = 0;
            if (!r.u32(vrp) || !r.u32(vfb) || !r.i32(rx) || !r.i32(ry) || !r.u32(rw) ||
                !r.u32(rh) || !r.u32(contents) || !r.u32(clear_count))
                return false;
            if (clear_count > 16) return false;  // VK_MAX color+depth attachments bound
            for (uint32_t i = 0; i < clear_count; ++i) {
                const uint8_t* cv = nullptr;
                if (!r.take_raw(cv, 16)) return false;  // 16-byte VkClearValue
                (void)cv;
            }
            return emit(op, vrp, vfb, static_cast<uint64_t>(contents), clear_count);
        }
        case ALR_VK_CMD_END_RENDER_PASS:
        case ALR_VK_CMD_END_RENDER_PASS2:
            return emit(op, 0, 0, 0, 0);
        case ALR_VK_CMD_NEXT_SUBPASS:
        case ALR_VK_CMD_NEXT_SUBPASS2: {
            uint32_t contents = 0;
            if (!r.u32(contents)) return false;
            return emit(op, 0, 0, static_cast<uint64_t>(contents), 0);
        }
        case ALR_VK_CMD_BIND_PIPELINE: {
            uint32_t bp = 0, vpipe = 0;
            if (!r.u32(bp) || !r.u32(vpipe)) return false;
            return emit(op, vpipe, 0, static_cast<uint64_t>(bp), 0);
        }
        case ALR_VK_CMD_BIND_DESCRIPTOR_SETS: {
            uint32_t bp = 0, vlayout = 0, firstSet = 0, set_count = 0, dyn_count = 0;
            if (!r.u32(bp) || !r.u32(vlayout) || !r.u32(firstSet) || !r.u32(set_count))
                return false;
            if (set_count > 256) return false;
            for (uint32_t i = 0; i < set_count; ++i) { uint32_t s = 0; if (!r.u32(s)) return false; }
            if (!r.u32(dyn_count)) return false;
            if (dyn_count > 256) return false;
            for (uint32_t i = 0; i < dyn_count; ++i) { uint32_t d = 0; if (!r.u32(d)) return false; }
            return emit(op, vlayout, 0, static_cast<uint64_t>(bp),
                        (static_cast<uint64_t>(set_count) << 32) | dyn_count);
        }
        case ALR_VK_CMD_BIND_VERTEX_BUFFERS: {
            uint32_t firstBinding = 0, count = 0;
            if (!r.u32(firstBinding) || !r.u32(count)) return false;
            if (count > 64) return false;  // maxVertexInputBindings bound
            uint32_t first_vb = 0;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t vb = 0; uint64_t off = 0;
                if (!r.u32(vb) || !r.u64(off)) return false;
                if (i == 0) first_vb = vb;
            }
            return emit(op, first_vb, 0, static_cast<uint64_t>(firstBinding), count);
        }
        case ALR_VK_CMD_BIND_INDEX_BUFFER: {
            uint32_t vb = 0, indexType = 0; uint64_t off = 0;
            if (!r.u32(vb) || !r.u64(off) || !r.u32(indexType)) return false;
            return emit(op, vb, 0, off, static_cast<uint64_t>(indexType));
        }
        case ALR_VK_CMD_DRAW: {
            uint32_t vtx = 0, inst = 0, fv = 0, fi = 0;
            if (!r.u32(vtx) || !r.u32(inst) || !r.u32(fv) || !r.u32(fi)) return false;
            return emit(op, 0, 0, (static_cast<uint64_t>(vtx) << 32) | inst,
                        (static_cast<uint64_t>(fv) << 32) | fi);
        }
        case ALR_VK_CMD_DRAW_INDEXED: {
            uint32_t ic = 0, inst = 0, fi = 0, firstInst = 0; int32_t vo = 0;
            if (!r.u32(ic) || !r.u32(inst) || !r.u32(fi) || !r.i32(vo) || !r.u32(firstInst))
                return false;
            return emit(op, 0, 0, (static_cast<uint64_t>(ic) << 32) | inst,
                        (static_cast<uint64_t>(static_cast<uint32_t>(vo)) << 32) | firstInst);
        }
        case ALR_VK_CMD_DRAW_INDIRECT: {
            uint32_t vb = 0, drawCount = 0, stride = 0; uint64_t off = 0;
            if (!r.u32(vb) || !r.u64(off) || !r.u32(drawCount) || !r.u32(stride)) return false;
            return emit(op, vb, 0, off, (static_cast<uint64_t>(drawCount) << 32) | stride);
        }
        case ALR_VK_CMD_DISPATCH: {
            uint32_t gx = 0, gy = 0, gz = 0;
            if (!r.u32(gx) || !r.u32(gy) || !r.u32(gz)) return false;
            return emit(op, 0, 0, (static_cast<uint64_t>(gx) << 32) | gy, gz);
        }
        case ALR_VK_CMD_SET_VIEWPORT: {
            uint32_t fvp = 0, count = 0;
            if (!r.u32(fvp) || !r.u32(count)) return false;
            if (count > 16) return false;
            for (uint32_t i = 0; i < count; ++i) {
                float x, y, w, h, mn, mx;
                if (!r.f32(x) || !r.f32(y) || !r.f32(w) || !r.f32(h) || !r.f32(mn) || !r.f32(mx))
                    return false;
            }
            return emit(op, 0, 0, static_cast<uint64_t>(fvp), count);
        }
        case ALR_VK_CMD_SET_SCISSOR: {
            uint32_t fs = 0, count = 0;
            if (!r.u32(fs) || !r.u32(count)) return false;
            if (count > 16) return false;
            for (uint32_t i = 0; i < count; ++i) {
                int32_t x, y; uint32_t w, h;
                if (!r.i32(x) || !r.i32(y) || !r.u32(w) || !r.u32(h)) return false;
            }
            return emit(op, 0, 0, static_cast<uint64_t>(fs), count);
        }
        case ALR_VK_CMD_PIPELINE_BARRIER:
        case ALR_VK_CMD_PIPELINE_BARRIER2: {
            uint32_t srcStage = 0, dstStage = 0, depFlags = 0, mem_c = 0, buf_c = 0, img_c = 0;
            if (!r.u32(srcStage) || !r.u32(dstStage) || !r.u32(depFlags) || !r.u32(mem_c))
                return false;
            if (mem_c > 64) return false;
            for (uint32_t i = 0; i < mem_c; ++i) { uint32_t a, b; if (!r.u32(a) || !r.u32(b)) return false; }
            if (!r.u32(buf_c)) return false;
            if (buf_c > 256) return false;
            for (uint32_t i = 0; i < buf_c; ++i) {
                uint32_t sa, da, sqf, dqf, vb; uint64_t off, sz;
                if (!r.u32(sa) || !r.u32(da) || !r.u32(sqf) || !r.u32(dqf) || !r.u32(vb) ||
                    !r.u64(off) || !r.u64(sz)) return false;
            }
            if (!r.u32(img_c)) return false;
            if (img_c > 256) return false;
            for (uint32_t i = 0; i < img_c; ++i) {
                uint32_t sa, da, ol, nl, sqf, dqf, vi, asp, bm, lc, bl, lyc;
                if (!r.u32(sa) || !r.u32(da) || !r.u32(ol) || !r.u32(nl) || !r.u32(sqf) ||
                    !r.u32(dqf) || !r.u32(vi) || !r.u32(asp) || !r.u32(bm) || !r.u32(lc) ||
                    !r.u32(bl) || !r.u32(lyc)) return false;
            }
            return emit(op, 0, 0, (static_cast<uint64_t>(srcStage) << 32) | dstStage,
                        (static_cast<uint64_t>(mem_c) << 40) | (static_cast<uint64_t>(buf_c) << 20) | img_c);
        }
        case ALR_VK_CMD_COPY_BUFFER: {
            uint32_t vsrc = 0, vdst = 0, rc = 0;
            if (!r.u32(vsrc) || !r.u32(vdst) || !r.u32(rc)) return false;
            if (rc > 256) return false;
            for (uint32_t i = 0; i < rc; ++i) {
                uint64_t so, dofs, sz;
                if (!r.u64(so) || !r.u64(dofs) || !r.u64(sz)) return false;
            }
            return emit(op, vsrc, vdst, rc, 0);
        }
        case ALR_VK_CMD_COPY_IMAGE: {
            uint32_t vsrc = 0, sl = 0, vdst = 0, dl = 0, rc = 0;
            if (!r.u32(vsrc) || !r.u32(sl) || !r.u32(vdst) || !r.u32(dl) || !r.u32(rc))
                return false;
            if (rc > 256) return false;
            for (uint32_t i = 0; i < rc; ++i) {
                uint32_t a[8]; int32_t b[6]; uint32_t whd[3];
                bool ok = true;
                for (int k = 0; k < 4; ++k) ok = ok && r.u32(a[k]);
                for (int k = 0; k < 3; ++k) ok = ok && r.i32(b[k]);
                for (int k = 4; k < 8; ++k) ok = ok && r.u32(a[k]);
                for (int k = 3; k < 6; ++k) ok = ok && r.i32(b[k]);
                for (int k = 0; k < 3; ++k) ok = ok && r.u32(whd[k]);
                if (!ok) return false;
            }
            return emit(op, vsrc, vdst, rc, (static_cast<uint64_t>(sl) << 32) | dl);
        }
        case ALR_VK_CMD_COPY_BUFFER_TO_IMAGE: {
            uint32_t vbuf = 0, vimg = 0, dl = 0, rc = 0;
            if (!r.u32(vbuf) || !r.u32(vimg) || !r.u32(dl) || !r.u32(rc)) return false;
            if (rc > 256) return false;
            for (uint32_t i = 0; i < rc; ++i) {
                uint64_t bo; uint32_t brl, bih, asp, mip, bl, lc; int32_t x, y, z; uint32_t w, h, d;
                if (!r.u64(bo) || !r.u32(brl) || !r.u32(bih) || !r.u32(asp) || !r.u32(mip) ||
                    !r.u32(bl) || !r.u32(lc) || !r.i32(x) || !r.i32(y) || !r.i32(z) ||
                    !r.u32(w) || !r.u32(h) || !r.u32(d)) return false;
            }
            return emit(op, vbuf, vimg, rc, static_cast<uint64_t>(dl));
        }
        case ALR_VK_CMD_PUSH_CONSTANTS: {
            uint32_t vlayout = 0, stageFlags = 0, offset = 0, size = 0;
            if (!r.u32(vlayout) || !r.u32(stageFlags) || !r.u32(offset) || !r.u32(size))
                return false;
            if (size > 256) return false;  // maxPushConstantsSize bound
            const uint8_t* d = nullptr;
            if (!r.take_raw(d, size)) return false;
            (void)d;
            return emit(op, vlayout, 0, (static_cast<uint64_t>(stageFlags) << 32) | offset, size);
        }
        case ALR_VK_CMD_CLEAR_ATTACHMENTS: {
            uint32_t att_c = 0, rect_c = 0;
            if (!r.u32(att_c)) return false;
            if (att_c > 16) return false;
            for (uint32_t i = 0; i < att_c; ++i) {
                uint32_t am = 0, ca = 0; const uint8_t* cv = nullptr;
                if (!r.u32(am) || !r.u32(ca) || !r.take_raw(cv, 16)) return false;
                (void)cv;
            }
            if (!r.u32(rect_c)) return false;
            if (rect_c > 16) return false;
            for (uint32_t i = 0; i < rect_c; ++i) {
                int32_t x, y; uint32_t w, h, bal, lc;
                if (!r.i32(x) || !r.i32(y) || !r.u32(w) || !r.u32(h) || !r.u32(bal) || !r.u32(lc))
                    return false;
            }
            return emit(op, 0, 0, att_c, rect_c);
        }
        default:
            return false;  // unknown cmd-opcode: a malformed/off-contract log
    }
}

// Wire-mode: decode an entire cmd-buffer log (terminated by ALR_VK_CMD_END), echoing each
// record to the sink. Returns the number of records decoded, or -1 on a malformed log.
inline int cmd_replay_log_wire(const uint8_t* log, uint32_t len, const VkCmdReplaySink& sink) {
    VkReader r(log, len);
    int n = 0;
    for (;;) {
        uint16_t op = 0;
        if (!r.u16(op)) return n;            // ran off the end (no explicit END — tolerate)
        if (op == ALR_VK_CMD_END) return n;  // clean terminator
        auto emit = [&](uint16_t o, uint32_t h0, uint32_t h1, uint64_t a, uint64_t b) -> bool {
            if (sink.on_cmd) sink.on_cmd(sink.ctx, o, h0, h1, a, b);
            return true;
        };
        if (!cmd_decode_one(op, r, emit)) return -1;
        ++n;
    }
}

}  // namespace alr::gpu

// The real-Mali replay (translate guest vids -> real handles, call the real vkCmd*) is in a
// companion header compiled only under ALR_VK_DECODE_REAL, so the wire codec above stays
// SDK-free. It is included at the END (file scope) so its <vulkan/vulkan.h> isn't nested.
#ifdef ALR_VK_DECODE_REAL
#include "alr_gpu/alr_gpu_vk_cmdlog_real.hpp"
#endif

#endif  // ALR_GPU_ALR_GPU_VK_CMDLOG_HPP
