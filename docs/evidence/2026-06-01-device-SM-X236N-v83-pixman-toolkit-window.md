# Device Evidence — SM-X236N, v83: a real 2D toolkit (pixman) renders vector graphics through the ALR compositor

The rung above a hand-filled buffer: **pixman 0.42.2 — the actual rasterizer inside Cairo, X.Org and Qt** — runs as an in-process ALR guest, draws a gradient + anti-aliased filled circles + anti-aliased vector text "ALR" into a `wl_shm` buffer, and that real 2D vector content is composited onto the Android SurfaceView through the in-app Wayland compositor. Non-root, public APIs only, **zero new rootfs library provisioning** (pixman compiled into the client; NEEDED resolves to the rootfs's existing `libffi`/`libm`/`libc`).

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.83-android-pixman-toolkit-v83`, SHA-256 `7e2041beb06760bc22ed195b2eb8f409ff667506f57aad12be685b710625f743`. pytest 235 passed, native-core PASS, validate-host PASS, BUILD SUCCESSFUL.

## Result

```
ALR WAYLAND FIRST WINDOW (guest wl_shm client → compositor → SurfaceView):           PASS
ALR REAL 2D TOOLKIT WINDOW (pixman gradient+AA shapes+text → SurfaceView):            PASS
   compositor frames=2 (alr-wl-test solid 256x256, then alr-pixman-test 480x320)

logcat (tag alr_wayland):
  client bound: wl_compositor v4 / xdg_wm_base v2
  surface committed shm: 256x256 stride=1024 fmt=1          (alr-wl-test)
  client bound: wl_compositor v4 / xdg_wm_base v2
  xdg_toplevel.set_title "alr-pixman"
  surface committed shm: 480x320 stride=1920 fmt=1          (alr-pixman-test, tight stride)

guest stdout:
  alr-toolkit: pixman drew frame (480x320 released=1 frame_done=1)
```

Screenshot `v83-pixman-screenshot.png`: the SurfaceView shows a blue gradient with large anti-aliased white vector glyphs "ALR" and an orange filled disc — real 2D vector rasterization (gradients, anti-aliasing, vector text), not a solid fill.

## Why this is the right next rung

- **pixman is a real production toolkit**, not a toy — it is the software rasterizer Cairo/X.Org/Qt use. Proving it renders end-to-end means the compositor's `wl_shm` path carries genuine anti-aliased 2D content correctly (format, stride, swizzle, V-flip all verified by a legible result).
- **No rootfs bloat**: the PoC rootfs ships no GUI toolkit libs (only `libffi` among GUI-relevant ones), so pixman was compiled into the client via the proven `zig cc --target=aarch64-linux-gnu.2.36 -fPIE -pie` recipe. NEEDED = `libffi.so.8`, `libm.so.6`, `libc.so.6`, all already present; max symbol `GLIBC_2.35` ≤ rootfs 2.39.
- **Two clients, one compositor**: alr-wl-test (solid) then alr-pixman-test (toolkit) both connected, mapped `xdg_toplevel`s, and committed — `frames=2` — exercising the compositor with sequential real clients.
- **Pixel path validated for real content**: pixman `a8r8g8b8` on LE-aarch64 = `[B,G,R,A]` in memory = exactly the compositor's `.bgra`-swizzling EGL present expects; the legible "ALR" confirms format + stride (1920 == 480×4, tight) + V-flip are all correct.

## Position / next (toward Phase 6)

Phase 3 now carries real 2D toolkit output. The documented next rungs (`docs/design/toolkit-on-wl_shm-rung.md`) reuse this exact compositor seam:
- **Cairo** (full `cairo_t` API, `cairo-font-face-twin` for FreeType-free text) — a superset of this pixman rung.
- **SDL2** (software renderer) — needs the `libSDL2` `.so` closure provisioned into the rootfs.
- **GTK3** (`gtk_window` + drawing area) — heavy (~30-lib closure + fontconfig cache), the real Phase-6 toolkit.
- Plus: input (Android MotionEvent/KeyEvent → `wl_seat` + xkb keymap), frame pacing (Choreographer), stride-aware upload (GLES3 `GL_UNPACK_ROW_LENGTH`), AHardwareBuffer/dmabuf zero-copy, multi-window.
Then Phase 5 system services (fontconfig/D-Bus/GSettings) → Phase 6: a real GTK app → GIMP.
