# Device Evidence — CP-5 batch: glmark2 texture scene on the 8MiB ring (Score 1163) + ws-4 overlays stage

Build `0.4.129-cp5-batch-8mibring-v129` (versionCode 129). Device SM-X236N / Mali-G615. Cold start. First device build carrying the 4-branch merge (ws-2 8MiB ring `dc4198e`, ws-3 present-fallback `6d1551b`, ws-4 overlays `b68482e`) + the shim shader-source fix (`997a1c0`, re-staged gpushim). PCGATE on, interpose on.

## GPU — 8MiB ring verified (headline CP-5 result)
glmark2-es2-wayland with two scenes (`build:duration=5 --benchmark texture:duration=5`):
```
[build]   duration=5: FPS: 1206 FrameTime: 0.829 ms
[texture] duration=5: FPS: 1123 FrameTime: 0.891 ms
                                  glmark2 Score: 1163
```
- The **texture scene** (a textured-quad scene whose texture upload is a single large `OP_TEX_IMAGE_2D` the old 1MiB host ring would silently drop) now **renders at 1123 FPS** on the 8MiB ring (ws-2 `dc4198e`, host `GpuRingAttachConfig.ring_bytes 1<<23`) — no `gl_error`, no `Set up failed`, no shader-source error. Combined **Score 1163** (build-only was 1074 in drain#7).
- Confirms CP-2 extends past the build (geometry) scene to a texture-using scene: a glibc GLES2 app's textured draws marshal through the ring and render on real Mali.

## ws-4 universality overlays — stage clean on device
`/data/local/tmp` tars auto-staged (MainActivity §line163 loop), overlay-guard 0 skips:
- `dpkg-db-stage: overlay done (extracted=195 skipped=0)` — dpkg admin DB reconstruct (apt/dpkg readiness).
- `x11-stage: overlay done (extracted=131 skipped=0)` — X11 client libs.
- `apt-config-stage: overlay done (extracted=2 skipped=0)` — apt config.
- (xwayland-stage.tar pushed but not in the §line163 auto-stage loop — sits unused this run.)

## Regression / health
- `ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU SCREEN CUBE: PASS` (persistent HostState, ws-3 present-fallback in the APK — no regression).
- `display: alr gpu screen size=1920x1199`, `XKB_CONFIG_ROOT` rootfs-absolute. GIMP 3.0.2 rendered (final GUI probe). No FATAL/SIGSEGV.

## Honest gaps (not blockers; WS-4 breadth)
- The *functional* exercise of apt/X11 (actually running an X11 client / `apt`/`dpkg` query) is reported only in the on-device execution-summary TextView, not logcat — overlays are proven STAGED here; a dedicated `alr_loader` probe (or summary capture) is needed to log the runtime result. WS-4 owns.
- `getpwuid_r(): failed due to unknown user id (10326)` still warns under gtk3-widget-factory — non-fatal (GUI runs), but it is WS-4's open §10(a) passwd/nss item, NOT fixed in this batch.

## Verdict
CP-5 GPU axis advanced: glmark2 **texture scene renders on Mali via the 8MiB ring, Score 1163**. ws-4 universality overlays (dpkg/x11/apt-config) stage cleanly on-device. Remaining CP-5 work is WS-4 functional (apt/X11 app run + passwd/nss) and the broader toolkit matrix (sdl2/netsurf/qt6).
