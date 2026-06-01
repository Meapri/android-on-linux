# Device Evidence — SM-X236N, v77: low-overhead rootfs path mediation proven (seccomp-trace + supervisor rewrite)

The last missing mechanism for running **real** Linux programs (dash, ls, coreutils — anything that opens `/etc`, `/lib`, `/usr`) under the in-process ALR loader: giving the guest a **rootfs filesystem view without PRoot's trap-every-syscall overhead**. Proven end-to-end on real hardware.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, arm64-v8a, untrusted_app). APK `0.4.77-android-seccomp-pathtrap-v77`, SHA-256 `73f30b300d6ab04675fe82d3d10638f806c05d99c831cce7f4f4d093e69f695b`.

## Result

```
ALR SECCOMP PATH-MEDIATION (seccomp-trace rootfs path rewrite): VIABLE

alr sc filter_install=OK                          (how=seccomp)
alr sc seccomp_trace_events=1
alr sc observed_guest_path=.../files/alr-sc-decoy-DOES-NOT-EXIST.txt
alr sc trapped_decoy_match=yes
alr sc path_rewrites=1
alr sc child_diag=FILTER_OK how=seccomp;OPEN_OK content=ALR_REWRITE_OK
alr sc PATH_MEDIATION_VIABLE=yes
```

## The keystone questions, answered on metal

The problem: the in-process guest's file syscalls (`openat`/`stat`/`readlinkat`/…) are **not** seccomp-blocked, so they run natively against the **Android** filesystem — the guest sees Android's `/etc`,`/lib`,`/usr`, not the extracted rootfs. PRoot fixes this by ptrace-ing *every* syscall and rewriting path args (high overhead). The cheap alternative is to trap **only** the path syscalls. Two unknowns had to hold on this device, and both do:

1. **Can an `untrusted_app` stack a *second* seccomp filter?** → **YES** (`filter_install=OK how=seccomp`). The kernel allows it because `NO_NEW_PRIVS` is already set by the zygote (the privilege gate is satisfied without root), and the `seccomp(2)` syscall itself is not blocked by the existing zygote filter. Stacked filters compose most-restrictively (`max(action)`), so an ALR filter that returns `SECCOMP_RET_TRACE` for `openat` wins over the zygote's `ALLOW`, while the zygote's existing blocks (set_robust_list etc.) are untouched.

2. **Does `SECCOMP_RET_TRACE` deliver `PTRACE_EVENT_SECCOMP` for the trapped syscall, before it runs?** → **YES** (`seccomp_trace_events=1`). The tracee stops at the seccomp-stop *before* the kernel dereferences the path, so the supervisor has a clean, TOCTOU-safe window to rewrite it.

## The full mechanism, demonstrated end-to-end

The probe (`build_seccomp_pathtrap_probe`, `runtime_report.cpp`) runs the entire chain a real loader would:

1. The child installs a 2nd seccomp filter: `RET_TRACE` for `openat`, `RET_ALLOW` otherwise (classic BPF, arch-guarded `AUDIT_ARCH_AARCH64`), via `seccomp(2)` (falls back to `prctl(PR_SET_SECCOMP)`).
2. The child `openat()`s a **decoy** path that does not exist (`…/alr-sc-decoy-DOES-NOT-EXIST.txt`).
3. The parent supervisor catches `PTRACE_EVENT_SECCOMP`, reads `x1` (the `pathname` arg) from the child's address space (`/proc/<tid>/mem`, with `process_vm_readv` fallback), and confirms it is the decoy (`trapped_decoy_match=yes`).
4. The parent **rewrites the path in place** to a **real** file (`…/alr-sc-real.txt`, a shorter string so it fits) holding the marker `ALR_REWRITE_OK` (`path_rewrites=1`).
5. `PTRACE_CONT` runs the syscall **once** against the rewritten path (`RET_TRACE` does not re-enter the filter, so no infinite-trap loop). The child's `openat` **succeeds** and reads the marker: `OPEN_OK content=ALR_REWRITE_OK`.

That last line is the proof: a guest open of one path was transparently serviced from a different (rootfs) path, with the guest none the wiser.

## Why this matters

This is the cheap path-mediation primitive the project needed. Combined with the already-proven pieces, the mechanical path to running real Linux apps is now complete on a real non-root device:
- **Native static glibc exec** (v73) + **threads/fork** (v76) — in-process, W^X-safe.
- **Selective seccomp path mediation** (v77, here) — rootfs filesystem view at only-trap-file-syscalls cost.
- **GPU marshalling** (v69) — guest GLES → real Mali render, pixel-verified.

Design note (from the parallel `alr-fs-mediation` workflow): use `RET_TRACE`→`PTRACE_EVENT_SECCOMP` in the parent supervisor (not an in-process `RET_TRAP`/SIGSYS handler) — it composes with the existing multi-tracee `waitpid(-1,__WALL)` loop, freezes the whole tracee while rewriting (TOCTOU-safe with a per-TID scratch), and never re-enters the trap. The probe validates exactly that model.

## Next

Wire this into `build_native_loader_probe`'s real supervisor: stack the `RET_TRACE` path-syscall filter in the child, add a `PTRACE_EVENT_SECCOMP` branch that rewrites `openat`/`openat2`/`newfstatat`/`statx`/`faccessat`/`faccessat2`/`readlinkat`/`execve(at)` args through `translate_rootfs_path`, then run the static-glibc `fileio-test` binary (built this session, opens `/etc/alr-probe.txt`) → expect `fileio: alr-rootfs-ok` once `/etc` routes into the rootfs. Static test binaries staged at `/tmp/alrbins/` (fileio-test, alr-ls, syscall-stress, busybox).
