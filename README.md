# Android on Linux Runtime

Goal: a non-root Android APK runtime that launches ARM64 glibc Linux apps and bridges Linux GUI/GPU output to Android public APIs, with an app-by-app native window experience.

Working product definition:

> GPU-vendor-independent, Android-API-backed, hardware-accelerated Linux-on-Android runtime.

This repository is seeded from 박찬우's `Meapri/Plib` work at reference commit `ce92165fab26a5a13cbb35fd75eb0bab2959fb5a`. Plib is treated as an owned upstream design and implementation source for this project, while Termux/UserLAnd/Andronix/AVF-style systems remain prior-art and comparison baselines.

## Current state (build 0.4.163, `targetSdk 28`)

The direction is recorded in [ADR 0004](docs/adr/0004-unified-direction.md). Short version: one execution engine (alr), three layers, three delivery surfaces.

| Area | State | Engine | Evidence |
| --- | --- | --- | --- |
| Guest execution | Stock Ubuntu 24.04 arm64 glibc rootfs, no patches. `git status` on 10k files: native 33 ms, alr 36 ms, proot-distro 930 ms | alr | [runtime/alr README](runtime/alr/README.md) |
| Compatibility | 96 of 96 curated Ubuntu packages install and run on two devices. Acceptance suite: 164 PASS | alr | [runtime/alr README](runtime/alr/README.md) |
| GUI | In-app Wayland compositor. GIMP 3, GTK3, Qt6, SDL2, foot, netsurf verified; Xwayland routing for X11-only apps | legacy loader (2026-06 set), alr (`sd` builds for GIMP, gpicview, MATE calculator) | [GUI universality](docs/research/gui-universality-status.md) |
| GPU | GLES 3.2 command ring to the real Mali, zero-copy `AHardwareBuffer` present, Vulkan enumerate/render/draw. glmark2 Score 1074 | **legacy loader only. Re-measurement on alr is open (P1)** | [PERFORMANCE](docs/PERFORMANCE.md) |
| Termux CLI | `alr install/run/shell`, `apt`, `dpkg`, `node`/`npm`, fakeroot, PTY | alr | [runtime/alr](runtime/alr/README.md) |

Honest limits:

- Both reference devices are Samsung (MediaTek and Snapdragon). Other OEMs are unverified.
- GPU evidence comes from one Mali-G615 device. Adreno and Tensor-class evidence is missing, so the project does not claim manufacturer-independent acceleration ([device evidence rules](docs/architecture/device-evidence.md)).
- `targetSdk 28` is what makes kernel `execve` of the guest `ld.so` possible. The price is no Google Play distribution and exposure to a future install floor.
- Static, Go, and other raw-syscall binaries run without path virtualization ([alr ADR 0008](runtime/alr/docs/adr/0008-static-guest-binaries-non-goal.md)).

## Architecture in one picture

```text
L2 surfaces   APK launcher/catalog     Termux CLI (alr)     [candidate] Termux-attached GUI
                       |                     |                        |
L1 services   Wayland compositor, GPU ring executor (Mali), audio, DoH, USB, SAF, input/IME
                       |
L0 engine     alr: execve(guest ld.so --preload) + LD_PRELOAD path virtualization
              + signal-only ptrace supervisor (SIGSYS rescue)
```

The pre-2026-08 in-process loader (guest `ld.so` mapped inside the app process, PCGATE, ptrace path mediation) is frozen as a legacy and research path. It remains as the fallback if Android ever blocks installing `targetSdk 28` apps.

## Documentation map

Start here:

- [ADR 0004: unified direction](docs/adr/0004-unified-direction.md): decisions D1-D8 and the phase plan.
- [ADR index](docs/adr/README.md): the three ADR namespaces (project, alr engine, workstream).
- [alr engine documentation](runtime/alr/README.md): the execution engine, its specs, acceptance suite, and device evidence.
- [PERFORMANCE](docs/PERFORMANCE.md): measured CPU/GPU numbers, labelled by engine.
- [Device evidence model](docs/architecture/device-evidence.md): proof rules before claiming manufacturer-independent GPU acceleration.
- [THIRD-PARTY](docs/THIRD-PARTY.md): licensing of shipped and removed components.

Architecture and decisions:

- [Architecture notes](docs/architecture.md)
- [Execution backend architecture](docs/architecture/execution-backend.md)
- [GPU/display bridge architecture](docs/architecture/gpu-display-bridge.md)
- [ADR 0001](docs/adr/0001-runtime-model.md): no-root, no-VM APK runtime model.
- [ADR 0002](docs/adr/0002-gpu-public-api.md): Android public GPU/display path.

Planning documents (written before the alr engine was adopted; each carries a status banner):

- [Product requirements](docs/product-requirements.md)
- [Prior art research](docs/research/prior-art.md): Termux, Termux:X11, proot-distro, UserLAnd, Andronix, Local Desktop, FluxLinux, tawc, Plib, and AVF/gfxstream.
- [Clean-room protocol](docs/clean-room-protocol.md)
- [ALR execution backend spec](docs/alr-execution-backend-spec.md)
- [Android graphics bridge spec](docs/android-graphics-bridge-spec.md)
- [Implementation milestones](docs/plans/implementation-milestones.md)
- [Agent coordination](docs/agent-coordination.md)
- [Parallel workstreams](docs/plans/parallel-workstreams.md)

## Repository layout

```text
app/                         Android APK (Kotlin UI, NDK runtime, Wayland compositor, GPU bridge)
app/src/main/cpp/            Legacy loader, alr_wayland, alr_gpu, alr_audio, alr_doh, alr_usb, ...
app/src/main/java/           Launcher, catalog, runtime services
runtime/alr/                 alr execution engine (Termux CLI and the engine embedded in the APK)
bench/                       Host-side benchmark harness
docs/                        Architecture, ADRs, research, device evidence
scripts/                     Build and validation scripts
tests/                       Host-side policy, model, and documentation tests
```

## Host tests

```bash
python3 -m pytest tests/ -q
```

Some tests need Git LFS rootfs assets, a specific compiler, or the NDK, and fail in a plain checkout. Compare against a baseline run before attributing a failure to a change.
