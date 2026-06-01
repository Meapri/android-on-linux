# Prior Art Research

This document records the projects that shape Android on Linux Runtime. The product target is not to clone every feature from these systems. The target is a stock Android APK that runs ARM64 glibc Linux apps through a low-overhead runtime and presents GUI/GPU output through Android public graphics APIs.

## Decision Summary

- Use Plib as the owned upstream baseline for code, tests, ALR planning, rootfs safety, PRoot fallback, and Android graphics bridge scaffolding.
- Use Termux and Termux packages as Android userland packaging references, not as the final execution model.
- Use Termux:X11, Local Desktop, and FluxLinux as GUI/graphics prior art, but keep this project app-window-first rather than full-desktop-first.
- Use PRoot/proot-distro/UserLAnd/Andronix as compatibility baselines and smoke-test comparisons, not as the long-term hot path.
- Treat AVF/gfxstream as a separate VM-class track. It is useful for understanding Android-backed GPU virtualization, but it does not satisfy the v1 no-VM native APK goal.
- Lock GPU work to Android public APIs first: `ANativeWindow`, `AHardwareBuffer`, `EGL/GLES`, ANGLE where available, and Vulkan. This keeps the success path close to normal Android game rendering instead of Linux desktop driver probing.

## Comparison Matrix

| Project | Execution model | GUI/GPU model | Useful lessons | Product fit |
| --- | --- | --- | --- | --- |
| Plib | APK-owned runtime with packaged native entrypoints, PRoot baseline, and ALR target | Android Surface/EGL/GLES bridge scaffolding and later Vulkan proxy planning | Best available owned baseline for this repository. Keep its docs, tests, launcher, rootfs extraction, and report shape. | Primary seed and continuing reference |
| Termux | Android app userland built around Bionic packages under app-private prefix | CLI-first; graphics usually needs add-ons or external display paths | Package layout, Android restrictions, nativeLibraryDir/W^X history, ecosystem expectations | Reference only; not the product shell |
| Termux:X11 | X server integration for Termux workloads | X11 server display path, often paired with software or experimental acceleration stacks | Good GUI compatibility benchmark and input/windowing reference | Comparison target, not core UX |
| proot-distro | PRoot-managed Linux distributions inside Termux | Depends on paired display solution such as VNC/X11 | Easy distro bootstrap and compatibility baseline | Baseline/fallback only |
| UserLAnd | PRoot-based app that runs distro sessions | VNC/SSH-oriented user experience | Shows broad no-root demand and UX tradeoffs | Prior art; too desktop/session-oriented |
| Andronix | Termux/proot distro installer workflow | Usually desktop environments through VNC/Termux:X11-like paths | Useful setup flow and user education model | Prior art; not native enough |
| Local Desktop | Android-hosted Linux desktop direction | Wayland/desktop-oriented bridge ideas | Relevant for compositor and desktop integration research | Reference for GUI architecture |
| FluxLinux | Linux-on-Android GUI/runtime exploration | Focus on integrated GUI experience | Useful for app UX and bridge investigation | Reference only until evidence is local |
| AVF/gfxstream | Android Virtualization Framework VM path | gfxstream-style GPU virtualization | Useful if unmodified distro compatibility becomes more important than native APK execution | Separate future track |

## Source Notes

### Plib

Plib is the owned upstream for this repository. It already contains the Android Gradle skeleton, NDK entrypoints, ALR planning docs, PRoot fallback artifacts, rootfs safety tests, and GUI/GPU report scaffolding. Because 찬우 owns Plib, this project may import and adapt Plib code directly. External projects below are still treated as prior art or dependencies with their own licenses.

### Termux

Termux describes itself as an Android terminal application and Linux environment. Its biggest value here is the proof that Android users want serious package-managed CLI workflows, and its package ecosystem gives good examples for adapting Unix tools to Bionic and Android paths.

Termux is not the final runtime model because this project targets a signed APK runtime with app-specific native windows, not a terminal-first environment. Termux's Android 10 notes also highlight the key policy issue: apps targeting API 29+ should load executable code from the APK, not from writable app home storage. That directly supports this project's `nativeLibraryDir` entrypoint rule.

Source links:

- <https://github.com/termux/termux-app>
- <https://github.com/termux/termux-packages/wiki/Termux-and-Android-10>
- <https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission>

### PRoot and proot-distro

proot-distro runs full Linux userlands on top of Termux or regular Linux without root, kernel modules, or Docker. It is valuable as a compatibility reference and as a baseline for rootfs layout, distro image handling, and command smoke tests.

