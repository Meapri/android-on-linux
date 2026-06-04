# Chromium native-Vulkan compositing on ALR → external VK loader+ICD → Mali

**Goal.** Make Chromium 147 composite with its **native Vulkan backend** (Viz +
SkiaRenderer on Vulkan, `GrVkBackendContext`) talking Vulkan **directly** to the
system Vulkan loader → **our guest VK ICD** → Mali — **bypassing ANGLE entirely**.

**Why this is distinct from the banked ANGLE path.** The banked path is
Chromium → ANGLE (GL→Vulkan, `RendererVk`) → our ICD. It walls inside ANGLE's
`RendererVk` per-format-texture NULL-helper deref on the first `glTexImage2D`
(vendor-invariant, unfixable without ARM dbgsym — see
`angle-vulkan-collision-fixed.md`, the wave-15..18 GPU-ANGLE evidence). The
native-Vulkan path **never enters ANGLE's `RendererVk` texture path**: Viz builds
a `GrVkBackendContext` from a `VkDevice` it creates itself via the loader, and
SkiaRenderer records Vulkan command buffers straight onto it. So the ANGLE wall is
structurally out of the picture; whatever walls here walls somewhere else (our ICD,
or Chromium-internal), which is the experiment's job to localize.

**Status of the substrate (all PROVEN on device):**
- Chromium 147.0.7727.137 RUNS on ALR today (`runChromiumStandalone`, software/CPU
  compositing, `--disable-gpu`). See `cp6-status.md`, `chromium-window-demo-plan.md`.
- Our DIRECT-VK ICD renders + presents a guest VK triangle on Mali through the AHB
  swapchain (ALR VK DRAW MARSHAL / ICD SERVICE / ICD PRESENT PASS). The loader
  already wires `ALR_VK_ICD=1` end-to-end: VK rings, `VK_DRIVER_FILES`/
  `VK_ICD_FILENAMES` → `alr_icd.json` → `libalr_mali_icd.so`, present sink →
  compositor. See `gpu-guest-accel-strategy.md`, `alr_gpu/guest_icd/`.

This doc = (1) the exact flag set, (2) the marker-gated wiring, (3) the ICD-gap
list Chromium-Vulkan needs that our ICD does not implement yet, (4) the precise
device experiment.

---

## 1. The exact Chromium-147 command line (native Vulkan via external ICD)

Newline-delimited argv tokens (the form `nativeAlrNativeLoaderProbe` consumes; each
`\n` is one argv element). These ADD TO / REPLACE the software-raster tail of the
default `runChromiumStandalone` launch; everything before `--disable-gpu` (the
ozone-wayland window + the sandbox-disable block + profile/networking flags) is
unchanged.

### 1.1 Turn the GPU + Vulkan ON (replaces the `--disable-gpu` software tail)

```
--use-vulkan=native
--enable-features=Vulkan,DefaultEnableOopRasterization,VulkanFromANGLE:disabled
--enable-gpu-rasterization
--enable-unsafe-webgpu        # (optional; only if WebGPU is exercised — off by default here)
--ignore-gpu-blocklist
--disable-gpu-driver-bug-workarounds
--use-gl=disabled             # do NOT bring up a GL/ANGLE display at all
--disable-vulkan-fallback-to-gl-for-testing
--disable-vulkan-surface      # headless/offscreen Viz: composite to an offscreen VkImage, not a VkSurfaceKHR swapchain  (see §3 — this is the load-bearing flag)
```

Then **remove** these four software-path flags that are on the default launch:
`--disable-gpu`, `--in-process-gpu` (kept — see note), `--disable-gpu-compositing`,
and do NOT pass `--disable-gpu-rasterization`.

**Citations (Chromium 147 source — names are stable across 130..147):**
- `--use-vulkan` with values `native` / `swiftshader` / `disabled`:
  `gpu/config/gpu_switches.cc` → `switches::kUseVulkan`, parsed in
  `gpu/config/gpu_finch_features.cc::UseVulkan()` into
  `gpu::VulkanImplementationName::{kNative,kSwiftshader,kForcedNative,kNone}`.
  `=native` selects the **system Vulkan loader** (`libvulkan.so.1`), i.e. our
  external loader+ICD. `=swiftshader` loads Chromium's bundled
  `libvk_swiftshader.so` — the A/B control (§4).
