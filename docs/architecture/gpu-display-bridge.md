# GPU and Display Bridge Architecture

Status: revised 2026-10-11 ([ADR 0004](../adr/0004-unified-direction.md)). The earlier version described a text-command TCP bridge and "Wayland-shaped" smoke clients. Those stages are done and replaced by the stack below.

Android on Linux Runtime must accelerate Linux GUI/GPU workloads through Android public graphics APIs, the same broad driver path used by ordinary Android apps and games. The generic path must not depend on vendor-private devices or Linux desktop GPU nodes ([ADR 0002](../adr/0002-gpu-public-api.md)).

## Rendering Stack

```text
Linux guest GUI/GL/Vulkan client
        |
        |  GUI: Wayland (xdg-shell, wl_shm, zwp_linux_dmabuf_v1, text-input-v3, wl_seat)
        |       Xwayland (rootful) for X11-only apps
        |  GL:  guest libEGL.so.1 / libGLESv2.so.2 shim, client-side virtual GL IDs
        |  VK:  guest libvulkan.so.1 ICD with generated marshalling
        v
Shared-memory SPSC command ring (8 MiB in the glmark2 runs)
        v
In-app GPU executor (owns the real EGL/GLES 3.2 and Vulkan contexts)
        v
Android GPU driver (Mali on the reference device)
        v
AHardwareBuffer -> EGLImage -> in-app Wayland compositor -> SurfaceView / ANativeWindow
```

The compositor was originally planned as "Wayland-shaped ingress" with constrained proxy protocols. It is now a real compositor built on vendored libwayland, so GTK3, Qt6, SDL2, and native Wayland clients connect without bridge-specific code.

## Non-Negotiable GPU Rules

- Do not use `/dev/dri`, KMS, GBM, KGSL, Turnip, Freedreno, or Mesa vendor-driver bundles as the default path.
- Do not load vendor GPU drivers into glibc guest processes (libhybris-style). It breaks the vendor-independence rule and is not adopted ([ADR 0004, D5](../adr/0004-unified-direction.md)).
- Do not claim manufacturer-independent GPU acceleration until the same APK passes on at least Adreno, Mali/Immortalis, and Tensor-class devices. Today the evidence covers one Mali-G615 device.
- Keep software rendering and SwiftShader as diagnostic fallback only, not as success criteria. CPU-rendered toolkits (Cairo, Qt raster) reach the screen through `wl_shm` and the zero-copy present path, and they are not GPU-render claims.
- Keep the graphics services independent from the execution engine. This is a design intent that is **not yet proven on the alr engine**: the GPU evidence below was captured on the legacy in-process loader, and ADR 0004 phase P1 re-measures it.

## App-by-App Window Model

The v1 UX maps each Linux app surface to an Android-owned visible surface. A full desktop session can be a later compatibility mode, but the primary experience is app-specific:

- launcher selects one Linux app,
- Android host creates and owns the target surface,
- the guest GUI protocol commits frames,
- Android handles focus, orientation, lifecycle, resize, and input,
- diagnostics report frame count, dropped frames, renderer name, and protocol path.

v1 shows one foreground app ([design ADR-004, D1](../design/adr-004-inapp-catalog-ux.md)). The v2 candidate maps each window to its own Android Task, as tawc does.

## Implementation Status

| Stage | State | Engine of the evidence |
| --- | --- | --- |
| Host EGL/GLES probe, `SurfaceView`/`ANativeWindow` clear | Done | n/a (host side) |
| Guest command bridge, then shared-memory ring | Done; the ring superseded the TCP text bridge as the GPU data path | legacy loader |
| Wayland compositor, input injection, IME | Done; GUI set in [gui-universality-status](../research/gui-universality-status.md) | legacy loader, and alr for the `sd` builds |
| Guest GLES shim: shaders, VBO, textures, draws, GLES3 ops | Done; glmark2 Score 1074 (1052 with the real `GL_RENDERER`) | legacy loader |
| Zero-copy present (`AHardwareBuffer` to `EGLImage`) | Done | legacy loader |
| Vulkan: host probe, surface renderer, guest ICD enumerate/render/draw/present | Done on the reference device | legacy loader |
| Same stack on the alr engine | **Open (P1)**: ring fd handoff, shim placement, glmark2, Vulkan | alr |
| Adreno and Tensor-class evidence | **Open** | none |

Honest limits recorded in [PERFORMANCE §5](../PERFORMANCE.md): absolute percentage of a hand-optimized native app is unmeasured, glmark2 covers two scenes of the suite, and `glMapBufferRange`/`glGetStringi` have documented gaps in the ring protocol.

## Vulkan Bring-up Stages (completed on the reference device)

The Vulkan path was built in the following order. Each step has a report gate and device evidence.

1. Vulkan host probe creates an instance, enumerates physical devices, and checks Android surface extension support.
2. Vulkan surface probe checks queue-family present support, surface formats, and present modes for the actual Android `Surface`.
3. Vulkan surface renderer creates an Android swapchain, records clear-color render-pass commands, presents guest-driven frames, and reports lossless frame evidence through public Android Vulkan WSI.
4. Guest Vulkan ICD (`libvulkan.so.1`) marshals device enumeration, device and queue creation, command buffers, graphics-pipeline draws, and swapchain present over the ring to the real Mali `libvulkan`.

## Wayland First, X11 Through Xwayland

Wayland is the first GUI protocol because it maps to app surfaces and modern GTK/Qt. X11-only apps are routed to a rootful Xwayland that is itself a Wayland client of the compositor. The decision is made from the binary (libX11 present, libwayland-client absent), not from a per-app list.

## Input and Lifecycle

The Android host owns input routing and lifecycle. The bridge must translate:

- touch and mouse pointer events,
- keyboard text and key codes,
- focus changes,
- resize and density changes,
- pause/resume/surface-loss events,
- frame pacing and backpressure.

Every lifecycle transition should produce report lines so real device failures are diagnosable without guessing.
