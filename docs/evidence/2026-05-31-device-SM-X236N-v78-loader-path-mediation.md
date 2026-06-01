# Device Evidence — SM-X236N, v78: a real file-using glibc program runs with a rootfs view

Path mediation is now **wired into the real native-exec loader** and proven end-to-end: a static-glibc ARM64 program that opens `/etc/alr-probe.txt` and `opendir("/etc")` reads the **rootfs** filesystem (not Android's), in-process, non-root, W^X-safe — no PRoot.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, arm64-v8a, untrusted_app). APK `0.4.78-android-loader-pathmed-v78`, SHA-256 `9431f6a154ba83dee3e13b5f06517e0186a96c7adb45269548b52bae85708631`.

## Result

```
ALR NATIVE LOADER GUEST EXEC (glibc static):        PASS   (/bin/hello — no regression)
ALR NATIVE LOADER GUEST EXEC (glibc threads+fork):  PASS   (/bin/mt-test — no regression)
ALR SECCOMP PATH-MEDIATION (standalone probe):      VIABLE
ALR LOADER PATH-MEDIATION REAL FILE READ:           PASS   (/bin/fileio-test)

fileio-test:  child exit=0
  path-mediation traps=4 rewrites=2
  first path rewrite=/etc/alr-probe.txt => …/files/rootfs/debian-arm64/etc/alr-probe.txt
  stdout:
    fileio: alr-rootfs-ok        <- read the ROOTFS file, not Android's /etc
    dirent: group                <- opendir("/etc") listed ROOTFS /etc
    dirent: apt
    dirent: nsswitch.conf
```

`/bin/fileio-test` (static glibc ET_EXEC, built this session) opens `/etc/alr-probe.txt`, prints its contents, then lists the first entries of `/etc`. Android's `/etc` has no `alr-probe.txt` — so reading `alr-rootfs-ok` (the rootfs fixture) and listing the Debian rootfs's `/etc` (group/apt/nsswitch.conf) is the proof the guest's file syscalls were transparently serviced from the **rootfs**.

## How it works in the loader

Built on the v77-proven mechanism, now integrated into `build_native_loader_probe`:

1. **Child stacks a path-mediation seccomp filter** (`alr_install_path_trace_filter`) right before `alr_enter_guest`, as the last thing before the guest runs — so none of the loader's own file reads trap. The filter `SECCOMP_RET_TRACE`s only the path syscalls (`openat`, `openat2`, `newfstatat`, `statx`, `faccessat`, `faccessat2`, `readlinkat`; all carry the pathname in x1), `RET_ALLOW` for everything else.
2. **The multi-tracee supervisor handles `PTRACE_EVENT_SECCOMP`** (added `PTRACE_O_TRACESECCOMP` to `SETOPTIONS`): it reads x1 from the frozen tracee's `/proc/<tid>/mem`, translates the guest path to its rootfs host path via `translate_rootfs_path`, writes the host path into a **per-thread stack scratch** (`sp - 2048`, race-free across threads/forks), repoints x1, and `PTRACE_CONT`s — the kernel runs the syscall once against the rewritten path (`RET_TRACE` does not re-enter the filter).
3. **Kernel virtual filesystems (`/proc`, `/sys`, `/dev`) are left native** (a prefix guard) — they have no rootfs backing and need synthesis later; leaving them native keeps e.g. `/proc/self/exe` valid. Observed live: glibc startup touches `/proc/self/exe` (1 trap, **not** rewritten), and fileio-test's two `/etc` accesses are rewritten (`rewrites=2`).

Only file syscalls trap — `hello`/`mt-test` (which open no rootfs files) still pass unchanged; their only trap is the harmless, non-rewritten `/proc/self/exe`. This is fundamentally cheaper than PRoot's trap-every-syscall model.

Also landed this build (from the adversarial loader audit): **argv[0] is now the actual guest program path** (was hardcoded `/bin/hello`) — required for busybox-style multi-call binaries.

## Significance

The full mechanical stack to run **real** Linux programs on non-root Android is now demonstrated on a real device:
- Native static glibc exec (v73) + threads/fork (v76) — in-process, W^X-safe.
- **Rootfs filesystem view via low-overhead selective seccomp path mediation (v78, here)** — real file-using programs work.
- GPU marshalling → real Mali render (v69).

Verification: pytest 235 passed, native-core PASS, validate-host PASS, Gradle BUILD SUCCESSFUL. Static test binaries shipped in the rootfs: `/bin/fileio-test`, `/bin/alr-ls`, `/bin/mt-test`; fixture `/etc/alr-probe.txt`.

## Next
- Run `/bin/alr-ls` (a real coreutils-style directory lister) through the mediated loader.
- Apply the remaining audit fixes (supervisor `si_code==SYS_SECCOMP` guard, runaway kill-group, 16 KiB page size).
- Implement the dynamic-linked-binary loader (design ready at `docs/design/dynamic-loader-patch.md`) to unlock the rootfs's real dynamic programs (dash/dpkg/apt).
- Procfs synthesis (`/proc/self/exe` → guest exe) via the interposer.
