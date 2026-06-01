# Device Evidence — multithread ptrace supervisor hardening (v123): GIMP/--version no-regression; chromium --dump-dom deadlock NOT yet fixed

Hardening of the multi-tracee ptrace supervisor toward the Chromium render deadlock. The hardening is
GIMP/`--version` no-regression-safe, but it does NOT fully fix the non-deterministic multithread
deadlock that blocks `chromium --dump-dom` (the deeper PTRACE_SEIZE fix is backlogged). Device SM-X236N,
APK `0.4.123-mt-ptrace-harden-v123`.

## What changed (runtime_report.cpp `build_native_loader_probe` supervisor loop)
- **Default branch group-stop signal suppress**: `SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU` are now resumed with
  signal 0 instead of being forwarded (forwarding a group-stop signal re-stops the whole thread group →
  a thread parks in state `t`). The real new behavior is suppressing `SIGTSTP/TTIN/TTOU` (SIGSTOP was
  already caught earlier).
- **New-tid SETOPTIONS**: in the SIGSTOP/SIGTRAP branch, cloned tids (`w != pid`) now also get
  PTRACE_SETOPTIONS (idempotent; guarantees TRACECLONE/TRACESECCOMP per thread even if inheritance raced).
- **ESRCH-safe PTRACE_CONT**: a CONT on an already-gone tid no longer wedges the loop.
Single-thread (GIMP/--version) paths are byte-identical (those guests never produce cloned tids nor
raise SIGTSTP/TTIN/TTOU, and the leader's attach SIGSTOP was already CONT'd with 0).

## Device-verified
- **GIMP path-mediation NO REGRESSION**: gimp-probes match baseline exactly — `/bin/dynhello` exit=0
  (alr-dyn-ok), `/usr/bin/env` exit=0, `/usr/bin/id` exit=0, `/bin/dash` exit=0 (x2),
  `/bin/alr-png-test` exit=0, all `traps=0 rewrites=0`.
- **chromium --version PASS**: `ALR NATIVE LOADER GUEST EXEC: PASS`, `guest stdout=Chromium 147.0.7727.137`.

## Honest scope — deadlock NOT fixed
`chromium --dump-dom` (V8 + ~22 worker threads) still hits a NON-DETERMINISTIC deadlock: measured the
chromium child at `state t`, `utime` frozen at ~4, `Threads=1` across multiple samples — it parks at
boot (before/at first thread creation) and the supervisor never resumes it. This reproduces on the
rolled-back v121 supervisor too, so it is NOT the fast-path nor (fully) the group-stop signal handling.
The root cause is the classic-TRACEME multi-thread ptrace race where a new clone tid's initial stop and
group-stops are indistinguishable from real signal-stops. **The definitive fix is converting the attach
flow from PTRACE_TRACEME to PTRACE_SEIZE + PTRACE_EVENT_STOP/PTRACE_LISTEN** (which makes group-stops
unambiguous) — a larger, higher-risk attach-flow change (touches every guest incl. the proven GIMP
path), backlogged for an isolated implementation. chromium EXECUTION (--version) remains the secured
Goal-2 milestone; render is the open frontier gated on this multithread-supervisor work.

277 host tests pass; all 4 ABIs build.
