# Android Graphics Bridge Specification

## Status

Draft v0.1. This document defines the graphics and GUI bridge target for running Linux GUI/GPU clients inside an Android APK without depending on guest access to Linux DRM/KMS devices.

## Objectives

- Present Linux GUI output through an Android-owned `Surface`.
- Use Android host GPU APIs for hardware acceleration.
- Keep execution backend and graphics bridge independent.
- Support multiple Android GPU vendors through public APIs.
- Build from small, measurable protocol stages rather than claiming full Wayland/X11/OpenGL/Vulkan compatibility early.

## Non-Goals

- Direct guest access to `/dev/dri`, KMS, GBM, or DRM nodes as the default path.
- Full Wayland compositor implementation in early milestones.
- Full X11 server implementation in early milestones.
- Full OpenGL desktop profile compatibility.
- Full Vulkan, DXVK, Wine, or Proton compatibility in early milestones.
- GPU driver reverse engineering.

## Architecture

```text
Linux guest process
  GUI client or GL/Vulkan client
        |
        v
Guest shim or proxy client
  Wayland-shaped, X11-shaped, EGL/GLES, or Vulkan ICD subset
        |
        v
IPC transport
  loopback TCP first, Unix socket later, shared memory/buffer transport later
        |
        v
Android host bridge service
  validates messages, tracks frames, owns renderer state
        |
        v
Native renderer
  EGL/GLES first, Vulkan later
        |
        v
Android Surface / ANativeWindow
        |
        v
Android vendor GPU driver
```

## Host Responsibilities

The Android host APK owns:

- Activity and Service lifecycle.
- `SurfaceView` or `TextureView`.
- `ANativeWindow` conversion.
- Renderer context creation.
- GPU API selection.
- IPC server lifecycle.
- Permission prompts.
- Device capability reporting.
- Frame pacing and ACK.
- Diagnostics visible in the app UI.

## Guest Responsibilities

The Linux guest side owns:

- Small command clients for smoke tests.
- Protocol-shaped GUI bridge clients.
- `libEGL`/`libGLES` shim for constrained GL workloads.
- Future Vulkan ICD/proxy.
- Environment-driven bridge connection settings.

Guest bridge environment:

```text
ALR_GPU_BRIDGE_HOST=127.0.0.1
ALR_GPU_BRIDGE_PORT=<port>
ALR_GPU_BRIDGE_TRANSPORT=tcp-loopback

ALR_GUI_BRIDGE_HOST=127.0.0.1
ALR_GUI_BRIDGE_PORT=<port>
ALR_GUI_BRIDGE_PROTOCOL=WAYLAND|X11

WAYLAND_DISPLAY=<future socket name>
DISPLAY=<future display name>
```

## Transport Stages

### Stage 1: Text Command TCP

Purpose:

- Prove guest-to-host communication.
- Prove frame sequencing and ACK.
- Keep debugging easy.

Example message:

```text
FRAME seq=1 protocol=WAYLAND r=0.05 g=0.18 b=0.45 label=guest
```

Example ACK:

```text
ALR_GUI_IPC_ACK protocol=WAYLAND received=4 lossless=true
```

### Stage 2: Binary Framing

Purpose:

- Reduce parsing overhead.
- Add message lengths and versioning.
- Prepare for buffer handles and shared-memory descriptors.

Header draft:

```text
magic: ALRG
version: u16
type: u16
length: u32
seq: u64
payload: bytes
```

### Stage 3: Shared Memory or Buffer Upload

Purpose:

- Avoid sending full frames as text or tiny commands.
- Support dirty rectangles.
- Measure copy count and latency.

Candidate paths:

- App-private ashmem/memfd-like path if available.
- TCP with binary payload for initial portability.
- Android `AHardwareBuffer` exploration for later host-managed buffers.

### Stage 4: Fence and Frame Pacing

Purpose:

- Avoid guest overproduction.
- Track present completion.
- Add frame deadlines and backpressure.

