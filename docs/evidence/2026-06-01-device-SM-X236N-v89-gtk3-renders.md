# Device Evidence — SM-X236N, v89: a real GTK3 app initializes (Wayland backend) and renders a window

A real **GTK3** application runs through the full ALR stack on device: `gtk_init` succeeds with the **Wayland backend**, it connects to the in-app compositor, creates a 640×480 toplevel, and **commits 5 cairo-rendered frames** — a genuine GTK window rendered onto the Android SurfaceView. It then aborts on one provisioning gap (a missing gdk-pixbuf PNG loader, being fixed), so the clean-exit gate is not yet PASS, but GTK is functionally running.

Also fixes the **startup crash** the GTK rootfs first caused.

Device SM-X236N (Android 16 / API 36, untrusted_app). APK `0.4.89-android-gtk3-symlink-v89`, SHA-256 `bb4553f866372056f04a17abeecef2e12dcd86ee6545b75c732bbb387a8e12d4`. pytest 235 passed, BUILD SUCCESSFUL.

## The startup crash — root-caused and fixed

The 71 MB merged GTK rootfs tar (Ubuntu noble closure) first crashed the app at launch:
```
FATAL EXCEPTION: RootfsInstaller.extractVerifiedTar:112 → onCreate
```
Cause: `RootfsInstaller` **rejected all symlinks** ("tar links are not supported yet"), and a real GTK rootfs is full of SONAME symlinks (`libfoo.so.N → libfoo.so.N.M.P`, multiarch links). This is a rootfs-installer limitation, **not** a compositor/GUI bug — no GUI code had run.

Fix: the installer now **creates symlinks safely** — only those whose target resolves *within* the rootfs (relative, in-tree), which the kernel follows correctly during the guest's path-mediated `open`; absolute or escaping targets are skipped (they'd resolve against the Android root). Hard links are copied from the in-rootfs original. After the fix the app launches, extracts the GTK rootfs, and all prior gates still PASS.

## GTK3 runs

```
ALR REAL GTK3 WINDOW (gtk_init→toplevel→cairo→SurfaceView):  FAIL (aborts after rendering)

guest=/bin/alr-gtk3-test  (dynamic glibc PIE, ~50-lib GTK3 closure)
LINK MODE: DYNAMIC(interp-handoff)   reached=jumped-to-entry
guest threads spawned=3
path-mediation traps=2088 rewrites=1915   (first: /etc/ld.so.preload → rootfs)
seccomp-emulated syscalls=4 (set_robust_list ×4)
guest stdout: alr-gtk3: gtk_init ok; backend=wayland
compositor logcat: client bound wl_compositor/wl_seat/xdg_wm_base;
                   surface committed shm: 640x480 stride=2560 ×5
child exit signal=6 (SIGABRT):
  Gtk:ERROR gtkiconhelper.c:495 ensure_surface_for_gicon:
  Failed to load image-missing.png: Unrecognized image file format (gdk-pixbuf)
```

What this proves:
- **GTK3 initialized fully** with the Wayland backend — the ~50-library closure, `gschemas.compiled`, fontconfig, the injected env (`GDK_BACKEND=wayland`, `GSETTINGS_BACKEND=memory`, etc.) all worked through the in-process `ld.so`.
- **The path-mediation layer scaled to a real toolkit**: GTK opened ~2088 files (libs, configs, icons, fonts) and 1915 were rewritten into the rootfs — the selective seccomp path mediation handles a heavyweight app's file access, not just a few opens.
- **GTK created and rendered a window**: a 640×480 cairo-drawn toplevel committed 5 frames to the compositor, which presented them to the SurfaceView.
- The abort is a single provisioning gap: the staged gdk-pixbuf loaders include ani/bmp/gif/… but **no PNG loader**, so GTK's CSD titlebar fallback icon (`image-missing.png`) can't decode → `g_assert` → abort. Fix in progress (ship the PNG loader + regenerate `loaders.cache`, and/or an undecorated client to avoid CSD icons).

## Position

This is the Phase-6 threshold: a real GTK3 app runs and renders through ALR's compositor on non-root Android, public APIs only. One gdk-pixbuf PNG-loader fix away from a clean, persistent GTK window — after which: subsurface compositing for menus, then the GIMP stack.
