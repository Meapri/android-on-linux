# Device Evidence — round-9 (v140): Option S (kernel-execve re-entry stub) is DEAD — W^X blocks execve of app-storage files

Build `0.4.140-r9-v140`. Device SM-X236N / Mali-G615 MC2 / Android 16 / untrusted_app, targetSdk 35. Two cold-start drains (drain#18). Tests ADR-003-v2 Option S: splice a guest `execve(target)` to run a static re-entry stub the kernel *can* load, which then maps the glibc target in-process.

## The splice mechanism works ✓ — the execve does not ✗
The supervisor splice fires correctly on every guest execve/execveat (`ALR_EXEC_REENTRY` was default-on for this experiment):
```
alr exec reentry=on spliced=1
alr exec reentry stub=<rootfs>/usr/lib/androlinux/alr-reentry target=<rootfs>/bin/sh
alr exec x0=/bin/sh reason=rewrite envp_reason=already
... child exit=0  exec_events=0
(same for /bin/dash)
STUB ran? -> (no ALR-REENTRY output at all)
```
`spliced=1` with the correct stub + target paths proves the argv/x0 rewrite is correct. But `exec_events=0` (PTRACE_O_TRACEEXEC *is* set — verified L1929 — so this means the execve never completed) and the stub produced **zero output** — it never ran.

## Root cause — W^X SELinux denial of execve on app_data_file (the wall ALR exists to avoid)
The first drain was invalid (cold-start re-provisioning of `/usr/lib/androlinux/` had wiped the manually-staged stub). The second drain **re-pushed the stub every 2 s** so it was confirmed present at the splice path (`-rwxr-xr-x ... alr-reentry`, verified at drain end) — and the result was **identical**: `exec_events=0`, stub never ran.

`ls -Z` shows the rootfs files (incl. the stub and `/bin/dash`) are labeled **`app_data_file`**. On Android 10+ with **targetSdk ≥ 29** (this app is 35), the `untrusted_app` SELinux policy carries `neverallow untrusted_app … app_data_file:file execute` (W^X enforcement). So the kernel **cannot `execve()` any file in app storage**, no matter how valid the static ELF is. This is the *same* wall that necessitates ALR's in-process ELF mapping (the loader maps via `mmap(PROT_EXEC)` of file-backed `app_data_file`, which *is* allowed — `execve` is not).

**Conclusion: Option S (kernel-execve of a rootfs-path re-entry stub) is architecturally dead on non-root untrusted_app.** `B-1` path-rewrite + `B-3` envp injection were necessary-but-not-sufficient; the missing piece cannot be a kernel execve of any kind.

## Action taken
- The splice is **gated default-OFF** (`ALR_EXEC_REENTRY=1` to opt in); behavior reverts to the proven v139 B-1 path, so v140 is regression-clean vs v139. The splice mechanism is retained only as the documented dead-end + a reusable argv-rewrite primitive.
- No regression with the gate off: GPU LIVE + VK RENDER PASS, glmark2 1075. (With the gate ON, a qt6 flake appeared — a side effect of routing every exec through the failing stub — another reason it is now OFF.)
- `nativeLibraryDir` (the bundled-executable execve hatch) is **not available** here: `extractNativeLibs` is unset (default false at targetSdk 23+), so native libs are mmap'd from inside the APK, not extracted as files — there is no executable on-disk path to target.

## The proper pivot — ADR-003-v3: in-process re-map, NO execve
Decided direction (per user: "the more fundamental, better-performing, proper solution"). On the execve seccomp-trap: **cancel the execve syscall and redirect the tracee into a resident, file-backed-`PROT_EXEC` trampoline** (mapped at guest launch — the same mmap ALR already uses, which W^X allows) that maps the guest ld.so + new target ELF in-process and jumps to ld.so — inheriting the seccomp filter + ptrace SEIZE with **no kernel execve at all**. This is also faster (no process image swap / thread teardown). Reuses the `alr_reentry.c` mapping core. Design + implementation = round-10.
