# Device Evidence — round-10 step 1 (v141): in-process re-map MECHANISM PROVEN — exec-re-entry with NO kernel execve

Build `0.4.141-r10-v141`. Device SM-X236N / Mali-G615 MC2 / Android 16 / untrusted_app, targetSdk 35. Cold-start drain#19. Tests ADR-003-v3 step 1: at a guest execve seccomp-trap, **cancel the execve syscall and PC-redirect the tracee into resident loader code** — no kernel execve (the W^X-forbidden primitive R9 proved dead).

## ★ Mechanism proven ✓
```
ALR-REEXEC: inproc trampoline reached (no execve)
alr exec reentry=off spliced=0 inproc=on inproc_redirected=1
alr native loader child exit=123 signal=0
```
At the `/bin/sh` execve trap the supervisor:
1. set `NT_ARM_SYSTEM_CALL = -1` → the kernel **skips the execve** (never runs the W^X-blocked exec), and
2. set `regs[32]` (pc) = `&alr_inproc_reexec_probe` → on resume the tracee jumped straight into a **resident** raw-syscall routine.

`&alr_inproc_reexec_probe` is identical in supervisor and tracee because the loader's `.text` is inherited via fork and never unmapped — so the redirect lands on real, mapped, executable guest code **with zero execve**. The probe ran in-guest (`ALR-REEXEC: inproc trampoline reached`), and the guest exited with the probe's own code (`child exit=123`). `inproc_redirected=1` counts the one redirect (the GUI/GPU guests don't execve, so they stay 0).

This is the keystone: **exec-re-entry without any kernel execve is viable on non-root untrusted_app.** It sidesteps the W^X `app_data_file:execute` wall that killed Option S (R9) — we never ask the kernel to exec a file; we redirect the PC into already-mapped code and (step 2) re-map the new ELF in-process via `mmap(PROT_EXEC)` (file-backed, which W^X *allows*).

## No regression ✓
`ALR GPU LIVE INTEGRATION: PASS`, `ALR VK RENDER MARSHAL: PASS`, `foot-result: rendered=true`, `netsurf-result: rendered=true`, `glmark2 Score 1050`. No crash. (For this experiment `ALR_REEXEC_INPROC` was default-on; since the probe body only exits 123 it would break an exec'ing guest, so it is now gated default-OFF — `ALR_REEXEC_INPROC=1` to opt in — until step 2.)

## Next — step 2: real in-process re-map
Replace the probe body with the actual loader: read target/argv/envp (supervisor passes them via callee-saved regs + a scratch host-path), `mmap` the guest ld.so + target ELF `PROT_EXEC`, build a fresh SysV stack (argv/envp/auxv), and jump to ld.so entry — inheriting the seccomp filter + SEIZE, no execve. Reuses the `alr_reentry.c` freestanding mapper core. Then `dash -c 'echo OK'` runs as a re-entered guest, and the dpkg/apt/GIMP-plugin/chromium-multiprocess chains unlock.
