// ALR minimal Wayland compositor — implementation.
//
// REAL libwayland-server (vendored, built with the NDK). Registers the globals
// an unmodified GTK3/SDL client needs to map a top-level window, accepts a
// commit, and pulls the wl_shm buffer out to the present_buffer() hook.
//
// What is real here:
//   * wl_display_create / wl_display_init_shm / wl_display_add_socket
//   * wl_compositor (v4) -> wl_surface (v4) + wl_region (v1, accepted/ignored)
//   * wl_shm (provided by wl_display_init_shm; ARGB8888/XRGB8888)
//   * xdg_wm_base (v2) -> xdg_surface -> xdg_toplevel, with the configure
//     handshake (first map sends xdg_toplevel.configure + xdg_surface.configure)
//   * wl_seat (v7) and wl_output (v4) globals (capabilities/mode advertised;
//     wl_output v4 emits name/description for chromium's display enumeration)
//   * an epoll reactor folding wl_event_loop_get_fd() + a wakeup eventfd
//
// What is deliberately a hook/TODO:
//   * present_buffer(): upload pixels via glTexImage2D + draw a textured quad on
//     the ANativeWindow + eglSwapBuffers. Left to the lead to avoid duplicating
//     the proven EGL path in runtime_report.cpp. See the big TODO block below.
//
// Logging uses __android_log_print so a connecting client is observable in
// logcat ("client bound: wl_compositor", "surface committed shm ...", etc.).

#include "alr_wayland/alr_compositor.hpp"
#include "alr_wayland/alr_present_source.hpp"  // §5-C GPU present contract
#include "alr_wayland/alr_xkb_keymap_us.h"     // embedded self-contained XKB keymap

#include <android/log.h>
#include <android/hardware_buffer.h>  // M2 §5-C: AHardwareBuffer_acquire/release

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
#include <wayland-server-core.h>
// Generated server glue (checked in under third_party/wayland_generated).
#include "wayland-server-protocol.h"
#include "xdg-shell-server-protocol.h"
}

#define ALR_WL_TAG "alr_wayland"
#define ALR_WL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ALR_WL_TAG, __VA_ARGS__)
#define ALR_WL_LOGW(...) __android_log_print(ANDROID_LOG_WARN, ALR_WL_TAG, __VA_ARGS__)
#define ALR_WL_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ALR_WL_TAG, __VA_ARGS__)

namespace alr::wayland {
namespace {

// ---- Versions advertised for each global (must be <= what the generated
// interface tables support; see *_INTERFACE in the generated headers). ----
constexpr int kCompositorVersion = 4;
constexpr int kShmVersion = 1;  // wl_display_init_shm manages wl_shm itself
constexpr int kSeatVersion = 7;  // generated ceiling is 10; GDK negotiates MIN(v,5)
// wl_output v4 (generated ceiling) so the bind handler can emit name/description.
// chromium ozone/wayland's WaylandOutput keys display enumeration off wl_output and
// is happier with a named output; GTK/Qt/SDL/foot all negotiate MIN(advertised,client)
// so v4 is a no-op regression risk for them (they ignore name/description if older).
constexpr int kOutputVersion = 4;
constexpr int kXdgWmBaseVersion = 2;

// ---------------------------------------------------------------------------
// Per-instance state. One Compositor owns one wl_display on one thread.
// ---------------------------------------------------------------------------

// Monotonic key source so every wl_surface gets a stable id the presenter can use
// to cache a texture across frames (pointers are unstable: a freed SurfaceState
// address may be reused). Compositor thread only.
uint64_t g_next_surface_key = 1;

struct SurfaceState {
    struct wl_resource* surface = nullptr;       // wl_surface
    struct wl_resource* xdg_surface = nullptr;   // xdg_surface (role object)
    struct wl_resource* xdg_toplevel = nullptr;  // xdg_toplevel role
    struct wl_resource* xdg_popup = nullptr;     // xdg_popup role (if a popup)
    struct wl_resource* pending_buffer = nullptr;  // attached, not yet committed
    struct wl_resource* frame_callback = nullptr;  // wl_callback for next frame
    bool configured = false;  // initial configure handshake done?
    bool mapped = false;      // role assigned AND last commit had a buffer

    // --- retained content (compositor-owned copy of the latest committed buffer,
    // tightly packed width*4, top-left origin, BGRA-in-memory). Kept so the
    // presenter can redraw this surface even when a DIFFERENT surface is the one
    // committing this frame (its own shm buffer was released right after commit).
    std::vector<uint8_t> pixels;
    int32_t buf_w = 0;
    int32_t buf_h = 0;
    uint64_t content_serial = 0;  // bumped each time `pixels` is refreshed
    uint64_t key = 0;             // stable id (assigned at create time)

    // --- placement metadata (the compositor positions windows) ---
    // For a toplevel: its xdg parent toplevel resource (set_parent), or null.
    // For a popup: the parent is tracked via parent_key below.
    struct wl_resource* parent_toplevel = nullptr;
    uint64_t parent_key = 0;      // popup: key of the surface it is anchored to
    int32_t popup_x = 0, popup_y = 0;  // popup: offset within parent (output px)
    int32_t popup_w = 0, popup_h = 0;  // popup: positioner size
    uint64_t map_serial = 0;      // popup: monotonic stamp set when it last mapped
    std::string title;            // last set_title (used as a dialog heuristic)
    bool is_popup = false;
    bool had_explicit_null_attach = false;  // client did attach(null) this cycle
    // --- §5-C GPU present: a WS-2-rendered AHardwareBuffer bound to this surface for
    // zero-copy import by the presenter. Acquired in alr_wayland_submit_gpu_frame,
    // released when the next GPU frame for this surface binds. null => CPU/shm only.
    void* gpu_ahb = nullptr;   // AHardwareBuffer* (compositor holds one acquire ref)
    int32_t gpu_w = 0;
    int32_t gpu_h = 0;
    uint64_t gpu_serial = 0;
    // M3 P0-4: wl_subsurface — a child surface composited at an offset within its
    // parent (GTK menus/tooltips). Tracked by parent KEY (never deref a parent
    // resource). sub_x/sub_y are the set_position offset relative to the parent.
    bool is_subsurface = false;
    uint64_t sub_parent_key = 0;
    int32_t sub_x = 0;
    int32_t sub_y = 0;
};

}  // namespace

class Compositor {
public:
    explicit Compositor(CompositorConfig config) : config_(std::move(config)) {}

    // Runs on the dedicated thread. Sets everything up, then drives the reactor.
    void run() {
        if (!setup()) {
            ready_.store(true);
            teardown();
            return;
        }
        ready_.store(true);
        reactor();
        teardown();
    }

    void request_stop() {
        stopping_.store(true);
        wake();
    }

    // Wake the reactor (e.g. after enqueuing an injected input event). Safe to
    // call from any thread.
    void wake() {
        if (wakeup_fd_ >= 0) {
            const uint64_t one = 1;
            ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
            (void)n;
        }
    }

    bool wait_ready_ok() {
        while (!ready_.load()) {
            std::this_thread::yield();
        }
        return setup_ok_;
    }

    const std::string& status() const { return status_; }
    const CompositorConfig& config() const { return config_; }

    // --- presentation: called from surface commit on the compositor thread ---
    void present(const PresentFrame& frame) {
        if (config_.present) {
            config_.present(frame);
        } else {
            // Default observable no-op so a commit is visible even before the
            // lead wires EGL.
            ALR_WL_LOGI("present_buffer (no EGL hook installed): %dx%d stride=%d fmt=%u serial=%u",
                        frame.width, frame.height, frame.stride, frame.shm_format, frame.serial);
        }
    }

    // --- presentation (multi-surface): called from present_composited() on the
    // compositor thread with the full z-ordered surface list. ---
    void present_list_frame(const std::vector<PresentSurface>& surfaces,
                            int32_t out_w, int32_t out_h) {
        if (config_.present_list) {
            config_.present_list(surfaces, out_w, out_h);
        } else {
            ALR_WL_LOGI("present_list (no EGL list hook): %zu surface(s) out=%dx%d",
                        surfaces.size(), out_w, out_h);
        }
    }

    struct wl_display* display() { return display_; }
    // Compositor-thread accessor used by the clipboard pipe reader/writer to fold
    // an anonymous pipe fd into the same reactor (no extra threads). Valid only on
    // the compositor thread.
    struct wl_event_loop* loop() { return loop_; }

private:
    // ----- lifecycle -----
    bool setup();
    void teardown();
    void reactor();
    void drain_input_queue();  // compositor thread: queue -> wl input protocol
    void drain_gpu_queue();    // compositor thread: bind submitted AHB frames (§5-C)
    void drain_clipboard_queue();  // compositor thread: apply Android->guest selections
    bool register_globals();
    bool make_socket();

    // ----- global bind callbacks (static thunks -> member) -----
    static void bind_compositor(struct wl_client*, void*, uint32_t, uint32_t);
    static void bind_seat(struct wl_client*, void*, uint32_t, uint32_t);
    static void bind_output(struct wl_client*, void*, uint32_t, uint32_t);
    static void bind_xdg_wm_base(struct wl_client*, void*, uint32_t, uint32_t);
    static void bind_subcompositor(struct wl_client*, void*, uint32_t, uint32_t);
    static void bind_data_device_manager(struct wl_client*, void*, uint32_t, uint32_t);

    CompositorConfig config_;
    std::string status_;
    struct wl_display* display_ = nullptr;
    struct wl_event_loop* loop_ = nullptr;
    struct wl_global* g_compositor_ = nullptr;
    struct wl_global* g_seat_ = nullptr;
    struct wl_global* g_output_ = nullptr;
    struct wl_global* g_xdg_wm_base_ = nullptr;
    struct wl_global* g_subcompositor_ = nullptr;
    struct wl_global* g_data_device_manager_ = nullptr;

    int epoll_fd_ = -1;
    int wakeup_fd_ = -1;
    int loop_fd_ = -1;
    int frame_timer_fd_ = -1;  // ~60 Hz frame-callback pacing

    std::atomic<bool> ready_{false};
    std::atomic<bool> stopping_{false};
    bool setup_ok_ = false;
};

// ===========================================================================
// Protocol implementations. These are plain functions (C callback ABI). They
// recover the owning Compositor* from wl_resource user-data / display data.
// ===========================================================================
namespace {

// A single active compositor instance (Phase 3 hosts exactly one display).
std::mutex g_instance_mutex;
Compositor* g_instance = nullptr;
std::thread g_thread;

Compositor* instance() {
    return g_instance;
}

// ---- input injection: events posted from the JNI/UI thread, drained on the
// compositor thread (wl_resource sends must happen there). ----
enum class InjectKind : uint8_t {
    PointerMotion, PointerButton, PointerAxis,
    TouchDown, TouchMotion, TouchUp, TouchFrame, Key,
};
struct InjectEvent {
    InjectKind kind;
    double x = 0, y = 0;
    uint32_t button = 0;    // evdev BTN_* (pointer) or evdev keycode (key)
    uint32_t state = 0;     // 1=pressed/down, 0=released/up
    int32_t touch_id = 0;
    int32_t axis = 0;       // 0=vertical, 1=horizontal
    double axis_value = 0;
    uint32_t time_ms = 0;
};
std::mutex g_inject_mutex;
std::vector<InjectEvent> g_inject_queue;  // guarded by g_inject_mutex

// §5-C GPU frame submissions from the WS-2 executor thread, drained on the
// compositor thread (drain_gpu_queue), mirroring the input queue above.
struct GpuSubmit { void* ahb = nullptr; int32_t w = 0, h = 0; uint64_t serial = 0; };
std::mutex g_gpu_mutex;
std::vector<GpuSubmit> g_gpu_queue;  // guarded by g_gpu_mutex
// §5-C fullscreen fallback: a GPU AHB with no Wayland toplevel to bind to (a headless /
// fullscreen GPU app like glmark2). Presented fullscreen when no other surface covers
// the screen. Compositor-thread-only (set in drain_gpu_queue, read in present_composited).
void* g_fullscreen_gpu_ahb = nullptr;
int32_t g_fullscreen_gpu_w = 0;
int32_t g_fullscreen_gpu_h = 0;
uint64_t g_fullscreen_gpu_serial = 0;

// M4: real-modifier state derived from injected key events so wl_keyboard.modifiers
// tells the client which mods are held (Shift/Ctrl/Alt/Super/AltGr) + Caps lock —
// without it, modified keys never register on the client. Bit positions are the
// real-modifier indices defined by the modifier_map of the keymap WE ACTUALLY SHIP
// (alr_xkb_keymap_us.h); they must match it exactly or the client's xkb_state
// diverges from the keys we inject. The shipped keymap binds (see modifier_map):
//   Shift   = bit 0   { <LFSH>, <RTSH> }                  evdev 42, 54
//   Lock    = bit 1   { <CAPS> }                          evdev 58 (locking, below)
//   Control = bit 2   { <LCTL>, <RCTL> }                  evdev 29, 97
//   Mod1    = bit 3   { <LALT>, <RALT>, <ALT>, <META> }   evdev 56, 100  (RALT==Alt_R)
//   Mod4    = bit 6   { <LWIN>, <RWIN>, <SUPR> }           evdev 125, 126
//   Mod5    = bit 7   { <LVL3> }                           evdev 84 (ISO_Level3/AltGr)
// NOTE: in this keymap <RALT> (evdev 100) is plain Alt_R -> Mod1, NOT AltGr/Mod5.
// AltGr/LevelThree is <LVL3> (evdev 84) only. A previous version mapped evdev 100
// to Mod5, which made the client enter LevelThree on right-Alt and emit the wrong
// glyphs (Shift/Ctrl still worked; right-Alt+key was corrupted).
uint32_t g_mods_depressed = 0;  // momentary mods currently held
uint32_t g_mods_locked = 0;     // locking mods (CapsLock)
inline uint32_t evdev_to_mod_bit(uint32_t code) {
    switch (code) {
        case 42: case 54:   return 1u << 0;  // KEY_LEFT/RIGHTSHIFT -> Shift
        case 29: case 97:   return 1u << 2;  // KEY_LEFT/RIGHTCTRL  -> Control
        case 56: case 100:  return 1u << 3;  // KEY_LEFT/RIGHTALT   -> Mod1 (RALT==Alt_R)
        case 84:            return 1u << 7;  // KEY_LEVEL3/AltGr     -> Mod5 (<LVL3>)
        case 125: case 126: return 1u << 6;  // KEY_LEFT/RIGHTMETA  -> Mod4
        default:            return 0;
    }
}
// Clear momentary (depressed) modifiers when the keyboard leaves a surface. We inject
// from a single Android keyboard source, so once focus moves the previous window's
// held-mod assumptions are stale; if a modifier key-up was lost across the transition
// (focus-follow tap, unmap, grab change) the depressed bit would otherwise leak into
// the next wl_keyboard.enter and give the freshly-focused window phantom-held
// Shift/Ctrl/Alt. Locked mods (CapsLock) intentionally persist — they model a latched
// indicator state, not a held key. The fresh state is re-sent on the next enter.
inline void clear_momentary_mods() { g_mods_depressed = 0; }
std::vector<struct wl_resource*> g_pointers;
std::vector<struct wl_resource*> g_keyboards;
std::vector<struct wl_resource*> g_touches;
struct wl_resource* g_focus_surface = nullptr;
bool g_pointer_entered = false;
bool g_keyboard_entered = false;
// P0-3: a popup (menu) that took a keyboard grab — while set, keys route here instead
// of the focused toplevel so menu keyboard navigation (arrows/Enter/Escape) works.
// null => no grab (keys go to g_focus_surface, unchanged). Cleared on the popup's
// teardown; leaves are sent at the grab transitions (popup_grab/popup_resource_destroy)
// where the surfaces are known alive, so no dangling-resource tracking is needed.
struct wl_resource* g_keyboard_grab_surface = nullptr;

// Pointer/touch target tracking. Distinct from g_focus_surface (keyboard focus,
// always the top toplevel): a mapped xdg_popup (GTK menu/combobox/tooltip) steals
// the *pointer* so its items are clickable. g_input_target_surface is the
// wl_surface that the LAST pointer/touch event was sent to; when the target
// changes we send leave-to-old / enter-to-new so GTK updates hover correctly.
struct wl_resource* g_input_target_surface = nullptr;
// Monotonic stamp bumped each time a popup MAPS, so input_target_at() can
// pick the most-recently-mapped popup (the innermost open submenu) deterministically.
uint64_t g_next_map_serial = 1;

// Touch grab: per Wayland a wl_touch sequence belongs to the surface named at
// wl_touch.down; every later motion/up for that touch-id stays with it (an implicit
// grab) regardless of where the finger moves. g_touch_target is that surface and
// g_touch_active counts the touch-ids currently down on it. When the grabbed surface
// is unmapped/destroyed mid-sequence we wl_touch.cancel (the only correct way to end
// it — no up will ever arrive) and clear these. Compositor-thread-only.
struct wl_resource* g_touch_target = nullptr;
int g_touch_active = 0;

uint32_t now_ms() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}

// Frame pacing: wl_surface.frame callbacks are not acked immediately (which makes
// clients busy-loop) — they are collected here and fired on a ~60 Hz timerfd tick
// in the reactor. The destroy listener drops a callback if its client disconnects
// before the tick, so the pending list never dangles.
std::vector<struct wl_resource*> g_pending_frames;
void frame_cb_destroyed(struct wl_resource* r) {
    g_pending_frames.erase(std::remove(g_pending_frames.begin(), g_pending_frames.end(), r),
                           g_pending_frames.end());
}

// Present coalescing: a composite-changing commit only marks the scene dirty; the
// reactor's ~60 Hz frame timer presents at most once per tick. This bounds
// eglSwapBuffers to <=60/s regardless of a client's commit storm (GIMP commits the
// splash dozens of times), which is what overruns the BLASTBufferQueue (max 4+2).
// Compositor-thread-only (set in surface_commit / *_destroy, read+cleared in the
// timerfd tick, all on g_thread) -> plain bool, no atomic/mutex needed.
bool g_scene_dirty = false;
bool g_first_present_done = false;  // first composite presents immediately; rest coalesce

// ---- wl_buffer release helper ----
void release_buffer(struct wl_resource* buffer) {
    if (buffer) {
        wl_buffer_send_release(buffer);
    }
}

// ---- wl_touch cancel helper ----
// If a touch sequence is in progress on `surface` (the grabbed touch target), end it
// with wl_touch.cancel: the protocol's only way to terminate a grab when no up will
// arrive (the surface is being unmapped/destroyed under the finger). Per spec, cancel
// drops ALL active touch points and is followed by a frame. Clears the grab state.
// Safe to call with a still-ALIVE `surface` resource; callers invoke it before the
// surface goes away. No-op if `surface` isn't the current touch target.
void cancel_touch_if_targeting(struct wl_resource* surface) {
    if (!surface || g_touch_target != surface || g_touch_active <= 0) {
        if (g_touch_target == surface) { g_touch_target = nullptr; g_touch_active = 0; }
        return;
    }
    for (auto* t : g_touches) {
        wl_touch_send_cancel(t);
        if (wl_resource_get_version(t) >= WL_TOUCH_FRAME_SINCE_VERSION)
            wl_touch_send_frame(t);
    }
    g_touch_target = nullptr;
    g_touch_active = 0;
}

// ---- multi-surface compositing: forward declarations (definitions below, after
// surface_resource_destroy, where Compositor/SurfaceState are complete). The
// early surface callbacks (commit/destroy/create) call these. ----
void registry_add(SurfaceState* s);
void registry_remove(SurfaceState* s);
void zorder_raise(SurfaceState* s);
void zorder_remove(SurfaceState* s);
SurfaceState* zorder_top();
void present_composited();

// =================== wl_surface ===================
void surface_destroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
void surface_attach(struct wl_client*, struct wl_resource* resource,
                    struct wl_resource* buffer, int32_t, int32_t) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    if (s) {
        s->pending_buffer = buffer;
        s->had_explicit_null_attach = (buffer == nullptr);
    }
}
void surface_damage(struct wl_client*, struct wl_resource*, int32_t, int32_t,
                    int32_t, int32_t) {}
