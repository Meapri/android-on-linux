# AndroLinux Runtime Product Requirements

## Purpose

AndroLinux Runtime is a non-root Android application runtime for running real Linux/glibc arm64 applications inside a normal APK while presenting GUI and GPU output through Android-native graphics APIs.

The product target is not a privileged container, not a virtual machine, and not a Termux replacement shell. It is an APK-owned Linux application environment:

- Linux userland lives in an app-private rootfs.
- Execution enters through packaged native code in `nativeLibraryDir`.
- The runtime avoids PRoot-style ptrace overhead for hot paths where possible.
- GUI and GPU output terminates on Android `Surface`, `EGL/GLES`, and later `Vulkan`.
- Android permissions, lifecycle, storage, networking, media, and hardware access remain controlled by the host APK.

## North Star

Run practical Linux desktop and developer applications on stock Android without root:

```text
Android APK
  owns permissions, lifecycle, Surface, renderer, IPC services
        |
        v
Low-overhead Linux execution backend
  launches and mediates glibc rootfs programs
        |
        v
Linux apps
  CLI, package tools, GUI apps, GL/Vulkan clients
        |
        v
Guest shims and proxy protocols
  translate GUI/GPU/media/hardware requests
        |
        v
Android public APIs
  Surface, EGL/GLES, Vulkan, MediaCodec, USB, sensors, NNAPI
```

## Goals

### Execution

- Launch Linux/glibc arm64 binaries from an app-private rootfs without root.
- Keep execution compatibility at least as good as the current packaged PRoot baseline for smoke workloads.
- Reduce execution overhead significantly below ptrace-heavy PRoot for common filesystem and process-heavy workloads.
- Reach at least proroot-class behavior for supported dynamic glibc programs, while keeping the implementation open and maintainable.
- Preserve a PRoot-compatible fallback backend until the low-overhead backend is proven across real workloads.

### GUI and GPU

- Render GUI output into an Android-owned `Surface`.
- Use Android host `EGL/GLES` for the first hardware-accelerated rendering path.
- Add a guest `libEGL`/`libGLES` shim for a constrained API subset before broad GL compatibility claims.
- Add a Vulkan host renderer and guest Vulkan ICD/proxy only after GLES proof is stable.
- Avoid depending on Linux DRM/KMS or `/dev/dri` as the generic rendering path.
- Keep the graphics bridge GPU-vendor-independent: Adreno, Mali, Xclipse, PowerVR, and desktop Android targets should all use Android public graphics APIs.

### Packaging and UX

- Package as a normal Android APK/AAB.
- Keep rootfs assets verified before extraction.
- Keep mutable rootfs state under app-private storage.
- Avoid broad storage permissions unless a user-facing feature explicitly requires them.
- Provide deterministic visible reports for every runtime and graphics smoke test.
- Provide developer diagnostics without exposing Android host environment variables to guest processes by default.

## Non-Goals

- Root access, Magisk modules, system image modification, or privileged helper installation.
- Full kernel virtualization.
- Full Docker, cgroups, systemd, namespaces, or mount parity.
- Direct access to host DRM/KMS nodes as the default graphics path.
- Immediate SteamOS, Proton, DXVK, or full desktop environment compatibility.
- Claiming security isolation beyond Android's app sandbox and the runtime's documented mediation.
- Copying proprietary runtime internals from closed binaries.

## User Personas

### Android Power User

Wants a Linux environment that behaves like an app, not a rooted device project. They expect install, launch, visible progress, and uninstall to be ordinary Android flows.

### Developer on Android

Wants shell tools, Python, Node, Git, package manager workflows, local servers, and editor-adjacent tasks without carrying a separate Linux machine.

### GUI Linux App User

Wants simple desktop-like Linux applications to display through the Android screen with acceptable responsiveness and hardware-accelerated rendering.

### Runtime Developer

Wants a clean, testable codebase where PRoot, optional proprietary probes, and the open ALR runtime can be compared without entangling implementation sources.

## Product Principles

- Android is the host platform, not an obstacle to hide.
- Linux apps see Linux-like APIs only where the runtime explicitly supports them.
- Unsupported kernel features must fail clearly and with diagnostics.
- Every backend must be A/B comparable through the same rootfs and report format.
- Runtime execution and graphics acceleration must stay loosely coupled.
- Keep clean-room boundaries documented before implementation.
- Device evidence beats architecture optimism.

## Runtime Backend Strategy

### Baseline Backend: Packaged PRoot

The current repo packages a Termux PRoot-derived backend. It remains the compatibility baseline because it already runs static and dynamic rootfs smoke tests under stock Android constraints.

Expected role:

- Compatibility fallback.
- Test oracle for baseline behavior.
- Regression guard for rootfs extraction, command launch, and report plumbing.

Limitations:

- Ptrace stop/resume overhead on syscall-heavy workloads.
- Device-specific ptrace/seccomp interactions.
- Package manager mutation limitations observed in current roadmap evidence.
- GPL obligations must remain explicit for redistributed artifacts.

### Optional Probe Backend: proroot-Class External Runtime

Closed or third-party low-overhead runtimes may be packaged as optional, unmodified external backends for black-box A/B testing.

Allowed role:

- Performance and compatibility comparison.
- Evidence that a low-ptrace or zero-ptrace execution model is worthwhile.
- Test-case discovery through command input/output, exit codes, logs, and timing.

Disallowed role:

- Source for ALR implementation internals.
- Disassembly-derived algorithm source.
- Modified binary redistribution.
- Long-term product dependency unless licensing and maintenance constraints are acceptable.

### Target Backend: Open ALR Runtime

The open ALR runtime is the long-term execution backend.

Target components:

