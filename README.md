# Android on Linux Runtime

Goal: build a non-root Android APK runtime that can launch ARM64 glibc Linux apps and bridge Linux GUI/GPU output to Android public APIs with an app-by-app native window experience.

Working product definition:

> GPU-vendor-independent, Android-API-backed, hardware-accelerated Linux-on-Android runtime.

This repository is seeded from 박찬우's `Meapri/Plib` work at reference commit `ce92165fab26a5a13cbb35fd75eb0bab2959fb5a`. Plib is treated as an owned upstream design and implementation source for this project, while Termux/PRoot/UserLAnd/Andronix/AVF-style systems remain prior-art and comparison baselines.

## Current strategy

1. Prove an APK-owned runtime can launch a bundled native loader from the package native library directory.
2. Keep writable rootfs data in app-private storage, but do not directly execute app-data binaries on modern Android.
3. Use a launcher/loader/proot-style runtime as the initial rootfs execution substrate.
4. Add GUI output through an Android-owned Surface.
5. Start graphics acceleration with Android-owned EGL/GLES rendering and a minimal guest GLES shim.
6. Add Android public Vulkan proof in stages: host capability probe, `Surface` capability probe, swapchain clear/present, then a guest Vulkan ICD/proxy.
7. Research a proroot-style low-overhead runtime separately from the graphics bridge.

## Planning documents

Start here when changing project direction or implementing a new runtime layer:

- [Product requirements](docs/product-requirements.md): product target, goals, non-goals, acceptance ladder, and risk model.
- [Prior art research](docs/research/prior-art.md): Termux, Termux:X11, proot-distro, UserLAnd, Andronix, Local Desktop, FluxLinux, Plib, and AVF/gfxstream comparison.
- [Clean-room protocol](docs/clean-room-protocol.md): rules for using open-source components, optional closed/proprietary probes, and black-box behavior evidence without contaminating ALR implementation.
- [ALR execution backend spec](docs/alr-execution-backend-spec.md): staged design for the open low-overhead runtime backend that should eventually outperform PRoot on hot paths.
- [Execution backend architecture](docs/architecture/execution-backend.md): product-facing architecture summary for PRoot baseline, ALR target, Android W^X handling, and process continuity.
- [Android graphics bridge spec](docs/android-graphics-bridge-spec.md): Surface/EGL/GLES/Vulkan bridge target for Linux GUI/GPU output.
- [GPU/display bridge architecture](docs/architecture/gpu-display-bridge.md): product-facing architecture summary for app-owned Android surfaces, Wayland-shaped ingress, GLES shim, and Vulkan research.
- [Device evidence model](docs/architecture/device-evidence.md): proof rules before claiming manufacturer-independent GPU acceleration.
- [ADR 0001](docs/adr/0001-runtime-model.md): no-root, no-VM APK runtime model.
- [ADR 0002](docs/adr/0002-gpu-public-api.md): Android public GPU/display path.
- [Implementation milestones](docs/plans/implementation-milestones.md): five-version bundle plan, acceptance strings, and stop conditions.
- [Agent coordination](docs/agent-coordination.md): Codex/Hermes ownership boundaries, handoff format, and conflict rules.
- [Parallel workstreams](docs/plans/parallel-workstreams.md): recommended split for clean ALR runtime work and device/probe evidence work.

## Repository layout

```text
app/                         Android APK skeleton
app/src/main/cpp/            Native loader/runtime entrypoint
app/src/main/java/           Kotlin UI entrypoint
docs/                        Architecture, risks, and PoC roadmap
scripts/                     Bootstrap and validation scripts
tests/                       Host-side policy/model tests
tools/                       Host-side planning helpers
```

## First PoC target

PoC 1: APK launches a native loader from `nativeLibraryDir`, points it at `files/rootfs/debian-arm64`, and emits a deterministic launch plan for `/bin/bash` without directly execing app-data binaries.

## Host tests

```bash
python3 -m pytest tests/ -q
```