## Renderer Stages

### Renderer v0: Host EGL/GLES Pbuffer Probe

Proves Android EGL context creation and hardware renderer detection without a UI surface.

Report:

```text
host gpu egl available=true
host gpu renderer=<renderer>
host gpu hardware candidate=true
```

### Renderer v1: Android Surface Clear

Proves rendering to an app-owned visible `Surface`.

Report:

```text
surface frames submitted=<n>
surface frames rendered=<n>
surface frames dropped=0
surface gpu hardware render=true
```

### Renderer v2: Guest Command Surface

Guest commands drive host clear colors and frame labels.

Report:

```text
guest gpu bridge command execution=PASS
guest gpu ipc bridge execution=PASS
surface frame lossless=true
```

### Renderer v3: GUI Protocol-Shaped Surface

Wayland-shaped and X11-shaped clients send frame commits.

Report:

```text
guest wayland gui bridge execution=PASS
guest x11 gui bridge execution=PASS
surface wayland frames rendered=4
surface x11 frames rendered=4
surface gui total frames rendered=8
```

### Renderer v4: Guest GLES Shim

Guest `libEGL`/`libGLES` shim forwards a constrained command subset to host GLES.

Required subset:

- `eglGetDisplay`
- `eglInitialize`
- `eglChooseConfig`
- `eglCreateContext`
- `eglMakeCurrent`
- `glClearColor`
- `glClear`
- `eglSwapBuffers`

Report:

```text
guest egl init via shim=PASS
guest gles clear via shim=PASS
guest egl swap via android surface=PASS
guest gles hardware render=PASS
```

### Renderer v5: Vulkan Host Renderer

Host Vulkan capability probe comes first. It proves Android Vulkan loader availability, instance creation, physical device enumeration, hardware-vs-software classification, and `VK_KHR_android_surface` support. The current Vulkan swapchain clear/present renderer then binds the same Android-owned `Surface`, selects a graphics+present queue family, creates a `VK_KHR_swapchain` chain, clears each guest-driven frame through a render pass, and presents it through Android's public Vulkan WSI path.

Report:

```text
android host vulkan probe execution=PASS
host vulkan device=<name>
host vulkan android surface extension=true
android host vulkan surface probe execution=PASS
host vulkan surface supported=true
host vulkan surface formats count=<n>
host vulkan surface present modes count=<n>
android host vulkan surface execution=PASS
host vulkan surface renderer=android-vulkan-swapchain-clear-present
vulkan surface frames rendered=<n>
vulkan surface frame lossless=true
host vulkan surface hardware render=true
guest vulkan clear present hardware render=true
```

### Renderer v6: Guest Vulkan ICD/Proxy

Guest Vulkan ICD reports device info and forwards clear/present subset.

Report:

```text
guest vulkan icd smoke execution=PASS
guest vulkan clear present execution=PASS
```

## Wayland-Shaped Path

Early Wayland work should not claim full compositor support.

Initial model:

- Provide a guest endpoint that looks like a display target to project-owned clients.
- Encode minimal surface create/commit semantics.
- Translate commits into host renderer frame commands.
- Add ACK and sequence tracking.

Future model:

- Implement enough protocol framing to run a tiny known client.
- Consider using or studying open-source Wayland components if license and scope fit.
- Keep protocol support documented as a subset.

## X11-Shaped Path

Early X11 work should not claim full X server support.

Initial model:

- Project-owned X11-shaped smoke client sends drawable updates.
- Host bridge renders frames and reports ACK.

Future model:

- Evaluate Xvfb/VNC comparison path.
- Evaluate a tiny X proxy for simple windows.
- Translate image or GL-present paths into host renderer frames.

## GLES Shim Model

Guest-side shim responsibilities:

- Export the small EGL/GLES symbols required by smoke apps.
- Track fake display/config/context/surface handles.
- Serialize GL commands to host bridge.
- Return deterministic errors for unsupported symbols.

Host-side responsibilities:

