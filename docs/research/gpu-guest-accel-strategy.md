# Guest GPU Acceleration Strategy — GL/GLES/Vulkan for Linux apps "like Android games"

Status: research + decision synthesis. Date: 2026-06-01.
Builds on: ADR 0002 (GPU and Display Path), `docs/architecture/gpu-display-bridge.md`.
Relates to: the in-repo `alr_gpu/` GLES track and its (out-of-repo) design note
`/tmp/gpu-native-app/DESIGN.md` (authored by the GPU-native session).

**Verified against commit `acbb2f8` ("GPU-native app M4 host half … v117").**
All code anchors below are by **function/struct name**, never line number (a concurrent
session shifts line numbers in `runtime_report.cpp`). Anchor existence, the native-core
GPU-ring test, and the 267 host pytest were re-run green at this commit (see Appendix).

---

## 0. Scope — which layer this is about

Three independent layers, only the third is open:

| Layer | Status | Anchor |
|---|---|---|
| CPU execution | native ARM64, zero emulation | in-process loader (`build_native_loader_probe`) |
| Display / compositing | **GPU (Mali), zero-copy** (`ahb gltex=0 path=zerocopy`) | `WaylandPresenter` (`init_ahb_path`, `ahb_upload`) |
| **Guest app's OWN GL/GLES/Vulkan render** | **OPEN — this doc** | `alr_gpu/` |

This strategy is **only** about the third layer: making a guest Linux app's *own* rendering
run on the Mali GPU. The display path is already hardware zero-copy for every app.

Not the target: **GIMP and other Cairo/CPU apps.** GIMP renders its canvas with Cairo
(`GDK_RENDERING=cairo`, set in the guest env) — CPU by design, same as desktop GIMP; its
only GPU-compute path is OpenCL (off by default, vendor-locked, out of scope). Nothing here
speeds up GIMP. The target is apps that actually issue GL/GLES/Vulkan: games, Wine/DXVK
titles, Blender, emulators, GLES demos.

---

## 1. Verified current assets (the foundation this builds on)

Device SM-X236N, Mali-G615 MC2 (Valhall, Vulkan 1.3), Android 16/API 36, untrusted_app.

| Asset | State | Anchor (by name) |
|---|---|---|
| Host Vulkan probe (enumerates Mali in-process) | ✅ device | `build_host_vulkan_probe_report` |
| Host EGL/GLES2 render on Mali | ✅ device | `build_host_gpu_probe_report` |
| GPU command **marshalling** (byte-stream → real GLES, pixel-verified) | ✅ device (v69) | `build_gpu_marshalling_probe` |
| Transport **boundary** cost model (batched shmem ring wins) | ✅ device (v59) | `build_gpu_boundary_probe` |
| AHB→EGLImage→external-OES zero-copy **sample** | ✅ device (v113) | `build_ahb_zerocopy_probe_report` |
| AHB zero-copy **present** in the live compositor | ✅ device (v114) | `WaylandPresenter::ahb_upload` |
| **GLES decoder** (shader/VBO/texture/draw, virtual→real IDs) | ✅ device (v115) | `alr_gpu/alr_gpu_decode.hpp` (`enum Op`, `HostState`) |
| **SPSC command ring** (host-tested) | ✅ host + device (v115) | `alr_gpu/alr_gpu_ring.hpp` (`RingHeader`) |
| Host draw / ring-draw probes | ✅ device (v115) | `alr_gpu/alr_gpu_probe.hpp` |

**M4 render-INTO-AHB — host half committed + Mali-verified (v117, `acbb2f8`):**
`alr_gpu/alr_gpu_fbo.hpp` (`AhbRenderTarget`, `run_fbo_present_probe`) + JNI probe
`nativeAlrGpuFboProbe`, report `ALR GPU FBO RENDERTARGET: PASS` — decoded guest draws land in an
AHB-backed FBO, then resample via external-OES. Evidence:
`docs/evidence/2026-06-01-device-SM-X236N-v117-gpu-native-ahb-rendertarget.md`.

**Net:** the gfxstream-shaped pieces — marshalling, ring, virtual-ID decoder, AHB display, and
now render-INTO-AHB — are built and device-verified for GLES2. The open work is breadth (more
GLES, then Vulkan), the guest `libEGL`/`libGLESv2` shim (M3), and wiring render→AHB to the live
present path.

