# Architecture verification — glibc rootfs, X-less Wayland, CPU native / GPU split (SM-X236N)

Answers, code- and device-verified, to three common questions about how the ALR runtime actually works. Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app, arm64-v8a).

## Q1. Is it a glibc rootfs? — YES

A full Debian glibc userland, not Android bionic:
- rootfs ships `/lib/ld-linux-aarch64.so.1` + `/lib/aarch64-linux-gnu/libc.so.6` (glibc, symbol versions GLIBC_2.33–2.39).
- GIMP's ELF requests interpreter `/lib/ld-linux-aarch64.so.1` (the guest glibc ld.so) — NOT Android's `/system/bin/linker64`.
- GIMP 3.0.2 + GTK3 + GEGL/babl (112-lib closure) all link this glibc.

## Q2. X server, or runs directly on the Android Surface? — NO X server; in-app Wayland

No X11/Xorg/XWayland at all. The app embeds a real Wayland compositor (vendored libwayland-server 1.25.0 + xdg-shell):
- guest env: `WAYLAND_DISPLAY=wayland-0`, `GDK_BACKEND=wayland`; there is no `DISPLAY`.
- compositor globals: `wl_compositor, wl_shm, wl_seat, wl_output, xdg_wm_base, wl_subcompositor, wl_data_device_manager`.
- path: GIMP (GTK Wayland client) → AF_UNIX socket → in-app Wayland compositor → wl_shm buffer → EGL/GLES texture → ANativeWindow (SurfaceView).
- Graphics use PUBLIC Android APIs only (EGL/GLES/AHardwareBuffer/ANativeWindow); no vendor-private paths (/dev/dri, KMS, GBM), no root.

## Q3. Is CPU/GPU performance native-app grade? — CPU yes; GPU "display yes, GIMP-internal render still CPU"

### CPU — native, no emulation
- GIMP ELF `e_machine = 183 (EM_AARCH64)` = device ABI `arm64-v8a`. Same instruction set.
- NO QEMU / emulation / binary translation. The in-process loader maps PT_LOAD into anon execmem (RW→memcpy→RX, W^X-safe) and jumps natively (`alr_enter_guest` = set sp/entry, branch). GIMP runs INSIDE the app process (verified: GIMP threads share the app PID; no qemu child).
- So CPU compute is 1:1 native — equal to a native Android app. The only overhead is filesystem path mediation (rootfs path remap), which happens per file-open only (not per compute op), and the LD_PRELOAD interposer moves most of that in-process to cut ptrace round-trips.

### GPU — compositing/display is hardware Mali; GIMP's own render is CPU (Cairo)
- DISPLAY/compositing path = real Mali GPU. EGL/GLES2 on Mali-G615 uploads + composites the surfaces. Device logs show `mali_gralloc` + `mali_platform_hal: useCompressionVulkan` (the real Mali driver), `software renderer=false`. Putting pixels on screen is fully hardware-accelerated.
- GIMP's INTERNAL rendering is still CPU: `GDK_RENDERING=cairo` → GIMP paints its canvas/widgets with the Cairo software rasterizer into wl_shm buffers, which Mali then displays. I.e. "GIMP computes pixels = CPU; showing them = Mali GPU."
- Perf signal: the device shows BLAST buffer-queue saturation (`Already acquired max frames`, 4+2) — the compositor swaps FASTER than the display drains, so the GPU display path is not the bottleneck (it's actually too fast; a present-throttle to 60Hz was added). GIMP boot to main window ≈ 4 s.

### One-line summary
glibc Debian rootfs, X-less, presented via an in-app Wayland compositor onto the Android SurfaceView. CPU is native ARM64 (zero emulation); compositing/display is Mali GPU hardware; only GIMP's internal canvas render is still Cairo (CPU). Next GPU step = GEGL OpenGL backend in-guest and/or AHardwareBuffer zero-copy display (design under evaluation).

### End-to-end proof
On device, by touch only: boot → close welcome → File menu → New → New Image dialog → OK → 1920×1080 canvas → painted brush strokes. (Evidence: v111-gimp-drawing-on-canvas.png + 2026-06-01-...-v111-gimp-fully-usable-drawing.md.)
