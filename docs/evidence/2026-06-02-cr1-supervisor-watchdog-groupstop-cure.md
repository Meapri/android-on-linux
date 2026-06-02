# CR-1 chromium supervisor: stall-watchdog + group-stop SIGCONT cure (v151/v152)

Date: 2026-06-02 · Device: SM-X236N / Mali-G615 / Android 16 / untrusted_app
Loader: app/src/main/cpp/runtime_report.cpp (build_native_loader_probe ptrace supervisor)

## What this run established (device-measured)

The chromium `--dump-dom` render had been hanging the in-process ptrace supervisor
**silently and forever** — `waitpid(-1,__WALL)` never reaped, the child's 180s
`alarm()` could not fire (a ptrace-stopped / LISTEN-parked thread cannot service
SIGALRM), so even a 600s drain captured **no verdict**. Two changes turned that
silent forever-hang into a bounded, *diagnosable* failure and advanced the guest
past its first wedge.

### 1. Parent-side stall WATCHDOG (v151) — bounds + dumps, never regresses healthy guests

A join-able `std::thread` spawned just before the supervisor `while(true)` loop.
It sleeps a per-guest deadline (chromium 200s / gimp 1830s / else 40s, each a margin
past the child `alarm()`), polling an `std::atomic<bool> sup_done` every 1s. The
supervisor sets `sup_done=true` immediately after the loop exits (ECHILD) and
`join()`s it — so a **healthy guest finishes first and the watchdog no-ops** (cannot
kill GIMP/glmark2/--version). Only if the deadline passes with `sup_done` still false
does it fire: it dumps every `/proc/<leader>/task/<tid>/{stat(state char),wchan,syscall}`
via `__android_log_print`, then `SIGKILL`s the guest group so `waitpid` returns and the
probe reports instead of hanging.

This is the diagnostic that made the rest of this run possible.

### 2. The early wedge it revealed — leader LISTEN-parked in a no-SIGCONT group-stop

First watchdog drain (v151), chromium `--dump-dom`:

```
alr sup-stall WATCHDOG fired after 200s leader=28213 — dumping + SIGKILL
alr sup-stall tid=28213 state=t wchan=ptrace_stop syscall=-1 ...
```

ONE thread (the leader itself), `state=t` (TASK_TRACED), `wchan=ptrace_stop`,
`syscall=-1` (no in-flight syscall) — i.e. a **group-stop**. The supervisor LISTEN-parks
a group-stop (correct under PTRACE_SEIZE) and resumes it only on the group-stop END,
which the kernel emits when a **SIGCONT** arrives. A headless guest has no job control
and no SIGCONT source → the leader parks **forever**. This is EARLY / single-thread,
so it is NOT the multi-thread END re-park the §2.1 `listening_tids` fix addressed.

### 3. The cure (v152) — self-resolve the group-stop with SIGCONT

In the GROUP-STOP BEGIN branch, after `PTRACE_LISTEN`, the supervisor now sends
`kill(pid, SIGCONT)` to the whole thread group. An in-process guest has no
job-control intent, so we end the group-stop ourselves: each parked tid re-reports
its group-stop END (GETSIGINFO EINVAL) and the §2.1 branch CONT-resumes it.
Idempotent (SIGCONT to a running group is a no-op); the watchdog bounds any
pathological re-stop loop.

**Result (v152 drain):** chromium `--dump-dom` (leader 29367) gets **past** the early
group-stop and now stalls **deeper**:

```
alr sup-stall WATCHDOG fired after 200s leader=29367 — dumping + SIGKILL
alr sup-stall tid=29367 state=t wchan=ptrace_stop syscall=56 0xffffffffffffff9c 0x... 0x80000 ...
```

`syscall=56` on arm64 = **openat**, args `AT_FDCWD(-100)`, path ptr, `O_CLOEXEC(0x80000)`.
The leader is now ptrace-stopped at an **openat path-mediation seccomp-trap** that the
supervisor is not resuming — during the ~20-thread clone-storm phase. chromium
`--version` (few threads) still runs fine in the same drain (`exit=0`,
`Chromium 147.0.7727.137`), as do dpkg-query 1.22.6 / apt 2.8.3.

