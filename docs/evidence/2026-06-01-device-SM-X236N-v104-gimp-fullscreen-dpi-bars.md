# Device Evidence — SM-X236N, v104: GIMP runs fullscreen, at the device's real resolution + DPI, with Android system bars handled

Making the Linux GUI feel Android-native, per the goal "run Linux GUI apps natively on Android — the GUI must adapt to Android's top/bottom bars and detect the device's resolution + DPI." GIMP 3.0.2 now runs **edge-to-edge fullscreen** (Android status + navigation bars hidden, swipe to reveal) at the **device's real resolution and DPI**. Public Android APIs only.

Device SM-X236N (gta11p, Android 16 / API 36, untrusted_app). APK `0.4.104-android-gimp-dpi-bars-v104`. Device `wm size` = 1200x1920, `wm density` = 240 (app runs landscape → 1920x1199 usable).

## Results — device-verified

### 1. Device resolution + DPI detected and applied (was: hardcoded 1280x720 / ~96 dpi)
```
client bound: wl_output v2 (1920x1199 px, 235x147 mm, scale=1, dpi=207)
sent initial xdg configure w=1920 h=1199
```
- The Wayland output advertises the device's **actual** usable size 1920x1199 px at the device's **207 dpi**, with the correct physical size in millimetres (1920 px / 207 dpi × 25.4 = 235 mm). The previous code passed pixels into `wl_output.geometry`'s mm fields, so a toolkit computed a nonsense ~2-metre-wide monitor and the wrong DPI.
- Metrics flow end to end: `MainActivity.resources.displayMetrics` (densityDpi/xdpi/ydpi) → JNI `nativeWaylandCompositorStart(…, densityDpi, xdpi, ydpi)` → `CompositorConfig{density_dpi,xdpi,ydpi,output_scale}` (alr_compositor.hpp) → `wl_output.geometry`/`mode`/`scale` + the `xdg_toplevel.configure` logical size. Integer buffer scale = round(dpi/160), clamped to keep ≥1024 logical px (here scale=1).

### 2. Android system bars handled — edge-to-edge fullscreen (was: a narrow 180dp band)
- `MainActivity.applyImmersive()` (called after `setContentView` and in `onWindowFocusChanged`): `setDecorFitsSystemWindows(false)` + `decorView.windowInsetsController.hide(statusBars | navigationBars)` with `BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE`. The SurfaceView fills the screen (`weight=1`); the diagnostic report is demoted to a 1px strip.
- The compositor drives the client to fill the output: `xdg_toplevel.configure` carries the full logical size **and** `XDG_TOPLEVEL_STATE_FULLSCREEN | MAXIMIZED | ACTIVATED`, so GTK/GIMP size to the screen instead of a small default.
- Screenshot `v104-gimp-fullscreen-dpi-bars.png` (1920x1200) — programmatic band check confirms no Android bars: top 40px = (52,41,39) and bottom 40px = (47,47,47) are GIMP's own dark content (an Android status/nav bar would be a distinct lighter strip with icons); mid (160,110,78) is GIMP's sunset welcome artwork; 63 distinct colour buckets = rich GIMP UI filling the display.

## Crash fixed during this work
v102 crashed at launch (`NullPointerException` in `getInsetsController`) because the immersive code ran in `onCreate` before the DecorView existed. Moved to `applyImmersive()` after `setContentView` (+ `onWindowFocusChanged`); v103/v104 launch clean and stay alive (ptid confirmed) through GIMP's full boot to its main window.

## Honest status
The two requested behaviours — Android-bar adaptation and device resolution/DPI detection — are done and verified on device: GIMP boots to its main window fullscreen at 1920x1199 / 207 dpi with no Android bars overlapping.

**Touch is still in progress (separate issue).** Android touches reach the compositor (`drain_input n>0`, valid focus surface), but GIMP/GTK's GDK Wayland backend does not bind `wl_seat` (its connection binds wl_compositor, wl_output, wl_subcompositor, xdg_wm_base — but not wl_seat), so it creates no `wl_pointer`/`wl_touch` and the events have no destination. A plain C Wayland client (`alr-input-test`) binds `wl_seat v5` on the same compositor and receives input fine — so the compositor's seat is correct; the gap is specifically GDK's seat setup. Root-causing it needs the guest's WAYLAND_DEBUG protocol trace, which the in-process loader does not yet surface (guest stderr isn't captured through the supervisor's fd routing) — that loader-diagnostic fix is the next step, then the GDK seat path. (Compositor groundwork already in place: seat sends name-before-capabilities, tracks live seats, and re-advertises capabilities on surface map.)

No host regression: 236 host tests pass, native-core PASS.

## Session arc (v100 → v104, device-verified)
- v100: shipped LD_PRELOAD path interposer (`/usr/lib/androlinux/libalr_interpose.so`) for fast in-process rootfs path mediation.
- v101: fullscreen layout + xdg_toplevel fullscreen state.
- v102: device resolution/DPI → wl_output (real mm + scale); immersive system-bar hiding; watchdog 100s→1800s. (crashed — NPE)
- v103: fixed the immersive NPE (apply after setContentView).
- v104: wl_output mm/dpi correctness verified on device; bars + DPI confirmed; touch diagnosed (GDK not binding wl_seat) and groundwork laid.