void surface_frame(struct wl_client* client, struct wl_resource* resource,
                   uint32_t callback) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    struct wl_resource* cb =
        wl_resource_create(client, &wl_callback_interface, 1, callback);
    if (cb && s) {
        wl_resource_set_implementation(cb, nullptr, nullptr, frame_cb_destroyed);
        s->frame_callback = cb;
    }
}
void surface_set_opaque_region(struct wl_client*, struct wl_resource*,
                               struct wl_resource*) {}
void surface_set_input_region(struct wl_client*, struct wl_resource*,
                              struct wl_resource*) {}
void surface_commit(struct wl_client*, struct wl_resource* resource) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    Compositor* comp = instance();
    if (!s || !comp) return;

    bool composite_dirty = false;

    if (s->pending_buffer) {
        struct wl_shm_buffer* shm = wl_shm_buffer_get(s->pending_buffer);
        if (shm) {
            wl_shm_buffer_begin_access(shm);
            const int32_t w = wl_shm_buffer_get_width(shm);
            const int32_t h = wl_shm_buffer_get_height(shm);
            const int32_t stride = wl_shm_buffer_get_stride(shm);
            const uint32_t fmt = wl_shm_buffer_get_format(shm);
            const auto* src = static_cast<const uint8_t*>(wl_shm_buffer_get_data(shm));
            // Retain a tightly-packed (width*4) top-left-origin copy so the
            // presenter can redraw this surface on frames driven by OTHER surfaces
            // (this shm buffer is released right after commit). Same byte order
            // wl_shm delivers (BGRA-in-memory); the presenter swizzles + V-flips.
            if (w > 0 && h > 0 && src != nullptr) {
                const int32_t tight = w * 4;
                s->pixels.resize(static_cast<std::size_t>(tight) *
                                 static_cast<std::size_t>(h));
                if (stride == tight) {
                    std::memcpy(s->pixels.data(), src,
                                static_cast<std::size_t>(tight) * h);
                } else {
                    for (int32_t y = 0; y < h; ++y) {
                        std::memcpy(s->pixels.data() + static_cast<std::size_t>(y) * tight,
                                    src + static_cast<std::size_t>(y) * stride,
                                    static_cast<std::size_t>(tight));
                    }
                }
                s->buf_w = w;
                s->buf_h = h;
                s->content_serial = static_cast<uint64_t>(
                    wl_display_next_serial(comp->display()));
                composite_dirty = true;

                // MAP transition: a surface with a role + a real buffer becomes
                // visible. Add to z-order (toplevel) and take focus.
                if (!s->mapped) {
                    s->mapped = true;
                    if (s->xdg_toplevel) {
                        zorder_raise(s);  // most-recently-mapped on top
                        // P0-1: a new toplevel steals focus while the previous one is
                        // still ALIVE (e.g. a GIMP dialog opening) -> send the old
                        // surface wl_keyboard.leave first, or it keeps believing it
                        // holds the keyboard (stuck modifiers / IME to the wrong
                        // window). Destroy paths intentionally send no leave (gone).
                        if (g_keyboard_entered && g_focus_surface &&
                            g_focus_surface != s->surface) {
                            for (auto* k : g_keyboards)
                                wl_keyboard_send_leave(
                                    k, wl_display_next_serial(comp->display()),
                                    g_focus_surface);
                        }
                        clear_momentary_mods();  // fresh mods for the new toplevel/dialog
                        g_focus_surface = s->surface;
                        g_pointer_entered = false;
                        g_keyboard_entered = false;
                    } else if (s->is_popup) {
                        // Stamp the popup so input_target_at() can prefer the
                        // most-recently-mapped (innermost) popup when submenus stack.
                        s->map_serial = g_next_map_serial++;
                    }
                }
                ALR_WL_LOGI("surface committed shm: %dx%d stride=%d fmt=%u key=%llu role=%s",
                            w, h, stride, fmt,
                            static_cast<unsigned long long>(s->key),
                            s->xdg_toplevel ? "toplevel" : (s->is_popup ? "popup" : "none"));
            }
            wl_shm_buffer_end_access(shm);
        } else {
            ALR_WL_LOGW("surface commit: attached buffer is not wl_shm (no dmabuf path yet)");
        }
        // We copied the pixels above; release immediately so the client's
        // double-buffered pool keeps flowing.
        release_buffer(s->pending_buffer);
        s->pending_buffer = nullptr;
    } else if (s->mapped && s->had_explicit_null_attach) {
        // UNMAP transition: client attached a NULL buffer then committed -> hide.
        // (Only treat an *explicit* null-attach as unmap; a plain commit with no
        // new attach keeps the last content, per wl_surface semantics.)
        s->mapped = false;
        zorder_remove(s);
        if (g_focus_surface == s->surface) {
            // P0-1: s is unmapped but still ALIVE -> release the keyboard from it.
            if (g_keyboard_entered) {
                for (auto* k : g_keyboards)
                    wl_keyboard_send_leave(
                        k, wl_display_next_serial(comp->display()), s->surface);
            }
            clear_momentary_mods();  // window gone: don't leak its held mods onward
            g_focus_surface = nullptr;
            g_pointer_entered = false;
            g_keyboard_entered = false;
            if (SurfaceState* nt = zorder_top()) {
                g_focus_surface = nt->surface;
            }
        }
        // P0-2: s is unmapped but still ALIVE -> release the pointer from it too, or
        // GTK keeps hover/grab state and the next menu mis-behaves (won't reopen).
        // The next drain re-resolves the pointer target (parent popup or toplevel).
        if (g_input_target_surface == s->surface) {
            if (g_pointer_entered) {
                for (auto* p : g_pointers) {
                    wl_pointer_send_leave(p, wl_display_next_serial(comp->display()),
                                          s->surface);
                    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                        wl_pointer_send_frame(p);
                }
            }
            g_input_target_surface = nullptr;
            g_pointer_entered = false;
        }
        // End a touch grab on the now-hidden surface (still ALIVE here) so an
        // in-flight tap doesn't dangle without an up.
        cancel_touch_if_targeting(s->surface);
        composite_dirty = true;
        ALR_WL_LOGI("surface unmapped (null buffer) key=%llu",
                    static_cast<unsigned long long>(s->key));
    }
    s->had_explicit_null_attach = false;

    // Frame pacing: queue the frame callback instead of acking immediately, so the
    // client redraws at ~60 Hz (the reactor's timerfd fires the queue) rather than
    // busy-looping. The callback's destroy listener keeps the queue clean if the
    // client goes away before the next tick.
    if (s->frame_callback) {
        g_pending_frames.push_back(s->frame_callback);
        s->frame_callback = nullptr;
    }

    // Recomposite the whole scene if anything visible changed — but COALESCE it to
    // the frame-pacing tick instead of presenting synchronously here. Pixels were
    // already retained into s->pixels and z-order/focus already updated above, so
    // deferring only the eglSwapBuffers is safe. The reactor's ~60 Hz timer
    // (frame_timer_fd_) presents once if g_scene_dirty. This bounds swaps to <=60/s
    // and stops the BLASTBufferQueue "Already acquired max frames" overrun under
    // commit storms (GIMP commits its splash dozens of times).
    if (composite_dirty) {
        g_scene_dirty = true;
        // First-ever content: present promptly so the very first frame (the splash)
        // isn't delayed up to ~16 ms behind the first tick. After that, coalesce.
        if (!g_first_present_done) {
            g_first_present_done = true;
            g_scene_dirty = false;
            present_composited();
        }
    }
}
void surface_set_buffer_transform(struct wl_client*, struct wl_resource*, int32_t) {}
void surface_set_buffer_scale(struct wl_client*, struct wl_resource*, int32_t) {}
void surface_damage_buffer(struct wl_client*, struct wl_resource*, int32_t,
                           int32_t, int32_t, int32_t) {}
void surface_offset(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void surface_get_release(struct wl_client*, struct wl_resource*, uint32_t) {}

const struct wl_surface_interface kSurfaceImpl = {
    surface_destroy,
    surface_attach,
    surface_damage,
    surface_frame,
    surface_set_opaque_region,
    surface_set_input_region,
    surface_commit,
    surface_set_buffer_transform,
    surface_set_buffer_scale,
    surface_damage_buffer,
    surface_offset,
    surface_get_release,
};

void surface_resource_destroy(struct wl_resource* resource) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    if (s != nullptr) {
        // If this surface had input focus, clear it so injected input never sends
        // to a destroyed surface (a use-after-free when a client exits — common
        // once multiple toplevels/popups come and go, e.g. GTK dialogs/menus).
        if (g_focus_surface == s->surface) {
            g_focus_surface = nullptr;
            g_pointer_entered = false;
            g_keyboard_entered = false;
        }
        // Same UAF guard for the pointer/touch target (which may be a popup, i.e.
        // NOT g_focus_surface): never leave it pointing at a freed wl_surface.
        if (g_input_target_surface == s->surface) {
            g_input_target_surface = nullptr;
            g_pointer_entered = false;
        }
        // End any touch grab held by this surface. The wl_surface is still ALIVE in
        // this destroy listener, so a wl_touch.cancel is well-formed; without it a
        // client whose window vanishes mid-tap never sees an up and stays stuck.
        cancel_touch_if_targeting(s->surface);
        // And for the keyboard grab (a menu popup that grabbed keys): if the surface
        // is torn down BEFORE its xdg_popup destroy listener runs (client disconnect
        // gives no resource-destroy order guarantee), drain_input_queue's Key path
        // would otherwise wl_keyboard_send_* to this freed surface — UAF.
        if (g_keyboard_grab_surface == s->surface) {
            g_keyboard_grab_surface = nullptr;
            g_keyboard_entered = false;
        }
        const bool was_visible = s->mapped;
        zorder_remove(s);
        registry_remove(s);
        // §5-C: release any retained GPU AHB bound to this surface (one acquire ref
        // held by drain_gpu_queue) so a GPU client exiting doesn't leak the buffer.
        if (s->gpu_ahb) {
            AHardwareBuffer_release(static_cast<AHardwareBuffer*>(s->gpu_ahb));
            s->gpu_ahb = nullptr;
        }
        // Refocus the new top-most window and repaint the remaining scene.
        if (SurfaceState* nt = zorder_top()) {
            g_focus_surface = nt->surface;
            g_pointer_entered = false;
            g_keyboard_entered = false;
        }
        if (was_visible) g_scene_dirty = true;  // repaint on next tick (erase the gone window)
    }
    delete s;
}

// ---------------------------------------------------------------------------
// Multi-surface compositing state (compositor thread only).
//
// g_zorder holds the currently MAPPED toplevels, bottom (front of vector) -> top
// (back of vector). The back element is the focused / top-most window. Popups are
// NOT in g_zorder; they are discovered per-frame by walking surfaces and matched
// to their parent so they always paint just above it.
//
// We keep a registry of every live SurfaceState so the snapshot builder can find
// a toplevel's child popups. (A handful of windows; linear scans are fine.)
// ---------------------------------------------------------------------------
std::vector<SurfaceState*> g_zorder;       // mapped toplevels, bottom->top
std::vector<SurfaceState*> g_all_surfaces; // every live SurfaceState

void registry_add(SurfaceState* s) { g_all_surfaces.push_back(s); }
void registry_remove(SurfaceState* s) {
    g_all_surfaces.erase(std::remove(g_all_surfaces.begin(), g_all_surfaces.end(), s),
                         g_all_surfaces.end());
}

[[maybe_unused]] SurfaceState* surface_state_for_wl(struct wl_resource* wl_surface_res) {
    if (!wl_surface_res) return nullptr;
    for (SurfaceState* s : g_all_surfaces)
        if (s->surface == wl_surface_res) return s;
    return nullptr;
}
[[maybe_unused]] SurfaceState* surface_state_for_key(uint64_t key) {
    if (key == 0) return nullptr;
    for (SurfaceState* s : g_all_surfaces)
        if (s->key == key) return s;
    return nullptr;
}

// Climb a popup's parent_key chain to the toplevel that ultimately owns it,
// summing the popup offsets along the way. GTK opens submenus as a popup anchored
// to ANOTHER popup (File > Export As > ...): such a nested popup's parent_key is
// the parent popup's key, NOT a toplevel key, so present_composited's per-toplevel
// match (`pp->parent_key == toplevel->key`) never catches it and the submenu was
// drawn nowhere (input still worked — surface_screen_rect already climbs). This
// returns the owning toplevel (or null) and, if out_off_x/out_off_y are given, the
// accumulated popup offset relative to that toplevel's placement origin, so the
// snapshot builder can paint nested popups at the right spot. Key-based throughout
// (surface_state_for_key) — never dereferences a possibly-dangling parent resource
// (the v106/v108 UAF discipline). Bounded by a 32-deep guard against cycles.
SurfaceState* popup_owning_toplevel(SurfaceState* popup, int32_t* out_off_x,
                                    int32_t* out_off_y) {
    int32_t ox = 0, oy = 0;
    SurfaceState* toplevel = nullptr;
    SurfaceState* cur = popup;
    for (int guard = 0; guard < 32 && cur && cur->is_popup; ++guard) {
        ox += cur->popup_x;
        oy += cur->popup_y;
        SurfaceState* parent = surface_state_for_key(cur->parent_key);
        if (!parent) break;                       // parent gone; can't anchor
        if (!parent->is_popup) { toplevel = parent; break; }  // reached a toplevel
        cur = parent;                             // nested submenu: keep climbing
    }
    if (out_off_x) *out_off_x = ox;
    if (out_off_y) *out_off_y = oy;
    return toplevel;
}

// Raise `s` to the top of the z-order (most-recently-activated wins). Idempotent.
void zorder_raise(SurfaceState* s) {
    g_zorder.erase(std::remove(g_zorder.begin(), g_zorder.end(), s), g_zorder.end());
    g_zorder.push_back(s);
}
void zorder_remove(SurfaceState* s) {
    g_zorder.erase(std::remove(g_zorder.begin(), g_zorder.end(), s), g_zorder.end());
}

// The top-most mapped toplevel (the focused window), or null if none mapped.
SurfaceState* zorder_top() {
    return g_zorder.empty() ? nullptr : g_zorder.back();
}

// ----- placement: the compositor decides where each window goes -----
struct Rect { int32_t x = 0, y = 0, w = 0, h = 0; };

// Output (SurfaceView) size the placements are computed against. Falls back to
// the buffer size if the config did not carry an output size.
void output_size(int32_t fallback_w, int32_t fallback_h, int32_t* ow, int32_t* oh) {
    Compositor* comp = instance();
    int32_t w = (comp && comp->config().output_width  > 0) ? comp->config().output_width  : fallback_w;
    int32_t h = (comp && comp->config().output_height > 0) ? comp->config().output_height : fallback_h;
    if (w <= 0) w = fallback_w > 0 ? fallback_w : 1;
    if (h <= 0) h = fallback_h > 0 ? fallback_h : 1;
    *ow = w; *oh = h;
}

