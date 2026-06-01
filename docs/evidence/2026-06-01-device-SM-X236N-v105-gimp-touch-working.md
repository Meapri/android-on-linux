# Device Evidence — SM-X236N, v105: touch input reaches GIMP (and GTK3)

Touch now works end to end: an Android touch on the SurfaceView is delivered to GIMP/GTK3 as Wayland pointer + touch events, and GIMP reacts (re-renders). This was the last "is it actually usable" gap. Public Android APIs only, non-root.

Device SM-X236N (gta11p, Android 16 / API 36, untrusted_app). APK `0.4.105-android-gimp-touch-ddm-v105`.

## Root cause (proven from GDK source), not guessed

GDK 3.24's Wayland backend does **not** bind `wl_seat` directly from the registry handler. It postpones the seat via `postpone_on_globals_closure()` with a hard precondition list `required_device_manager_globals[] = {"wl_compositor", "wl_data_device_manager", NULL}` (gtk `gtk-3-24` `gdk/wayland/gdkdisplay-wayland.c`). The closure that actually calls `wl_registry_bind(&wl_seat_interface, …)` runs only once **both** globals exist. Our compositor advertised wl_compositor, wl_shm, wl_seat, wl_output, xdg_wm_base, wl_subcompositor — but **not wl_data_device_manager** — so GDK never bound the seat → no `wl_pointer`/`wl_keyboard`/`wl_touch` → zero input. A bare C client (`alr-input-test`) has no such precondition machinery, so it bound the same seat fine — which is exactly why "the seat is broken" was the wrong diagnosis. The seat version (advertised 5; GDK negotiates `MIN(version,5)`, no floor) was NOT the cause.

## Fix

Added a minimal stub `wl_data_device_manager` global (v3) to the compositor, mirroring the existing `wl_subcompositor` minimal-impl pattern: no-op `wl_data_source` (offer/destroy/set_actions), no-op `wl_data_device` (start_drag/set_selection/release), and a manager (create_data_source/get_data_device/release) that hands out those stubs. An empty clipboard (no selection/data_offer events) is valid. Also bumped `kSeatVersion` 5→7 (generated ceiling is 10; companion change, not the fix).

## Device-verified — before vs after

Before (v104, no data_device_manager): touches reached the compositor but had nowhere to go:
```
drain_input n=4 focus=<surface> pointers=0 touches=0 keyboards=0
```
After (v105): GTK3 and GIMP both bind the manager, then immediately bind the seat and create input devices:
```
client bound: wl_data_device_manager v3
client bound: wl_seat v5
wl_seat.get_pointer bound (now 1)
wl_seat.get_touch  bound (now 1)
... "ALR GTK3" ... "GIMP Startup" ... "GNU Image Manipulation Program"
```
And injected touches are now delivered AND GIMP reacts:
```
drain_input n=4 focus=<surface> pointers=1 touches=1 keyboards=1 scale=1
surface committed shm: 10x16   (cursor/highlight)
surface committed shm: 647x714 (widget re-render)
```
The focus surface changes between taps (`…565880` → `…546b30`) = focus moving between GIMP widgets/windows as you tap them. App stays alive throughout.

## Honest status
Touch input is functional: Android touch → wl_pointer + wl_touch → GIMP, and GIMP re-renders in response. Combined with v104 (fullscreen at the device's real resolution/DPI, Android system bars hidden), GIMP now behaves like an Android-native GUI you can touch.

What's still open for full desktop fidelity: **multi-surface compositing** — GIMP commits several surfaces (main window 1920x1199, the welcome dialog, tooltips/cursors like the 10x16 buffer), but the presenter shows only the last-committed one, so the welcome dialog covers the main editing window. A multi-surface present (painter's algorithm, design in /tmp/multisurface/IMPLEMENTATION.md) is the next step so the main window + dialog are drawn together. Then reaching/using the main editing window itself (watchdog is 1800s; the LD_PRELOAD interposer from v100 speeds the file path).

No host regression: 236 host tests pass, native-core PASS.

## Session arc (v100 → v105, device-verified)
- v100: LD_PRELOAD path interposer for fast in-process rootfs path mediation.
- v101: fullscreen layout + xdg_toplevel fullscreen state.
- v102: device resolution/DPI → wl_output (real mm + scale) + immersive bars; watchdog 1800s. (NPE crash)
- v103: fixed immersive NPE (apply after setContentView).
- v104: wl_output mm/dpi correctness + bars verified on device.
- v105: **touch fixed** — added wl_data_device_manager so GDK binds wl_seat; touches delivered to GIMP, GIMP re-renders.
