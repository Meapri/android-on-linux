# WS-2 ↔ WS-1 / WS-3 integration recipe (CP-0)

The CP-0 contract lives in [`alr_gpu_ring_hook.hpp`](alr_gpu_ring_hook.hpp). This file
is the **copy-pasteable wiring** the other sessions need. Nothing here changes the
build (`alr_gpu/**` is header-only and already on `alr_loader`'s include path).

## WS-1 (loader) — call site in `build_native_loader_probe`

The guest is launched by in-process jump (`alr_enter_guest`) in the forked child, so
the ring/doorbell fds are simply **inherited at fork** — they are created non-CLOEXEC,
no clearing needed (a clear is a harmless no-op).

```cpp
#include "alr_gpu/alr_gpu_ring_hook.hpp"   // one include, no CMake change

// --- in the PARENT, BEFORE fork(), after guest_env is built ---
alr::gpu::GpuRing gpu_ring;
alr::gpu::GpuRingAttachConfig gcfg;
gcfg.fb_w = /* present surface width  */;   // e.g. wl_output width from WS-3
gcfg.fb_h = /* present surface height */;
gcfg.present_window = /* ANativeWindow* or nullptr */;  // WS-3 fallback present
// gcfg.frame_sink = <WS-3 AhbFrameSource>;             // set once WS-3 PresentSource lands
if (alr::gpu::alr_loader_attach_gpu_ring(gpu_ring, gcfg)) {
    for (auto& kv : alr::gpu::gpu_ring_guest_env(gpu_ring))
        guest_env.push_back(kv);            // ALR_GPU_RING_FD/BYTES[/DOORBELL_FD]
}
// else: GPU bring-up failed -> proceed CPU/SW-only, DO NOT push the env
//       (the guest shim then runs ring-less = quiet no-op, no crash).

// --- fork()+jump as today; the child inherits gpu_ring.ring_fd / .doorbell_fd ---

// --- on teardown (guest exited / loader done) ---
alr::gpu::alr_loader_detach_gpu_ring();     // stops the executor, frees the ring
```

WS-1 owns ONLY these ~8 lines. The ring, doorbell, executor, Mali thread, and present
are all inside the hook (WS-2). WS-1 must not hand-write the env keys — use
`gpu_ring_guest_env()` (single source of truth, mirrors `guest_shim/alr_shim_env.h`).

## WS-3 (compositor) — the `AhbFrameSource` sink (§5-B)

Until WS-3 publishes `PresentSource` (§5-C), leave `frame_sink` null and pass a
`present_window`; the executor presents straight to that window (external-OES blit,
the v114-proven path). When WS-3 is ready for zero-copy:

```cpp
gcfg.frame_sink = [](AHardwareBuffer* ahb, int w, int h, uint64_t serial) {
    // Called on the executor GL thread after glFinish (pixels are ready).
    // Import `ahb` as an EGLImage and present zero-copy via PresentSource.
    // Must complete the GPU import before returning.
};
```

## M1 staging (WS-2 produces, WS-4 installs)

- `/tmp/gpushim-stage.tar` — **conforms to §5-E**: `./` root, flat SONAME at
  `/usr/lib/androlinux/{libEGL.so.1,libGLESv2.so.2}` + dev symlinks. Private dir only
  → cannot downgrade any base lib. `/usr/lib/androlinux` MUST be first on the guest's
  `LD_LIBRARY_PATH` (WS-1 guest_env) so the shim wins over any real Mali driver.
- `/tmp/glmark2-stage.tar` — `./` root, `usr/bin/glmark2-es2-wayland` + `usr/share/glmark2`
  data. **§5-E flag for WS-4:** it ships `libjpeg.so.62*` into the *base* path
  `/usr/lib/aarch64-linux-gnu/` (glmark2 NEEDs `libjpeg.so.62` + `libpng16.so.16`).
  That is the base-lib-shadow vector WS-4 M1 guards against. Resolve by either
  (a) WS-4 ensuring `libjpeg62-turbo` + `libpng16` are in the base bookworm closure, or
  (b) WS-2 rebuilding the tar to drop glmark2's private libs under `/usr/lib/androlinux/`
  (on `LD_LIBRARY_PATH` ahead of base, so no base path is touched). Likely the same
  `libjpeg.so.62.3.0` version as base (no real downgrade) — verify against the
  installed rootfs before shipping.

## Status

- CP-0 header: **published + compile-verified** (aarch64 NDK 27.2, C++20, -Wall -Wextra, clean).
- M2 (ring connect): unblocked for WS-1 — needs the ~8-line call site above + M1 stage on device.
- Doorbell: created and advertised; the executor currently spin-polls (correct, busier).
  Doorbell-driven wakeup is a WS-2 follow-up (additive change in `alr_gpu_host_service.hpp`).
