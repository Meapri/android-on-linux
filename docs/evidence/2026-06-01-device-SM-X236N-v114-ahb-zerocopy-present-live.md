# Device Evidence — SM-X236N, v114: AHardwareBuffer zero-copy present is LIVE (gltex=0 path=zerocopy)

The display path is now full hardware zero-copy on device: GIMP's surfaces are sampled by the Mali GPU directly from per-surface AHardwareBuffers via `GL_TEXTURE_EXTERNAL_OES`, with NO per-frame `glTexImage2D` CPU→GPU upload. This is milestone M2 of the GEGL-GPU design's Option C (display-path acceleration), the achievable "full hardware acceleration" under non-root + public-API-only. APK `0.4.114-android-gimp-ahb-present-v114`, device SM-X236N (Mali-G615 MC2 / MT6878, Android 16, untrusted_app).

## Device-verified

Read from the live compositor status (`nativeWaylandCompositorStatus()` → report) on a running GIMP:
```
alr wl present=presented-scene ahb=2 gltex=0 path=zerocopy
alr wl present=presented-scene ahb=8 gltex=0 path=zerocopy
```
- `path=zerocopy` — the present loop is on the AHardwareBuffer/external-OES path (all extensions present: EGL_ANDROID_get_native_client_buffer, EGL_KHR_image_base, GL_OES_EGL_image_external).
- `ahb=8 gltex=0` — every content upload (8 so far) went through the zero-copy AHB path; the `glTexImage2D` CPU→GPU fallback fired **zero** times.
- GIMP renders correctly via the external-OES sampler (screenshot `v114-gimp-ahb-present.png`: welcome artwork, logo, tabs, main window — colors correct, BGRA swizzle right), no GL errors, app alive.

## What M2 does (runtime_report.cpp WaylandPresenter)

Before: on each surface content change, `present_list` did `glTexImage2D(GL_RGBA, w, h, …, s.pixels)` — a CPU→GPU re-upload of the whole surface (~33 MB/frame for a 4K canvas).

Now: each surface caches a per-surface `AHardwareBuffer` (R8G8B8A8, GPU_SAMPLED | CPU_WRITE), imported once as `GL_TEXTURE_EXTERNAL_OES` via `eglGetNativeClientBufferANDROID` → `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` → `glEGLImageTargetTexture2DOES`. On content change the committed pixels are `memcpy`'d into the AHB's locked CPU plane (honoring its stride) and the GPU samples the AHB directly — no `glTexImage2D` transfer. A dedicated `samplerExternalOES` program with the same V-flip + `.bgra` swizzle composites it. Falls back to the sampler2D/`glTexImage2D` path if AHB/EGLImage/external-OES is unavailable (`path=gltex-fallback`), so nothing regresses. AHBs + EGLImages are reaped with their surfaces.

## Honest scope (what "full hardware acceleration" means here)
- DISPLAY: now zero-copy / native-grade — the per-frame CPU→GPU canvas upload is gone. Most visible on large canvases and pan/zoom/scrub.
- One CPU copy remains (compositor copies the guest's wl_shm into the AHB plane). Eliminating it (M3: the guest's Cairo surface IS the AHB) needs the in-app compositor to hand the guest an AHB-backed wl_shm pool — the guest GTK still allocates its own shm pool today, so M3 requires guest-side cooperation; deferred and documented.
- GIMP's INTERNAL pixel compute (GEGL filters, brush composite) stays CPU — GIMP 3.0's only GPU compute path is OpenCL, which is off-by-default upstream and vendor-locked/unreachable on this Android device under the constraints. This is a GIMP/Android limit, not an ALR one; desktop GIMP runs the same ops on the CPU by default. We do NOT pursue OpenCL (would break the non-root/public-API contract).

## No regression
267 host tests pass; native build green; GIMP's full touch workflow (boot → File menu → New → canvas → draw) still works.

## Note (concurrent work)
This was committed as a two-session checkpoint (`9dba4db`) alongside an in-flight PC-gate seccomp / interposer workstream owned by another session (files: runtime_report.cpp path-mediation counters, libalr_interpose.c, scripts/build-interpose.sh, docs/design/pcgate-seccomp.md, tests/test_pcgate_bpf_logic.py). The M2 present-path code and the PC-gate code are in non-overlapping regions; both compile and pass tests. M2 measurement instrumentation (a logcat mirror of the present status) and M3 are deferred until the PC-gate workstream lands, to avoid concurrent edits to runtime_report.cpp.
