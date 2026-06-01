# Device Evidence — SM-X236N (MediaTek/Mali), APK v54

First on-device verification run. Captured automatically over adb.

## Device

| Field | Value |
| --- | --- |
| Model | SM-X236N (Galaxy Tab A9+ 5G, MediaTek variant) |
| SoC / board | MediaTek MT6878 (`ro.hardware=mt6878`) |
| GPU | ARM Mali-G615 MC2 (vendor `ARM`) — **Mali GPU bucket** |
| ABI | arm64-v8a |
| Android | 16 (release), API 36 |
| transport | usb |

## APK under test

- `app/build/outputs/apk/debug/app-debug.apk`
- versionName `0.4.54-android-procfs-interpose-v54`, versionCode 54
- SHA-256 `f6c7db7169cfa7a479ae7c1990fa8010a7b39dfe49edd3b8d81809f6d3884310`
- install: `adb install -r -d` → Success; launch `dev.chanwoo.androlinux/.MainActivity`; no native crash / tombstone / SIGSYS in logcat.

## Captured artifacts (this directory)

- `v54-app-report.txt` — full in-app report (decoded from the on-screen TextView, 1188 lines)
- `v54-uiautomator-raw.xml` — raw uiautomator dump (provenance)
- `v54-screenshot.png` — device screenshot
- `v54-logcat-tail.txt` — logcat tail during the run

## This session's ALR mediation layers — DEVICE-PROVEN

All four gates added this session passed on the real device, with real device values:

| Gate | Result | On-device detail |
| --- | --- | --- |
| `ALR PROCFS VIRTUALIZATION PLAN` | PASS | self-exe host truth = real packaged trampoline `…/lib/arm64/libalr_runtime_trampoline.so`; guest view `/bin/hello`; status uid=10326 (real app uid, fakeroot=false); mounts host-leak tokens=0 |
| `ALR WX-SAFE EXEC STRATEGY` | PASS | entrypoint = packaged trampoline `.so` (read-only APK lib area); target `…/files/rootfs/debian-arm64/bin/hello` correctly kept as a *rejected* exec target; primary `memfd-execveat`; rejects `direct-rootfs-execve` + `file-backed-mmap-exec` with the real `app_data_file` path |
| `ALR PERF HARNESS` | PASS | real ARM measurement: ALR path-xlate ≈ 4450 ns/op, getppid ≈ 218 ns/op, xlate ≈ 20.5 syscall units; PRoot-vs-ALR comparison correctly `PENDING_DEVICE` (not fabricated) |
| `ALR PROCFS INTERPOSE MECHANISM` | PASS | `/proc/self/exe` → `/bin/hello` (not the trampoline), host path `synthetic` (no host `/proc` access); `/proc/mounts` synthesized 291 bytes, no host-rootfs leak |
| `ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION` | PASS | (prior milestone, now also device-confirmed) |

The W^X strategy's rejection of writable-rootfs exec is independently corroborated below by the PRoot dpkg failure.

## GPU bridge through Android public APIs — DEVICE-PROVEN on Mali

Summary lines for surface items read `PENDING_SURFACE_CALLBACK` because that text is built before the SurfaceView callback fires; the **appended sections prove the actual render** (see report tail):

- Vulkan Android Surface (public WSI): `vulkan create android surface=ok`, `software renderer=false`, **frames rendered 8 / dropped 0**.
- EGL/GLES Android Surface: `surface gl renderer=Mali-G615 MC2`, `software renderer=false`, guest GUI **8 frames (wayland 4 + x11 4) / dropped 0**.
- Host GPU probe: `gl renderer=Mali-G615 MC2`, `gl vendor=ARM`, hardware candidate=true, software=false; host Vulkan device=Mali-G615 MC2.

No vendor-private path used (no /dev/dri, KMS, GBM, KGSL, Turnip). GPU vendor bucket covered: **ARM Mali (MediaTek)**.

## PRoot baseline — pass/fail

PASS: rootfs exec, shell script, `sh -c`, glibc dynamic, distro userland, clean guest env, identity/NSS (uid=0 root), dpkg/apt `--version`/`--print-architecture`/query/split, apt-get/apt-cache/apt-config `--version`, Android permission model.

### KNOWN_FAIL (PRoot baseline limitation, not an ALR-layer regression)

- `DPKG LOCAL INSTALL EXECUTION: FAIL`
- `INSTALLED PACKAGE EXECUTION: FAIL` (dependent — the package never installed)

Root cause, from the captured stderr:

```
dpkg (subprocess): unable to execute split package reassembly (dpkg-split):
  Function not implemented            <- ENOSYS
dpkg: error processing archive …/alr-smoke_1.0_arm64.deb (--install):
  subprocess dpkg-split returned error exit status 2
```

`PROOT_NO_SECCOMP=1` is set, so this is **not** a seccomp SIGSYS filter. The ENOSYS appears only when a guest glibc (≥2.34) program forks+execs a child (`dpkg` → `dpkg-split`); every single-process smoke (`dpkg --version`, `id`, `glibc-hello`, …) passes. That signature matches glibc `posix_spawn`/`fork+exec` using `clone3()`, which PRoot's ptrace path does not service on this Android 16 kernel and returns ENOSYS. A secondary `status-old: Permission denied` fakeroot quirk is moot once the spawn fails.

This is a baseline-fallback limitation of the ptrace-based PRoot path. It directly motivates the ALR direction (no ptrace, no clone3 emulation gap) and is consistent with the W^X strategy gate. Not hardcoded to pass; left as a real KNOWN_FAIL for baseline hardening (PRoot clone3 handling) as a follow-up.

## Honest scope

- One device, one GPU vendor bucket (ARM Mali / MediaTek). Adreno/Qualcomm and other buckets are still unverified.
- ALR mediation layers are proven as **plan/mechanism** on-device; they are not yet a live `LD_PRELOAD` runtime, and the native-exec methods (`memfd-execveat`/`anon-mmap-loader`) are not yet executed (SELinux outcome still unmeasured).
- The overall project goal is **not** complete: it requires multi-vendor GPU evidence and a real ALR native-exec runtime, not just the baseline + mediation plans.
