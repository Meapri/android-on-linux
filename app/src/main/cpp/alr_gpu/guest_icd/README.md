# ALR guest Vulkan ICD (`libvulkan.so.1`)

The **loader-discoverable guest entrypoint** for ALR's universal GPU path: a
glibc-aarch64 `libvulkan` an unmodified Vulkan client (or later ANGLE/zink) loads via
the standard mechanism, whose calls **marshal over the SPSC ring to the app-process
host**, where they are replayed on the **real vendor Mali libvulkan** — the exact
DEVICE-PROVEN path of `run_vk_marshal_mali_probe` (`../alr_gpu_vk_marshal_probe.hpp` /
`../alr_gpu_vk_decode.hpp`, evidence `docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`),
but reached through the **standard Vulkan ICD entry instead of in-process probe code**.

This is the Vulkan analogue of the GLES shim (`../guest_shim/`, `libGLESv2.so.2`): same
wire (`../alr_gpu_vk_proto.hpp`), same ring (`../guest_shim/alr_gpu_ring_c.h`), same
client-side virtual-handle model — but driven by the guest app, not probe code, and
implementing the standard Vulkan loader/ICD ABI.

## Files
| File | Role |
|---|---|
| `alr_icd_vulkan.c` | the ICD: Khronos loader/ICD interface + `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` dispatch + the ENUM-rung entry points (marshalled over the ring) |
| `alr_icd_runtime.h` | ring transport (request producer + **reply consumer**) + the `alr_icd_roundtrip()` request→reply primitive |
| `alr_icd_env.h` | host↔guest env contract (`ALR_VK_RING_FD/BYTES` + `ALR_VK_REPLY_FD/BYTES` + doorbell) |
| `alr_icd_vk_min.h` | a **minimal, ABI-exact** Vulkan + `vk_icd.h` subset (zig cc ships no Vulkan headers) — sizes/offsets static-asserted vs NDK r27's `vulkan_core.h` |
| `alr-vk-enum.c` | the tiny device-test client (`vkCreateInstance`+`vkEnumeratePhysicalDevices`+print `deviceName`) |
| `build-icd.sh` | cross-compile (zig cc, glibc-2.34 NEEDED-clean) → `out/libvulkan.so.1` + `out/alr-vk-enum` |

## Decision — **DIRECT SONAME** route (ship as `libvulkan.so.1`)

We ship the ICD **directly as SONAME `libvulkan.so.1`**, NOT behind the Khronos Vulkan
loader + an ICD manifest. Rationale:

- The base rootfs has **NO libvulkan** (confirmed) → **zero conflict**, exactly like the
  GLES shim ships `libGLESv2.so.2` directly.
- `vkGetInstanceProcAddr` IS the public entry an app/ANGLE/loader calls; a single
  NEEDED-clean `.so` (only `libc.so.6`) is what the tiny rootfs can load (a
  `libpthread.so.0` NEEDED would break it — the glibc-2.34 target pin folds pthread in).
- No extra moving part: the loader's `LD_LIBRARY_PATH` prepend of `/usr/lib/androlinux`
  (gated on `ALR_VK_ICD=1`) already makes `libvulkan.so.1` resolve first.

**We still export the Khronos loader/ICD interface** (`vk_icdNegotiateLoaderICDInterfaceVersion`,
`vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`) **and ship the manifest
`alr_icd.json`**, so if a real Vulkan loader is ever introduced it discovers us via
`VK_ICD_FILENAMES` too — at no extra cost. Best of both; the trade is a few extra exported
symbols (harmless). The only concrete blocker that would force the loader route is an app
that *hard-requires* the real Khronos loader (e.g. for layer injection) — not in scope for
the ENUM milestone, and the manifest is already staged for that day.

## Transport — why a **second (reply) ring**

