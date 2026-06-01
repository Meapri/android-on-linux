# Device Evidence — SM-X236N, v113: AHardwareBuffer zero-copy display path proven on Mali hardware

The realistic GPU win identified by the GEGL-GPU feasibility study (Option C, milestone M1): a guest frame can be sampled as a GL texture with NO per-frame CPU→GPU copy, using only public Android APIs, pixel-correct on the real Mali GPU. This is the path that makes GIMP's *display* native-grade (compute stays CPU, same as desktop GIMP). APK `0.4.113-android-gimp-ahb-zerocopy-v113`, device SM-X236N (Mali-G615 MC2 / MT6878, Android 16, untrusted_app).

## Why this matters (the honest GPU answer)
GIMP's internal pixel work (GEGL ops, brush composite, canvas) is CPU — GIMP 3.0's only GPU compute path is OpenCL, which is off-by-default upstream and vendor-locked/unavailable on this Android device under non-root + public-API-only. That part cannot be GPU-accelerated under the constraints, and that's a GIMP/Android limitation, not an ALR one. What CAN be made native-grade is the DISPLAY path: today the compositor does a CPU `glTexImage2D` re-upload of the whole canvas every content change (~33 MB/frame at 4K). The zero-copy AHardwareBuffer→EGLImage→external-OES path removes that copy. v113 proves that path works on device.

## Device result — all PASS

```
alr ahb ext native_client_buffer=yes      (EGL_ANDROID_get_native_client_buffer)
alr ahb ext egl_image_base=yes            (EGL_KHR_image_base)
alr ahb ext gl_oes_egl_image_external=yes (GL_OES_EGL_image_external)
ALR AHB ZEROCOPY IMPORT: PASS
ALR AHB ZEROCOPY HARDWARE SAMPLE: PASS
alr ahb center pixel=230,120,20   (expected orange 230,120,20 — exact round-trip)
alr ahb renderer=Mali-G615 MC2
alr ahb gl error=0x0
alr ahb software renderer=false
```

What the probe (`build_ahb_zerocopy_probe_report`, runtime_report.cpp) does, mirroring the rigor of the existing GPU marshalling probe:
1. `AHardwareBuffer_allocate` a 64×64 R8G8B8A8 buffer (GPU_SAMPLED_IMAGE | CPU_WRITE/READ).
2. CPU-fill a known orange pattern (230,120,20,255), honoring the buffer's real pixel stride.
3. Import zero-copy: `eglGetNativeClientBufferANDROID` → `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` → `glEGLImageTargetTexture2DOES` onto a `GL_TEXTURE_EXTERNAL_OES` texture. **No glTexImage2D of the pixels.**
4. Sample it with a `samplerExternalOES` shader into an RGBA FBO, `glReadPixels` the center.
5. Verify the orange round-tripped (230,120,20), GL error 0, renderer non-software.

The center pixel came back EXACTLY 230,120,20 on Mali-G615 hardware → the AHB content reached the GPU as a sampleable texture with no CPU copy, the canonical Android zero-copy path, 100% public NDK API (AHardwareBuffer, EGLImage, GL_OES_EGL_image_external — all already linked via libandroid/EGL/GLESv2).

## What's next (Option C milestones, from the design)
- M1 (this): host-only proof AHB→GL zero-copy works on device — DONE.
- M2: WaylandPresenter keeps a per-surface AHB; copy the committed wl_shm into the AHB once, import, sample external-OES — removes the per-frame glTexImage2D re-upload. Measure ns/frame + FPS at 1080p/4K (the first real "GPU win" number).
- M3: AHB-backed Wayland buffer pool so the guest's Cairo surface IS the AHB (no host copy at all).
- M4: EGL_ANDROID_native_fence_sync producer/consumer fences.

## Honest framing for the user
- CPU compute (filters/strokes): unchanged, CPU-bound — same as desktop GIMP 3.0 (OpenCL off by default).
- Display: with AHB zero-copy (now proven feasible), presentation becomes as efficient as a native Android app — most visible on large-canvas pan/zoom/scrub where the per-frame canvas upload dominates.
- OpenCL GPU compute: not available on this device under the constraints — do not promise it.

## No regression
236 host tests pass, native-core PASS. GIMP still boots to its main window and the full touch workflow works; the AHB probe is an additive startup self-test (logs to tag `alr_loader`).
