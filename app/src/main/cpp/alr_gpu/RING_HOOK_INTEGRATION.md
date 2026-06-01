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
- **M2 (ring connect): WS-1 wiring IN PROGRESS** — `build_native_loader_probe` now `#include`s this
  hook, attaches for `glmark2` (fb 1280×720), pushes `gpu_ring_guest_env`, and `detach`es on guest
  reap. Matches this contract exactly. ✓
- Doorbell: created + advertised; the executor now blocks on the eventfd (poll, not spin).
- Off-device GLES op surface is wire-complete (harness PASS): create/shader/program/VBO/texture/
  draw-arrays, attrib-by-name + drawElements, cull/front-face, all uniform scalar/vector/matrix
  variants, FBO/renderbuffer (render-to-texture), BufferSubData/GenerateMipmap/TexSubImage2D.

## M3 device-bringup notes (for WS-1 / WS-3 — risks WS-2 can't device-test)

1. **Drawable size.** `glmark2-es2-wayland` takes its size from the WAYLAND surface (WS-3's
   compositor / the `wl_egl_window` it creates), NOT from this shim's EGL — the shim's
   `eglQuerySurface`/`eglCreateWindowSurface` are sentinels. The host AHB-FBO is `gcfg.fb_w×fb_h`
   (1280×720). If glmark2's `glViewport`/surface ≠ the AHB size, the render is cropped/scaled.
   Align the AHB size with the Wayland surface glmark2 gets, or (if glmark2 ends up gating on
   `eglQuerySurface`) we add `ALR_GPU_FB_W/H` to `gpu_ring_guest_env` + an `eglQuerySurface` that
   reads them (a small WS-2 follow-up — left out now to avoid a speculative contract change while
   M2 is mid-integration).
2. **Present path.** `present_window=null` ⇒ executor renders **headless** into the AHB (glmark2
   **score/fps is valid headless**). For an on-screen cube/scene, WS-3 consumes the AHB via
   `frame_sink` (§5-B `AhbFrameSource` → §5-C `PresentSource`), or set `gcfg.present_window`.
3. **software=false gate.** `GpuExecutorService` captures `GL_RENDERER`; WS-5's gpu bench should
   assert it's Mali (not swiftshader/llvmpipe) — `gpu_ring_frames_presented()` exposes liveness.
4. **First light target:** `glmark2 -b build:use-vbo=true` (mat4-only uniforms, no FBO) is the
   simplest scene fully covered by the wire ops; bring that up before the render-to-texture scenes.
