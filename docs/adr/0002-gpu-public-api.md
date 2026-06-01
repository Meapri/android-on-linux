# ADR 0002: GPU and Display Path

## Status

Accepted.

## Decision

The generic GPU/display path will use Android public graphics APIs:

- Java/Kotlin `SurfaceView` or equivalent host surface owner.
- NDK `ANativeWindow` for native rendering.
- EGL/GLES first.
- `AHardwareBuffer` for later shared-buffer work.
- Vulkan host renderer and guest ICD/proxy after GLES proof is stable.

The generic path must not depend on `/dev/dri`, KMS, GBM, KGSL, Turnip, Freedreno, Panfrost, or other vendor/private Linux GPU driver paths.

## Context

The user requirement is GPU acceleration similar to ordinary Android games across manufacturers. Android's public graphics stack is the only path that can plausibly satisfy that goal for a normal APK. Linux desktop GPU device nodes and Mesa driver stacks are useful research references, but they are not a manufacturer-independent Android app contract.

## Consequences

- Surface/EGL/GLES proof comes before Wayland, X11, GLES shim expansion, or Vulkan claims.
- Guest graphics clients talk to the Android host through a bridge/shim/proxy.
- Wayland-shaped ingress is the primary GUI protocol direction. X11 comes later as smoke/proxy or Xwayland compatibility.
- Software renderers are diagnostic only and must not satisfy GPU acceleration claims.
- Vendor-independent acceleration requires the same APK to pass on multiple GPU families.

## Verification Gates

Minimum gates for any broad GPU claim:

- `HOST GPU EGL/GLES EXECUTION: PASS`
- `HOST GPU SURFACE EXECUTION: PASS`
- `GUEST GPU IPC BRIDGE EXECUTION: PASS`
- `GUEST GUI GPU SURFACE EXECUTION: PASS`
- Renderer is not SwiftShader, llvmpipe, softpipe, lavapipe, or another software rasterizer.
- Evidence covers Adreno, Mali/Immortalis, and Tensor-class devices.
- Evidence states that no vendor-specific driver bundle or private device-node path was used.