```text
libalr_runtime_launcher.so
  Android/Bionic launcher packaged in nativeLibraryDir

libalr_runtime_hook.so
  glibc LD_PRELOAD library for filesystem, process, identity, and procfs mediation

libalr_runtime_bridge.so
  child exec trampoline for execve/posix_spawn continuity

libalr_runtime_linker.so
  optional later clean-room loader if standard guest dynamic loader setup is insufficient
```

Initial success is not full Linux parity. Initial success is a measured path from:

```text
hello -> shell -> child process -> dynamic glibc binary -> dpkg preflight -> package install subset
```

## Graphics Strategy

### Stage 1: Host Surface Renderer

The Android app owns a `SurfaceView` or `TextureView`. Native host code renders known frames with Android `EGL/GLES` and reports renderer, frame count, drops, and loss.

### Stage 2: Guest Command Bridge

Guest binaries send simple frame commands to the host renderer over loopback or Unix-domain IPC. The host reports sequence, ACK, frame loss, and protocol identity.

### Stage 3: GUI Protocol Shape

Introduce Wayland-shaped and X11-shaped ingress endpoints. These are initially constrained proxy protocols, not full compositors.

### Stage 4: GLES Shim

Guest `libEGL`/`libGLES` shims implement a small, testable subset:

- `eglGetDisplay`
- `eglInitialize`
- `eglChooseConfig`
- `eglCreateContext`
- `eglMakeCurrent`
- `glClearColor`
- `glClear`
- `eglSwapBuffers`

### Stage 5: Vulkan Proxy

After GLES is stable, add a host Vulkan renderer and a guest Vulkan ICD/proxy. The first target is device enumeration and Android `Surface` clear/present through `VK_KHR_android_surface` and `VK_KHR_swapchain`, not DXVK.

## Acceptance Ladder

### Level 0: Host Tests

- Python source tests pass.
- Native host C++ path/env tests pass.
- Rootfs manifest and safe extraction tests pass.

### Level 1: APK Launch

- APK installs on target device.
- App loads native library.
- Native launch plan is visible.
- Rootfs is verified and extracted.

### Level 2: PRoot Baseline

- Static rootfs `hello` passes.
- Shell script passes.
- Dynamic glibc binary passes.
- `id` reports fakeroot-compatible root identity where configured.
- Android host environment does not leak into guest.

### Level 3: GUI/GPU Proof

- Host EGL/GLES renderer reports hardware renderer.
- Guest frame commands reach the host renderer.
- Wayland-style and X11-style smoke clients receive ACK.
- Surface frames are lossless for the smoke bundle.

### Level 4: Low-Overhead Runtime MVP

- ALR runtime executes `hello`.
- ALR runtime executes `/bin/sh -c`.
- ALR runtime preserves child execution across `execve` and `posix_spawn`.
- ALR runtime passes path translation, cwd, procfs, and identity smoke tests.
- ALR runtime shows lower syscall-heavy overhead than PRoot baseline.

### Level 5: Practical Userland

- `dpkg --version`, `dpkg --print-architecture`, `apt --version`, and local package install subset pass.
- Python and Node startup pass.
- Git basic command pass.
- Simple GUI app renders through Android Surface.

### Level 6: Real App Track

- GTK/Qt hello.
- Browser or Electron-adjacent controlled smoke.
- Basic GL demo through GLES shim.
- Vulkan clear/present through ICD/proxy.

## Metrics

### Execution Metrics

- Process startup time.
- Syscall-heavy microbenchmark time.
- `fork`/`exec`/`posix_spawn` latency.
- `stat/open/readlink` heavy workload time.
- `npm install` or package-manager workload time when stable.
- Failure class and errno mapping.

### Graphics Metrics

- Host renderer name.
- Frames submitted.
- Frames rendered.
- Frames dropped.
- Sequence gaps.
- Duplicate frames.
- Submit-to-render latency.
- Copy count estimate.
- CPU usage during frame stream.

### Packaging Metrics

- APK size.
- Rootfs archive size.
- Rootfs extraction time.
- First launch time.
- App-private storage footprint.

## Risks

### Technical Risks

- Android may block direct execution from writable app-data paths.
- Device ptrace/seccomp policy may vary.
- LD_PRELOAD cannot catch static binaries or direct raw syscalls.
- Dynamic linker behavior can differ across glibc versions.
- Procfs, hardlinks, symlinks, and fakeroot behavior can become compatibility traps.
- GPU bridge IPC and buffer copies may erase expected acceleration wins.

### Legal and Licensing Risks

- PRoot/Termux artifacts carry GPL obligations.
- Optional closed runtimes may restrict modified redistribution.
- Clean-room process must be preserved when replacing proprietary-compatible behavior.
- Vendoring GPL path-rewrite shims may constrain app distribution.

### Product Risks

- Users may expect full desktop Linux parity.
- Heavy desktop apps may exceed memory and thermal budgets.
- Package manager mutation support may take longer than simple command execution.
- Vulkan/DXVK expectations may arrive before GLES fundamentals are stable.

## Documentation Requirements

Every major feature must have:

- A scope statement.
- Non-goals.
- Test plan.
- Expected report lines.
- Device evidence checklist.
- Licensing notes if third-party artifacts are involved.
- Clean-room notes if behavior overlaps proprietary or closed runtimes.

## First Documentation Bundle

The project documentation should begin with these stable documents:

- Product requirements: this file.
- Clean-room protocol: `docs/clean-room-protocol.md`.
- Execution backend specification: `docs/alr-execution-backend-spec.md`.
- Graphics bridge specification: `docs/android-graphics-bridge-spec.md`.
- Milestone plan: `docs/plans/implementation-milestones.md`.
