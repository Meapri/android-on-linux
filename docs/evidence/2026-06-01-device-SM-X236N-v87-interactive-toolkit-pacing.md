# Device Evidence — SM-X236N, v87: interactive 2D toolkit (input→redraw→display) + frame pacing

Phase 3 closes the loop: a real 2D toolkit (pixman) **responds to input visually** — injected Android touches move a circle the client redraws and commits, and the compositor presents each new frame to the SurfaceView. Plus the compositor now **paces frame callbacks at ~60 Hz** (instead of acking immediately), so toolkit clients animate at display rate rather than busy-looping. The single-window interactive substrate an app like GIMP needs is complete.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.87-android-pacing-interactive-v87`, SHA-256 `077c58b6d099f6afc1d20cd4868909d3f8f606bfd1b7b0c5f6d4b8e183ab05c9`. pytest 235 passed, native-core PASS, validate-host PASS, BUILD SUCCESSFUL.

## Result

```
ALR WAYLAND INTERACTIVE TOOLKIT (input→redraw→display loop):  PASS (redraws=5 hits=2)
   guest stdout: alr-interactive: redraws=5 hits=2 lastxy=480,360

(no regression: WAYLAND FIRST WINDOW + REAL 2D TOOLKIT + INPUT INJECTION all still PASS)
```

Screenshot `v87-interactive-screenshot.png`: the SurfaceView shows the pixman scene — "ALR" vector text on a gradient — with a filled orange disc and concentric "hit" rings at the **last injected touch point (480,360)**, i.e. the marker followed the input.

## The full interactive loop, proven on device

1. `/bin/alr-interactive-test` (pixman compiled in; in-process ALR guest) maps a window and gets `wl_pointer` + `wl_touch`.
2. The app injects moving touch points (via the SurfaceView `OnTouchListener`'s path / the inject self-test) → the compositor delivers them over `wl_seat`.
3. The client updates its cursor `(cx,cy)` per event and **redraws** a pixman scene with the circle at `(cx,cy)` + a ring per hit, then commits the `wl_shm` buffer.
4. The compositor's present callback uploads each new buffer via EGL → the SurfaceView. `redraws=5` (multiple frames as the input arrived), `hits=2`, `lastxy=480,360` (the circle tracked to the last point).

This is the complete round trip: **Android input → compositor → client logic → client render → compositor → Android display**, every layer public-API-only, non-root.

## Frame pacing (Stage A polish, needed for usable GTK)

The compositor previously acked `wl_surface.frame` callbacks immediately, which makes a frame-driven toolkit (e.g. GTK) busy-loop. Now a `timerfd` at ~60 Hz is folded into the epoll reactor; committed frame callbacks are queued (`g_pending_frames`) and fired on each tick. A per-callback destroy listener drops any whose client disconnects before the tick, so the queue never dangles; the tick pops-then-sends to stay safe against the listener's mutation. Verified no regression — the existing single-commit and interactive clients still render (they get their callback within ≤16 ms, no stall).

## Position / next (toward GIMP)

Phase 3 is functionally complete for **single-window interactive 2D toolkits**: render (static + real toolkit), input (Android→wl_seat), the interactive loop, and 60 Hz pacing all work on device. Remaining toward a real GTK app then GIMP:
- **GTK3 library closure (~30 `.so`s) + Phase-5 service files** (gschemas.compiled, fontconfig, gdk-pixbuf loaders, icon theme) staged into the rootfs — in progress (background provisioning is staging `libgtk-3.so.0` et al.).
- A real GTK3 window client → verify a genuine toolkit window + interactivity through this exact compositor seam.
- Then: real subsurface compositing (GTK menus/popups — marshalling already vendored), GPU-in-guest for GIMP's GL/GEGL, and the GIMP stack.
