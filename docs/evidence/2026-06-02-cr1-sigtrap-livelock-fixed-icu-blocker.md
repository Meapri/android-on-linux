# CR-1 BREAKTHROUGH: chromium supervisor livelock FIXED — blocker moved into chromium (ICU)

Date: 2026-06-02 · Device SM-X236N / Mali-G615 / Android 16 / untrusted_app
Loader: app/src/main/cpp/runtime_report.cpp (build_native_loader_probe ptrace supervisor)
Builds: v154 (§2.1 INTERRUPT guard) · v155 (event ring) · v156/v157 (SIGTRAP-forward)

## TL;DR
chromium `--dump-dom` was NOT a deadlock and NOT a path-mediation bug — it was a
**SIGTRAP livelock**. Forwarding genuine `brk` traps to the guest broke it. chromium now
executes real code (19 path rewrites) and fails in ~1s at a CONCRETE, fixable point:
**ICU data loading** (`base/i18n/icu_util.cc:232 Invalid file descriptor to ICU data
received`). **The in-process ptrace supervisor is no longer the blocker.** No regression:
GPU LIVE/VK RENDER/VK DRAW PASS, glmark2 1051, gtk3/foot/glmark2-es2 all run.

## How the ring buffer cracked it (v155)
The v155 watchdog dump added a recent-event ring + the supervisor's own /proc state. For
the chromium leader it showed:
```
SUPERVISOR tid=… state=S wchan=do_wait syscall=260   (idle in wait4 — NOT blocked in pread)
guard_fires=0                                        (the group-stop guard correctly never fired)
ev_ring: 573:e0/s5 ×47 …                             (event=0, stopsig=5=SIGTRAP, 4434 total)
```
So the supervisor was NOT deadlocked and NOT in a group-stop — it was busily servicing a
**SIGTRAP storm**: the chromium leader hit a `brk`, the supervisor suppressed the SIGTRAP
(resume signal 0) WITHOUT advancing the PC, so the guest re-executed the same `brk` →
SIGTRAP again, forever (4434×).

## Root cause + fix
runtime_report.cpp signal-delivery-stop handling suppressed BOTH SIGSTOP and SIGTRAP
(`PTRACE_CONT(w, 0)`). Suppressing a genuine instruction trap (`brk`, si_code=TRAP_BRKPT)
leaves the PC on the faulting instruction → infinite re-fire. Fix: split the branch —
- **SIGTRAP**: `PTRACE_GETSIGINFO`; if `si_code > 0` (kernel-generated trap: TRAP_BRKPT
  etc.) FORWARD it (`PTRACE_CONT(w, SIGTRAP)`) so the guest's own handler runs or the
  default action fires deterministically. si_code≤0 (user/queue-sent) keeps the suppress.
- **SIGSTOP**: still suppressed (forwarding would re-stop the thread group → deadlock).

Also shipped this round (the prior §2.1/§2.2 hardening, all no-regression-verified):
- §2.1 all-parked guard: before each `waitpid`, if every live tracee is LISTEN-parked,
  `PTRACE_INTERRUPT` them (forces a re-report from any stop, unlike SIGCONT). known_tids/
  listening_tids now erased on tracee exit so "live set" is accurate.
- Removed the racy v152 SIGCONT broadcast (superseded by INTERRUPT; SIGCONT only clears
  job-control stops, which is why it failed for dpkg-query).
- Stall watchdog now also dumps the supervisor's own tracer thread + the event ring.

## Device evidence (v157)
```
alr SIGTRAP tid=4833 si_code=1 pc=0x77b1a9aee8 fwd=5 (was livelock)   ← brk forwarded
chromium-headless-shell exit=-1 sig=5  traps=19 rewrites=19  exec_ms=1016   ← crashes in 1s, not 200s
guest stdout=[0602/…:ERROR:base/i18n/icu_util.cc:232] Invalid file descriptor to ICU data received.
e0/s5 SIGTRAP storm count = 0                                          ← livelock gone
```
Same drain, no regression: `ALR GPU LIVE INTEGRATION: PASS`, `VK RENDER MARSHAL: PASS`,
`VK DRAW MARSHAL: PASS`, `glmark2 Score: 1051`, gtk3 `gtk_init ok backend=wayland` exit=0,
foot exit=0, glmark2-es2 exit=0, gtk3-widget-factory sig=14 (its own 25s alarm).

## NEW CR-1 blocker (concrete, next iteration)
`base/i18n/icu_util.cc:232 Invalid file descriptor to ICU data received` — chromium's ICU
init expects a valid FD for `icudtl.dat` and got an invalid one. chromium-headless-shell
uses the FD-based ICU path; under `--single-process --no-zygote` no launcher passes the
ICU FD. Candidate fixes to try next:
1. Ensure `icudtl.dat` exists + is readable at chromium's expected path in the rootfs and
   that chromium falls back to opening the FILE (not the passed FD).
2. The interposer/loader may need to let chromium's own `open(icudtl.dat)` succeed (path
   mediation) so `InitializeICU` takes the file path, not the (absent) zygote FD.
3. A chromium flag / data-file layout so headless-shell embeds or self-opens ICU.

## Status
- Supervisor (the CR blocker, task #53): **RESOLVED** — chromium runs in-process under the
  supervisor; no deadlock, no livelock, no path-mediation wedge. The supervisor correctly
  drives chromium to its own ICU CHECK.
- chromium `--dump-dom` render (CR-1): **not yet** — now gated on chromium ICU data loading,
  a chromium-environment issue, NOT a loader/supervisor issue.
