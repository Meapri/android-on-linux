# Design — The multithread ptrace-supervisor wall (chromium `--dump-dom` hang)

- Status: **Design / root-cause hypothesis (host-analyzed, device-pending).** No code change in this doc — it is the analysis + the exact fix + the diagnostic spec WS-1 implements in `app/src/main/cpp/runtime_report.cpp` (which I do NOT edit; WS-1 owns the supervisor).
- Owner of the fix + instrumentation: **WS-1** (`build_native_loader_probe`). This doc: branch `auto/sd-design`, file `docs/design/multithread-supervisor-wall.md` only.
- Evidence this addresses: `docs/evidence/2026-06-02-cr1-chromium-supervisor-deadlock.md` (builds v147–v149). `chromium-headless-shell --version` (few threads) runs, traps=0. `chromium --dump-dom` (~20 threads, `--single-process --no-zygote`) **never reaps even at a 600s alarm** — the loader log goes silent the instant the probe starts and the `chromium-CR1` report never prints. This is a **genuine hang**, not slow throughput.
- This doc **supersedes the central hypothesis of `docs/design/adr-chromium-storm-deadlock.md`** (ADR-004, "window-too-short, not deadlock"). The 600s drain (24× the original 25s window) refutes the window hypothesis for the render path: a hang that survives 24× the time budget is a stuck `waitpid`, not a slow bring-up. ADR-004's §3 measurement plan (clone time-series + stall dump) is RETAINED and folded into §4 below — it is exactly the instrumentation that will CONFIRM which of the candidates here is live.

All line numbers are `app/src/main/cpp/runtime_report.cpp` at this worktree's HEAD (the supervisor is `build_native_loader_probe`, the multi-tracee `waitpid` loop opens at **L2172** `while (true)` / **L2174** `::waitpid(-1, &status, __WALL)`).

---

## 0. The supervisor at a glance (the surface under analysis)

The guest is launched by `fork()` (L1698) in `build_native_loader_probe`; the child blocks on a go-pipe read (L1715–1722) until the parent `PTRACE_SEIZE`s it with `kSeizeOpts = TRACECLONE|TRACEFORK|TRACEVFORK|TRACEEXEC|TRACESECCOMP` (L1983–1989) and writes the go byte (L1993–1997). The child then installs the seccomp filter LAST (L1922–1930) and `::alarm(alarm_sec)` (L1960–1962, **chromium=180s**) before `alr_enter_guest`. **The alarm is armed in the CHILD; the parent supervisor has NO alarm and NO signal handler.**

The single parent thread then loops `waitpid(-1, __WALL)` (L2174) and dispatches by `event = status >> 16` (L2199):

| handler | lines | resume |
|---|---|---|
| `WIFEXITED` / `WIFSIGNALED` | L2181–2193 | evict mem fd; if `w==pid` record code/sig; `continue` |
| `PTRACE_EVENT_SECCOMP` (path/exec rewrite) | L2200–2855 | `PTRACE_CONT(w,0)` L2854 |
| `PTRACE_EVENT_EXEC` | L2857–2878 | evict mem fd(s); `PTRACE_CONT(w,0)` L2877 |
| `PTRACE_EVENT_STOP` (==128) | L2880–2947 | **new tid → CONT (L2914); group-stop → LISTEN (L2929); else → CONT (L2943)** |
| other event 1..6 (CLONE/FORK/VFORK) | L2948–2959 | `++guest_threads` if CLONE; `PTRACE_CONT(w,0)` L2957 |
| `SIGSTOP`/`SIGTRAP` signal-stop | L2960–2981 | suppress, `PTRACE_CONT(w,0)` L2977 |
| `SIGSYS` (seccomp emulate) | L2982–3024 | `-ENOSYS` regset, `PTRACE_CONT(w,0)` L3008 |
| `SIGSEGV`/`SIGBUS`/`SIGILL` capture | L3025–3040 | falls through to default deliver |
| default signal-delivery-stop | L3041–3061 | deliver signal except group-stop signals (L3052–3055), `PTRACE_CONT` L3058 |

