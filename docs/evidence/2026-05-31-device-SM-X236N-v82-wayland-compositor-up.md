# Device Evidence — SM-X236N, v82: FIRST Linux GUI window on Android via the ALR Wayland compositor

Phase 3 (the critical-path display server) reaches its first exit criterion: a **real `libwayland-server` 1.25.0 compositor** runs inside the Android app, and an **unmodified Wayland client — running as an in-process ALR guest (dynamic glibc via the in-process ld.so) — connects, commits a `wl_shm` buffer, and that buffer is composited onto the app's SurfaceView via the proven EGL/GLES path.** A solid-color Linux window appears on the Android screen. Non-root, no X-server hack, no VNC, public graphics APIs only.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.82-android-wayland-compositor-v82`, SHA-256 `af5a3fbc90c997360e48fad8ea08ea52913b12b1e51f50b27e2c7a6c832c6899`. pytest 235 passed, native-core PASS, validate-host PASS, BUILD SUCCESSFUL.

## Result

```
ALR WAYLAND COMPOSITOR UP (libwayland-server on SurfaceView):            PASS
ALR WAYLAND FIRST WINDOW (guest wl_shm client → compositor → SurfaceView):  PASS
   compositor STATUS: running   present=presented-frame   frames=1

compositor logcat (tag alr_wayland):
  wayland socket bound: /data/user/0/…/cache/alr-xdg/wayland-0
  ALR WAYLAND COMPOSITOR: started globals=wl_compositor,wl_shm,wl_seat,wl_output,xdg_wm_base
  compositor reactor entering epoll loop
  client bound: wl_compositor v4
  client bound: xdg_wm_base v2
  wl_compositor.create_surface id=3
  xdg_wm_base.get_xdg_surface id=7
  client mapped xdg_toplevel id=8
  sent initial xdg configure w=1920 h=239 serial=1
  xdg_toplevel.set_title "alr-wl-test"
  xdg_surface.ack_configure serial=1
  surface committed shm: 256x256 stride=1024 fmt=1   (XRGB8888)

guest stdout:
  alr-wl: bound compositor+shm+xdg
  alr-wl: committed frame (released=1 frame_done=1)
```

Screenshot `v82-firstwindow-screenshot.png`: the SurfaceView at the top of the app shows the solid blue surface (the client's `0xFF3050C0` buffer — the `.bgra` swizzle resolves it to the correct colour) — the first Linux GUI window rendered on the Android screen through this stack.

## The full path, proven end to end

1. The guest `/bin/alr-wl-test` (a real Wayland client: dynamically-linked aarch64 glibc PIE with the `libwayland-client` core embedded) runs as an **in-process ALR guest** via the userspace ELF loader + in-process `ld.so` (Phase 1).
2. It `wl_display_connect(NULL)` over the `AF_UNIX` socket the loader pointed it at (`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` injected into the guest env), reaching the in-app compositor with **no socket-path mediation** (Phase 2).
3. The **real `libwayland-server` compositor** (Phase 3) handles the registry, `wl_compositor`, `xdg_wm_base`, the `xdg_toplevel` map + `configure`/`ack` handshake, and the `wl_shm` commit.
4. The compositor's present callback uploads the committed buffer with `glTexImage2D` and draws a textured quad onto the SurfaceView's `ANativeWindow` via the **proven EGL/GLES2 path** — `present=presented-frame, frames=1`.

This is the convergence of the execution track and the graphics track: a real Linux GUI program's window is on the Android display, every layer using only public APIs.

## What stands up

- **Real canonical `libwayland-server` 1.25.0 + `libffi` 3.5.2 + `xdg-shell` (wayland-protocols 1.48)**, vendored and compiled with the NDK into the app (`third_party/{libffi,wayland,wayland_generated,protocols}`, built via an append-only `arm64-v8a`-gated CMake block). Not hand-rolled — the same wire/`wl_interface` tables a stock client speaks.
- **A compositor on its own app thread** (`alr_wayland/alr_compositor.cpp`): `wl_display_create` → `wl_display_init_shm` → globals `wl_compositor`, `wl_shm`, `wl_seat`, `wl_output`, `xdg_wm_base` → `wl_display_add_socket` at `<cacheDir>/alr-xdg/wayland-0` → an epoll reactor folding `wl_event_loop_get_fd()`. Implements `wl_surface`/`xdg_surface`/`xdg_toplevel` enough to take a commit and pull the `wl_shm_buffer`.
- **Presentation hook → EGL** (`WaylandPresenter` in `runtime_report.cpp`): on each committed `wl_shm` buffer the compositor calls a present callback that uploads the pixels with `glTexImage2D` and draws a textured quad onto the SurfaceView's `ANativeWindow` via the proven EGL/GLES2 window-surface path (lazy EGL init on the compositor thread; `.bgra` swizzle for ARGB8888-in-memory; V-flipped UVs). No frame yet (no client).
- **Guest env injection**: the loader now builds the guest `envp` as a vector including `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=<cacheDir>/alr-xdg`, `GDK_BACKEND=wayland`, `SDL_VIDEODRIVER=wayland`, plus `LD_LIBRARY_PATH`/`PATH`/`HOME` — so a stock `libwayland-client` guest finds this socket with no path mediation.

## Why this matters

This is the dominant remaining subsystem (Layer C / Phase 3) coming alive: an unmodified Linux GUI toolkit can now, in principle, connect to a real Wayland server hosted in the app and have its window composited to the Android screen through public APIs only — no X-server hack, no VNC, no vendor-private GPU path. The transport (v81) and the compositor (here) are proven; the remaining step to a visible window is a guest client that commits a buffer.

## Next (immediate)

Ship the static/dynamic guest `wl_shm` test client (`/bin/alr-wl-test`, being built in parallel) into the rootfs, run it through the loader, and confirm: the compositor logs `client bound: wl_compositor`, the present callback fires, `frames>0`, and a solid-color window appears on the SurfaceView — the `ALR WAYLAND FIRST WINDOW` gate flips to PASS. Then: real `weston-terminal`/GTK demo, input wiring (Android MotionEvent → `wl_seat`), and the dmabuf/AHardwareBuffer zero-copy upgrade.
