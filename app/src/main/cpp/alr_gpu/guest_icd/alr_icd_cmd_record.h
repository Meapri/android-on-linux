/* alr_icd_cmd_record.h — the GUEST-SIDE (C) command-buffer RECORDER + submit/sync ring
 * encoders for the cmd-log band. The C twin of the C++ AlrVkCmdRecorder/AlrVkCmdEncoder in
 * alr_gpu/alr_gpu_vk_cmdlog.hpp — SAME wire, byte for byte (the host decoder is the source
 * of truth: alr_gpu_vk_cmdlog.hpp + alr_gpu_vk_cmd_dispatch.hpp).
 *
 * WHY (the vkCmd* command-recording mechanism — the hard part of GPU full-passthrough):
 * vkCmd* calls do NOT execute; they RECORD into a command buffer, and the GPU work happens at
 * vkQueueSubmit. So the guest ICD's vkCmd* entrypoints do NOT ring-round-trip per call — each
 * APPENDS one record to a per-command-buffer byte log held HERE (guest-local), and
 * vkQueueSubmit ships the log (+ the queue/fence/semaphore vids) over the ring ONCE; the host
 * replays each log into a real Mali VkCommandBuffer.
 *
 * STORAGE: each AlrIcdCommandBuffer owns one AlrIcdCmdLog (a growable guest-local buffer).
 * vkBeginCommandBuffer resets it; each vkCmd* appends; vkEndCommandBuffer caps it;
 * vkResetCommandBuffer clears it. At submit the log is shipped INLINE on the wire (the
 * portable path — every guest can do it). A future rung can instead place the log in a
 * shared-arena slot and ship its offset (zero-copy) — the host submit op accepts BOTH (an
 * arena offset, or kAlrVkArenaNoOffset + inline bytes); this recorder uses the inline path.
 *
 * HOW THE ANGLE-PUSHING AGENT WIRES IT (the composition surface):
 *   * in alr_vkAllocateCommandBuffers: alr_icd_cmdlog_init(&cb->log)
 *   * in vkBeginCommandBuffer:         alr_icd_cmdlog_reset(&cb->log)
 *   * in each vkCmd*:                  alr_icd_cmd_<name>(&cb->log, ...)
 *   * in vkEndCommandBuffer:           alr_icd_cmdlog_end(&cb->log)
 *   * in vkResetCommandBuffer:         alr_icd_cmdlog_reset(&cb->log)
 *   * in vkQueueSubmit: build the submit op with alr_icd_cmd_submit_* (queue + fence +
 *     wait/signal sems + each cb's vcmd + log bytes), alr_icd_roundtrip it, read the result.
 *   * fences/semaphores/waits: alr_icd_cmd_create_fence / _wait_for_fences / etc.
 *
 * Pure C99 + the ICD's existing AlrVkEncoder (alr_gpu_vk_proto.hpp) + AlrVkReader
 * (alr_icd_gen_glue.h). NO Vulkan headers needed for the wire itself.
 */

#ifndef ALR_GPU_GUEST_ICD_ALR_ICD_CMD_RECORD_H
#define ALR_GPU_GUEST_ICD_ALR_ICD_CMD_RECORD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alr_gpu/alr_gpu_vk_proto.hpp"  /* AlrVkEncoder (the ring builder) + ALR_VK_OP_END */

/* The reply scanners below use AlrVkReader (the ICD's little-endian reply reader). The ICD
 * defines it in alr_icd_gen_glue.h, which alr_icd_vulkan.c #includes BEFORE this header, so
 * it is already in scope there. A standalone includer must #include "alr_icd_gen_glue.h"
 * first (it has its own include guard). */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The shared escape byte + the cmd-log RING sub-opcode band (0x4000). MUST equal the
 * C++ AlrVkCmdSubOp in alr_gpu_vk_cmdlog.hpp + kVkGenEscapeOp (230) in alr_gpu_vk_decode.hpp.
 * Kept as explicit numbers here (the C side has no access to the C++ enums); a drift would
 * be caught by the host decode fail-stopping on an unknown sub-op. ---- */
#define ALR_ICD_VK_GEN_ESCAPE        230u   /* == kVkGenEscapeOp / ALR_VK_OP_GEN_ESCAPE */
#define ALR_ICD_VK_REPLY_GEN_ESCAPE  230u   /* == ALR_VK_REPLY_GEN_ESCAPE */