The group-stop classification is the device-noted "THE MULTI-THREAD FIX (PTRACE_SEIZE only)" block (comment at **L2881–2908**), using `known_tids.insert(w).second` (L2909) to split a freshly-cloned tid's initial stop from a real group-stop, then `PTRACE_GETSIGINFO`==EINVAL → `group_stop` → `PTRACE_LISTEN` (L2923–2932).

---

## 1. Root-cause hypotheses, each evaluated against the code

### (a) — PRIMARY — a group-stopped thread is `PTRACE_LISTEN`-ed and never receives its `SIGCONT`, so `waitpid` never returns it AND the SIGALRM that should bound the run can never run on it. **VULNERABLE. This is the most likely cause.**

The fix block's own comment (L2895–2898) states the LISTEN contract: a group-stopped thread is parked with `PTRACE_LISTEN`, "is NOT run, and re-reports when the group-stop ends (SIGCONT)." That contract has a hole for the chromium render path:

1. **What puts the group into a group-stop at all?** chromium does not use job-control SIGSTOP in normal operation, BUT the supervisor's own design routes the **180s SIGALRM into the guest** (alarm armed in the child, L1960). When the guest's wall-deadline hits, `SIGALRM` (default action: terminate) is delivered to the thread group. That is the intended kill path. The problem is the *ordering*: SIGALRM is a **process-directed** signal; the kernel picks one not-blocked thread to run the default-terminate. **A thread sitting in `PTRACE_LISTEN` (ptrace-stopped) cannot be chosen to take a process-directed fatal signal until it leaves the stop.** If the *only* threads not LISTEN-parked are themselves blocked in an uninterruptible/futex wait that depends on a LISTEN-parked sibling, the SIGALRM has no runnable target and the group never dies → `waitpid` never returns → exactly the observed "silent past 600s, never reaped."

2. **Why does a thread get LISTEN-parked and never released?** `PTRACE_LISTEN` re-reports only when the group-stop *ends* — i.e. when a `SIGCONT` is delivered to the group. In chromium's `--single-process` model there is **no SIGCONT generator**: nothing in the guest sends SIGCONT, and the supervisor never sends one. So once `group_stop` is taken (L2925) and `PTRACE_LISTEN` (L2929) parks a tid, **the only event that can un-park it is a SIGCONT that will never come.** The comment "re-reports when the group-stop ends (SIGCONT)" silently assumes an external SIGCONT that does not exist for a self-contained headless guest. The parked tid is invisible to `waitpid` forever.

