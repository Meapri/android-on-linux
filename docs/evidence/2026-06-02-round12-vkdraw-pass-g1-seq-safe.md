# Device Evidence — round-12 (v145): VK DRAW on real Mali ✓ + inproc sequence-safe (re-mapped-guest exec still pending)

Build `0.4.145-r12-v145`. Device SM-X236N / Mali-G615 MC2 / Android 16. Cold-start drain of the R12 workflow merge (6 lanes: g1-seqint, g3-vk, g5-gles3, g4-input, apt-fakeroot, docs) + WS-1 VK-draw JNI wiring. inproc=on for this drain (then gated default-OFF).

## ★ G3 — VK render BREADTH: a real graphics-pipeline DRAW on real Mali ✓
```
ALR VK DRAW MARSHAL: PASS
alr vk draw submit result=VK_SUCCESS   render result=0 (OK)
alr vk draw center px=242,26,204,255  (expect ~242,26,204 triangle)
```
The marshalled VK path now does a full graphics pipeline — guest vkCreateShaderModule(x2, embedded SPIR-V) + vkCreatePipelineLayout/RenderPass/GraphicsPipelines + a HOST_VISIBLE vertex buffer + vkCmdBeginRenderPass/BindPipeline/BindVertexBuffers/**vkCmdDraw(3)**/EndRenderPass — to the **real Mali libvulkan**, and the AHB center pixel reads back **242,26,204 = the triangle's baked color** (deliberately far from the ~black clear bg), proving a real `vkCmdDraw` (not a clear) executed on hardware. Beyond enumerate (round-5) + clear-submit (round-7), the guest→host Vulkan path now does a **textured/geometry draw** on Mali — the breadth step toward VK-M3 (guest ICD).

## ★ G1 — inproc re-map is now sequence-SAFE (the stall is gone)
With `ALR_REEXEC_INPROC=1`, the full onCreate probe sequence **completes** — every later probe runs:
```
ALR GPU LIVE INTEGRATION: PASS   ALR VK RENDER MARSHAL: PASS   ALR GPU SCREEN CUBE: PASS
foot/netsurf/qt6 rendered=true   glmark2 Score 1090
alr exec ... inproc=on inproc_redirected=1 inproc_skipped=1
```
The ~9 s stall seen in v144 was the inline chromium `--dump-dom` probe hanging the serialized supervision (reverted in R11) — NOT inproc itself. The R12-g1 scoping (skip `/proc/self/exe`/`/proc/*`, non-rootfs targets, and the stub) fired (`inproc_skipped=1`), and the redirect fired for a rootfs glibc target (`inproc_redirected=1`). So inproc-on no longer breaks the sequence (regression-safe).

## ⚠️ Remaining (G1): the re-mapped static guest still crashes during its own startup
The re-mapped `/bin/sh` (static ET_EXEC, span 0x1e9000, mapped 0x400000) maps + jumps with healthy diag (`hwcap=0x119fff`, per-segment flags shown), but the guest then faults — `signal 11 (SIGSEGV)` (pid 19412) and `signal 4` (foot's re-mapped slave shell). The R11 single-span/no-boundary-clobber fix was necessary but NOT sufficient for this full 1.9 MB static glibc binary; there is a further static-startup bug (candidates: data-segment/relro layout for a multi-PT_LOAD ET_EXEC, GNU_RELRO mprotect, or a glibc static-init expectation). **Contained**: the app survives and the whole sequence completes (no-regression). So inproc provides no functional gain yet (the re-mapped guest doesn't run) → kept gated **default-OFF** (`ALR_REEXEC_INPROC=1` opt-in).

## R12 lane status
- **g3-vk**: DEVICE-VERIFIED (VK DRAW PASS, triangle pixel). ✓
- **g1-seqint**: inproc sequence-safe (scoping fires); re-mapped-guest execution still crashes → G1 not promoted.
- **g5-gles3** (constant vertex-attrib wire ops 84/85), **g4-input** (right-Alt modifier fix + momentary-mod clear), **apt-fakeroot** (LD_PRELOAD fakeroot shim + builder): host-merged + host-gated; device-verify pending (g5 needs gpushim re-stage + harder glmark2 scenes; g4 needs interactive input; apt-fakeroot needs the fakeroot+apt-dpkg overlays staged for a dedicated dpkg-unpack drain).

## No regression ✓
GPU LIVE + VK RENDER + VK DRAW + SCREEN CUBE PASS, foot/netsurf/qt6 rendered, glmark2 1090, no app crash. The signal 4/11 are contained re-mapped-guest child faults (inproc opt-in), not app regressions.

## Verdict
R12 lands **VK DRAW on real Mali** (G3 breadth) and makes **inproc-on sequence-safe** (G1 scoping). The last G1 mile = make the re-mapped static guest actually execute (the SEGV/SIGILL in glibc static startup) — a focused mapper/startup debug on a foundation that is otherwise fully device-proven.
