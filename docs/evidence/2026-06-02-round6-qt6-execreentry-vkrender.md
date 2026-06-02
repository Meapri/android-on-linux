# Device Evidence — round-6 (v138): Qt6 renders (EGL→SHM), exec-re-entry path-rewrite, VK render device-created

Build `0.4.138-r6-v138` (versionCode 138). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. Drain of the round-6 merges (exec-re-entry ADR-003, qt6 EGL-plugin exclusion, VK-M2 render body, svc-scanner, compositor popup-grab scoping, CP-6 docs) + integration (VK render JNI wiring, `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`). → main `8b87a95`.

## Qt6 analogclock RENDERS via ALR ✓ (new — EGL→SHM fix worked)
```
qt6gui-result: rendered=true frames=2215->2216  bin=.../qt6/examples/widgets/widgets/analogclock/analogclock
sdl2gui-result: rendered=true frames=2216->2217
```
The round-4/5 qt6 SIGSEGV (Qt selecting the wayland **EGL** client-buffer integration → `eglGetDisplay` with no ICD) is **fixed**: the r6 overlay excludes the EGL QPA platform plugin + the compositor-server HwIntegration plugins, and the integration adds `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`. Qt6 now uses the **wl_shm backing store** and the analogclock window **renders** to the SurfaceView (frame advanced, no crash). **Qt6 joins the device-proven universal-GUI set** (GIMP / gtk3-widget-factory / gtk3-demo / foot / netsurf / SDL2 / **Qt6**).

## exec-re-entry (ADR-003 B-1) fires on device ✓
```
alr exec traps=1 rewrites=1 exec_events=0 clone_events=7
alr exec x0=/bin/sh reason=rewrite
```
A guest's `execve(/bin/sh)` was trapped at EVENT_SECCOMP and its **program path (x0) rewritten** into the rootfs (argv/envp untouched), and the guest forked (7 clones). The ADR-003 B-1 (execve path-rewrite) path works on hardware. No-exec guests show `traps=0 rewrites=0 exec_events=0` (no regression). **Still incomplete**: `apt-install: unpacked=false` — dpkg's full fork+exec chain (maintainer scripts) needs the B-3 interposer re-injection into the exec'd child (depends on the launcher propagating LD_PRELOAD/ALR_ROOTFS to the child envp — device-only, not yet).

## VK-M2 render — device created on Mali, clear-submit fails ⚠️
```
ALR VK RENDER MARSHAL: FAIL
mode=mali-libvulkan  ops decoded=11  transport=ok
device created=yes  gfx queue family=0  submit result=fail
```
The VK-M2 body marshals guest `vkCreateDevice` + `vkGetDeviceQueue` + command-pool/buffer to the **real Mali libvulkan** — `device created=yes`, queue family resolved. The `vkQueueSubmit` of the clear into the AHB-backed color attachment **fails** (render-pass / image-layout / AHB-import setup needs debugging). The enumerate path is unaffected (`ALR VK ENUM MARSHAL: PASS`). So the device/queue/command marshalling is proven on Mali; the clear-render submit is the next fix.

## No regression ✓
`ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU SCREEN CUBE: PASS`, `glmark2 Score 1061`, `foot/gtkdemo/netsurf/sdl2 rendered=true`, chromium `--version` still runs (Chromium 147.0.7727.137). No FATAL/crash. The compositor popup-grab scoping + exec-re-entry + qt env changes are regression-clean.

## Verdict
Round-6 lands **Qt6 rendering** (the EGL→SHM fix), **exec-re-entry path-rewrite on device** (ADR-003 B-1), and the **VK-M2 device/queue marshalling on real Mali** (clear-submit pending). Remaining focused: VK clear-submit fix, exec-re-entry B-3 (child re-injection → dpkg/apt + multiprocess), the `--dump-dom` storm deadlock (then M-R5/M-R1 A/B), svc-scan wiring.
