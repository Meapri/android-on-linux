# GPU and Display Bridge Architecture

Android on Linux Runtime must accelerate Linux GUI/GPU workloads through Android public graphics APIs, the same broad driver path used by ordinary Android apps and games. The generic path must not depend on vendor-private devices or Linux desktop GPU nodes.

## Rendering Stack

```text
Linux guest GUI/GL/Vulkan client
        |
        v
Guest shim or protocol client
  Wayland-shaped, X11-shaped, EGL/GLES shim, Vulkan ICD/proxy
        |
        v
Bridge transport
  text command TCP, binary framing, then shared/buffer transport
        |
        v
Android host renderer
  Surface, ANativeWindow, EGL/GLES, later Vulkan
        |
        v
Android GPU driver
  Adreno, Mali/Immortalis, Tensor-class, Xclipse, PowerVR
```

## Non-Negotiable GPU Rules

- Do not use `/dev/dri`, KMS, GBM, KGSL, Turnip, Freedreno, or Mesa vendor-driver bundles as the default path.
- Do not claim manufacturer-independent GPU acceleration until the same APK passes on at least Adreno, Mali/Immortalis, and Tensor-class devices.
- Keep software rendering and SwiftShader as diagnostic fallback only, not as success criteria.
- Keep graphics bridge code independent from the execution backend so PRoot and ALR can both drive the same renderer tests.

## App-by-App Window Model

The v1 UX maps each Linux app surface to an Android-owned visible surface. A full desktop session can be a later compatibility mode, but the primary experience is app-specific:

- launcher selects one Linux app,
- Android host creates and owns the target surface,
- guest GUI protocol commits frames,
- Android handles focus, orientation, lifecycle, resize, and input,
- diagnostics report frame count, dropped frames, renderer name, and protocol path.

## Staged Implementation

1. Host EGL/GLES pbuffer probe reports renderer and hardware candidate status.
2. Android `SurfaceView`/`ANativeWindow` clear test submits visible frames.
3. Guest command bridge drives host clear colors and receives ACKs.
4. Wayland-shaped and X11-shaped smoke clients commit simple frames.
5. Guest `libEGL`/`libGLES` shim forwards a minimal clear/swap subset.
6. Vulkan host probe creates an instance, enumerates physical devices, and checks Android surface extension support. Vulkan surface probe checks queue-family present support, surface formats, and present modes for the actual Android `Surface`. Vulkan surface renderer creates an Android swapchain, records clear-color render-pass commands, presents guest-driven frames, and reports lossless frame evidence through public Android Vulkan WSI.
7. Guest Vulkan ICD/proxy supports device enumeration and clear-present.

## Wayland First, X11 Later

Wayland-shaped ingress is the first GUI protocol because it maps more cleanly to app surfaces and modern GTK/Qt. X11 support should begin as smoke/proxy coverage only, then expand through Xwayland or a constrained X server path after the Android surface and input model is stable.

## Input and Lifecycle

The Android host owns input routing and lifecycle. The bridge must translate:

- touch and mouse pointer events,
- keyboard text and key codes,
- focus changes,
- resize and density changes,
- pause/resume/surface-loss events,
- frame pacing and backpressure.

Every lifecycle transition should produce report lines so real device failures are diagnosable without guessing.