// Letterbox-fit src (buf_w x buf_h) inside the out_w x out_h view, preserving
// aspect ratio and centering. This is how the MAIN, parentless toplevel is shown:
// as large as possible without distortion. (If src already matches the output we
// get an exact full-view rect, identical to today's single-surface fill.)
Rect place_fit(int32_t buf_w, int32_t buf_h, int32_t out_w, int32_t out_h) {
    Rect r;
    if (buf_w <= 0 || buf_h <= 0) { r.w = out_w; r.h = out_h; return r; }
    // Scale to fit (min of the two ratios). Use 64-bit to avoid overflow.
    const double sx = static_cast<double>(out_w) / buf_w;
    const double sy = static_cast<double>(out_h) / buf_h;
    const double s = sx < sy ? sx : sy;
    r.w = static_cast<int32_t>(buf_w * s + 0.5);
    r.h = static_cast<int32_t>(buf_h * s + 0.5);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

// Center src at NATIVE size over the parent rect (dialog look), clamped to the
// view so it never runs off-screen.
Rect place_centered_over(int32_t buf_w, int32_t buf_h, const Rect& parent,
                         int32_t out_w, int32_t out_h) {
    Rect r;
    r.w = buf_w > 0 ? buf_w : parent.w;
    r.h = buf_h > 0 ? buf_h : parent.h;
    // Don't let a dialog exceed the view.
    if (r.w > out_w) r.w = out_w;
    if (r.h > out_h) r.h = out_h;
    r.x = parent.x + (parent.w - r.w) / 2;
    r.y = parent.y + (parent.h - r.h) / 2;
    // Clamp into [0, out-size].
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.w > out_w) r.x = out_w - r.w;
    if (r.y + r.h > out_h) r.y = out_h - r.h;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    return r;
}

// Decide whether a mapped toplevel should be treated as a dialog (centered over
// its parent at native size) vs. a primary window (fit to the view). A toplevel
// is a dialog if it has an explicit xdg parent, OR it is clearly smaller than the
// output (covers GIMP's "Welcome to GIMP 3.0.2", which sets no parent but is much
// smaller than the editing window / the screen).
bool looks_like_dialog(const SurfaceState* s, int32_t out_w, int32_t out_h) {
    if (s->parent_toplevel != nullptr) return true;
    if (s->buf_w <= 0 || s->buf_h <= 0) return false;
    // Heuristic: a window that covers less than ~70% of the output in either
    // dimension is a secondary/dialog window. The main editing window is sized to
    // (or near) the output by GTK when it maximizes onto our single wl_output.
    const bool narrow = s->buf_w * 10 < out_w * 7;
    const bool shortw = s->buf_h * 10 < out_h * 7;
    return narrow || shortw;
}

// Compute a toplevel's placement rect. `out_w/out_h` is the view size.
Rect place_toplevel(SurfaceState* s, int32_t out_w, int32_t out_h) {
    if (looks_like_dialog(s, out_w, out_h)) {
        // Parent rect: the parent toplevel's own placement if we have it, else the
        // bottom-most mapped toplevel (the main window), else the whole view.
        Rect parent;
        SurfaceState* p = nullptr;
        // Find the parent among LIVE mapped toplevels ONLY. Never dereference the
        // stored parent_toplevel resource — it may be a destroyed/dangling xdg
        // resource (e.g. GIMP tears down its "Startup" splash toplevel while a
        // later dialog still references it → use-after-free, the v106 crash).
        // Pointer-VALUE comparison against a live surface's xdg_toplevel is safe
        // even if parent_toplevel dangles (no deref); a stale address simply won't
        // match any live surface and we fall through to the size-based scan.
        if (s->parent_toplevel) {
            for (SurfaceState* cand : g_zorder) {
                if (cand != s && cand->xdg_toplevel == s->parent_toplevel) { p = cand; break; }
            }
        }
        if (!p) {
            for (SurfaceState* cand : g_zorder) {
                if (cand != s && !looks_like_dialog(cand, out_w, out_h)) { p = cand; break; }
            }
        }
        if (p && p != s) {
            parent = place_fit(p->buf_w, p->buf_h, out_w, out_h);
        } else {
            parent = Rect{0, 0, out_w, out_h};
        }
        return place_centered_over(s->buf_w, s->buf_h, parent, out_w, out_h);
    }
    return place_fit(s->buf_w, s->buf_h, out_w, out_h);
}

// Build the ordered snapshot and hand it to the presenter. Called on every
// composite-changing commit. Walks g_zorder bottom->top; after each toplevel,
// appends any mapped popups anchored to it (so popups paint just above their
// parent). Uses the per-surface retained `pixels` copies (valid for the call).
void present_composited() {
    Compositor* comp = instance();
    if (!comp) return;

    // Determine output size from config, falling back to the top window's buffer.
    int32_t out_w = 0, out_h = 0;
    SurfaceState* top = zorder_top();
    output_size(top ? top->buf_w : 0, top ? top->buf_h : 0, &out_w, &out_h);

    std::vector<PresentSurface> snap;
    snap.reserve(g_zorder.size() * 2 + 1);
    uint32_t z = 0;

    for (SurfaceState* s : g_zorder) {
        if (!s) continue;  // defensive: never deref a stray/stale z-order entry
        // A surface presents if it has shm pixels OR a §5-C GPU buffer. drain_gpu_queue
        // sets buf_w/buf_h from the AHB dims for a GPU-only surface, so the placement
        // below works for both; the presenter prefers ps.ahb (zero-copy) over ps.pixels.
        if (!s->mapped || (s->pixels.empty() && s->gpu_ahb == nullptr) ||
            s->buf_w <= 0 || s->buf_h <= 0) continue;
        const Rect r = place_toplevel(s, out_w, out_h);
        PresentSurface ps;
        ps.pixels = s->pixels.empty() ? nullptr : s->pixels.data();
        ps.ahb = s->gpu_ahb;  // §5-C: non-null => presenter imports zero-copy
        ps.width = s->buf_w;
        ps.height = s->buf_h;
        ps.dst_x = r.x; ps.dst_y = r.y; ps.dst_w = r.w; ps.dst_h = r.h;
        ps.surface_key = s->key;
        ps.content_serial = s->content_serial;
        ps.z = z++;
        snap.push_back(ps);

        // P0-4: composite subsurfaces parented to this toplevel, above it, at their
        // set_position offset (GTK menus/tooltips). Same shape as the popup loop;
        // matched by parent KEY, never by dereferencing a parent resource.
        for (SurfaceState* su : g_all_surfaces) {
            if (!su || su == s) continue;
            if (!su->is_subsurface || !su->mapped || su->pixels.empty()) continue;
            if (su->sub_parent_key != s->key) continue;
            if (su->buf_w <= 0 || su->buf_h <= 0) continue;
            PresentSurface sq;
            sq.pixels = su->pixels.data();
            sq.width = su->buf_w;
            sq.height = su->buf_h;
            int32_t sx = r.x + su->sub_x;
            int32_t sy = r.y + su->sub_y;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            sq.dst_x = sx; sq.dst_y = sy; sq.dst_w = su->buf_w; sq.dst_h = su->buf_h;
            sq.surface_key = su->key;
            sq.content_serial = su->content_serial;
            sq.z = z++;
            snap.push_back(sq);
        }

        // Append popups OWNED by this toplevel, on top of it, at their offset.
        // "Owned" follows the full parent_key chain (popup_owning_toplevel), so a
        // nested submenu (popup anchored to another popup) paints above its toplevel
        // too — not just popups whose direct parent is this toplevel. Collect first,
        // then emit in map order (innermost submenu last = on top), so a child
        // submenu always draws over the parent menu it opened from.
        struct PopupHit { SurfaceState* pp; int32_t off_x; int32_t off_y; };
        std::vector<PopupHit> hits;
        for (SurfaceState* pp : g_all_surfaces) {
            if (!pp || pp == s) continue;  // defensive: skip stray entries / self
            if (!pp->is_popup || !pp->mapped || pp->pixels.empty()) continue;
            int32_t off_x = 0, off_y = 0;
            if (popup_owning_toplevel(pp, &off_x, &off_y) != s) continue;
            hits.push_back({pp, off_x, off_y});
        }
        std::sort(hits.begin(), hits.end(), [](const PopupHit& a, const PopupHit& b) {
            return a.pp->map_serial < b.pp->map_serial;
        });
        for (const PopupHit& hit : hits) {
            SurfaceState* pp = hit.pp;
            PresentSurface pq;
            pq.pixels = pp->pixels.data();
            pq.width = pp->buf_w;
            pq.height = pp->buf_h;
            // Offset accumulated up the popup chain, relative to the toplevel origin.
            int32_t px = r.x + hit.off_x;
            int32_t py = r.y + hit.off_y;
            int32_t pw = pp->buf_w > 0 ? pp->buf_w : pp->popup_w;
            int32_t ph = pp->buf_h > 0 ? pp->buf_h : pp->popup_h;
            // Clamp into the view.
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            if (px + pw > out_w) px = out_w - pw;
            if (px < 0) px = 0;
            if (py + ph > out_h) py = out_h - ph;
            if (py < 0) py = 0;
            pq.dst_x = px; pq.dst_y = py; pq.dst_w = pw; pq.dst_h = ph;
            pq.surface_key = pp->key;
            pq.content_serial = pp->content_serial;
            pq.z = z++;
            snap.push_back(pq);
        }
    }

    // §5-C fullscreen fallback: present a GPU AHB that has no toplevel to bind to (a
    // fullscreen GPU app) when nothing else covers the screen.
    if (snap.empty() && g_fullscreen_gpu_ahb && g_fullscreen_gpu_w > 0 &&
        g_fullscreen_gpu_h > 0) {
        PresentSurface ps;
        ps.pixels = nullptr;
        ps.ahb = g_fullscreen_gpu_ahb;
        ps.width = g_fullscreen_gpu_w;
        ps.height = g_fullscreen_gpu_h;
        ps.dst_x = 0; ps.dst_y = 0; ps.dst_w = out_w; ps.dst_h = out_h;
        ps.surface_key = 0;
        ps.content_serial = g_fullscreen_gpu_serial;
        ps.z = 0;
        snap.push_back(ps);
    }

    if (comp->config().present_list) {
        comp->present_list_frame(snap, out_w, out_h);
    } else if (!snap.empty()) {
        // Legacy fallback: no list hook installed — present the TOP surface only
        // through the single-surface callback so behaviour degrades to "last
        // window wins" exactly like before this change.
        const PresentSurface& t = snap.back();
        PresentFrame f{};
        f.width = t.width; f.height = t.height; f.stride = t.width * 4;
        f.shm_format = 0; f.pixels = t.pixels;
        f.serial = static_cast<uint32_t>(t.content_serial);
        comp->present(f);
    }
}

// Map an output/SurfaceView pixel coordinate to surface-local pixels for the
// focused toplevel. Returns false if there is no focused, placed surface (caller
// then drops the event). Mirrors place_toplevel() so input lands where the window
// is actually drawn. (Superseded for live input by map_input_to_surface(), which
// also handles popups; kept as the canonical toplevel-mapping reference.)
[[maybe_unused]] bool map_input_to_focus(double in_x, double in_y, double* out_x, double* out_y) {
    SurfaceState* top = zorder_top();
    if (!top || top->buf_w <= 0 || top->buf_h <= 0) { *out_x = in_x; *out_y = in_y; return top != nullptr; }
    int32_t ow = 0, oh = 0;
    output_size(top->buf_w, top->buf_h, &ow, &oh);
    const Rect r = place_toplevel(top, ow, oh);
    if (r.w <= 0 || r.h <= 0) { *out_x = in_x; *out_y = in_y; return true; }
    // Linear map from the placement rect to [0, buf_w) x [0, buf_h).
    const double lx = (in_x - r.x) * (static_cast<double>(top->buf_w) / r.w);
    const double ly = (in_y - r.y) * (static_cast<double>(top->buf_h) / r.h);
    *out_x = lx; *out_y = ly;
    return true;
}

// On-screen placement rect of `tgt` (a toplevel or a popup), matching EXACTLY how
// present_composited() positions it. For a popup we resolve the parent chain by
// KEY against the live registry (never dereferencing a possibly-dangling parent
// resource — the v106/v108 UAF discipline): accumulate popup offsets up to the
// owning toplevel, then add that toplevel's placement-rect origin. Popup size is
// buf_w/buf_h (falling back to the positioner size), clamped into the view, just
// like the snapshot builder. Returns {0,0,0,0} if the target can't be placed.
Rect surface_screen_rect(SurfaceState* tgt, int32_t out_w, int32_t out_h) {
    if (!tgt) return Rect{};
    if (!tgt->is_popup) {
        return place_toplevel(tgt, out_w, out_h);
    }
    // Walk parent_key -> ... -> toplevel, summing popup offsets along the way.
    // Shared with present_composited so input lands EXACTLY where the popup draws.
    int32_t off_x = 0, off_y = 0;
    SurfaceState* toplevel = popup_owning_toplevel(tgt, &off_x, &off_y);
    Rect base = toplevel ? place_toplevel(toplevel, out_w, out_h)
                         : Rect{0, 0, out_w, out_h};
    Rect r;
    r.w = tgt->buf_w > 0 ? tgt->buf_w : tgt->popup_w;
    r.h = tgt->buf_h > 0 ? tgt->buf_h : tgt->popup_h;
    r.x = base.x + off_x;
    r.y = base.y + off_y;
    // Same clamp present_composited() applies so input lands where it is drawn.
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.w > 0 && r.x + r.w > out_w) r.x = out_w - r.w;
    if (r.x < 0) r.x = 0;
    if (r.h > 0 && r.y + r.h > out_h) r.y = out_h - r.h;
    if (r.y < 0) r.y = 0;
    return r;
}

// Coordinate-aware hit-test: the top-most MAPPED TOPLEVEL whose on-screen rect
// actually contains (in_x,in_y). Reuses surface_screen_rect() — the SAME placement
// helper present_composited() draws with — so a pointer/touch lands on whatever
// window is visibly under it. Walks g_zorder top->bottom (back->front of the vector)
// so an overlapping dialog drawn on top of the main window wins when both cover the
// point, but a tap OUTSIDE a smaller dialog (over the exposed main window) correctly
// targets the main window instead of always hijacking to z-order top. Returns null
// if no mapped toplevel covers the point (caller can then fall back to z-order top).
SurfaceState* toplevel_at(double in_x, double in_y, int32_t out_w, int32_t out_h) {
    for (auto it = g_zorder.rbegin(); it != g_zorder.rend(); ++it) {
        SurfaceState* s = *it;
        if (!s || !s->mapped || s->buf_w <= 0 || s->buf_h <= 0) continue;
        const Rect r = surface_screen_rect(s, out_w, out_h);
        if (r.w <= 0 || r.h <= 0) continue;
        if (in_x >= r.x && in_x < r.x + r.w && in_y >= r.y && in_y < r.y + r.h)
            return s;
    }
    return nullptr;
}

// The surface a pointer/touch at (in_x,in_y) should target. A mapped popup (GTK menu/
// combobox/tooltip) holds an implicit grab, so while one is open ALL pointer/touch go
// to the innermost popup regardless of coordinate (a tap outside dismisses it via the
// client's grab-broken handling) — this preserves the existing menu behaviour. With no
// popup open, hit-test the coordinate against the placed toplevels (toplevel_at) so a
// tap lands on the window actually drawn under the finger (e.g. the main window when
// you tap beside a smaller centred dialog), falling back to z-order top only if the
// point misses every window. Never dereferences a dangling resource (key/value only).
//
// Multi-GUI correctness: a popup's implicit grab is scoped to the client/toplevel that
// opened it, NOT global. With several apps up at once (e.g. a GIMP window leaving a
// tooltip popup mapped while the user taps a separate Qt window), an UNQUALIFIED
// "highest map_serial popup wins" would hijack EVERY tap to the unrelated app's popup.
// So only a popup OWNED by the focused (top-most) toplevel grabs input; a popup
// belonging to a different toplevel is ignored for grab purposes and the tap falls
// through to coordinate hit-testing. Ownership is resolved by KEY via
// popup_owning_toplevel (never dereferencing a possibly-dangling parent resource).
SurfaceState* input_target_at(double in_x, double in_y) {
    int32_t ow = 0, oh = 0;
    SurfaceState* top = zorder_top();
    output_size(top ? top->buf_w : 0, top ? top->buf_h : 0, &ow, &oh);
    // Prefer the innermost (highest map_serial) mapped popup owned by the focused
    // toplevel — that is the only popup whose grab should steal this app's input.
    SurfaceState* best_popup = nullptr;
    for (SurfaceState* s : g_all_surfaces) {
        if (!s || !s->is_popup || !s->mapped || s->pixels.empty()) continue;
        // Only honour the grab when this popup's tree roots at the focused toplevel.
        // (top == null => no toplevel mapped; a popup with no owning toplevel can't be
        // placed/grabbed sensibly, so it is skipped here and erased on its teardown.)
        if (!top || popup_owning_toplevel(s, nullptr, nullptr) != top) continue;
        if (!best_popup || s->map_serial > best_popup->map_serial) best_popup = s;
    }
    if (best_popup) return best_popup;
    if (SurfaceState* hit = toplevel_at(in_x, in_y, ow, oh)) return hit;
    if (top) return top;
    return surface_state_for_wl(g_focus_surface);
}

// Map an output/SurfaceView pixel coordinate to surface-local pixels for an
// ARBITRARY target surface (toplevel or popup). For a toplevel this matches
// map_input_to_focus(); for a popup it uses the popup's on-screen rect so a tap on
// a menu item lands on the right widget. Returns false if the target has no usable
// buffer/rect (caller drops the event).
bool map_input_to_surface(SurfaceState* tgt, double in_x, double in_y,
                          double* out_x, double* out_y) {
    if (!tgt || tgt->buf_w <= 0 || tgt->buf_h <= 0) { *out_x = in_x; *out_y = in_y; return tgt != nullptr; }
    int32_t ow = 0, oh = 0;
    output_size(tgt->buf_w, tgt->buf_h, &ow, &oh);
    const Rect r = surface_screen_rect(tgt, ow, oh);
    if (r.w <= 0 || r.h <= 0) { *out_x = in_x; *out_y = in_y; return true; }
    *out_x = (in_x - r.x) * (static_cast<double>(tgt->buf_w) / r.w);
    *out_y = (in_y - r.y) * (static_cast<double>(tgt->buf_h) / r.h);
    return true;
}

