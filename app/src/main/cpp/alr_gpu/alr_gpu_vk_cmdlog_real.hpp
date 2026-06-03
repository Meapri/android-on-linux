// alr_gpu_vk_cmdlog_real.hpp — HAND-WRITTEN real-Mali replay for the cmd-log band.
//
// Compiled ONLY under ALR_VK_DECODE_REAL (the on-device path; runtime_report.cpp defines
// it). Reads ONE command-buffer's record log (the bytes the guest recorded into the shared
// arena, located via its arena offset) and REPLAYS each record into a real Mali
// VkCommandBuffer — translating guest virtual ids to real Mali handles and calling the real
// vkCmd* on the vendor libvulkan — then the host's submit path issues the real vkQueueSubmit
// on the owner thread.
//
// FIELD ORDER is NOT duplicated: this file reuses cmd_decode_one() (alr_gpu_vk_cmdlog.hpp)
// to walk each record's fixed operands in lock-step, then — for ops with variable arrays —
// re-reads the same arrays here off the SAME reader cmd_decode_one left positioned. (For the
// fixed-only ops, cmd_decode_one's emit callback carries the operands and we call vkCmd*
// directly; for array ops we read the arrays a second time because vkCmd* needs the full
// VkStructs. The reader passed to cmd_decode_one and the one we read from are the SAME, so
// no drift is possible.) This keeps the wire codec (SDK-free) the single source of truth for
// the byte layout while the real Vulkan struct-building stays auditable here.
//
// HANDLE TRANSLATION: guest vids -> real Mali handles.
//   * Buffers / images: VkGenTables (the create-forwards codegen's tables) — gen_tables(st).
//   * Fences / semaphores: cmd_tables(st) (THIS band owns them).
//   * Pipelines / descriptor sets / pipeline layouts / render passes / framebuffers: the
//     create-forwards codegen will add these as it grows; until then they resolve through
//     cmd_tables(st)'s generic-handle maps, which the host servicer populates from whatever
//     create band owns them. An unresolved handle aborts the record's replay with
//     ALR_VK_CMD_REPLAY_HANDLE (never passes a bogus handle to Mali). This is the composition
//     seam with the create-forwards agent: it fills the maps, this file consumes them.

#ifndef ALR_GPU_ALR_GPU_VK_CMDLOG_REAL_HPP
#define ALR_GPU_ALR_GPU_VK_CMDLOG_REAL_HPP

#ifdef ALR_VK_DECODE_REAL

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "alr_gpu/alr_gpu_vk_cmdlog.hpp"            // AlrVkCmd opcodes + cmd_decode_one
#include "alr_gpu/alr_gpu_vk_decode.hpp"            // VkDecodeState
#include "alr_gpu/generated/alr_gpu_vk_arena.hpp"    // arena offset -> host pointer
#include "alr_gpu/generated/alr_gpu_vk_gen_decode.hpp"  // VkGenTables + gen_tables(st)

namespace alr::gpu {

// ---------------------------------------------------------------------------
// Side handle tables this band owns + the COMPOSITION SEAM for handles the create-forwards
// band creates. Keyed by the owning VkDecodeState's address (same pattern as gen_tables),
// so the cmd band finds them without editing the hand-written VkDecodeState struct.
// ---------------------------------------------------------------------------
struct VkCmdTables {
    // Sync objects this band creates (vkCreateFence/Semaphore on the cmd-log sub-ops).
    std::map<uint32_t, VkFence> fences;          // vfence -> real
    std::map<uint32_t, VkSemaphore> semaphores;  // vsem   -> real

