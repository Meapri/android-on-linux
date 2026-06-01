// ALR Wayland compositor — §5-C PresentSource contract (WS-3).
//
// FROZEN cross-workstream interface (5-session orchestration plan §5; see
// docs/research/orchestration-5session-plan.md). Changing a signature here needs
// integration-session approval. This is the WS-3 side of the GPU-present
// boundary between:
//
//   * WS-2 (GPU marshalling, alr_gpu/**)        — PRODUCES finished GPU frames
//   * WS-3 (compositor/present, alr_wayland/**)  — CONSUMES them and composites
//       them onto the Android SurfaceView, zero-copy, alongside wl_shm surfaces.
//
// Per plan §5 ("계약이 서면 양쪽은 서로를 mock으로 두고 병렬 진행"): this header lets
// WS-2 compile + exercise the present-submit path NOW. Until WS-3 M2
// (zwp_linux_dmabuf_v1 + AHB->EGLImage import) lands, the submit entry is a
// logging stub that drops the frame; the signature is frozen, only the body
// changes. WS-2 keeps its executor->window direct present until
// alr_wayland_gpu_present_ready() returns true.

#ifndef ALR_WAYLAND_ALR_PRESENT_SOURCE_HPP
#define ALR_WAYLAND_ALR_PRESENT_SOURCE_HPP

#include <cstdint>

namespace alr::wayland {

// ============================================================================
//  §5-B  AhbFrameSource   — WS-2 OWNS IT (alr_gpu/alr_gpu_ring_hook.hpp)
// ============================================================================
//
// §5-B is WS-2's contract; its single source of truth is
//     alr::gpu::AhbFrameSource
//       = std::function<void(AHardwareBuffer*, int w, int h, uint64_t serial)>
// declared in alr_gpu/alr_gpu_ring_hook.hpp. WS-3 does NOT redefine it — one
// header per contract (plan §5).
//
// Data flow (per WS-2's GpuRingAttachConfig::frame_sink): WS-3 SUPPLIES the sink
// and WS-2's executor CALLS it with each finished AHardwareBuffer. WS-3 wires it
// as a thin adapter onto the §5-C entry below:
//
//     alr::gpu::GpuRingAttachConfig cfg;
//     cfg.frame_sink = [](AHardwareBuffer* b, int w, int h, uint64_t s) {
//         alr::wayland::alr_wayland_submit_gpu_frame(b, w, h, s);  // AHB* -> void*
//     };
//
// AHardwareBuffer* converts implicitly to the void* parameter below, so this
// header needs no <android/hardware_buffer.h> and stays host-compilable.

// ============================================================================
//  §5-C  PresentSource    (WS-3 provides  ->  WS-2 / L2 consume)
// ============================================================================
//
// The compositor's UNIFIED present entry. The existing wl_shm path (CPU pixels,
// CompositorConfig::present_list in alr_compositor.hpp) and this GPU/AHB path
// feed the SAME present pipeline and the SAME swap, so a guest can mix GPU and
// CPU surfaces in one composited frame.
//
// alr_wayland_submit_gpu_frame() is the concrete sink target: WS-3's §5-B
// frame_sink adapter (above) forwards each finished AHB here; the compositor
// imports it as an EGLImage and composites it zero-copy (no CPU readback — plan
// CP-4). Provided as a plain symbol so the forwarding adapter is one line.
//
//   ahardware_buffer : the finished frame, an AHardwareBuffer* (carried as void*
//                      to keep this header host-compilable; the .cpp casts it).
//   width / height   : pixel dimensions of the buffer.
//   serial           : monotonic frame / req_seq from the GPU ring (pacing/debug).
//
// Surface association: this initial contract targets a SINGLE GPU toplevel
// (glmark2 bring-up — WS-2 M3); the compositor binds the frame to the sole GPU
// surface. Multi-surface keying is a future extension (signature change => needs
// integration approval).
//
// Ownership: the buffer is BORROWED for the call. The compositor presents on its
// own thread, so when M2 lands it will AHardwareBuffer_acquire() and release on
// the next frame for that surface; WS-2 must not recycle a submitted buffer into
// its GPU write set until release. (The current drop-frame stub retains nothing,
// so WS-2 may recycle on return.)
//
// Thread-safe: callable from any thread (the GPU executor). The compositor
// marshals the submission onto its own thread — the only thread that touches
// wl_* / EGL.
void alr_wayland_submit_gpu_frame(void* ahardware_buffer, int32_t width,
                                  int32_t height, uint64_t serial);

// True once the compositor can actually composite GPU frames (WS-3 M2 done).
// WS-2 polls this to decide whether to drive the GPU present path or keep its
// executor->window direct present. False while the submit entry is a stub.
bool alr_wayland_gpu_present_ready();

}  // namespace alr::wayland

#endif  // ALR_WAYLAND_ALR_PRESENT_SOURCE_HPP
