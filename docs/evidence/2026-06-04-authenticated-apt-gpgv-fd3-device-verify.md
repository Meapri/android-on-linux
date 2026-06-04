# Authenticated apt-get (gpgv) — exec-re-entry characterization + DEVICE-VERIFY checklist

Date: 2026-06-04. Branch: see commit trailer. HEAD baseline: `5d1eb3e`.

## 1. The exec-re-entry mechanism (host-read characterization)

The authenticated `apt-get update` verify chain is, on noble apt 2.8.3:

```
apt (re-mapped guest, the launch process)
  └─ fork + execve /usr/lib/apt/methods/gpgv   (the "gpgv method", an ELF)
       └─ ExecGPGV: pipe(status) [both ends CLOEXEC] ; dup2(write,3) [clears CLOEXEC on 3]
          fork ; child execvp(/usr/bin/apt-key, [apt-key, verify, --keyring <Signed-By>, sig, data])
            └─ /usr/bin/apt-key  is a  #!/bin/sh  SCRIPT  → re-mapped to /bin/sh (dash)
                 ├─ $(mktemp …) , chmod , cat , head , rm …   (coreutils grandchildren)
                 └─ exec  $GPGV --status-fd 3 --homedir <tmp> --keyring <kr> <sig> <data>
                       gpgv writes "[GNUPG:] GOODSIG … / VALIDSIG …" to fd 3
apt's PARENT reads the status pipe → on EOF parses GOODSIG → repo trusted.
```

**There is NO depth counter and NO re-entry guard that caps nesting** anywhere in
`alr_inproc_reexec.c` or the supervisor (`runtime_report.cpp`). Supervision is fully
recursive:

* `PTRACE_SEIZE` installs `PTRACE_O_TRACEFORK | TRACEVFORK | TRACECLONE | TRACEEXEC |
  TRACESECCOMP` atomically; the kernel auto-attaches **every** fork/clone/vfork
  descendant and inherits those options to it (runtime_report.cpp:2437). So gpgv at
  depth-3 is a traced tracee exactly like the depth-0 apt process.
* The in-process re-map gate `inproc_reexec_on` is a **process-global boolean** read
  ONCE from the supervisor's OWN environment (`getenv("ALR_REEXEC_INPROC")`,
  runtime_report.cpp:2542) — it is NOT per-trap and NOT a function of the guest envp,
  so it stays on for all descendants. (`ALR_REEXEC_INPROC` is set by the app via
  `Os.setenv` in AptInstaller.kt:228 and is NOT pushed into `guest_env`; it never had
  to survive in the guest envp.)
* Every `execve`/`execveat` from any traced tid RET_TRACEs → `PTRACE_EVENT_SECCOMP` →
  the supervisor cancels the syscall (`NT_ARM_SYSTEM_CALL=-1`) and PC-redirects to the
  resident trampoline, seeding x19=host target, x20=argv, x21=envp, x22=rootfs
  (runtime_report.cpp:3494-3705).

So the prior-wave "grandchild's execve stops being re-mapped at depth N" framing was
**imprecise** — the break was never a nesting cap. It was a *sequence of distinct
one-syscall-early failures*, each fixed in a prior wave and **all present in HEAD**:

| Rung | Symptom | Root cause | Fix (in HEAD) |
|------|---------|-----------|---------------|
| 1 | `apt-key` never runs → "Unknown error executing apt-key" | re-map bailed `sys_exit(72)` on the `#!/bin/sh` non-ELF magic | binfmt_script emulation in the worker (map interp, splice script into argv) — `a5d8b1c` |
| 2 | gpgv runs but apt never sees GOODSIG → "not signed" | no-execve re-map left the pre-dup CLOEXEC status write end open → pipe never EOFs | `close_cloexec_fds()` sweep before the jump — `8ba1573` |
| 3 | gpgv NEVER spawns (grandchild dies before its execve) | `setgid(getgid())`/`setuid(getuid())` guard in the method fork → base seccomp `RET_TRAP`s the cred family → SIGSYS killed it ONE SYSCALL before exec | supervisor emulates setuid/setgid family (nr 143–159) as SUCCESS(0) — `9e3ddaf` |
| 4 | gpgv runs but `NODATA` → "not signed" | `apt-key` realpath's its guest temp gpghome; interposer returned the HOST spelling → `--homedir`/`--keyring` path-split | `guest_canon()` strips the rootfs prefix off realpath results — `833866e` |

All four are ancestors of HEAD (`git merge-base --is-ancestor … HEAD` = YES for each).
Memory (Wave-4) records the device state after rung 3: *"apt gpgv SPAWNED (worker
target=…/gpgv) = cred-drop proven"*. Rung 4 then closed the keyring path-split.

## 2. This change (loader diagnostic + overlay docs)

