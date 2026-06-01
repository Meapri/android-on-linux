# ALR Wayland compositor — integration notes for the lead

The compositor compiles and links into `alr_loader` today (static lib
`libalr_wayland.a`, gated to `arm64-v8a`). Nothing references its symbols yet, so
the linker prunes them from the final `.so`. The four steps below wire it in.

Public API (see `alr_compositor.hpp`):

```cpp
namespace alr::wayland {
  std::string alr_start_wayland_compositor(const std::string& socket_path);
  std::string alr_start_wayland_compositor(const CompositorConfig& config);
  std::string alr_stop_wayland_compositor();
  bool        alr_wayland_compositor_running();
}
```

`alr_start_wayland_compositor` spawns the compositor thread, runs the
`wl_display` + epoll reactor, and returns a status string (e.g.
`"ALR WAYLAND COMPOSITOR: started socket=/data/.../alr-xdg/wayland-0 globals=..."`).

---

## 1. JNI export (add to `runtime_report.cpp` — the lead owns that file)

Mirror the existing `nativeRenderVulkanSurfaceFrames` JNI signature shape. Add
near the other `extern "C" JNIEXPORT` blocks:

```cpp
#include "alr_wayland/alr_compositor.hpp"   // at top with the other includes

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandCompositorStart(
    JNIEnv* env, jobject /*thiz*/, jobject surface, jstring cache_dir) {
    const std::string cache = jstring_to_string(env, cache_dir);
    alr::wayland::CompositorConfig cfg;
    cfg.socket_path = cache + "/alr-xdg/wayland-0";

    // Output size from the SurfaceView's ANativeWindow (optional but recommended)
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    if (win) {
        cfg.output_width  = ANativeWindow_getWidth(win);
        cfg.output_height = ANativeWindow_getHeight(win);
        // NOTE: keep a reference to `win` for the EGL present hook (step 4);
        // do NOT release it here if you install present (it needs the window).
    }

    // TODO(step 4): cfg.present = <lambda that uploads PresentFrame to EGL>;
    const std::string status = alr::wayland::alr_start_wayland_compositor(cfg);
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandCompositorStop(
    JNIEnv* env, jobject /*thiz*/) {
    return env->NewStringUTF(alr::wayland::alr_stop_wayland_compositor().c_str());
}
```

CMake already links `alr_wayland` into `alr_loader` and defines
`ALR_HAVE_WAYLAND=1`, so no CMake change is needed. (The include path
`${CMAKE_CURRENT_SOURCE_DIR}` is already on `alr_loader`, and `alr_wayland` is
linked `PUBLIC` to its headers.)

---

## 2. MainActivity call (the lead owns `MainActivity.kt`)

```kotlin
external fun nativeWaylandCompositorStart(surface: Surface, cacheDir: String): String
external fun nativeWaylandCompositorStop(): String

// e.g. from SurfaceHolder.Callback.surfaceCreated(holder):
val status = nativeWaylandCompositorStart(holder.surface, cacheDir.absolutePath)
Log.i("ALR", status)   // "ALR WAYLAND COMPOSITOR: started socket=..."
```

`cacheDir` is `Context.getCacheDir()`. The compositor creates
`<cacheDir>/alr-xdg/` (0700) and binds `wayland-0` inside it.

---

## 3. Guest environment (so an UNMODIFIED client connects)

The socket path **is** the host path (the guest's `connect()` is not
path-trapped by the seccomp filter — verified in the Phase-3 design). Export to
the guest:

