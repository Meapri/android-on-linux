# Zero-Overhead Linux GPU Runtime Roadmap

> **For Hermes:** Use the android-linux-runtime and writing-plans skills when implementing this roadmap. Work in five-version bundles, deliver APKs at bundle boundaries, and keep device evidence as the source of truth.

**Goal:** Run real Linux/glibc apps inside a non-root Android APK with minimal execution overhead and Android-native GPU/GUI acceleration.

**Architecture:** Separate the runtime execution backend from graphics acceleration. The execution backend should evolve from ptrace-based PRoot to a zero/low-ptrace proroot-style backend, while GPU/GUI acceleration should terminate on Android public graphics APIs: Surface + EGL/GLES first, Vulkan later. Linux apps should see Linux-like APIs, but rendering must be proxied to Android host GPU services instead of relying on Linux DRM/KMS device nodes.

**Tech Stack:** Kotlin/Android Activity, Android NDK/C++, PRoot/optional proroot-style backend, glibc rootfs, localhost/Unix IPC, EGL/GLES Surface renderer, future Vulkan host renderer, guest-side GL/EGL/Wayland/X11 shims.

---

## Current State

Device-proven milestones:

- Linux/glibc rootfs execution through packaged PRoot: PASS.
- Android host EGL/GLES hardware renderer on Mali-G615 MC2: PASS.
- Android Surface EGL/GLES hardware render: PASS.
- Guest GPU command controlling host Surface renderer: PASS.
- Guest TCP IPC multi-frame bridge: PASS.
- Guest GLES shim smoke: PASS.
- Guest Wayland/X11-style GUI frame bridge to Android Surface GPU: functionally PASS.

V35 device evidence:

```text
surface frames rendered=8
surface frames dropped=0
surface frame lossless=true
surface gl renderer=Mali-G615 MC2
surface gpu hardware render=true
guest gui gpu compositor hardware render=true
guest wayland/x11 gui gpu surface hardware render=true
```

Known issue:

- V35 summary says `GUEST WAYLAND GUI GPU BRIDGE EXECUTION: FAIL` and `GUEST X11 GUI GPU BRIDGE EXECUTION: FAIL` because ACK writing happens after socket input is closed. Frames were received and rendered losslessly; this is a report/ACK lifecycle bug, not a GPU-path failure.

Major unresolved runtime issue:

- `dpkg -i local deb` still fails under current PRoot backend:

```text
dpkg-split: Function not implemented
status-old: Permission denied
```

This points to PRoot/fakeroot/path/syscall limitations and is one reason to evaluate a proroot-style backend.

---

## Architecture Target

```text
Android APK
├─ UI / Activity / Surface owner
├─ Execution backend manager
│  ├─ proot backend: current compatibility baseline
│  ├─ proroot backend probe: optional binary-backed experiment
│  └─ alr runtime backend: our future open implementation
├─ Rootfs manager
│  ├─ verified tar payloads
│  ├─ app-private extracted rootfs
│  └─ package-manager state
├─ Graphics host services
│  ├─ EGL/GLES Surface renderer
│  ├─ Vulkan Surface renderer, later
│  ├─ GUI compositor bridge
│  └─ frame/latency/loss metrics
└─ Linux guest rootfs
   ├─ userland apps: shell, apt, Python, Node, Chromium, etc.
   ├─ Wayland/X11 client/proxy tools
   ├─ libEGL/libGLES shim
   ├─ future Vulkan ICD shim
   └─ IPC clients to Android host services
```

Core rule:

```text
Linux execution should be low-overhead.
GPU presentation must end on Android Surface/EGL/Vulkan.
Do not rely on guest Linux /dev/dri as the generic solution.
```

---

## Difference From coderredlab/proroot

`coderredlab/proroot` is useful but solves a different layer.

### proroot

Purpose:

- Rootless Linux execution runtime for Android.
- Drop-in PRoot replacement.
- Avoid ptrace overhead with LD_PRELOAD, binary patching, syscall trampolines, and a clean-room glibc-compatible linker.

Relevant components:

```text
libproroot.so           Android/Bionic launcher
libproroot-runtime.so   glibc LD_PRELOAD runtime
libproroot-bridge.so    child exec trampoline
libproroot-linker.so    clean-room glibc-compatible dynamic linker
```

Strength:

