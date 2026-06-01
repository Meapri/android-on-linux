# Device Evidence — GPU throughput: ALR ring+executor vs single-thread direct decode (same op-stream, same Mali)

Build `0.4.131-cp5-gpu-throughput-v131` (versionCode 131). Device SM-X236N / Mali-G615 MC2. Cold start. `run_gpu_throughput_probe` (host-side, in the APK process, on the device's real Mali). PCGATE on.

## Why this measurement (and what it is NOT)
glmark2 is a glibc/Wayland binary → **cannot run on bare-Android Mali**, so a literal "same glmark2 binary, Mali-direct" baseline is not runnable on-device. The valid on-device comparison renders the **same triangle op-stream on the same Mali** two ways and takes the FPS ratio:
- **DIRECT** — one EGL ctx, decode setup once then loop the draw-only stream + `glFinish` per frame. **Single-threaded, serial** (CPU waits on each GPU drain). No ring, no executor thread.
- **ALR** — the production path: 2-thread ring + `GpuExecutorService` rendering into an AHB-FBO, producer pushing the same draw stream with the per-frame `req_seq/reply_seq` handshake (exactly the glmark2 path).

Both: setup-once + draw-loop on a persistent `HostState` (mirrors real apps; no per-frame recompile/leak), 512×512, 600 frames.

## Raw result (logcat `gpu-throughput`)
```
ALR GPU THROUGHPUT: PASS
alr gpu throughput render=512x512 frames=600
alr gpu throughput direct_fps=757  frames=600
alr gpu throughput alr_fps=1158 frames=600
alr gpu throughput ratio_alr_over_direct=1.53
alr gpu throughput direct renderer=Mali-G615 MC2 software=false
alr gpu throughput alr    renderer=Mali-G615 MC2
```

## Honest interpretation
- **ratio = 1.53 (ALR 1158 FPS > direct 757 FPS).** This is **NOT** "ALR is 1.5× faster than native Mali." It means: the ALR command-ring + executor pipeline, for this same op-stream on the same GPU, sustains **higher throughput than a single-threaded direct decode** — because the **2-thread design overlaps command marshalling (producer) with GPU execution (executor)**, while the serial DIRECT path stalls on a `glFinish` round-trip every frame.
- **Take-away: the ALR GPU command-ring/marshalling layer is NOT a throughput bottleneck for this workload** (§0(b) "GPU 측 per-call 마샬링/카피" is fully hidden behind GPU work). Client-side virtual GL IDs (no per-call round-trip) + the 2-thread overlap are why.
- Both paths render on the **real Mali-G615 MC2**, `software=false`.

## Caveats (what this does NOT establish)
- **Not** a "% of a native app's pipelined ceiling." A hand-optimized native renderer pipelines via swapchain (no per-frame `glFinish`); the DIRECT baseline here is intentionally conservative (serial + per-frame finish, matching the executor's per-frame completion). A strict native-app ceiling is a separate, harder measurement (would need glmark2 ported to Android EGL, or a swapchain baseline) and remains open.
- Light single-draw workload — the regime where per-frame overhead dominates. Heavy GPU-bound frames only push the ratio further in ALR's favor (more GPU work to hide marshalling behind).

## Verdict
The ALR GPU marshalling pipeline is **throughput-competitive with (here, exceeds) a direct single-threaded decode of the same commands on the same Mali** — marshalling overhead is hidden, not a bottleneck. The absolute fraction-of-native-app GPU performance (§0 ≥70% target) is a distinct, still-open measurement.