- `features::kVulkan` (`--enable-features=Vulkan`): `gpu/config/gpu_finch_features.cc`.
  Git gates the whole Viz-on-Vulkan path; without it `--use-vulkan` is ignored.
- `features::kDefaultEnableOopRasterization`: `gpu/config/gpu_finch_features.cc` —
  routes raster to the GPU (Skia) instead of the software raster worker.
- `switches::kEnableGpuRasterization`: `gpu/config/gpu_switches.cc`.
- `switches::kIgnoreGpuBlocklist`: `gpu/config/gpu_switches.cc`. **Required** — the
  Mali entry in `gpu/config/software_rendering_list.json` /
  `gpu_driver_bug_list.json` blocklists/virtualizes Mali; on the GL path this forces
  virtualization (the memory note), and on the Vulkan path the blocklist can mark
  Vulkan UNSUPPORTED for Mali → silent demote to GL/software. This flag clears the
  whole blocklist so Vulkan is allowed.
- `switches::kDisableGpuDriverBugWorkarounds`: `gpu/config/gpu_switches.cc` — stops
  Chromium auto-applying Mali driver-bug workarounds derived from the blocklist
  (some of which disable Vulkan features our ICD does back).
- `switches::kUseGL` = `"disabled"` (`ui/gl/gl_switches.cc` →
  `gl::kGLImplementationDisabledName`): commit to Vulkan; never initialize a GL/ANGLE
  display. (If `--use-gl=angle` is left on, Chromium's GpuInit will still create an
  ANGLE display for the non-composited GL contexts and can fall back into it — we want
  zero ANGLE.)
- `switches::kDisableVulkanFallbackToGLForTesting`
  (`gpu/config/gpu_switches.cc`): if Vulkan init fails, do NOT silently fall back to
  GL — **fail visibly** so the experiment sees the Vulkan wall instead of a silent GL
  demote. (Pairs with `--use-gl=disabled`.)
- `switches::kDisableVulkanSurface` (`gpu/config/gpu_switches.cc` →
  `switches::kDisableVulkanSurface`): tells Viz to run Vulkan **surfaceless** — it
  composites into an offscreen `VkImage` and hands the result to the platform
  presenter (Ozone) rather than owning a `VkSurfaceKHR` swapchain. This is the
  load-bearing flag for us (see §3): our ICD has an AHB swapchain but **no WSI
  surface-query family** — surfaceless Viz sidesteps the entire `VkSurfaceKHR` path.

### 1.2 Keep (unchanged from the default launch)

`--ozone-platform=wayland`, the full sandbox-disable block
(`--no-sandbox --disable-seccomp-filter-sandbox --disable-setuid-sandbox
--disable-namespace-sandbox --disable-gpu-sandbox`), `--single-process --no-zygote`,
`--disable-dev-shm-usage --user-data-dir=… --no-first-run
--no-default-browser-check --disable-crash-reporter --disable-breakpad`, the
background-networking kills, the window-size/screen-size, `--enable-logging=stderr
--v=1`, and the `data:text/html` page.

**Note on `--single-process` + `--in-process-gpu`.** Keep BOTH. `--single-process`
collapses renderer + GPU + utility into the ONE browser process (the loader needs
this — it stops the per-child ~258 MiB re-map storm, see `runChromiumStandalone`
comments). With single-process the **Viz/GPU thread runs in the launch process**, so
our `ALR_VK_ICD` env (set on the launch process) covers it directly — there is **no
GPU child** to inject VK flags into (the loader's `decide_chromium_child_argv` only
re-injects *sandbox* flags into forked children; with single-process there is no GPU
child fork, so that path is moot for the VK env). This is why the Vulkan env can be a
pure host-env + launch-argv change with **zero loader edits**.

### 1.3 Add GPU diagnostics so the experiment can read the decision

