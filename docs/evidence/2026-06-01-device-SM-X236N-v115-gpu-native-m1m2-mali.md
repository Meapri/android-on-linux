# Device Evidence — SM-X236N, v115: GPU-native GLES marshalling renders on Mali (M1 + M2)

The host half of the GPU-native-Linux-app path (Phase 4) is device-proven: a real GLES2 shader+VBO+texture+draw op stream is decoded and executed on the Mali-G615 GPU — both directly (M1) and after travelling through the shared-memory command ring (M2). This is the layer that will let a glibc guest drive the real GPU (a future libGLESv2 shim feeds this exact op stream). Non-root, public Android GPU APIs only. APK `0.4.115-android-gpu-native-m1m2-v115`, device SM-X236N (Mali-G615 MC2 / MT6878, Android 16, untrusted_app).

## M1 — decode shader+VBO+texture+draw directly on Mali (PASS)
```
alr gpu draw probe=marshalling-shader-vbo-texture-draw
ALR GPU DRAW DECODE+EXECUTE: PASS
ALR GPU DRAW HARDWARE RENDER: PASS
ops decoded=32   stream bytes=739
center pixel=0,220,0   (expect ~0,220,0 textured triangle)
corner pixel=26,26,102 (expect ~25,25,102 clear)
renderer=Mali-G615 MC2   gl error=0x0   software renderer=false
```
A hand-built op stream (`build_triangle_stream`) — create/compile a VS+FS, link a program, upload a VBO of 3 verts (pos+UV), upload a 2x2 green texture, set an MVP uniform, bind attribs, glDrawArrays(GL_TRIANGLES) — is decoded by `alr::gpu::decode_batch` (alr_gpu_decode.hpp) on a real EGL pbuffer + GLES2 context. The center pixel reads back the texture's green (the drawn triangle), the corner the blue clear — exact. 32 ops decoded through the virtual->real ID translation (HostState vmaps). Renderer is Mali-G615 MC2, not software. **Real GLES shaders + VBO + texture + draw run on the GPU through the marshalling decoder.**

## M2 — same op stream through the SPSC command ring, then Mali (PASS)
```
alr gpu ring probe=spsc-ring-decode-draw
ALR GPU RING TRANSPORT: PASS
ALR GPU RING DECODE+EXECUTE: PASS
ALR GPU RING HARDWARE RENDER: PASS
bytes pushed=739 drained=739   ops decoded=32   center pixel=0,220,0
renderer=Mali-G615 MC2   software renderer=false
```
The same 739-byte stream is pushed through the `alr::gpu::RingProducer` into the shared-memory SPSC ring (alr_gpu_ring.hpp), drained by `RingConsumer` (snapshot+linearize a wrap), and decoded on Mali. 739 pushed == 739 drained (zero loss), same pixel-correct triangle. Proves the ring transport carries a real GL command batch intact end to end. (The `sync reply_seq>=req=no` line is benign: in this same-thread self-test the consumer posts the reply after the producer's spin-1 check; the device fork model has a host thread that replies before the producer wakes.)

## What this establishes
The host decoder + virtual-ID model + ring transport — the entire host half of the gfxstream-style GLES marshalling layer — runs on real Mali hardware. The remaining work is the GUEST half (M3: a glibc libEGL/libGLESv2 shim that emits this op stream; a spinning-cube client linking it) and the device wiring (M4: a host GPU thread that creates the ring, the loader forks the guest with the ring fd inherited, render into an AHB and DISPLAY the cube via the v114 external-OES present path). The crux — client-side virtual GL IDs so object creation never blocks on a round-trip — is implemented and exercised (32 ops including 5 object creations decoded with no round-trip).

## No regression
267 host tests pass (incl. the SPSC ring integrity test); native-core PASS; GIMP still boots and is usable; the GPU probes are additive startup self-tests (logged to tag `alr_loader`). Concurrent note: the PC-gate seccomp workstream is also live and healthy in the same build (`pcgate=1 interpose=1 traps=0 rewrites=0` observed — PC-gating drove path-mediation traps to zero); no file overlap with the GPU track (alr_gpu/ headers vs runtime_report.cpp loader/interposer).

## Milestone status (Phase 4, GPU-native track)
- M1 decode+draw on Mali — DONE (device, this doc)
- M2 ring transport + decode on Mali — DONE (device, this doc)
- M3 glibc libEGL/libGLESv2 shim + alr-gles-cube client — in progress (source being written off-tree)
- M4 render into AHB + DISPLAY the spinning cube on the SurfaceView — pending
- M5 glmark2-es2 — pending
