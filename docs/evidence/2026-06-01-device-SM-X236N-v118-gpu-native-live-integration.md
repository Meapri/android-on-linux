# Device Evidence — SM-X236N, v118: the live guest→ring→host-executor→AHB-present loop runs on Mali (M4 STEP A)

The live-integration backbone of M4 is device-proven: a guest GLES op stream is streamed through
the SPSC command ring to a dedicated host **executor thread** that owns the Mali GLES2 context +
an AHardwareBuffer render target, which decodes and presents **8 frames** synchronized by the
per-frame `req_seq`/`reply_seq` handshake — all on the real Mali-G615, pixel-verified. This closes
every piece of the guest→GPU→AHB→screen path *except* the loader fork (STEP B): the same ring +
executor + AHB present now run as a live, multi-frame, two-thread pipeline rather than a one-shot
host probe. APK `0.4.118-android-gpu-native-live-v118`, device SM-X236N (Mali-G615 MC2).

## Device-verified (M4 STEP A — live integration)
```
ALR GPU LIVE INTEGRATION: PASS
alr gpu live model=two-thread in-process ring (producer thread + GL executor thread), per-frame req_seq/reply_seq handshake
alr gpu live frames_requested=8
alr gpu live frames_sent=8
alr gpu live frames_presented=8
alr gpu live frames_verified=8
alr gpu live last center=0,220,0 corner=26,26,102  (center ~0,220,0 textured-green, corner ~26,26,102 clear-blue — exact)
alr gpu live renderer=Mali-G615 MC2
alr gpu live software renderer=false
alr gpu live producer ok=true
alr gpu live error=(none)
```
`alr_gpu/alr_gpu_host_service.hpp`: `class GpuExecutorService` spawns a consumer thread that creates
its OWN EGL display + GLES2 context + `AhbRenderTarget` (GL is thread-affine, so the context lives
entirely on that thread), then drains the SPSC ring (`RingConsumer::snapshot`/`advance`) into a
per-frame buffer. A completed frame is detected when `RingHeader.req_seq` advances past the last
`req_seq` replied to — the producer's `flush_and_wait` bumps `req_seq` (acq_rel) *after* publishing
`head` (release), so observing the bump (acquire) guarantees the frame's bytes are visible. The
executor then binds the AHB-FBO, runs `decode_batch` with a **fresh `HostState` per frame** (so the
shim's constant virtual IDs resolve identically each frame), `glFinish()`es, invokes the present
callback (which `glReadPixels`-verifies center-green + corner-blue on the same GL thread), and
`post_reply()`s to release the producer. The in-process probe runs a producer thread that `append`s
the triangle op stream + `flush_and_wait`s for 8 frames over a plain 64 KiB heap ring (no fd/fork).

PASS gate: `frames_presented==8 && frames_verified==8 && software==false && error empty` (+ producer
ok / frames_sent==8). All satisfied.

## No regression — every prior GPU gate still PASS in this build
```
ALR GPU DRAW HARDWARE RENDER: PASS            (alr gpu draw software renderer=false)
ALR GPU RING TRANSPORT: PASS
ALR GPU RING DECODE+EXECUTE: PASS
ALR GPU RING HARDWARE RENDER: PASS            (alr gpu ring software renderer=false)
ALR GPU AHB RENDER (guest draw landed in AHB, direct read): PASS   (alr gpu fbo software renderer=false)
ALR GPU LIVE INTEGRATION: PASS                (alr gpu live software renderer=false)
```

## Where M4 stands now
The guest→GPU→AHB→screen path is proven on device piece-by-piece AND, as of this build, as a live
multi-frame pipeline:
- guest GLES op stream decoded + executed on Mali — v115 (M1)
- same stream via the SPSC command ring — v115 (M2)
- decoded draw renders INTO an AHB render target — v117 (M4 host half)
- AHB sampled zero-copy via external-OES and displayed — v114
- **live: producer → ring → executor thread → AHB → present, 8 frames, per-frame sync — v118 (this, M4 STEP A)**

Remaining = **STEP B (loader fork):** the loader `exec`s the real `alr-gles-cube` glibc binary (M3,
`app/src/main/cpp/alr_gpu/guest_shim/`) with the ring fd inherited (env `ALR_GPU_RING_FD`); its
`libEGL.so.1`/`libGLESv2.so.2` shim emits this exact op stream; the executor presents each
`eglSwapBuffers` frame to `WaylandPresenter` (external-OES, proven v114) → **a spinning cube on the
SurfaceView.** That step touches the loader fork in `runtime_report.cpp` and is the final mile.

## Build / verification
267→**270 host tests pass** (added `tests/test_android_alr_gpu_live_probe.py`); APK
`versionCode=118`, all 4 ABIs built (`buildCMakeDebug[arm64-v8a|armeabi-v7a|x86|x86_64]`); the new
JNI symbol `Java_…_nativeAlrGpuLiveProbe` is present in the shipped arm64 `libalr_loader.so`. The
live probe is an additive startup self-test (tag `alr_loader`); no existing probe or GIMP path
changed. `alr_gpu_host_service.hpp` is header-only; the `guest_shim/*.c` are explicitly listed-out
of the bionic build (built separately via `build-shim.sh`).