```
--enable-logging=stderr --v=1
--vmodule=*vulkan*=2,*gpu_init*=2,*viz*=1,*skia*=1,gpu_data_manager*=2
```
(`--vmodule` raises the log level for just the Vulkan/GpuInit/Viz/Skia TUs;
`gpu_data_manager*` logs the blocklist decision.) Plus our ICD trace: the loader
defaults `ALR_ICD_DIAG=1` only under `ALR_ANGLE`; on this NON-ANGLE path we set the
host env `ALR_ICD_DIAG=1` explicitly so `[alr-icd]` logs every vk\* call the Viz
context makes and where it stops. `VK_LOADER_DEBUG=all` lights up the Khronos
loader's ICD discovery.

---

## 2. The env (reuses the proven ANGLE VK-stack staging — minus ANGLE)

The loader (`runtime_report.cpp`, **not edited** by this task) already does the whole
VK-ICD wiring when it sees `ALR_VK_ICD=1` in the host env:
- prepends `…/usr/lib/androlinux` to the guest `LD_LIBRARY_PATH` (where the Khronos
  loader `libvulkan.so.1` + our `libalr_mali_icd.so` + `alr_icd.json` live);
- attaches the VK request/reply rings and installs the present sink → compositor;
- pushes `VK_DRIVER_FILES=…/alr_icd.json` and `VK_ICD_FILENAMES=…/alr_icd.json` into
  the guest env (the modern loader's authoritative driver-manifest selector).

So the marker path sets these **host** env vars before
`nativeAlrNativeLoaderProbe`, then lets the loader do the rest:

| host env | value | effect |
|---|---|---|
| `ALR_VK_ICD` | `1` | loader: VK rings + `androlinux` on path + `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` → our ICD |
| `ALR_GPU_ACCEL` | `1` | loader: GPU ring attach + `androlinux` shim dir on path (Chromium's Vulkan needs the ring; harmless to GLES shim) |
| `ALR_ICD_DIAG` | `1` | our ICD emits `[alr-icd]` per-entrypoint trace (default-on only under ANGLE; we force it) |
| `VK_LOADER_DEBUG` | `all` | Khronos loader ICD-discovery trace |
| `ALR_PERSIST_GUEST` | `1` | (already set) no lifetime cap / watchdog kill |
| `ALR_REEXEC_INPROC` | `1` | (already set) in-process re-map of self-exec children |

**Crucially `ALR_ANGLE` is NOT set.** Setting it would prepend the
`androlinux-angle` dir (ANGLE's `libEGL/libGLESv2`) and unset `DISPLAY` /
set `XDG_SESSION_TYPE=wayland`. We don't want ANGLE on the path at all (Chromium
talks Vulkan directly), and `--ozone-platform=wayland` already binds Chromium to the
in-app compositor via `WAYLAND_DISPLAY` regardless of `DISPLAY`. So the direct-VK
path uses `ALR_VK_ICD` **without** `ALR_ANGLE` — the first time these are decoupled
(every prior VK-ICD device run rode the ANGLE gate).

**Overlays required on device** (same tars the ANGLE path stages; already extracted
on the cronly path's `for (name in …)` loop which includes `"vk-icd","vk-loader"`):
- `vk-loader-stage.tar` → `…/usr/lib/androlinux/libvulkan.so.1` (Khronos loader) +
  `alr_icd.json` (manifest → `libalr_mali_icd.so`).
- `vk-icd-stage.tar` → `…/usr/lib/androlinux/libalr_mali_icd.so` (our ICD,
  `alr_gpu/guest_icd/` built by `tools/build_vk_icd_overlay.py`).

Both are ALREADY in the cronly staging list (`runChromiumStandalone` line ~2329), so
no new staging is needed — the marker path only flips the env + swaps the launch-argv
tail.

---

## 3. ICD-gap analysis — what Chromium-Vulkan needs that our ICD lacks

Chromium's Viz Vulkan context (`gpu::VulkanImplementation` →
`gpu::VulkanDeviceQueue` → Skia `GrVkBackendContext`) has a HARDER requirement set
than ANGLE's `RendererVk`. The two big axes are **(A) WSI/surface** and **(B)
external-memory/semaphore interop + sync2**. Inventory below is from
`alr_gpu/guest_icd/alr_icd_vulkan.c` (the GIPA/GDPA table), the generated band
(`alr_gpu/generated/alr_gpu_vk_gen_icd.inc`), and
`alr_icd_cmd_entrypoints.inc` (the vkCmd\* recorder).

### 3.A Implemented today (what the ICD DOES back)

- **Global/instance:** `vkEnumerateInstanceVersion`,
  `vkEnumerateInstance{Extension,Layer}Properties`, `vkCreateInstance`,
  `vkDestroyInstance`, `vkGetInstanceProcAddr`, `vk_icdGetInstanceProcAddr`,
  `vk_icdGetPhysicalDeviceProcAddr`.
- **Physical device:** `vkEnumeratePhysicalDevices`,
  `vkGetPhysicalDevice{Properties,Properties2,Features,Features2,
  MemoryProperties,MemoryProperties2,QueueFamilyProperties,QueueFamilyProperties2,
  FormatProperties,FormatProperties2,ImageFormatProperties,ImageFormatProperties2,
  SparseImageFormatProperties}` (+ KHR aliases),
  `vkEnumerateDevice{Extension,Layer}Properties`.
- **Device/queue:** `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceQueue`,
  `vkGetDeviceProcAddr`, `vkDeviceWaitIdle`, `vkQueueWaitIdle`, `vkQueueSubmit`.
- **Resources (generated band):** `vkAllocateMemory`, `vkFreeMemory`, `vkMapMemory`
  (zero-copy arena), `vkUnmapMemory`, `vkFlushMappedMemoryRanges`,
  `vkCreate/Destroy{Buffer,Image,ImageView,Sampler,RenderPass,Framebuffer,
  DescriptorSetLayout,DescriptorPool,PipelineLayout,PipelineCache,
  GraphicsPipelines,ComputePipelines,QueryPool,Event,Fence,Semaphore}`,
  `vkAllocate/FreeDescriptorSets`, `vkUpdateDescriptorSets`,
  `vkBind{Buffer,Image}Memory(+2,+2KHR)`, `vkGet{Buffer,Image}MemoryRequirements(2,2KHR)`.
- **Command recording (`alr_icd_cmd_entrypoints.inc`):**
  `vkCreate/DestroyCommandPool`, `vkResetCommandPool`, `vkAllocate/FreeCommandBuffers`,
  `vkBegin/End/ResetCommandBuffer`, `vkCmd{BeginRenderPass,EndRenderPass,NextSubpass}
  (+2,+2KHR)`, `vkCmd{BindPipeline,BindDescriptorSets,BindVertexBuffers,
  BindIndexBuffer,Draw,DrawIndexed,DrawIndirect,Dispatch,SetViewport,SetScissor,
  PipelineBarrier,CopyBuffer,CopyImage,CopyBufferToImage,PushConstants,
  ClearAttachments}`, `vkWaitForFences`, `vkResetFences`, `vkGetFenceStatus`.
- **Swapchain (surfaceless / AHB):** `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`,
  `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR` — BUT these
  **ignore the `VkSurfaceKHR`** (`pCreateInfo->surface` is commented "ignored by the
  ALR ICD"); the swapchain is a host-owned AHB ring keyed by a virtual id.
- **Advertised extensions.** Instance: `VK_KHR_surface`, `VK_KHR_wayland_surface`,
  `VK_KHR_xcb_surface`, `VK_EXT_headless_surface`,
  `VK_KHR_get_physical_device_properties2`,
  `VK_KHR_external_memory_capabilities`, `VK_EXT_debug_utils`. Device:
  `VK_KHR_swapchain`, `VK_KHR_maintenance1`, `VK_KHR_dedicated_allocation`,
  `VK_KHR_get_memory_requirements2`, `VK_KHR_bind_memory2`.

### 3.B GAPS — Chromium-Vulkan requires, ICD does NOT implement

> Enumerated, NOT implemented — `guest_icd` is stable / owned elsewhere. This list is
> the experiment's hypothesis set for **where it walls in OUR ICD** (tractable) vs
> Chromium-internal.

**GAP-1 (the headline): WSI surface-query family advertised but UNIMPLEMENTED.**
The ICD's instance extension list advertises `VK_KHR_surface` +
`VK_KHR_wayland_surface` + `VK_KHR_xcb_surface` + `VK_EXT_headless_surface`, but the
GIPA table backs **none** of the functions those extensions promise:
- `vkGetPhysicalDeviceSurfaceSupportKHR` — MISSING
- `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` — MISSING
- `vkGetPhysicalDeviceSurfaceFormatsKHR` — MISSING
- `vkGetPhysicalDeviceSurfacePresentModesKHR` — MISSING
- `vkDestroySurfaceKHR` — MISSING
- `vkCreateWaylandSurfaceKHR` / `vkGetPhysicalDeviceWaylandPresentationSupportKHR` — MISSING
- `vkCreateXcbSurfaceKHR` — MISSING
- `vkCreateHeadlessSurfaceEXT` — MISSING

If Chromium's Viz takes the **surface-backed** swapchain path it calls
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` (to size the swapchain) and
`…SurfaceFormatsKHR` (to pick the format) right after `vkCreateWaylandSurfaceKHR`.
GIPA returns NULL for all of these → Chromium's `gpu::VulkanSurface` init fails →
Viz aborts Vulkan (or, without `--disable-vulkan-fallback-to-gl-for-testing`,
silently demotes). **This is why §1.1 forces `--disable-vulkan-surface`**:
surfaceless Viz never creates a `VkSurfaceKHR`, never calls the surface-query family,
and presents via Ozone (which on our stack is the AHB present sink that
`vkQueuePresentKHR` already drives). So GAP-1 is **side-stepped by the flag**, not
filled — the experiment confirms whether surfaceless Viz truly avoids the surface
family (expected) or whether some code path still probes
`vkGetPhysicalDeviceSurfaceSupportKHR` (then GAP-1 is real and small to fill: 4 query
stubs returning a fixed B8G8R8A8/FIFO cap-set + a no-op destroy).

**GAP-2: external-memory/semaphore FD interop (advertised capability, no device
exts, no entrypoints).** Chromium's `SharedImage` + Viz share GPU images across the
(would-be) GPU/renderer boundary and synchronize with semaphores via FDs. Even in
single-process, Skia's `GrVkBackendContext` enables and Chromium's
`VulkanDeviceQueue` *requires* (in `gpu/vulkan/vulkan_device_queue.cc`'s required-ext
set) the external-memory/semaphore device extensions:
- `VK_KHR_external_memory` + `VK_KHR_external_memory_fd` — MISSING (ext + funcs
  `vkGetMemoryFdKHR`, `vkGetMemoryFdPropertiesKHR`)
- `VK_KHR_external_semaphore` + `VK_KHR_external_semaphore_fd` — MISSING (ext + funcs
  `vkGetSemaphoreFdKHR`, `vkImportSemaphoreFdKHR`)
- `VK_EXT_external_memory_dma_buf` / `VK_EXT_image_drm_format_modifier` /
  `VK_KHR_external_memory_capabilities` interop — MISSING on the device list
- `vkGetPhysicalDeviceExternal{Buffer,Semaphore,Fence}Properties` — MISSING
On Android the relevant import is actually `VK_ANDROID_external_memory_android_hardware_buffer`
(`vkGetAndroidHardwareBufferPropertiesANDROID`,
`vkGetMemoryAndroidHardwareBufferANDROID`) — also MISSING. Chromium's
`VulkanDeviceQueue::Initialize` walks a **required** extension list and FAILS device
creation if any required ext is absent → `vkCreateDevice` "succeeds" in our ICD (we
ignore `ppEnabledExtensionNames`) but Chromium's own pre-check (it intersects desired
vs `vkEnumerateDeviceExtensionProperties`) sees the ext is NOT advertised and bails
**before** calling `vkCreateDevice`. So GAP-2 most likely walls at
**`VulkanDeviceQueue::Initialize` → "required extension not present"** in Chromium
(visible in the `*vulkan*=2` log), NOT in our ICD. Filling it is non-trivial (real
FD/AHB export plumbing through the ring) — flag-mitigation is limited; see §4 fallback.

**GAP-3: timeline semaphores.** Modern Viz/Skia use `VK_KHR_timeline_semaphore`
(core in 1.2) for cross-submit sync: `vkGetSemaphoreCounterValue`,
`vkWaitSemaphores`, `vkSignalSemaphore` — all MISSING; `VK_KHR_timeline_semaphore`
not on the device ext list and the `VkPhysicalDeviceTimelineSemaphoreFeatures`
`timelineSemaphore` bit is whatever Mali reports (forwarded raw). If Chromium enables
timeline semaphores (it does when the device advertises ≥1.2 + the feature), it will
resolve the three funcs via GDPA and NULL-deref on first use, OR gate on the ext and
demote. Our `vkCreateSemaphore` is binary-only.

**GAP-4: synchronization2 / submit2.** Newer SkiaRenderer paths prefer
`VK_KHR_synchronization2` (`vkQueueSubmit2`, `vkCmdPipelineBarrier2`,
`vkCmdSetEvent2`/`WaitEvents2`) — all MISSING. Skia degrades to legacy
`vkQueueSubmit`/`vkCmdPipelineBarrier` when sync2 is absent (we do back those), so
this is **likely not a hard wall** provided we do NOT advertise sync2.

**GAP-5: render-critical vkCmd\* that Skia emits but the recorder lacks.** Skia's
Ganesh/Graphite Vulkan backend records a wider command set than our triangle bring-up:
- `vkCmdBlitImage` — MISSING (Skia uses it for mipmap/scale blits)
- `vkCmdResolveImage` — MISSING (MSAA resolve; SkiaRenderer may MSAA the root surface)
- `vkCmdCopyImageToBuffer` — MISSING (readback / `glReadPixels`-equivalent — needed
  for the experiment's frame-evidence capture path!)
- `vkCmdFillBuffer`, `vkCmdUpdateBuffer` — MISSING (buffer init/upload)
- `vkCmd{SetEvent,ResetEvent,WaitEvents}` — MISSING (event sync)
- `vkCmd{BeginQuery,EndQuery,WriteTimestamp,ResetQueryPool,CopyQueryPoolResults}` —
  MISSING (occlusion/timestamp queries; Skia uses timestamp queries when GPU timing
  is on — usually off in release)
- `vkCmdExecuteCommands` — MISSING (secondary command buffers; Skia uses secondaries
  for some wrapped-surface paths)
Each of these resolves via `vkGetDeviceProcAddr` at Skia context init; if Skia caches
the pointer and later calls it, NULL-deref. Skia is mostly defensive (checks caps /
has fallbacks), but `vkCmdBlitImage` + `vkCmdCopyImageToBuffer` are the highest-risk
because the root-surface composite + any readback hit them directly. These are the
**most tractable** gaps (each is one recorder entry + one host decode band, same
pattern as the existing `vkCmdCopyImage`).

**GAP-6: memory-management completeness.** `vkInvalidateMappedMemoryRanges` (paired
with the existing `vkFlushMappedMemoryRanges`) — MISSING; `vkGetBufferDeviceAddress`
/ `VK_KHR_buffer_device_address` — MISSING (Skia Graphite uses BDA; Ganesh does not —
Chromium 147 still defaults to Ganesh, so low risk). `vkGetPipelineCacheData` /
`vkMergePipelineCaches` — MISSING (Chromium serializes the pipeline cache; it tolerates
a stub that returns size 0).

### 3.C Gap-severity summary (the experiment's ranked wall hypotheses)

| # | gap | where it walls | in OUR ICD? | mitigation |
|---|---|---|---|---|
| 2 | external-memory/semaphore/AHB device exts | `VulkanDeviceQueue::Initialize` required-ext check | **NO** (Chromium-internal pre-check) | none via flag; needs real ring plumbing — the deepest gap |
| 1 | WSI surface-query family | `gpu::VulkanSurface` init | NO if `--disable-vulkan-surface` works; else YES (small stubs) | `--disable-vulkan-surface` |
| 3 | timeline semaphores | Skia/Viz sync init | YES (NULL-deref) if device claims 1.2+feature | cap timeline feature off / `ALR_ICD_APIVER_CAP` |
| 5 | `vkCmdBlitImage`/`CopyImageToBuffer`/… | Skia command recording / readback | **YES** (NULL-deref, tractable) | implement recorders (out of scope) |
| 4 | synchronization2 | SkiaRenderer (degrades) | NO (don't advertise) | leave unadvertised |
| 6 | invalidate/BDA/pipeline-cache | minor paths | mostly NO (tolerant) | stubs later |

**Net read:** the LIKELY first wall is **GAP-2 (Chromium-internal required-extension
check at `VulkanDeviceQueue::Initialize`)** — i.e. the same *class* as the ANGLE wall
(Chromium-side, not our-ICD-side), because Chromium gates device creation on
advertised external-memory/semaphore extensions our ICD doesn't list. If we get PAST
device creation (e.g. a Chromium build/flag that relaxes the required set), the NEXT
wall is **GAP-5/GAP-1 inside our ICD** (NULL `vkCmd*` / surface query), which is the
tractable, fill-it-ourselves regime. The experiment's job is to read the
`*vulkan*=2` + `[alr-icd]` logs to place the wall on exactly one of these rungs.

---

## 4. Device-experiment plan (BATCHED — device owned by concurrent agents)

Marker-gated, default-OFF. The marker is `/data/local/tmp/.alr-chromium-vulkan`.
Without it, `runChromiumStandalone` is byte-for-byte the proven software-raster
launch (strict no-regression).

### 4.1 Pre-reqs (host, one-time)
- APK built from this branch (the marker path compiled in).
- Overlays present at `/data/local/tmp/`: `chromium-gui-stage.tar`,
  `interpose/nss/chromium-net/xkb-gegl/gpushim` stages, **and**
  `vk-icd-stage.tar` + `vk-loader-stage.tar` (Khronos loader + our ICD + manifest).
  (`tools/build_vk_icd_overlay.py`, `tools/build_vk_loader_overlay.py`.)

### 4.2 Steps
1. `adb install -r` the APK; `adb shell am force-stop dev.chanwoo.androlinux`
   (per memory: force-stop before relaunch or onCreate overlays/probes are skipped).
2. `adb push <empty> /data/local/tmp/.alr-chromium-vulkan` (the gate).
3. `adb shell am start -n dev.chanwoo.androlinux/.MainActivity` (or the chromium
   launcher entry); the SurfaceView's `surfaceCreated` brings up the compositor and
   the marker path takes the Vulkan-flag branch.
4. `adb logcat -s alr_loader alr_cr_out alr_cr_diag` and grep for the rungs below.

### 4.3 Expected log ladder (each rung = a checkpoint)
- **R0 staging:** `cronly vk-icd-stage: done`, `cronly vk-loader-stage: done`
  (overlays in place). `VK_DRIVER_FILES=…/alr_icd.json` echoed by the loader.
- **R1 loader VK env:** `alr native loader vk ring attached=yes`; the loader's env
  dump shows `ALR_VK_ICD` forwarded, `androlinux` first on `LD_LIBRARY_PATH`, and
  `angle=off` (we did NOT set `ALR_ANGLE`).
- **R2 Chromium GpuInit selects Vulkan:** in `alr_cr_out`,
  `GpuInit … Vulkan` / `Using Vulkan` / `VulkanImplementationName: native`; the
  blocklist line (`gpu_data_manager*=2`) shows Vulkan **not** blocklisted (the
  `--ignore-gpu-blocklist` effect). No `Vulkan disabled` / `Falling back to GL`.
- **R3 ICD loaded:** Khronos loader (`VK_LOADER_DEBUG=all`) logs it found
  `alr_icd.json` → `libalr_mali_icd.so`; our ICD ctor logs `[alr-icd]` + the Mali
  device (`vkGetPhysicalDeviceProperties` → `Mali-G615`, `driverID` ARM).
- **R4 device creation (the GAP-2 rung):** `[alr-icd] vkCreateDevice` reached?
  - If Chromium logs `… required extension VK_KHR_external_memory_fd not supported`
    / `VulkanDeviceQueue::Initialize failed` **before** `[alr-icd] vkCreateDevice`
    → **wall = GAP-2, Chromium-internal** (same class as the ANGLE wall; our ICD is
    not the blocker — the fix is advertising+backing the external-memory/semaphore
    exts, a real-plumbing effort).
  - If `[alr-icd] vkCreateDevice` IS reached → Chromium accepted our advertised
    device exts; proceed.
- **R5 Skia/Viz context (GAP-1/3/5 rungs):** `[alr-icd]` trace of the vk\* calls Viz
  makes after device creation:
  - a NULL-deref / abort right after a `vkGetDeviceProcAddr("vkCmdBlitImage")` or
    `…("vkCmdCopyImageToBuffer")` → **wall = GAP-5, in OUR ICD (tractable)**.
  - a hang/abort in surface setup despite `--disable-vulkan-surface` (a
    `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` appears in the trace) → **wall =
    GAP-1, in OUR ICD (small stubs)**; the flag did not fully suppress the surface
    path.
  - a sync abort referencing `vkWaitSemaphores`/`vkGetSemaphoreCounterValue` →
    **wall = GAP-3, in OUR ICD**; cap the timeline feature.
- **R6 first GPU frame:** `[alr-icd] vkQueueSubmit … replay=0` then
  `[alr-icd] vkQueuePresentKHR` → the present sink
  (`alr_wayland_submit_gpu_frame`) drives the AHB onto the SurfaceView. Evidence =
  the branded `data:text/html` page rendered via the GPU path (visually identical,
  but `chrome://gpu`-equivalent log says Vulkan), and/or a `screencap` that is NOT
  black. If `vkCmdCopyImageToBuffer` (GAP-5) is filled, a readback dump can be the
  pixel evidence; until then the frame-on-screen is the proof.

### 4.4 Localizing the wall (the deliverable of the run)
The single question the run answers: **does it wall in OUR ICD (a gap to fill —
tractable, in `alr_gpu/guest_icd`) or Chromium-internal (like ANGLE)?**
- Wall at **R4 before `[alr-icd] vkCreateDevice`** = Chromium-internal
  (GAP-2 required-ext pre-check). Verdict: same *class* as ANGLE; needs ext plumbing.
- Wall at **R5 with an `[alr-icd]` NULL/abort** = OUR ICD (GAP-1/3/5). Verdict:
  tractable — enumerate the exact missing entrypoint from the trace and fill it.
- Reaches **R6** = native-Vulkan Chromium composites on Mali through our ICD. North
  star hit; remaining work is breadth (more `vkCmd*`, real WSI surface).

### 4.5 A/B controls (disambiguate ICD vs environment)
- **`--use-vulkan=swiftshader`** (Chromium's bundled software Vulkan, no Mali, no our
  ICD): if Chromium reaches R6 (renders) under SwiftShader but walls at R4/R5 under
  `=native`, the wall is **our ICD/Mali path**, not Chromium's Vulkan compositor
  wiring. If it ALSO walls under SwiftShader, the wall is Chromium's
  Vulkan-on-Linux/Ozone-Wayland integration (environment), independent of our ICD.
  (Mirrors the `ALR_VK_ICD_OVERRIDE` SwiftShader bisection the loader already
  supports for the ANGLE path.)
- **`ALR_ICD_APIVER_CAP=4198400` (1.2)** or lower: cap the device apiVersion so
  Chromium does not auto-enable 1.3/1.2 features (timeline, sync2) our ICD can't back,
  to test whether GAP-3/4 move the wall.

### 4.6 No-regression guard
The marker absent → the default `--disable-gpu` software launch (unchanged). The
Vulkan branch only differs in: the host env (§2) + the launch-argv tail (§1.1). No
loader edit, no `guest_icd` edit, no change to any other launch path. The existing
GLES/ANGLE probes (`launchAngleGlesProbe`, gated on `.alr-angle`) are independent and
untouched.

---

## 5. Open questions for the run to resolve
1. Does `--disable-vulkan-surface` fully suppress the `VkSurfaceKHR` path in Viz on
   Ozone-Wayland 147, or does some code still probe `vkGetPhysicalDeviceSurface*`?
   (GAP-1 real-or-not.)
2. Is the **required**-extension set in `VulkanDeviceQueue::Initialize` (147) a hard
   gate, or are the external-memory/semaphore exts merely *desired*? If desired,
   Chromium may proceed to R5 even without GAP-2 — then the wall is the tractable
   ICD-side regime.
3. Under single-process, does Chromium's `SharedImageManager` still demand FD/AHB
   export, or does the in-process path use plain `VkImage` handles (no external
   memory) — which our ICD already backs? (Determines whether GAP-2 is actually
   exercised.)
