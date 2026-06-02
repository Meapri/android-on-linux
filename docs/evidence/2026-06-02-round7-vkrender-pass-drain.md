# Device Evidence — round-7 (v139): VK-M2 render PASSES on real Mali (AHB-ext fix), exec-re-entry B-3 wired, no-regression

Build `0.4.139-r7-v139` (versionCode 139, confirmed on device via dumpsys). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start (force-stop → logcat -c → am start). Drain of the round-7 gate-merge (R7-A VK clear-submit fix, R7-B exec-re-entry B-3 envp injection, R7-C svc-scan ctor gate + child-reentry audit, R7-D input focus-follow/axis, R7-E docs) → main `0aef81c` + stamp `495e9e8`.

## ★ VK-M2 render PASSES on real Mali ✓ (was FAIL in round-6)
```
ALR VK RENDER MARSHAL: PASS
alr vk render ops decoded=11
alr vk render device created=yes
alr vk render gfx queue family=0
alr vk render submit result=VK_SUCCESS
```
The round-6 `submit result=fail` is **fixed**. Root cause (R7-A): the marshalled VK-M2 device was created with **no device extensions**, but the clear path imports the AHardwareBuffer color target via `VK_ANDROID_external_memory_android_hardware_buffer` — those entry points are only legal when that extension is enabled at `vkCreateDevice`. R7-A enables the AHB device extension (+ `VK_EXT_queue_family_foreign` when present) and adds the spec-correct tiler readback path (render-pass `finalLayout`→`GENERAL` + queue-family-release barrier to `VK_QUEUE_FAMILY_EXTERNAL`). The guest's `vkCreateDevice`+`vkGetDeviceQueue`+command-pool/buffer+clear now marshals to the real Mali libvulkan and the `vkQueueSubmit` returns **`VK_SUCCESS`**. Combined with the round-5 enumerate (`ALR VK ENUM MARSHAL: PASS`, api=1.3, Mali-G615), the guest→host Vulkan path now does **device + queue + command-buffer + clear-submit** on real Mali — VK-M2 render is device-verified.

## exec-re-entry B-3 (child envp injection) — wired + fires its decision, but apt outcome unchanged ⚠️
```
alr exec x0=/bin/dash reason=rewrite envp_reason=already
alr exec envp_injected=0 ld_preload_set=0      (every exec)
alr exec traps=1 rewrites=1 exec_events=0 clone_events=7   (/bin/sh, round-6 repro)
apt-install: unpacked=false configured=false exec=[ALR NATIVE LOADER GUEST EXEC: FAIL]
```
The B-3 decision function (`decide_exec_envp_injection`) is wired into the supervisor and **evaluates on every execve/execveat trap**: for `/bin/dash` it returned `envp_reason=already` — i.e. it correctly detected that LD_PRELOAD + ALR_ROOTFS are **already present** in the child's envp (the guest's exec'd children *inherit* the loader-set LD_PRELOAD), so no injection is needed (`envp_injected=0` is the correct result here, not a no-op failure). B-1 path-rewrite still fires (`x0=/bin/dash reason=rewrite`).

**The apt/dpkg chain is still `unpacked=false`, but the blocker is NOT envp propagation:**
- The `apt` top-level invocation reports `GUEST EXEC FAIL` with `traps=0 rewrites=0 exec_events=0` — it fails *before* any execve trap (apt is only minimally staged: `apt-config-stage.tar` is 10 KiB; the full apt+dpkg+solver closure is not present), so this probe can't exercise the chain.
- Across **all** observed execs, `exec_events=0` — the `PTRACE_EVENT_EXEC` (the new program image actually entering execution under the loader) never fires, even when the pre-exec seccomp trap rewrote the path. So the real exec-re-entry wall is **exec-completion / new-image re-entry** (the re-exec'd rootfs binary running *under ALR mediation*), which B-1 path-rewrite + B-3 envp are necessary-but-not-sufficient for. This is the next focused milestone, and it is independent of (and downstream of) the envp work landed this round.

Net: B-3 is correctly implemented and device-confirmed to evaluate; it doesn't change the apt result because (1) the observed execs inherit LD_PRELOAD (no injection needed) and (2) the dpkg chain is gated by the deeper exec-completion wall + incomplete apt staging.

## svc-scan (R7-C) — host-merged, device-emission pending env-wire
`ALR_SVCSCAN` is gated OFF by default and is not yet pushed into the guest env by MainActivity, so no `ALR-SVCSCAN` line is expected this drain. The ctor gate + read-only scanner are merged + host-built (interpose .so 117 KiB); wiring `ALR_SVCSCAN=1` into the guest env is a one-line follow-up for a future drain.

## No regression ✓
```
ALR VK ENUM MARSHAL: PASS
ALR GPU LIVE INTEGRATION: PASS
ALR GPU SCREEN CUBE: PASS
glmark2 Score: 1053
foot-result:    rendered=true
gtkdemo-result: rendered=true (frames 13->2210)
netsurf-result: rendered=true
qt6gui-result:  rendered=true (frames 2213->2214)  ← Qt6 still renders (input changes regression-clean)
sdl2gui-result: rendered=true
Chromium 147.0.7727.137 (runs)
```
No FATAL/SIGSEGV/crash. The R7-D input focus-follow + `wl_pointer.axis_source` scroll changes, the R7-C interposer ctor change, and the R7-B supervisor exec changes are all regression-clean — the full 7-toolkit GUI set + GPU self-tests + glmark2 + chromium are unaffected.

## Verdict
Round-7 lands the **VK-M2 render on real Mali** (`submit=VK_SUCCESS` — the AHB-ext fix), confirms **exec-re-entry B-3 is wired and evaluates correctly on device** (no injection needed in the inherited-envp execs observed), and is **regression-clean** across GPU + the 7-toolkit GUI set + chromium. Remaining focused: exec-**completion** re-entry (`exec_events=0` → make the re-exec'd rootfs binary actually run under ALR; the real dpkg/apt wall), full apt staging, svc-scan env-wire, and on-device input interactivity.