The GLES path is almost entirely fire-and-forget (ops in, a sync seqno back; identity
from the ring header). The Vulkan **ENUM rung is not**: `vkEnumeratePhysicalDevices` /
`vkGetPhysicalDeviceProperties` must return **data** (device count, `"Mali-G615 MC2"`,
apiVersion, queue families). So the ICD uses a **request ring** (it produces) **and a
reply ring** (it consumes) — the two-ring shape `run_vk_marshal_mali_probe` already
proves in-process, now spanning the forked guest and the app-process host. The host half
is `../alr_gpu_vk_host_service.hpp` (`VkRingServicer`): drain request ring →
`decode_vk_batch(..., ALR_VK_DECODE_REAL, nullptr)` on real Mali → write reply ring → bump
the **request ring's** `reply_seq` (the completion signal the ICD's `alr_icd_roundtrip`
spins on).

## Build (host)
```sh
bash app/src/main/cpp/alr_gpu/guest_icd/build-icd.sh   # -> out/libvulkan.so.1 (+ alr-vk-enum)
# overlay tar (libvulkan.so.1 0755 + libvulkan.so symlink + alr_icd.json):
python -m tools.build_vk_icd_overlay --out /tmp/vk-icd-stage.tar
python -m tools.stage_tar_spec --overlay /tmp/vk-icd-stage.tar --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar   # CONFORMANT
python -m tools.overlay_guard  --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar --overlay /tmp/vk-icd-stage.tar    # 0 BLOCK
```
`libvulkan.so.1`: `NEEDED libc.so.6` only, `SONAME libvulkan.so.1`, exports the 3 ICD
symbols + the ENUM-rung entry points (verify: `nm -D out/libvulkan.so.1`).

## DEVICE-TEST PLAN (queued; do not run here — CR-2 owns the device)

1. Build + push the overlay (and the client):
   ```sh
   python -m tools.build_vk_icd_overlay --out /tmp/vk-icd-stage.tar
   adb push /tmp/vk-icd-stage.tar /data/local/tmp/vk-icd-stage.tar
   # the loader extracts it into <rootfs>/usr/lib/androlinux on next cold start
   # (MainActivity stage loop, size-keyed .vk-icd-staged marker). Also push the client
   # into the rootfs so it can be the guest program:
   adb push app/src/main/cpp/alr_gpu/guest_icd/out/alr-vk-enum /data/local/tmp/alr-vk-enum
   ```
2. Install the APK (versionCode unchanged), force-stop first (memory: force-stop before
   re-launch or onCreate overlays/probes are skipped), cold-launch.
3. **Host servicer self-test (no guest needed)** — already wired into the probe report:
   grep `ALR VK ICD SERVICE: PASS` (logcat tag `vk-icd-service:`). Expected:
   ```
   ALR VK ICD SERVICE: PASS
   renderer=Mali-G615 MC2
   api=1.3
   software renderer=false
   ```
4. **Guest-ICD end-to-end** — run `alr-vk-enum` THROUGH the ALR native loader with the
   ICD opted in (this is the milestone proof: Mali reached via the standard ICD entry):
   ```
   ALR_VK_ICD=1  VK_LOADER_DEBUG=all   (loader sets these into the guest env)
   guest program = /alr-vk-enum   (copied into the rootfs, or staged under /usr/bin)
   ```
   The loader (`ALR_VK_ICD=1`) puts `/usr/lib/androlinux` first on the guest lib path
   (so `libvulkan.so.1` resolves to OUR ICD), attaches the VK request/reply rings, and
   starts the host servicer. Expected guest stdout (the PASS signal):
   ```
   ALR VK ICD CLIENT: PASS device=Mali-G615 MC2 api=1.3
   ```
   i.e. `"Mali-G615"` surfaced **THROUGH the ICD** (`vkGetPhysicalDeviceProperties` via
   `libvulkan.so.1`), not the in-process probe.

## NOT yet covered (next rungs)
- swapchain / WSI present (`VK_KHR_swapchain`, AHB-backed → in-app compositor zero-copy)
- full draw/compute breadth + real-app **SPIR-V over the wire** (`vkCreateShaderModule`
  blobs, pipelines, buffers, descriptors) — the ENUM rung carries handles, not SPIR-V yet
- **ANGLE-on-top** (GLES3 via our Vulkan ICD) and zink (desktop GL, Mali-capped)
- multi-threaded submit fairness (one global lock serializes the round-trip today)
