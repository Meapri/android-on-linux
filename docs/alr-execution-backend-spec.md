# ALR Execution Backend Specification

## Status

Draft v0.1. This document defines the target open execution backend that will grow beyond the existing path/env skeleton. It intentionally starts smaller than a full proroot-class runtime and expands only when tests prove each layer.

## Objectives

- Execute Linux/glibc arm64 userland from an app-private rootfs on stock Android.
- Avoid direct execution of writable app-data rootfs binaries as the first process entrypoint.
- Reduce common syscall and process-management overhead below ptrace-heavy PRoot.
- Preserve predictable fallback to packaged PRoot.
- Keep implementation clean-room and test-driven.

## Component Model

```text
Android Activity / Service
  gathers package paths, permissions, Surface, user action
        |
        v
libalr_runtime_launcher.so
  Android/Bionic packaged executable or shared-object entrypoint
  prepares config, env, fds, rootfs paths
        |
        v
guest dynamic loader path
  initially use the guest rootfs loader where possible
  later add libalr_runtime_linker.so only if required
        |
        v
libalr_runtime_hook.so
  glibc LD_PRELOAD mediation layer
        |
        v
guest program
  shell, CLI tools, GUI clients, package tools
```

Optional later:

```text
libalr_runtime_bridge.so
  preserves runtime config across execve/posix_spawn

libalr_runtime_patch.so or patch module
  handles direct raw syscalls only after wrapper coverage is measured insufficient
```

## Versioned Scope

### ALR Exec v0: Existing Skeleton

Already present:

- Lexical guest path normalization.
- Rootfs host path translation.
- Minimal deterministic guest environment helper.
- Host-native C++ tests.

### ALR Exec v1: Preload Filesystem MVP

Required wrappers:

- `open`
- `open64`
- `openat`
- `openat64`
- `access`
- `faccessat`
- `stat`
- `stat64`
- `lstat`
- `lstat64`
- `fstatat`
- `newfstatat` where exposed
- `statx`
- `readlink`
- `readlinkat`
- `mkdir`
- `mkdirat`
- `unlink`
- `unlinkat`
- `rename`
- `renameat`
- `renameat2`
- `symlink`
- `symlinkat`
- `chmod`
- `fchmodat`
- `chown`
- `lchown`
- `fchownat`
- `utimensat`
- `opendir`
- `realpath`
- `getcwd`
- `chdir`
- `fchdir`

Acceptance:

```text
ALR PATH NORMALIZATION: PASS
ALR OPEN READ ROOTFS FILE: PASS
ALR STAT ROOTFS FILE: PASS
ALR READLINK PROC SELF EXE: PASS
ALR CWD TRANSLATION: PASS
ALR BASIC SHELL FILE OPS: PASS
```

### ALR Exec v2: Process Continuity

Required behavior:

- `execve` translates guest path to host executable or loader plan.
- `execvp` and `execvpe` perform guest `PATH` lookup.
- `posix_spawn` and `posix_spawnp` preserve runtime environment.
- Child process inherits rootfs config through environment and/or config fd.
- Child continuation re-enters through a packaged ALR trampoline; writable rootfs binaries are not the first Android process entrypoint.
- Android host environment is not leaked by default.
- Guest `argv[0]` remains guest-visible where practical.

Acceptance:

```text
ALR EXEC CONTINUITY PLAN: PASS
ALR EXECVE CHILD CONTINUITY PLAN: PASS
ALR EXECVP PATH LOOKUP PLAN: PASS
ALR POSIX_SPAWN CHILD PLAN: PASS
ALR EXEC CONTINUITY CONFIG HANDOFF: PASS
ALR TRAMPOLINE CONTINUE EXEC: PASS
ALR TRAMPOLINE CONTINUE CHECKSUM: PASS
ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: PASS
ALR EXECVE HELLO: PASS
ALR EXECVP PATH LOOKUP: PASS
ALR POSIX_SPAWN CHILD: PASS
ALR SHELL -C CHILD: PASS
CLEAN GUEST ENVIRONMENT: PASS
```

### ALR Exec v3: Identity and Procfs

Required behavior:

- Optional fakeroot mode returns uid/gid 0 for common identity calls.
- `stat` and `statx` metadata can be patched for root-looking ownership where enabled.
- Minimal `/proc/self`, `/proc/thread-self`, `/proc/mounts`, `/proc/self/exe`, `/proc/self/status`, `/proc/self/cmdline`, and `/proc/self/environ` are virtualized or safely passed through.
- Android app-private host paths are not unnecessarily exposed to guest tools.

