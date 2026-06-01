# PoC Roadmap

## PoC 0: Host-side policy model

Status: started.

Success criteria:

- Launch plan always executes loader from Android `nativeLibraryDir`.
- Rootfs remains in app-private `files/rootfs/<name>`.
- Tests reject direct execution from writable app-data paths.

Verification:

```bash
python3 -m pytest tests/ -q
```

## PoC 1: Minimal Android APK loader

Success criteria:

- Build debug APK.
- App loads native library.
- Native loader can print:
  - package name
  - native library dir
  - app files dir
  - intended rootfs dir
  - intended program
- No rootfs execution yet.

## PoC 2: Rootfs manager

Status: host-side manifest model started.

Success criteria:

- APK copies or extracts a tiny test rootfs into app-private files.
- SHA256 manifest verifies archive payload before extraction.
- Asset paths reject traversal such as `../escape.tar`.
- Install plan keeps archives under `files/rootfs/.downloads/<name>/<version>/`.
- Extracted rootfs lives under `files/rootfs/<name>`.
- Version marker path is deterministic: `.alr-installed-<version>`.
- Runtime can list rootfs paths through native code.

## PoC 3: Shell backend

Success criteria:

- Initial backend launches a shell-like test process safely.
- Then integrate PRoot/proot-rs or glibc-loader experiment.
- App terminal captures stdout/stderr through PTY or pipes.

## PoC 4: GUI output

Success criteria:

- Android app owns a `SurfaceView`/`TextureView`.
- Host renderer draws a test frame from native code.
- Linux-side GUI bridge design selected.

## PoC 5: OpenGL hardware acceleration

Success criteria:

- Android EGL/GLES backend renders to app Surface.
- Linux-side GL client sends a simple command stream or uses virgl-style path.
- Test on at least one Adreno and one non-Adreno device.

## PoC 6: Vulkan minimal

Success criteria:

- Android host creates Vulkan instance/device/swapchain for app Surface.
- Linux-side proxy can request device info.
- Then attempt `vulkaninfo`/`vkcube` equivalent.

## Non-goals for early MVP

- Docker/systemd/full cgroups.
- Root-required device access.
- DXVK/Wine gaming compatibility.
- Claiming perfect Linux kernel parity.