- Potentially much faster than ptrace PRoot.
- Claims tested Node, Python, Git, npm, Chromium/Playwright, XFCE/VNC, apt-style workloads.
- Includes wrappers for open/stat/exec/fork/clone/connect/dlopen/fakeroot/procfs/link handling.

Limitations:

- Source is not public.
- Proprietary license; modified binary redistribution not allowed.
- Does not directly solve Android Surface GPU bridge.
- Debuggability is limited to black-box tests, symbols, strings, and runtime logs.

### Our runtime

Purpose:

- Non-root APK Linux runtime plus Android-native GPU/GUI acceleration.
- Guest GUI/GPU commands proxy to Android host renderer.
- Long-term target: Linux apps run with low overhead and render through Android GPU APIs.

Therefore:

```text
proroot can become an optional execution backend.
It does not replace our GPU bridge.
The ideal system may use proroot-style execution + our Android GPU host bridge.
```

---

## Roadmap by Five-Version Bundles

## V36–V40: Make GUI GPU Proof Clean and Trustworthy

**Goal:** Convert V35 functional GPU proof into clean summary PASS with ACK, seq, loss, and per-protocol reporting.

### Task V36: Fix GUI IPC ACK lifecycle

**Objective:** Remove false `SocketException: Socket is closed` after frame reception.

**Files:**

- Modify: `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`
- Test: `tests/test_android_guest_gpu_bridge.py`

**Implementation:**

- Do not use `socket.getInputStream().bufferedReader().useLines { ... }` if ACK must be written after reading.
- Keep socket open until after ACK write.
- Read lines manually or use reader without closing the socket prematurely.
- Write ACK before closing socket/output.

Expected report:

```text
guest wayland gui ipc error=none
guest x11 gui ipc error=none
```

### Task V37: Add explicit GUI ACK stdout

**Objective:** Make guest clients print ACK evidence.

**Files:**

- Modify rootfs payload generation for:
  - `/usr/bin/alr-wayland-gpu-client`
  - `/usr/bin/alr-x11-gpu-client`
- Modify tests checking rootfs content.

Expected guest stdout:

```text
ALR_GUI_IPC_CLIENT ok sent=4 transport=tcp-loopback ack=ALR_GUI_IPC_ACK protocol=WAYLAND received=4 lossless=true
ALR_GUI_IPC_CLIENT ok sent=4 transport=tcp-loopback ack=ALR_GUI_IPC_ACK protocol=X11 received=4 lossless=true
```

### Task V38: Add seq gap and loss metrics

**Objective:** Track loss beyond simple count matching.

Metrics:

```text
guest wayland gui ipc expected seq=1..4
guest wayland gui ipc seq gaps=0
guest wayland gui ipc duplicate seq=0
guest wayland gui ipc out of order=false
guest wayland gui ipc lossless=true
```

Same for X11.

### Task V39: Add per-protocol Surface render counts

**Objective:** Prove Wayland-style and X11-style frames both rendered on host GPU.

Expected Surface report:

```text
surface wayland frames rendered=4
surface x11 frames rendered=4
surface gui total frames rendered=8
surface frames dropped=0
surface frame lossless=true
```

### Task V40: Build, verify, and deliver APK

Commands:

```bash
python3 -m pytest tests -q
scripts/test-native-core.sh
./gradlew :app:assembleDebug --no-daemon
```

APK name:

```text
dist/androlinux-gui-gpu-proof-clean-v40.apk
```

Acceptance criteria on device:

```text
GUEST WAYLAND GUI GPU BRIDGE EXECUTION: PASS
GUEST X11 GUI GPU BRIDGE EXECUTION: PASS
GUEST GUI GPU SURFACE EXECUTION: PASS or Surface callback PASS section present
guest wayland/x11 gui gpu surface hardware render=true
surface gpu hardware render=true
surface frames dropped=0
```

---

## V41–V45: Add Optional Proroot Backend Probe

**Goal:** Evaluate whether proroot-style execution reduces overhead and fixes dpkg/package-manager mutation failures.

### Task V41: Package proroot binaries as optional backend

**Files:**

- Add optional prebuilts under a clearly named directory, preserving upstream filenames.
- Record source URL, release tag, sha256, and license note in docs.

Important:

- Do not modify proroot binaries.
- Mark as optional/proprietary external backend.
- Keep current PRoot backend as baseline.

### Task V42: Add backend selector and report lines

Expected summary:

```text
PROOT BACKEND EXECUTION: PASS
PROROOT BACKEND AVAILABLE: PASS/FAIL
PROROOT VERSION/HELP EXECUTION: PASS/FAIL
```

