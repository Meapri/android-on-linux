# Device Evidence — round-4 milestones (v135): SDL2 window renders; qt6/apt honest gaps

Build `0.4.135-r4-milestones-v135` (versionCode 135). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. Drain of the round-4 merges (Vulkan enumerate marshalling, nested-submenu present + UAF guard, qt6/sdl2 GUI demos, launch probes, GUI-universality SSOT) + integration QT_QPA_PLATFORM=wayland. → main `1d2fb2c`.

## SDL2 GUI window RENDERS via ALR ✓ (new)
```
sdl2gui-result: rendered=true frames=2217->2218  bin=/usr/libexec/installed-tests/SDL2/testdraw2
sdl2-stage: overlay done (extracted=120 skipped=0)
```
An **SDL2 window demo (testdraw2)** runs via the ALR loader and renders to the SurfaceView (frame advanced, `SDL_VIDEODRIVER=wayland`). SDL2 joins the device-proven GUI set (GIMP / gtk3-widget-factory / gtk3-demo / foot / netsurf-gtk / **SDL2**).

## qt6 analogclock — crashes in Qt init (guest child, NOT app) ✗
```
qt6gui-result: rendered=false frames=2217->2217  bin=.../qt6/examples/widgets/widgets/analogclock/analogclock
qt6-stage: overlay done (extracted=289 skipped=0)
F/DEBUG (pid 21297): signal 11 (SIGSEGV), code 1 (SEGV_MAPERR)
```
The analogclock binary is present (staged, QT_QPA_PLATFORM=wayland set) but the **forked guest child (pid 21297) SIGSEGVs during Qt init** → rendered=false. **NOT an app regression**: the app process (pid 21224) stayed alive and ran every later probe (all result markers logged at 10:43:08–18, after the 10:41:55 crash). Qt needs a more complete plugin/dependency closure (the 105MB overlay + wayland QPA plugin is insufficient for full Qt platform init). Next: complete the Qt6 wayland closure (libQt6WaylandClient + integration plugins + their deps).

## gimp babl/gegl filter — partial
`gimp-filter: via=gimp-console-batch ok=true` (exec=FAIL is the exit/SIGALRM code). gimp-console batch ran and the babl/gegl op modules (0755) loaded (ok marker matched). Full filter-output verification pending.

## apt/dpkg install — blocked on exec-re-entry (expected)
`apt-install: unpacked=false configured=false exec=GUEST EXEC FAIL`. `dpkg -i` forks helper processes (tar/maintainer scripts) → the loader's **exec-re-entry** (execve of a rootfs binary re-entering the ALR loader) is the gating feature, not yet implemented. Documented honestly; this is a known large loader milestone, not a quick fix.

## Vulkan — host-merged, device exercise deferred
The guest Vulkan enumerate/props marshalling backbone (alr_gpu_vk_{proto,decode,marshal_probe}) is merged + host-verified (native_vk_marshal_test ALL PASS, 4-ABI clean). Device exercise (a JNI `run_vk_marshal_mali_probe` next to nativeHostVulkanProbe → `ALR VK ENUM MARSHAL: PASS` on real Mali) is a one-line follow-up, not wired this drain.

## No regression ✓
`netsurf-result rendered=true (2214→2217)`, `foot rendered=true`, `gtkdemo rendered=true`, `glmark2 Score 1047`, `ALR GPU SCREEN CUBE: PASS`. The app survived the qt6 guest crash and completed the full probe sequence.

## Verdict
Round-4 added an **SDL2 GUI window** to the proven set and a host-verified **Vulkan marshalling backbone**, with honest gaps surfaced: qt6 needs a fuller wayland closure, apt/dpkg needs exec-re-entry. No app regression. Remaining milestones: complete Qt6 closure, exec-re-entry (unlocks dpkg/apt + GIMP plugins), Vulkan device wiring + render pipeline, GLES3+/full glmark2.