* **`alr_inproc_reexec.c` — `audit_status_fd()`** (new, called right after the CLOEXEC
  sweep, before `enter_guest`). Diagnostic-ONLY (reads fd flags via `fcntl F_GETFD`;
  closes nothing). It emits, on the diag pipe, the exact fd-3 invariant the apt chain
  depends on, so a device drain can PROVE GOODSIG-via-fd-3 survives **each** re-map and
  localize any future EOF regression to the precise depth:
  - `ALR-INPROC: status-fd3=OPEN-keep`  → fd 3 open & NOT cloexec (the wanted live
    gpgv status write end). `=CLOSED` → no write end (gpgv `--status-fd 3` would
    EBADF). `=OPEN-cloexec(LEAK)` → a sweep miss.
  - `ALR-INPROC: inheritable_fds>=3=0x1` → count of OPEN non-CLOEXEC fds ≥ 3. The
    invariant wants **exactly 1** (fd 3 alone). `>1` = a stray inheritable fd lingers
    beside fd 3 → the status pipe never EOFs → "not signed".
* Host model + tests: `tools/elf_remap_model.py:inheritable_status_fds_after_remap`
  mirrors the survivor count; `tests/test_elf_remap_model.py` asserts the healthy=1,
  leaky=2, missing-fd3=0 cases AND the source-invariant that the audit runs
  after-sweep / before-jump.
* `tools/build_apt_mirror_overlay.py`: corrected the now-stale "authenticated is BROKEN
  ON DEVICE — loader can't load a `#!` interpreter" framing (the four loader rungs are
  in HEAD). Default stays **demo-trust** as the conservative posture until a device
  `gpgv` GOODSIG is observed — an honest gate, not a known loader defect. No behavior
  change to the emitted overlay; the `--authenticated` opt-in was already fully wired.

Loader-side code in `runtime_report.cpp` and `libalr_interpose.c` was **read and left
unmodified** — the supervisor exec-trace/redirect + the realpath guest_canon are
already correct for the deep grandchild; no change was warranted (don't regress them).

## 3. Build + host verification (this change)

* 4-ABI compile of `alr_inproc_reexec.c` with the strict build flags
  (`-Wall -Wextra -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2 -O2`,
  NDK 27.2 clang): aarch64 / armv7a / x86_64 / i686 → **all OK**.
  `alr_inproc_reexec_trampoline` exported (T); `audit_status_fd` file-local (t).
* `runtime_report.cpp` arm64 `-fsyntax-only` → clean.
* interpose `libalr_interpose.so` via `scripts/build-interpose.sh` (zig) → OK.
* `tools/build_apt_mirror_overlay.py --selftest` → ALL PASS (incl. authenticated-mode
  shape: Signed-By keyring staged, NO AllowUnauthenticated, keyring sha256 + the
  `keyring_from_rootfs(tiny-rootfs.tar)` byte-match).
* full host pytest: **2007 passed, 7 skipped**.

## 4. DEVICE-VERIFY CHECKLIST (authenticated apt-get) — for the device-owning agent

Prereqs staged into the rootfs:
1. `tools/build_apt_mirror_overlay.py --authenticated --out /tmp/apt-mirror-stage.tar`
   (+ `--mirror ports.ubuntu.com --scheme http`, `--rootfs-abs <device rootfs path>`).
   → stages `Signed-By: …/ubuntu-archive-keyring.gpg`, pins `Dir::Bin::apt-key` +
   `Apt::Key::gpgvcommand`, NO `AllowUnauthenticated`.
2. `tools/build_apt_dpkg_overlay.py --self-contained` → stages `apt-key`,
   `methods/gpgv`, `gpgv`, and the coreutils apt-key shells out to.
3. App sets `ALR_REEXEC_INPROC=1` and `ALR_FAKEROOT=1` for the apt run (AptInstaller.kt
   already does).

Run `apt-get update` (authenticated). In `logcat -s alr_loader alr_cr_diag`, expect the
**re-map chain to appear with the fd-3 audit at each rung**:

- [ ] `ALR-INPROC: worker target=…/usr/lib/apt/methods/gpgv`  (method re-mapped)
- [ ] `ALR-INPROC: shebang interp=/bin/sh` … `worker target=…/bin/dash`
      (apt-key `#!` re-mapped — rung 1)
- [ ] `ALR-INPROC: worker target=…/usr/bin/gpgv`  (gpgv SPAWNED — rung 3 proven)
- [ ] at the **gpgv** re-map: `ALR-INPROC: status-fd3=OPEN-keep`
      **and** `ALR-INPROC: inheritable_fds>=3=0x1`
      (fd 3 is the live, sole status write end — the load-bearing fd-3 invariant)
- [ ] supervisor: `alr CR4 blocked-syscall nr=144 -> OK(0)` (and 146) — cred-drop fired
- [ ] gpgv emits `[GNUPG:] GOODSIG … 871920D1991BC93C` / `VALIDSIG …F6EC…C93C` on fd 3
- [ ] apt does **NOT** print `E: The repository … is not signed` and `apt-get update`
      exits 0
- [ ] then `apt-get install <a signed pkg>` → downloads from the pinned mirror,
      unpacks + configures, exits 0 (the authenticated install)

Failure localization via the new audit:
- `status-fd3=CLOSED` at the gpgv rung → fd 3 was dropped upstream (regression in the
  CLOEXEC sweep or apt's dup2 path).
- `inheritable_fds>=3=0x2`+ → a stray inheritable fd leaked into gpgv (extra write end);
  the pipe won't EOF → "not signed". The depth at which the count first exceeds 1 names
  the offending re-map.
- gpgv re-map line never appears → a rung-1/3 regression (apt-key `#!` or cred-drop);
  cross-check the `shebang interp` line and the `nr=144 -> OK(0)` supervisor line.