// Keyboard focus-follow: a press (pointer button down / touch down) on a DIFFERENT
// toplevel than the one that currently holds the keyboard makes that toplevel the
// keyboard-focused window, and raises it to the top of the z-order so the next
// dialog/menu anchors to it (the convention: keyboard goes to zorder_top, never a
// popup). Without this, two mapped toplevels (e.g. netsurf + a separate dialog, or
// two GUI apps at once) would leave keys stuck on whichever window mapped last —
// tapping the other window moved only the POINTER target, not keyboard focus.
//
// Honors an active popup keyboard grab: while a menu holds g_keyboard_grab_surface
// the focus is the grab, not a toplevel, so we don't disturb it (a press outside the
// menu is the client's own grab-dismiss path; the grab is released on popup teardown,
// after which the next press lands here). Pure key/value comparison against live
// SurfaceState — never dereferences a possibly-dangling parent/role resource (the
// v106/v108 UAF discipline). No-op if `tgt` is null, not a mapped toplevel, already
// the focus, or a keyboard grab is in effect.
void focus_follow_to_toplevel(SurfaceState* tgt) {
    if (!tgt || !tgt->surface || !tgt->xdg_toplevel || !tgt->mapped) return;
    // A menu/combobox grabbing keys owns keyboard routing; don't override it here.
    if (g_keyboard_grab_surface) return;
    if (g_focus_surface == tgt->surface) {
        // Same window — still make sure it's the raised one (a press re-activates it).
        if (zorder_top() != tgt) { zorder_raise(tgt); g_scene_dirty = true; }
        return;
    }
    Compositor* comp = instance();
    if (!comp) return;
    // Leave the previously-focused (still-ALIVE) surface so it stops believing it
    // holds the keyboard (stuck modifiers / IME to the wrong window), mirroring the
    // P0-1 map-time focus steal. The next Key event sends enter to the new focus.
    if (g_keyboard_entered && g_focus_surface && g_focus_surface != tgt->surface) {
        for (auto* k : g_keyboards)
            wl_keyboard_send_leave(k, wl_display_next_serial(comp->display()),
                                   g_focus_surface);
    }
    clear_momentary_mods();      // don't leak held Shift/Ctrl/Alt into the new focus
    g_focus_surface = tgt->surface;
    g_keyboard_entered = false;  // force a fresh wl_keyboard.enter on the next key
    zorder_raise(tgt);           // most-recently-activated window is on top
    g_scene_dirty = true;        // repaint: the raised window draws above the others
    ALR_WL_LOGI("keyboard focus-follow -> key=%llu (tap raised toplevel)",
                static_cast<unsigned long long>(tgt->key));
}

// =================== wl_region (accept + ignore) ===================
void region_destroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
void region_add(struct wl_client*, struct wl_resource*, int32_t, int32_t,
                int32_t, int32_t) {}
void region_subtract(struct wl_client*, struct wl_resource*, int32_t, int32_t,
                     int32_t, int32_t) {}
const struct wl_region_interface kRegionImpl = {region_destroy, region_add,
                                                region_subtract};

// =================== wl_compositor ===================
void compositor_create_surface(struct wl_client* client,
                               struct wl_resource* resource, uint32_t id) {
    auto* s = new SurfaceState();
    struct wl_resource* surf = wl_resource_create(
        client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!surf) {
        delete s;
        wl_client_post_no_memory(client);
        return;
    }
    s->surface = surf;
    s->key = g_next_surface_key++;
    registry_add(s);  // track for the snapshot builder / popup parent lookup
    wl_resource_set_implementation(surf, &kSurfaceImpl, s, surface_resource_destroy);
    ALR_WL_LOGI("wl_compositor.create_surface id=%u key=%llu", id,
                static_cast<unsigned long long>(s->key));
}
void compositor_create_region(struct wl_client* client,
                              struct wl_resource* resource, uint32_t id) {
    struct wl_resource* reg = wl_resource_create(
        client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (!reg) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(reg, &kRegionImpl, nullptr, nullptr);
}
void compositor_release(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
const struct wl_compositor_interface kCompositorImpl = {
    compositor_create_surface, compositor_create_region, compositor_release};

// =================== xdg_toplevel ===================
void send_initial_configure(SurfaceState* s) {
    Compositor* comp = instance();
    if (!comp || !s || !s->xdg_surface) return;
    // Drive the client to fill the whole output. Without a real size AND a
    // fullscreen/maximized state, GTK/GIMP picks its own (small) default size and
    // the window shows as a narrow band. The presenter stretches the single
    // surface to the entire SurfaceView, so a fullscreen client also makes the
    // injected touch coordinates line up 1:1 with the client's widgets.
    // configure is in LOGICAL (surface) coordinates; with a HiDPI buffer scale of
    // S the client renders an S*logical-px buffer. Logical size = device px / S.
    const int32_t scale = comp->config().output_scale > 0 ? comp->config().output_scale : 1;
    int32_t w = (comp->config().output_width > 0 ? comp->config().output_width : 1280) / scale;
    int32_t h = (comp->config().output_height > 0 ? comp->config().output_height : 720) / scale;
    struct wl_array states;
    wl_array_init(&states);
    auto push_state = [&states](uint32_t v) {
        if (uint32_t* st = static_cast<uint32_t*>(wl_array_add(&states, sizeof(uint32_t))))
            *st = v;
    };
    push_state(XDG_TOPLEVEL_STATE_ACTIVATED);
    push_state(XDG_TOPLEVEL_STATE_FULLSCREEN);
    push_state(XDG_TOPLEVEL_STATE_MAXIMIZED);
    if (s->xdg_toplevel) {
        xdg_toplevel_send_configure(s->xdg_toplevel, w, h, &states);
    }
    const uint32_t serial = wl_display_next_serial(comp->display());
    xdg_surface_send_configure(s->xdg_surface, serial);
    wl_array_release(&states);
    s->configured = true;
    ALR_WL_LOGI("sent initial xdg configure w=%d h=%d serial=%u", w, h, serial);
}

void toplevel_destroy(struct wl_client*, struct wl_resource* resource) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    if (s) {
        Compositor* comp = instance();
        s->mapped = false;
        s->xdg_toplevel = nullptr;
        zorder_remove(s);
        if (g_focus_surface == s->surface) {
            clear_momentary_mods();  // destroyed toplevel: don't leak held mods onward
            g_focus_surface = nullptr;
            g_pointer_entered = false;
            g_keyboard_entered = false;
            if (SurfaceState* nt = zorder_top()) g_focus_surface = nt->surface;
        }
        // The xdg_toplevel ROLE is gone but s->surface is still ALIVE here (GTK can
        // destroy the role and reuse the wl_surface). If this surface was the pointer
        // target or held the keyboard grab, drop it WITH a real leave so the client
        // doesn't keep believing it owns input on a now-unmapped window — symmetric
        // with the P0-2 null-buffer unmap path and popup_resource_destroy. Without
        // this the next drain_input_queue would send a leave to a surface that may be
        // torn down before the next tick (no role-vs-surface destroy order guarantee).
        if (g_input_target_surface == s->surface) {
            if (g_pointer_entered && comp) {
                for (auto* p : g_pointers) {
                    wl_pointer_send_leave(p, wl_display_next_serial(comp->display()),
                                          s->surface);
                    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                        wl_pointer_send_frame(p);
                }
            }
            g_input_target_surface = nullptr;
            g_pointer_entered = false;
        }
        // End a touch grab on the unmapped toplevel (wl_surface still ALIVE here).
        cancel_touch_if_targeting(s->surface);
        if (g_keyboard_grab_surface == s->surface) {
            if (g_keyboard_entered && comp) {
                for (auto* k : g_keyboards)
                    wl_keyboard_send_leave(k, wl_display_next_serial(comp->display()),
                                           s->surface);
            }
            g_keyboard_grab_surface = nullptr;
            g_keyboard_entered = false;
        }
        g_scene_dirty = true;  // repaint on next tick (single coalesced present path)
    }
    wl_resource_destroy(resource);
}
void toplevel_set_parent(struct wl_client*, struct wl_resource* resource,
                         struct wl_resource* parent) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    if (s) {
        s->parent_toplevel = parent;  // may be null (un-parenting)
        ALR_WL_LOGI("xdg_toplevel.set_parent key=%llu parent=%p",
                    static_cast<unsigned long long>(s->key), (void*)parent);
    }
}
void toplevel_set_title(struct wl_client*, struct wl_resource* resource,
                        const char* title) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    if (s && title) s->title = title;
    ALR_WL_LOGI("xdg_toplevel.set_title \"%s\"", title ? title : "");
}
void toplevel_set_app_id(struct wl_client*, struct wl_resource*, const char* app_id) {
    ALR_WL_LOGI("xdg_toplevel.set_app_id \"%s\"", app_id ? app_id : "");
}
void toplevel_show_window_menu(struct wl_client*, struct wl_resource*,
                               struct wl_resource*, uint32_t, int32_t, int32_t) {}
void toplevel_move(struct wl_client*, struct wl_resource*, struct wl_resource*,
                   uint32_t) {}
void toplevel_resize(struct wl_client*, struct wl_resource*, struct wl_resource*,
                     uint32_t, uint32_t) {}
