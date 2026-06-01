// ALR GPU ring hook — the CP-0 interface contract between the CPU loader (WS-1),
// the GPU marshalling executor (WS-2, this file's owner), and the compositor
// (WS-3). See docs/research/orchestration-5session-plan.md §5-A / §5-B.
//
// PURPOSE
// -------
// The loader (`build_native_loader_probe`, owned by WS-1) forks a child that runs
// an unmodified glibc GLES2 guest in-process. Before that fork, WS-1 calls
// `alr_loader_attach_gpu_ring()` (defined here, owned by WS-2). That call:
//   1. creates a memfd-backed SPSC command ring (alr_gpu_ring.hpp) + an eventfd
//      doorbell, both INHERITABLE (no FD_CLOEXEC) so the forked guest keeps them;
//   2. ring_init()s the region and maps it in the PARENT (the app process);
//   3. starts a host `GpuExecutorService` (alr_gpu_host_service.hpp) on its own
//      Mali GLES2 thread, consuming that ring and presenting each decoded frame;
//   4. fills `GpuRing out` with {ring_fd, ring_bytes, doorbell_fd}.
// WS-1 then, per the contract below, pushes the three env strings from
// `gpu_ring_guest_env(out)` into the guest env and forks. The guest's GLES shim
// (alr_gpu/guest_shim) reads `ALR_GPU_RING_FD/BYTES/DOORBELL_FD`, mmaps the same
// memfd MAP_SHARED, and every GL call becomes ring bytes the host replays on Mali.
//
// OWNERSHIP / WHO TOUCHES WHAT (so the five sessions don't collide)
// -----------------------------------------------------------------
//   * WS-2 (this file): the ring/doorbell creation, the executor lifetime, the
//     present adapter, and the env-key names. ALL of it lives in alr_gpu/**.
//   * WS-1: calls attach() once before fork; inherits out.ring_fd / out.doorbell_fd
//     into the child (they are already non-CLOEXEC — WS-1 need not clear it, and a
//     belt-and-suspenders clear is harmless); appends gpu_ring_guest_env(out) to its
//     `guest_env` vector. WS-1 owns NONE of the GPU logic. The guest in this project
//     is launched by in-process jump (alr_enter_guest), not execve, so the fds are
//     simply inherited at fork; the non-CLOEXEC create also keeps the execve path
//     (memfd/static) correct.
//   * WS-3: supplies an `AhbFrameSource` sink (§5-B) via GpuRingAttachConfig to
//     receive each finished AHardwareBuffer for zero-copy Wayland present. Until
//     WS-3 publishes its `PresentSource` (§5-C), leave `frame_sink` null and pass a
//     `present_window` so the executor presents straight to that ANativeWindow
//     (the "executor->window direct" fallback the WS-2 kickoff calls for).
//
// CONTRACT STABILITY: the signatures of GpuRing / AhbFrameSource /
// GpuRingAttachConfig / alr_loader_attach_gpu_ring / alr_loader_detach_gpu_ring /
// gpu_ring_guest_env are the CP-0 contract. Changing any of them requires the
// integration session's sign-off (plan §5). The env-key STRINGS here must stay
// byte-identical to alr_gpu/guest_shim/alr_shim_env.h (the guest reads them).
//
// Header-only and self-contained (public Android/EGL/GLES2 APIs only), matching the
// rest of alr_gpu/**, so WS-1 wires it with a single `#include` + one attach() call
// and no CMake/build change. Compile target: aarch64 NDK, C++20.

#ifndef ALR_GPU_ALR_GPU_RING_HOOK_HPP
#define ALR_GPU_ALR_GPU_RING_HOOK_HPP

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include "alr_gpu/alr_gpu_host_service.hpp"  // GpuExecutorService, AhbRenderTarget
#include "alr_gpu/alr_gpu_ring.hpp"          // ring_init, ring_region_size