#define ALR_ICD_CMD_SUBOP_QUEUE_SUBMIT      0x4000u
#define ALR_ICD_CMD_SUBOP_QUEUE_WAIT_IDLE   0x4001u
#define ALR_ICD_CMD_SUBOP_DEVICE_WAIT_IDLE  0x4002u
#define ALR_ICD_CMD_SUBOP_CREATE_FENCE      0x4010u
#define ALR_ICD_CMD_SUBOP_DESTROY_FENCE     0x4011u
#define ALR_ICD_CMD_SUBOP_WAIT_FOR_FENCES   0x4012u
#define ALR_ICD_CMD_SUBOP_RESET_FENCES      0x4013u
#define ALR_ICD_CMD_SUBOP_GET_FENCE_STATUS  0x4014u
#define ALR_ICD_CMD_SUBOP_CREATE_SEMAPHORE  0x4020u
#define ALR_ICD_CMD_SUBOP_DESTROY_SEMAPHORE 0x4021u

#define ALR_ICD_CMD_REPLY_SUBMIT  0x4000u
#define ALR_ICD_CMD_REPLY_RESULT  0x4001u

/* The arena no-offset sentinel (== kAlrVkArenaNoOffset). The inline path always uses this. */
#define ALR_ICD_CMD_ARENA_NO_OFFSET  ((uint64_t)-1)

/* ---- The cmd-OPCODES inside the per-command-buffer record log (u16). MUST equal the C++
 * AlrVkCmd enum in alr_gpu_vk_cmdlog.hpp. ---- */
#define ALR_ICD_CMD_END                    0u
#define ALR_ICD_CMD_BEGIN_RENDER_PASS      1u
#define ALR_ICD_CMD_END_RENDER_PASS        2u
#define ALR_ICD_CMD_NEXT_SUBPASS           3u
#define ALR_ICD_CMD_BEGIN_RENDER_PASS2     4u
#define ALR_ICD_CMD_END_RENDER_PASS2       5u
#define ALR_ICD_CMD_NEXT_SUBPASS2          6u
#define ALR_ICD_CMD_BIND_PIPELINE          10u
#define ALR_ICD_CMD_BIND_DESCRIPTOR_SETS   11u
#define ALR_ICD_CMD_BIND_VERTEX_BUFFERS    12u
#define ALR_ICD_CMD_BIND_INDEX_BUFFER      13u
#define ALR_ICD_CMD_DRAW                   20u
#define ALR_ICD_CMD_DRAW_INDEXED           21u
#define ALR_ICD_CMD_DRAW_INDIRECT          22u
#define ALR_ICD_CMD_DISPATCH               23u
#define ALR_ICD_CMD_SET_VIEWPORT           30u
#define ALR_ICD_CMD_SET_SCISSOR            31u
#define ALR_ICD_CMD_PIPELINE_BARRIER       40u
#define ALR_ICD_CMD_PIPELINE_BARRIER2      41u
#define ALR_ICD_CMD_COPY_BUFFER            50u
#define ALR_ICD_CMD_COPY_IMAGE             51u
#define ALR_ICD_CMD_COPY_BUFFER_TO_IMAGE   52u
#define ALR_ICD_CMD_PUSH_CONSTANTS         60u
#define ALR_ICD_CMD_CLEAR_ATTACHMENTS      70u

/* ===========================================================================
 * (1) AlrIcdCmdLog — the per-command-buffer guest-local record buffer.
 * =========================================================================== */
typedef struct AlrIcdCmdLog {
    uint8_t *buf;       /* malloc'd; grows on demand */
    uint32_t cap;       /* allocated capacity */
    uint32_t len;       /* bytes recorded so far (BEFORE the END terminator) */
    int      overflow;  /* set if a grow failed (the record is then incomplete) */
} AlrIcdCmdLog;

/* Initialize a fresh log (call once when the command buffer is allocated). */
static inline void alr_icd_cmdlog_init(AlrIcdCmdLog *L) {
    L->buf = NULL; L->cap = 0; L->len = 0; L->overflow = 0;
}
/* Free a log's storage (call when the command buffer / its pool is destroyed). */
static inline void alr_icd_cmdlog_free(AlrIcdCmdLog *L) {
    if (L->buf) free(L->buf);
    alr_icd_cmdlog_init(L);
}
/* Reset to empty (vkBeginCommandBuffer / vkResetCommandBuffer) — keeps the allocation. */
static inline void alr_icd_cmdlog_reset(AlrIcdCmdLog *L) { L->len = 0; L->overflow = 0; }