### Task V43: Run minimal rootfs hello through proroot

Expected report:

```text
proroot hello exit=0
proroot hello stdout=hello from static arm64 rootfs
PROROOT ROOTFS EXECUTION: PASS
```

### Task V44: A/B test dpkg/apt workload

Compare:

```text
proot dpkg -i local deb exit=2
proroot dpkg -i local deb exit=?
```

Acceptance:

- If proroot passes local deb install, prioritize it for package-manager workloads.
- If proroot fails, collect exact stderr and continue with our own backend research.

### Task V45: Deliver A/B runtime APK

APK:

```text
dist/androlinux-proroot-ab-runtime-v45.apk
```

Device acceptance:

```text
PROROOT ROOTFS EXECUTION: PASS
PROROOT DPKG LOCAL INSTALL EXECUTION: PASS/FAIL with detailed reason
```

---

## V46–V50: Start Our Own Clean-Room Low-Overhead Runtime Backend

**Goal:** Begin replacing ptrace-heavy PRoot for hot paths with our own open implementation, inspired only by independently observed behavior and public Linux/Android/glibc interfaces. Do not copy proroot binaries, private code, or non-public implementation details.

### Clean-room rule

Use a two-track process:

```text
Spec track:
  Observe public behavior from Linux APIs, Android constraints, PRoot behavior, device logs, and optional black-box proroot A/B outputs.
  Write ALR runtime requirements and tests without implementation code from proroot.

Implementation track:
  Implement from our own tests/specs using Android NDK, public syscall ABI docs, glibc behavior, and our existing rootfs/runtime code.
```

Allowed references:

- Public Linux syscall ABI and man pages.
- Android NDK/Bionic behavior.
- glibc dynamic loader behavior as observed from public docs and our own rootfs tests.
- Our own device logs and tests.
- Black-box command input/output comparisons, without disassembling or copying proprietary algorithms.

Do not depend on proroot as the long-term implementation. Use it only as an optional benchmark/probe to validate whether the class of approach is worthwhile.

### Design

Create `alr-runtime` as an open, incremental backend:

```text
libalr_runtime_launcher.so   Android/Bionic launcher packaged in nativeLibraryDir
libalr_runtime_hook.so       glibc LD_PRELOAD path/syscall shim inside guest process
libalr_runtime_bridge.so     child exec trampoline for execve/posix_spawn continuity
libalr_runtime_linker.so     optional later clean-room loader only if glibc loader pathing requires it
```

Initial scope is deliberately smaller than a full proroot-style runtime:

- `open/openat/stat/newfstatat/access/readlink/getcwd/chdir`
- `execve`, `execvp`, `posix_spawn` wrapping for child processes
- fake root identity for `getuid/getgid/geteuid/getegid/stat/chown` basics
- procfs minimal virtualization for `/proc/self`, `/proc/self/exe`, `/proc/mounts`, `/proc/cpuinfo`
- path translation for rootfs, cwd, bind-like mappings, and app-private temp dirs
- no aggressive binary patching until LD_PRELOAD wrapper coverage proves insufficient

### Overhead target

The intended overhead reduction is:

```text
current PRoot:
  ptrace trap/context-switch per intercepted syscall

ALR runtime v1:
  in-process wrapper + direct syscall for common filesystem/process calls

ALR runtime v2:
  selective raw-syscall patch/trampoline only for calls that bypass libc wrappers
```

Expected win:

- CLI/userland startup becomes faster.
- npm/node/python/git child-process workloads improve.
- dpkg/apt mutation may avoid some PRoot `Function not implemented` failures.
- GUI/GPU bridge becomes less CPU-bound because guest command producers spend less time in ptrace.

### Acceptance

```text
ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: PASS
ALR LOW-OVERHEAD RUNTIME SHELL EXECUTION: PASS
ALR LOW-OVERHEAD RUNTIME CHILD EXECUTION: PASS
ALR LOW-OVERHEAD RUNTIME DPKG PREFLIGHT: PASS/FAIL with reason
ALR LOW-OVERHEAD RUNTIME SYSCALL OVERHEAD SMOKE: lower than PRoot baseline
```

### Non-goals for V46–V50

- Full Chromium compatibility.
- Full binary patching coverage.
- Full dynamic linker replacement.
- Full X11/Wayland protocol compatibility.
- Full Vulkan/GL ABI compatibility.

