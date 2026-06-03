// ALR minimal Wayland compositor — public interface.
//
// Part of Phase 3 (see third_party/VENDORING.md and the Phase 3 design). This
// stands up a REAL libwayland-server display inside the Android app process on
// its own native thread, accepts an unmodified Wayland client (GTK/SDL) over an
// AF_UNIX socket, and pulls committed wl_shm buffers ready to be uploaded to a
// GL texture and drawn onto the SurfaceView's ANativeWindow.
//
// The EGL/GLES presentation itself is intentionally NOT implemented here — it is
// delegated to a single hook (`present_buffer`) so the lead can wire it to the
// existing, proven EGL+ANativeWindow path in runtime_report.cpp without this
// module duplicating that code.
//
// Threading: alr_start_wayland_compositor() spawns a detached thread that owns
// the wl_display, its wl_event_loop, and an epoll reactor. All wl_* and (later)
// GL calls happen on that thread. Callers must treat the returned handle's
// pixel-presentation callback as running on the compositor thread.

#ifndef ALR_WAYLAND_ALR_COMPOSITOR_HPP
#define ALR_WAYLAND_ALR_COMPOSITOR_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace alr::wayland {

// A committed wl_shm buffer, normalized for the presenter. `pixels` points into
// the client's shared-memory pool and is only valid for the duration of the
// present_buffer() call (the compositor releases the wl_buffer right after).
struct PresentFrame {
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;       // bytes per row
    uint32_t shm_format = 0;  // WL_SHM_FORMAT_* (0=ARGB8888, 1=XRGB8888)
    const void* pixels = nullptr;
    uint32_t serial = 0;      // surface commit serial (monotonic)
};

// One placed, ready-to-upload surface in a composited frame. Unlike PresentFrame
// (which borrows the client's shm pool for the duration of one call), the pixel
// data here is a compositor-owned copy that stays valid until the present()
// callback returns, so the presenter can upload several of them in one pass.
//
// pixels is ALWAYS tightly packed: stride == width*4, top-left origin, the same
// little-endian B,G,R,A byte order wl_shm delivers (the presenter swizzles .bgra
// and V-flips, exactly as in the single-surface path). The compositor does the
// stride repack on commit so the presenter never sees padded rows here.
struct PresentSurface {
    const void* pixels = nullptr;  // width*height*4 bytes, owned by compositor
    int32_t width = 0;
    int32_t height = 0;
    // Placement rectangle in SurfaceView/output pixels (top-left origin, +Y down).
    // The presenter maps this rect to NDC and draws the textured quad there.
    int32_t dst_x = 0;
    int32_t dst_y = 0;
    int32_t dst_w = 0;
    int32_t dst_h = 0;
    // Stable per-surface key so the presenter can cache a GL texture across frames
    // and only re-upload when the pixels change (content_serial bumps on commit).
    uint64_t surface_key = 0;
    uint64_t content_serial = 0;
    uint32_t z = 0;  // 0 = bottommost; larger = nearer the top (paint order)
    // GPU path (§5-C, see alr_present_source.hpp): when non-null this is an
    // AHardwareBuffer* (opaque void*) the presenter imports as an EGLImage and
    // samples zero-copy; `pixels` is then ignored. null => CPU/shm surface (the
    // existing path). Set by the GPU present pipeline once WS-3 M2 lands.
    void* ahb = nullptr;
};

// Whole-frame presentation hook: invoked on the compositor thread whenever the
// composited scene changes (any mapped surface commits). `surfaces` is ordered
// bottom->top; the presenter clears once, draws each quad in order, swaps once.
// `out_w`/`out_h` are the logical output (SurfaceView) size the placement rects
// were computed against. The vector and its pixel buffers are valid only for the
// duration of the call.
using PresentListCallback =
    std::function<void(const std::vector<PresentSurface>& surfaces,
                       int32_t out_w, int32_t out_h)>;

// Presentation hook. Invoked on the compositor thread on every surface commit
// that carries a wl_shm buffer. Default is a no-op logger; the lead installs a
// real implementation that uploads `pixels` via glTexImage2D and draws a
// textured quad onto the ANativeWindow, then eglSwapBuffers.
//   *** EGL WIRING TODO (lead): see present_buffer() in alr_compositor.cpp and
//       the integration notes returned by the scaffolding task. ***
using PresentCallback = std::function<void(const PresentFrame&)>;

// Configuration for the compositor instance.
struct CompositorConfig {
    // Absolute path to the AF_UNIX socket to create, e.g.
    // "<cacheDir>/alr-xdg/wayland-0". The parent dir is created (0700) and used
    // as XDG_RUNTIME_DIR for the guest; the basename becomes WAYLAND_DISPLAY.
    std::string socket_path;

