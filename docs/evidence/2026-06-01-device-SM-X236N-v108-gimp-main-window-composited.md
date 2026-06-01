# Device Evidence — SM-X236N, v108: GIMP's MAIN EDITING WINDOW is visible (multi-surface compositing)

The Phase-6 visual milestone: GIMP 3.0.2's **main editing window** — menu bar, toolbox, canvas, dockable panels (Layers/Channels/Brushes), status bar — is composited and visible on the Android SurfaceView, with the "Welcome to GIMP 3.0.2" dialog drawn on top of it. Before this, the compositor showed only the last-committed surface, so the welcome dialog hid the main window entirely. Public Android APIs only, non-root.

Device SM-X236N (gta11p, Android 16 / API 36, untrusted_app). APK `0.4.108-android-gimp-multisurface-v108`. Device 1200x1920 (landscape app → 1920x1199 usable).

## What changed: painter's-algorithm multi-surface compositing

The compositor now retains a per-surface tight-packed pixel copy, tracks a z-ordered list of mapped xdg_toplevels (+ their popups), computes a placement rect per surface (parentless/large toplevel → fit the view; smaller/child toplevel → centered dialog over its parent), and hands the whole ordered snapshot to the presenter, which clears once, uploads+draws each quad bottom→top (per-surface GL texture cache keyed by a stable surface id, premultiplied-alpha blend), and swaps once. The presenter keeps the single-surface fast path; the legacy `present` callback still exists.

## Device-verified

```
surface committed shm: 1920x1199 ... key=11 role=toplevel   (main editing window)
surface committed shm:  647x714  ... key=13 role=toplevel   (welcome dialog)
xdg_toplevel.set_title "GNU Image Manipulation Program"
xdg_toplevel.set_title "Welcome to GIMP 3.0.2"
```
Screenshot `v108-gimp-multisurface.png` (1920x1200): GIMP's full main editing window fills the screen — the menu bar (File/Edit/Select/View/Image/Layer/Colors/Tools/Filters/Windows/Help), the left toolbox icon grid + tool options (FG/BG color, brush), the central canvas, the right dock (Layers/Channels/Paths, Brushes/Patterns), the bottom status bar — with the "Welcome to GIMP 3.0.2" dialog (logo + sunset artwork + Welcome/Personalize/Contribute/Create tabs + GIMP website/Tutorials/Documentation links + Help/Close) centered on top. Both surfaces drawn together = compositing works.

The app stays alive and interactive: tapping advanced the compositor frame counter 64 → 119 (interaction → redraw), `drain_input n=4` keeps delivering, no crash.

## Crash fixed to get here (multi-surface UAF)
The first multi-surface build (v106/v107) crashed because `place_toplevel` dereferenced the stored `parent_toplevel` xdg_toplevel resource to find a dialog's parent — but GIMP tears down its "GIMP Startup" splash toplevel while later windows still reference it, so that pointer dangled (use-after-free). Fixes (v108): (1) never dereference `parent_toplevel`; instead match it by pointer VALUE against live mapped toplevels in `g_zorder` (a stale address simply won't match, then fall back to a size-based parent scan); (2) `toplevel_destroy` now unmaps the surface (drops it from `g_zorder`, refocuses, repaints) before the role resource is freed; (3) defensive null-guards in the present loop. After this, GIMP boots to its main window and stays alive (CRASH=0).

## Honest status
GIMP's main editing window is on screen, composited with its welcome dialog — the headline Phase-6 visual is achieved on a real non-root device. Combined with v104 (fullscreen at the device's real resolution/DPI, Android bars hidden) and v105 (touch delivered to GIMP), this is a real, heavyweight Linux GUI behaving like an Android-native app.

Remaining polish (next): per-surface INPUT coordinate mapping for dialogs — touches currently map to the focused surface in output space, so tapping the welcome dialog's "Close" button (which is composited centered, not full-screen) can miss; the placement rect is known, so mapping a tap into the dialog's rect is the fix. Then: reaching/using the main canvas to actually draw (the LD_PRELOAD interposer from v100 + 1800s watchdog give the time budget).

No host regression: 236 host tests pass, native-core PASS.

## Session arc (v100 → v108, device-verified)
- v100: LD_PRELOAD path interposer (fast in-process rootfs path mediation).
- v101: fullscreen layout + xdg_toplevel fullscreen state.
- v102: device resolution/DPI → wl_output (real mm + scale) + immersive bars; watchdog 1800s (NPE crash).
- v103: fixed immersive NPE (apply after setContentView).
- v104: wl_output mm/dpi + bars verified on device.
- v105: touch fixed — added wl_data_device_manager so GDK binds wl_seat; touches delivered to GIMP.
- v106/107: multi-surface compositing applied (per-surface pixels, z-order, placement, present loop) — crashed (parent_toplevel UAF).
- v108: **multi-surface UAF fixed — GIMP's main editing window + welcome dialog composited and visible on device.**
