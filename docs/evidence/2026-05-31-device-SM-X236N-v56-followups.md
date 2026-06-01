# Device Evidence — SM-X236N, APK v56 (follow-ups to v54 findings)

Same device as `2026-05-31-device-SM-X236N-mali-v54.md` (SM-X236N, MediaTek MT6878, Mali-G615 MC2, Android 16 / API 36, arm64-v8a). This run verifies three follow-ups requested after v54.

- APK `0.4.56-android-syscall-sandbox-probe-v56`, SHA-256 `566b043df04f1e4e3dbe69a375fcd99454443ad0e0ed6c6025c90c7646d7b18e`.
- Captured fresh after `pm clear` (clean app data → fresh rootfs). Artifacts: `v56-app-report.txt`, `v56-uiautomator-raw.xml`, `v56-screenshot.png`, `v56-logcat-tail.txt`.

## #3 Surface summary gates — FIXED and verified

The five surface gates previously stuck at `PENDING_SURFACE_CALLBACK` in the summary now resolve from the real render results after the SurfaceView callback. On device all five read **PASS after Surface callback**:

```
ANDROID HOST VULKAN SURFACE PROBE EXECUTION: PASS after Surface callback
ANDROID HOST VULKAN SURFACE EXECUTION: PASS after Surface callback
HOST GPU SURFACE EXECUTION: PASS after Surface callback
GUEST GPU MULTI-FRAME SURFACE EXECUTION: PASS after Surface callback
GUEST GUI GPU SURFACE EXECUTION: PASS after Surface callback
```

Resolution is gated on a non-software renderer plus all frames drawn with zero drops (Mali-G615 MC2, 8/8 frames).

## #4 memfd-execveat W^X-safe native exec — attempted, BLOCKED by SELinux

`ALR MEMFD W^X-SAFE NATIVE EXEC: FAIL`. The probe read the 537664-byte static guest ELF and created the anonymous memfd successfully (`ALR MEMFD CREATE: PASS`, flag `plain` — the kernel rejected `MFD_EXEC`), but `execveat(fd, "", AT_EMPTY_PATH)` returned **`EXECVEAT_ERRNO=13` (EACCES)**. Android's `untrusted_app` SELinux domain denies executing anonymous in-memory fds. This is an honest, important result: on this device the planned native-exec primary `memfd-execveat` is blocked, so the `anon-mmap-loader`/packaged-interpreter fallbacks must be evaluated next. (Not hardcoded to pass.)

## #2 dpkg local install — root-caused, two hypotheses disproven, partially fixed

`DPKG LOCAL INSTALL EXECUTION: FAIL` still, but the root cause is now pinned by measurement:

- **`clone3` hypothesis — DISPROVEN.** Via `run-as`, nested fork+exec of real binaries and pipes work; `dpkg-split` runs fine as a single and nested exec.
- **Android app-seccomp hypothesis — DISPROVEN.** The new `ALR SYSCALL SANDBOX PROBE` ran in the real `untrusted_app` domain and measured every proot-relevant syscall **ALLOWED** (ENOSYS-free): `execveat`, `symlinkat`, `linkat`, `mknodat`, `clone3`, `unshare` — `alr syscall blocked count=0`.
- **hardlink layer — FIXED.** A bare `ln a b` inside PRoot returns `Permission denied` (Android app-data FS rejects `hardlink()`), which breaks dpkg's `status`→`status-old` backup. PRoot's `--link2symlink` (`-l`) extension fixes it; via `run-as` the full `dpkg -i` then succeeds and the installed binary runs. `-l` is now enabled for the package-manager smokes.
- **residual `ENOSYS` — proot-internal.** In `untrusted_app`, dpkg's nested `dpkg-split` exec still returns `Function not implemented`, and with `-l` the backup `symlink()` also returns `ENOSYS`. Since those syscalls are allowed in the domain (measured above), the `ENOSYS` originates inside PRoot's ptrace emulation in the `untrusted_app` context (it does not reproduce in `runas_app`). Fixing it needs PRoot internals work; PRoot is baseline-only, so this stays a `KNOWN_FAIL` and motivates ALR.

## Net

`#3` fixed and device-verified. `#4` and `#2` produced honest, measured negative results that sharpen the ALR direction: native exec from `untrusted_app` is SELinux-constrained (memfd `EACCES`), and the PRoot baseline has a domain-specific internal `ENOSYS` for Debian package install that is not a syscall-allowlist or `clone3` problem. All other v54 gates (ALR mediation layers, GPU bridge, PRoot rootfs/shell/glibc) remain PASS. Still single device / single GPU vendor (Mali); goal not complete.