---

## 2. Hard constraints (these eliminate most of the field)

From ADR 0002 / `gpu-display-bridge.md`: **public Android APIs only; no `/dev/dri`, KMS, GBM,
KGSL, Turnip, Freedreno, Panfrost; no root; in-process glibc-in-bionic; same arch (aarch64).**

Research-confirmed **disqualifiers** (cited in §9):

- **Turnip** (Mesa Vulkan, Adreno-only) needs `/dev/kgsl` or `/dev/dri`; rootless load via
  libadrenotools only works because Adreno splits `libvulkan.adreno.so` — **Mali is monolithic
  (`libGLES_mali.so`), no hook point.** Disqualified (Adreno-only + kernel DRM).
- **Panfrost / PanVK** (open Mesa Mali) need the DRM render node (`/dev/dri`) = root/custom
  kernel — **and Mali-G615 (v11) is not yet supported by PanVK** (only G610/v10). Disqualified.
- **VirGL host-GL** runs the GL side on host *software* GL on Android (llvmpipe-class, DX9-era).
  Allowed but not "native-like." Fallback only.

**Consequence (the central external validation):** on a non-root Mali device, the **vendor
Mali Vulkan driver, reached in-process via the public NDK `libvulkan`, is the *only* hardware
path that exists.** Calling it is not a compromise — there is no rootless alternative. This is
exactly what `build_host_vulkan_probe_report` already does.

---

## 3. Research findings (five load-bearing facts)

1. **Only path on non-root Mali = vendor `libvulkan` in-process** (§2). `libvulkan` is a public
   NDK lib; the loader crosses into the vendor `sphal` namespace for us; `untrusted_app` does
   full Mali Vulkan with no special permission.
2. **Cross-process streaming stacks' *transport* is pure overhead for us; their *semantics* are
   reusable.** gfxstream/VirGL/Venus exist to cross a VM/process boundary we mostly don't have.
   Reuse gfxstream's host-side handle/`pNext`/descriptor/sync **replay** (Apache-2.0, same arch);
   read `venus-protocol` as the Vulkan-correctness checklist; **do not link Venus** (its dma-buf/
   blob memory mapping is VM-boundary tax).
3. **Vortek (Winlator 10+) is the direct precedent — borrow its workarounds, not its IPC.**
   Vortek's socket+ashmem ring exists *only* to bridge Box64's glibc world to the bionic driver.
   We don't need that transport (in-process). We **do** need its Mali-weakness layer: BCn texture
   decode, `gl_ClipDistance` SPIR-V strip, `*SCALED` format emulation, extension faking.
4. **zink on Mali is capped; ANGLE is the robust GLES path.** Mali's proprietary Vulkan lacks
   `VK_EXT_transform_feedback` (and geometry/tessellation), which zink needs for GLES 3.0 / GL 3.0
   — so **zink-on-Mali can't cleanly clear GLES 3.0** (why Winlator-Mali ships VirGL+WineD3D, not
   zink). **ANGLE** exposes only GLES (not desktop GL) but is the Vulkan-backend GLES translator
   Chrome/Android run on Mali in production — robust GLES 2.0/3.0/3.1.
5. **AHB zero-copy swapchain is solidly feasible on Mali.**
   `VK_ANDROID_external_memory_android_hardware_buffer` (+ `VK_ANDROID_external_format`) is
   **CDD-mandatory** on a Vulkan-1.1+ device → guaranteed on VK1.3 Mali-G615. Rendering INTO an
   AHB as a `COLOR_ATTACHMENT` `VkImage` with `R8G8B8A8_UNORM` is spec-normative (no Ycbcr). The
   AHB native handle's `data[0]` is a real dma-buf FD → drops into `zwp_linux_dmabuf_v1` for the
   in-app compositor (or used directly, same process). This is the Vulkan twin of the GLES M4 work
   already done host-side (`AhbRenderTarget`, v117).

---

## 4. Two architectures compared (the actual decision)

Both end at the **same** place — guest → vendor Mali Vulkan; **zink** for desktop GL/GLES;
render into an **AHB**; in-app compositor presents **zero-copy**. They differ in exactly **one**
thing: **how the glibc guest crosses into the bionic Mali driver.**

