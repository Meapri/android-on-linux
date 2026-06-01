/* alr_shim_env.h — the EXACT host<->guest env contract for ring discovery.
 *
 * The shim attaches to the GPU command ring the HOST (parent app process)
 * created BEFORE fork(). Per DESIGN §3.1 the guest is the fork()ed child of the
 * app process, so it INHERITS the ring's fd. The loader must set these env vars
 * in the guest's environment (alongside ALR_ROOTFS / LD_LIBRARY_PATH / LD_PRELOAD
 * in build_native_loader_probe's guest_env), so the shim's lazy init can find it.
 *
 * MODEL: fd-inheritance (matches the fork-inherit design; no path negotiation).
 *
 *   ALR_GPU_RING_FD       (required) decimal integer: an inherited fd to a shared
 *                         region (memfd / ashmem / shm) the host already
 *                         ring_init()'d. The shim mmap()s it MAP_SHARED. The fd
 *                         must survive exec (the loader must NOT set FD_CLOEXEC on
 *                         it, or must clear it before handing control to the guest).
 *   ALR_GPU_RING_BYTES    (required) decimal integer: the DATA-region size in
 *                         bytes (power of two), i.e. the `ring_bytes` passed to
 *                         ring_init(). The shim maps sizeof(RingHeader)+ring_bytes
 *                         = 48 + ALR_GPU_RING_BYTES. (The shim also reads
 *                         ring_bytes back out of the mapped header and prefers the
 *                         header value; this env var lets it size the mmap before
 *                         reading the header.)
 *   ALR_GPU_RING_DOORBELL_FD (optional) decimal integer: an inherited eventfd the
 *                         shim writes (8-byte increment) on flush/flush_and_wait so
 *                         the host GPU thread wakes immediately instead of the host
 *                         spin-polling `head`. If unset/-1, the host falls back to
 *                         polling and the shim falls back to spin-waiting on
 *                         reply_seq (correct, just busier). Recommended to set it.
 *
 * The host side (the main session wires this) must, before fork():
 *   1. create the shared region (memfd_create / ashmem) of
 *      alr::gpu::ring_region_size(ring_bytes) bytes, ftruncate, mmap MAP_SHARED;
 *   2. alr::gpu::ring_init(region, ring_bytes);
 *   3. create an eventfd (optional doorbell);
 *   4. clear FD_CLOEXEC on both fds;
 *   5. set ALR_GPU_RING_FD / ALR_GPU_RING_BYTES / ALR_GPU_RING_DOORBELL_FD in the
 *      child env;
 *   6. fork()+exec the guest (alr-gles-cube) under the loader;
 *   7. in a host GPU thread that owns the real Mali EGL/GLES2 context, run a
 *      drain loop: (block on doorbell or poll RingConsumer::available()) ->
 *      snapshot -> alr::gpu::decode_batch(snapshot, n, hostState) -> advance(n);
 *      on the per-frame sync (req_seq advanced) glFinish + present the AHB +
 *      RingConsumer::post_reply() (+ write the doorbell back if used).
 */
#ifndef ALR_SHIM_ENV_H
#define ALR_SHIM_ENV_H

#define ALR_ENV_RING_FD          "ALR_GPU_RING_FD"
#define ALR_ENV_RING_BYTES       "ALR_GPU_RING_BYTES"
#define ALR_ENV_RING_DOORBELL_FD "ALR_GPU_RING_DOORBELL_FD"

/* Optional: the host render-target (AHB-FBO) size the executor renders into. The
 * loader sets these (= GpuRingAttachConfig.fb_w/fb_h, via gpu_ring_guest_env) so the
 * shim's eglQuerySurface reports a drawable size that MATCHES where the host draws,
 * letting the guest set a correct glViewport. If unset, the shim uses a default. */
#define ALR_ENV_FB_W             "ALR_GPU_FB_W"
#define ALR_ENV_FB_H             "ALR_GPU_FB_H"

#endif /* ALR_SHIM_ENV_H */
