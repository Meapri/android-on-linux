# Device Evidence — SM-X236N, v73: a real static glibc binary runs natively, in-process

**Breakthrough for pillar 1.** A real static-glibc ARM64 Linux binary executes natively inside the non-root Android app process — no PRoot, no ptrace-per-syscall, W^X-safe — and prints its output.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, arm64-v8a, untrusted_app). APK `0.4.73-android-native-loader-seccomp-emul-v73`, SHA-256 `78b1e93550d48f8244c08dc7a2b814ef8e029862248b9b835f13e74403e4d545`.

## Result

```
ALR NATIVE LOADER GUEST EXEC (glibc static): PASS
alr native loader child exit=0 signal=0
alr native loader seccomp-emulated syscalls=1 nums=99
alr native loader guest stdout=hello from static arm64 rootfs
```

`/bin/hello` (static ET_EXEC glibc, 537664 bytes, with IFUNC IRELATIVE relocs + PT_TLS) was loaded by the userspace ELF loader and ran to a clean `exit(0)`, producing `hello from static arm64 rootfs` on its stdout. Only **one** syscall (number 99 = `set_robust_list`) had to be serviced by the supervisor.

## How it was cracked (the diagnosis chain)

Each step used parent-side `ptrace` (the parent has intact bionic TLS, so it can observe the guest even after the guest hijacks the thread pointer):

1. Crash captured: `signal 11, pc=0x7b9d4a0fe0, fault addr=0x30`. `/proc/<pid>/maps` placed the pc in **bionic `libc.so`** at offset 0x33fe0.
2. Disassembly of `libc.so+0x33fe0` = **`pthread_getspecific+0x40`**, instruction `ldr x10,[x8]` with `x8=0x30` (a TLS read off a null base).
3. The fault registers gave `lr=0x7b804e9bb0` → **`/apex/com.android.art/lib64/libsigchain.so`** (ART signal chaining), `x0=0x80000003` (a pthread key). So: a guest signal routed through ART's process-wide `libsigchain` SIGSEGV handler, which called bionic `pthread_getspecific` using bionic TLS — but the guest had set its own `TPIDR_EL0`, so that read faulted. libsigchain was *masking* the real guest fault.
4. **Fix A — bypass libsigchain:** in the child, reset all signal dispositions to `SIG_DFL` via the RAW `rt_sigaction` syscall (not libc's, which libsigchain wraps). The pthread_getspecific crash vanished; the real fault surfaced: **`signal 31 (SIGSYS)`**, pc/lr both in the guest image → the guest glibc made a syscall the Android app seccomp filter forbids. `x8=99`.
5. **Fix B — lightweight syscall supervisor:** the parent traces the child and, on `SIGSYS` only, emulates the blocked syscall (set the return register to 0, resume WITHOUT delivering the signal). `set_robust_list` (99) — which glibc calls during thread setup — was emulated once, and the guest ran to completion.

## Why this matters

This is the decisive pillar-1 proof: ALR can run real glibc Linux binaries natively on non-root Android. The execution model is:
- **W^X-safe in-process native exec**: a userspace ELF loader maps `PT_LOAD` into anonymous `execmem` (proven allowed in untrusted_app), applies IRELATIVE relocs, builds a SysV stack+auxv with a clean TCB, and branches to the entry — no writable-file exec, no memfd exec (which is SELinux-blocked here).
- **libsigchain bypass** so ART's signal interception doesn't fight the guest's TLS.
- **A seccomp supervisor that traps ONLY the forbidden syscalls** (here: 1 syscall) — fundamentally lower overhead than PRoot, which ptrace-traps EVERY syscall. Most syscalls run natively at full speed; only the rare blocked ones round-trip to the supervisor.

## Both pillars now demonstrated on real hardware

- **Native CPU exec (1): PASS** — real static glibc binary runs natively in-process (this run).
- **GPU passthrough (2): demonstrated** — host GPU hardware render (Mali, 0 drops), cheap transport boundary (shared-ring ~105K cmds/frame), and a command-marshalling PoC that dispatches a guest GLES stream to real GLES calls with pixel-verified output (v69).

Remaining for production: broaden the seccomp-emulation supervisor (more guest binaries will hit more blocked syscalls; some may need real servicing, not just success-stubbing), path/procfs/fakeroot mediation for the in-process guest, the GPU marshalling API surface, and multi-vendor (Adreno) coverage. But the two fundamental unknowns are now both answered YES on a real device.