    // Logical output size advertised to clients (wl_output mode + the first
    // xdg_toplevel.configure). Use the SurfaceView pixel size. 0 => default.
    int32_t output_width = 0;
    int32_t output_height = 0;
    int32_t output_refresh_mhz = 60000;  // 60 Hz in mHz
    int32_t output_scale = 1;
    // Device display metrics so the Wayland output advertises the device's real
    // physical size (mm) and the guest renders at the correct DPI. 0 = unknown.
    int32_t density_dpi = 0;  // Android DisplayMetrics.densityDpi
    float xdpi = 0.0f;        // physical x dpi (DisplayMetrics.xdpi)
    float ydpi = 0.0f;        // physical y dpi (DisplayMetrics.ydpi)

    // Optional: installed by the lead to receive committed shm frames for EGL
    // upload. If null, commits are logged but not drawn.
    PresentCallback present;

    // Multi-surface presentation hook. If set, this REPLACES `present` for drawing
    // (the compositor calls this with the full z-ordered surface list on every
    // composite-changing commit). If null, the legacy single-surface `present`
    // path above is used. The lead installs this to composite all toplevels.
    PresentListCallback present_list;
};

// Opaque running-compositor handle. Destroying it requests the thread to stop
// and joins it.
class Compositor;

// Start the compositor on a new thread. Returns a human-readable status string
// (the same style as the runtime_report.cpp probes), e.g.
//   "ALR WAYLAND COMPOSITOR: started socket=<path> globals=wl_compositor,..."
// or an error line on failure. On success the singleton stays alive until
// alr_stop_wayland_compositor() is called.
//
// This is the JNI-ready entry the lead wires to a native method. The minimal
// form takes just the socket path; the richer form takes a full config (so the
// lead can pass the SurfaceView size and the EGL present callback).
std::string alr_start_wayland_compositor(const std::string& socket_path);
std::string alr_start_wayland_compositor(const CompositorConfig& config);

// Stop and join the running compositor (idempotent). Returns a status string.
std::string alr_stop_wayland_compositor();

// True if a compositor thread is currently running.
bool alr_wayland_compositor_running();

// ---- Input injection (call from any thread; events are queued and delivered to
// the focused client on the compositor thread). Coordinates are surface-local
// pixels. Button/key codes are Linux evdev codes. ----
void alr_wayland_inject_pointer_motion(double x, double y);
void alr_wayland_inject_pointer_button(uint32_t evdev_button, uint32_t pressed);
void alr_wayland_inject_pointer_axis(double value, int32_t axis);  // axis: 0=vertical 1=horizontal
void alr_wayland_inject_touch(int32_t id, double x, double y, int32_t phase);  // 0=down 1=move 2=up
void alr_wayland_inject_key(uint32_t evdev_key, uint32_t pressed);
// Inject a synthetic burst (motion+click+touch+key) at the given point for
// verifying the input path end to end. Returns the number of events queued.
int alr_wayland_inject_selftest(double x, double y);

// ---------------------------------------------------------------------------
// Clipboard bridge (Android <-> Linux-guest selection). See
// docs/design/android-clipboard-bridge.md. The compositor owns the
// wl_data_device selection model; this header exposes only std::function sinks
// and plain-data setters so alr_compositor.cpp stays free of any <jni.h>
// dependency (the JNI glue lives in runtime_report.cpp, exactly like `present`).
//
// Threading: the sinks are invoked on the compositor thread (a raw pthread); a
// JNI sink installed by runtime_report.cpp must AttachCurrentThread. The setter
// (alr_wayland_set_android_selection) is callable from any thread — it enqueues
// onto a clipboard queue and wakes the reactor.
// ---------------------------------------------------------------------------

// Native -> Android: the GUEST published a new selection advertising `mimes`.
// Android decides whether/what to pull (we pull eagerly, then the text/image
// sinks below deliver the bytes). Called on the compositor thread.
using ClipboardGuestOfferCb =
    std::function<void(const std::vector<std::string>& mimes)>;
// Native -> Android: the guest bytes for a text `mime` (UTF-8) are ready.
using ClipboardGuestTextCb =
    std::function<void(const std::string& mime, const std::string& utf8)>;
// Native -> Android: the guest bytes for image/png are ready.
using ClipboardGuestImageCb =
    std::function<void(const std::string& png_bytes)>;

// Install the Android-side sinks. Pass empty std::functions to clear. Copies
// the std::functions; callable from any thread (stored under a mutex).
void alr_wayland_set_clipboard_sink(ClipboardGuestOfferCb on_offer,
                                    ClipboardGuestTextCb on_text,
                                    ClipboardGuestImageCb on_image);

// Android -> guest: the Android primary clip changed. `mimes` is the set the
// host can satisfy (e.g. {"text/plain;charset=utf-8","text/plain","UTF8_STRING"}
// and/or "text/html"/"image/png"). `text`/`html` are UTF-8 (empty if absent);
// `png` is raw PNG bytes (empty if absent). Empty `mimes` clears the Android
// selection. Enqueues + wakes the reactor; the compositor synthesizes a
// server-owned wl_data_offer on each guest data_device. Callable from any thread.
void alr_wayland_set_android_selection(const std::vector<std::string>& mimes,
                                       const std::string& text,
                                       const std::string& html,
                                       const std::string& png);

}  // namespace alr::wayland

#endif  // ALR_WAYLAND_ALR_COMPOSITOR_HPP