| var | value |
|---|---|
| `XDG_RUNTIME_DIR` | `<cacheDir>/alr-xdg` (the socket's parent dir) |
| `WAYLAND_DISPLAY` | `wayland-0` |
| `GDK_BACKEND` | `wayland` (force GTK off X11) |
| `SDL_VIDEODRIVER` | `wayland` |
| `GSK_RENDERER` | `cairo` (GTK4; avoids a GL renderer we can't back) |
| `XDG_SESSION_TYPE` | `wayland` |

Add these in `build_guest_environment` (`alr_runtime/alr_env.cpp:16`). Extend
`GuestEnvironmentInput` (`alr_env.hpp:9`) with a `wayland_runtime_dir` field and
emit:

```cpp
// in build_guest_environment(), append to .values:
{"XDG_RUNTIME_DIR", input.wayland_runtime_dir},   // = <cacheDir>/alr-xdg
{"WAYLAND_DISPLAY", "wayland-0"},
{"GDK_BACKEND", "wayland"},
{"SDL_VIDEODRIVER", "wayland"},
{"GSK_RENDERER", "cairo"},
{"XDG_SESSION_TYPE", "wayland"},
```

### IMPORTANT — env must actually reach the guest

The in-process native-loader path (`build_native_loader_probe`) hands control
straight to `ld-linux-aarch64.so.1` via the execmem mapping and **does not build
a guest `envp`/auxv block today** (the `build_env_strings`/`envp` machinery at
`alr_launch.cpp:182` belongs to the separate `execve`-based backend in
`alr_exec.cpp`). So one of these is required for the guest to *see*
`WAYLAND_DISPLAY`:

* **(a)** route the Wayland guest through the `execve` backend (which already
  builds `envp` from `build_guest_environment`), **or**
* **(b)** construct the env block on the guest stack in the forked child before
  the ld.so handoff in the loader path.

Until that is done, libwayland-client has no `WAYLAND_DISPLAY` and won't connect.
This is the one true integration blocker on the guest side; the compositor
itself is ready.

---

## 4. EGL present hook (the only rendering seam)

`CompositorConfig::present` is a `std::function<void(const PresentFrame&)>` called
on the compositor thread on every committed `wl_shm` buffer. Install a lambda
that reuses the **proven** EGL+ANativeWindow setup from
`render_to_android_surface_frames()` in `runtime_report.cpp` (do NOT duplicate it
— factor that EGL-on-ANativeWindow setup into a shared helper you own, or call
into it). Per frame:

```cpp
// PresentFrame: width, height, stride (bytes/row), shm_format (0=ARGB8888,1=XRGB8888),
//               pixels (valid only during the call), serial.
// ARGB8888 is little-endian B,G,R,A in memory.
//   - upload as GL_RGBA and swizzle .bgra in the fragment shader, OR use
//     GL_EXT_texture_format_BGRA8888 if glGetString(GL_EXTENSIONS) advertises it.
//   - V-FLIP: wl_shm origin is top-left, GL texcoord origin bottom-left -> flip V.
//   - respect stride: set GL_UNPACK_ROW_LENGTH (GLES3) if stride != width*4,
//     else upload row-by-row (GLES2) or require stride==width*4.
glBindTexture(GL_TEXTURE_2D, tex);
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
// draw a textured quad, then:
eglSwapBuffers(display, window_surface);
```

A `present_buffer()` TODO block with the full checklist is at the bottom of
`alr_compositor.cpp`. The compositor already releases the `wl_buffer`
(`wl_buffer.release`) right after `present()` returns, so the lambda must finish
its texture upload synchronously (it copies into the GL texture, so this is fine).

Frame pacing: for first light the compositor acks `wl_callback.done` immediately
after commit (simple clients keep drawing). To remove tearing, gate the swap +
the `wl_callback.send_done` on the next `AChoreographer` vsync — the hook point is
the frame-callback block in `surface_commit()`.

---

## 5. Smallest guest `wl_client` smoke test

Confirm the wire end-to-end with a ~40-line C client linking the rootfs's
`libwayland-client.so.0` + `libffi.so.8`, using the checked-in
`wayland_generated/wayland-client-protocol.h` +
`wayland_generated/xdg-shell-client-protocol.h` (+ `xdg-shell-protocol.c`):

```c
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
// 1. wl_display_connect(NULL)           // uses WAYLAND_DISPLAY + XDG_RUNTIME_DIR
// 2. wl_display_get_registry + roundtrip; bind wl_compositor, wl_shm, xdg_wm_base
// 3. wl_compositor_create_surface
// 4. xdg_wm_base_get_xdg_surface + xdg_surface_get_toplevel
// 5. wl_surface_commit (no buffer) -> wait for xdg_surface.configure -> ack_configure
// 6. memfd_create pool, fill solid color, wl_shm_create_pool/create_buffer
// 7. wl_surface_attach + damage + commit
```

Expected logcat from the compositor (tag `alr_wayland`):
```
client bound: wl_compositor v4
client bound: xdg_wm_base v2
xdg_wm_base.get_xdg_surface id=...
client mapped xdg_toplevel id=...
sent initial xdg configure w=... h=... serial=...
xdg_surface.ack_configure serial=...
surface committed shm: <w>x<h> stride=... fmt=0
present_buffer (no EGL hook installed): ...    <- until step 4 is wired
```

Or just run unmodified `weston-simple-shm` once the rootfs has
`libwayland-client.so.0` — same observable result, proving it's real Wayland and
not a custom stream.
```
