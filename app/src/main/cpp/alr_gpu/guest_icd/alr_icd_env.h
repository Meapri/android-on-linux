/* alr_icd_env.h — the host<->guest env contract for the ALR Vulkan ICD's ring.
 *
 * This is the Vulkan twin of alr_gpu/guest_shim/alr_shim_env.h. The guest Vulkan
 * ICD (libvulkan.so.1, alr_icd_vulkan.c) marshals its calls over an SPSC command
 * ring to the host, which decodes them on the REAL vendor Mali libvulkan
 * (alr_gpu_vk_decode.hpp with ALR_VK_DECODE_REAL) and writes a REPLY stream back.
 *
 * WHY A SECOND (REPLY) RING — the difference from the GLES shim. The GLES path is
 * almost entirely fire-and-forget: the guest appends ops, the host replays them,
 * and the only thing that comes back is a per-frame sync seqno (glGetString
 * identity is read from the ring HEADER block, not a reply stream). The Vulkan
 * ENUM rung is NOT fire-and-forget: vkEnumeratePhysicalDevices /
 * vkGetPhysicalDeviceProperties MUST return DATA (device count, the
 * "Mali-G615 MC2" deviceName, apiVersion, queue families). So the ICD uses a
 * SECOND ring, in the reverse direction, on which the host writes the reply op
 * stream (AlrVkReply, alr_gpu_vk_proto.hpp) and the guest drains it — exactly the
 * two-ring shape the in-process host self-test (run_vk_marshal_mali_probe) already
 * proves, but now the two ends are the forked guest and the app-process host.
 *
 * MODEL: fd-inheritance (matches the fork-inherit design; no path negotiation).
 * The host (app process) creates BOTH rings before fork; the guest inherits both
 * fds and mmaps them MAP_SHARED.
 *
 *   ALR_VK_RING_FD        (required) decimal fd of the REQUEST ring (guest->host):
 *                         a shared region the host already ring_init()'d. The ICD
 *                         is the PRODUCER on it. Must NOT be FD_CLOEXEC.
 *   ALR_VK_RING_BYTES     (required) decimal data-region size (power of two) of the
 *                         request ring, i.e. the `ring_bytes` passed to ring_init().
 *   ALR_VK_REPLY_FD       (required) decimal fd of the REPLY ring (host->guest): a
 *                         second shared region the host ring_init()'d. The ICD is
 *                         the CONSUMER on it; the host is the producer. The host
 *                         appends the reply op stream here after draining a request
 *                         batch and bumps its reply_seq so the ICD's wait returns.
 *   ALR_VK_REPLY_BYTES    (required) decimal data-region size of the reply ring.
 *   ALR_VK_RING_DOORBELL_FD (optional) decimal eventfd the ICD writes on submit so
 *                         the host servicer wakes promptly instead of spin-polling.
 *
 * If ALR_VK_RING_FD / ALR_VK_REPLY_FD are absent (or the regions aren't valid ALRG
 * rings) the ICD reports ZERO physical devices (vkEnumeratePhysicalDevices returns
 * count 0, VK_SUCCESS) — the conformant "no GPU here" answer, so a smoke run of a
 * Vulkan client off-device degrades quietly rather than crashing. This mirrors the
 * GLES shim's ring-less quiet no-op.
 *
 * The HOST side (alr_gpu_vk_host_service.hpp) must, before fork:
 *   1. create TWO shared regions (request + reply) via ring_region_size(), mmap
 *      MAP_SHARED, ring_init() both;
 *   2. (optional) create a doorbell eventfd;
 *   3. clear FD_CLOEXEC on all of them;
 *   4. set ALR_VK_RING_FD / ALR_VK_RING_BYTES / ALR_VK_REPLY_FD / ALR_VK_REPLY_BYTES
 *      [/ ALR_VK_RING_DOORBELL_FD] in the child env;
 *   5. start a host servicer thread that owns NDK libvulkan: drain the request ring
 *      -> decode_vk_batch(..., ALR_VK_DECODE_REAL, nullptr) on real Mali -> append
 *      the reply op stream to the reply ring -> post_reply() (+ write the doorbell).
 */
#ifndef ALR_ICD_ENV_H
#define ALR_ICD_ENV_H

#define ALR_VK_ENV_RING_FD          "ALR_VK_RING_FD"
#define ALR_VK_ENV_RING_BYTES       "ALR_VK_RING_BYTES"
#define ALR_VK_ENV_REPLY_FD         "ALR_VK_REPLY_FD"
#define ALR_VK_ENV_REPLY_BYTES      "ALR_VK_REPLY_BYTES"
#define ALR_VK_ENV_RING_DOORBELL_FD "ALR_VK_RING_DOORBELL_FD"

/* The same-process MAP_SHARED ARENA for HOST_VISIBLE device memory (the zero-copy
 * vkMapMemory keystone — see alr_gpu/generated/alr_gpu_vk_arena.hpp). The host creates
 * the arena memfd before fork and advertises it here; the guest ICD maps the SAME memfd
 * MAP_SHARED so a guest write through a vkMapMemory pointer lands directly in the memory
 * the host's real VkDeviceMemory aliases (host-pointer import). If absent, the generated
 * memory entrypoints fall back to no-arena (vkMapMemory then fails for memory the host
 * could not arena-back — the conformant degradation, like the ring-less path). */
#define ALR_VK_ENV_ARENA_FD         "ALR_VK_ARENA_FD"
#define ALR_VK_ENV_ARENA_BYTES      "ALR_VK_ARENA_BYTES"

#endif /* ALR_ICD_ENV_H */
