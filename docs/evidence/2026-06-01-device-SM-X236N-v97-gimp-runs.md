# Device Evidence — SM-X236N, v97: GIMP 3.0 runs and renders a window on non-root Android

The Phase 6 final target: **GIMP 3.0.2** — one of the heaviest Linux desktop applications — runs in-process on stock non-root Android and **renders a real GTK3 window** (its "GIMP User Installation" first-run dialog, with title, body text, log, and a Quit button) onto the Android SurfaceView. Every layer uses only public Android APIs.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.97-android-gimp-glib-v97`, SHA-256 `dc6ded37cc33eaba04ecdee58a3192da2bfa5e7697965eb8c14ff5bea866da58`. pytest 235 passed, native-core PASS, validate-host PASS.

## Result — all gates PASS

```
ALR GIMP 3.0 LOADS (gimp-console-3.0 --version, 112-lib closure in-process):  PASS
   stdout: GNU Image Manipulation Program version 3.0.2   (child exit=0)
ALR GIMP 3.0 GUI (gimp-3.0 → main window → SurfaceView):                       PASS (frames 9→10)

compositor logcat:
  xdg_wm_base.get_xdg_surface; xdg_toplevel.set_title "GIMP User Installation";
  xdg_toplevel.set_app_id "gimp-3.0"; surface committed shm: 1972x294 stride=7888

(no regression: GTK3 window, interactive toolkit, input, image decode, first window all PASS)
```

Screenshot `v97-gimp-screenshot.png`: the SurfaceView shows GIMP's **"GIMP User Installation"** window — GIMP-rendered GTK3/Cairo content: the title, the body ("It appears that you are using GIMP for the first time…"), an installation log, and a **Quit** button — composited to the Android display.

## What runs

GIMP 3.0.2 (GTK3) as an in-process ALR guest:
1. **`gimp-console-3.0 --version`** (rung 1, headless) loads GIMP's **112-library closure** (libgimp\*, GEGL, babl, codecs, the full GTK3/GLib stack) via the in-process `ld.so` and prints its version — clean exit 0. The closure is **symbol-complete**: 0 unresolved across 14 core ELFs + 77 modules + 112 plug-in binaries.
2. **`gimp-3.0`** (rung 4, full GUI) runs, connects to the in-app `libwayland-server` compositor, creates an `xdg_toplevel` ("GIMP User Installation"), and **commits a 1972×294 buffer** that the compositor uploads via EGL to the SurfaceView — a real GIMP window on the Android screen. (It exits on `SIGALRM` — the 40 s loader watchdog stopping a long-running GUI, not a crash; it rendered first.)

The path mediation carried GIMP's thousands of file opens into the rootfs; the xdg_popup/positioner hardening (v96) keeps GIMP from crashing on menu/tooltip objects; image decode (v95) works for its icons.

## The fixes that got here (this session)

- **GLib version reconciliation**: GIMP 3.0.2 (Ubuntu plucky) needs GLib 2.84's `g_variant_builder_init_static`; the rootfs had noble GLib 2.80. Upgraded GLib + libmount + added libatomic (noble→plucky), all GLIBC ≤ 2.38 (under the rootfs's 2.39) → the GIMP closure became symbol-complete. GTK3 stays working (plucky GLib is strictly additive).
- **Image decode** (v95): shipped the shared-mime-info DB (`/usr/share/mime`) — gdk-pixbuf selects decoders by MIME, which needs that DB. PNG/JPEG/BMP/GIF now decode in-process.
- **xdg_popup/positioner** real impls (v96): prevent a compositor crash when GIMP builds menus.
- Loader: captures guest **stderr** (revealed the symbol error), watchdog raised to 40 s for heavy apps. Build stores the 240 MB+ rootfs tar uncompressed (`noCompress`).

## Honest status

GIMP **runs and renders its window** — the hardest Phase-6 app is on screen. The current window is GIMP's first-run dialog reporting "user installation failed" (GIMP couldn't create its writable config dir — a next-iteration data/env fix: a writable `GIMP3_DIRECTORY`/`$HOME/.config`). Reaching the **main editing window** additionally needs: that config-dir fix, **exec-re-entry** for GIMP's 124 plug-in child processes (File I/O — GIMP is non-fatal without them), and **multi-surface compositing** for GIMP's docks/dialogs/menus to all draw. But the core thesis is proven on a real device: a real, unmodified, heavyweight Linux GUI application runs natively in-process on non-root Android and presents through public graphics APIs.

## Session arc (v73 → v97, all device-verified)
static glibc exec → threads/fork → path mediation → dynamic ld.so → real Debian programs → `dash -c` → write-path → Wayland compositor → first window → real 2D toolkit (pixman) → input → interactive loop → frame pacing → **real GTK3 window** → in-process image decode → **GIMP 3.0 loads + renders a window**.
