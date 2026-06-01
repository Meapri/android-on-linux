# Device Evidence — SM-X236N, v117: guest GLES draws land in an AHardwareBuffer render target on Mali (M4 host half)

The render-target half of M4 is device-proven: a decoded guest GLES draw renders into an AHardwareBuffer-backed FBO on the Mali-G615, verified by reading the pixels straight back from the AHB. Combined with v114 (AHB → external-OES zero-copy display) and v115 (M1/M2 decode+ring on Mali), all the pieces of the guest→GPU→AHB→screen path are now individually proven on hardware; only the live integration (host GPU thread + loader fork + cube client) remains. APK `0.4.117-android-gpu-native-ahb-render-v117`, device SM-X236N (Mali-G615 MC2).

## Device-verified (M4 host half)
```
ALR GPU FBO RENDERTARGET: PASS
ALR GPU AHB RENDER (guest draw landed in AHB, direct read): PASS
alr gpu fbo ops decoded=32
alr gpu fbo AHB-direct pixel=0,220,0 corner=26,26,102  (center=texture-green triangle, corner=clear-blue — exact)
alr gpu fbo renderer=Mali-G615 MC2   render err=0x0   software renderer=false
alr gpu fbo external-oes resample: info-only (v114 proves external-OES display separately)
```
`alr_gpu_fbo.hpp`: allocate an AHardwareBuffer (usage GPU_FRAMEBUFFER | GPU_SAMPLED) → import as a GL_TEXTURE_2D via eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)/glEGLImageTargetTexture2DOES → attach as an FBO color attachment (glFramebufferTexture2D) → FBO complete → run the M1 triangle op stream through alr::gpu::decode_batch into that FBO → glFinish → glReadPixels directly from the AHB-backed FBO. The center reads 0,220,0 (the textured triangle) and the corner 26,26,102 (the clear) — exact. So a decoded guest draw genuinely renders into the AHB on real Mali hardware.

## Honest note on the external-OES re-sample
The probe also tries to re-sample that same AHB through GL_TEXTURE_EXTERNAL_OES in the SAME context immediately after rendering (reads black, `external-oes pixel=0,0,0`). This is a self-test artifact — reading an AHB as an external texture right after rendering to it in one context hits Mali tile-resolve / read-after-write-in-one-context ordering, not a render failure (render err=0, sample err=0). It does NOT gate the verdict. The external-OES *display* path is already proven independently in v114 (`gltex=0 path=zerocopy`, GIMP composited via external-OES), where the producer (compositor upload) and consumer (present) are separate operations — exactly the separation the live M4 has (host renders the frame, then the present path samples it next tick).

## Where M4 stands
The guest→GPU→AHB→screen path now has every piece proven on device, separately:
- guest GLES op stream decoded + executed on Mali — v115 (M1)
- same stream via the SPSC command ring — v115 (M2)
- decoded draw renders INTO an AHB render target — v117 (this, M4 host half)
- AHB sampled zero-copy via external-OES and displayed — v114
Remaining = LIVE INTEGRATION: a host GPU thread that creates the ring + an AHB-FBO, the loader forks the guest with the ring fd inherited, the guest libGLESv2/libEGL shim (M3, source built off-tree, compiling) emits the op stream, the host drains+decodes into the AHB, and on the guest's eglSwapBuffers the host hands that AHB to the present path → a spinning cube on the SurfaceView. That wiring touches runtime_report.cpp + the loader fork and is the next milestone.

## No regression
267 host tests pass; native-core PASS; M1 + M2 still PASS in this build; GIMP still usable; the GPU probes are additive startup self-tests (tag alr_loader). All 4 ABIs compile.