3. **What generates the group-stop in the first place during render (before the alarm)?** This is the live question, and the most plausible trigger is a **misclassification race**, which is candidate (b) below feeding (a): a freshly-cloned tid whose `PTRACE_EVENT_STOP` arrives and is *correctly* CONT-ed by L2909–2917, but a *subsequent* `PTRACE_EVENT_STOP` for a tid that the kernel reports as a group-stop because **a clone or a fatal-signal-in-flight put the whole group into group-stop state**. `PTRACE_GETSIGINFO` returns EINVAL for that (L2923–2924) → LISTEN (L2929). If the group-stop was actually transient (e.g. the kernel's group-stop bookkeeping during a clone storm) the LISTEN can park a tid that no SIGCONT will ever wake.

**Verdict on (a):** The LISTEN path (L2925–2932) is correct *only if* a SIGCONT always follows. For a headless self-contained chromium guest with no job control, **there is no SIGCONT source**, so any tid that reaches the `group_stop==true` branch is parked permanently. Combined with the child-side SIGALRM (which a LISTEN-parked thread cannot service), this is a self-consistent permanent hang that survives any alarm value — matching the 600s evidence exactly. **This is the primary suspect.**

### (b) — clone-event race: a new thread auto-attaches (PTRACE_EVENT_CLONE) and its initial `PTRACE_EVENT_STOP` is misclassified as a group-stop. **PARTIALLY GUARDED, but a residual race feeds (a).**

The code explicitly guards the *common* form of this: `known_tids.insert(w).second` (L2909) treats any never-before-seen tid's `PTRACE_EVENT_STOP` as a fresh-clone initial stop and CONTs it (L2914), with the comment at L2904–2908 calling out that classifying purely by siginfo "would wrongly LISTEN-park a new worker thread at birth — the Chromium 1-thread hang." So the **first** stop of each tid is safe.

The residual race is on the **ordering of two events for the same new tid**:
- `PTRACE_EVENT_CLONE` is delivered to the **parent** tid (L2954, `++guest_threads`, generic CONT L2957), reporting that a child was created.
- `PTRACE_EVENT_STOP` (the new child's initial stop) is delivered for the **child** tid.

The kernel does not guarantee these arrive in a fixed order relative to each other or to a group-stop the parent may enter. With ~20 threads cloning in a storm, it is possible for the supervisor to observe a `PTRACE_EVENT_STOP` for a tid that **was already inserted into `known_tids` by an earlier stop**, where this *second* stop is a genuine group-stop (GETSIGINFO==EINVAL, L2924) → LISTEN (L2929). That is the doorway into the permanent-park of (a). So (b) is not independently fatal (the first-stop guard holds), but it is the most plausible **producer** of the group-stop that (a) then fails to release.

**Verdict on (b):** the first-stop misclassification is correctly guarded (L2909–2918). The residual is the *second-and-later* group-stop on a known tid during a clone storm, which is real and routes into (a).

### (c) — a blocking `pread` on `/proc/<tid>/mem` for a tid that is mid-exec/dead, hanging the single supervisor thread. **NOT the cause of THIS hang (chromium `--single-process` does not exec), but a latent single-thread-serialization hazard.**

The mem-fd cache (`mem_fd_for`, L2131–2144) opens `/proc/<tid>/mem` `O_RDWR` and `pread`/`pwrite`s it at L2229 / L2286. `pread` on `/proc/<tid>/mem` of a *ptrace-stopped* tracee does **not** block — the tracee is frozen at the seccomp-entry stop, so the read returns immediately or fails (ESRCH/EIO/EBADF, handled by the evict+reopen at L2235–2242 / L2293–2300). It would only block if the target tid were *running* and the kernel serialized the access, but at an `EVENT_SECCOMP` stop the trapping tid `w` is stopped by definition, and the code only `pread`s the trapping tid's own mem. So for the path-rewrite hot loop there is no blocking read.

The genuine hazard is **serialization, not blocking**: with ONE supervisor thread, every per-trap `pread`+`translate_rootfs_path`+`pwrite` (L2229–2303) is on the critical path for ALL ~20 threads. If chromium's path-syscall rate is high this throttles throughput — but the 600s evidence rules out "merely slow" (a throttle would still eventually reap). `--single-process` chromium never execs a child, so the EXEC/former-tid mem-fd churn (L2857–2878) does not fire here either.

**Verdict on (c):** not the active hang for `--single-process --no-zygote` chromium (no exec; stopped-tid preads don't block). Keep as a watch-item; it is the throughput ceiling, not the deadlock.

### (d) — a futex held by a tracee that is itself ptrace-stopped (LISTEN-ed) while a sibling spins → classic ptrace-induced futex deadlock. **VULNERABLE — this is the mechanism that makes (a) lethal rather than merely slow.**

chromium's threads coordinate heavily through futexes (the message loop, the V8 heap, the thread pool). futex(98) is **not** a traced syscall — the PCGATE BPF (`alr_interpose/libalr_interpose.c`) RET_TRACEs only the 9 path nrs, and the execve-trace filter (L1244–1267, installed L1926) RET_TRACEs only execve/execveat. So futex runs un-traced and un-emulated; the supervisor never sees it.

The deadlock is therefore **ptrace-induced**: if thread T holds a futex (or is the only thread that can release one) and T is `PTRACE_LISTEN`-parked by (a), then sibling S that blocks in `futex(FUTEX_WAIT)` on that word **never wakes** — and S's wait is a kernel sleep the supervisor also never sees (no trap). The whole group quiesces: T parked-in-ptrace, S asleep-in-futex, no event ever reaches `waitpid(-1)`. This is a textbook "tracer parked a thread mid-critical-section" deadlock. It is *consequent* on (a): (a) supplies the wrongly-parked T; (d) is why parking T wedges the *whole* group instead of just losing one thread.

**Verdict on (d):** vulnerable, and it is the amplifier — it explains why a single mis-parked thread silences the *entire* guest (no further events at all), which is exactly the "loader goes completely silent" signature.

### (e) — the leader exiting while threads remain (group-leader vs thread-group reaping). **NOT the cause, but verify with the diagnostic.**

The loop keys completion on `waitpid` returning ECHILD (L2179, "every tracee has been reaped") and records the exit code only for `w==pid` (L2183–2185 / L2190–2192). With `__WALL` (L2174) the supervisor reaps all threads and the leader regardless of leader-vs-non-leader status, and `PTRACE_O_TRACEEXIT` is deliberately NOT set (comment L2114–2116), so reaping is the death signal. If chromium's leader thread exited but workers remained, the loop would keep draining workers (correct) and only break at ECHILD. The observed signature is the **opposite** — nothing exits at all (no events) — so (e) does not match. A leader-exit-with-stuck-workers would still show `WIFEXITED(w==pid)` and then a drain; the evidence shows neither.

**Verdict on (e):** not the active cause. The diagnostic in §4 will confirm by showing the live-tracee count never decrements.

### Summary ranking
1. **(a) permanent LISTEN-park with no SIGCONT source** — primary root cause.
2. **(d) futex deadlock around the parked thread** — the amplifier that makes (a) silence the whole group.
3. **(b) clone-storm group-stop on a known tid** — the most plausible producer of the group-stop that (a) mishandles.
4. (c) mem-fd serialization — throughput ceiling, not this hang.
5. (e) leader-reaping — ruled out by the signature.

The three live ones compose into ONE failure: a clone-storm-induced group-stop (b) is LISTEN-parked (a) with no SIGCONT to release it, and a sibling blocks forever on that thread's futex (d) → total silence, no reap, survives any alarm.

---

## 2. The exact fix

### Primary fix — never leave a group-stopped tracee parked without a release path

The bug is that `PTRACE_LISTEN` (L2929) assumes an external SIGCONT will end the group-stop, which never arrives for a self-contained headless guest. Two concrete, mutually-reinforcing changes, both at the `group_stop==true` branch (L2925–2932):

**Fix 2.1 (minimal, highest-confidence): re-arm a re-report so a LISTEN-parked tid is never invisible.** After `PTRACE_LISTEN(w)` succeeds, the tracee is stopped+listening; a subsequent `PTRACE_INTERRUPT(w)` forces it to re-report as a `PTRACE_EVENT_STOP` *even while the group-stop persists*, which lets the supervisor observe it and decide again instead of losing it forever. The supervisor should track the set of LISTEN-parked tids and, when the live-tracee count would otherwise stall (no other runnable tracee), `PTRACE_INTERRUPT` the parked set to break the wedge. Concretely, replace the bare park at L2925–2932 with: LISTEN, record `w` in a `listening_tids` set; and add, just before the blocking `waitpid` (L2174), a guard: if every known live tid is in `listening_tids` (i.e. the whole group is parked → guaranteed no event will come), `PTRACE_INTERRUPT` each parked tid to force a re-report and clear it from the set. This converts "park forever" into "park, but always re-examinable," which structurally cannot deadlock.

> Note: `PTRACE_INTERRUPT` is referenced only in the *comments* today (L2888, L2920, L2934) — it is **never actually called**. Adding the real `PTRACE_INTERRUPT` calls is the core of this fix.

**Fix 2.2 (defense-in-depth): do not let the group-stop classification swallow the guest's own SIGALRM-driven exit.** Because the 180s alarm is armed in the child (L1960) and a LISTEN-parked thread cannot service a process-directed SIGALRM, the kill path is unreliable. **Move the wall-deadline to the PARENT supervisor** (see §4 watchdog) and have the parent `kill(pid, SIGKILL)` the whole group on deadline. SIGKILL is delivered to a ptrace-stopped/LISTEN-ed tracee unconditionally (it cannot be parked against SIGKILL), so this guarantees the guest is reaped and `waitpid` returns ECHILD even if a thread is wrongly parked — turning a permanent hang into a bounded failure the drain can read. Keep the child alarm as a backstop but make the parent SIGKILL authoritative.

### Fallback fix (if §2.1 is wrong — i.e. the group-stop is real and load-bearing, not spurious)

If the device diagnostic shows the LISTEN-parked tid is in a *legitimate* group-stop that SHOULD persist (not a clone-storm artifact), then the fix is not to interrupt it but to **stop generating group-stops in the first place** for this guest class: a headless chromium has no controlling terminal and no job-control need, so the supervisor can **swallow the originating stop signal** at its signal-delivery-stop. Strengthen the default suppress set (L3052–3055 already drops SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU with `deliver=0`) by ensuring it fires *before* the kernel commits the group to group-stop — i.e. catch the group-stop signal at the **signal-delivery-stop** for the first thread and resume with `0`, so no group-stop is ever entered and the L2925 branch is never reached. The risk (noted honestly): if chromium ever depends on a real SIGSTOP/SIGCONT pair internally this would break it, but a `--single-process --no-zygote` headless render does not.

A second fallback, if BOTH the LISTEN and the suppress paths prove load-bearing: convert the seccomp filter to also RET_TRACE `clone`/`clone3` so the supervisor sees thread creation as an `EVENT_SECCOMP` and can serialize its own bookkeeping (known_tids insert) *before* the child's initial stop can race — removing the (b) ordering window entirely. This costs a trap per clone (acceptable: clone is not a hot-loop syscall) and closes the race deterministically.

---

## 3. Is a MULTI-THREADED supervisor feasible? — honest verdict: NO, and it is not the fix.

**ptrace is tracer-thread-affine.** Per `ptrace(2)`: a tracee is traced by exactly one thread (the one that SEIZE/ATTACH-ed it or, for auto-attached children under `PTRACE_O_TRACECLONE`, the **same** tracer thread as the parent). The chromium thread-group is SEIZE-d as a unit (L1988, the leader is seized; all clones auto-attach to the **seizing thread** by kernel rule). Therefore:

- You **cannot** split chromium's ~20 threads across N tracer threads. Every clone auto-attaches to the one thread that holds the group, and only that thread may `waitpid`/`PTRACE_CONT` them. A second supervisor thread calling `waitpid(-1,__WALL)` would either steal nothing (the tracees are not its children/tracees) or get `ECHILD`. Re-seizing a sibling from another thread is not possible while the first thread holds it.
- The only way to get multiple tracer threads would be to make chromium spawn its threads as **separate ptrace attaches per thread**, which requires intercepting clone and re-seizing each child from a dedicated thread — but the child is *already* auto-attached to the seizer the instant it is created, so there is no un-attached window to hand it to another thread. This is not achievable with `PTRACE_SEIZE` + `TRACECLONE`.

**Verdict: a multi-threaded supervisor is NOT feasible for a single SEIZE-d thread-group, and is NOT the fix.** The evidence doc's own speculation ("one tracer thread per tracee thread") does not survive the tracer-affinity rule. The real fix is the **specific-stop handling** of §2 (never-permanently-park + parent-side SIGKILL deadline) plus, optionally, the clone-RET_TRACE serialization fallback. Non-blocking `/proc/mem` is a throughput improvement (§1(c)) but irrelevant to the deadlock. The single-threaded supervisor is the *correct* architecture; it just must never park a tracee with no release path.

(If breadth ever demands genuinely concurrent tracing — e.g. a *multi-process* chromium with `--no-zygote` removed — that is a separate axis: distinct PROCESSES can be SEIZE-d by distinct threads, since a fork child can be re-seized. But that is ADR-003's exec/multiprocess wall, orthogonal to this thread wall, and not needed for the `--single-process` render path.)

---

## 4. Diagnostic instrumentation WS-1 should add to CONFIRM on the next device drain

All of this is supervisor-internal (no external strace, no new tracee cost in the hot path beyond a printf on infrequent events). It must answer, in ONE drain: *which tid is stuck, in which event/state, on which syscall.* Three pieces:

### 4.1 Per-event trace line (one line per non-hot event)

Emit at the top of each event handler (NOT inside the `EVENT_SECCOMP` path-rewrite hot loop — that one only on the FIRST trap per tid, to avoid flooding). Format:

```
alr-mt evt tid=<w> event=<event> stopsig=<stopsig> gsi_rc=<rc> gsi_errno=<errno> known=<0|1> live=<n>
```

- placement: immediately after `const int event = status >> 16;` (L2199), guarded so the `EVENT_SECCOMP` case prints only when `known_tids` did not yet contain `w` (first trap), and `EVENT_STOP`/`EVENT_CLONE`/`EVENT_EXEC`/signal-stops print always.
- `gsi_rc`/`gsi_errno`: for the `EVENT_STOP` branch, log the result of the `PTRACE_GETSIGINFO` at L2923 (the EINVAL-vs-success that drives LISTEN-vs-CONT). This directly shows every LISTEN decision (the suspected mis-park).
- `live`: the current live-tracee count (see 4.2).

### 4.2 Live-tracee counter

Maintain `int live_tracees` alongside `known_tids` (L2126): `++live_tracees` when a tid is first inserted into `known_tids` (L2909 success) and when `++guest_threads` fires on `EVENT_CLONE` (L2954–2956); `--live_tracees` on `WIFEXITED`/`WIFSIGNALED` (L2181/L2188, where `mem_fd_evict` already runs). Also maintain `std::unordered_set<pid_t> listening_tids` updated at the LISTEN site (L2929, insert) and at any subsequent re-report (erase). The invariant the drain proves: **if `live_tracees > 0` AND `live_tracees == listening_tids.size()` the group is fully parked = guaranteed deadlock** (this is also the trigger for the §2.1 INTERRUPT-break).

### 4.3 Stall watchdog (the decisive instrument)

The parent currently has no timeout (L2174 `waitpid` blocks unbounded). Add a parent-side `SIGALRM` watchdog so a blocked `waitpid` past N seconds dumps the full tracee state:

1. In the parent, before the loop (around L2168), install a `sigaction(SIGALRM, ...)` whose handler sets a `volatile sig_atomic_t g_stall = 1` and arm `alarm(N)` (N ~ 30s for the diagnostic drain — long enough past normal init, short enough to fire well inside the 600s budget). Re-arm `alarm(N)` after EACH successful `waitpid` return (so it only fires when `waitpid` is genuinely stuck, not during progress).
2. Make `waitpid` interruptible: the existing `if (errno == EINTR) continue;` at L2176–2178 already handles the SIGALRM interrupting `waitpid`. On that EINTR, check `g_stall`; if set, run the dump (below) ONCE, then either re-arm and continue (read-only diagnostic mode) or `kill(pid, SIGKILL)` to bound it (deadline mode, per §2.2).
3. **The dump** — for every tid in `known_tids` (minus those already reaped), read three procfs files and print:

```
alr-mt STALL after <N>s, live=<n> listening=<m>
alr-mt stuck tid=<tid> state=<S> wchan=<wchan> syscall=<nr,args...>
   ... one line per known tid ...
```

   - `state`: char field 3 of `/proc/<tid>/stat` (`R`/`S`/`D`/`t`/`T`/`Z`). The smoking gun for (a) is a tid in **`t`** (tracing stop / group-stop-listening) that never leaves; for (d) a sibling in **`S`/`D`** parked on a futex.
   - `wchan`: `/proc/<tid>/wchan` (kernel symbol, e.g. `futex_wait_queue_me` confirms (d); `ptrace_stop` confirms (a)).
   - `syscall`: `/proc/<tid>/syscall` (first field = syscall nr the tid is blocked in; `98`=futex confirms (d), `-1`=not in a syscall / stopped).

   These three files are readable by the tracer for its own tracees with no ptrace op and no risk of perturbing state (pure reads of procfs). They give the EXACT "which thread is stuck where" the evidence doc asks for.

### 4.4 What each outcome proves (decision table for the drain)

| drain shows | conclusion | fix to ship |
|---|---|---|
| a tid `state=t` `wchan=ptrace_stop`, in `listening_tids`, `live==listening` | **(a) confirmed**: permanent LISTEN-park | §2.1 INTERRUPT-break + §2.2 parent SIGKILL deadline |
| above + a sibling `state=S` `wchan=futex_*` `syscall=98` | **(a)+(d) confirmed** | same as above (fixing (a) releases (d)) |
| the parked tid's PRIOR `alr-mt evt` line shows `event=128 gsi_errno=22(EINVAL) known=1` | confirms the L2924 group_stop branch parked a **known** tid → **(b)** is the producer | add clone-RET_TRACE serialization (§2 second fallback) |
| `live` never decrements AND no `alr-mt evt` lines after init | total quiescence = ptrace-induced deadlock (not throughput) | §2 (definitively NOT window-too-short — refutes ADR-004) |
| `live` decrements slowly, events keep flowing, eventually reaps under a big alarm | throughput ceiling **(c)**, not a deadlock | non-blocking/parallel mem access; not this doc's deadlock fix |
| `w==pid` `WIFEXITED` while other tids remain, then drains | **(e)** leader-exit (ruled-out predictor) | none — loop already correct |

The §4.3 watchdog alone converts the current "silent 600s, no information" drain into a single decisive measurement. It is the first thing WS-1 should add.

---

## 5. Honest notes / what is device-pending

- I cannot device-verify (host-only). Everything in §1–§2 is a code-grounded hypothesis; §4 is the instrument that turns it into a measured fact. The ranking (a)>(d)>(b) is an argued best-estimate, not a measurement.
- The single strongest *code* fact supporting (a): `PTRACE_LISTEN` (L2929) is the ONLY way a tracee is intentionally left stopped, its release contract (L2895–2898) depends on a SIGCONT that has no source in a headless self-contained guest, and `PTRACE_INTERRUPT` — the primitive that would make a LISTEN-parked tid re-examinable — is named in three comments (L2888/L2920/L2934) but **never actually called**. That gap is precisely where a wrongly-parked thread becomes invisible forever.
- The single strongest *evidence* fact: the 600s drain reaped nothing and logged nothing after the probe start. A throughput problem (c) would have reaped under 24× the budget; a window problem (ADR-004) would have reaped under 24× the budget. Only a stuck `waitpid` (no event will ever come) matches — i.e. (a)/(d).
- §2.2 (parent-side SIGKILL deadline) is worth shipping REGARDLESS of which hypothesis wins, because it bounds the failure and lets the §4 dump always print — it is the difference between "silent forever" and "one decisive drain."
- This does not touch `runtime_report.cpp`; WS-1 implements §2 + §4. It does not touch `MainActivity.kt` or the chromium-storm bench/test files (other owners).
```
