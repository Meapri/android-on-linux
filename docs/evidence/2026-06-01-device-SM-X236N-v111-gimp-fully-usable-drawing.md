# Device Evidence — SM-X236N, v111: GIMP is FULLY USABLE — drew brush strokes on a new canvas by touch

The Phase-6 final goal, complete on a real non-root device: a real, unmodified **GIMP 3.0.2** runs natively in-process on stock Android, and a full interactive workflow works **entirely by touch** — boot → close the welcome dialog → File menu → New → the New Image dialog → OK → a 1920×1080 canvas is created → **paint brush strokes on it with a finger**, and the pixels are drawn. Public Android graphics APIs only (EGL/GLES/ANativeWindow), W^X-safe, no PRoot, no vendor-private GPU.

Device SM-X236N (gta11p, Android 16 / API 36, untrusted_app). APK `0.4.111-android-gimp-dialoginput-v111`.

## The full workflow — every step device-verified by touch

1. **Boot**: GIMP loads its 112-lib closure in-process, reaches its main window. Titles: "GIMP Startup" → "GNU Image Manipulation Program" → "Welcome to GIMP 3.0.2".
2. **Close welcome** (tap): the welcome dialog (a child toplevel of the main window, `set_parent key=13 parent=main`) is dismissed; the bare main editing window shows (screenshot `v111-gimp-main-clean.png`).
3. **File menu** (tap top-left): `xdg_surface.get_popup id=28 ... parent_key=11` → a 290×466 popup committed `role=popup` — GIMP's File dropdown rendered over the main window (`v108-gimp-file-menu-open.png`).
4. **New…** (tap the menu item): `drain_input target_kind=popup` — input routed to the popup — and GIMP opened `xdg_toplevel.set_title "Create a New Image"`, a 433×325 dialog (`v111-gimp-new-dialog.png`: Template, Image Size Width 1920 / Height 1080 px, 300 ppi RGB, Advanced Options, Help/Reset/Cancel/OK).
5. **OK** (tap, mapped into the dialog's composited rect): the dialog closed and the main window re-rendered repeatedly — GIMP created the image. Screenshot `v111-gimp-new-canvas-1920x1080.png`: a white **1920×1080 canvas** with rulers, a "Background" layer in the Layers dock, "Background (1920×1080)" in the status bar.
6. **Draw** (three finger swipes on the canvas): screenshot `v111-gimp-drawing-on-canvas.png` shows **three black paintbrush strokes** (a V shape + a horizontal line) painted on the canvas; status bar reads "Click to paint line; SHIFT for straight line; CTRL to pick a color". GIMP's Paintbrush actually painted pixels from touch input.

## What made the last steps work (this push, v109→v111)

- **v109 — popup/menu input routing + Android keyboard**: injected pointer/touch now target the top-most mapped popup (`input_target_surface()` picks the popup with the highest `map_serial`), and coordinates map into the popup's composited rect (`map_input_to_surface`, parent chain resolved by KEY to avoid the v108 UAF). This makes menu items clickable. Also wired Android KeyEvent → evdev → `wl_keyboard` (`nativeWaylandInjectKey` JNI + a keymap in MainActivity), with the SurfaceView focusable for hardware keys.
- **v110 — IME on demand**: stopped auto-raising the soft keyboard on every tap (it stole focus and covered menu items); the IME now only appears on long-press. Hardware keys still inject.
- **v111 — dialog input routing**: GIMP's dialogs ("Welcome", "Create a New Image") are **child toplevels**, not popups. When no popup is mapped, input now follows `zorder_top()` (the top-most mapped toplevel = the open dialog) instead of the stale focus surface, so taps on a dialog's buttons (Close, OK) land correctly. This is real modal-dialog behavior: while a dialog is up, input goes to it.

## Status — Phase 6 achieved
A heavyweight Linux desktop application (GIMP 3.0) runs natively in-process on non-root Android, presents through public graphics APIs, and is **driven by touch through a complete create-and-draw workflow**. Combined with the earlier milestones — fullscreen at the device's real resolution/DPI (v104), Android system bars hidden, touch delivered to GIMP (v105), multi-surface compositing so the main window + dialogs + menus draw together (v108) — this is a usable, Android-native-feeling GIMP.

Remaining are refinements, not blockers: present-throttling to the 60Hz tick (BLAST buffer-queue saturation under commit storms — design in /tmp/loader-damage/DESIGN.md), surfacing the loader's interposer counters to prove/quantify the in-process path-mediation speedup (same design doc, measuring `path_rewrites` not `path_traps`), per-surface damage, and exec-re-entry for GIMP's 124 plug-ins (non-fatal without them).

No host regression: 236 host tests pass, native-core PASS.

## Session arc (v100 → v111, all device-verified)
- v100 LD_PRELOAD path interposer · v101 fullscreen state · v102 device DPI/resolution + immersive (NPE) · v103 NPE fix · v104 wl_output mm/dpi + bars verified · v105 touch (wl_data_device_manager → GDK binds wl_seat) · v106/107 multi-surface (UAF) · v108 multi-surface UAF fixed → **main editing window + welcome dialog composited** · v109 popup input routing + keyboard · v110 IME on-demand · v111 dialog input routing → **File→New→OK→canvas→drew brush strokes by touch**.
