// alr_gpu_vk_cmd_dispatch_real.hpp — HAND-WRITTEN real-Mali ring dispatch for the cmd-log
// band's RING ops (vkQueueSubmit + fence/semaphore/wait sync). Compiled ONLY under
// ALR_VK_DECODE_REAL.
//
// vkQueueSubmit is THE replay point. For each cmd-buffer vid the guest listed, the host:
//   1. looks up its ALREADY-ALLOCATED real Mali VkCommandBuffer (st.real_cmd[vcmd] — the
//      one vk_real_alloc_cmd made when the guest did vkAllocateCommandBuffers);
//   2. vkBeginCommandBuffer's it (ONE_TIME_SUBMIT), replays the guest's record log into it
//      (cmd_replay_log_real — translate guest vids -> real handles, call the real vkCmd*),
//      then vkEndCommandBuffer's it;
//   3. accumulates the real VkCommandBuffer.
// Then it translates the wait/signal semaphores + the fence and issues ONE real
// vkQueueSubmit on the queue's owner thread (the servicer thread, which owns the Mali
// driver). The recorded BYTES came from the shared arena — they never crossed the ring.
//
// Reusing the guest-allocated real command buffer (rather than a host-private transient
// pool) keeps the device/pool ownership where vk_real_alloc_cmd already put it, and means a
// re-submit of the same vcmd re-records into the same buffer (the guest reset it first, the
// pool was created RESET_COMMAND_BUFFER_BIT in vk_real_create_pool).

#ifndef ALR_GPU_ALR_GPU_VK_CMD_DISPATCH_REAL_HPP
#define ALR_GPU_ALR_GPU_VK_CMD_DISPATCH_REAL_HPP

#ifdef ALR_VK_DECODE_REAL

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "alr_gpu/alr_gpu_vk_cmdlog.hpp"        // ALR_VK_CMD_REPLAY_* + cmd_replay_log_real
#include "alr_gpu/alr_gpu_vk_cmdlog_real.hpp"   // cmd_tables(st) (fences/semaphores) + replay
#include "alr_gpu/alr_gpu_vk_decode.hpp"        // VkDecodeState (real_queue / real_cmd / real_dev)

namespace alr::gpu {

// The VkCmdSubmitInfo the wire dispatcher built (vqueue/vfence/waits/signals + each cmd's
// log pointer + len). Forward-declared in alr_gpu_vk_cmd_dispatch.hpp; we use it after.
struct VkCmdSubmitInfo;

// ---- fence/semaphore resolvers (0 == VK_NULL_HANDLE). ----
inline VkFence cmd_real_fence(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).fences.find(vid);
    return it == cmd_tables(st).fences.end() ? VK_NULL_HANDLE : it->second;
}
inline VkSemaphore cmd_real_semaphore(VkDecodeState& st, uint32_t vid) {
    if (!vid) return VK_NULL_HANDLE;
    auto it = cmd_tables(st).semaphores.find(vid);
    return it == cmd_tables(st).semaphores.end() ? VK_NULL_HANDLE : it->second;
}

// Resolve the real VkDevice behind a vcmd (the device its command buffer belongs to). All
// cmd buffers in one submit share a device; we take it from the first resolvable cmd.
inline VkDevice cmd_real_device_of_cmd(VkDecodeState& st, uint32_t vcmd) {
    auto it = st.real_cmd.find(vcmd);
    if (it == st.real_cmd.end()) return VK_NULL_HANDLE;
    auto dit = st.real_dev.find(it->second.first);
    return dit == st.real_dev.end() ? VK_NULL_HANDLE : dit->second.dev;
}