namespace alr::gpu {

// ===========================================================================
// §5-B  AhbFrameSource (WS-2 provides -> WS-3 consumes)
// ---------------------------------------------------------------------------
// Invoked on the executor's GL thread at each frame boundary (after glFinish), with
// the just-rendered AHardwareBuffer. WS-3's compositor imports it as an EGLImage and
// presents zero-copy. `serial` increments per presented frame (frame-pacing / drop
// detection). The AHB is owned by the executor and valid only for the duration of
// the call (it is re-rendered next frame); WS-3 must finish its GPU import before
// returning (the executor glFinish()ed, so the pixels are ready).
// ===========================================================================
using AhbFrameSource = std::function<void(AHardwareBuffer*, int w, int h, uint64_t serial)>;

// ===========================================================================
// §5-A  GpuRing (WS-1 receives, then inherits + advertises)
// ---------------------------------------------------------------------------
// Filled by alr_loader_attach_gpu_ring(). All three fields are what the guest needs;
// WS-1 inherits the fds across fork and advertises them via gpu_ring_guest_env().
// ===========================================================================
struct GpuRing {
    int ring_fd = -1;          // inheritable memfd of the ring_init'd shared region
    uint32_t ring_bytes = 0;   // data-region size (power of two); the guest maps 48 + ring_bytes
    int doorbell_fd = -1;      // inheritable eventfd the guest signals on flush (-1 = none)
    int fb_w = 0;              // host AHB render-target size; advertised so the guest shim's
    int fb_h = 0;              // eglQuerySurface reports a drawable size matching where the host draws
};

// Tuning + present wiring for attach(). Defaults give the executor's own
// on-screen present (when present_window is set) and a 1 MiB ring.
struct GpuRingAttachConfig {
    int fb_w = 0;                       // render-target (AHB-FBO) width  in pixels (required, > 0)
    int fb_h = 0;                       // render-target (AHB-FBO) height in pixels (required, > 0)
    uint32_t ring_bytes = 1u << 20;     // SPSC data ring size; MUST be a power of two
    ANativeWindow* present_window = nullptr;  // WS-3 fallback: executor presents straight here
    AhbFrameSource frame_sink = nullptr;      // §5-B sink (WS-3); null => no zero-copy handoff
};

namespace detail {

// Process-global holder for the attached ring + executor. The app (host) process
// persists across the loader fork, so the executor — created in the parent before
// fork — keeps consuming while the forked guest produces. Function-local static
// avoids any cross-TU ODR concern in this header-only design.
struct GpuRingHolder {
    std::mutex mu;
    std::unique_ptr<GpuExecutorService> svc;
    void* region = nullptr;       // parent's mapping of the memfd (executor reads it)
    size_t region_sz = 0;
    int ring_fd = -1;             // parent's copy of the inheritable fds
    int doorbell_fd = -1;
    std::atomic<uint64_t> present_serial{0};
    bool attached = false;
};

inline GpuRingHolder& holder() {
    static GpuRingHolder h;
    return h;
}

// memfd_create via raw syscall (matches runtime_report.cpp's usage; NDK headers may
// not declare memfd_create on every target). Flags 0 => the fd is NOT close-on-exec,
// so it survives both the in-process-jump and the execve guest-launch paths.
inline int make_memfd(const char* name) {
    long fd = ::syscall(__NR_memfd_create, name, 0u);
    return fd < 0 ? -1 : static_cast<int>(fd);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// create_shared_gpu_ring — POSIX-only (NO GL): allocate a memfd, size it, map it in
// the parent, ring_init() it, and create an inheritable eventfd doorbell. This is
// the genuinely new transport primitive for M2 (the executor + decoder are already
// device-proven). Factored out with no GL/EGL dependency so it is unit-testable in
// isolation (a forked producer + a plain consumer, no Mali context) — see the
// alr_gpu fork-transport harness.
//
// On success: out = {ring_fd, ring_bytes, doorbell_fd}, *out_region / *out_region_sz
// give the PARENT's mapping (the executor's consumer view). On failure: returns
// false, closes/unmaps anything partial, out untouched.
// ---------------------------------------------------------------------------
inline bool create_shared_gpu_ring(uint32_t ring_bytes, GpuRing& out,
                                   void** out_region, size_t* out_region_sz) {
    if (ring_bytes == 0 || (ring_bytes & (ring_bytes - 1)) != 0) return false;  // pow2
    const size_t region_sz = ring_region_size(ring_bytes);

    const int ring_fd = detail::make_memfd("alr_gpu_ring");
    if (ring_fd < 0) return false;
    if (::ftruncate(ring_fd, static_cast<off_t>(region_sz)) != 0) {
        ::close(ring_fd);
        return false;
    }
    void* region = ::mmap(nullptr, region_sz, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0);
    if (region == MAP_FAILED) {
        ::close(ring_fd);
        return false;
    }
    if (!ring_init(region, ring_bytes)) {
        ::munmap(region, region_sz);
        ::close(ring_fd);
        return false;
    }
    // Inheritable doorbell eventfd (flags 0 => not close-on-exec). Optional: if it
    // fails the ring still works (guest falls back to spin/poll), so do not abort.
    const int bell_fd = ::eventfd(0, 0);

    out.ring_fd = ring_fd;
    out.ring_bytes = ring_bytes;
    out.doorbell_fd = bell_fd;  // -1 acceptable
    if (out_region) *out_region = region;
    if (out_region_sz) *out_region_sz = region_sz;
    return true;
}

// ---------------------------------------------------------------------------
// gpu_ring_guest_env — the three env strings WS-1 appends to its guest_env vector.
// SINGLE SOURCE OF TRUTH for the key names (they must equal the guest shim's
// alr_gpu/guest_shim/alr_shim_env.h). WS-1 must not hand-write these keys.
// ---------------------------------------------------------------------------
inline std::vector<std::string> gpu_ring_guest_env(const GpuRing& r) {
    std::vector<std::string> env;
    if (r.ring_fd < 0 || r.ring_bytes == 0) return env;  // not attached => no GPU env
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ALR_GPU_RING_FD=%d", r.ring_fd);
    env.emplace_back(buf);
    std::snprintf(buf, sizeof(buf), "ALR_GPU_RING_BYTES=%u", r.ring_bytes);
    env.emplace_back(buf);
    if (r.doorbell_fd >= 0) {
        std::snprintf(buf, sizeof(buf), "ALR_GPU_RING_DOORBELL_FD=%d", r.doorbell_fd);
        env.emplace_back(buf);
    }
    // AHB render-target size (= guest_shim/alr_shim_env.h ALR_ENV_FB_W/H): lets the guest
    // shim's eglQuerySurface report a drawable size matching where the host draws.
    if (r.fb_w > 0 && r.fb_h > 0) {
        std::snprintf(buf, sizeof(buf), "ALR_GPU_FB_W=%d", r.fb_w);
        env.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "ALR_GPU_FB_H=%d", r.fb_h);
        env.emplace_back(buf);
    }
    return env;
}

// ---------------------------------------------------------------------------
// §5-A  alr_loader_attach_gpu_ring — WS-1 calls this ONCE, before fork.
//
// Creates the shared ring + doorbell, starts the host executor consuming it, and
// fills `out`. Returns true if the GPU pipeline is live (WS-1 should then advertise
// gpu_ring_guest_env(out) and inherit the fds). Returns false if a ring/executor
// could not be brought up (e.g. no Mali EGL) — WS-1 then proceeds CPU/SW-only and
// must NOT advertise the env (the guest shim runs ring-less = quiet no-op).
//
// Idempotent: a second call without detach returns the existing ring.
// ---------------------------------------------------------------------------
inline bool alr_loader_attach_gpu_ring(GpuRing& out, const GpuRingAttachConfig& cfg) {
    auto& h = detail::holder();
    std::lock_guard<std::mutex> lk(h.mu);
    if (h.attached) {
        out.ring_fd = h.ring_fd;
        out.ring_bytes = static_cast<uint32_t>(
            ring_valid(h.region) ? static_cast<RingHeader*>(h.region)->ring_bytes : 0u);
        out.doorbell_fd = h.doorbell_fd;
        return h.svc && h.svc->error().empty();
    }
    if (cfg.fb_w <= 0 || cfg.fb_h <= 0) return false;

    void* region = nullptr;
    size_t region_sz = 0;
    GpuRing ring{};
    if (!create_shared_gpu_ring(cfg.ring_bytes, ring, &region, &region_sz)) return false;
    ring.fb_w = cfg.fb_w;  // advertised to the guest shim (eglQuerySurface) so its viewport
    ring.fb_h = cfg.fb_h;  // matches the AHB render target the host draws into

    // Present adapter: hand each finished AHB to WS-3's sink (§5-B) with a running
    // serial. If frame_sink is null the executor's optional window-present path (when
    // present_window != null) is the on-screen output; if both are null it renders
    // headless into the AHB (valid, just not displayed).
    GpuExecutorService::PresentFn present =
        [serial = &h.present_serial, sink = cfg.frame_sink](const AhbRenderTarget& rt) {
            if (sink) {
                const uint64_t s = serial->fetch_add(1, std::memory_order_relaxed);
                sink(rt.ahb, rt.w, rt.h, s);
            }
        };

    // Pass the doorbell so the executor's idle wait blocks on the guest's flush
    // signal instead of spin-yielding (it reads/drains the fd but does not own it —
    // the holder closes it on detach).
    auto svc = std::make_unique<GpuExecutorService>(region, region_sz, cfg.fb_w, cfg.fb_h,
                                                    std::move(present), cfg.present_window,
                                                    ring.doorbell_fd);
    if (!svc->start()) {
        // Executor failed (no Mali ctx / FBO incomplete). Tear the ring down so WS-1
        // sees false and runs SW-only; do not leak the fds/mapping.
        if (region) ::munmap(region, region_sz);
        if (ring.ring_fd >= 0) ::close(ring.ring_fd);
        if (ring.doorbell_fd >= 0) ::close(ring.doorbell_fd);
        return false;
    }

    h.svc = std::move(svc);
    h.region = region;
    h.region_sz = region_sz;
    h.ring_fd = ring.ring_fd;
    h.doorbell_fd = ring.doorbell_fd;
    h.attached = true;

    out = ring;
    return true;
}

// ---------------------------------------------------------------------------
// alr_loader_detach_gpu_ring — stop the executor + release the parent's ring
// resources. Call when the guest has exited / the app tears the loader down. Safe to
// call when not attached. The forked child's inherited fds are independent and
// closed with the child; this only releases the parent's copies. Idempotent.
// ---------------------------------------------------------------------------
inline void alr_loader_detach_gpu_ring() {
    auto& h = detail::holder();
    std::lock_guard<std::mutex> lk(h.mu);
    if (!h.attached) return;
    if (h.svc) {
        h.svc->stop();   // joins the GL thread (GL teardown happens inside it)
        h.svc.reset();
    }
    if (h.region) { ::munmap(h.region, h.region_sz); h.region = nullptr; h.region_sz = 0; }
    if (h.ring_fd >= 0) { ::close(h.ring_fd); h.ring_fd = -1; }
    if (h.doorbell_fd >= 0) { ::close(h.doorbell_fd); h.doorbell_fd = -1; }
    h.attached = false;
}

// Observability for WS-5 evidence: frames the executor has presented since attach.
inline uint32_t gpu_ring_frames_presented() {
    auto& h = detail::holder();
    std::lock_guard<std::mutex> lk(h.mu);
    return h.svc ? h.svc->frames_presented() : 0u;
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_RING_HOOK_HPP
