# Architecture Notes

## Non-root Android constraints

A normal Android APK is not a Linux container. The rootfs is just files owned by the app UID.

Key rules:

- Executable entrypoints must be shipped as trusted APK/package native code.
- Writable app data is not a stable executable location on modern target SDKs.
- Real `chroot`, bind mounts, mount namespaces, capabilities, setuid, and device-node access are unavailable without root/system privileges.
- glibc binaries expect `/lib/ld-linux-aarch64.so.1` and Linux FHS paths; Android does not provide those paths.
- Android SELinux/seccomp restrictions remain in force for child processes.

## Runtime shape

```text
Android Activity / Service
  owns Surface, permissions, lifecycle
        |
        v
Native runtime loader, packaged in nativeLibraryDir
  executable path: .../lib/arm64/libalr-loader.so
        |
        v
Rootfs manager
  writable files: /data/user/0/<pkg>/files/rootfs/<name>
        |
        v
Initial execution backend
  PRoot/proot-rs or explicit glibc-loader experiment
        |
        v
Linux userspace programs
```

## Graphics shape

Direct Linux DRM/GBM/KMS is not the generic path. The generic path must terminate graphics on Android APIs:

```text
Linux app
  -> Linux-side GL/Vulkan/proxy client
  -> socket/shared-memory transport
  -> Android host renderer service
  -> Android EGL/GLES/Vulkan
  -> Android Surface / ANativeWindow
  -> vendor Android GPU driver
```

## Graphics tracks

### Track A: OpenGL/GLES first

Most realistic MVP.

- Mesa virgl client or GL proxy in Linux rootfs
- Android host renderer using EGL/GLES or ANGLE
- Surface owned by APK

### Track B: Zink over Vulkan

Longer-term desktop OpenGL route.

- Mesa Zink in rootfs
- Custom Vulkan ICD/proxy or Venus transport
- Android Vulkan host backend

### Track C: Native Vulkan / DXVK

Research track only at first.

- Venus if feasible
- Otherwise focused custom Vulkan proxy subset
- Initial success criteria: `vulkaninfo` and `vkcube`, not full DXVK

## Product principles

- Do not pretend to be a full privileged Linux container.
- Make unsupported kernel features explicit.
- Prefer Android public APIs over vendor-specific kernel nodes.
- Keep runtime execution and graphics bridge loosely coupled so PRoot can later be replaced by a lower-overhead runtime.
