# Device Evidence — SM-X236N, v119: GPU screen-cube present pipeline (STEP B-1)

The live GPU command pipeline now presents to a real on-screen `ANativeWindow`: a spinning textured
cube op stream goes guest-producer → SPSC ring → host executor → AHB render target → **cross-context
external-OES present to the SurfaceView's ANativeWindow**, 60 frames, on the Mali-G615. APK
`0.4.119-android-gpu-screen-cube-v119`.

## Device-verified (present pipeline)
```
ALR GPU SCREEN CUBE: PASS
alr gpu screen model=in-process producer thread + GL executor thread, decode->AHB-FBO then cross-context external-OES present to ANativeWindow
alr gpu screen size=1920x1199
alr gpu screen frames_requested=60  frames_sent=60  frames_presented=60
alr gpu screen window present ready=true
alr gpu screen renderer=Mali-G615 MC2
alr gpu screen software renderer=false
alr gpu screen producer ok=true   error=(none)
```
`alr_gpu_host_service.hpp` gained an optional `ANativeWindow*` (nullptr preserves the pbuffer-only
probe). In window mode the executor's GL thread holds TWO EGL contexts: the pbuffer context renders
the cube into the AHB-FBO (`decode_batch` + `glFinish`), then the window-surface context imports that
AHB as `GL_TEXTURE_EXTERNAL_OES` and draws a full-screen quad + `eglSwapBuffers` — sampling in a
DIFFERENT context than the render, which is the v114-proven way around the v117 same-context
black-AHB tile-resolve hazard. `alr_gpu_screen.hpp` builds the 36-vertex rotating cube stream.

## Honest scope — pipeline verified, on-screen visual still pending
All 60 frames were presented to the ANativeWindow with no GL/EGL error (window ready, software=false).
What is NOT yet visually confirmed: the cube **pixels on the physical screen**. The demo runs
synchronously inside `surfaceCreated`, which blocks the first window traversal, so during the demo the
device still shows the launcher splash (the app window hasn't come up, so its child SurfaceView isn't
composited yet) — screencaps at the PASS instant show the splash, not the cube. This is a UI-timing
issue, not a render/present failure: the pipeline swapped 60 frames to the Surface successfully.
Visually confirming the cube needs a non-blocking path (run the demo off the UI thread after the
window is up, ordered before the compositor takes the single Surface).

## Bonus discovery — the real glibc rootfs
The LD_PRELOAD log revealed the full Debian glibc rootfs at
`files/rootfs/debian-arm64/` (PCGATE's R3 absolute-rootfs LD_PRELOAD path is live there). Earlier
"rootfs empty" was looking one level too high. This unblocks running real forked glibc guests
(shims + binaries can be staged under `debian-arm64/`).

## Build / verification
274 host tests pass (added `tests/test_android_alr_gpu_screen_cube_probe.py`); APK versionCode 119,
all 4 ABIs built; `nativeAlrGpuScreenCube` present in the shipped arm64 `libalr_loader.so`. The
spinning-cube demo is an additive surfaceCreated step; existing probes + Wayland compositor path
unchanged. Also folded in: the guest shim's `GL_RENDERER` is now vendor-neutral (no hardcoded "Mali").
