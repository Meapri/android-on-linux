// alr_gpu_vk_cmd_dispatch.hpp — the RING-LEVEL host dispatcher for the cmd-log band.
//
// The cmd-log band (alr_gpu_vk_cmdlog.hpp) has two layers:
//   * the per-command-buffer RECORD LOG (vkCmd* records, guest-local in the arena), replayed
//     by cmd_replay_log_* — that's the cmdlog header; and
//   * the RING ops that DO cross the wire: vkQueueSubmit (ship the submit topology), the
//     fence/semaphore create/destroy/wait/reset/status sync ops, and queue/device-wait-idle.
// THIS header is the host half of those RING ops. It rides ALR_VK_OP_GEN_ESCAPE (230) on the
// 0x4000.. u16 sub-opcode band (disjoint from the create-forwards codegen's 1.. band), and
// self-registers a SECOND dispatcher into decode_vk_batch's default-case seam
// (set_vk_cmd_dispatch) so the two bands coexist — the create-forwards agent owns
// vk_gen_dispatch (low sub-ops), this owns vk_cmd_dispatch (0x4000 sub-ops).
//
// On vkQueueSubmit the host: for each listed cmd-buffer vid, locates its record log in the
// shared arena (via the log_off the guest shipped), allocates+begins a real Mali
// VkCommandBuffer, replays the log into it (cmd_replay_log_real — translate guest vids to
// real handles, call the real vkCmd*), ends it, and accumulates it; then translates the
// wait/signal semaphores + fence and issues the real vkQueueSubmit on the owner thread.
//
// A synthetic-Mali PROVIDER seam (VkCmdProvider) lets the host wire test drive the submit +
// sync ops with NO Vulkan SDK (it asserts the submit topology + cmd-log decode round-trips);
// under ALR_VK_DECODE_REAL with a null provider the real-Mali path runs.
//
// Header-only + self-contained, matching the alr_gpu/** rule.

#ifndef ALR_GPU_ALR_GPU_VK_CMD_DISPATCH_HPP
#define ALR_GPU_ALR_GPU_VK_CMD_DISPATCH_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "alr_gpu/alr_gpu_vk_cmdlog.hpp"            // sub-opcodes + cmd_replay_log_*
#include "alr_gpu/alr_gpu_vk_decode.hpp"            // VkDecodeState, VkReader, VkReplyEncoder
#include "alr_gpu/generated/alr_gpu_vk_arena.hpp"    // arena offset -> host pointer
// The generated proto header defines the SHARED escape bytes ALR_VK_OP_GEN_ESCAPE /
// ALR_VK_REPLY_GEN_ESCAPE (== 230) the cmd-log band rides (alongside the create-forwards
// codegen, on a disjoint u16 sub-op band). Pulled here so this band uses the same escape
// constant by name.
#include "alr_gpu/generated/alr_gpu_vk_gen_proto.hpp"

