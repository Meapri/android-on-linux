# Device Evidence — SM-X236N, v76: multi-threaded + forking static glibc runs natively

Generalization of the v73 single-binary breakthrough: a static-glibc ARM64 binary that **spawns a pthread and fork()s a child** runs natively in-process under ALR — all threads/children traced, all seccomp-blocked syscalls serviced, clean exit.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, arm64-v8a, untrusted_app). APK `0.4.76-android-native-loader-mt-ship-v76`, SHA-256 `7500ba08fee7e330809066695d809f48b54d56d16e26b8bd2ffcfffe3be47a43`.

## Result

```
ALR NATIVE LOADER GUEST EXEC (glibc static):        PASS   (/bin/hello)
ALR NATIVE LOADER GUEST EXEC (glibc threads+fork):  PASS   (/bin/mt-test)
  child exit=0 signal=0
  guest threads spawned=1
  seccomp-emulated syscalls=3 nums=99,99,99
  stdout: main ok / thread ok / child ok
```

`/bin/mt-test` (static glibc ET_EXEC, with PT_TLS + IRELATIVE relocs; built by a subagent via zig-cc + Debian glibc `libc.a` + ld.lld) prints `main ok`, creates a pthread that prints `thread ok` and joins it, then `fork()`s a child that prints `child ok` and `waitpid()`s it, exit 0. All three lines appear in the captured stdout — every path ran natively.

## How the multi-process generalization works

1. **Multi-tracee supervisor** (`runtime_report.cpp`, the parent ptrace loop): the child does `PTRACE_TRACEME` then `SIGSTOP`s so the parent can set `PTRACE_O_TRACECLONE | TRACEFORK | TRACEVFORK | TRACEEXEC` before the guest runs. The parent then `waitpid(-1, __WALL)` — catching the guest plus every thread it `clone`s and every process it `fork`s. Here: `guest threads spawned=1` (the pthread's `PTRACE_EVENT_CLONE`), and the forked child was traced too.
2. **Per-tracee seccomp servicing**: each of the 3 tasks (main, pthread, forked child) calls `set_robust_list` (nr 99) during glibc thread/fork startup — Android's app seccomp filter blocks it (SIGSYS). The supervisor emulates each (3 total) and resumes. Only blocked syscalls trap; everything else (the pthread `clone`, the `fork`, `futex`, `write`, `waitpid`, …) runs natively — Android's app policy *allows* `clone`/`clone3`/`futext`/`statx`/`getrandom`, so they never trap.
3. **`-ENOSYS`, not `0`**: the supervisor now writes `-ENOSYS (-38)` into the return register for a blocked syscall, not a fake `0`. glibc ignores the result of `set_robust_list`/`rseq`, but for syscalls it has fallbacks for (e.g. `faccessat2 -> faccessat`) the fallback keys specifically on `ENOSYS`; faking success or `EPERM` would break it. (This correction came from the parallel seccomp-policy analysis.)

## Significance

The ALR native-exec model now handles **real multi-process glibc workloads** (threads + fork), not just a single static binary:
- W^X-safe in-process native execution (anonymous execmem ELF loader).
- ART libsigchain bypassed (raw `rt_sigaction` SIG_DFL reset).
- A lightweight supervisor that traces every guest thread/child but only round-trips on the rare seccomp-blocked syscalls (3 here) — fundamentally cheaper than PRoot's trap-every-syscall model.

This is the path from "hello runs" to "shells and package managers run." Remaining: the supervisor's blocked-syscall dispatch should *service* (not just `-ENOSYS`-stub) the few syscalls a guest depends the result of — wire `translate_rootfs_path`/`resolve_interposed_access` for path/procfs/fakeroot — and broaden the GPU marshalling API surface. Both pillars (native exec + GPU passthrough) are now demonstrated and generalizing on a real device.