    // Generic real-handle maps for the resource kinds vkCmd* references that the
    // create-forwards codegen owns the CREATION of (pipeline / descriptor set / pipeline
    // layout / render pass / framebuffer). The host servicer registers each real handle here
    // when the corresponding create op runs (the create-forwards agent calls cmd_register_*
    // from its real bodies). Until those creates exist, a vkCmd* that references one
    // resolves to VK_NULL_HANDLE and the record fails-safe (ALR_VK_CMD_REPLAY_HANDLE).
    std::map<uint32_t, VkPipeline> pipelines;          // vpipeline -> real
    std::map<uint32_t, VkPipelineLayout> layouts;      // vlayout   -> real
    std::map<uint32_t, VkDescriptorSet> descsets;      // vdescset  -> real
    std::map<uint32_t, VkRenderPass> renderpasses;     // vrp       -> real
    std::map<uint32_t, VkFramebuffer> framebuffers;    // vfb       -> real
};
inline VkCmdTables& cmd_tables(VkDecodeState& st) {
    static std::map<const VkDecodeState*, VkCmdTables> g;
    return g[&st];
}

// Registration helpers the create-forwards real bodies call so vkCmd* can translate the
// handle they created. (Inline so they merge cleanly — the create agent adds one call per
// create body; no shared struct is edited.)
inline void cmd_register_pipeline(VkDecodeState& st, uint32_t vid, VkPipeline h) { cmd_tables(st).pipelines[vid] = h; }
inline void cmd_register_layout(VkDecodeState& st, uint32_t vid, VkPipelineLayout h) { cmd_tables(st).layouts[vid] = h; }
inline void cmd_register_descset(VkDecodeState& st, uint32_t vid, VkDescriptorSet h) { cmd_tables(st).descsets[vid] = h; }
inline void cmd_register_renderpass(VkDecodeState& st, uint32_t vid, VkRenderPass h) { cmd_tables(st).renderpasses[vid] = h; }
inline void cmd_register_framebuffer(VkDecodeState& st, uint32_t vid, VkFramebuffer h) { cmd_tables(st).framebuffers[vid] = h; }

// Handle resolvers (return VK_NULL_HANDLE if the guest vid is unknown). A vid of 0 always
// means VK_NULL_HANDLE (the guest encodes a null handle as 0).
inline VkBuffer cmd_real_buffer(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto& t = gen_tables(st);
    auto it = t.buffers.find(vid);
    return it == t.buffers.end() ? VK_NULL_HANDLE : it->second;
}
inline VkImage cmd_real_image(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto& t = gen_tables(st);
    auto it = t.images.find(vid);
    return it == t.images.end() ? VK_NULL_HANDLE : it->second;
}
inline VkPipeline cmd_real_pipeline(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).pipelines.find(vid);
    return it == cmd_tables(st).pipelines.end() ? VK_NULL_HANDLE : it->second;
}
inline VkPipelineLayout cmd_real_layout(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).layouts.find(vid);
    return it == cmd_tables(st).layouts.end() ? VK_NULL_HANDLE : it->second;
}
inline VkDescriptorSet cmd_real_descset(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).descsets.find(vid);
    return it == cmd_tables(st).descsets.end() ? VK_NULL_HANDLE : it->second;
}
inline VkRenderPass cmd_real_renderpass(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).renderpasses.find(vid);
    return it == cmd_tables(st).renderpasses.end() ? VK_NULL_HANDLE : it->second;
}
inline VkFramebuffer cmd_real_framebuffer(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).framebuffers.find(vid);
    return it == cmd_tables(st).framebuffers.end() ? VK_NULL_HANDLE : it->second;
}