> Reference for "A": the proposal to base a guest-side bridge ICD on Termux's
> **`vulkan-wrapper-android`** = a custom Mesa fork (`xMeM/mesa`) built `-Dvulkan-drivers=wrapper`
> for **bionic** (NDK), wrapping the Android system Vulkan and adding X11/Wayland WSI.

| | **A — direct in-process ICD** (libhybris-shaped) | **B — command ring + bionic executor** (Vortek-shaped, no IPC) |
|---|---|---|
| Guest frontend | zink + wrapper ICD | zink + thin glibc forwarding ICD |
| Vulkan backend | vendor Mali `libvulkan` | vendor Mali `libvulkan` |
| GL/GLES | mesa zink | mesa zink (or ANGLE for GLES) |
| Present | AHB zero-copy → in-app compositor | AHB zero-copy → in-app compositor |
| **glibc↔bionic crossing** | guest thread calls bionic driver **synchronously** | hand off via ring to a **dedicated bionic-TLS executor** |
| TLS hazard | **yes** — glibc thread in bionic driver; Mali driver assumes it owns TLS (libhybris #559) | **no** — driver always runs in pure bionic TLS |
| Per-call cost | lowest *if* TLS cooperates; else per-call TLS gate (leaky at callbacks) | in-process ring enqueue (no socket/copy) + one thread hop, amortized at submit |
| Maturity on Mali | libhybris-class, fragile on Mali specifically | the model that actually ships Mali games (Vortek) |

**They are ~85% the same stack.** The wrapper (Mesa `wrapper` driver) + zink + vendor Vulkan +
AHB present are common; the *only* divergence is the boundary row.

**Performance:** comparable. A is theoretically lower-overhead, but Vulkan is call-heavy
(thousands of `vkCmd*`/frame) so A's per-call TLS gate can cost more than B's submit-amortized
handoff; our B is *in-process*, so it avoids the socket/serialize/copy that makes Vortek's
cross-process ring expensive. In both, the **dominant** cost is zink (GL→VK) + the Mali driver,
not the boundary.

**Compatibility:** API coverage is identical (both = vendor Vulkan + zink), and **both inherit
the same zink-on-Mali ceiling** (no transform_feedback → GLES2/GL-2.x–3.0 class) — so the GL
ceiling is **not** a differentiator. The difference is **stability**: A risks intermittent Mali
TLS corruption; B sidesteps it.

**The decider:** `vulkan-wrapper-android` is built for **bionic**. In our in-process model a
glibc guest can't cleanly load a bionic ICD without hitting the boundary at load time. So the
clean way to use the wrapper is to run it **host-side on the bionic executor**, with a thin
**glibc forwarding ICD** in the guest rootfs — **which is exactly architecture B.** B is not an
alternative to the wrapper proposal; it is how that proposal is correctly realized here.

**Decision: adopt the wrapper proposal's frontend, back it with B's executor boundary.** Borrow
`vulkan-wrapper-android` (Mesa `wrapper` driver + WSI/extension/feature code), cross the boundary
with a dedicated bionic-TLS executor + the already-built `alr_gpu` ring, add the AHB swapchain
glue and the Mali-workaround shim. Drop libdrm/X11 deps (Wayland only) and Termux topology
assumptions.

---

## 5. Recommended architecture — one ring, one host executor, three frontends

```text
 guest (glibc rootfs)                  │  boundary = command ring  │ host (bionic, owns Mali driver)
 ──────────────────────────────────────┼───────────────────────────┼──────────────────────────────
 [F1] libGLESv2/libEGL shim   ─encode─▶ │                           │ ─▶ host GLES2 ctx (decode+replay)
 [F2] libvulkan_alr (ICD)     ─encode─▶ │  SPSC shmem ring          │ ─▶ host Mali libvulkan
 [F3] zink / ANGLE  (atop F2/F1)        │  (alr_gpu_ring.hpp)       │    + Mali workaround shim
                                        │  1 sync / frame at swap   │ ─▶ render INTO AHB (AhbRenderTarget)
                                        │                           │ ─▶ WaylandPresenter zero-copy present
```

Why the ring+executor is correct for **both** topologies: whether the guest is a fork()ed child
(ring crosses a COW process boundary, as the GLES track does today) or fully in-process (ring
crosses a thread boundary to the bionic-TLS executor), the model neutralizes the glibc↔bionic
boundary at **one** controlled point. In-process, ring "encode" is a memcpy into a shared heap —
no serialization-to-socket, unlike Vortek. The host executor is the single place that touches the
real driver, in permanent bionic TLS — so the Mali "I own TLS" assumption holds.

---

## 6. API layering priority

Headline goal ("like Android games") → modern games and Wine titles are **Vulkan** (via
DXVK/VKD3D); desktop GL is the legacy long tail.

| Order | Frontend | Covers | Mali difficulty |
|---|---|---|---|
| **A (headline)** | Vulkan ICD passthrough | native-Vulkan games + **all Wine/DXVK/VKD3D** | medium (needs workaround shim) |
| **B (robust)** | GLES — own shim (in progress) or **ANGLE→Vulkan** | GLES2/3 games, emulators, demos | low (ANGLE Mali-proven) |
| **C (R&D)** | desktop GL — **zink→Vulkan** | legacy GL games, **Blender** | high, **capped at GLES3.0/GL3.0** |

- The GLES track (F1) is the de-risking keystone and is already at M1/M2 device-verified.
- The Vulkan frontend (F2) is the highest-value *addition* and reuses the same ring + AHB present.
- zink (F3) is last and **R&D**: Blender (GL 4.3+) needs a transform-feedback-via-compute shim
  under zink (bionic-vulkan-wrapper style). **Do not promise Blender on Mali-G615** until proven.

---

## 7. Roadmap (aligned with the in-repo `alr_gpu` track)

GLES track (existing; mirrors the GPU-native session's milestones):

- **M1 — host decoder** (shader/VBO/texture/draw, virtual IDs) — ✅ `alr_gpu_decode.hpp`, device v115.
- **M2 — SPSC ring** carries the op stream from a guest binary — ✅ `alr_gpu_ring.hpp`, host+device v115.
- **M3 — guest `libEGL.so.1`/`libGLESv2.so.2` shim** + `alr-gles-cube` client — ✅ **DONE**:
  glibc-aarch64, zig-built; the shim's emitted wire decodes **byte-for-byte** on the committed host
  decoder (33 ops; vmaps/uniform-by-name/8×8 texture/36-vertex draw intact). Source in
  `app/src/main/cpp/alr_gpu/guest_shim/`.
- **M4 — render into AHB** — ✅ host half device-verified (v117): `AhbRenderTarget`,
  `run_fbo_present_probe`, `ALR GPU FBO RENDERTARGET: PASS`. Live present (cube on screen) pending
  the host executor service (`alr_gpu_host_service.hpp`, in progress) + the loader fork — M3 done.
- **M5 — glmark2-es2** breadth — ⬜ later.

Vulkan frontend track (new; this doc):

- **VK-M1 — host wrapper-ICD probe:** on the bionic executor, create instance/device via vendor
  `libvulkan`, clear-render INTO an `AhbRenderTarget`, present via `WaylandPresenter`. Pixel-verify
  on Mali, software-rejected. (Direct extension of `build_host_vulkan_probe_report` +
  `build_ahb_zerocopy_probe_report`; no guest, no ring — the de-risking keystone, mirrors GLES M1.)
- **VK-M2 — Vulkan ops over the ring** from a guest binary (reuse `alr_gpu_ring.hpp`); executor
  drains + replays on vendor `libvulkan`. Handle/`pNext`/descriptor/sync model from gfxstream;
  `venus-protocol` as the checklist.
- **VK-M3 — guest Vulkan ICD** (`libvulkan_alr.so` + ICD manifest); a normal Vulkan client binds it.
- **VK-M4 — AHB-backed swapchain** (`VK_ANDROID_external_memory_AHB`) → in-app compositor zero-copy.
- **VK-M5 — Mali workaround shim** (BCn decode, SPIR-V patches, extension faking); device-probe
  `vkGetPhysicalDeviceImageFormatProperties2` before relying on any format/usage.
- **VK-M6 — zink atop F2** for desktop GL (capped); **ANGLE** for robust GLES3 if the own-shim is
  too narrow. DXVK/VKD3D + Wine atop F2 for Windows games.

Every gate keeps the project's evidence bar: `renderer=Mali-…`, `software=false`,
`glReadPixels`/pixel verification, device screenshot.

---

## 8. Risks & honest limits

- **glibc↔bionic TLS boundary** — the one real engineering risk; mitigation is the dedicated
  bionic-TLS executor (not per-call TLS swap, which leaks at callbacks and on Mali specifically).
- **Mali proprietary Vulkan weakness** — no transform_feedback/geometry/tessellation; driver bugs
  (`textureQueryLod` crash, etc.). Needs the workaround shim; caps zink.
- **BCn texture decode is CPU** on Mali — cost on texture-heavy titles.
- **AHB-as-render-target** — now proven host-side on Mali (v117, `ALR GPU FBO RENDERTARGET: PASS`,
  `AhbRenderTarget`); remaining is wiring to the live `WaylandPresenter` present and driving it
  from the guest shim. Keep a fallback (plain FBO + copy/blit to AHB at swap) if direct AHB-FBO
  color attachment misbehaves under load.
- **"Native game speed" scope** — realistic for Vulkan-native and GLES content; desktop-GL legacy
  is limited on Mali. Don't promise AAA.

---

## 9. Prior art & sources

- Winlator GPU matrix (Mali = VirGL+WineD3D; Turnip+zink = Adreno-only): https://winlator.dev/best-gpu-drivers/ , https://winlator.dev/winlator-mali/
- Vortek (Winlator Vulkan forwarding): https://github.com/brunodev85/vortek ; internals: https://leegao.github.io/winlator-internals/2025/06/01/Vortek1.html
- bionic-vulkan-wrapper (Mali Vk shim, feature faking): https://github.com/leegao/bionic-vulkan-wrapper/releases
- libadrenotools (why Mali has no rootless driver swap): https://github.com/bylaws/libadrenotools
- gfxstream (guest→host graphics, Apache-2.0, in-process/IPC transports): https://android.googlesource.com/platform/hardware/google/gfxstream/+/refs/heads/main/README.md
- Mesa Venus (assumes virtio/blob — avoid linking): https://docs.mesa3d.org/drivers/venus.html
- Mesa zink requirements (transform_feedback → GLES3): https://docs.mesa3d.org/drivers/zink.html
- Collabora desktop-GL-on-Mali / TF-mandatory: https://www.collabora.com/news-and-blog/blog/2021/01/13/desktop-opengl-3-1-on-mali-gpus-with-panfrost/
- ANGLE (GLES-only output): https://github.com/google/angle
- PanVK still no TF (Mesa 26.1): https://christian-gmeiner.info/2026-04-20-panvk-extensions/
- VK_ANDROID_external_memory_android_hardware_buffer (CDD-mandatory; render-into-AHB): https://registry.khronos.org/vulkan/specs/latest/man/html/VK_ANDROID_external_memory_android_hardware_buffer.html
- libhybris #559 (Mali driver clobbers TLS across glibc/bionic): https://github.com/libhybris/libhybris/issues/559
- PPSSPP Mali Vulkan driver bugs: https://www.ppsspp.org/docs/articles/vulkan-driver-bugs/
- Termux `vulkan-wrapper-android` (Mesa `wrapper` driver, the "A" base): https://github.com/lfdevs/termux-packages/tree/dev/wrapper/packages/vulkan-wrapper-android
- Container-On-Android virglrenderer (non-root Mali GL uplift precedent): https://github.com/Container-On-Android/virglrenderer

---

## 10. Convergence — the native-performance / stability decision

Capstone: given everything above **plus the now-complete GLES guest shim (M3)**, this is the
single converged plan, optimized for **native-grade performance AND stability together** (the
explicit mandate). It does not re-open §4's A-vs-B boundary; it settles the "could we go *faster*?"
question and fixes the execution order.

### 10.1 "Native performance" is already won at the layer that matters
In **every** architecture on the table (A, A′, B) the GPU executes **real Mali command buffers at
full hardware speed** — no software rasterizer, no translation of the GPU's own work. Native GPU
speed is therefore **not the variable**. The only variables are (i) the CPU cost of crossing the
glibc→bionic boundary and (ii) the cost of moving pixels. The converged plan drives both to ≈0:

- **Boundary tax → negligible.** The hot path (`vkCmd*`, GLES draws) is **fire-and-forget**: the
  guest encodes into the in-process shared-heap ring with client-side **virtual object IDs** (no
  round-trip on create calls) and **blocks exactly once per frame** at swap (`req_seq`/`reply_seq`).
  Encode = a `memcpy` into shared heap — no socket, no serialize, no copy-to-kernel. That is what
  makes our **in-process** ring fundamentally cheaper than Vortek's cross-process one. Vulkan
  command-buffer recording is already deferred by API design, so ring-batching adds ~nothing the
  API wasn't already doing.
- **Pixel movement → zero-copy.** Render targets are **AHB-backed** (`AhbRenderTarget` for GLES;
  `VK_ANDROID_external_memory_AHB` `COLOR_ATTACHMENT` for Vulkan) and the compositor samples that
  same AHB (external-OES / dma-buf) — **no per-frame blit or readback**. Proven on Mali host-side
  at v114 (present) + v117 (render-into-AHB).

Net: the dominant cost is the **driver + the app's own GL→VK translation** (zink/ANGLE), exactly
as for a native Android game. The boundary sits in the noise.

### 10.2 Why B stays the choice even under a "max performance" mandate
A (direct in-process ICD) is *theoretically* one `memcpy` cheaper per call, but Vulkan is thousands
of calls/frame, so A's **per-call TLS gate** (the Mali blob assumes it owns bionic TLS, libhybris
#559) can cost **more** than B's one-thread-hop-**per-frame**; and A's Mali stability is
libhybris-class fragile (it leaks at driver callbacks). B crosses the boundary **once, permanently**,
on a dedicated bionic-TLS executor thread — the driver always runs in pure bionic TLS. So for a
call-heavy Vulkan workload B wins **both** axes: comparable-or-better CPU cost **and** the only one
of the two that is stable. "Max performance AND stability" therefore selects **B**, not merely
"stability."

- **Deferred micro-opt (A′).** A libhybris-style per-call TLS-trampoline direct ICD could shave the
  residual thread hop. **Explicitly deferred** — re-open ONLY if on-device profiling ever shows the
  executor hop (not the driver) is the frame bottleneck, which a per-frame-sync design should never
  exhibit. Recorded here so "can zero-marshalling go faster?" is **settled, not forgotten.**

### 10.3 Converged execution order (what gets built, in order)
1. **GLES live integration (finish M4) — the de-risking keystone, in progress now.** M3 (guest
   `libEGL.so.1`/`libGLESv2.so.2` shim + `alr-gles-cube`) is **DONE**: glibc-aarch64, zig-built, and
   the shim's emitted wire **decodes byte-for-byte on the committed host decoder** (33 ops; vmaps,
   uniform-by-name, 8×8 texture, 36-vertex draw all intact). The host **executor service**
   (`alr_gpu/alr_gpu_host_service.hpp`: a ring-consumer thread that owns the Mali GLES2 context +
   AHB-FBO, decodes per frame, presents the AHB) is being built header-side; once its in-process
   live probe is Mali-green, the only step left is the loader fork (cube `exec`'d with the ring fd)
   → **a spinning cube on the SurfaceView**, proving the ring+executor+AHB+present backbone live.
2. **VK-M1 — Vulkan executor keystone (the native-perf headline).** Same backbone, Vulkan frontend:
   the bionic executor creates instance/device on vendor `libvulkan`, renders into an
   `AhbRenderTarget`, presents zero-copy. Native-Vulkan games + all Wine/DXVK/VKD3D land here.
3. **ANGLE for GLES breadth** atop the same executor once the own-shim's reach is the limiter
   (Mali-proven, conformant GLES2/3).
4. **zink last / R&D** — desktop GL, capped at GLES3.0/GL3.0 on Mali; **no Blender promise**.

The backbone (**one SPSC ring + one bionic-TLS executor + AHB zero-copy present**) is **identical
across 1–4**; each new frontend is an additive encoder — which is precisely why finishing the GLES
keystone de-risks the entire Vulkan headline.

---

## Appendix — verification (this commit)

Re-run at `acbb2f8` (v117; working tree clean except this untracked doc):

- `scripts/test-native-core.sh` → **PASS**, including the SPSC ring
  (`pushed=10000 drained=10000 match=YES`, sync handshake ok, wrap-around `free-when-empty=255`).
- `pytest tests` → **267 passed**.
- All code anchors in §1 confirmed present (`grep` by name).

Commit-cadence note: the GPU-native track advanced several times during this writing
(`d7c1952`/v115 → `acbb2f8`/v117); M4's render-into-AHB host half is now committed and
Mali-verified. Anchors above are by name, so they stay valid across further version bumps.