## Residual blocker — ROOT-CAUSED (v153 supervisor self-dump)

The v153 drain dumps the supervisor's OWN tracer thread alongside the guest. It is
**decisive**:

```
alr sup-stall SUPERVISOR tid=30548 state=S wchan=do_wait syscall=260   (wait4)
alr sup-stall guest      tid=30656 state=t wchan=ptrace_stop syscall=56 (openat)   [40s guest]
alr sup-stall guest      tid=30658 state=t wchan=ptrace_stop syscall=-1            [chromium 200s]
```

- **Candidate c (supervisor blocked in pread/process_vm_readv) — REFUTED.** The
  supervisor sits in `wait4`/`do_wait` (`state=S syscall=260`), i.e. idle in
  `waitpid(-1,__WALL)` — NOT blocked on any `/proc/<tid>/mem`.
- The seccomp path-mediation handler ALWAYS resumes (runtime_report.cpp:2944
  `PTRACE_CONT` + `continue`, on every failure path it merely skips the rewrite and
  falls through to that CONT) — so this is NOT a missing-CONT in path mediation.
- Therefore the guest is **LISTEN-parked**: `state=t` ptrace-stopped is *non-waitable*
  only when `PTRACE_LISTEN`-ed, which is exactly why `waitpid` blocks forever. A
  group-stop got LISTEN-parked and **never un-parked**, even with the SIGCONT cure —
  for BOTH a single-thread guest (dpkg-query, stopped mid-openat) and chromium
  (`syscall=-1`, a pure stop). So the wedge is in the **group-stop / LISTEN resume**,
  not in path mediation and not in a blocking supervisor read.

### Why the SIGCONT cure is incomplete
`kill(pid, SIGCONT)` on each group-stop BEGIN ends the group-stop for the WHOLE thread
group at once, but per-thread BEGIN events still queued in `waitpid` then get processed
as stale: a thread that already left the group-stop (via the broadcast SIGCONT) is
`PTRACE_LISTEN`-ed again from its stale BEGIN, re-parking it with no further SIGCONT to
release it. (And a single-thread guest can re-group-stop after the SIGCONT.) The cure
helps chromium past the FIRST group-stop but does not robustly drain a multi-thread
group-stop storm.

### Next step (from the committed v153 baseline)
Rework the group-stop resume so it cannot leave a tracee LISTEN-parked: either resume
group-stops directly with `PTRACE_CONT(w, 0)` (signal 0 suppresses the job-control stop;
an in-process guest has no job-control intent — this is the transparent "keep running"
policy) instead of LISTEN+broadcast-SIGCONT, or gate the LISTEN so a tid that already
left the stop is never re-LISTEN-ed from a stale BEGIN. Verify against BOTH the
single-thread dpkg-query repro and chromium `--dump-dom`; the stall watchdog now bounds
any regression and re-dumps the signature.

## Status
- chromium `--version`: **runs in-process** (Chromium 147.0.7727.137, exit 0). ✅
- chromium `--dump-dom` (full render = CR-1): **not yet** — root-caused to the group-stop
  LISTEN-park (NOT path mediation, NOT a blocking supervisor read).
- Watchdog + self-dump: **permanent robustness + diagnostic win** — no guest can silently
  hang the loader; the supervisor's own wedge state is now visible.
- No-regression VERIFIED (v152/v153, no `.alr-cr1`): GPU LIVE/VK RENDER/VK DRAW PASS,
  glmark2 Score 1076, gtk3/foot/netsurf/Xwayland all run; the watchdog only fires past a
  guest's alarm deadline (healthy guests finish first).

## Status
- chromium `--version`: **runs in-process** (Chromium 147.0.7727.137, exit 0). ✅
- chromium `--dump-dom` (full render = CR-1): **not yet** — advanced past the early
  group-stop (SIGCONT cure), now bounded at the multi-thread openat-trap wedge.
- Watchdog: **permanent robustness win** — no guest can silently hang the loader forever.