namespace alr::gpu {

// ---------------------------------------------------------------------------
// The synthetic-Mali provider for the cmd-log ring ops (wire-test seam; null on device ->
// real-Mali path). Mirrors VkProvider/VkGenProvider. A submit provides the replay of every
// listed cmd-buffer log (the test decodes the log via cmd_replay_log_wire and asserts the
// records); the sync providers answer create/wait/reset/status.
// ---------------------------------------------------------------------------
struct VkCmdSubmitInfo {
    uint32_t vqueue = 0;
    uint32_t vfence = 0;
    std::vector<std::pair<uint32_t, uint32_t>> waits;    // (vsem, stage_mask_lo)
    std::vector<uint32_t> signals;                       // vsem
    // (vcmd, log pointer, log_len). The log pointer is resolved from the arena offset (or a
    // null pointer if the offset was the no-arena sentinel — the provider then has no bytes).
    struct Cmd { uint32_t vcmd; const uint8_t* log; uint32_t log_len; };
    std::vector<Cmd> cmds;
};

struct VkCmdProvider {
    // Replay the submit. Return { vk_result, replay_result } (0/0 == ok). The provider
    // decodes each cmd's log itself (the wire test uses cmd_replay_log_wire). Null on device.
    bool (*submit)(void* ctx, const VkCmdSubmitInfo& info, int32_t* vk_result_out,
                   int32_t* replay_result_out) = nullptr;
    int (*create_fence)(void* ctx, uint32_t vdev, uint32_t vfence, uint32_t flags) = nullptr;
    void (*destroy_fence)(void* ctx, uint32_t vdev, uint32_t vfence) = nullptr;
    int (*wait_fences)(void* ctx, uint32_t vdev, uint32_t wait_all, uint64_t timeout,
                       const std::vector<uint32_t>& vfences) = nullptr;
    int (*reset_fences)(void* ctx, uint32_t vdev, const std::vector<uint32_t>& vfences) = nullptr;
    int (*get_fence_status)(void* ctx, uint32_t vdev, uint32_t vfence) = nullptr;
    int (*create_semaphore)(void* ctx, uint32_t vdev, uint32_t vsem, uint32_t flags) = nullptr;
    void (*destroy_semaphore)(void* ctx, uint32_t vdev, uint32_t vsem) = nullptr;
    int (*queue_wait_idle)(void* ctx, uint32_t vqueue) = nullptr;
    int (*device_wait_idle)(void* ctx, uint32_t vdev) = nullptr;
    void* ctx = nullptr;
};

// NOTE: the opaque cmd-provider pointer the wire test injects is set via
// set_vk_cmd_provider_seam() (declared in alr_gpu_vk_decode.hpp next to the gen seam);
// decode_vk_batch forwards vk_cmd_provider_ptr_seam() to this band's adapter. There is no
// separate provider holder here — one seam, owned by the decoder, keeps the two bands
// symmetric (the create-forwards band uses set_vk_gen_provider the same way).

// ---------------------------------------------------------------------------
// Read the QUEUE_SUBMIT topology off the reader into `info`, resolving each cmd-buffer's
// arena log offset to a host pointer. Returns false on a malformed op.
// ---------------------------------------------------------------------------
inline bool cmd_read_submit(VkReader& r, VkCmdSubmitInfo& info) {
    if (!r.u32(info.vqueue) || !r.u32(info.vfence)) return false;
    uint32_t wait_count = 0;
    if (!r.u32(wait_count)) return false;
    if (wait_count > 64) return false;
    for (uint32_t i = 0; i < wait_count; ++i) {
        uint32_t vsem = 0, stage = 0;
        if (!r.u32(vsem) || !r.u32(stage)) return false;
        info.waits.emplace_back(vsem, stage);
    }
    uint32_t signal_count = 0;
    if (!r.u32(signal_count)) return false;
    if (signal_count > 64) return false;
    for (uint32_t i = 0; i < signal_count; ++i) {
        uint32_t vsem = 0;
        if (!r.u32(vsem)) return false;
        info.signals.push_back(vsem);
    }
    uint32_t cmd_count = 0;
    if (!r.u32(cmd_count)) return false;
    if (cmd_count > 256) return false;
    for (uint32_t i = 0; i < cmd_count; ++i) {
        uint32_t vcmd = 0, log_len = 0; uint64_t log_off = 0;
        if (!r.u32(vcmd) || !r.u64(log_off) || !r.u32(log_len)) return false;
        const uint8_t* log = nullptr;
        if (log_off != kAlrVkArenaNoOffset) {
            // ZERO-COPY (the primary path): the log lives in the shared arena; the host reads
            // it straight from there — the recorded bytes NEVER cross the ring. Bound it
            // against the arena size so a bogus len can't over-read.
            log = static_cast<const uint8_t*>(alr_vk_arena_ptr(log_off));
            if (log && (log_off + log_len > alr_vk_arena_size())) { log = nullptr; log_len = 0; }
        } else if (log_len > 0) {
            // INLINE FALLBACK (no arena, e.g. a guest with no shared region or a log too big
            // for its arena slot): the `log_len` recorded bytes follow inline on the wire. A
            // copy, but correctness over zero-copy when the arena path is unavailable. The
            // reader borrows them in place (valid for the duration of this batch decode).
            if (!r.take_raw(log, log_len)) return false;
        }
        info.cmds.push_back({vcmd, log, log_len});
    }
    return true;
}

}  // namespace alr::gpu

// The real-Mali ring dispatch (replay each log into a real VkCommandBuffer + real
// vkQueueSubmit + the sync calls) is in a companion header compiled only under
// ALR_VK_DECODE_REAL, so this header stays SDK-free for the wire test.
#ifdef ALR_VK_DECODE_REAL
#include "alr_gpu/alr_gpu_vk_cmd_dispatch_real.hpp"
#endif