static inline void alr_icd_cmdlog_raw(AlrIcdCmdLog *L, const void *p, uint32_t n) {
    if (L->overflow) return;
    if (L->len + n > L->cap) {
        uint32_t need = L->len + n;
        uint32_t ncap = L->cap ? L->cap * 2 : 256;
        uint8_t *nb;
        while (ncap < need) ncap *= 2;
        nb = (uint8_t *)realloc(L->buf, ncap);
        if (!nb) { L->overflow = 1; return; }
        L->buf = nb; L->cap = ncap;
    }
    if (n) memcpy(L->buf + L->len, p, n);
    L->len += n;
}
static inline void alr_icd_cmdlog_u8 (AlrIcdCmdLog *L, uint8_t v)  { alr_icd_cmdlog_raw(L, &v, 1); }
static inline void alr_icd_cmdlog_u16(AlrIcdCmdLog *L, uint16_t v) { alr_icd_cmdlog_raw(L, &v, 2); }
static inline void alr_icd_cmdlog_u32(AlrIcdCmdLog *L, uint32_t v) { alr_icd_cmdlog_raw(L, &v, 4); }
static inline void alr_icd_cmdlog_i32(AlrIcdCmdLog *L, int32_t v)  { alr_icd_cmdlog_raw(L, &v, 4); }
static inline void alr_icd_cmdlog_u64(AlrIcdCmdLog *L, uint64_t v) { alr_icd_cmdlog_raw(L, &v, 8); }
static inline void alr_icd_cmdlog_f32(AlrIcdCmdLog *L, float v)    { alr_icd_cmdlog_raw(L, &v, 4); }

/* Open a record (write its u16 cmd-opcode). The caller appends the args via the helpers. */
static inline void alr_icd_cmdlog_cmd(AlrIcdCmdLog *L, uint16_t op) { alr_icd_cmdlog_u16(L, op); }

/* ===========================================================================
 * (2) Per-vkCmd* record helpers. Each appends ONE record in the EXACT field order the host
 * cmd_decode_one (alr_gpu_vk_cmdlog.hpp) reads back. Handles are guest VIRTUAL ids (u32) —
 * the host translates them. (The ANGLE-pushing agent calls these from the ICD's vkCmd*
 * entrypoints, converting each VkHandle to its virtual id via (uint32_t)(uintptr_t)handle.)
 * Only the fixed-args + simple-array ops are spelled out here; the agent appends the
 * variable arrays inline using the cmdlog_* appenders for the few list-bearing ops.
 * =========================================================================== */

