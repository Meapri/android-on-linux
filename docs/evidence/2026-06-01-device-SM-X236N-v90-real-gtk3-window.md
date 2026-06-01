# Device Evidence — SM-X236N, v90: a REAL GTK3 application window on non-root Android

A genuine, unmodified **GTK3 application** runs end-to-end on stock non-root Android and renders its window on screen — `gtk_init` (Wayland backend) → a mapped top-level → GTK/Cairo rendering → composited to the Android SurfaceView. Clean exit, no abort. Every layer uses only public Android APIs.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.90-android-gtk3-window-clean-v90`, SHA-256 `7ec9eaa21479ea76b806bd2b07360fd098dee42ace29ea1b0f8be93b8983f59a`. pytest 235 passed, native-core PASS, validate-host PASS, BUILD SUCCESSFUL.

## Result — all GUI gates PASS

```
ALR WAYLAND FIRST WINDOW (guest wl_shm client → compositor → SurfaceView):  PASS
ALR REAL 2D TOOLKIT WINDOW (pixman gradient+AA shapes+text):                PASS
ALR WAYLAND INPUT INJECTION (Android→wl_seat→client):                       PASS (received=24)
ALR WAYLAND INTERACTIVE TOOLKIT (input→redraw→display loop):                PASS (redraws=5 hits=2)
ALR REAL GTK3 WINDOW (gtk_init→toplevel→cairo→SurfaceView):                 PASS

GTK3 client (/bin/alr-gtk3-test), child exit=0 signal=0:
  alr-gtk3: gtk_init ok; backend=wayland
  alr-gtk3: show_all called (undecorated)
  alr-gtk3: pump done (iters=51, mapped=yes, elapsed=0.10s)
  alr-gtk3: realized window
compositor: client bound wl_compositor/wl_seat/xdg_wm_base/wl_subcompositor;
            xdg_toplevel.set_app_id "alr-gtk3-test"
```

Screenshot `v90-gtk3-window-screenshot.png`: the SurfaceView shows GTK's own rendering — a light (Adwaita) window with the **"ALR GTK3"** label — the real GTK3 toolkit drawing, composited to the Android display.

## The full stack, every layer public-API-only and non-root

A real GTK3 binary (dynamically linked against a ~50-library closure: libgtk-3, gdk, glib/gobject/gio, pango, cairo, gdk-pixbuf, harfbuzz, fontconfig, freetype, libwayland-client, libxkbcommon, …) runs as an **in-process ALR guest**:
1. **Execution**: the userspace ELF loader loads the guest's own `ld-linux-aarch64.so.1`, which links the 50-lib closure from the rootfs (each a file-backed `PROT_EXEC` mmap — allowed in `untrusted_app`); glibc threads + `set_robust_list` serviced; `gtk_init` succeeds with the **Wayland backend**.
2. **Filesystem**: the selective seccomp path mediation rewrote GTK's **~2088 file opens** into the rootfs (libs, `gschemas.compiled`, fontconfig, icon theme, configs) — the mediation scales to a heavyweight toolkit.
3. **Display**: GTK's Wayland backend connects over `AF_UNIX` to the in-app **`libwayland-server` compositor**, which receives the `wl_shm` buffer GTK/Cairo rendered into and composites it via **EGL/GLES → the SurfaceView** on the Mali GPU.
4. **Clean exit 0** — no `SIGABRT`.

## Two fixes that got here from v89

- **Startup crash (symlinks)**: `RootfsInstaller` rejected all symlinks; a real GTK rootfs is full of SONAME symlinks. It now safely creates in-rootfs relative symlinks (the kernel follows them within the path-mediated rootfs) and skips escaping ones. → app launches with the 71 MB GTK rootfs.
- **GTK `SIGABRT` (gdk-pixbuf PNG)**: v89 aborted loading the CSD titlebar's `image-missing.png`. PNG decode is builtin-but-not-engaging in-process; the robust fix is an **undecorated** window (`gtk_window_set_decorated(FALSE)` + a plain `GtkLabel`) — no CSD titlebar → no icon decode → clean run. (The `loaders.cache` was also corrected for the external loaders, useful for GIMP later.)

## Position — Phase 6 threshold crossed

The orthodox roadmap's hard milestone — a **real GTK application** rendering on non-root Android through ALR — is met. The compositor is interactive (input + 60 Hz pacing), and a genuine GTK3 toolkit window is on screen. Remaining toward GIMP:
- **In-process PNG/image decode** (so GTK can show icons/decorations and GIMP can load images) — the gdk-pixbuf builtin loader's in-process relocation/dispatch under the ALR loader.
- **Real subsurface compositing** (GTK menus/popups/CSD; marshalling already vendored).
- **GPU-in-guest** (GIMP's GL/GEGL), then the GIMP application stack.
