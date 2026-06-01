# Device Evidence — CP-2 drain: ws-2 persistent HostState device-verified; glmark2 advances to a NEW shader-source failure

Build `0.4.128-cp2-glmark2-persist-hoststate-v128` (versionCode 128). Device SM-X236N / Mali-G615 (mt6878, Android 16). Cold start (`am force-stop` → `logcat -c` → `am start`). Integration-session device drain of ws-2 merge `b6a886a` (WS-2 CP-2: executor persistent HostState across frames). PCGATE on, interpose on.

## ws-2 fix VERIFIED — DEVICE-REQ condition (1): PASS
Both GPU self-tests pass on the now-persistent `HostState` (no regression from the fresh-per-frame state that the self-test triangle/cube previously relied on):
- `gpushim-stage: overlay done (extracted=7 skipped=0)` — shim (`libEGL.so.1`/`libGLESv2.so.2`, NEEDED=libc.so.6 only, no libpthread) staged.
- `glmark2-stage: overlay done (extracted=292 skipped=0)`.
- **`ALR GPU LIVE INTEGRATION: PASS`** (guest → ring → host executor → AHB present, 8 frames on Mali).
- **`ALR GPU SCREEN CUBE: PASS`** (live pipeline → AHB → external-OES on SurfaceView).
- `display: 1920x1200 @ 90000mHz density=213`.
- Supplementary: device screen at capture showed **GIMP 3.0.2 fully rendered** (later GUI probe) — full GUI pipeline confirmed working on-device.

## glmark2 — DEVICE-REQ condition (2): NOT MET (Score still 0), but failure mode ADVANCED
`/usr/bin/glmark2-es2-wayland --data-path /usr/share/glmark2 --benchmark build` reaches a **live GL context via the shim** (config/EGL OK — past the prior Score=0 cause), then fails at build-scene shader setup:

```
glmark2 2023.01
    GL_VENDOR:      Android-on-Linux (ALR)
    GL_RENDERER:    ALR command-stream (host GPU passthrough)
    GL_VERSION:     OpenGL ES 2.0 ALR
    Surface Config: buf=32 r=8 g=8 b=8 a=8 depth=24 stencil=0 samples=0
    Surface Size:   1920x1200 windowed
Error: Failed to add vertex shader from file None:
Error: source length 1252, but got 0
[build] <default>: Set up failed
                                  glmark2 Score: 0
```
- `glmark2-result: frames 2214->2215` (compositor wl-frame counter; the shim presents executor→window directly, so wl frames do not track glmark2's GPU frames — not the Score signal).
- Loader: child `exit=0 signal=0`, `reached=jumped-to-entry`, path-mediation `traps=37 rewrites=16`, seccomp-emulated syscalls=1. No FATAL / SIGSEGV / AndroidRuntime crash anywhere in the run.

### Root cause (NEW, distinct from the merged fix) → WS-2
The persistent-HostState fix removed the per-frame virtual→real map reset. The next wall is **shader-source marshalling**: glmark2 sets a 1252-byte vertex shader source, then `glGetShaderiv(GL_SHADER_SOURCE_LENGTH)` returns **0**, so glmark2 rejects it ("source length 1252, but got 0"). Either
- (a) `glShaderSource` is not transferring the source string payload through the ring to the host real GL object, or
- (b) `glGetShaderiv(GL_SHADER_SOURCE_LENGTH)` is not marshalled/implemented in the executor and defaults to 0.

Minor (not the blocker): `GL_RENDERER` is the shim's synthetic string, not the real Mali-G615 — by design (guest sees shim GL strings; Mali rendering is host-side and is proven by LIVE/CUBE PASS = software=false). WS-2 may optionally pass the host `glGetString(GL_RENDERER)` through for authenticity.

## Verdict
- ws-2 merge `b6a886a` is correct and stays: condition (1) PASS, zero regression.
- CP-2 FINAL not yet reached. Single remaining critical path unchanged in owner (WS-2), advanced in symptom: **shader-source transfer (`glShaderSource` / `glGetShaderiv(GL_SHADER_SOURCE_LENGTH)`)** so the build scene compiles its shaders → Score>0.
- WS-5: CP-2 Score parse remains blocked on the WS-2 shader-source fix; LIVE/CUBE PASS + GUI-render evidence captured here.
