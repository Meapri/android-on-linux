# Execution Backend Architecture

Android on Linux Runtime executes Linux apps as an Android-hosted product, not as a rooted container and not as a VM. The host APK owns permissions, lifecycle, package integrity, native entrypoints, diagnostics, and update policy.

## Runtime Layers

```text
Android Activity / Service
  package paths, UI, permissions, reports
        |
        v
Packaged native entrypoint in nativeLibraryDir
  PRoot baseline or ALR launcher
        |
        v
App-private rootfs
  ARM64 glibc userland, mutable data, package state
        |
        v
Guest program
  shell, distro tools, GUI clients, GL/Vulkan clients
```

## Backend Strategy

`PRoot baseline` remains the compatibility reference and A/B fallback. It is useful because it can run rootfs smoke tests today, but it has ptrace overhead and Android seccomp/device-policy risks.

`ALR launcher` is the target backend. It starts from a deterministic Android-native launch plan, injects a clean runtime config, and grows toward path/process/procfs/fakeroot mediation.

`LD_PRELOAD mediation` is the first low-overhead implementation layer. It covers path-bearing libc calls, environment cleanup, guest path translation, and later `execve`/`posix_spawn` continuity.

The current ALR execution code keeps guest execution unclaimed but now builds deterministic child continuation plans. `execve`, `execvp`, and `posix_spawn` targets are resolved in the guest namespace, PATH lookup is recorded for `execvp`, serialized config handoff is generated, and the planned argv re-enters through the packaged ALR trampoline instead of treating writable rootfs files as the Android process entrypoint. The packaged trampoline has a `--continue-exec` dry-run mode that validates the config checksum and continuation target before any future guest loader path is allowed to claim execution. The APK report must include `ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: PASS` before the project treats this handoff as device-proven.

`Clean-room syscall support` is allowed only after tests prove that libc wrapper coverage is insufficient. It must be isolated, documented, and backed by behavior tests.

## Android W^X and Packaging Rule

Modern Android does not allow this project to treat app-private writable files as the first executable entrypoint. The direct launch boundary is therefore:

- packaged `.so`/native executable artifacts under the APK native library area,
- generated deterministic launch plans,
- verified rootfs extraction into app-private storage,
- writable rootfs data used as data, not as the first host executable.

Guest rootfs binaries may be executed only through an explicit backend plan that records the backend, rootfs path, program path, cwd, env allowlist, and report labels.

### W^X-safe exec strategy

`build_wx_safe_exec_strategy` (in `app/src/main/cpp/alr_runtime/alr_wx.cpp`) is the clean-room decision matrix for how the packaged trampoline hands control to the guest without violating W^X. It is **plan-only**: the chosen native mechanisms still require on-device SELinux verification and must not be reported as device-proven.

- Entrypoint must be the read-only packaged trampoline (`ALR_TRAMPOLINE_PATH` under the APK native library area), never a file inside the writable rootfs. The plan refuses (`ALR WX-SAFE ENTRYPOINT IS PACKAGED: FAIL`) when the entrypoint resolves inside `rootfs_dir`.
- Ranked W^X-safe guest-load methods: `memfd-execveat` (primary — copy the guest ELF into an anonymous memfd and `execveat(AT_EMPTY_PATH)`, which is not an `app_data_file`), then `anon-mmap-loader` (a userspace loader copies segments into anonymous RW→RX `execmem`), then `proot-baseline` (ptrace fallback, always available, highest overhead). PRoot is only a fallback — a viable plan needs a native-exec primary.
- Explicitly rejected shortcuts: `direct-rootfs-execve` and `file-backed-mmap-exec`, because both need execute permission on a writable `app_data_file`, which W^X denies for targetSdk >= 29.

The APK report must include `ALR WX-SAFE EXEC STRATEGY: PASS` before the project treats the native-exec handoff as planned. This is planning-only evidence; the `memfd-execveat`/`anon-mmap-loader` SELinux outcomes are still unproven until captured on a real device.

### memfd-execveat on-device attempt