Acceptance:

```text
ALR FAKE ROOT IDENTITY: PASS
ALR PROC SELF EXE: PASS
ALR PROC MOUNTS ROOTFS: PASS
ALR PROC STATUS SANITIZED: PASS
```

### ALR Exec v4: Package-Manager Preflight

Required behavior:

- `dpkg --version` passes.
- `dpkg --print-architecture` passes.
- `dpkg-query --version` passes.
- `apt --version`, `apt-get --version`, `apt-cache --version`, and `apt-config --version` pass.
- Minimal device binds such as `/dev/null`, `/dev/zero`, and `/dev/urandom` work.
- Local `.deb` install either passes or fails with a classified unsupported syscall/path/fakeroot reason.

Acceptance:

```text
ALR DPKG VERSION: PASS
ALR DPKG ARCH: PASS
ALR APT VERSION: PASS
ALR DPKG LOCAL INSTALL: PASS/KNOWN_FAIL:<reason>
```

### ALR Exec v5: Direct Syscall Coverage

Only after v1-v4 evidence shows wrapper bypass:

- Detect direct raw syscall sites in open-source guest libraries through public source or runtime behavior.
- Add selective instrumentation, seccomp user-notification experiments, or clean-room patching.
- Keep patching isolated and heavily tested.

Non-goal for v5:

- Full arbitrary binary transparency.
- Closed-runtime patch algorithm parity.

## Rootfs Model

Inputs:

- `rootfs_dir`: absolute host path under app-private storage.
- `guest_cwd`: absolute guest path.
- `guest_path`: path from guest call.
- `binds`: optional mapping list.

Rules:

- All guest absolute paths start at guest `/`.
- Relative paths resolve against guest cwd.
- `.` is removed.
- `..` clamps at guest root.
- No lexical traversal may escape `rootfs_dir`.
- Host symlinks are not resolved during lexical translation.
- Existence checks occur only after translation.
- Embedded NUL is invalid.

Bind mapping:

```text
host_path:guest_path
```

Rules:

- Longest guest-prefix match wins.
- Guest bind paths must be absolute and normalized.
- Host bind paths must be absolute.
- Bind paths are explicit escape hatches and must be visible in diagnostics.

## Environment Model

Minimal guest environment:

```text
ALR_PACKAGE=<android package>
ALR_ROOTFS=<host rootfs path>
ALR_PROGRAM=<guest program path>
ALR_BACKEND=alr-runtime
HOME=/root
TMPDIR=/tmp
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
```

Optional backend environment:

```text
ALR_CONFIG_FD=<fd>
ALR_BRIDGE_PATH=<host path>
ALR_HOOK_PATH=<host path>
ALR_VERBOSE=0|1|2
ALR_TRACE_PATH=0|1
ALR_TRACE_EXEC=0|1
ALR_FAKE_ROOT=0|1
```

Sanitization:

- Clear inherited Android environment before guest launch.
- Add only allowlisted variables.
- Do not pass `ANDROID_ROOT`, `ANDROID_DATA`, `BOOTCLASSPATH`, `DEX2OATBOOTCLASSPATH`, `SYSTEMSERVERCLASSPATH`, `ANDROID_SOCKET_*`, `EXTERNAL_STORAGE`, or vendor storage variables unless explicitly required by an Android-host helper.

## Filesystem Behavior

### Open-like Calls

For path-bearing open calls:

1. Validate path string.
2. Resolve guest path using cwd or dirfd model.
3. Apply bind mapping.
4. Translate to host path.
5. Call real syscall or libc function.
6. Preserve errno.
7. Log if tracing is enabled.

Dirfd rules:

- `AT_FDCWD` uses guest cwd.
- Rootfs-relative directory fds require fd-to-guest-path tracking.
- Host-only fds are not assumed to map back to guest paths unless opened through ALR.

### Metadata Calls

For `stat`, `fstatat`, and `statx`:

- Translate path.
- Call host metadata function.
- Patch guest-visible path-dependent metadata where required.
- Apply fakeroot uid/gid patch only when enabled.
- Apply procfs virtual metadata for known virtual proc paths.

### Symlink Calls

- `readlink` returns guest-visible target when target is under rootfs.
- Absolute host rootfs prefixes must be converted back to guest paths.
- Unsupported host symlink escapes are reported as known limitations.

### Hardlink Calls

Initial ALR does not emulate hardlinks beyond host filesystem support.