- Maintain real EGL context and surface.
- Map guest context state to host context state.
- Execute supported commands.
- Send swap ACK with frame id.

Unsupported command behavior:

```text
return documented failure
log unsupported symbol once
report GUEST GLES UNSUPPORTED SYMBOL=<name>
```

## Vulkan Proxy Model

Vulkan comes after GLES.

Initial guest ICD responsibilities:

- Register as an ICD inside the rootfs.
- Report one virtual physical device backed by Android host Vulkan.
- Support instance/device creation subset.
- Support one clear/present path.

Host responsibilities:

- Create Android Vulkan instance.
- Select physical device.
- Create swapchain for Android Surface.
- Execute clear/present command.

Non-goals:

- DXVK.
- Full descriptor/pipeline ecosystem.
- WSI parity beyond the test surface.

## Synchronization and Backpressure

Every frame-carrying protocol must track:

- `seq`.
- submit time.
- host receive time.
- render start.
- render completion.
- ACK send time.
- guest ACK receive time where available.

Lossless criteria:

```text
received == submitted
rendered == received
seq gaps == 0
duplicates == 0
out_of_order == false
frames dropped == 0
```

## Security and Robustness

IPC parsers must:

- Limit message size.
- Reject malformed numeric fields.
- Reject unknown protocol versions.
- Bound frame queues.
- Close idle clients.
- Avoid blocking UI thread.
- Avoid trusting guest file paths for host resource access.

Renderer must:

- Release EGL/Vulkan resources on lifecycle events.
- Survive Surface recreation.
- Report lost context/device conditions.
- Avoid leaking host Android details to guest clients.

## Device Matrix

Minimum device categories:

- Mali device.
- Adreno device.
- Xclipse or other Vulkan-capable Samsung device where available.
- Emulator or desktop Android target for CI-adjacent smoke.

Per-device evidence:

```text
device model
android version
sdk
gpu renderer
egl version
vulkan availability
surface frame report
guest bridge report
thermal or stability note
```

## Integration With Execution Backends

The graphics bridge must work with:

- PRoot baseline.
- Optional external low-overhead backend.
- ALR runtime.

Rules:

- Bridge protocol must not depend on backend-private behavior.
- Guest clients receive connection data through environment variables.
- Report format includes backend name.
- Graphics tests must run while execution backend A/B tests remain comparable.

## Milestone Acceptance

### GUI/GPU MVP

```text
HOST GPU EGL/GLES EXECUTION: PASS
HOST GPU SURFACE EXECUTION: PASS
GUEST GPU IPC BRIDGE EXECUTION: PASS
GUEST WAYLAND GUI GPU BRIDGE EXECUTION: PASS
GUEST X11 GUI GPU BRIDGE EXECUTION: PASS
GUEST GUI GPU SURFACE EXECUTION: PASS
```

### GLES MVP

```text
GUEST EGL INIT VIA SHIM: PASS
GUEST GLES CLEAR VIA SHIM: PASS
GUEST EGL SWAP VIA ANDROID SURFACE: PASS
GUEST GLES HARDWARE RENDER: PASS
```

### Vulkan MVP

```text
ANDROID HOST VULKAN PROBE EXECUTION: PASS
ANDROID HOST VULKAN SURFACE PROBE EXECUTION: PASS
ANDROID HOST VULKAN SURFACE EXECUTION: PASS
GUEST VULKAN ICD SMOKE EXECUTION: PASS
GUEST VULKAN CLEAR PRESENT EXECUTION: PASS
```

## Open Questions

- Which transport should replace loopback TCP first: Unix socket, binary TCP, or shared memory?
- Can `AHardwareBuffer` provide a practical cross-boundary buffer path for guest-generated frames?
- How much Wayland protocol is worth implementing before using an existing compositor/proxy component?
- Should X11 support begin with image transport, GLX proxy research, or Xvfb/VNC comparison?
- Which Vulkan subset is small enough to be credible but useful enough to guide the ICD design?