static inline void alr_icd_cmd_bind_pipeline(AlrIcdCmdLog *L, uint32_t bindPoint,
                                             uint32_t vpipeline) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_BIND_PIPELINE);
    alr_icd_cmdlog_u32(L, bindPoint);
    alr_icd_cmdlog_u32(L, vpipeline);
}
static inline void alr_icd_cmd_bind_index_buffer(AlrIcdCmdLog *L, uint32_t vbuffer,
                                                 uint64_t offset, uint32_t indexType) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_BIND_INDEX_BUFFER);
    alr_icd_cmdlog_u32(L, vbuffer);
    alr_icd_cmdlog_u64(L, offset);
    alr_icd_cmdlog_u32(L, indexType);
}
static inline void alr_icd_cmd_draw(AlrIcdCmdLog *L, uint32_t vtx, uint32_t inst,
                                    uint32_t firstVtx, uint32_t firstInst) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_DRAW);
    alr_icd_cmdlog_u32(L, vtx); alr_icd_cmdlog_u32(L, inst);
    alr_icd_cmdlog_u32(L, firstVtx); alr_icd_cmdlog_u32(L, firstInst);
}
static inline void alr_icd_cmd_draw_indexed(AlrIcdCmdLog *L, uint32_t indexCount,
                                            uint32_t instanceCount, uint32_t firstIndex,
                                            int32_t vertexOffset, uint32_t firstInstance) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_DRAW_INDEXED);
    alr_icd_cmdlog_u32(L, indexCount); alr_icd_cmdlog_u32(L, instanceCount);
    alr_icd_cmdlog_u32(L, firstIndex); alr_icd_cmdlog_i32(L, vertexOffset);
    alr_icd_cmdlog_u32(L, firstInstance);
}
static inline void alr_icd_cmd_draw_indirect(AlrIcdCmdLog *L, uint32_t vbuffer, uint64_t offset,
                                             uint32_t drawCount, uint32_t stride) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_DRAW_INDIRECT);
    alr_icd_cmdlog_u32(L, vbuffer); alr_icd_cmdlog_u64(L, offset);
    alr_icd_cmdlog_u32(L, drawCount); alr_icd_cmdlog_u32(L, stride);
}
static inline void alr_icd_cmd_dispatch(AlrIcdCmdLog *L, uint32_t gx, uint32_t gy, uint32_t gz) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_DISPATCH);
    alr_icd_cmdlog_u32(L, gx); alr_icd_cmdlog_u32(L, gy); alr_icd_cmdlog_u32(L, gz);
}
static inline void alr_icd_cmd_end_render_pass(AlrIcdCmdLog *L) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_END_RENDER_PASS);
}
static inline void alr_icd_cmd_next_subpass(AlrIcdCmdLog *L, uint32_t contents) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_NEXT_SUBPASS);
    alr_icd_cmdlog_u32(L, contents);
}
static inline void alr_icd_cmd_push_constants(AlrIcdCmdLog *L, uint32_t vlayout,
                                              uint32_t stageFlags, uint32_t offset,
                                              uint32_t size, const void *data) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_PUSH_CONSTANTS);
    alr_icd_cmdlog_u32(L, vlayout); alr_icd_cmdlog_u32(L, stageFlags);
    alr_icd_cmdlog_u32(L, offset); alr_icd_cmdlog_u32(L, size);
    alr_icd_cmdlog_raw(L, data, size);
}
/* BeginRenderPass opens with the fixed fields + the clear count; the agent then appends the
 * clear values (16 bytes each, raw VkClearValue) via alr_icd_cmdlog_raw. */
static inline void alr_icd_cmd_begin_render_pass(AlrIcdCmdLog *L, uint32_t vrenderpass,
                                                 uint32_t vframebuffer, int32_t rx, int32_t ry,
                                                 uint32_t rw, uint32_t rh, uint32_t contents,
                                                 uint32_t clear_count) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_BEGIN_RENDER_PASS);
    alr_icd_cmdlog_u32(L, vrenderpass); alr_icd_cmdlog_u32(L, vframebuffer);
    alr_icd_cmdlog_i32(L, rx); alr_icd_cmdlog_i32(L, ry);
    alr_icd_cmdlog_u32(L, rw); alr_icd_cmdlog_u32(L, rh);
    alr_icd_cmdlog_u32(L, contents); alr_icd_cmdlog_u32(L, clear_count);
}
/* BindVertexBuffers opens with first+count; the agent appends `count` × { u32 vbuf, u64 off }. */
static inline void alr_icd_cmd_bind_vertex_buffers_begin(AlrIcdCmdLog *L, uint32_t firstBinding,
                                                         uint32_t count) {
    alr_icd_cmdlog_cmd(L, ALR_ICD_CMD_BIND_VERTEX_BUFFERS);
    alr_icd_cmdlog_u32(L, firstBinding); alr_icd_cmdlog_u32(L, count);
}
static inline void alr_icd_cmd_bind_vertex_buffer_entry(AlrIcdCmdLog *L, uint32_t vbuf,
                                                        uint64_t off) {
    alr_icd_cmdlog_u32(L, vbuf); alr_icd_cmdlog_u64(L, off);
}
/* Finalize: write the END terminator. Returns the recorded length INCLUDING the terminator
 * (0 on overflow). The submit then ships [buf, len). */
static inline uint32_t alr_icd_cmdlog_end(AlrIcdCmdLog *L) {
    alr_icd_cmdlog_u16(L, (uint16_t)ALR_ICD_CMD_END);
    return L->overflow ? 0u : L->len;
}

/* ===========================================================================
 * (3) The vkQueueSubmit + sync RING op builders (over the ICD's AlrVkEncoder). These DO cross
 * the ring (unlike the per-cmd records). Build into a request buffer, alr_icd_roundtrip it,
 * scan the reply with alr_icd_cmd_scan_result / _scan_submit.
 * =========================================================================== */

