# Vendored: tawcroot (shipped as `alr-lane`)

Status: **built as the separate binary `alr-lane` and shipped beside `alr`** ([alr ADR 0010](../../docs/adr/0010-tawcroot-lane.md), which is the "new ADR" rule 2 below asked for). It is never linked into `alr`. Make target `lane`, `scripts/make-release.sh` (`libexec/alr/alr-lane`), the Termux recipe and `scripts/dev-push.sh` all build it with `-DTAWCROOT_ALR`. That flag selects local changes 3 to 9 below. It started as the reference input for spike S2 ([ADR 0004 D6](https://github.com/Meapri/android-on-linux/blob/main/docs/adr/0004-unified-direction.md), [alr ADR 0006 addendum](../../docs/adr/0006-raw-syscall-binaries.md)).

## Provenance

| Field | Value |
|---|---|
| Upstream | https://github.com/wmww/tawc (directory `tawcroot/`) |
| Commit | `d9efb4ae60da85154210c2edfb10f46e8ac58755` (2026-10-08, "release: v4") |
| Author | Sophie Winter |
| License | MIT, text in [LICENSE.MIT](LICENSE.MIT) (copied verbatim from upstream `LICENSE.MIT`) |
| Local modifications | nine, listed under [Local changes](#local-changes) |

Upstream's own licensing note ([notes/licensing.md](https://github.com/wmww/tawc/blob/main/notes/licensing.md)): tawc's sources are MIT; the distributed tawc APK is GPLv3-only because it links termux-app's `termux-shared` extra-keys code. That code is **not** part of what is vendored here. `tawcroot` is freestanding C with no third-party dependencies (no libc, no malloc).

## What is included

- `src/`, `include/`: the production sources (about 16k lines of C and four assembly stubs for aarch64 and x86_64).
- `perf/`: the micro-benchmark harness and upstream's recorded results.
- `LICENSE.MIT`.

## What is excluded

- `tests/` (needs the `cleat` test framework, a separate dependency whose license has not been reviewed here).
- `build.sh`, `Makefile`, `ando/`, and the design notes (`notes/tawcroot/*.md`). The notes are worth reading upstream: `sigsys-handler.md`, `seccomp-filter.md`, `path-translation.md`.

## How it works, in one paragraph

A freestanding static non-PIE binary installs a seccomp filter that returns `SECCOMP_RET_ALLOW` for syscalls issued from its own trampoline range and `SECCOMP_RET_TRAP` for the path-bearing syscalls issued anywhere else. The `SIGSYS` handler runs in the guest thread, translates the path against the rootfs, and re-issues the syscall from the allowed range. It also loads the target ELF itself (`loader_*.c`), which is how it installs the handler before the first guest instruction in a static binary. This is the mechanism ADR 0004 D6 wants to measure.

## Build check (host only)

All of `src/*.c` and the `x86_64` stubs compile with upstream's flags (`-O2 -ffreestanding -fno-stack-protector -D_GNU_SOURCE -Iinclude`) and link with GNU ld using `-static -nostdlib -nostartfiles -Wl,-Ttext-segment=0x10000000`. Upstream uses `--image-base` (lld). Verified 2026-10-11 on an x86_64 Linux VM (kernel 6.18).

A cross-compile against musl headers (`zig cc -target aarch64-linux-musl`, 2026-10-11) fails in `include/arch/aarch64.h` because musl declares `mcontext_t.regs` as `unsigned long[31]`, not `__u64[31]`, and in `linux/stat.h` (`statx_timestamp` redefined). Upstream builds against the NDK's bionic sysroot.

aarch64 builds on the device with Termux's clang 21 (2026-10-11), with the flags above plus `-mno-outline-atomics -fno-pie`, linked `-Wl,--image-base=0x10000000`. Without `-mno-outline-atomics` the link fails on `__aarch64_cas4_acq` and friends, which live in libgcc/compiler-rt and are not linked into a `-nostdlib` binary.

The release build uses NDK 27.2 clang (`--target=aarch64-linux-android24`) with the same flags, plus 16 KiB page alignment. It is warning-free, and the device acceptance run used it ([lane evidence](../../docs/evidence/2026-10-11-t5-lane.md)).

Host smoke, same VM: a static glibc binary run under `tawcroot -r ROOT -- /bin/h` opened `/etc/hostname` and read the file inside `ROOT`, not the host's. That is the case `LD_PRELOAD` cannot reach.

## Reference numbers (not evidence for Android)

| Source | stat() through the trap path | native stat() |
|---|---|---|
| Upstream `perf/results.md`, AMD laptop, 2026-06-10 | 2,285 ns | 352 ns |
| This repository, x86_64 VM, 2026-10-11, 200k iterations, 3 runs | 4,510 to 4,807 ns | 444 to 504 ns |

The D6 gate itself is now measured on a device without tawcroot: [`bench/microbench/trap_cost.c`](../../bench/microbench/trap_cost.c) reproduces tawcroot's filter shape and measured +410 to +925 ns per `getppid` round trip on the Snapdragon reference device ([evidence](../../docs/evidence/2026-10-11-s2-ret-trap-cost.md)). That excludes path translation; the table below still has no device row.

Neither number is a measurement on an Android device. The device workload numbers are in the [T5 coexistence spike](../../docs/evidence/2026-10-11-t5-coexistence-spike.md): `find /usr/share -type f` over 61,865 files took 349 to 368 ms under tawcroot against 151 to 177 ms native (Snapdragon 8 Elite, Android 17).

## Local changes

1 and 2 were made for the [T5 coexistence spike](../../docs/evidence/2026-10-11-t5-coexistence-spike.md), 2026-10-11, and are unconditional.

1. **`RET_TRAP` carries data `0x7477`** (`include/filter_build.h` `TAWCROOT_TRAP_DATA`, `src/filter_build.c`). The data reaches the `SIGSYS` as `si_errno`. alr's ptrace supervisor rescues the zygote filter's traps, whose data is 0, and must let tawcroot's through to its handler; with data 0 on both it answered tawcroot's path traps with `-ENOSYS` and nothing loaded.
2. **`prctl(PR_GET_AUXV)` returns the guest's auxv** (`src/syscalls_control.c` `handle_prctl`, `src/loader_stack.c` `tawc_loader_guest_auxv`). Passed through, it returned tawcroot's own auxv, whose `AT_EXECFN` is tawcroot or the `--exec-child` fd; uutils names itself from it via rustix and failed with `unknown program 'tawcroot'`.

3 to 9 were made for the [lane integration](../../docs/evidence/2026-10-11-t5-lane.md), 2026-10-11. They exist only under `-DTAWCROOT_ALR` (`include/alr_mode.h` lists them). Without the flag the tree builds upstream's behaviour. The first three exist because alr's preload lane shares the rootfs: what one lane writes, the other must read the same way.

3. **Real ids unless `--fakeroot`** (`src/main.c`, `src/identity.c` `tawcroot_identity_reset`, `src/syscalls_stat.c` `decorate_stat`). The lane starts with the real uid/gid/groups and does not decorate stat as root-owned. alr passes `--fakeroot` when `ALR_FAKEROOT=1`, which restores upstream's fake root. Upstream's uid 0 differed from the preload lane: `id` changed when a process crossed lanes, and git's `safe.directory` check compares `st_uid` with `geteuid`.
4. **Absolute symlink targets are stored relative** (`src/syscalls_fs.c` `alr_relativize`). This is the same rule as the preload's `relativize`. The depth comes from the parent directory opened `O_PATH` and read back through `/proc/self/fd`. Targets in `/proc`, `/sys` and `/dev` stay absolute. Stored verbatim, `/etc/os-release` pointed at Android's `/etc` from the preload lane.
5. **No link store; a refused hardlink is a copy** (`src/main.c`, `src/syscalls_link.c` `alr_link_or_copy`). The source's bytes and mode are copied with `sendfile`; a symlink's target is copied as a new symlink. Upstream's store names were `tawcroot:link:<token>` symlinks, which the preload lane read as dangling links. It also created a `tawcroot/` directory beside the distro directories.
6. **Rescue and dircache hooks in the dispatcher** (`src/rescue.c` `tawcroot_dispatch_call`). The EACCES rescue widens modes as `CAP_DAC_OVERRIDE` would, so it only runs when the virtual euid is 0. Claiming its slot cost a `gettid` on every trap. After `renameat`, `renameat2`, `unlinkat`, `symlinkat` and `chroot` the directory cache (7) is cleared.
7. **Directory cache in the symlink resolver** (`src/path_resolve.c`). The resolver remembers prefixes it found not to be symlinks: 1024 direct-mapped slots, one seqcount each. A hit lets the walk start at the leaf. This cut `readlinkat` from 2.3 to 0.44 per trap on `find`. Staleness and why it is acceptable in an alr rootfs are covered in the comment there.
8. **tgid cache** (`src/usercopy.c` `tawc_self_tgid`). The tgid of the process is cached in a `MADV_WIPEONFORK` page that fork(2) zeroes. It was first used to address `process_vm_readv`/`writev`, which cost 1 to 2 `gettid` per trap. Since 10 those copies are a fallback and address the calling thread, because a vfork child can fill the cache with a pid that is later reused. The cache now tells the memory owner from a `CLONE_VM` child (10), and a fork child fills it before running guest code (16).
9. **Stat results written by the kernel** (`src/syscalls_stat.c` `alr_stat_direct`). With the real ids and no store there is nothing to fix up, so `fstatat`, `statx` and `fstat` get the guest's buffer directly. This saves a `process_vm_writev` per stat.

Also for 3 to 9: `include/sysnr.h` gains `sendfile` and `madvise`, and `include/errno_neg.h` gains `ESRCH`.

10 to 15 were made to bring the lane's per-syscall cost within the T2 budget ([evidence](../../docs/evidence/2026-10-11-t5-lane-cost.md), [ADR 0010](../../docs/adr/0010-tawcroot-lane.md) decision 7), 2026-10-11. They also exist only under `-DTAWCROOT_ALR`. busybox `find /usr/share` went from 1.28x to 1.08x of `ALR_LANE=0`, `ls -lR` from 1.33x to 1.09x.

10. **Guest memory is read directly; SIGSEGV and SIGBUS are owned** (`src/alr_fault.c`, `src/arch/aarch64_alr_fault.S`, `src/usercopy.c`, `src/handler.c`, `src/syscalls_control.c`, `src/exec_handler.c`, `src/supervisor.c`). Paths and stat buffers are copied with load/store loops between `tawc_safe_begin` and `tawc_safe_end`. A fault inside them resumes at `tawc_safe_fault`, which returns -1, and the caller reports `EFAULT`. `process_vm_readv` cost about as much as the lookup it served. To see those faults, alr-lane keeps its own entry installed for both signals. The guest's `rt_sigaction` reads and writes a shadow, and the kernel gets the guest's flags and mask so delivery looks the same. A guest fault tail-calls the guest handler with the kernel's arguments; under `SIG_DFL` the kernel action is reset and the instruction faults again; `SA_RESETHAND` is applied to the shadow; a signal sent while alr-lane's own code runs is requeued with `rt_tgsigqueueinfo` and stays blocked until the trap returns. A `CLONE_VM` child (vfork) keeps its settings in a separate small table, so it cannot change its parent's handler. The SIGSYS handler unblocks both signals while it runs. `SIG_IGN` is handed back to the kernel before `execve`, because exec inherits it. `include/sysnr.h` gains `rt_tgsigqueueinfo`.
11. **One name is looked up by the kernel** (`src/alr_fast.c`, `src/syscalls_stat.c`, `src/syscalls_fs.c`, `src/path.c`, `src/rescue.c`). A relative path that is one name other than `..` is passed through with the guest's dirfd when the directory is on the rootfs's filesystem, the name is not a bind at the rootfs root, and the name is not a symlink (stat looks with `AT_SYMLINK_NOFOLLOW` first; open checks with `readlinkat`). Only when no fake root, no store, no read-only view and only depth-1 binds are in use. The cwd's kind and guest path are cached per process; `chdir`, `fchdir` and `chroot` bump a generation, and the cached value carries the pid that computed it, so a vfork child's `chdir` is not used by its parent.
12. **A following stat looks at the leaf without following first** (`src/syscalls_stat.c` `alr_follow_stat`, `alr_follow_statx`, `src/alr_fast.c`). Most leaves are not symlinks, so the leaf's `readlinkat` is skipped. A symlink leaf is translated through a 64-slot cache keyed by the link's rootfs path and checked by the link's inode and ctime. A symlink's target cannot change in place, so a matching inode and ctime mean the same target. The cache is cleared with the directory cache.
13. **`fstat` and `getdents64` trap only where needed** (`src/filter_build.c`, `src/alr_fast.c` `tawcroot_alr_procfd_place`, `src/syscalls_fs.c`). With the real ids `fstat` has nothing to decorate, so it traps only for alr-lane's reserved fds, as `close` does. `getdents64` only hides alr-lane's fds in `/proc/<pid>/fd`, so it traps only for fds 900 to 999, and a `/proc/<pid>/fd` directory opened through the bind gets a small stacked seccomp filter (`SECCOMP_FILTER_FLAG_TSYNC`) that traps `getdents64` on exactly its fd number. The fd keeps the number the kernel gave it. The filter stays after the directory is closed, so a later directory at that number traps and is listed unchanged by the handler; at most 32 numbers per image get one, and a `CLONE_VM` child installs none. When no filter can be added the directory moves into the 900-999 band instead.
14. **The directory cache holds following leaves too** (`src/path_resolve.c`). A directory opened with following, as `opendir` does, is remembered, so stats of its children do not read the parent again. 6 now also invalidates the cwd cache on `chdir` and `fchdir`, and 11 and 12's caches on every mutator.
15. **The handler's frame limit is an error** (`Makefile`, `scripts/dev-push.sh`, `scripts/make-release.sh`, `packaging/termux/alr/build.sh`). The handler runs on the guest thread's stack. Upstream's `-Wframe-larger-than=1024` is now `-Wframe-larger-than=1024 -Werror=frame-larger-than`; clang does not accept `-Werror=frame-larger-than=1024`.
16. **Fork-type clones trap** (`src/syscalls_control.c` `handle_clone_fork`, `src/filter_build.c`, `src/usercopy.c` `tawc_self_tgid_forked`). Made after 10 to 15 for a defect they left: a fork child whose first trap came from its own vfork child cached that child's pid as the memory owner, and the vfork child's signal reset then replaced the fork child's SIGSEGV handler. `clone` without `CLONE_VM` now traps; the handler runs it, and the child stores its tgid before it returns to guest code. A new child stack is applied to the child's signal context. Threads and vfork still do not trap. Fork cost was unchanged within noise ([follow-up](../../docs/evidence/2026-10-11-t5-lane-cost.md) §5).

## Rules for using this code

1. Keep `LICENSE.MIT` and the copyright notice with every copy or derivative.
2. Do not link this into the shipped product until spike S2 passes and a new ADR says so. (Done: [ADR 0010](../../docs/adr/0010-tawcroot-lane.md) ships it as the separate binary `alr-lane`; it is still never linked into `alr`.)
3. Record every local change in this file, with the reason.
4. Do not copy files from upstream's GPL-covered dependencies (`deps/` there).