It is not the desired hot path because ptrace-style mediation is expensive for filesystem-heavy and process-heavy workloads. ALR should keep PRoot as a fallback and A/B oracle while moving common path/process/procfs operations into lower-overhead runtime layers.

Source link:

- <https://github.com/termux/proot-distro>

### Termux:X11

Termux:X11 is a fully fledged X server add-on app built with Android NDK and optimized for Termux. It proves that an Android-side display server can serve Linux GUI workloads, and its proot/chroot notes are useful for TMPDIR, SELinux, and namespace edge cases.

It is not the v1 UX target because this project should avoid making a whole desktop session the default. The product should map Linux app surfaces to Android-owned app windows first, then use X11/Xwayland as a compatibility layer.

Source link:

- <https://github.com/termux/termux-x11>

### UserLAnd and Andronix

UserLAnd and Andronix prove demand for no-root Linux distributions and app-like setup flows. UserLAnd emphasizes installing and uninstalling like a regular app; Andronix documents the common Termux + PRoot + PulseAudio + XSDL/VNC shape.

They are not sufficient for this project because the target is lower-overhead execution plus Android-native GPU presentation. VNC and full desktop sessions are acceptable compatibility references, not the primary rendering path.

Source links:

- <https://github.com/CypherpunkArmory/UserLAnd>
- <https://docs.andronix.app/get-started/how-does-andronix-work>

### Local Desktop

Local Desktop is close to this project's GUI research direction: app internal storage rootfs, PRoot, an Android NDK Wayland compositor, and rendering back to the Android activity. It is the strongest external reference for a Wayland-shaped bridge.

The main difference is product shape. Local Desktop is desktop-environment-first, while Android on Linux Runtime is app-window-first and wants ALR to replace the PRoot hot path over time.

Source link:

- <https://github.com/localdesktop/localdesktop>

### AVF and gfxstream

Android Virtualization Framework provides VM-class execution environments and stronger isolation, while gfxstream forwards OpenGL and Vulkan calls to a host in virtualized Android/Cuttlefish contexts. These are useful for understanding Android-backed graphics virtualization and future fallback tracks.

They are not the v1 runtime because this project explicitly targets a normal APK-owned native runtime, not a VM. If unmodified distro compatibility becomes more important than no-VM execution, AVF/gfxstream should become a separate architecture decision, not a hidden dependency.

Source links:

- <https://source.android.com/docs/core/virtualization>
- <https://source.android.com/docs/devices/cuttlefish/gpu>

### Android Public Graphics APIs

The generic GPU path should use Android's public surface and graphics APIs. `ANativeWindow` is the native counterpart of Java `Surface`; `AHardwareBuffer` can represent GPU-accessible shared buffers and bind to EGL/OpenGL or Vulkan primitives; Android's Vulkan guidance describes Vulkan as the primary low-level graphics API on Android, with ANGLE providing OpenGL ES on top of Vulkan on supported devices.

This supports the project rule that manufacturer-independent acceleration must be proven through the Android graphics stack, not by bundling or probing GPU-specific Linux drivers.

Source links:

- <https://developer.android.com/ndk/reference/group/a-native-window>
- <https://developer.android.com/ndk/reference/group/a-hardware-buffer>
- <https://developer.android.com/games/develop/vulkan/overview>

## Architecture Consequences

- The first executable boundary must be packaged Android-native code from `nativeLibraryDir`, not a writable app-data binary.
- Rootfs data can be mutable, but executable launch plans must be deterministic and report whether they use PRoot, ALR, or an optional external backend.
- GPU support must be Android-public-API-first: `Surface`, `ANativeWindow`, `EGL/GLES`, and later Vulkan. `/dev/dri`, KMS, GBM, KGSL, Turnip, or other vendor/private paths are not the default route.
- The UI should feel like Android is hosting Linux apps one by one. Full desktop mode can exist later as an advanced compatibility path, but it is not the v1 product shape.
- A GPU acceleration claim requires real device evidence from at least Adreno, Mali/Immortalis, and Tensor-class hardware using the same APK and no vendor-specific driver bundle.

## Open Research Questions

- Whether ALR can reliably launch dynamic glibc programs without a custom linker beyond guest loader orchestration.
- Which subset of Wayland semantics is needed before GTK/Qt smoke apps become useful.
- Whether the GLES shim should remain command-proxy based or move quickly to shared buffers after basic `eglSwapBuffers`.
- Whether Vulkan proxy work should follow a minimal custom ICD first or study Venus/gfxstream patterns before implementation.
- Whether `AHardwareBuffer` can become the first efficient shared-buffer path for guest frames without creating lifecycle or fence complexity too early.