// ===========================================================================
// cmd_real_queue_submit — replay every listed cmd-buffer log into its real Mali command
// buffer, then real vkQueueSubmit. Returns the vkQueueSubmit VkResult; *replay_out gets the
// cmd-log replay outcome (ALR_VK_CMD_REPLAY_*). Must run on the queue's owner thread.
// ===========================================================================
inline int32_t cmd_real_queue_submit(VkDecodeState& st, const VkCmdSubmitInfo& info,
                                     int32_t* replay_out) {
    *replay_out = ALR_VK_CMD_REPLAY_OK;
    auto qit = st.real_queue.find(info.vqueue);
    if (qit == st.real_queue.end()) { *replay_out = ALR_VK_CMD_REPLAY_NO_QUEUE; return VK_ERROR_INITIALIZATION_FAILED; }
    VkQueue queue = qit->second;

    // Build + record each real command buffer from its arena log.
    std::vector<VkCommandBuffer> real_cmds;
    real_cmds.reserve(info.cmds.size());
    for (const auto& c : info.cmds) {
        auto cit = st.real_cmd.find(c.vcmd);
        if (cit == st.real_cmd.end()) { *replay_out = ALR_VK_CMD_REPLAY_NO_CMD; return VK_ERROR_INITIALIZATION_FAILED; }
        VkCommandBuffer cmd = cit->second.second;
        VkDevice cdev = cmd_real_device_of_cmd(st, c.vcmd);  // for the *2 op proc-addr lookup
        if (c.log == nullptr && c.log_len != 0) { *replay_out = ALR_VK_CMD_REPLAY_LOG_UNREADABLE; return VK_ERROR_INITIALIZATION_FAILED; }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { *replay_out = ALR_VK_CMD_REPLAY_BEGIN; return VK_ERROR_INITIALIZATION_FAILED; }
        const int rr = (c.log && c.log_len)
                           ? cmd_replay_log_real(c.log, c.log_len, st, cdev, cmd)
                           : ALR_VK_CMD_REPLAY_OK;  // empty log = a no-op command buffer
        if (rr != ALR_VK_CMD_REPLAY_OK) {
            vkEndCommandBuffer(cmd);  // best-effort close before bailing
            *replay_out = rr;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { *replay_out = ALR_VK_CMD_REPLAY_END; return VK_ERROR_INITIALIZATION_FAILED; }
        real_cmds.push_back(cmd);
    }

    // Translate the wait/signal semaphores + fence.
    std::vector<VkSemaphore> wait_sems;
    std::vector<VkPipelineStageFlags> wait_stages;
    for (const auto& w : info.waits) {
        VkSemaphore s = cmd_real_semaphore(st, w.first);
        if (s == VK_NULL_HANDLE) { *replay_out = ALR_VK_CMD_REPLAY_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
        wait_sems.push_back(s);
        wait_stages.push_back(w.second ? w.second
                                       : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    std::vector<VkSemaphore> signal_sems;
    for (uint32_t vs : info.signals) {
        VkSemaphore s = cmd_real_semaphore(st, vs);
        if (s == VK_NULL_HANDLE) { *replay_out = ALR_VK_CMD_REPLAY_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
        signal_sems.push_back(s);
    }
    VkFence fence = cmd_real_fence(st, info.vfence);  // 0 -> VK_NULL_HANDLE is legal

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = static_cast<uint32_t>(wait_sems.size());
    si.pWaitSemaphores = wait_sems.empty() ? nullptr : wait_sems.data();
    si.pWaitDstStageMask = wait_stages.empty() ? nullptr : wait_stages.data();
    si.commandBufferCount = static_cast<uint32_t>(real_cmds.size());
    si.pCommandBuffers = real_cmds.empty() ? nullptr : real_cmds.data();
    si.signalSemaphoreCount = static_cast<uint32_t>(signal_sems.size());
    si.pSignalSemaphores = signal_sems.empty() ? nullptr : signal_sems.data();

    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) *replay_out = ALR_VK_CMD_REPLAY_SUBMIT;
    return static_cast<int32_t>(r);
}

// ---- queue / device wait-idle ----
inline int32_t cmd_real_queue_wait_idle(VkDecodeState& st, uint32_t vqueue) {
    auto it = st.real_queue.find(vqueue);
    if (it == st.real_queue.end()) return VK_ERROR_INITIALIZATION_FAILED;
    return static_cast<int32_t>(vkQueueWaitIdle(it->second));
}
inline int32_t cmd_real_device_wait_idle(VkDecodeState& st, uint32_t vdev) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    return static_cast<int32_t>(vkDeviceWaitIdle(it->second.dev));
}

// ---- fences ----
inline int32_t cmd_real_create_fence(VkDecodeState& st, uint32_t vdev, uint32_t vfence,
                                     uint32_t flags) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = flags;  // e.g. VK_FENCE_CREATE_SIGNALED_BIT
    VkFence f = VK_NULL_HANDLE;
    VkResult r = vkCreateFence(it->second.dev, &fci, nullptr, &f);
    if (r == VK_SUCCESS) cmd_tables(st).fences[vfence] = f;
    return static_cast<int32_t>(r);
}
inline void cmd_real_destroy_fence(VkDecodeState& st, uint32_t vdev, uint32_t vfence) {
    auto dit = st.real_dev.find(vdev);
    auto fit = cmd_tables(st).fences.find(vfence);
    if (dit != st.real_dev.end() && fit != cmd_tables(st).fences.end()) {
        vkDestroyFence(dit->second.dev, fit->second, nullptr);
        cmd_tables(st).fences.erase(fit);
    }
}
inline int32_t cmd_real_wait_fences(VkDecodeState& st, uint32_t vdev, uint32_t wait_all,
                                    uint64_t timeout, const std::vector<uint32_t>& vfences) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkFence> fences;
    for (uint32_t v : vfences) {
        VkFence f = cmd_real_fence(st, v);
        if (f == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
        fences.push_back(f);
    }
    if (fences.empty()) return VK_SUCCESS;
    return static_cast<int32_t>(vkWaitForFences(it->second.dev,
                                                static_cast<uint32_t>(fences.size()),
                                                fences.data(), wait_all ? VK_TRUE : VK_FALSE,
                                                timeout));
}
inline int32_t cmd_real_reset_fences(VkDecodeState& st, uint32_t vdev,
                                     const std::vector<uint32_t>& vfences) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkFence> fences;
    for (uint32_t v : vfences) {
        VkFence f = cmd_real_fence(st, v);
        if (f == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
        fences.push_back(f);
    }
    if (fences.empty()) return VK_SUCCESS;
    return static_cast<int32_t>(vkResetFences(it->second.dev,
                                              static_cast<uint32_t>(fences.size()),
                                              fences.data()));
}
inline int32_t cmd_real_get_fence_status(VkDecodeState& st, uint32_t vdev, uint32_t vfence) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkFence f = cmd_real_fence(st, vfence);
    if (f == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    return static_cast<int32_t>(vkGetFenceStatus(it->second.dev, f));
}

// ---- semaphores ----
inline int32_t cmd_real_create_semaphore(VkDecodeState& st, uint32_t vdev, uint32_t vsem,
                                         uint32_t flags) {
    auto it = st.real_dev.find(vdev);
    if (it == st.real_dev.end()) return VK_ERROR_INITIALIZATION_FAILED;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.flags = flags;
    VkSemaphore s = VK_NULL_HANDLE;
    VkResult r = vkCreateSemaphore(it->second.dev, &sci, nullptr, &s);
    if (r == VK_SUCCESS) cmd_tables(st).semaphores[vsem] = s;
    return static_cast<int32_t>(r);
}
inline void cmd_real_destroy_semaphore(VkDecodeState& st, uint32_t vdev, uint32_t vsem) {
    auto dit = st.real_dev.find(vdev);
    auto sit = cmd_tables(st).semaphores.find(vsem);
    if (dit != st.real_dev.end() && sit != cmd_tables(st).semaphores.end()) {
        vkDestroySemaphore(dit->second.dev, sit->second, nullptr);
        cmd_tables(st).semaphores.erase(sit);
    }
}

}  // namespace alr::gpu

#endif  // ALR_VK_DECODE_REAL
#endif  // ALR_GPU_ALR_GPU_VK_CMD_DISPATCH_REAL_HPP
