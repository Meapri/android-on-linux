# AndroLinux Runtime Product Requirements

> **Status (2026-10-11): revised against build 0.4.163 and [ADR 0004](adr/0004-unified-direction.md).** PRoot has been removed and the execution backend is the alr engine (`runtime/alr`), so the PRoot baseline and fallback language of the first draft is replaced below. Acceptance levels now carry a state line. Items that are open are marked `PENDING_DEVICE` or "open"; nothing here is claimed without device evidence.

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
- Keep execution compatibility at least as good as the former PRoot baseline for smoke workloads. Met: the alr engine passes the acceptance suite (164 PASS) and the curated 96-package breadth set on two devices.
- Reduce execution overhead significantly below ptrace-heavy PRoot for common filesystem and process-heavy workloads.
- Reach at least proroot-class behavior for supported dynamic glibc programs, while keeping the implementation open and maintainable.
- No PRoot fallback is kept (removed; see THIRD-PARTY). The fallback for a future `targetSdk` install floor is the frozen legacy in-process loader (ADR 0004, D2).

### GUI and GPU

- Render GUI output into an Android-owned `Surface`.
- Use Android host `EGL/GLES` for the first hardware-accelerated rendering path.
- Add a guest `libEGL`/`libGLES` shim for a constrained API subset before broad GL compatibility claims.
- Add a Vulkan host renderer and guest Vulkan ICD/proxy only after GLES proof is stable.
- Avoid depending on Linux DRM/KMS or `/dev/dri` as the generic rendering path.
- Keep the graphics bridge GPU-vendor-independent: Adreno, Mali, Xclipse, PowerVR, and desktop Android targets should all use Android public graphics APIs.

### Delivery surfaces

- S1, the APK app: catalog, launcher, and one foreground Linux app with GUI/GPU output.
- S2, the Termux CLI (`alr install/run/shell`): the same engine without the APK UI.
- S3, a Termux-attached GUI using the companion-app pattern of Termux:X11: candidate only, decided by spike S1 in ADR 0004.

### Agent and developer CLIs

- `node`/`npm`-based CLIs run with path virtualization (met, goal G5 of the alr engine).
- Static binaries such as the Rust-built `codex` run without path virtualization when given a host-form environment ([alr ADR 0008](../runtime/alr/docs/adr/0008-static-guest-binaries-non-goal.md)).
- Open: Go and uutils-class raw-syscall binaries (ADR 0004 D6 spike), and long-running PTY sessions.

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

### Agent CLI User

Runs terminal coding agents and developer tools (Node, Rust, Python, Git) on a phone or tablet, from Termux or from the app, and needs a stock Ubuntu userland with working `apt`, DNS, and a PTY.

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

### Former Baseline Backend: PRoot (removed)

The first draft packaged a Termux PRoot-derived backend as the compatibility baseline and fallback. Device work ended that role: PRoot could not `dpkg -i` in the app's SELinux domain, and at `targetSdk 28` it stopped executing programs at all (its void-syscall cancellation returned `ENOSYS`). It was removed, which also removed its GPL-2.0 obligations ([THIRD-PARTY](THIRD-PARTY.md)). A proot-distro installation is still used as an external comparison oracle in benchmarks.

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

### Product Backend: alr

The alr engine (`runtime/alr`) is the product backend. It is built as a Termux CLI and, from the same source, embedded in the APK.

```text
libalr.so            bionic CLI + signal-only ptrace supervisor (APK: jniLibs; Termux: $PREFIX/bin/alr)
libalr_preload.so    glibc LD_PRELOAD interposer: path rewrite, /proc/self virtualization,
                     exec re-dispatch, link2symlink, fakeroot identity (APK: asset)
libdl.so.2 stub      own 3 KB stub for trimmed rootfs images
```

Measured on two Samsung Android 16 devices (MediaTek, Snapdragon): stock Ubuntu 24.04 boots with zero glibc patches, `git status` on 10k files runs at 36 ms versus 33 ms native and 930 ms under proot-distro, and the path layer never traps (`path_traps=0 syscall_stops=0`). Details and caveats: [runtime/alr README](../runtime/alr/README.md).

Known limits: the preload sees only libc calls, so static, Go, and other raw-`svc` binaries run without path virtualization. The engine needs `targetSdk 28`, so there is no Google Play distribution.

The pre-alr plan (launcher, hook, bridge, linker libraries) is superseded. The frozen legacy in-process loader remains for the bundled fixture tree and research ([architecture notes](architecture.md)).

## Graphics Strategy

### Stage 1: Host Surface Renderer (done)

The Android app owns a `SurfaceView` or `TextureView`. Native host code renders known frames with Android `EGL/GLES` and reports renderer, frame count, drops, and loss.

### Stage 2: Guest Command Bridge (done; superseded by the shared-memory ring)

Guest binaries send simple frame commands to the host renderer over loopback or Unix-domain IPC. The host reports sequence, ACK, frame loss, and protocol identity.

### Stage 3: GUI Protocol Shape (done; a real in-app Wayland compositor, Xwayland for X11-only apps)

Introduce Wayland-shaped and X11-shaped ingress endpoints. These are initially constrained proxy protocols, not full compositors.

### Stage 4: GLES Shim (done on the legacy loader; open on the alr engine)

Guest `libEGL`/`libGLES` shims implement a small, testable subset:

- `eglGetDisplay`
- `eglInitialize`
- `eglChooseConfig`
- `eglCreateContext`
- `eglMakeCurrent`
- `glClearColor`
- `glClear`
- `eglSwapBuffers`

### Stage 5: Vulkan Proxy (enumerate, render, draw, and present done on the legacy loader; open on the alr engine)

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

### Level 2: Baseline Execution (originally PRoot, now alr)

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

### Current state of the ladder (2026-10-11)

| Level | State | Evidence |
| --- | --- | --- |
| 0 Host tests | Passing, with environment-dependent failures in a plain checkout | `tests/`, CI `ws5-host-ci.yml` |
| 1 APK launch | Done | `docs/evidence/v100-*`, `v109-*` |
| 2 Baseline execution | Done on alr | runtime/alr acceptance, 164 PASS |
| 3 GUI/GPU proof | Done on the legacy loader. GUI re-verified on alr for GIMP and others. GPU re-measurement on alr is open | [PERFORMANCE](PERFORMANCE.md), ADR 0004 P1 |
| 4 Low-overhead runtime MVP | Done: path layer never traps, 25.8x over proot-distro on `git status` | runtime/alr README |
| 5 Practical userland | Done: apt, dpkg, node/npm, git, 96 of 96 curated packages | runtime/alr M10-M14 |
| 6 Real app track | GIMP, GTK3/Qt6/SDL2 apps, netsurf done. Chromium renders in-process on the legacy loader (CR-1). Its multi-process path is unfinished there and unmeasured on alr | [cp6-status](research/cp6-status.md) |

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

- PRoot artifacts carried GPL obligations; they are removed. Remaining shipped native code is MIT (THIRD-PARTY). Any code taken from tawc or other GPL projects needs a per-file license check first.
- Optional closed runtimes may restrict modified redistribution.
- Clean-room process must be preserved when replacing proprietary-compatible behavior.
- Vendoring GPL path-rewrite shims may constrain app distribution.
- `targetSdk 28` removes Google Play distribution and depends on Android continuing to install such apps.

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
