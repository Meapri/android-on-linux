# Device Evidence — CP-2 FINAL: glmark2 renders on Mali through the ALR shim, Score 1074

Build `0.4.128-cp2-glmark2-persist-hoststate-v128` (versionCode 128, UNCHANGED — the fix ships in the shim, not the APK). Device SM-X236N / Mali-G615 (mt6878, Android 16). Cold start. Shim fix commit `997a1c0` (glGetShaderiv(GL_SHADER_SOURCE_LENGTH)), deployed via a rebuilt `gpushim-stage.tar` (268800 B) auto-restaged over the prior 256000 marker. PCGATE on, interpose on.

## Result — glmark2-es2-wayland --benchmark build
```
glmark2 2023.01
    GL_VENDOR:      Android-on-Linux (ALR)
    GL_RENDERER:    ALR command-stream (host GPU passthrough)
    GL_VERSION:     OpenGL ES 2.0 ALR
    Surface Config: buf=32 r=8 g=8 b=8 a=8 depth=24 stencil=0 samples=0
    Surface Size:   1920x1200 windowed
[build] <default>: FPS: 1075 FrameTime: 0.931 ms
                                  glmark2 Score: 1074
```
- **glmark2 Score: 1074** (build scene, 1075 FPS @ 1920x1200). The prior `"Expected shader source length 1252, but got 0"` / `"Set up failed"` errors are **GONE** — the build scene now compiles its shaders and renders.
- `gpushim-stage: overlay done (extracted=14 skipped=0)` (re-staged the fixed shim, libGLESv2.so.2 218720→221512); `glmark2-stage` already present.
- `ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU SCREEN CUBE: PASS` (persistent HostState, on real Mali).
- `display: 1920x1200 @ 90000mHz density=213`.
- Loader: child `exit=0 signal=0`, `reached=jumped-to-entry`, path-mediation `traps=40 rewrites=17`. No FATAL / SIGSEGV anywhere.
- Supplementary: GIMP 3.0.2 rendered on-device (later GUI probe) — full GUI pipeline intact.

## What this proves (CP-2 = GPU 풀가속 마지막 관문)
A glibc GLES2 app (glmark2) runs **in-process via ALR**, its GL calls encoded by the guest `libGLESv2.so.2`/`libEGL.so.1` shim into the SPSC ring, decoded and replayed on the **real Mali-G615** by the host `GpuExecutorService`, rendered into an AHB and presented — at **1075 FPS**, `software=false`. 1075 FPS on a 1920x1200 scene is GPU-class, not a software rasterizer.

Note on GL_RENDERER: the shim returns its synthetic string (guest sees the shim; the actual draws run on Mali host-side — proven by LIVE INTEGRATION/SCREEN CUBE rendering on real Mali at software=false). Passing the host's real GL_RENDERER through is a cosmetic follow-up, not a CP-2 gate.

Scope/caveat: the on-device APK is the pre-merge v128 (built at `ba57faf`, before ws-2's 8MiB ring `dc4198e` and ws-3/ws-4 were merged). The build scene (no textures, ~72KB VBO) fits the old 1MiB host ring, so it scored. Texture/bump scenes (1024x1024 = 4MiB single op) need the 8MiB host ring, which is in source (`alr_gpu_ring_hook.hpp` default) but NOT in this on-device APK — a future APK rebuild (e6d513f+) + full `glmark2` run would exercise those. CP-2's gate (a real glmark2 scene scoring on Mali, software=false) is met by the build scene here.

## Root-cause chain closed (3 drains)
- merge `b6a886a` (persistent HostState): fixed per-frame virtual→real map reset → self-tests pass, but glmark2 stuck at config (drain pre-#6).
- drain#6: glmark2 reached a live GL context but failed build-scene setup on shader-source validation (Score 0).
- commit `997a1c0` + drain#7 (this): shim answers `glGetShaderiv(GL_SHADER_SOURCE_LENGTH)` locally from the cached source length → build scene compiles → **Score 1074 = CP-2 FINAL**.

## Verdict
**CP-2 (GPU 네이티브 풀가속) ACHIEVED.** The single remaining critical path of the orchestration plan is CLOSED. CPU (CP-3 0%) / display (90Hz) / present (CP-4) / GUI (gtk3/foot/GIMP) / GPU (glmark2 on Mali) are all device-proven.
