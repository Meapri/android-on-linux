# Device Evidence — SM-X236N, v86: Android input reaches a Wayland client (interactive substrate)

Stage B of Phase 3: the in-app Wayland compositor now delivers **input** as well as rendering. Android touch/pointer/keyboard events injected into the compositor are delivered over `wl_seat` to a connected in-process Wayland client, which receives them as real `wl_pointer`/`wl_keyboard`/`wl_touch` events. Combined with the render path (v82/v83), the compositor is now a **bidirectional, interactive** Wayland display server — the substrate an interactive GUI app needs.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.86-android-input-injection-v86`, SHA-256 `bc8c5a291de96ef82372b18135978c1ecba0763157a304ec083f9cbc2d1544dd`. pytest 235 passed, native-core PASS, validate-host PASS, BUILD SUCCESSFUL.

## Result

```
ALR WAYLAND INPUT INJECTION (Android→wl_seat→client):  PASS (received=24)
   echo client stdout: alr-input: received 24 input events (pointer=10 key=8 touch=6)

compositor logcat (tag alr_wayland):
  client bound: wl_seat v5
  wl_seat.get_pointer bound / get_keyboard bound / get_touch bound
  xdg_toplevel.set_title "alr-input-test" → surface committed shm 320x240

(no regression: WAYLAND FIRST WINDOW + REAL 2D TOOLKIT + all Phase 1 gates still PASS)
```

## The full input path, proven end to end

1. **A guest Wayland client** (`/bin/alr-input-test`, dynamic glibc PIE, in-process ALR guest) binds `wl_seat` and, on the advertised capabilities, creates `wl_pointer` + `wl_keyboard` + `wl_touch` with full listeners.
2. **The compositor** (`alr_compositor.cpp`) now tracks those resources (the seat getters were greenfield), advertises pointer+keyboard+touch capabilities, and on `wl_keyboard` bind sends a keymap (`NO_KEYMAP` → the client's default; a server xkb keymap needs data the PoC rootfs lacks).
3. **Injection**: `nativeWaylandInjectSelfTest(x,y)` (and the production `nativeWaylandInjectTouch` from the SurfaceView's `OnTouchListener`) enqueue `InjectEvent`s under a mutex and `write()` the compositor's wakeup `eventfd`. Three synthetic bursts (pointer motion + button down/up, touch down/up, key down/up) were fired ~1.8/2.6/3.4 s after the client connected.
4. **The compositor's epoll reactor** wakes on the eventfd and drains the queue **on the compositor thread** (where `wl_resource` sends must happen), translating each event into `wl_pointer_send_{enter,motion,button,frame}` / `wl_touch_send_{down,up,motion,frame}` / `wl_keyboard_send_{enter,key}` with proper serials, to the focused surface.
5. **The client received 24 events** across all three device types (pointer=10, key=8, touch=6) — proof the path works.

The focus surface is set when a client commits its first buffer (maps); `g_pointer_entered`/`g_keyboard_entered` reset there so the next event sends a fresh `enter`. A real Android touch on the SurfaceView is forwarded (as both `wl_touch` and `wl_pointer`, toolkit-agnostic) by the `OnTouchListener` — so once a real toolkit window is up, it is interactive.

## Position / next (toward GIMP)

Phase 3 now has **render (Stage A) + input (Stage B)** working on device. The compositor is a real interactive Wayland server reachable by unmodified clients through public Android APIs only, non-root. Remaining toward a real interactive GTK app (then GIMP):
- **The GTK3 library closure** (~30 `.so`s) + Phase-5 service files (gschemas.compiled, fontconfig, gdk-pixbuf loaders, icon theme) staged into the rootfs — in progress (background provisioning).
- A minimal GTK3 window client → verify a real toolkit window renders + responds to the forwarded touches.
- Then: real subsurface compositing (GTK menus/popups), frame pacing (AChoreographer), GPU-in-guest (GIMP's GL/GEGL), and the rest of the GIMP stack.