static inline void alr_icd_cmd_escape(AlrVkEncoder *e, uint16_t sub) {
    alr_vk_enc_u8(e, (uint8_t)ALR_ICD_VK_GEN_ESCAPE);
    alr_vk_enc_raw(e, &sub, 2);  /* u16 sub-opcode */
}
static inline void alr_icd_cmd_create_fence(AlrVkEncoder *e, uint32_t vdev, uint32_t vfence,
                                            uint32_t flags) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_CREATE_FENCE);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, vfence); alr_vk_enc_u32(e, flags);
}
static inline void alr_icd_cmd_destroy_fence(AlrVkEncoder *e, uint32_t vdev, uint32_t vfence) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_DESTROY_FENCE);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, vfence);
}
static inline void alr_icd_cmd_create_semaphore(AlrVkEncoder *e, uint32_t vdev, uint32_t vsem,
                                                uint32_t flags) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_CREATE_SEMAPHORE);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, vsem); alr_vk_enc_u32(e, flags);
}
static inline void alr_icd_cmd_destroy_semaphore(AlrVkEncoder *e, uint32_t vdev, uint32_t vsem) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_DESTROY_SEMAPHORE);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, vsem);
}
static inline void alr_icd_cmd_queue_wait_idle(AlrVkEncoder *e, uint32_t vqueue) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_QUEUE_WAIT_IDLE);
    alr_vk_enc_u32(e, vqueue);
}
static inline void alr_icd_cmd_device_wait_idle(AlrVkEncoder *e, uint32_t vdev) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_DEVICE_WAIT_IDLE);
    alr_vk_enc_u32(e, vdev);
}
static inline void alr_icd_cmd_get_fence_status(AlrVkEncoder *e, uint32_t vdev, uint32_t vfence) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_GET_FENCE_STATUS);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, vfence);
}
/* wait_for_fences / reset_fences open with the count; the caller appends `count` × u32 vfence
 * via alr_vk_enc_u32 (they fit before ALR_VK_OP_END). */
static inline void alr_icd_cmd_wait_for_fences_begin(AlrVkEncoder *e, uint32_t vdev,
                                                     uint32_t wait_all, uint64_t timeout,
                                                     uint32_t count) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_WAIT_FOR_FENCES);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, wait_all);
    alr_vk_enc_u64(e, timeout); alr_vk_enc_u32(e, count);
}
static inline void alr_icd_cmd_reset_fences_begin(AlrVkEncoder *e, uint32_t vdev,
                                                  uint32_t count) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_RESET_FENCES);
    alr_vk_enc_u32(e, vdev); alr_vk_enc_u32(e, count);
}

/* vkQueueSubmit (the recording replay point). Built incrementally: begin (queue+fence), then
 * the wait list (count first), the signal list, then each cmd-buffer { vcmd, log_off, log_len,
 * [inline log bytes if log_off == NO_OFFSET] }. */
static inline void alr_icd_cmd_submit_begin(AlrVkEncoder *e, uint32_t vqueue, uint32_t vfence) {
    alr_icd_cmd_escape(e, (uint16_t)ALR_ICD_CMD_SUBOP_QUEUE_SUBMIT);
    alr_vk_enc_u32(e, vqueue); alr_vk_enc_u32(e, vfence);
}
static inline void alr_icd_cmd_submit_wait_count(AlrVkEncoder *e, uint32_t n) { alr_vk_enc_u32(e, n); }
static inline void alr_icd_cmd_submit_wait(AlrVkEncoder *e, uint32_t vsem, uint32_t stage_lo) {
    alr_vk_enc_u32(e, vsem); alr_vk_enc_u32(e, stage_lo);
}
static inline void alr_icd_cmd_submit_signal_count(AlrVkEncoder *e, uint32_t n) { alr_vk_enc_u32(e, n); }
static inline void alr_icd_cmd_submit_signal(AlrVkEncoder *e, uint32_t vsem) { alr_vk_enc_u32(e, vsem); }
static inline void alr_icd_cmd_submit_cmd_count(AlrVkEncoder *e, uint32_t n) { alr_vk_enc_u32(e, n); }
/* Add one cmd-buffer to the submit, shipping its recorded log INLINE (the portable path). */
static inline void alr_icd_cmd_submit_cmd_inline(AlrVkEncoder *e, uint32_t vcmd,
                                                 const uint8_t *log, uint32_t log_len) {
    alr_vk_enc_u32(e, vcmd);
    alr_vk_enc_u64(e, ALR_ICD_CMD_ARENA_NO_OFFSET);
    alr_vk_enc_u32(e, log_len);
    if (log && log_len) alr_vk_enc_raw(e, log, log_len);
}
/* Add one cmd-buffer whose log lives in the shared arena at `log_off` (zero-copy; no bytes
 * on the wire). The future arena-resident-log rung uses this. */