namespace alr::gpu {

// Append one ALR_VK_CMD_REPLY_RESULT { u32 key, i32 result } record (used by the wait /
// create / status ops). Declared before decode_vk_cmd_op, which calls it.
inline void cmd_reply_result(VkReplyEncoder& reply, uint32_t key, int32_t result) {
    reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
    reply.u16(static_cast<uint16_t>(ALR_VK_CMD_REPLY_RESULT));
    reply.u32(key);
    reply.i32(result);
}

// ---------------------------------------------------------------------------
// decode_vk_cmd_op — dispatch ONE cmd-log RING op. `op` is the u8 already read by
// decode_vk_batch; we only claim ALR_VK_OP_GEN_ESCAPE with a sub-opcode in the 0x4000 band.
// On a sub-op outside our band we return false WITHOUT consuming anything beyond the u16 we
// must peek — but because the create-forwards dispatcher runs FIRST and consumes its own
// (low) sub-ops, by the time we are called for the escape the sub-op is ours-or-unknown. To
// stay robust we read the u16; if it is below our base we treat it as "not ours" and signal
// the caller (return false) WITHOUT touching st.ok — but we have already consumed the u16,
// so the only safe contract is: the cmd dispatcher is consulted ONLY for escapes the gen
// dispatcher declined, and an escape sub-op below 0x4000 that reached here is malformed
// (the gen dispatcher should have claimed it). We therefore fail-stop on a sub-op below our
// base (it cannot legally reach us), and handle 0x4000.. ourselves.
// ---------------------------------------------------------------------------
inline bool decode_vk_cmd_op(uint8_t op, VkReader& r, VkDecodeState& st, VkReplyEncoder& reply,
                             const VkCmdProvider* cp) {
    if (op != ALR_VK_OP_GEN_ESCAPE) return false;  // not ours
    uint16_t sub = 0;
    if (!r.u16(sub)) { st.ok = false; return true; }
    if (sub < ALR_VK_CMD_SUBOP_BASE) {
        // A low sub-op reached the cmd dispatcher — the gen dispatcher should have claimed
        // it. Treat as off-contract (fail-stop) rather than silently mis-decode.
        st.ok = false;
        return true;
    }
    switch (sub) {
        case ALR_VK_CMD_SUBOP_QUEUE_SUBMIT: {
            VkCmdSubmitInfo info;
            if (!cmd_read_submit(r, info)) { st.ok = false; return true; }
            int32_t vk_result = -1, replay_result = ALR_VK_CMD_REPLAY_OK;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) {
                vk_result = cmd_real_queue_submit(st, info, &replay_result);
            }
#endif
            if (cp && cp->submit)
                cp->submit(cp->ctx, info, &vk_result, &replay_result);
            reply.u8(static_cast<uint8_t>(ALR_VK_REPLY_GEN_ESCAPE));
            reply.u16(static_cast<uint16_t>(ALR_VK_CMD_REPLY_SUBMIT));
            reply.u32(info.vqueue);
            reply.i32(vk_result);
            reply.i32(replay_result);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_QUEUE_WAIT_IDLE: {
            uint32_t vqueue = 0;
            if (!r.u32(vqueue)) { st.ok = false; return true; }
            int32_t res = 0;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_queue_wait_idle(st, vqueue);
#endif
            if (cp && cp->queue_wait_idle) res = cp->queue_wait_idle(cp->ctx, vqueue);
            cmd_reply_result(reply, vqueue, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_DEVICE_WAIT_IDLE: {
            uint32_t vdev = 0;
            if (!r.u32(vdev)) { st.ok = false; return true; }
            int32_t res = 0;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_device_wait_idle(st, vdev);
#endif
            if (cp && cp->device_wait_idle) res = cp->device_wait_idle(cp->ctx, vdev);
            cmd_reply_result(reply, vdev, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_CREATE_FENCE: {
            uint32_t vdev = 0, vfence = 0, flags = 0;
            if (!r.u32(vdev) || !r.u32(vfence) || !r.u32(flags)) { st.ok = false; return true; }
            int32_t res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_create_fence(st, vdev, vfence, flags);
#endif
            if (cp && cp->create_fence) res = cp->create_fence(cp->ctx, vdev, vfence, flags);
            cmd_reply_result(reply, vfence, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_DESTROY_FENCE: {
            uint32_t vdev = 0, vfence = 0;
            if (!r.u32(vdev) || !r.u32(vfence)) { st.ok = false; return true; }
#ifdef ALR_VK_DECODE_REAL
            if (!cp) cmd_real_destroy_fence(st, vdev, vfence);
#endif
            if (cp && cp->destroy_fence) cp->destroy_fence(cp->ctx, vdev, vfence);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_WAIT_FOR_FENCES: {
            uint32_t vdev = 0, wait_all = 0, count = 0; uint64_t timeout = 0;
            if (!r.u32(vdev) || !r.u32(wait_all) || !r.u64(timeout) || !r.u32(count)) { st.ok = false; return true; }
            if (count > 256) { st.ok = false; return true; }
            std::vector<uint32_t> vfences(count);
            for (uint32_t i = 0; i < count; ++i)
                if (!r.u32(vfences[i])) { st.ok = false; return true; }
            int32_t res = 0;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_wait_fences(st, vdev, wait_all, timeout, vfences);
#endif
            if (cp && cp->wait_fences)
                res = cp->wait_fences(cp->ctx, vdev, wait_all, timeout, vfences);
            cmd_reply_result(reply, vdev, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_RESET_FENCES: {
            uint32_t vdev = 0, count = 0;
            if (!r.u32(vdev) || !r.u32(count)) { st.ok = false; return true; }
            if (count > 256) { st.ok = false; return true; }
            std::vector<uint32_t> vfences(count);
            for (uint32_t i = 0; i < count; ++i)
                if (!r.u32(vfences[i])) { st.ok = false; return true; }
            int32_t res = 0;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_reset_fences(st, vdev, vfences);
#endif
            if (cp && cp->reset_fences) res = cp->reset_fences(cp->ctx, vdev, vfences);
            cmd_reply_result(reply, vdev, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_GET_FENCE_STATUS: {
            uint32_t vdev = 0, vfence = 0;
            if (!r.u32(vdev) || !r.u32(vfence)) { st.ok = false; return true; }
            int32_t res = 0;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_get_fence_status(st, vdev, vfence);
#endif
            if (cp && cp->get_fence_status) res = cp->get_fence_status(cp->ctx, vdev, vfence);
            cmd_reply_result(reply, vfence, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_CREATE_SEMAPHORE: {
            uint32_t vdev = 0, vsem = 0, flags = 0;
            if (!r.u32(vdev) || !r.u32(vsem) || !r.u32(flags)) { st.ok = false; return true; }
            int32_t res = -1;
#ifdef ALR_VK_DECODE_REAL
            if (!cp) res = cmd_real_create_semaphore(st, vdev, vsem, flags);
#endif
            if (cp && cp->create_semaphore) res = cp->create_semaphore(cp->ctx, vdev, vsem, flags);
            cmd_reply_result(reply, vsem, res);
            st.decoded++;
            return true;
        }
        case ALR_VK_CMD_SUBOP_DESTROY_SEMAPHORE: {
            uint32_t vdev = 0, vsem = 0;
            if (!r.u32(vdev) || !r.u32(vsem)) { st.ok = false; return true; }
#ifdef ALR_VK_DECODE_REAL
            if (!cp) cmd_real_destroy_semaphore(st, vdev, vsem);
#endif
            if (cp && cp->destroy_semaphore) cp->destroy_semaphore(cp->ctx, vdev, vsem);
            st.decoded++;
            return true;
        }
        default:
            st.ok = false;  // escape with an unknown 0x4000.. sub-op: fail-stop
            return true;
    }
}

// ---- Self-registration into decode_vk_batch's SECOND generated-op seam. The opaque
// gen_provider from decode_vk_batch's cmd-seam is the VkCmdProvider* the caller set (null on
// device -> real-Mali). #including this header wires the 0x4000.. band into decode_vk_batch.
inline bool vk_cmd_dispatch_adapter(uint8_t op, VkReader& r, VkDecodeState& st,
                                    VkReplyEncoder& reply, const void* cmd_provider) {
    return decode_vk_cmd_op(op, r, st, reply,
                            static_cast<const VkCmdProvider*>(cmd_provider));
}
inline bool vk_cmd_register_dispatch() {
    set_vk_cmd_dispatch(&vk_cmd_dispatch_adapter);
    return true;
}
inline const bool kVkCmdDispatchRegistered = vk_cmd_register_dispatch();

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_VK_CMD_DISPATCH_HPP
