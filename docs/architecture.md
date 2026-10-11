# Architecture Notes

Status: revised 2026-10-11 to match build 0.4.163 and [ADR 0004](adr/0004-unified-direction.md). The earlier version described a PRoot-based launcher and three graphics tracks that were planning-stage ideas.

## Non-root Android constraints

A normal Android APK is not a Linux container. The rootfs is just files owned by the app UID.

- Real `chroot`, bind mounts, mount namespaces, capabilities, setuid, and device-node access are unavailable without root.
- glibc binaries expect `/lib/ld-linux-aarch64.so.1` and Linux FHS paths. Android provides neither, so the guest `ld.so` is invoked explicitly and paths are virtualized in the guest process.
- Android's zygote seccomp filter blocks 239 syscalls on the reference devices. One of them, `set_robust_list`, runs before any constructor in a glibc process, so a signal-only supervisor must rescue `SIGSYS`.
- The SELinux domain of an app depends on its `targetSdk`. Only the legacy domain `untrusted_app_27` may execute files from app-private storage. This project therefore pins `targetSdk 28`, the same trade Termux makes, and gives up Google Play distribution ([alr ADR 0005](../runtime/alr/docs/adr/0005-play-store-unsupported.md)).

## Layers

```text
L2 surfaces   APK launcher/catalog     Termux CLI (alr)     [candidate] Termux-attached GUI
                       |                     |                        |
L1 services   Wayland compositor, GPU ring executor, audio, DoH, USB, SAF, input/IME
                       |
L0 engine     alr
```

### L0: execution engine (alr)

Source: `runtime/alr`. Documentation: [runtime/alr/docs](../runtime/alr/docs/02-architecture.md).

```text
alr (bionic) --fork--> [child] PTRACE_TRACEME -> execve(<R>/lib/ld-linux-aarch64.so.1 --preload libalr_preload.so <program>)
   |
   +-- supervisor: handles only SIGSYS (SYS_SECCOMP) stops; never PTRACE_SYSCALL; path syscalls never trap
```

- The kernel executes the guest. No emulation, no in-process ELF mapping.
- `libalr_preload.so` (glibc ABI, shipped as an APK asset) rewrites guest paths, virtualizes `/proc/self/*`, re-dispatches `exec*`, emulates `link` with symlinks, and fakes root for `dpkg`.
- Limits: the preload sees only libc calls. Static, Go, and other raw-`svc` binaries run without path virtualization.

The same source builds the Termux CLI (`make alr`) and the library shipped inside the APK (`buildAlrRuntime` in `app/build.gradle.kts`).

### L1: host services (in the APK)

All services use Android public APIs only.

```text
Linux guest GUI/GL/Vulkan client
        |  Wayland (wl_shm, linux-dmabuf, xdg-shell, text-input-v3) / Xwayland for X11-only apps
        |  guest libEGL/libGLESv2 shim and Vulkan ICD -> shared-memory SPSC command ring
        v
In-app compositor + GPU ring executor
        |  real EGL/GLES 3.2 and Vulkan on the device GPU
        v
AHardwareBuffer -> EGLImage -> SurfaceView (zero-copy present)
```

Details: [GPU/display bridge](architecture/gpu-display-bridge.md), [ADR 0002](adr/0002-gpu-public-api.md). Measured results: [PERFORMANCE](PERFORMANCE.md).

### L2: delivery surfaces

| Surface | State |
| --- | --- |
| APK app: catalog, launcher, one foreground Linux app | Working |
| Termux CLI: `alr install/run/shell` | Working |
| Termux-attached GUI (companion pattern, as Termux:X11) | Candidate, spike S1 in ADR 0004 |

## Contracts between L0 and L1

1. Wayland socket inside the rootfs: `/tmp/alr-rt/alr-xdg/wayland-0`. Host and guest name the same socket differently.
2. GPU ring: today an inherited file descriptor (`ALR_GPU_RING_FD`) created before `fork()`. Whether this holds when the guest is a separately exec'd alr process is unverified (ADR 0004, phase P1).
3. Environment variables: `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `GDK_BACKEND`, `ALR_GPU_*`. alr reserves `LD_PRELOAD` and `LD_LIBRARY_PATH`.

## Legacy in-process loader

The loader in `app/src/main/cpp/runtime_report.cpp` maps the guest `ld.so` into the app process and mediates paths with PCGATE (PC-gated seccomp) plus ptrace ([ADR 0003](adr/0003-cpu-syscall-mediation-overhead.md)). It works at any `targetSdk`. It is frozen: bug fixes only. It is kept as the fallback if Android stops installing `targetSdk 28` apps, and as the research vehicle for Chromium-class multi-process work.

## Product principles

- Do not pretend to be a full privileged Linux container.
- Make unsupported kernel features explicit.
- Prefer Android public APIs over vendor-specific kernel nodes.
- Keep the execution engine and the graphics services loosely coupled, and prove the coupling on a device rather than assuming it.
- Every performance or compatibility claim names its engine and points to device evidence.