static inline void alr_icd_cmd_submit_cmd_arena(AlrVkEncoder *e, uint32_t vcmd,
                                                uint64_t log_off, uint32_t log_len) {
    alr_vk_enc_u32(e, vcmd);
    alr_vk_enc_u64(e, log_off);
    alr_vk_enc_u32(e, log_len);
}

/* ===========================================================================
 * (4) Reply scanners. The host writes ALR_VK_REPLY_GEN_ESCAPE + u16 sub + payload, terminated
 * by ALR_VK_REPLY_END (0). These walk it for the cmd-log results. Use the ICD's AlrVkReader.
 * =========================================================================== */

/* Scan for the submit reply { u32 vqueue, i32 vk_result, i32 replay_result }. Returns 1 if
 * found (and fills the out params), 0 otherwise. AlrVkReader comes from alr_icd_gen_glue.h. */
static inline int alr_icd_cmd_scan_submit(const uint8_t *buf, uint32_t len, uint32_t want_queue,
                                          int32_t *vkr_out, int32_t *replay_out) {
    AlrVkReader rd; uint8_t op; uint16_t sub;
    alr_vk_reader_init(&rd, buf, len);
    for (;;) {
        if (!alr_vk_reader_u8(&rd, &op)) return 0;
        if (op == 0u /*ALR_VK_REPLY_END*/) return 0;
        if (op != (uint8_t)ALR_ICD_VK_REPLY_GEN_ESCAPE) return 0;
        if (!alr_vk_reader_u16(&rd, &sub)) return 0;
        if (sub == (uint16_t)ALR_ICD_CMD_REPLY_SUBMIT) {
            uint32_t vq; int32_t vkr, rr;
            if (!alr_vk_reader_u32(&rd, &vq) || !alr_vk_reader_i32(&rd, &vkr) ||
                !alr_vk_reader_i32(&rd, &rr)) return 0;
            if (vq == want_queue) { if (vkr_out) *vkr_out = vkr; if (replay_out) *replay_out = rr; return 1; }
        } else if (sub == (uint16_t)ALR_ICD_CMD_REPLY_RESULT) {
            uint32_t key; int32_t res;
            if (!alr_vk_reader_u32(&rd, &key) || !alr_vk_reader_i32(&rd, &res)) return 0;
        } else {
            return 0;  /* a non-cmd-log reply record: stop (don't guess its size) */
        }
    }
}

/* Scan for a result reply { u32 key, i32 result } whose key matches (create/wait/status). */
static inline int alr_icd_cmd_scan_result(const uint8_t *buf, uint32_t len, uint32_t want_key,
                                          int32_t *res_out) {
    AlrVkReader rd; uint8_t op; uint16_t sub;
    alr_vk_reader_init(&rd, buf, len);
    for (;;) {
        if (!alr_vk_reader_u8(&rd, &op)) return 0;
        if (op == 0u) return 0;
        if (op != (uint8_t)ALR_ICD_VK_REPLY_GEN_ESCAPE) return 0;
        if (!alr_vk_reader_u16(&rd, &sub)) return 0;
        if (sub == (uint16_t)ALR_ICD_CMD_REPLY_RESULT) {
            uint32_t key; int32_t res;
            if (!alr_vk_reader_u32(&rd, &key) || !alr_vk_reader_i32(&rd, &res)) return 0;
            if (key == want_key) { if (res_out) *res_out = res; return 1; }
        } else if (sub == (uint16_t)ALR_ICD_CMD_REPLY_SUBMIT) {
            uint32_t vq; int32_t vkr, rr;
            if (!alr_vk_reader_u32(&rd, &vq) || !alr_vk_reader_i32(&rd, &vkr) ||
                !alr_vk_reader_i32(&rd, &rr)) return 0;
        } else {
            return 0;
        }
    }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALR_GPU_GUEST_ICD_ALR_ICD_CMD_RECORD_H */