// ---------------------------------------------------------------------------
// cmd_replay_one_real — replay ONE record into the real `cmd` buffer. The u16 opcode was
// already read into `op`; this reads its operands off `r` (the SAME field order
// cmd_decode_one defines) and calls the real vkCmd*. Sets *bad to a ALR_VK_CMD_REPLAY_* on a
// malformed record or an unresolved required handle; returns false then (the caller aborts
// the whole log). For ops with variable arrays, the arrays are rebuilt into local vectors
// here, then handed to the real vkCmd* as the proper VkStructs.
//
// NOTE on field-order safety: this function reads the EXACT same byte sequence cmd_decode_one
// reads (it is the same wire). Where cmd_decode_one merely skips an array, here we capture it.
// The two are kept structurally identical, record by record, in the same file pair.
// ---------------------------------------------------------------------------
inline bool cmd_replay_one_real(uint16_t op, VkReader& r, VkDecodeState& st,
                                VkDevice dev, VkCommandBuffer cmd, int* bad) {
    switch (op) {
        case ALR_VK_CMD_BEGIN_RENDER_PASS:
        case ALR_VK_CMD_BEGIN_RENDER_PASS2: {
            uint32_t vrp = 0, vfb = 0, contents = 0, rw = 0, rh = 0, clear_count = 0;
            int32_t rx = 0, ry = 0;
            if (!r.u32(vrp) || !r.u32(vfb) || !r.i32(rx) || !r.i32(ry) || !r.u32(rw) ||
                !r.u32(rh) || !r.u32(contents) || !r.u32(clear_count)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (clear_count > 16) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkClearValue> clears(clear_count);
            for (uint32_t i = 0; i < clear_count; ++i) {
                const uint8_t* cv = nullptr;
                if (!r.take_raw(cv, 16)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                std::memcpy(&clears[i], cv, 16);
            }
            VkRenderPass rp = cmd_real_renderpass(st, vrp);
            VkFramebuffer fb = cmd_real_framebuffer(st, vfb);
            if (rp == VK_NULL_HANDLE || fb == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            VkRenderPassBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            bi.renderPass = rp;
            bi.framebuffer = fb;
            bi.renderArea.offset = {rx, ry};
            bi.renderArea.extent = {rw, rh};
            bi.clearValueCount = clear_count;
            bi.pClearValues = clears.empty() ? nullptr : clears.data();
            if (op == ALR_VK_CMD_BEGIN_RENDER_PASS) {
                vkCmdBeginRenderPass(cmd, &bi, static_cast<VkSubpassContents>(contents));
            } else {
                // vkCmdBeginRenderPass2 (Vulkan 1.2 core) is NOT a link-time symbol in the
                // NDK 26 libvulkan stub — resolve it via vkGetDeviceProcAddr at runtime (it
                // is present on any device that supports 1.2 / VK_KHR_create_renderpass2,
                // which ANGLE requires before it records the *2 form). Fall back to the
                // non-*2 path if the driver lacks it (semantically identical for our use).
                auto p = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(
                    vkGetDeviceProcAddr(dev, "vkCmdBeginRenderPass2"));
                if (p) {
                    VkSubpassBeginInfo sbi{};
                    sbi.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
                    sbi.contents = static_cast<VkSubpassContents>(contents);
                    p(cmd, &bi, &sbi);
                } else {
                    vkCmdBeginRenderPass(cmd, &bi, static_cast<VkSubpassContents>(contents));
                }
            }
            return true;
        }
        case ALR_VK_CMD_END_RENDER_PASS:
            vkCmdEndRenderPass(cmd);
            return true;
        case ALR_VK_CMD_END_RENDER_PASS2: {
            auto p = reinterpret_cast<PFN_vkCmdEndRenderPass2>(
                vkGetDeviceProcAddr(dev, "vkCmdEndRenderPass2"));
            if (p) {
                VkSubpassEndInfo sei{};
                sei.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
                p(cmd, &sei);
            } else {
                vkCmdEndRenderPass(cmd);
            }
            return true;
        }
        case ALR_VK_CMD_NEXT_SUBPASS: {
            uint32_t contents = 0;
            if (!r.u32(contents)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            vkCmdNextSubpass(cmd, static_cast<VkSubpassContents>(contents));
            return true;
        }
        case ALR_VK_CMD_NEXT_SUBPASS2: {
            uint32_t contents = 0;
            if (!r.u32(contents)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            auto p = reinterpret_cast<PFN_vkCmdNextSubpass2>(
                vkGetDeviceProcAddr(dev, "vkCmdNextSubpass2"));
            if (p) {
                VkSubpassBeginInfo sbi{};
                sbi.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
                sbi.contents = static_cast<VkSubpassContents>(contents);
                VkSubpassEndInfo sei{};
                sei.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
                p(cmd, &sbi, &sei);
            } else {
                vkCmdNextSubpass(cmd, static_cast<VkSubpassContents>(contents));
            }
            return true;
        }
        case ALR_VK_CMD_BIND_PIPELINE: {
            uint32_t bp = 0, vpipe = 0;
            if (!r.u32(bp) || !r.u32(vpipe)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            VkPipeline p = cmd_real_pipeline(st, vpipe);
            if (p == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdBindPipeline(cmd, static_cast<VkPipelineBindPoint>(bp), p);
            return true;
        }
        case ALR_VK_CMD_BIND_DESCRIPTOR_SETS: {
            uint32_t bp = 0, vlayout = 0, firstSet = 0, set_count = 0, dyn_count = 0;
            if (!r.u32(bp) || !r.u32(vlayout) || !r.u32(firstSet) || !r.u32(set_count)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (set_count > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkDescriptorSet> sets(set_count);
            for (uint32_t i = 0; i < set_count; ++i) {
                uint32_t vs = 0; if (!r.u32(vs)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                sets[i] = cmd_real_descset(st, vs);
                if (sets[i] == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            }
            if (!r.u32(dyn_count)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (dyn_count > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<uint32_t> dyn(dyn_count);
            for (uint32_t i = 0; i < dyn_count; ++i)
                if (!r.u32(dyn[i])) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            VkPipelineLayout lay = cmd_real_layout(st, vlayout);
            if (lay == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdBindDescriptorSets(cmd, static_cast<VkPipelineBindPoint>(bp), lay, firstSet,
                                    set_count, sets.empty() ? nullptr : sets.data(),
                                    dyn_count, dyn.empty() ? nullptr : dyn.data());
            return true;
        }
        case ALR_VK_CMD_BIND_VERTEX_BUFFERS: {
            uint32_t firstBinding = 0, count = 0;
            if (!r.u32(firstBinding) || !r.u32(count)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (count > 64) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkBuffer> bufs(count);
            std::vector<VkDeviceSize> offs(count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t vb = 0; uint64_t off = 0;
                if (!r.u32(vb) || !r.u64(off)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                bufs[i] = cmd_real_buffer(st, vb);
                if (bufs[i] == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
                offs[i] = off;
            }
            vkCmdBindVertexBuffers(cmd, firstBinding, count, bufs.empty() ? nullptr : bufs.data(),
                                   offs.empty() ? nullptr : offs.data());
            return true;
        }
        case ALR_VK_CMD_BIND_INDEX_BUFFER: {
            uint32_t vb = 0, indexType = 0; uint64_t off = 0;
            if (!r.u32(vb) || !r.u64(off) || !r.u32(indexType)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            VkBuffer b = cmd_real_buffer(st, vb);
            if (b == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdBindIndexBuffer(cmd, b, off, static_cast<VkIndexType>(indexType));
            return true;
        }
        case ALR_VK_CMD_DRAW: {
            uint32_t vtx = 0, inst = 0, fv = 0, fi = 0;
            if (!r.u32(vtx) || !r.u32(inst) || !r.u32(fv) || !r.u32(fi)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            vkCmdDraw(cmd, vtx, inst, fv, fi);
            return true;
        }
        case ALR_VK_CMD_DRAW_INDEXED: {
            uint32_t ic = 0, inst = 0, fi = 0, firstInst = 0; int32_t vo = 0;
            if (!r.u32(ic) || !r.u32(inst) || !r.u32(fi) || !r.i32(vo) || !r.u32(firstInst)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            vkCmdDrawIndexed(cmd, ic, inst, fi, vo, firstInst);
            return true;
        }
        case ALR_VK_CMD_DRAW_INDIRECT: {
            uint32_t vb = 0, drawCount = 0, stride = 0; uint64_t off = 0;
            if (!r.u32(vb) || !r.u64(off) || !r.u32(drawCount) || !r.u32(stride)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            VkBuffer b = cmd_real_buffer(st, vb);
            if (b == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdDrawIndirect(cmd, b, off, drawCount, stride);
            return true;
        }
        case ALR_VK_CMD_DISPATCH: {
            uint32_t gx = 0, gy = 0, gz = 0;
            if (!r.u32(gx) || !r.u32(gy) || !r.u32(gz)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            vkCmdDispatch(cmd, gx, gy, gz);
            return true;
        }
        case ALR_VK_CMD_SET_VIEWPORT: {
            uint32_t fvp = 0, count = 0;
            if (!r.u32(fvp) || !r.u32(count)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (count > 16) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkViewport> vps(count);
            for (uint32_t i = 0; i < count; ++i) {
                if (!r.f32(vps[i].x) || !r.f32(vps[i].y) || !r.f32(vps[i].width) ||
                    !r.f32(vps[i].height) || !r.f32(vps[i].minDepth) || !r.f32(vps[i].maxDepth)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            }
            vkCmdSetViewport(cmd, fvp, count, vps.empty() ? nullptr : vps.data());
            return true;
        }
        case ALR_VK_CMD_SET_SCISSOR: {
            uint32_t fs = 0, count = 0;
            if (!r.u32(fs) || !r.u32(count)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (count > 16) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkRect2D> rects(count);
            for (uint32_t i = 0; i < count; ++i) {
                int32_t x, y; uint32_t w, h;
                if (!r.i32(x) || !r.i32(y) || !r.u32(w) || !r.u32(h)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                rects[i].offset = {x, y};
                rects[i].extent = {w, h};
            }
            vkCmdSetScissor(cmd, fs, count, rects.empty() ? nullptr : rects.data());
            return true;
        }
        case ALR_VK_CMD_PIPELINE_BARRIER:
        case ALR_VK_CMD_PIPELINE_BARRIER2: {
            uint32_t srcStage = 0, dstStage = 0, depFlags = 0, mem_c = 0, buf_c = 0, img_c = 0;
            if (!r.u32(srcStage) || !r.u32(dstStage) || !r.u32(depFlags) || !r.u32(mem_c)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (mem_c > 64) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkMemoryBarrier> mems(mem_c);
            for (uint32_t i = 0; i < mem_c; ++i) {
                uint32_t sa, da; if (!r.u32(sa) || !r.u32(da)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                mems[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mems[i].srcAccessMask = sa; mems[i].dstAccessMask = da;
            }
            if (!r.u32(buf_c)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (buf_c > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkBufferMemoryBarrier> bufs(buf_c);
            for (uint32_t i = 0; i < buf_c; ++i) {
                uint32_t sa, da, sqf, dqf, vb; uint64_t off, sz;
                if (!r.u32(sa) || !r.u32(da) || !r.u32(sqf) || !r.u32(dqf) || !r.u32(vb) ||
                    !r.u64(off) || !r.u64(sz)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                VkBuffer b = cmd_real_buffer(st, vb);
                if (b == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
                bufs[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                bufs[i].srcAccessMask = sa; bufs[i].dstAccessMask = da;
                bufs[i].srcQueueFamilyIndex = sqf; bufs[i].dstQueueFamilyIndex = dqf;
                bufs[i].buffer = b; bufs[i].offset = off; bufs[i].size = sz;
            }
            if (!r.u32(img_c)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (img_c > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkImageMemoryBarrier> imgs(img_c);
            for (uint32_t i = 0; i < img_c; ++i) {
                uint32_t sa, da, ol, nl, sqf, dqf, vi, asp, bm, lc, bl, lyc;
                if (!r.u32(sa) || !r.u32(da) || !r.u32(ol) || !r.u32(nl) || !r.u32(sqf) ||
                    !r.u32(dqf) || !r.u32(vi) || !r.u32(asp) || !r.u32(bm) || !r.u32(lc) ||
                    !r.u32(bl) || !r.u32(lyc)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                VkImage im = cmd_real_image(st, vi);
                if (im == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
                imgs[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                imgs[i].srcAccessMask = sa; imgs[i].dstAccessMask = da;
                imgs[i].oldLayout = static_cast<VkImageLayout>(ol);
                imgs[i].newLayout = static_cast<VkImageLayout>(nl);
                imgs[i].srcQueueFamilyIndex = sqf; imgs[i].dstQueueFamilyIndex = dqf;
                imgs[i].image = im;
                imgs[i].subresourceRange.aspectMask = asp;
                imgs[i].subresourceRange.baseMipLevel = bm;
                imgs[i].subresourceRange.levelCount = lc;
                imgs[i].subresourceRange.baseArrayLayer = bl;
                imgs[i].subresourceRange.layerCount = lyc;
            }
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, depFlags,
                                 mem_c, mems.empty() ? nullptr : mems.data(),
                                 buf_c, bufs.empty() ? nullptr : bufs.data(),
                                 img_c, imgs.empty() ? nullptr : imgs.data());
            return true;
        }
        case ALR_VK_CMD_COPY_BUFFER: {
            uint32_t vsrc = 0, vdst = 0, rc = 0;
            if (!r.u32(vsrc) || !r.u32(vdst) || !r.u32(rc)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (rc > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkBufferCopy> regions(rc);
            for (uint32_t i = 0; i < rc; ++i) {
                uint64_t so, dofs, sz;
                if (!r.u64(so) || !r.u64(dofs) || !r.u64(sz)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                regions[i] = {so, dofs, sz};
            }
            VkBuffer s = cmd_real_buffer(st, vsrc), d = cmd_real_buffer(st, vdst);
            if (s == VK_NULL_HANDLE || d == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdCopyBuffer(cmd, s, d, rc, regions.empty() ? nullptr : regions.data());
            return true;
        }
        case ALR_VK_CMD_COPY_IMAGE: {
            uint32_t vsrc = 0, sl = 0, vdst = 0, dl = 0, rc = 0;
            if (!r.u32(vsrc) || !r.u32(sl) || !r.u32(vdst) || !r.u32(dl) || !r.u32(rc)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (rc > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkImageCopy> regions(rc);
            for (uint32_t i = 0; i < rc; ++i) {
                uint32_t sAspect, sMip, sBL, sLC; int32_t sx, sy, sz;
                uint32_t dAspect, dMip, dBL, dLC; int32_t dx, dy, dz; uint32_t w, h, dd;
                if (!r.u32(sAspect) || !r.u32(sMip) || !r.u32(sBL) || !r.u32(sLC) ||
                    !r.i32(sx) || !r.i32(sy) || !r.i32(sz) ||
                    !r.u32(dAspect) || !r.u32(dMip) || !r.u32(dBL) || !r.u32(dLC) ||
                    !r.i32(dx) || !r.i32(dy) || !r.i32(dz) ||
                    !r.u32(w) || !r.u32(h) || !r.u32(dd)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                regions[i].srcSubresource = {sAspect, sMip, sBL, sLC};
                regions[i].srcOffset = {sx, sy, sz};
                regions[i].dstSubresource = {dAspect, dMip, dBL, dLC};
                regions[i].dstOffset = {dx, dy, dz};
                regions[i].extent = {w, h, dd};
            }
            VkImage s = cmd_real_image(st, vsrc), d = cmd_real_image(st, vdst);
            if (s == VK_NULL_HANDLE || d == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdCopyImage(cmd, s, static_cast<VkImageLayout>(sl), d,
                           static_cast<VkImageLayout>(dl), rc,
                           regions.empty() ? nullptr : regions.data());
            return true;
        }
        case ALR_VK_CMD_COPY_BUFFER_TO_IMAGE: {
            uint32_t vbuf = 0, vimg = 0, dl = 0, rc = 0;
            if (!r.u32(vbuf) || !r.u32(vimg) || !r.u32(dl) || !r.u32(rc)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (rc > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkBufferImageCopy> regions(rc);
            for (uint32_t i = 0; i < rc; ++i) {
                uint64_t bo; uint32_t brl, bih, asp, mip, bl, lc; int32_t x, y, z; uint32_t w, h, d;
                if (!r.u64(bo) || !r.u32(brl) || !r.u32(bih) || !r.u32(asp) || !r.u32(mip) ||
                    !r.u32(bl) || !r.u32(lc) || !r.i32(x) || !r.i32(y) || !r.i32(z) ||
                    !r.u32(w) || !r.u32(h) || !r.u32(d)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                regions[i].bufferOffset = bo;
                regions[i].bufferRowLength = brl;
                regions[i].bufferImageHeight = bih;
                regions[i].imageSubresource = {asp, mip, bl, lc};
                regions[i].imageOffset = {x, y, z};
                regions[i].imageExtent = {w, h, d};
            }
            VkBuffer b = cmd_real_buffer(st, vbuf);
            VkImage im = cmd_real_image(st, vimg);
            if (b == VK_NULL_HANDLE || im == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdCopyBufferToImage(cmd, b, im, static_cast<VkImageLayout>(dl), rc,
                                   regions.empty() ? nullptr : regions.data());
            return true;
        }
        case ALR_VK_CMD_PUSH_CONSTANTS: {
            uint32_t vlayout = 0, stageFlags = 0, offset = 0, size = 0;
            if (!r.u32(vlayout) || !r.u32(stageFlags) || !r.u32(offset) || !r.u32(size)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (size > 256) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            const uint8_t* d = nullptr;
            if (!r.take_raw(d, size)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            VkPipelineLayout lay = cmd_real_layout(st, vlayout);
            if (lay == VK_NULL_HANDLE) { *bad = ALR_VK_CMD_REPLAY_HANDLE; return false; }
            vkCmdPushConstants(cmd, lay, stageFlags, offset, size, d);
            return true;
        }
        case ALR_VK_CMD_CLEAR_ATTACHMENTS: {
            uint32_t att_c = 0, rect_c = 0;
            if (!r.u32(att_c)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (att_c > 16) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkClearAttachment> atts(att_c);
            for (uint32_t i = 0; i < att_c; ++i) {
                uint32_t am = 0, ca = 0; const uint8_t* cv = nullptr;
                if (!r.u32(am) || !r.u32(ca) || !r.take_raw(cv, 16)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                atts[i].aspectMask = am;
                atts[i].colorAttachment = ca;
                std::memcpy(&atts[i].clearValue, cv, 16);
            }
            if (!r.u32(rect_c)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            if (rect_c > 16) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
            std::vector<VkClearRect> rects(rect_c);
            for (uint32_t i = 0; i < rect_c; ++i) {
                int32_t x, y; uint32_t w, h, bal, lc;
                if (!r.i32(x) || !r.i32(y) || !r.u32(w) || !r.u32(h) || !r.u32(bal) || !r.u32(lc)) { *bad = ALR_VK_CMD_REPLAY_DECODE; return false; }
                rects[i].rect.offset = {x, y};
                rects[i].rect.extent = {w, h};
                rects[i].baseArrayLayer = bal;
                rects[i].layerCount = lc;
            }
            vkCmdClearAttachments(cmd, att_c, atts.empty() ? nullptr : atts.data(),
                                  rect_c, rects.empty() ? nullptr : rects.data());
            return true;
        }
        default:
            *bad = ALR_VK_CMD_REPLAY_DECODE;
            return false;  // unknown cmd-opcode
    }
}

// Replay an ENTIRE cmd-buffer log (located via its arena offset) into the real `cmd`. The
// caller has already vkBeginCommandBuffer'd `cmd`. `dev` is the command buffer's device (so
// the *2 render-pass ops can resolve their Vulkan-1.2 entrypoints via vkGetDeviceProcAddr —
// they are not link-time symbols in the NDK stub). Returns ALR_VK_CMD_REPLAY_OK or the first
// failing stage. (vkEndCommandBuffer is the caller's responsibility — it groups multiple
// logs into one buffer if a submit lists several.)
inline int cmd_replay_log_real(const uint8_t* log, uint32_t len, VkDecodeState& st,
                               VkDevice dev, VkCommandBuffer cmd) {
    VkReader r(log, len);
    for (;;) {
        uint16_t op = 0;
        if (!r.u16(op)) return ALR_VK_CMD_REPLAY_OK;            // ran off end (tolerate)
        if (op == ALR_VK_CMD_END) return ALR_VK_CMD_REPLAY_OK;  // clean terminator
        int bad = ALR_VK_CMD_REPLAY_OK;
        if (!cmd_replay_one_real(op, r, st, dev, cmd, &bad)) return bad;
    }
}

}  // namespace alr::gpu

#endif  // ALR_VK_DECODE_REAL
#endif  // ALR_GPU_ALR_GPU_VK_CMDLOG_REAL_HPP