`build_memfd_exec_probe` (in `runtime_report.cpp`) actually performs the primary method: it reads the packaged static guest ELF (`/bin/hello`) as data, copies it into an anonymous `memfd_create` fd (with `MFD_EXEC`, falling back to a plain memfd on older kernels), and `execveat(fd, "", AT_EMPTY_PATH)`. A memfd is not an `app_data_file`, so it is W^X-safe by construction. The report gate `ALR MEMFD EXECVEAT W^X-SAFE EXECUTION: PASS` is declared only when the child actually printed the guest's output — otherwise it reports the **SELinux** errno. This is the real on-device test of whether the app domain may execute an anonymous in-memory fd.

On-device result (SM-X236N, Android 16, `untrusted_app`): `memfd-execveat` is **denied** (`EACCES` — SELinux refuses to execute a file-backed memfd), but `build_execmem_probe` proves the complementary `anon-mmap-loader` primitive **works**: an anonymous page flipped to `PROT_EXEC` via `mprotect` and called returns `42` with no fault (`ALR EXECMEM ANON RX EXECUTION: PASS`). The app domain holds `execmem` (the ART JIT path) but not file-backed memfd exec. So on this device ALR's native guest exec should use the `anon-mmap-loader` method (map ELF segments into anonymous `execmem`), not `memfd-execveat` — the strategy ranking is effectively reversed by the device evidence.

### PRoot dpkg limitation (root-caused on device)

The PRoot baseline cannot `dpkg -i` a local package from the app's `untrusted_app` domain on this Android 16 device. Two distinct causes were isolated on device, and two earlier hypotheses were disproven by measurement:

- **hardlink layer (fixed):** Android app-data filesystems reject `hardlink()` with `EACCES`, breaking dpkg's atomic `status`→`status-old` backup (a bare `ln a b` inside PRoot returns `Permission denied`). The fix is PRoot's built-in `--link2symlink` (`-l`) extension, enabled for the package-manager smokes. Via `run-as` (the `runas_app` domain) this makes the full `dpkg -i` succeed and the installed binary run.
- **residual nested-exec `ENOSYS` (proot-internal):** in the real app (`untrusted_app`) domain, dpkg's nested exec of `dpkg-split` still returns `Function not implemented` and `--link2symlink` then turns the backup into a `symlink()` that also returns `ENOSYS`. This is **not** a `clone3` issue (nested fork+exec of real binaries works) and **not** an Android app-seccomp block: the `ALR SYSCALL SANDBOX PROBE` measured `execveat`, `symlinkat`, `linkat`, `mknodat`, `clone3`, and `unshare` all **ALLOWED** (blocked count 0) in `untrusted_app`. The `ENOSYS` originates inside PRoot's ptrace emulation in the `untrusted_app` context (it does not reproduce in `runas_app`). A full fix needs PRoot internals work; PRoot is baseline-only, so this is recorded as a `KNOWN_FAIL` that motivates the ALR direction.

The `ALR MEMFD EXECVEAT W^X-SAFE EXECUTION` probe likewise returns `EACCES` (**SELinux** denies executing an anonymous memfd in `untrusted_app`), so the native-exec primary `memfd-execveat` is blocked on this device and the `anon-mmap-loader`/packaged-interpreter fallbacks must be evaluated next.

## Rootfs Layout

```text
files/
  rootfs/
    debian-arm64/
      bin/
      usr/
      lib/
      etc/
      var/
    .downloads/
      debian-arm64/<version>/
cache/
  proot-tmp/
```

Rootfs assets must have a manifest with safe relative paths, sha256, and size. Extraction rejects absolute paths, `..`, device-like tar members, unsafe links, and traversal attempts.

## Guest Environment Policy

The guest receives an allowlisted environment only:

- `ALR_PACKAGE`
- `ALR_ROOTFS`
- `ALR_PROGRAM`
- `ALR_BACKEND`
- `HOME`
- `TMPDIR`
- `PATH`
- backend-specific `ALR_*` config keys

Android host variables such as `ANDROID_ROOT`, `ANDROID_DATA`, `BOOTCLASSPATH`, `ANDROID_SOCKET_*`, broad storage paths, and vendor storage variables are blocked unless a future Android-host helper explicitly requires them.

## Process Continuity Target

The implementation path is:

1. deterministic ALR config report,
2. path translation for `open/stat/readlink/getcwd/chdir`,
3. guest executable resolution,
4. `execve`, `execvp`, `execvpe`, `posix_spawn`, and `posix_spawnp` continuation plans through the packaged trampoline,
5. procfs and fakeroot mediation,
6. dpkg/apt preflight,
7. performance comparison against PRoot.

Each stage must keep PRoot baseline behavior intact and classify failures as `PASS`, `FAIL`, `SKIP`, or `KNOWN_FAIL:<reason>`.

## Procfs and Fakeroot Mediation

A guest process that runs under ALR must observe guest-namespace truth in `/proc`, not the Android host truth. The clean-room planner `build_procfs_virtualization_plan` (in `app/src/main/cpp/alr_runtime/alr_procfs.cpp`) synthesizes the views a future `/proc` interposer will serve and records which syscalls it hooks. It never reads or mutates the host `/proc`; it is **plan-only** and must not be reported as device-proven.

- `/proc/self/exe` (and `/proc/<pid>/exe`): on Android the real symlink resolves to the packaged trampoline/loader host path. The guest must instead see its own guest executable path, so the plan records the host truth, the required guest view, and the `readlink`/`readlinkat`/`openat` interposition points. The plan refuses (`ALR PROCFS SELF EXE PLAN: FAIL`) when the host truth is unknown, because the virtualization gap would be undefined.
- `/proc/self/status`: the guest identity must match the PRoot `-r` baseline. Under fakeroot the synthesized `Uid:`/`Gid:` lines collapse to `0`; without fakeroot the plan reports the process's own identity. Identity is taken from the process (`getuid`/`getgid`), never scraped from the host `/proc`.
- `/proc/mounts` (and `/proc/self/mounts`): the plan presents the rootfs as `/` plus the pseudo-filesystems the guest expects, with a host-leak guard that rejects any synthesized source/options column exposing the real Android backing store (rootfs host path, bind host paths, `/data`, `/system`, `/vendor`, …). Guest mount targets such as `/mnt/share` are guest-namespace paths and are not leaks.

The APK report must include `ALR PROCFS VIRTUALIZATION PLAN: PASS` before the project treats procfs/fakeroot mediation as planned. This gate is planning-only evidence; it is not a substitute for on-device `/proc` virtualization proof.

`resolve_interposed_access` (in `app/src/main/cpp/alr_runtime/alr_interposer.cpp`) turns that plan into in-process behavior: a guest access to `/proc/self/exe`, `/proc/<pid>/exe`, `/proc/self/status`, or `/proc/mounts` is answered from the synthesized guest view (the guest executable path, the fakeroot status lines, the synthesized mount table) with no host `/proc` access, while every other path falls back to rootfs translation. The APK report must include `ALR PROCFS INTERPOSE MECHANISM: PASS`, which proves the resolver returns the guest exe path (not the trampoline host path) and a mount table free of the host rootfs path. This is the in-process resolver, **not yet real LD_PRELOAD hooks**; wiring it into the live `LD_PRELOAD` interposer and proving it on-device is still pending.

## Performance Comparison Scaffolding

`run_perf_comparison` (in `app/src/main/cpp/alr_runtime/alr_perf.cpp`) is the harness for the PRoot-vs-ALR overhead claim. It measures only the half that is honest to measure off-device:

- ALR's in-process path translation (`translate_rootfs_path`) per-op cost. This runs identically on host and device, and **both** PRoot and ALR pay it — it is not where ALR wins.
- A real `getppid` syscall round-trip, the device-portable unit of the ptrace stop PRoot adds per intercepted guest syscall and that ALR's in-process interposer avoids.

The harness reports the translation cost in syscall-round-trip units (an ALR hot-path optimization signal, not a PRoot comparison). PRoot's actual ptrace per-syscall overhead is device/kernel specific, so `ALR PERF PROOT PTRACE DEVICE BASELINE` and `ALR PERF PROOT VS ALR DEVICE COMPARISON` stay `PENDING_DEVICE` until real on-device numbers exist. The harness never fabricates a speedup.

The APK report must include `ALR PERF HARNESS: PASS` to show the measurement scaffolding ran. The final PRoot-vs-ALR verdict remains `PENDING_DEVICE` and must be filled in only from device evidence.
