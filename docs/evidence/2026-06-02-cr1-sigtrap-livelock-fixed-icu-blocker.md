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

## ICU blocker RESOLVED (v158) — /proc/self/exe → guest path
icudtl.dat IS staged + valid (10.8MB, run-as confirmed), so the invalid FD was NOT
file-not-found: chromium opens icudtl.dat relative to its executable's directory, which it
derives from `readlink("/proc/self/exe")`. Under in-process ALR that returns the Android
APK path, so chromium computed the wrong module dir → open failed → `g_icudtl_pf = -1` →
the CHECK. Fix (general, benefits every glibc app that locates assets via the exe path):
- runtime_report.cpp: loader injects `ALR_GUEST_EXE=<guest argv[0]>` into the guest env.
- libalr_interpose.c: `readlink`/`readlinkat`("/proc/self/exe") returns `$ALR_GUEST_EXE`
  (the guest-visible binary path) instead of the kernel's APK path. This implements the
  long-standing `resolve_interposed_access` self-exe design that was only a host probe.
Deployed via interpose-stage.tar (re-extract verified: `extracted=8`).

**Device result (v158):** chromium `--dump-dom` no longer hits the ICU error — it runs
PAST ICU init. No regression (same drain): GPU LIVE/VK RENDER/VK DRAW PASS, gtk3 gtk_init
ok backend=wayland exit=0, foot exit=0.

## NEW post-ICU wedge (next CR-1 step)
Past ICU, chromium `--dump-dom` now runs the full 200s (watchdog SIGKILL, stdout=0) with
the supervisor near-idle: `ev_total=1` (one SIGCHLD on a worker), leader `state=t syscall=-1`
stopped, supervisor `state=S wchan=do_wait`, guard_fires=0. So chromium reaches post-ICU
init and blocks on something OUTSIDE the supervisor's view (a futex / a service / a thread
that never starts) rather than trapping. This is the next layer to diagnose (chromium init
sequencing under --single-process --no-zygote), distinct from the now-fixed ICU/supervisor
layers.

## Status
- Supervisor (the CR blocker, task #53): **RESOLVED** — no deadlock/livelock/path wedge.
- chromium ICU data loading (task #57): **RESOLVED** — /proc/self/exe → guest path.
- chromium `--dump-dom` render (CR-1): **not yet** — now gated on the post-ICU init wedge,
  a chromium-internals issue. Two real blockers cleared this session (supervisor + ICU).