void toplevel_set_max_size(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void toplevel_set_min_size(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void toplevel_set_maximized(struct wl_client*, struct wl_resource*) {}
void toplevel_unset_maximized(struct wl_client*, struct wl_resource*) {}
void toplevel_set_fullscreen(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void toplevel_unset_fullscreen(struct wl_client*, struct wl_resource*) {}
void toplevel_set_minimized(struct wl_client*, struct wl_resource*) {}
const struct xdg_toplevel_interface kToplevelImpl = {
    toplevel_destroy,        toplevel_set_parent,    toplevel_set_title,
    toplevel_set_app_id,     toplevel_show_window_menu, toplevel_move,
    toplevel_resize,         toplevel_set_max_size,  toplevel_set_min_size,
    toplevel_set_maximized,  toplevel_unset_maximized, toplevel_set_fullscreen,
    toplevel_unset_fullscreen, toplevel_set_minimized};

// =================== xdg_surface ===================
void xdg_surface_destroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
// =================== xdg_positioner / xdg_popup ===================
// GIMP/GTK use xdg_popup for every menu, combobox, and tooltip. These MUST have
// real (non-null) implementations: libwayland-server dispatches a request to
// impl[opcode], so a null impl + any client request = a null call = compositor
// (=app) crash. We accept all positioner/popup requests, remember the popup
// geometry, and send the required configure so the popup maps. (Compositing the
// popup over its parent is a later multi-surface step; for now it just doesn't
// crash and the client proceeds.)
struct PositionerState {
    int32_t width = 0, height = 0;
    int32_t anchor_x = 0, anchor_y = 0;
    int32_t offset_x = 0, offset_y = 0;
    uint32_t gravity = 0;  // xdg_positioner gravity (P1 popup placement)
};
void positioner_destroy(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
void positioner_set_size(struct wl_client*, struct wl_resource* r, int32_t w, int32_t h) {
    auto* p = static_cast<PositionerState*>(wl_resource_get_user_data(r));
    if (p) { p->width = w; p->height = h; }
}
void positioner_set_anchor_rect(struct wl_client*, struct wl_resource* r,
                                int32_t x, int32_t y, int32_t, int32_t) {
    auto* p = static_cast<PositionerState*>(wl_resource_get_user_data(r));
    if (p) { p->anchor_x = x; p->anchor_y = y; }
}
void positioner_set_anchor(struct wl_client*, struct wl_resource*, uint32_t) {}
void positioner_set_gravity(struct wl_client*, struct wl_resource* r, uint32_t gravity) {
    auto* p = static_cast<PositionerState*>(wl_resource_get_user_data(r));
    if (p) p->gravity = gravity;
}
void positioner_set_constraint_adjustment(struct wl_client*, struct wl_resource*, uint32_t) {}
void positioner_set_offset(struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
    auto* p = static_cast<PositionerState*>(wl_resource_get_user_data(r));
    if (p) { p->offset_x = x; p->offset_y = y; }
}
void positioner_set_reactive(struct wl_client*, struct wl_resource*) {}
void positioner_set_parent_size(struct wl_client*, struct wl_resource*, int32_t, int32_t) {}
void positioner_set_parent_configure(struct wl_client*, struct wl_resource*, uint32_t) {}
const struct xdg_positioner_interface kPositionerImpl = {
    positioner_destroy, positioner_set_size, positioner_set_anchor_rect,
    positioner_set_anchor, positioner_set_gravity, positioner_set_constraint_adjustment,
    positioner_set_offset, positioner_set_reactive, positioner_set_parent_size,
    positioner_set_parent_configure};
void positioner_resource_destroy(struct wl_resource* r) {
    delete static_cast<PositionerState*>(wl_resource_get_user_data(r));
}

void popup_destroy(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
void popup_grab(struct wl_client*, struct wl_resource* resource, struct wl_resource*,
                uint32_t) {
    // P0-3: the popup (menu) takes a keyboard grab -> route keys to it so the user can
    // navigate with arrows/Enter/Escape. Leave the previous keyboard holder (the prior
    // grab popup, or the toplevel — both alive here) now; the grab popup gets enter on
    // the next key. The grab is cleared on the popup's teardown.
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    if (!s || !s->surface) return;
    struct wl_resource* prev =
        g_keyboard_grab_surface ? g_keyboard_grab_surface : g_focus_surface;
    Compositor* comp = instance();
    if (g_keyboard_entered && prev && prev != s->surface && comp) {
        for (auto* k : g_keyboards)
            wl_keyboard_send_leave(k, wl_display_next_serial(comp->display()), prev);
    }
    clear_momentary_mods();  // fresh mod state for the grabbing menu (re-sent on enter)
    g_keyboard_grab_surface = s->surface;
    g_keyboard_entered = false;
}
void popup_reposition(struct wl_client*, struct wl_resource*, struct wl_resource*, uint32_t) {}
const struct xdg_popup_interface kPopupImpl = {popup_destroy, popup_grab, popup_reposition};

// When a popup resource is destroyed, unmap its surface and repaint (menus/tooltips
// come and go constantly under GTK; without this they would linger on screen).
void popup_resource_destroy(struct wl_resource* r) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(r));
    if (s) {
        // If injected pointer/touch was targeting this popup, drop the target so a
        // later event never sends to a now-unmapped/destroyed surface (UAF). The
        // next drain re-resolves the target (the parent popup or the toplevel).
        if (g_input_target_surface == s->surface) {
            // The popup's wl_surface is still ALIVE in this role-destroy listener
            // (GTK destroys the xdg_popup ROLE and reuses the surface), so send a
            // real pointer leave (+frame) before dropping it — symmetric with the
            // keyboard-grab block below and the P0-2 unmap path. Without it GTK keeps
            // the menu item's prelight/hover and the reused surface mis-behaves on the
            // next popup map.
            if (g_pointer_entered) {
                Compositor* comp = instance();
                if (comp) {
                    for (auto* p : g_pointers) {
                        wl_pointer_send_leave(p, wl_display_next_serial(comp->display()),
                                              s->surface);
                        if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                            wl_pointer_send_frame(p);
                    }
                }
            }
            g_input_target_surface = nullptr;
            g_pointer_entered = false;
        }
        // End a touch grab on the dismissed popup (wl_surface still ALIVE here): a
        // tap-and-hold on a menu item that closes the menu must not leave the touch
        // dangling.
        cancel_touch_if_targeting(s->surface);
        // P0-3: release the keyboard grab if this menu held it; leave the menu (alive
        // here) so the next key re-enters the toplevel (g_focus_surface).
        if (g_keyboard_grab_surface == s->surface) {
            Compositor* comp = instance();
            if (g_keyboard_entered && comp) {
                for (auto* k : g_keyboards)
                    wl_keyboard_send_leave(k, wl_display_next_serial(comp->display()),
                                           s->surface);
            }
            clear_momentary_mods();  // don't carry the menu's held mods back to the toplevel
            g_keyboard_grab_surface = nullptr;
            g_keyboard_entered = false;
        }
        s->is_popup = false;
        s->mapped = false;
        s->xdg_popup = nullptr;
        s->map_serial = 0;
        g_scene_dirty = true;  // repaint on next tick (erase the gone popup/menu)
    }
}

void xdg_surface_get_toplevel(struct wl_client* client,
                              struct wl_resource* resource, uint32_t id) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    struct wl_resource* tl = wl_resource_create(
        client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!tl) {
        wl_client_post_no_memory(client);
        return;
    }
    if (s) s->xdg_toplevel = tl;
    wl_resource_set_implementation(tl, &kToplevelImpl, s, nullptr);
    ALR_WL_LOGI("client mapped xdg_toplevel id=%u", id);
    // The xdg-shell protocol requires the compositor to send an initial
    // configure after the role is assigned and before the client attaches a
    // buffer. Do it now.
    send_initial_configure(s);
}
void xdg_surface_get_popup(struct wl_client* client, struct wl_resource* resource,
                           uint32_t id, struct wl_resource* parent,
                           struct wl_resource* positioner) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
    struct wl_resource* pop = wl_resource_create(
        client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    if (!pop) {
        wl_client_post_no_memory(client);
        return;
    }
    // Give the popup resource the SAME SurfaceState as its xdg_surface, so the
    // destroy listener can unmap it. (Role objects share the wl_surface's state.)
    wl_resource_set_implementation(pop, &kPopupImpl, s, popup_resource_destroy);

    const auto* p = positioner
        ? static_cast<PositionerState*>(wl_resource_get_user_data(positioner))
        : nullptr;
    const int32_t w = (p && p->width > 0) ? p->width : 1;
    const int32_t h = (p && p->height > 0) ? p->height : 1;
    int32_t x = p ? p->anchor_x + p->offset_x : 0;
    int32_t y = p ? p->anchor_y + p->offset_y : 0;
    // P1: apply positioner gravity. Default (NONE/BOTTOM/RIGHT/BOTTOM_RIGHT) keeps the
    // popup top-left at the anchor (extends down-right) = prior behaviour; LEFT/TOP
    // gravity shifts it by its size so edge submenus open the correct direction.
    const uint32_t g = p ? p->gravity : 0u;
    if (g == XDG_POSITIONER_GRAVITY_LEFT || g == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
        g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT) x -= w;
    if (g == XDG_POSITIONER_GRAVITY_TOP || g == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
        g == XDG_POSITIONER_GRAVITY_TOP_RIGHT) y -= h;

    if (s) {
        s->is_popup = true;
        s->xdg_popup = pop;
        s->popup_x = x; s->popup_y = y;
        s->popup_w = w; s->popup_h = h;
        // Resolve the parent surface -> its stable key, so the snapshot builder
        // can paint this popup above the right toplevel.
        SurfaceState* parent_state = parent
            ? static_cast<SurfaceState*>(wl_resource_get_user_data(parent))
            : nullptr;
        s->parent_key = parent_state ? parent_state->key : 0;
    }

    // The popup must receive xdg_popup.configure THEN xdg_surface.configure before
    // it can attach a buffer (xdg-shell protocol).
    xdg_popup_send_configure(pop, x, y, w, h);
    Compositor* comp = instance();
    if (comp) {
        xdg_surface_send_configure(resource, wl_display_next_serial(comp->display()));
    }
    ALR_WL_LOGI("xdg_surface.get_popup id=%u %dx%d@%d,%d parent_key=%llu", id, w, h, x, y,
                static_cast<unsigned long long>(s ? s->parent_key : 0));
}
void xdg_surface_set_window_geometry(struct wl_client*, struct wl_resource*,
                                     int32_t, int32_t, int32_t, int32_t) {}
void xdg_surface_ack_configure(struct wl_client*, struct wl_resource*, uint32_t serial) {
    ALR_WL_LOGI("xdg_surface.ack_configure serial=%u", serial);
}
const struct xdg_surface_interface kXdgSurfaceImpl = {
    xdg_surface_destroy, xdg_surface_get_toplevel, xdg_surface_get_popup,
    xdg_surface_set_window_geometry, xdg_surface_ack_configure};

// =================== xdg_wm_base ===================
void wm_base_destroy(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
void wm_base_create_positioner(struct wl_client* client,
                               struct wl_resource* resource, uint32_t id) {
    struct wl_resource* pos = wl_resource_create(
        client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    if (!pos) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(pos, &kPositionerImpl, new PositionerState(),
                                   positioner_resource_destroy);
}
void wm_base_get_xdg_surface(struct wl_client* client, struct wl_resource* resource,
                             uint32_t id, struct wl_resource* surface) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(surface));
    struct wl_resource* xs = wl_resource_create(
        client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (!xs) {
        wl_client_post_no_memory(client);
        return;
    }
    if (s) s->xdg_surface = xs;
    wl_resource_set_implementation(xs, &kXdgSurfaceImpl, s, nullptr);
    ALR_WL_LOGI("xdg_wm_base.get_xdg_surface id=%u", id);
}
void wm_base_pong(struct wl_client*, struct wl_resource*, uint32_t) {}
const struct xdg_wm_base_interface kWmBaseImpl = {
    wm_base_destroy, wm_base_create_positioner, wm_base_get_xdg_surface,
    wm_base_pong};

// =================== wl_seat (input injection) ===================
void drop_resource(std::vector<struct wl_resource*>& v, struct wl_resource* r) {
    v.erase(std::remove(v.begin(), v.end(), r), v.end());
}
void pointer_destroyed(struct wl_resource* r) { drop_resource(g_pointers, r); }
void keyboard_destroyed(struct wl_resource* r) { drop_resource(g_keyboards, r); }
void touch_destroyed(struct wl_resource* r) { drop_resource(g_touches, r); }

void pointer_set_cursor(struct wl_client*, struct wl_resource*, uint32_t,
                        struct wl_resource*, int32_t, int32_t) {}  // Android draws cursor
void pointer_release(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
const struct wl_pointer_interface kPointerImpl = {pointer_set_cursor, pointer_release};
void keyboard_release(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
const struct wl_keyboard_interface kKeyboardImpl = {keyboard_release};
void touch_release(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
const struct wl_touch_interface kTouchImpl = {touch_release};

void seat_get_pointer(struct wl_client* client, struct wl_resource* resource,
                      uint32_t id) {
    struct wl_resource* p = wl_resource_create(
        client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (!p) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(p, &kPointerImpl, nullptr, pointer_destroyed);
    g_pointers.push_back(p);
    ALR_WL_LOGI("wl_seat.get_pointer bound (now %zu)", g_pointers.size());
}
// Build a sealed memfd holding the embedded self-contained XKB_V1 keymap so guest
// clients (GDK/libxkbcommon) compile it directly via xkb_keymap_new_from_string
// and never look for rootfs /usr/share/X11/xkb data (absent -> XKB-338 -> NULL
// keymap -> SEGV). Returns fd (>=0) and sets *out_size to text+NUL length, else -1.
int make_xkb_keymap_fd(size_t* out_size) {
    const size_t size = sizeof(kUsXkbKeymapV1);  // includes trailing NUL
    int fd = static_cast<int>(::syscall(__NR_memfd_create, "alr-xkb-keymap",
                                        MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) {
        ALR_WL_LOGE("memfd_create(keymap) failed: %s", std::strerror(errno));
        return -1;
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ALR_WL_LOGE("ftruncate(keymap) failed: %s", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ALR_WL_LOGE("mmap(keymap) failed: %s", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    std::memcpy(map, kUsXkbKeymapV1, size);
    ::munmap(map, size);
    // Seal so the client can MAP_PRIVATE it safely (wl_keyboard v7+). Best-effort.
    ::fcntl(fd, F_ADD_SEALS,
            F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    *out_size = size;
    return fd;
}
void seat_get_keyboard(struct wl_client* client, struct wl_resource* resource,
                       uint32_t id) {
    struct wl_resource* k = wl_resource_create(
        client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (!k) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(k, &kKeyboardImpl, nullptr, keyboard_destroyed);
    g_keyboards.push_back(k);
    // Send a self-contained XKB_V1 keymap (alr_xkb_keymap_us.h): the guest compiles
    // it directly and never needs rootfs /usr/share/X11/xkb data (absent here ->
    // was XKB-338 -> NULL keymap -> SEGV in GTK/GIMP/foot). NO_KEYMAP fallback only
    // if the memfd can't be built.
    size_t km_size = 0;
    int km_fd = make_xkb_keymap_fd(&km_size);
    if (km_fd >= 0) {
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, km_fd,
                                static_cast<uint32_t>(km_size));
        ::close(km_fd);  // libwayland dups the fd during marshalling
        ALR_WL_LOGI("wl_keyboard.keymap sent XKB_V1 (%zu bytes)", km_size);
    } else {
        int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
                                devnull >= 0 ? devnull : 0, 0);
        if (devnull >= 0) ::close(devnull);
    }
    if (wl_resource_get_version(k) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
        wl_keyboard_send_repeat_info(k, 25, 600);
    }
    ALR_WL_LOGI("wl_seat.get_keyboard bound (now %zu)", g_keyboards.size());
}
void seat_get_touch(struct wl_client* client, struct wl_resource* resource,
                    uint32_t id) {
    struct wl_resource* t = wl_resource_create(
        client, &wl_touch_interface, wl_resource_get_version(resource), id);
    if (!t) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(t, &kTouchImpl, nullptr, touch_destroyed);
    g_touches.push_back(t);
    ALR_WL_LOGI("wl_seat.get_touch bound (now %zu)", g_touches.size());
}
void seat_release(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
const struct wl_seat_interface kSeatImpl = {seat_get_pointer, seat_get_keyboard,
                                            seat_get_touch, seat_release};

// =================== wl_output ===================
void output_release(struct wl_client*, struct wl_resource* resource) {
    wl_resource_destroy(resource);
}
const struct wl_output_interface kOutputImpl = {output_release};

// =================== wl_subsurface / wl_subcompositor ===================
// Minimal: GTK's Wayland backend probes wl_subcompositor at startup (and uses
// subsurfaces for menus/tooltips/popups). A single toplevel window needs the
// global to exist; actual subsurface compositing (stacking the child quads) is
// a later stage. These requests are accepted so GTK initializes cleanly.
void subsurface_destroy(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
void subsurface_set_position(struct wl_client*, struct wl_resource* res,
                             int32_t x, int32_t y) {
    auto* s = static_cast<SurfaceState*>(wl_resource_get_user_data(res));
    if (s) { s->sub_x = x; s->sub_y = y; }
}
void subsurface_place_above(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void subsurface_place_below(struct wl_client*, struct wl_resource*, struct wl_resource*) {}
void subsurface_set_sync(struct wl_client*, struct wl_resource*) {}
void subsurface_set_desync(struct wl_client*, struct wl_resource*) {}
const struct wl_subsurface_interface kSubsurfaceImpl = {
    subsurface_destroy, subsurface_set_position, subsurface_place_above,
    subsurface_place_below, subsurface_set_sync, subsurface_set_desync};

void subcompositor_destroy(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
void subcompositor_get_subsurface(struct wl_client* client, struct wl_resource* resource,
                                  uint32_t id, struct wl_resource* surface,
                                  struct wl_resource* parent) {
    struct wl_resource* sub = wl_resource_create(
        client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
    if (!sub) {
        wl_client_post_no_memory(client);
        return;
    }
    // P0-4: link the child surface to its parent by KEY so present_composited can
    // composite it at its offset. The subsurface resource's user_data = the child
    // SurfaceState (so set_position resolves it). Never store/deref the parent
    // resource later — match by key/value (UAF discipline).
    auto* child = surface ? static_cast<SurfaceState*>(wl_resource_get_user_data(surface))
                          : nullptr;
    auto* par = parent ? static_cast<SurfaceState*>(wl_resource_get_user_data(parent))
                       : nullptr;
    if (child) {
        child->is_subsurface = true;
        child->sub_parent_key = par ? par->key : 0;
    }
    wl_resource_set_implementation(sub, &kSubsurfaceImpl, child, nullptr);
    ALR_WL_LOGI("wl_subcompositor.get_subsurface id=%u parent_key=%llu", id,
                static_cast<unsigned long long>(par ? par->key : 0));
}
const struct wl_subcompositor_interface kSubcompositorImpl = {
    subcompositor_destroy, subcompositor_get_subsurface};

// =================== wl_data_device_manager (clipboard selection) ===================
// CRITICAL for GTK/GIMP input: GDK 3.24's Wayland backend POSTPONES binding
// wl_seat until BOTH wl_compositor AND wl_data_device_manager are advertised
// (required_device_manager_globals[] in gdkdisplay-wayland.c). Without this
// global GDK never binds the seat and receives NO input.
//
// Beyond satisfying that bind requirement, this is now a REAL selection
// (Ctrl-C / Ctrl-V) clipboard bridged to the Android host's ClipboardManager —
// see docs/design/android-clipboard-bridge.md. Drag-and-drop stays stubbed
// (start_drag / set_actions are no-ops); only the *selection* is wired.
//
// Two directions share one compositor-thread model (g_clip_* state below):
//   GUEST -> ANDROID: guest wl_data_source.offer(mimes)+set_selection records the
//     source; we pull its bytes lazily over a pipe and hand them to Android.
//   ANDROID -> GUEST: Android clip changes -> alr_wayland_set_android_selection
//     caches the payloads + synthesizes a server-owned wl_data_offer on each
//     guest's wl_data_device; the guest pastes via wl_data_offer.receive.
// ALL wl_* sends happen on the compositor thread (see clipboard.* helpers).

// Per-source mime accumulator: the user_data of a guest-created wl_data_source.
// Lives in the guest client; freed by data_source_destroyed.
struct DataSourceState {
    std::vector<std::string> mimes;
};

void data_source_offer(struct wl_client*, struct wl_resource* src, const char* mime) {
    if (!mime) return;
    auto* st = static_cast<DataSourceState*>(wl_resource_get_user_data(src));
    if (st) st->mimes.emplace_back(mime);
}
void data_source_destroy(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
void data_source_set_actions(struct wl_client*, struct wl_resource*, uint32_t /*dnd*/) {}
const struct wl_data_source_interface kDataSourceImpl = {
    data_source_offer, data_source_destroy, data_source_set_actions};

// Forward decls (defined in the clipboard section just below).
void clip_on_guest_set_selection(struct wl_resource* source, uint32_t serial);
void clip_on_data_source_destroyed(struct wl_resource* source);
void clip_on_data_device_created(struct wl_resource* dev);
void clip_on_data_device_destroyed(struct wl_resource* dev);
void clip_data_offer_receive(struct wl_resource* offer, const char* mime, int fd);
void clip_data_offer_destroyed(struct wl_resource* offer);

void data_source_destroyed(struct wl_resource* r) {
    clip_on_data_source_destroyed(r);
    auto* st = static_cast<DataSourceState*>(wl_resource_get_user_data(r));
    delete st;  // frees the mime accumulator (UAF discipline: nothing else owns it)
}

void data_device_start_drag(struct wl_client*, struct wl_resource*,
                            struct wl_resource* /*source*/, struct wl_resource* /*origin*/,
                            struct wl_resource* /*icon*/, uint32_t /*serial*/) {}
void data_device_set_selection(struct wl_client*, struct wl_resource*,
                               struct wl_resource* source, uint32_t serial) {
    clip_on_guest_set_selection(source, serial);
}
void data_device_release(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
const struct wl_data_device_interface kDataDeviceImpl = {
    data_device_start_drag, data_device_set_selection, data_device_release};

// NEW: server-owned wl_data_offer (the Android selection presented to a guest).
// `accept`/`set_actions` are no-ops for a selection; `receive` writes the cached
// Android bytes into the guest's pipe (§3b); `finish`/`destroy` clean up.
void data_offer_accept(struct wl_client*, struct wl_resource*, uint32_t /*serial*/,
                       const char* /*mime*/) {}
void data_offer_receive(struct wl_client*, struct wl_resource* offer, const char* mime,
                        int32_t fd) {
    clip_data_offer_receive(offer, mime, fd);
}
void data_offer_destroy(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
void data_offer_finish(struct wl_client*, struct wl_resource*) {}
void data_offer_set_actions(struct wl_client*, struct wl_resource*, uint32_t /*dnd*/,
                            uint32_t /*pref*/) {}
const struct wl_data_offer_interface kDataOfferImpl = {
    data_offer_accept, data_offer_receive, data_offer_destroy, data_offer_finish,
    data_offer_set_actions};
void data_offer_destroyed(struct wl_resource* r) { clip_data_offer_destroyed(r); }

void data_device_destroyed(struct wl_resource* r) { clip_on_data_device_destroyed(r); }

void ddm_create_data_source(struct wl_client* client, struct wl_resource* mgr,
                            uint32_t id) {
    struct wl_resource* res = wl_resource_create(
        client, &wl_data_source_interface, wl_resource_get_version(mgr), id);
    if (!res) { wl_client_post_no_memory(client); return; }
    // user_data = the per-source mime accumulator; destroy listener frees it AND
    // clears the selection if this source currently owns it.
    auto* st = new DataSourceState();
    wl_resource_set_implementation(res, &kDataSourceImpl, st, data_source_destroyed);
}
void ddm_get_data_device(struct wl_client* client, struct wl_resource* mgr,
                         uint32_t id, struct wl_resource* /*seat*/) {
    struct wl_resource* res = wl_resource_create(
        client, &wl_data_device_interface, wl_resource_get_version(mgr), id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &kDataDeviceImpl, nullptr, data_device_destroyed);
    // Track the device + (if Android currently owns the selection) push the
    // current offer so a guest started after the copy still sees it.
    clip_on_data_device_created(res);
}
void ddm_release(struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); }
const struct wl_data_device_manager_interface kDataDeviceManagerImpl = {
    ddm_create_data_source, ddm_get_data_device, ddm_release};

// ===================== clipboard selection model (impl) =====================
// All state + helpers below run ONLY on the compositor thread, except the
// inbound queue (g_clip_queue) + the sink registry (g_clip_sink_*), which are
// mutex-guarded because Kotlin/JNI calls them from other threads.

enum class SelOwner : uint8_t { None, Guest, Android };
SelOwner g_clip_owner = SelOwner::None;               // compositor thread
struct wl_resource* g_clip_guest_source = nullptr;    // current guest wl_data_source
std::vector<std::string> g_clip_guest_mimes;          // its advertised mimes
uint32_t g_clip_sel_serial = 0;
std::vector<struct wl_resource*> g_clip_data_devices; // all bound wl_data_device
// Cached Android payloads (compositor thread): what data_offer_receive writes.
std::string g_clip_android_text;   // UTF-8 text (for text/plain* + UTF8_STRING)
std::string g_clip_android_html;   // UTF-8 text/html
std::string g_clip_android_png;    // raw image/png bytes
std::vector<std::string> g_clip_android_mimes;        // mimes we advertise to guests
// Server-owned offers we synthesized (so we can prune on destroy). Compositor thread.
std::vector<struct wl_resource*> g_clip_server_offers;
// Loop-guard: hash of the last bytes WE pushed to Android (guest->android), so an
// echoed Android push (android->guest of our own copy) is dropped (§7).
uint64_t g_clip_last_guest_hash = 0;

// Sinks installed by runtime_report.cpp (JNI up-calls). Guarded: the setter runs
// off-thread; reads copy under the lock then invoke outside it.
std::mutex g_clip_sink_mutex;
ClipboardGuestOfferCb g_clip_on_offer;
ClipboardGuestTextCb  g_clip_on_text;
ClipboardGuestImageCb g_clip_on_image;

// Inbound (Android->guest) queue, drained on the compositor thread (mirrors
// g_inject_queue / enqueue_inject).
struct AndroidSelection {
    std::vector<std::string> mimes;
    std::string text, html, png;
};
std::mutex g_clip_queue_mutex;
std::vector<AndroidSelection> g_clip_queue;  // guarded by g_clip_queue_mutex

// In-flight pipe reads (guest->android): one per outstanding wl_data_source.send.
// Registered with the wl_event_loop so the read never blocks the reactor (§3a).
struct ClipReadCtx {
    struct wl_event_source* src = nullptr;
    int fd = -1;
    std::string mime;
    std::string buf;
    bool is_image = false;
};
// In-flight pipe writes (android->guest): drains a cached payload into the
// guest's receive() fd without blocking (§3b).
struct ClipWriteCtx {
    struct wl_event_source* src = nullptr;
    int fd = -1;
    std::string data;   // OWN copy so a mid-paste clip change can't free it
    size_t off = 0;
};

constexpr size_t kClipMaxBytes = 8u * 1024u * 1024u;  // cap (image) — §3a/§6c

uint64_t clip_fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : (const std::string&)s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Pick the richest guest mime we can map to Android (§2 priority order).
const char* clip_pick_guest_mime(const std::vector<std::string>& mimes, bool* out_is_image) {
    auto has = [&](const char* m) -> const char* {
        for (const auto& s : mimes) if (s == m) return s.c_str();
        return nullptr;
    };
    *out_is_image = false;
    if (const char* m = has("image/png")) { *out_is_image = true; return m; }
    if (const char* m = has("text/html")) return m;
    if (const char* m = has("text/plain;charset=utf-8")) return m;
    if (const char* m = has("text/plain")) return m;
    if (const char* m = has("UTF8_STRING")) return m;
    return nullptr;
}

// ---- guest -> Android: pull bytes out of a guest source over a pipe (§3a) ----
// wl_event_loop callback: append readable bytes; on EOF deliver to Android.
int clip_on_pipe_readable(int fd, uint32_t mask, void* data) {
    auto* ctx = static_cast<ClipReadCtx*>(data);
    for (;;) {
        char tmp[4096];
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (ctx->buf.size() + static_cast<size_t>(n) > kClipMaxBytes) {
                ctx->buf.append(tmp, kClipMaxBytes - ctx->buf.size());
                ALR_WL_LOGW("clipboard guest read hit %zu cap, truncating", kClipMaxBytes);
                break;  // treat as done
            }
            ctx->buf.append(tmp, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Not done yet; wait for the next readable wake — unless the writer
            // hung up (HANGUP without readable data means EOF on some kernels).
            if (!(mask & WL_EVENT_HANGUP)) return 0;
        }
        break;  // n==0 (EOF), real error, or hangup-with-no-data => finished
    }
    // Finished: deliver to Android, then tear down this transfer.
    ClipboardGuestTextCb on_text;
    ClipboardGuestImageCb on_image;
    {
        std::lock_guard<std::mutex> lk(g_clip_sink_mutex);
        on_text = g_clip_on_text;
        on_image = g_clip_on_image;
    }
    g_clip_last_guest_hash = clip_fnv1a(ctx->buf);
    if (ctx->is_image) {
        if (on_image) on_image(ctx->buf);
    } else {
        if (on_text) on_text(ctx->mime, ctx->buf);
    }
    if (ctx->src) wl_event_source_remove(ctx->src);
    if (ctx->fd >= 0) ::close(ctx->fd);
    delete ctx;
    return 0;
}

// Compositor thread: ask the current guest source for its richest mime, fold the
// pipe read end into the reactor. Idempotent-safe: a guest with no source is a no-op.
void clip_pull_guest_bytes() {
    if (g_clip_owner != SelOwner::Guest || !g_clip_guest_source) return;
    Compositor* comp = instance();
    if (!comp) return;
    bool is_image = false;
    const char* mime = clip_pick_guest_mime(g_clip_guest_mimes, &is_image);
    if (!mime) return;
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        ALR_WL_LOGE("clipboard pipe2 failed: %s", std::strerror(errno));
        return;
    }
    // Guest receives the WRITE end; compositor keeps the READ end.
    wl_data_source_send_send(g_clip_guest_source, mime, fds[1]);
    ::close(fds[1]);
    struct wl_client* gc = wl_resource_get_client(g_clip_guest_source);
    if (gc) wl_client_flush(gc);  // push the .send event now so the guest writes
    auto* ctx = new ClipReadCtx();
    ctx->fd = fds[0];
    ctx->mime = mime;
    ctx->is_image = is_image;
    ctx->src = wl_event_loop_add_fd(comp->loop(), fds[0],
                                    WL_EVENT_READABLE, clip_on_pipe_readable, ctx);
    if (!ctx->src) {
        ::close(fds[0]);
        delete ctx;
        ALR_WL_LOGE("clipboard wl_event_loop_add_fd(read) failed");
    }
}

// Compositor thread: guest set/cleared its selection (data_device.set_selection).
void clip_on_guest_set_selection(struct wl_resource* source, uint32_t serial) {
    if (source == nullptr) {
        // Guest cleared. We do NOT clobber the Android clipboard on a guest clear
        // (§7): only drop our guest-owned state.
        if (g_clip_owner == SelOwner::Guest) {
            g_clip_owner = SelOwner::None;
            g_clip_guest_source = nullptr;
            g_clip_guest_mimes.clear();
        }
        return;
    }
    // Cancel a previous guest source so the old client drops its stale selection.
    if (g_clip_guest_source && g_clip_guest_source != source)
        wl_data_source_send_cancelled(g_clip_guest_source);
    g_clip_guest_source = source;
    g_clip_owner = SelOwner::Guest;
    g_clip_sel_serial = serial;
    g_clip_guest_mimes.clear();
    if (auto* st = static_cast<DataSourceState*>(wl_resource_get_user_data(source)))
        g_clip_guest_mimes = st->mimes;
    ALR_WL_LOGI("clipboard: guest selection (%zu mimes, serial=%u)",
                g_clip_guest_mimes.size(), serial);
    // Notify Android (optional sink) then pull eagerly so the bytes are ready.
    ClipboardGuestOfferCb on_offer;
    { std::lock_guard<std::mutex> lk(g_clip_sink_mutex); on_offer = g_clip_on_offer; }
    if (on_offer) on_offer(g_clip_guest_mimes);
    clip_pull_guest_bytes();
}

void clip_on_data_source_destroyed(struct wl_resource* source) {
    if (g_clip_guest_source == source) {  // never deref after destroy (UAF discipline)
        g_clip_guest_source = nullptr;
        g_clip_guest_mimes.clear();
        if (g_clip_owner == SelOwner::Guest) g_clip_owner = SelOwner::None;
    }
}

// ---- Android -> guest: synthesize a server-owned offer on a data_device ----
void clip_send_android_offer(struct wl_resource* dev) {
    if (g_clip_owner != SelOwner::Android || g_clip_android_mimes.empty()) return;
    struct wl_client* c = wl_resource_get_client(dev);
    struct wl_resource* offer = wl_resource_create(
        c, &wl_data_offer_interface, wl_resource_get_version(dev), 0 /*server id*/);
    if (!offer) return;
    wl_resource_set_implementation(offer, &kDataOfferImpl, nullptr, data_offer_destroyed);
    g_clip_server_offers.push_back(offer);
    wl_data_device_send_data_offer(dev, offer);
    for (const auto& m : g_clip_android_mimes)
        wl_data_offer_send_offer(offer, m.c_str());
    wl_data_device_send_selection(dev, offer);
    if (c) wl_client_flush(c);
}

void clip_on_data_device_created(struct wl_resource* dev) {
    g_clip_data_devices.push_back(dev);
    // A guest that connects after an Android copy still sees the current selection.
    if (g_clip_owner == SelOwner::Android) clip_send_android_offer(dev);
}
void clip_on_data_device_destroyed(struct wl_resource* dev) {
    drop_resource(g_clip_data_devices, dev);
}
void clip_data_offer_destroyed(struct wl_resource* offer) {
    drop_resource(g_clip_server_offers, offer);
}

// wl_event_loop callback: drain a cached Android payload into the guest's fd (§3b).
int clip_on_pipe_writable(int fd, uint32_t mask, void* data) {
    auto* ctx = static_cast<ClipWriteCtx*>(data);
    if (mask & WL_EVENT_HANGUP) { ctx->off = ctx->data.size(); }  // guest gave up
    while (ctx->off < ctx->data.size()) {
        ssize_t n = ::write(fd, ctx->data.data() + ctx->off, ctx->data.size() - ctx->off);
        if (n > 0) { ctx->off += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;  // wait
        break;  // error or 0 => stop
    }
    if (ctx->src) wl_event_source_remove(ctx->src);
    if (ctx->fd >= 0) ::close(ctx->fd);
    delete ctx;
    return 0;
}

// Compositor thread: a guest pasted (wl_data_offer.receive) against our server
// offer. Answer with the cached Android bytes for `mime`.
void clip_data_offer_receive(struct wl_resource* /*offer*/, const char* mime, int fd) {
    if (fd < 0) return;
    if (!mime || g_clip_owner != SelOwner::Android) { ::close(fd); return; }
    const std::string m(mime);
    const std::string* payload = nullptr;
    if (m == "text/html") payload = &g_clip_android_html;
    else if (m == "image/png") payload = &g_clip_android_png;
    else if (m == "text/plain;charset=utf-8" || m == "text/plain" || m == "UTF8_STRING")
        payload = &g_clip_android_text;
    if (!payload) { ::close(fd); return; }
    Compositor* comp = instance();
    // Make the fd non-blocking so a full pipe never wedges the reactor.
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    auto* ctx = new ClipWriteCtx();
    ctx->fd = fd;
    ctx->data = *payload;  // OWN copy (mid-paste clip change can't free it)
    // Try a fast direct write first (text usually fits one pipe buffer).
    while (ctx->off < ctx->data.size()) {
        ssize_t n = ::write(fd, ctx->data.data() + ctx->off, ctx->data.size() - ctx->off);
        if (n > 0) { ctx->off += static_cast<size_t>(n); continue; }
        break;
    }
    if (ctx->off >= ctx->data.size() || !comp) {
        ::close(fd);
        delete ctx;
        return;
    }
    // Didn't finish: fold the write end into the reactor and drain on WRITABLE.
    ctx->src = wl_event_loop_add_fd(comp->loop(), fd, WL_EVENT_WRITABLE,
                                    clip_on_pipe_writable, ctx);
    if (!ctx->src) { ::close(fd); delete ctx; }
}

// Compositor thread: apply a queued Android selection (broadcast to all guests).
void clip_apply_android_selection(const AndroidSelection& sel) {
    // Loop-guard (§7): if a guest currently owns the selection and this push is the
    // echo of the guest's OWN copy (same text hash), ignore it.
    if (g_clip_owner == SelOwner::Guest && !sel.text.empty() &&
        clip_fnv1a(sel.text) == g_clip_last_guest_hash) {
        return;
    }
    if (sel.mimes.empty()) {
        // Android cleared. Drop Android ownership; cancel nothing on the guest.
        if (g_clip_owner == SelOwner::Android) {
            g_clip_owner = SelOwner::None;
            g_clip_android_mimes.clear();
            g_clip_android_text.clear();
            g_clip_android_html.clear();
            g_clip_android_png.clear();
            for (auto* dev : g_clip_data_devices) {
                wl_data_device_send_selection(dev, nullptr);
                struct wl_client* c = wl_resource_get_client(dev);
                if (c) wl_client_flush(c);
            }
        }
        return;
    }
    // The user genuinely copied in an Android app: flip ownership to Android and
    // cancel any guest source so the guest drops its stale selection.
    if (g_clip_guest_source) {
        wl_data_source_send_cancelled(g_clip_guest_source);
        g_clip_guest_source = nullptr;
        g_clip_guest_mimes.clear();
    }
    g_clip_owner = SelOwner::Android;
    g_clip_android_mimes = sel.mimes;
    g_clip_android_text = sel.text;
    g_clip_android_html = sel.html;
    g_clip_android_png = sel.png;
    ALR_WL_LOGI("clipboard: android selection (%zu mimes, text=%zuB html=%zuB png=%zuB)",
                sel.mimes.size(), sel.text.size(), sel.html.size(), sel.png.size());
    for (auto* dev : g_clip_data_devices) clip_send_android_offer(dev);
}

}  // namespace

// ===========================================================================
// Compositor member functions
// ===========================================================================

void Compositor::bind_compositor(struct wl_client* client, void* /*data*/,
                                 uint32_t version, uint32_t id) {
    struct wl_resource* r =
        wl_resource_create(client, &wl_compositor_interface,
                           static_cast<int>(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kCompositorImpl, nullptr, nullptr);
    ALR_WL_LOGI("client bound: wl_compositor v%u", version);
}

void Compositor::bind_seat(struct wl_client* client, void* /*data*/,
                           uint32_t version, uint32_t id) {
    struct wl_resource* r = wl_resource_create(
        client, &wl_seat_interface, static_cast<int>(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kSeatImpl, nullptr, nullptr);
    // Advertise pointer + keyboard + touch so GTK/SDL set up input objects.
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER |
                                     WL_SEAT_CAPABILITY_KEYBOARD |
                                     WL_SEAT_CAPABILITY_TOUCH);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(r, "alr-seat");
    }
    ALR_WL_LOGI("client bound: wl_seat v%u", version);
}

void Compositor::bind_output(struct wl_client* client, void* data,
                             uint32_t version, uint32_t id) {
    auto* self = static_cast<Compositor*>(data);
    struct wl_resource* r = wl_resource_create(
        client, &wl_output_interface, static_cast<int>(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kOutputImpl, nullptr, nullptr);
    const int32_t w = self->config_.output_width > 0 ? self->config_.output_width : 1280;
    const int32_t h = self->config_.output_height > 0 ? self->config_.output_height : 720;
    // wl_output.geometry takes the PHYSICAL size in MILLIMETRES (not pixels) — a
    // toolkit divides px/mm to get monitor DPI. Derive mm from the device's real
    // dpi (xdpi/ydpi, else densityDpi, else ~160): mm = px / dpi * 25.4.
    const float xdpi = self->config_.xdpi > 1.0f ? self->config_.xdpi
        : (self->config_.density_dpi > 0 ? static_cast<float>(self->config_.density_dpi) : 160.0f);
    const float ydpi = self->config_.ydpi > 1.0f ? self->config_.ydpi
        : (self->config_.density_dpi > 0 ? static_cast<float>(self->config_.density_dpi) : 160.0f);
    const int32_t phys_w_mm = static_cast<int32_t>(static_cast<float>(w) / xdpi * 25.4f + 0.5f);
    const int32_t phys_h_mm = static_cast<int32_t>(static_cast<float>(h) / ydpi * 25.4f + 0.5f);
    const int32_t scale = self->config_.output_scale > 0 ? self->config_.output_scale : 1;
    wl_output_send_geometry(r, 0, 0, phys_w_mm, phys_h_mm, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "ALR", "android-surface", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, w, h,
                        self->config_.output_refresh_mhz);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(r, scale);
    }
    // wl_output v4 name/description (sent before done, per protocol event ordering).
    // chromium ozone/wayland's WaylandOutput records these; a stable machine-readable
    // name also lets a client correlate the output across reconnects. Older clients
    // (which negotiated <v4) never receive these, so this is additive only.
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(r, "ALR-0");
    }
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) {
        wl_output_send_description(r, "ALR Android SurfaceView output");
    }
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(r);
    }
    ALR_WL_LOGI("client bound: wl_output v%u (%dx%d px, %dx%d mm, scale=%d, dpi=%.0f)",
                version, w, h, phys_w_mm, phys_h_mm, scale, xdpi);
}

void Compositor::bind_xdg_wm_base(struct wl_client* client, void* /*data*/,
                                  uint32_t version, uint32_t id) {
    struct wl_resource* r = wl_resource_create(
        client, &xdg_wm_base_interface, static_cast<int>(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kWmBaseImpl, nullptr, nullptr);
    ALR_WL_LOGI("client bound: xdg_wm_base v%u", version);
}

void Compositor::bind_subcompositor(struct wl_client* client, void* /*data*/,
                                    uint32_t version, uint32_t id) {
    struct wl_resource* r = wl_resource_create(
        client, &wl_subcompositor_interface, static_cast<int>(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kSubcompositorImpl, nullptr, nullptr);
    ALR_WL_LOGI("client bound: wl_subcompositor v%u", version);
}

void Compositor::bind_data_device_manager(struct wl_client* client, void* /*data*/,
                                          uint32_t version, uint32_t id) {
    struct wl_resource* r = wl_resource_create(
        client, &wl_data_device_manager_interface, static_cast<int>(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kDataDeviceManagerImpl, nullptr, nullptr);
    ALR_WL_LOGI("client bound: wl_data_device_manager v%u", version);
}

// ---------------------------------------------------------------------------
// CR-4 (chromium --ozone-platform=wayland) global-requirement audit.
//
// chromium's WaylandConnection::Initialize() (ui/ozone/platform/wayland/host) has
// exactly ONE hard global requirement that aborts startup if missing: the xdg
// shell (stable xdg_wm_base). It also needs wl_compositor and a wl_shm/buffer
// factory to put pixels on screen, and binds wl_seat + wl_output for input/sizing.
// All of these are advertised below already, so chromium binds and starts; it does
// NOT print "wl_X not available" for the set we ship. (Confirmed against this
// project's Scout-3 recon in docs/research/chromium-native-plan.md: "Mandatory wl
// interfaces ... wl_compositor, wl_shm, xdg_wm_base (stable). Strongly-needed:
// wl_seat + wl_output.")
//
// Globals chromium *also* binds when advertised but treats as OPTIONAL (a missing
// one only warns / changes behaviour, never aborts) — and why we do NOT add them
// here:
//   * zxdg_decoration_manager_v1 — server-side decorations. Absent => chromium uses
//     its own client-side decorations (CSD), which is what we want anyway (we are a
//     borderless single-output SurfaceView compositor). No generated glue exists for
//     this protocol in third_party/wayland_generated/, and adding it would mean
//     touching the generator/CMake (not in this file's scope). NOT NEEDED for CR-4.
//   * wp_presentation (presentation-time) — optional frame-timing feedback. Absent =>
//     chromium falls back to its own swap pacing (we already pace at the panel rate
//     via the frame timer + wl_callback). No generated glue; NOT NEEDED for CR-4.
//   * xdg_output_manager_v1 — logical output geometry. Absent => chromium derives
//     geometry from wl_output mode/scale (which we now send at v4, incl. name). No
//     generated glue; NOT NEEDED for CR-4.
//   * zwp_linux_dmabuf_v1 — DELIBERATELY un-advertised (per the plan + the task
//     constraint): we are wl_shm-only, so chromium must fall back to shm buffers and
//     let our AHB presenter composite. Faking dmabuf without /dev/dri would break it.
//
// Already-present extras that chromium binds: wl_data_device_manager (clipboard/DnD —
// originally added for GDK; chromium binds it too) and wl_subcompositor (overlay/
// background subsurfaces). So the registry below is already a superset of chromium's
// required globals; CR-4's remaining work is purely the present path (shm -> SurfaceView),
// not registry completeness. The DEVICE-PENDING gate is: chromium binds + maps a
// toplevel the compositor presents.
// ---------------------------------------------------------------------------
bool Compositor::register_globals() {
    g_compositor_ = wl_global_create(display_, &wl_compositor_interface,
                                     kCompositorVersion, this, bind_compositor);
    g_seat_ = wl_global_create(display_, &wl_seat_interface, kSeatVersion, this,
                               bind_seat);
    g_output_ = wl_global_create(display_, &wl_output_interface, kOutputVersion,
                                 this, bind_output);
    g_xdg_wm_base_ = wl_global_create(display_, &xdg_wm_base_interface,
                                      kXdgWmBaseVersion, this, bind_xdg_wm_base);
    g_subcompositor_ = wl_global_create(display_, &wl_subcompositor_interface, 1,
                                        this, bind_subcompositor);
    // GDK 3.24 won't bind wl_seat until wl_data_device_manager is also present.
    g_data_device_manager_ = wl_global_create(
        display_, &wl_data_device_manager_interface, 3, this, bind_data_device_manager);
    (void)kShmVersion;  // wl_shm is created by wl_display_init_shm().
    if (!g_compositor_ || !g_seat_ || !g_output_ || !g_xdg_wm_base_ ||
        !g_subcompositor_ || !g_data_device_manager_) {
        ALR_WL_LOGE("wl_global_create failed for one or more globals");
        return false;
    }
    return true;
}

bool Compositor::make_socket() {
    // Derive XDG_RUNTIME_DIR (parent dir) + WAYLAND_DISPLAY (basename).
    const std::string& path = config_.socket_path;
    const auto slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".")
                                                         : path.substr(0, slash);
    const std::string name = (slash == std::string::npos) ? path
                                                          : path.substr(slash + 1);
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        ALR_WL_LOGW("mkdir %s failed: %s (continuing)", dir.c_str(), std::strerror(errno));
    }
    // libwayland's wl_display_add_socket builds the path from XDG_RUNTIME_DIR +
    // the supplied name. Set XDG_RUNTIME_DIR for *this* process so the socket
    // lands exactly at config_.socket_path; this is also the value the guest
    // env must use (see integration notes).
    ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    if (wl_display_add_socket(display_, name.c_str()) != 0) {
        ALR_WL_LOGE("wl_display_add_socket(%s) failed: %s", name.c_str(),
                    std::strerror(errno));
        return false;
    }
    ALR_WL_LOGI("wayland socket bound: %s/%s", dir.c_str(), name.c_str());
    return true;
}

bool Compositor::setup() {
    display_ = wl_display_create();
    if (!display_) {
        status_ = "ALR WAYLAND COMPOSITOR: FAIL wl_display_create";
        ALR_WL_LOGE("%s", status_.c_str());
        return false;
    }
    if (wl_display_init_shm(display_) != 0) {
        status_ = "ALR WAYLAND COMPOSITOR: FAIL wl_display_init_shm";
        ALR_WL_LOGE("%s", status_.c_str());
        return false;
    }
    if (!register_globals()) {
        status_ = "ALR WAYLAND COMPOSITOR: FAIL register_globals";
        return false;
    }
    if (!make_socket()) {
        status_ = "ALR WAYLAND COMPOSITOR: FAIL add_socket path=" + config_.socket_path;
        return false;
    }

    loop_ = wl_display_get_event_loop(display_);
    loop_fd_ = wl_event_loop_get_fd(loop_);

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wakeup_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (epoll_fd_ < 0 || wakeup_fd_ < 0 || loop_fd_ < 0) {
        status_ = "ALR WAYLAND COMPOSITOR: FAIL epoll/eventfd setup";
        ALR_WL_LOGE("%s", status_.c_str());
        return false;
    }
    // A repeating timer paces frame callbacks (see surface_commit) at the PANEL's
    // refresh rate (config_.output_refresh_mhz), so a 90Hz device drives 90 frame
    // callbacks/s and presents at 90Hz instead of a hardcoded 60.
    frame_timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (frame_timer_fd_ >= 0) {
        long refresh_mhz = config_.output_refresh_mhz > 0 ? config_.output_refresh_mhz : 60000;
        long period_ns = 1'000'000'000'000LL / refresh_mhz;  // mHz -> ns (90000 -> 11'111'111)
        if (period_ns < 1'000'000 || period_ns > 1'000'000'000LL) period_ns = 16'666'667;  // sane bounds -> 60Hz
        struct itimerspec its{};
        its.it_interval.tv_nsec = period_ns;
        its.it_value.tv_nsec = period_ns;
        ::timerfd_settime(frame_timer_fd_, 0, &its, nullptr);
    }

    // Fold the wl_event_loop fd + the wakeup eventfd (+ frame timer) into epoll.
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = loop_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, loop_fd_, &ev);
    ev.data.fd = wakeup_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
    if (frame_timer_fd_ >= 0) {
        ev.data.fd = frame_timer_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, frame_timer_fd_, &ev);
    }

    setup_ok_ = true;
    status_ = "ALR WAYLAND COMPOSITOR: started socket=" + config_.socket_path +
              " globals=wl_compositor,wl_shm,wl_seat,wl_output,xdg_wm_base,"
              "wl_subcompositor,wl_data_device_manager";
    ALR_WL_LOGI("%s", status_.c_str());
    return true;
}

void Compositor::drain_input_queue() {
    std::vector<InjectEvent> local;
    {
        std::lock_guard<std::mutex> lk(g_inject_mutex);
        local.swap(g_inject_queue);
    }
    if (local.empty()) return;
    // wl_pointer/wl_touch coordinates are surface-local LOGICAL units; Android
    // touches arrive in device px. With buffer scale S, logical = px / S. (S=1 on
    // most phones/tablets, so this is a no-op there but correct for HiDPI.)
    const double sc = config_.output_scale > 0 ? config_.output_scale : 1;
    // Pointer target is now resolved PER pointer event against the touch/cursor
    // COORDINATE (input_target_at): a popup grab still wins, but with no popup open the
    // event lands on whatever toplevel is actually drawn under the point (the main
    // window vs. a smaller centred dialog), using the same placement helper the
    // presenter draws with. Keyboard still goes to g_focus_surface (or a popup grab).
    ALR_WL_LOGI("drain_input n=%zu focus=%p target=%p pointers=%zu touches=%zu keyboards=%zu scale=%g",
                local.size(), static_cast<void*>(g_focus_surface),
                static_cast<void*>(g_input_target_surface),
                g_pointers.size(), g_touches.size(), g_keyboards.size(), sc);
    // Re-point the pointer at the surface under (x,y); leave the old hover target
    // (paired with a frame, like the unmap path) so GTK clears its prelight before the
    // new surface gets enter. Returns the resolved target's wl_surface (or null).
    auto retarget_pointer = [&](double x, double y) -> struct wl_resource* {
        SurfaceState* t = input_target_at(x, y);
        struct wl_resource* surf = t ? t->surface : nullptr;
        if (surf != g_input_target_surface) {
            if (g_pointer_entered && g_input_target_surface) {
                for (auto* p : g_pointers) {
                    wl_pointer_send_leave(p, wl_display_next_serial(display_),
                                          g_input_target_surface);
                    if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                        wl_pointer_send_frame(p);
                }
            }
            g_input_target_surface = surf;
            g_pointer_entered = false;  // force a fresh enter to the new target
        }
        return surf;
    };
    for (const InjectEvent& e : local) {
        switch (e.kind) {
        case InjectKind::PointerMotion: {
            SurfaceState* tgt = input_target_at(e.x, e.y);
            struct wl_resource* tgt_surf = retarget_pointer(e.x, e.y);
            if (!tgt_surf || !tgt) break;
            double mx = e.x, my = e.y;
            if (!map_input_to_surface(tgt, e.x, e.y, &mx, &my)) break;
            const wl_fixed_t fx = wl_fixed_from_double(mx);
            const wl_fixed_t fy = wl_fixed_from_double(my);
            for (auto* p : g_pointers) {
                if (!g_pointer_entered) {
                    wl_pointer_send_enter(p, wl_display_next_serial(display_),
                                          tgt_surf, fx, fy);
                }
                wl_pointer_send_motion(p, e.time_ms, fx, fy);
                if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(p);
            }
            g_pointer_entered = true;
            break;
        }
        case InjectKind::PointerButton: {
            // Button stays with the surface the pointer last entered (an implicit
            // pointer grab); we do NOT re-hit-test here, matching Wayland semantics.
            if (!g_input_target_surface || !g_pointer_entered) break;
            // Keyboard focus-follow: a PRESS activates the toplevel under the cursor.
            // The pointer target may be a popup (menu) — resolve its owning toplevel by
            // KEY so keyboard focus lands on the window (zorder_top convention), never
            // the popup. (focus_follow_to_toplevel is a no-op while a popup keyboard
            // grab is active, so menu navigation is preserved.)
            if (e.state) {
                SurfaceState* ts = surface_state_for_wl(g_input_target_surface);
                if (ts && ts->is_popup)
                    ts = popup_owning_toplevel(ts, nullptr, nullptr);
                focus_follow_to_toplevel(ts);
            }
            const uint32_t serial = wl_display_next_serial(display_);
            for (auto* p : g_pointers) {
                wl_pointer_send_button(p, serial, e.time_ms, e.button,
                    e.state ? WL_POINTER_BUTTON_STATE_PRESSED
                            : WL_POINTER_BUTTON_STATE_RELEASED);
                if (wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(p);
            }
            break;
        }
        case InjectKind::PointerAxis: {
            if (!g_input_target_surface || !g_pointer_entered) break;
            const uint32_t a = e.axis == 1 ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
                                           : WL_POINTER_AXIS_VERTICAL_SCROLL;
            for (auto* p : g_pointers) {
                const int ver = wl_resource_get_version(p);
                // wl_pointer v5+ groups a scroll as axis_source -> axis -> frame. GTK's
                // wayland backend keys its smooth-scroll handling off axis_source; an
                // axis with NO source (the prior behaviour) is treated ambiguously and a
                // web page (netsurf) often won't scroll. Send WHEEL (discrete mouse/
                // trackpad wheel) — the Android side maps a real scroll wheel / 2-finger
                // wheel gesture to this. axis_stop (value 0) lets a kinetic client end the
                // sequence cleanly.
                if (ver >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
                    wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
                if (e.axis_value != 0.0) {
                    wl_pointer_send_axis(p, e.time_ms, a,
                                         wl_fixed_from_double(e.axis_value));
                } else if (ver >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
                    wl_pointer_send_axis_stop(p, e.time_ms, a);
                }
                if (ver >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(p);
            }
            break;
        }
        case InjectKind::TouchDown: {
            // A touch sequence is grabbed by the surface named at down: hit-test the
            // DOWN coordinate so a tap lands on the window drawn under the finger, then
            // keep that surface for the whole sequence (motion/up below).
            SurfaceState* tgt = input_target_at(e.x, e.y);
            struct wl_resource* tgt_surf = tgt ? tgt->surface : nullptr;
            if (!tgt_surf) break;
            // A new first touch establishes the grab; additional fingers join the same
            // grabbed surface (multi-touch on one window). A down arriving on a
            // DIFFERENT surface while a grab is active is ignored for grab purposes
            // (single-surface grab) but still delivered to the grabbed surface so the
            // client's touch-id bookkeeping stays consistent.
            if (g_touch_active == 0) g_touch_target = tgt_surf;
            // Keyboard focus-follow on the first finger of a tap: activate the toplevel
            // under the finger (the same press-activates-window behaviour as a mouse
            // click). If the tap hit a popup, resolve its owning toplevel by KEY so
            // keyboard focus lands on the window, not the menu (no-op under a popup
            // keyboard grab, so menu navigation is preserved).
            if (g_touch_active == 0) {
                SurfaceState* act = (tgt && tgt->is_popup)
                    ? popup_owning_toplevel(tgt, nullptr, nullptr) : tgt;
                focus_follow_to_toplevel(act);
            }
            struct wl_resource* deliver = g_touch_target ? g_touch_target : tgt_surf;
            double mx = e.x, my = e.y;
            SurfaceState* dtgt = surface_state_for_wl(deliver);
            if (!map_input_to_surface(dtgt ? dtgt : tgt, e.x, e.y, &mx, &my)) break;
            const uint32_t serial = wl_display_next_serial(display_);
            for (auto* t : g_touches)
                wl_touch_send_down(t, serial, e.time_ms, deliver, e.touch_id,
                                   wl_fixed_from_double(mx), wl_fixed_from_double(my));
            ++g_touch_active;
            break;
        }
        case InjectKind::TouchMotion: {
            // Stay with the grabbed surface; map the coordinate into ITS rect so a
            // drag that leaves the window still reports sensible (possibly negative)
            // surface-local coords to the owning client.
            SurfaceState* dtgt = surface_state_for_wl(g_touch_target);
            double mx = e.x, my = e.y;
            map_input_to_surface(dtgt, e.x, e.y, &mx, &my);
            for (auto* t : g_touches)
                wl_touch_send_motion(t, e.time_ms, e.touch_id,
                                     wl_fixed_from_double(mx), wl_fixed_from_double(my));
            break;
        }
        case InjectKind::TouchUp: {
            const uint32_t serial = wl_display_next_serial(display_);
            for (auto* t : g_touches) wl_touch_send_up(t, serial, e.time_ms, e.touch_id);
            if (g_touch_active > 0 && --g_touch_active == 0)
                g_touch_target = nullptr;  // last finger up: release the grab
            break;
        }
        case InjectKind::TouchFrame:
            for (auto* t : g_touches) wl_touch_send_frame(t);
            break;
        case InjectKind::Key: {
            // P0-3: route keys to a grabbing popup (menu) if any, else the focused
            // toplevel. No grab => g_keyboard_grab_surface null => unchanged behaviour.
            struct wl_resource* tgt_kbd =
                g_keyboard_grab_surface ? g_keyboard_grab_surface : g_focus_surface;
            if (!tgt_kbd) break;
            // M4: update real-modifier state so the client's xkb_state matches and
            // Shift/Ctrl/Alt apply to the keys that follow.
            const uint32_t mbit = evdev_to_mod_bit(e.button);
            bool mods_changed = false;
            if (mbit) {
                const uint32_t before = g_mods_depressed;
                if (e.state) g_mods_depressed |= mbit; else g_mods_depressed &= ~mbit;
                mods_changed = (g_mods_depressed != before);
            }
            if (e.button == 58 /*KEY_CAPSLOCK*/ && e.state) {
                g_mods_locked ^= (1u << 1);  // Lock
                mods_changed = true;
            }
            for (auto* k : g_keyboards) {
                if (!g_keyboard_entered) {
                    struct wl_array keys;
                    wl_array_init(&keys);
                    wl_keyboard_send_enter(k, wl_display_next_serial(display_),
                                           tgt_kbd, &keys);
                    wl_array_release(&keys);
                    // Initial modifier state for the newly-focused surface.
                    wl_keyboard_send_modifiers(k, wl_display_next_serial(display_),
                                               g_mods_depressed, 0, g_mods_locked, 0);
                }
                // e.button carries the evdev keycode directly.
                wl_keyboard_send_key(k, wl_display_next_serial(display_), e.time_ms,
                    e.button, e.state ? WL_KEYBOARD_KEY_STATE_PRESSED
                                      : WL_KEYBOARD_KEY_STATE_RELEASED);
                if (mods_changed) {
                    wl_keyboard_send_modifiers(k, wl_display_next_serial(display_),
                                               g_mods_depressed, 0, g_mods_locked, 0);
                }
            }
            g_keyboard_entered = true;
            break;
        }
        }
    }
    wl_display_flush_clients(display_);
}

// §5-C: bind the newest submitted GPU frame (AHardwareBuffer) to the top toplevel
// for zero-copy present. Compositor thread only (reactor wakeup). Single-GPU-surface
// bring-up: newest frame wins; superseded/older frames in the batch are released.
void Compositor::drain_gpu_queue() {
    std::vector<GpuSubmit> local;
    {
        std::lock_guard<std::mutex> lk(g_gpu_mutex);
        local.swap(g_gpu_queue);
    }
    if (local.empty()) return;
    for (std::size_t i = 0; i + 1 < local.size(); ++i)
        if (local[i].ahb)
            AHardwareBuffer_release(static_cast<AHardwareBuffer*>(local[i].ahb));
    GpuSubmit& last = local.back();
    // Single-GPU-surface contract (alr_present_source.hpp): keep the GPU stream bound
    // to the SAME surface across frames. If a surface already holds a GPU AHB (the GPU
    // app's window), prefer it — otherwise a later shm toplevel (e.g. a dialog) that
    // becomes zorder_top() would steal the GPU frames, leaving the GPU window stale and
    // painting the GPU content onto the wrong window. Fall back to the top toplevel only
    // when no GPU surface exists yet (the first frame / bring-up).
    SurfaceState* tgt = nullptr;
    for (SurfaceState* s : g_all_surfaces) {
        if (s && s->mapped && s->gpu_ahb != nullptr) { tgt = s; break; }
    }
    if (!tgt) tgt = zorder_top();
    if (!tgt) {
        // No Wayland toplevel to bind to (a headless / fullscreen GPU app, e.g.
        // glmark2): keep the newest AHB for a fullscreen present instead of dropping.
        if (g_fullscreen_gpu_ahb && g_fullscreen_gpu_ahb != last.ahb)
            AHardwareBuffer_release(static_cast<AHardwareBuffer*>(g_fullscreen_gpu_ahb));
        g_fullscreen_gpu_ahb = last.ahb;
        g_fullscreen_gpu_w = last.w;
        g_fullscreen_gpu_h = last.h;
        g_fullscreen_gpu_serial = last.serial;
        g_scene_dirty = true;
        return;
    }
    // A real toplevel exists now -> drop any stale fullscreen GPU buffer.
    if (g_fullscreen_gpu_ahb) {
        AHardwareBuffer_release(static_cast<AHardwareBuffer*>(g_fullscreen_gpu_ahb));
        g_fullscreen_gpu_ahb = nullptr;
    }
    // Contract (alr_present_source.hpp): release the previously-held buffer when the
    // next one binds, so at most one AHB is retained per GPU surface.
    if (tgt->gpu_ahb && tgt->gpu_ahb != last.ahb)
        AHardwareBuffer_release(static_cast<AHardwareBuffer*>(tgt->gpu_ahb));
    tgt->gpu_ahb = last.ahb;
    tgt->gpu_w = last.w;
    tgt->gpu_h = last.h;
    tgt->gpu_serial = last.serial;
    // Drive sizing/placement for a GPU-only surface (no shm buffer was committed).
    if (tgt->buf_w <= 0) tgt->buf_w = last.w;
    if (tgt->buf_h <= 0) tgt->buf_h = last.h;
    tgt->content_serial = static_cast<uint64_t>(wl_display_next_serial(display_));
    g_scene_dirty = true;  // present on the next frame-timer tick
}

// Apply any Android->guest clipboard selections pushed from Kotlin/JNI. Runs on
// the compositor thread, where all wl_data_* sends are legal (mirrors
// drain_input_queue / drain_gpu_queue). See clip_apply_android_selection.
void Compositor::drain_clipboard_queue() {
    std::vector<AndroidSelection> local;
    {
        std::lock_guard<std::mutex> lk(g_clip_queue_mutex);
        local.swap(g_clip_queue);
    }
    for (const AndroidSelection& sel : local) clip_apply_android_selection(sel);
}

void Compositor::reactor() {
    ALR_WL_LOGI("compositor reactor entering epoll loop (loop_fd=%d)", loop_fd_);
    constexpr int kMaxEvents = 8;
    struct epoll_event events[kMaxEvents];
    while (!stopping_.load()) {
        // Flush any queued protocol output before blocking.
        wl_display_flush_clients(display_);
        const int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, /*timeout=*/-1);
        if (n < 0) {
            if (errno == EINTR) continue;
            ALR_WL_LOGE("epoll_wait failed: %s", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == wakeup_fd_) {
                uint64_t v = 0;
                ssize_t r = ::read(wakeup_fd_, &v, sizeof(v));
                (void)r;  // drain; loop condition handles stop
                drain_input_queue();  // deliver any injected input events
                drain_gpu_queue();    // bind any submitted §5-C GPU (AHB) frames
                drain_clipboard_queue();  // apply any Android->guest selection pushes
            } else if (events[i].data.fd == frame_timer_fd_) {
                uint64_t v = 0;
                ssize_t r = ::read(frame_timer_fd_, &v, sizeof(v));
                (void)r;
                // Fire all paced frame callbacks. Pop-then-send so the destroy
                // listener (which mutates g_pending_frames) never invalidates us.
                const uint32_t t = now_ms();
                while (!g_pending_frames.empty()) {
                    struct wl_resource* cb = g_pending_frames.back();
                    g_pending_frames.pop_back();
                    wl_callback_send_done(cb, t);
                    wl_resource_destroy(cb);
                }
                // Coalesced presentation: if any commit since the last tick changed
                // the scene, present exactly once now. This caps eglSwapBuffers at the
                // timer rate (~60 Hz) no matter how many times clients committed in
                // between, fixing the BLASTBufferQueue "Already acquired max frames"
                // overrun. teardown() closes frame_timer_fd_ before any post-stop tick,
                // and the stopping_ guard is belt-and-suspenders.
                if (g_scene_dirty && !stopping_.load()) {
                    g_scene_dirty = false;
                    present_composited();
                }
            } else if (events[i].data.fd == loop_fd_) {
                // Dispatch all ready Wayland events (non-blocking; we already
                // know the fd is readable).
                wl_event_loop_dispatch(loop_, 0);
            }
        }
    }
    wl_display_flush_clients(display_);
    ALR_WL_LOGI("compositor reactor exiting");
}

void Compositor::teardown() {
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
    if (wakeup_fd_ >= 0) { ::close(wakeup_fd_); wakeup_fd_ = -1; }
    if (frame_timer_fd_ >= 0) { ::close(frame_timer_fd_); frame_timer_fd_ = -1; }
    g_pending_frames.clear();
    g_scene_dirty = false;        // reset so a re-created compositor starts clean
    g_first_present_done = false;
    g_zorder.clear();
    g_all_surfaces.clear();
    g_focus_surface = nullptr;
    g_input_target_surface = nullptr;
    g_keyboard_grab_surface = nullptr;  // stale-pointer guard on restart (UAF discipline)
    g_touch_target = nullptr;           // drop any in-flight touch grab on restart
    g_touch_active = 0;
    g_pointer_entered = false;
    g_keyboard_entered = false;
    g_mods_depressed = 0;  // don't leak held mods / CapsLock into a re-created compositor
    g_mods_locked = 0;
    // Clipboard selection model: drop all resource pointers (the wl_display teardown
    // below frees the resources themselves) so a re-created compositor starts clean.
    // UAF discipline: never deref these stale pointers after the display is gone.
    g_clip_owner = SelOwner::None;
    g_clip_guest_source = nullptr;
    g_clip_guest_mimes.clear();
    g_clip_data_devices.clear();
    g_clip_server_offers.clear();
    g_clip_android_mimes.clear();
    g_clip_android_text.clear();
    g_clip_android_html.clear();
    g_clip_android_png.clear();
    g_clip_last_guest_hash = 0;
    { std::lock_guard<std::mutex> lk(g_clip_queue_mutex); g_clip_queue.clear(); }
    // §5-C fullscreen GPU fallback: release the retained AHB ref (one acquire held in
    // drain_gpu_queue) and null the globals. Without this a stop while a headless GPU
    // app (glmark2) is presenting leaks the buffer, and a re-created compositor would
    // hand the now-dangling AHB to the presenter for a zero-copy import -> crash.
    if (g_fullscreen_gpu_ahb) {
        AHardwareBuffer_release(static_cast<AHardwareBuffer*>(g_fullscreen_gpu_ahb));
        g_fullscreen_gpu_ahb = nullptr;
    }
    g_fullscreen_gpu_w = 0;
    g_fullscreen_gpu_h = 0;
    g_fullscreen_gpu_serial = 0;
    if (g_compositor_) wl_global_destroy(g_compositor_);
    if (g_seat_) wl_global_destroy(g_seat_);
    if (g_output_) wl_global_destroy(g_output_);
    if (g_xdg_wm_base_) wl_global_destroy(g_xdg_wm_base_);
    if (g_subcompositor_) wl_global_destroy(g_subcompositor_);
    if (display_) {
        wl_display_destroy_clients(display_);
        wl_display_destroy(display_);
        display_ = nullptr;
    }
}

// ===========================================================================
// Public API
// ===========================================================================
std::string alr_start_wayland_compositor(const std::string& socket_path) {
    CompositorConfig cfg;
    cfg.socket_path = socket_path;
    return alr_start_wayland_compositor(cfg);
}

std::string alr_start_wayland_compositor(const CompositorConfig& config) {
    std::lock_guard<std::mutex> lk(g_instance_mutex);
    if (g_instance != nullptr) {
        return "ALR WAYLAND COMPOSITOR: already running socket=" +
               g_instance->config().socket_path;
    }
    auto* comp = new Compositor(config);
    g_instance = comp;
    g_thread = std::thread([comp]() { comp->run(); });

    const bool ok = comp->wait_ready_ok();
    if (!ok) {
        // setup failed; thread is winding down. Join and clean up.
        if (g_thread.joinable()) g_thread.join();
        std::string status = comp->status();
        delete comp;
        g_instance = nullptr;
        return status.empty() ? "ALR WAYLAND COMPOSITOR: FAIL (setup)" : status;
    }
    return comp->status();
}

std::string alr_stop_wayland_compositor() {
    std::lock_guard<std::mutex> lk(g_instance_mutex);
    if (g_instance == nullptr) {
        return "ALR WAYLAND COMPOSITOR: not running";
    }
    g_instance->request_stop();
    if (g_thread.joinable()) g_thread.join();
    delete g_instance;
    g_instance = nullptr;
    return "ALR WAYLAND COMPOSITOR: stopped";
}

bool alr_wayland_compositor_running() {
    std::lock_guard<std::mutex> lk(g_instance_mutex);
    return g_instance != nullptr;
}

// ---- input injection (enqueue + wake the compositor reactor) ----
namespace {
void enqueue_inject(const InjectEvent& e) {
    {
        std::lock_guard<std::mutex> lk(g_inject_mutex);
        g_inject_queue.push_back(e);
    }
    Compositor* c = instance();
    if (c) c->wake();
}
}  // namespace

void alr_wayland_inject_pointer_motion(double x, double y) {
    InjectEvent e{};
    e.kind = InjectKind::PointerMotion;
    e.x = x; e.y = y; e.time_ms = now_ms();
    enqueue_inject(e);
}
void alr_wayland_inject_pointer_button(uint32_t evdev_button, uint32_t pressed) {
    InjectEvent e{};
    e.kind = InjectKind::PointerButton;
    e.button = evdev_button; e.state = pressed; e.time_ms = now_ms();
    enqueue_inject(e);
}
void alr_wayland_inject_pointer_axis(double value, int32_t axis) {
    InjectEvent e{};
    e.kind = InjectKind::PointerAxis;
    e.axis = axis; e.axis_value = value; e.time_ms = now_ms();
    enqueue_inject(e);
}
void alr_wayland_inject_touch(int32_t id, double x, double y, int32_t phase) {
    InjectEvent e{};
    e.kind = phase == 0 ? InjectKind::TouchDown
           : phase == 2 ? InjectKind::TouchUp
                        : InjectKind::TouchMotion;
    e.touch_id = id; e.x = x; e.y = y; e.time_ms = now_ms();
    enqueue_inject(e);
    InjectEvent f{};
    f.kind = InjectKind::TouchFrame; f.time_ms = e.time_ms;
    enqueue_inject(f);
}
void alr_wayland_inject_key(uint32_t evdev_key, uint32_t pressed) {
    InjectEvent e{};
    e.kind = InjectKind::Key;
    e.button = evdev_key; e.state = pressed; e.time_ms = now_ms();
    enqueue_inject(e);
}
int alr_wayland_inject_selftest(double x, double y) {
    // A synthetic burst exercising pointer + touch + keyboard, as a real tap at
    // (x,y) plus a key press, so an input client observes events end to end.
    constexpr uint32_t kBtnLeft = 0x110;  // BTN_LEFT
    constexpr uint32_t kKeyA = 30;        // evdev KEY_A
    alr_wayland_inject_pointer_motion(x, y);
    alr_wayland_inject_pointer_button(kBtnLeft, 1);
    alr_wayland_inject_pointer_button(kBtnLeft, 0);
    alr_wayland_inject_touch(0, x, y, 0);
    alr_wayland_inject_touch(0, x, y, 2);
    alr_wayland_inject_key(kKeyA, 1);
    alr_wayland_inject_key(kKeyA, 0);
    return 7;
}

// ---------------------------------------------------------------------------
// Clipboard bridge public API (see alr_compositor.hpp + the design). The sink
// registry is set once by runtime_report.cpp; the selection setter enqueues onto
// the clipboard queue + wakes the reactor (mirroring enqueue_inject). All wl_*
// work happens later on the compositor thread in drain_clipboard_queue.
// ---------------------------------------------------------------------------
void alr_wayland_set_clipboard_sink(ClipboardGuestOfferCb on_offer,
                                    ClipboardGuestTextCb on_text,
                                    ClipboardGuestImageCb on_image) {
    std::lock_guard<std::mutex> lk(g_clip_sink_mutex);
    g_clip_on_offer = std::move(on_offer);
    g_clip_on_text = std::move(on_text);
    g_clip_on_image = std::move(on_image);
}

void alr_wayland_set_android_selection(const std::vector<std::string>& mimes,
                                       const std::string& text,
                                       const std::string& html,
                                       const std::string& png) {
    AndroidSelection sel;
    sel.mimes = mimes;
    sel.text = text;
    sel.html = html;
    sel.png = png;
    {
        std::lock_guard<std::mutex> lk(g_clip_queue_mutex);
        g_clip_queue.push_back(std::move(sel));
    }
    Compositor* c = instance();
    if (c) c->wake();
}

// ---------------------------------------------------------------------------
// §5-C PresentSource — GPU/AHB present entry (see alr_present_source.hpp).
// M2 Phase A (compositor half): acquire the AHB + queue it; drain_gpu_queue (on the
// compositor thread) binds it to the top toplevel and marks the scene dirty, so
// present_composited emits it as a §5-C PresentSurface (ps.ahb). The presenter's
// zero-copy EGLImage import (A4, runtime_report.cpp WaylandPresenter) lights it on
// screen; until that lands, alr_wayland_gpu_present_ready() stays false so WS-2
// keeps its direct present. Callable from the WS-2 executor thread.
// ---------------------------------------------------------------------------
void alr_wayland_submit_gpu_frame(void* ahardware_buffer, int32_t width,
                                  int32_t height, uint64_t serial) {
    if (!ahardware_buffer || width <= 0 || height <= 0) return;
    // Hold a ref across the thread handoff; drain_gpu_queue releases it when the
    // next frame binds (or drops it if there is no surface yet).
    AHardwareBuffer_acquire(static_cast<AHardwareBuffer*>(ahardware_buffer));
    {
        std::lock_guard<std::mutex> lk(g_gpu_mutex);
        g_gpu_queue.push_back({ahardware_buffer, width, height, serial});
    }
    Compositor* c = instance();
    if (c) c->wake();
}

bool alr_wayland_gpu_present_ready() {
    // A4 landed: the presenter imports the submitted AHB zero-copy (external-OES,
    // .rgba, V-flipped) and composites it through present_list, so WS-2 may drive the
    // GPU present path. (Per-device EGL/GL extension availability is still checked
    // presenter-side; a device lacking them silently keeps the CPU paths.)
    return true;
}

// ===========================================================================
// *** EGL WIRING TODO (lead) ***
// ---------------------------------------------------------------------------
// present_buffer() is the single integration seam. Install a PresentCallback in
// CompositorConfig::present that does, on the compositor thread:
//
//   1. (once) Bring up EGL on the SurfaceView's ANativeWindow by REUSING the
//      setup block of render_to_android_surface_frames() in runtime_report.cpp
//      (ANativeWindow_fromSurface, eglChooseConfig with
//      EGL_WINDOW_BIT|EGL_OPENGL_ES2_BIT, eglCreateWindowSurface, eglMakeCurrent).
//      Do NOT re-implement EGL here; share that code or factor it into a helper
//      the lead controls. Compile a trivial textured-quad program once.
//
//   2. (per frame) Upload PresentFrame::pixels:
//        - wl_shm ARGB8888/XRGB8888 is little-endian B,G,R,A in memory.
//        - Upload as GL_RGBA + swizzle .bgra in the fragment shader, OR use
//          GL_EXT_texture_format_BGRA8888 if advertised by glGetString(GL_EXTENSIONS).
//        - Respect PresentFrame::stride (set GL_UNPACK_ROW_LENGTH if stride !=
//          width*4, available in GLES3; for GLES2 upload row-by-row or require
//          stride==width*4).
//        - V-FLIP: wl_shm origin is top-left, GL texcoord origin is bottom-left.
//          Flip V in the quad UVs.
//        glBindTexture + glTexImage2D(GL_RGBA, w, h, ... pixels), draw quad,
//        eglSwapBuffers(display, window_surface).
//
//   3. Frame pacing: ideally gate eglSwapBuffers + the wl_callback.send_done in
//      surface_commit() on the next AChoreographer vsync rather than acking
//      immediately (see surface_commit()'s frame-callback block). For first
//      light the immediate ack is fine.
// ===========================================================================

}  // namespace alr::wayland