Those come after basic shell/child-process/package-manager correctness is proven.

---

## V51–V55: Real GUI Bridge Shape

**Goal:** Move from Wayland/X11-style smoke frames to more realistic GUI protocol bridging.

Wayland path:

- Provide a guest `WAYLAND_DISPLAY` endpoint.
- Implement minimal object/message framing for a tiny subset or proxy shape.
- Translate commit-like events into host compositor frames.

X11 path:

- Provide a guest `DISPLAY` endpoint.
- Start with a tiny X11 proxy or Xvfb/VNC comparison path.
- Extract drawable/window updates into host compositor commands.

Acceptance:

```text
WAYLAND DISPLAY SOCKET AVAILABLE: PASS
X11 DISPLAY SOCKET AVAILABLE: PASS
GUEST GUI APP FRAME COMMIT EXECUTION: PASS
ANDROID GPU SURFACE COMPOSITOR EXECUTION: PASS
```

---

## V56–V60: GLES/EGL Shim Expansion

**Goal:** Make Linux GL apps talk to a guest-side libEGL/libGLES shim that forwards commands to Android host renderer.

Start with a constrained API subset:

- `eglGetDisplay`
- `eglInitialize`
- `eglChooseConfig`
- `eglCreateContext`
- `eglMakeCurrent`
- `glClearColor`
- `glClear`
- `eglSwapBuffers`

Acceptance:

```text
GUEST EGL INIT VIA SHIM: PASS
GUEST GLES CLEAR VIA SHIM: PASS
GUEST EGL SWAP VIA ANDROID SURFACE: PASS
GUEST GLES HARDWARE RENDER: PASS
```

---

## V61–V70: Buffer/Texture Transport and Frame Pacing

**Goal:** Move beyond clear-color commands into real frame buffers/textures while minimizing copies.

Stages:

1. CPU shared memory frame upload.
2. Tiled/dirty-rect updates.
3. Android `AHardwareBuffer` exploration.
4. Fence/ACK frame pacing.
5. Latency measurements.

Metrics:

```text
frames submitted
frames rendered
frames dropped
avg submit-to-render ms
p95 submit-to-render ms
copy count estimate
shared buffer bytes/frame
```

Acceptance:

```text
GUEST GUI BUFFER FRAME EXECUTION: PASS
ANDROID GPU TEXTURE UPLOAD EXECUTION: PASS
FRAME PACING LOSSLESS EXECUTION: PASS
```

---

## V71–V80: Vulkan Host Renderer and ICD Proxy Research

**Goal:** Add Vulkan host path after GLES proof is reliable.

Do not begin with full DXVK/Wine. Start with:

- Android Vulkan instance/device/swapchain proof.
- Guest Vulkan ICD stub that reports basic info.
- One simple clear/present path.

Acceptance:

```text
ANDROID HOST VULKAN SURFACE EXECUTION: PASS
GUEST VULKAN ICD SMOKE EXECUTION: PASS
GUEST VULKAN CLEAR PRESENT EXECUTION: PASS
```

---

## V81+: Real App Compatibility Track

Target app classes in this order:

1. CLI apps: shell, git, Python, Node, npm.
2. Package manager: dpkg/apt local install and network install.
3. Simple GUI: xeyes-like, GTK hello, Qt hello.
4. Browser engine: Chromium/Playwright smoke.
5. GPU demos: glmark2-style subset, es2gears equivalent.
6. Heavy apps only after metrics are stable.

Acceptance for each app:

```text
app launches
input works
frame renders via Android GPU
renderer is not SwiftShader/llvmpipe/softpipe
frame loss/dropped metrics acceptable
latency measured
```

---

## Non-Negotiable Verification Rules

- Never claim native-grade acceleration from APK build alone.
- Device logs are source of truth.
- Always report renderer string and software-renderer rejection.
- Keep PRoot and new runtime backends A/B comparable.
- Keep APK names unique per bundle.
- Deliver APK at every five-version boundary.
- If a summary says FAIL but detailed evidence says PASS, fix the summary/reporting before adding more layers.

---

## Immediate Next Step

Continue with V36–V40:

1. Fix ACK lifecycle bug.
2. Add explicit GUI ACK stdout.
3. Add seq/loss/gap metrics.
4. Add per-protocol Surface render counts.
5. Build and deliver `androlinux-gui-gpu-proof-clean-v40.apk`.

After V40 device PASS, proceed to V41–V45 proroot A/B backend probe.