Later `link2symlink`-style emulation may be added only with a project-owned metadata format:

- Document metadata directory.
- Make original-anchor preservation explicit.
- Add migration and cleanup tests.
- Do not copy closed-runtime metadata formats.

## Process Behavior

### execve

Inputs:

- guest path
- argv
- envp

Rules:

- Program path must resolve inside rootfs or allowed bind.
- Scripts with shebang require guest shebang resolution.
- Dynamic glibc binaries require guest loader strategy.
- Child environment must include ALR runtime state.
- `argv[0]` should remain guest-compatible.
- The generated continuation argv uses the packaged trampoline as `argv[0]` and passes guest paths after `--`, not arbitrary host-selected executables.
- The packaged trampoline must validate continuation config and checksum in dry-run mode before a later milestone enables guest execution.

Fallbacks:

- If ALR cannot launch safely, return errno and report reason.
- Do not silently fall back to host shell for guest commands.

### PATH Lookup

For `execvp` and `posix_spawnp`:

- Use guest `PATH`.
- Resolve each candidate lexically inside guest namespace.
- Preserve Linux search semantics as closely as possible.
- Report candidate misses under trace mode.

## Procfs Virtualization

Initial virtual paths:

- `/proc/self/exe`
- `/proc/self/cwd`
- `/proc/self/root`
- `/proc/self/status`
- `/proc/self/cmdline`
- `/proc/self/environ`
- `/proc/mounts`
- `/proc/thread-self`

Rules:

- Prefer small generated virtual files for stable metadata.
- Pass through only paths that are safe and do not expose confusing Android internals.
- Make unsupported procfs leaves return Linux-compatible errors where possible.

## Networking and IPC

Initial ALR does not virtualize network namespaces.

Behavior:

- TCP loopback is host Android loopback.
- Guest bridge clients use explicit `ALR_*_BRIDGE_HOST` and port variables.
- Unix socket path length issues may require a runtime socket directory under app cache.
- Netlink behavior is unsupported unless explicitly emulated later.

## Error and Report Model

Every device report should include:

```text
backend=<proot|optional-proroot|alr-runtime>
command=<guest command>
exit=<code>
stdout=<trimmed or summarized>
stderr=<trimmed or summarized>
elapsed_ms=<number>
result=<PASS|FAIL|KNOWN_FAIL>
reason=<stable reason>
```

Stable reason examples:

- `unsupported-static-binary`
- `unsupported-direct-syscall`
- `missing-guest-loader`
- `path-escape-rejected`
- `procfs-leaf-unsupported`
- `fakeroot-metadata-mismatch`
- `android-seccomp-denied`
- `timeout`

## Performance Benchmarks

Minimum benchmark set:

- `hello` startup.
- `/bin/sh -c true`.
- 1,000 `stat` calls.
- 1,000 `open/read/close` calls.
- 100 child process spawns.
- `dpkg --print-architecture`.
- Guest GPU client frame stream while command producer is active.

Compare:

- PRoot backend.
- Optional external low-overhead backend if present.
- ALR runtime backend.

Report:

```text
backend
device
android_sdk
rootfs_version
workload
iterations
elapsed_ms
relative_to_proot
pass_fail
```

## Security Constraints

- Runtime translation is not a security boundary by itself.
- Rootfs archives must remain verified and safely extracted.
- Host path escapes must be rejected unless represented by explicit bind mapping.
- Guest input must not directly select arbitrary host executable paths.
- Guest-visible `/proc` should not leak sensitive app or system details beyond required compatibility.
- IPC bridge parsers must be bounded and robust against malformed guest input.

## Implementation Order

1. Expand path/env tests.
2. Add backend report schema for ALR runtime even before execution.
3. Implement launcher skeleton.
4. Implement hook library with open/stat/readlink/getcwd/chdir.
5. Add hello execution path.
6. Add shell and child execution.
7. Add procfs minimal virtualization.
8. Add fakeroot identity.
9. Add package-manager preflight.
10. Add benchmark harness.
11. Investigate direct syscall coverage only after measured bypass.

## Open Questions

- Can the first ALR dynamic execution path use the guest glibc loader directly from rootfs under Android constraints, or is a packaged clean-room loader required?
- Which Android SDK/device combinations allow the needed child process and memory mapping behavior?
- Is seccomp user notification available from ordinary APK UIDs on target devices?
- How much procfs virtualization is required before Python, Node, Git, and package tools are practical?
- What hardlink strategy is acceptable for package managers without copying external metadata formats?
