# Device Evidence — round-5 (v136): guest Vulkan enumerate marshalled to real Mali (VK 1.3); qt6 EGL gap

Build `0.4.136-r5-v136` (versionCode 136). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. Drain of the round-5 merges (GLES glPixelStorei row-align + glGetActiveUniform/Attrib, coordinate hit-test input routing + wl_touch grab/cancel, qt6 closure+machine-id, gui-universality/loader-gaps docs) + integration: Vulkan device probe wired (ALR_VK_DECODE_REAL + nativeAlrGpuVkMarshalProbe → run_vk_marshal_mali_probe) + QT_WAYLAND_DISABLE_WINDOWDECORATION. → main `28c4880`.

## Vulkan enumerate/props marshalling — DEVICE-VERIFIED on real Mali ✓ (landmark)
```
ALR VK ENUM MARSHAL: PASS
mode=mali-libvulkan
request bytes=33  reply bytes=82   transport=ok (req drained=33 rep drained=82)
ops decoded=4
instance result=VK_SUCCESS
device count=1  vphys_base=100
renderer=Mali-G615 MC2
api=1.3
vendorID=0x13b5   (ARM)
```
A guest-libvulkan-style **vkCreateInstance / vkEnumeratePhysicalDevices / vkGetPhysicalDeviceProperties** request stream is encoded by the guest, pushed through the SPSC ring, **decoded on the host by the real vendor Mali libvulkan**, the result encoded back as a reply stream, and decoded by the guest. `VK_SUCCESS`, the real **Mali-G615 MC2** device, **Vulkan API 1.3**, ARM vendor ID — all flowed through the marshalling path. The guest→ring→host Vulkan backbone (the GLES ring's Vulkan twin) works **end-to-end on hardware**. Mali-G615 supports **Vulkan 1.3** — the foundation for the Vulkan-first guest render pipeline ([[guest-gpu-accel-strategy]]).

## No regression ✓
GLES3 shim row-align fix + input-routing + Vulkan wiring did not regress anything:
`ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU THROUGHPUT: PASS` + `ALR GPU SCREEN CUBE: PASS`, `glmark2 Score 1083`, `netsurf-result rendered=true`, `foot rendered=true`, `sdl2gui-result rendered=true`.

## qt6 analogclock — still crashes (EGL hwintegration, not files) ✗
```
qt6gui-result: rendered=false frames=2217->2217   F/DEBUG (pid 24125) signal 11 SIGSEGV
```
machine-id injection + QT_WAYLAND_DISABLE_WINDOWDECORATION did NOT fix it. The qt6 overlay closure is complete (35 reachable libs incl `libQt6WaylandEglClientHwIntegration.so`); the crash is the **forked guest child (pid 24125)** during Qt init — consistent with Qt selecting the wayland **EGL** client-buffer integration and calling `eglGetDisplay` with no ICD (the ALR shim is GLES-marshalling, not a Qt-usable EGL platform). App survived (ran all later probes). Focused next step: force Qt's wl_shm buffer integration / disable the EGL hwintegration plugin (env or overlay), or provide an EGL platform Qt accepts. Documented in docs/research/loader-feature-gaps.md (G2).

## Verdict
Round-5 device-verifies the **Vulkan enumerate marshalling path on real Mali (VK 1.3)** — a landmark toward guest Vulkan acceleration — with zero regression. qt6 remains a focused EGL-integration fix; SDL2/netsurf/foot/GIMP/gtk3 GUI set intact. Remaining: qt6 EGL, Vulkan render pipeline (VK-M2 body + ICD), exec-re-entry (apt/dpkg/GIMP plugins).
