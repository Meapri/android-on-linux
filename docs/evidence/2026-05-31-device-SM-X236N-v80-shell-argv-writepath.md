# Device Evidence — SM-X236N, v80: a real shell runs commands (argv) and writes to the rootfs

Phase 1 (execution substrate) progress toward GUI apps: the loader now passes a full `argv`, so a real Debian shell runs commands; and guest file **creation** lands in the rootfs via the existing path mediation. On the orthodox roadmap these are prerequisites for everything above (shells, package tools, GTK apps).

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.80-android-argv-shell-v80`, SHA-256 `005babc2e46caf037c863f95cb79881f7ec22aa3831c0d9bd2090d6f233d93be`.

## Result

```
ALR REAL SHELL COMMAND (/bin/dash -c 'echo …', argv passing):   PASS
   argc=3, LINK MODE DYNAMIC, exit 0, stdout: alr-shell-ok
ALR WRITE-PATH MEDIATION (dash writes+reads /tmp in rootfs):     PASS
   /bin/dash -c 'echo wp-ok > /tmp/alrwp; read x < /tmp/alrwp; echo got:$x'
   stdout: got:wp-ok

(no regression: glibc static / threads+fork / dynamic / env / id all still PASS)
```

## What changed

1. **Full argv passing.** The loader's program spec is now newline-delimited `argv` — the first token is the guest path, the rest are arguments. The child pushes all `argv` strings onto the SysV stack and builds the real `argc`/`argv[]`/NULL vector (was hardcoded `argc=1`). So `/bin/dash -c 'echo alr-shell-ok'` runs as `argc=3` and the shell executes the command (`echo` is a dash builtin — no child exec needed). This is the "the Linux command line runs" milestone.

2. **Write-path mediation already works for file creation.** The stacked `SECCOMP_RET_TRACE` filter traps `openat` regardless of flags, so `openat(…, O_CREAT|O_WRONLY)` to a guest path is rewritten into the rootfs exactly like a read. dash created `/tmp/alrwp` → `…/rootfs/debian-arm64/tmp/alrwp`, wrote `wp-ok`, and read it back via a redirect — all dash builtins. Guest writes persist in the rootfs.

## Position on the roadmap

This advances Phase 1 (execution substrate) toward "general-purpose Linux process host." Still open in Phase 1, being designed in parallel (two workflows running): **unix-domain sockets** (the key new capability for Wayland/X11/D-Bus), **exec re-entry** (shells/dpkg spawning children → re-enter the ALR loader, W^X-safe), **write-path breadth** (mkdirat/unlinkat/renameat2 multi-arg rewrite), **procfs/dev synthesis**, and the **LD_PRELOAD interposer** to replace per-file-syscall ptrace at GUI scale. The critical-path Phase 3 (a minimal real Wayland compositor → Android SurfaceView) is also being designed in parallel.

Verification: pytest 235 passed, native-core PASS, validate-host PASS, Gradle BUILD SUCCESSFUL.
