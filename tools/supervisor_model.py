"""Pure-Python LIVENESS model of the ALR multi-thread ptrace supervisor.

CR-1 (chromium-run blocker). This module models — as a pure-Python discrete-event
state machine — the *global scheduling / liveness* behaviour of the single-threaded
ptrace supervisor in ``app/src/main/cpp/runtime_report.cpp``
(``build_native_loader_probe``, the ``while (true) { waitpid(-1, &status, __WALL) }``
multi-tracee loop). It REPRODUCES the device-measured chromium ``--dump-dom``
deadlock (evidence: docs/evidence/2026-06-02-cr1-chromium-supervisor-deadlock.md)
as a host-verifiable failing assertion, PINPOINTS the offending transition, and
shows a candidate FIX clears it — so the device fix has a regression guard that
runs on darwin with no kernel.

WHY A SECOND MODEL (distinct from tests/mt_supervisor_model.py)
---------------------------------------------------------------
``tests/mt_supervisor_model.py`` (owned by another session) models the PER-EVENT
*branch* decision: "given one already-decoded waitpid stop, which ptrace op does
the C code pick?" and proves no single branch wrongly parks a brand-new tid. That
is necessary but NOT sufficient: it feeds one event at a time and never models
what happens to a thread AFTER it is LISTEN-parked, nor the global question
"can the supervisor's waitpid(-1) ever stop reaping?" — i.e. LIVENESS.

The chromium deadlock is a LIVENESS failure, not a single-branch misclassify:
a thread is correctly LISTEN-parked on a group-stop, but the supervisor then has
no transition that ever resumes it back to RUNNING, so a sibling blocked on that
thread's futex waits forever and the leader never exits → waitpid(-1) blocks
forever past even a 600s alarm. This module models exactly that closed loop.

THE MODELLED STATE MACHINE (faithful to runtime_report.cpp)
-----------------------------------------------------------
Per-tracee runtime state:
  RUNNING        : last resume was PTRACE_CONT; the tid is executing guest code.
  LISTENING      : parked via PTRACE_LISTEN after a group-stop (NOT running). It
                   re-reports a PTRACE_EVENT_STOP when the group-stop *ends*.
  FUTEX_WAIT     : blocked in futex(2) waiting on a sibling thread. futex is a
                   non-path nr → PCGATE RET_ALLOW → no trap, no supervisor event;
                   the supervisor cannot see or resolve this. The ONLY thing that
                   un-blocks it is the sibling running far enough to FUTEX_WAKE.
  DEAD           : reaped (WIFEXITED / WIFSIGNALED).

Supervisor event handling (mirrors the C while-loop branch order):
  - WIFEXITED/WIFSIGNALED            -> reap (close fd); leader exit ends the run.
  - EVENT_SECCOMP (path/exec nr)     -> rewrite + PTRACE_CONT (stays RUNNING).
  - EVENT_CLONE / EVENT_EXEC         -> PTRACE_CONT the parent.
  - EVENT_STOP, tid NOT yet known    -> insert + PTRACE_CONT (anti-park guard).
  - EVENT_STOP, known tid, group-stop (GETSIGINFO==EINVAL) -> PTRACE_LISTEN (park).
  - EVENT_STOP, known tid, interrupt (GETSIGINFO ok)       -> PTRACE_CONT.
  - SIGSYS(SYS_SECCOMP)              -> emulate -ENOSYS + PTRACE_CONT.

THE BUG THIS MODEL EXPOSES (the resume policy gap)
--------------------------------------------------
When a group-stop *ENDS* (SIGCONT delivered to the group), each LISTEN-parked
thread re-reports a PTRACE_EVENT_STOP. It is a KNOWN tid, so it bypasses the
anti-park guard. Under PTRACE_SEIZE the kernel still has no queued siginfo for
that stop (it is the trailing edge of the same group-stop), so PTRACE_GETSIGINFO
again returns EINVAL → the C code classifies it as a group-stop AGAIN → PTRACE_
LISTEN AGAIN. The thread is re-parked instead of resumed: its state never leaves
LISTENING. A sibling in FUTEX_WAIT on that thread therefore never gets its
FUTEX_WAKE, so it never runs, never exits; the leader never exits; waitpid(-1)
blocks forever. THAT is the chromium hang.

THE CANDIDATE FIX (what WS-1 must implement in runtime_report.cpp)
------------------------------------------------------------------
On a known tid's EVENT_STOP that the current code would LISTEN, distinguish the
group-stop *BEGIN* (park with LISTEN) from the group-stop *END* / SIGCONT
re-report (must resume with PTRACE_CONT, signal 0, NOT re-LISTEN). The robust
mechanism is: track which tids are already LISTENING; a fresh EINVAL EVENT_STOP
on an already-LISTENING tid is the SIGCONT trailing edge → PTRACE_CONT it back to
RUNNING. Equivalently: never leave a tracee in LISTEN across a SIGCONT — re-CONT
on the group-stop-end re-report. ``apply_fix=True`` switches the model to this
policy and the same chromium scenario then drains to a clean leader exit.

HONESTY / HOST LIMITATION (load-bearing — do NOT delete)
--------------------------------------------------------
darwin runs no real seccomp filter, no PTRACE_SEIZE/LISTEN, no real group-stop or
futex. This model encodes the supervisor's resume POLICY and the futex/group-stop
*coupling* the way the kernel manual specifies it, then asks whether that policy
admits a state with a permanently un-resumed tracee. A green "fixed" bar here
PROVES the policy gap and that the candidate policy closes it in the model; it
does NOT by itself prove the device no longer hangs — only a device drain
(DEVICE-REQ, WS-1) confirms the real kernel matches the modelled group-stop-end
semantics. The model's value is: (1) a concrete, runnable repro of the deadlock,
(2) a precise pin on the offending transition, (3) a regression guard that fails
if a future edit reintroduces a re-LISTEN-without-resume policy.

Pure-Python, no third-party deps.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Dict, List, Optional


# --------------------------------------------------------------------------- #
# Syscall-nr facts shared with tests/mt_supervisor_model.py + PCGATE BPF.
# (Re-pinned here so this module is self-contained; the regression test asserts
#  they stay identical to the other model — drift is a bug.)
# --------------------------------------------------------------------------- #

# The 9 arm64 path nrs PCGATE RET_TRACEs; everything else RET_ALLOW.
PATH_SYSCALL_NRS = frozenset({34, 35, 48, 56, 78, 79, 291, 437, 439})

NR_FUTEX = 98       # RET_ALLOW: a futex wait/wake is INVISIBLE to the supervisor.
NR_CLONE = 220
NR_CLONE3 = 435
NR_EXECVE = 221
NR_OPENAT = 56


# --------------------------------------------------------------------------- #
# Tracee runtime state (the LIVENESS state — NOT the per-event branch view).
# --------------------------------------------------------------------------- #

class RunState(str, Enum):
    RUNNING = "RUNNING"          # CONT'd; executing guest code
    LISTENING = "LISTENING"      # LISTEN-parked on a group-stop; not running
    FUTEX_WAIT = "FUTEX_WAIT"    # blocked in futex on a sibling (supervisor-invisible)
    DEAD = "DEAD"                # reaped


class PtraceOp(str, Enum):
    """The ptrace resume op the supervisor issues for an event (observable)."""
    CONT = "PTRACE_CONT"
    LISTEN = "PTRACE_LISTEN"
    EMULATE_CONT = "emulate_then_CONT"
    REAP = "reap"                # WIFEXITED/WIFSIGNALED: no resume
    NOOP_ESRCH = "noop_esrch"    # tid raced away


# --------------------------------------------------------------------------- #
# Event stream — what waitpid(-1, __WALL) returns, pre-decoded the way the C loop
# decodes status>>16 + WSTOPSIG + the GETSIGINFO disambiguation. Crucially we add
# the two events the per-event model does NOT carry: GROUP_STOP_END (the SIGCONT
# trailing-edge re-report of a LISTEN-parked thread) and the futex coupling.
# --------------------------------------------------------------------------- #

class EvKind(str, Enum):
    SECCOMP = "seccomp"                  # EVENT_SECCOMP (path/exec nr)
    CLONE = "clone"                      # EVENT_CLONE (parent side)
    EXEC = "exec"                        # EVENT_EXEC
    NEW_TID_STOP = "new_tid_stop"        # EVENT_STOP, tid not yet known
    GROUP_STOP_BEGIN = "group_stop_begin"  # EVENT_STOP, known tid, group-stop START (EINVAL)
    GROUP_STOP_END = "group_stop_end"    # EVENT_STOP, known tid, group-stop END (SIGCONT)
    SIGSYS_SECCOMP = "sigsys_seccomp"    # SIGSYS, SYS_SECCOMP -> emulate
    EXIT = "exit"                        # WIFEXITED
    KILLED = "killed"                    # WIFSIGNALED


@dataclass(frozen=True)
class Ev:
    """A pre-decoded waitpid stop. ``futex_on`` couples a thread that BEGINS a
    group-stop to the sibling that is (or will be) FUTEX_WAITing on it: while the
    grouped thread is parked, the sibling cannot make progress."""
    tid: int
    kind: EvKind
    nr: Optional[int] = None
    new_child_tid: Optional[int] = None
    # For GROUP_STOP_*: the set of sibling tids that block in futex on this tid
    # for the duration of its group-stop (they only wake when it RUNS again).
    futex_waiters: tuple = ()


@dataclass
class Step:
    tid: int
    op: PtraceOp
    state_after: RunState
    note: str = ""


class SupervisorLivenessError(AssertionError):
    """Raised by ``assert_drains`` when the modelled supervisor reaches a state
    with a permanently un-resumed tracee while the leader is still alive — i.e.
    the waitpid(-1) loop would block forever (the chromium deadlock)."""


# --------------------------------------------------------------------------- #
# The liveness model.
# --------------------------------------------------------------------------- #

class SupervisorModel:
    """Discrete-event model of the single-threaded waitpid(-1) supervisor.

    Feed it a list of pre-decoded ``Ev`` (the stream waitpid would return). It
    applies the same resume policy the C code applies and tracks the global
    liveness state. After draining the explicit stream it runs a FIXED-POINT
    settle: any tid left LISTENING that the policy can resume is resumed, any
    FUTEX_WAITer whose target is RUNNING/dead wakes. If a fixed point is reached
    in which the leader is NOT dead and a tracee is stuck LISTENING/FUTEX_WAIT
    with no producible event to free it, the run has DEADLOCKED.

    ``apply_fix=False`` == the current device policy (re-LISTEN on group-stop-end)
    → reproduces the deadlock. ``apply_fix=True`` == the candidate fix (resume a
    LISTENING tid on its group-stop-end / SIGCONT re-report) → drains cleanly.
    """

    def __init__(self, leader_pid: int, *, apply_fix: bool = False):
        self.leader_pid = leader_pid
        self.apply_fix = apply_fix
        self.known_tids: set[int] = {leader_pid}
        self.state: Dict[int, RunState] = {leader_pid: RunState.RUNNING}
        # futex coupling: waiter_tid -> target_tid it is blocked on.
        self.futex_block: Dict[int, int] = {}
        self.guest_threads = 0
        self.path_traps = 0
        self.exec_events = 0
        self.emulated_syscalls = 0
        self.history: List[Step] = []
        self.leader_exited = False

    # -- queries ----------------------------------------------------------- #

    def listening_tids(self) -> List[int]:
        return [t for t, s in self.state.items() if s == RunState.LISTENING]

    def futex_waiting_tids(self) -> List[int]:
        return [t for t, s in self.state.items() if s == RunState.FUTEX_WAIT]

    def running_tids(self) -> List[int]:
        return [t for t, s in self.state.items() if s == RunState.RUNNING]

    def alive_tids(self) -> List[int]:
        return [t for t, s in self.state.items() if s != RunState.DEAD]

    def stuck_tids(self) -> List[int]:
        """Tracees that are neither RUNNING nor DEAD — i.e. parked/blocked."""
        return [
            t for t, s in self.state.items()
            if s in (RunState.LISTENING, RunState.FUTEX_WAIT)
        ]

    # -- single-event transition (mirror of the C while-loop body) --------- #

    def step(self, ev: Ev) -> Step:
        kind = ev.kind
        if kind in (EvKind.EXIT, EvKind.KILLED):
            return self._reap(ev)
        if kind == EvKind.SECCOMP:
            return self._seccomp(ev)
        if kind == EvKind.CLONE:
            return self._clone(ev)
        if kind == EvKind.EXEC:
            return self._exec(ev)
        if kind == EvKind.NEW_TID_STOP:
            return self._new_tid_stop(ev)
        if kind == EvKind.GROUP_STOP_BEGIN:
            return self._group_stop_begin(ev)
        if kind == EvKind.GROUP_STOP_END:
            return self._group_stop_end(ev)
        if kind == EvKind.SIGSYS_SECCOMP:
            return self._sigsys(ev)
        raise ValueError(f"unhandled event kind {kind!r}")

    def run(self, events: List[Ev]) -> List[Step]:
        out = [self.step(e) for e in events]
        return out

    # -- handlers ---------------------------------------------------------- #

    def _record(self, step: Step) -> Step:
        self.history.append(step)
        return step

    def _reap(self, ev: Ev) -> Step:
        self.state[ev.tid] = RunState.DEAD
        self.known_tids.discard(ev.tid)
        if ev.tid == self.leader_pid:
            self.leader_exited = True
        return self._record(Step(ev.tid, PtraceOp.REAP, RunState.DEAD, "reaped"))

    def _seccomp(self, ev: Ev) -> Step:
        self.path_traps += 1
        if self.state.get(ev.tid) != RunState.DEAD:
            self.state[ev.tid] = RunState.RUNNING
        return self._record(
            Step(ev.tid, PtraceOp.CONT, RunState.RUNNING,
                 f"path/exec trap nr={ev.nr} -> rewrite+CONT")
        )

    def _clone(self, ev: Ev) -> Step:
        self.guest_threads += 1
        if ev.new_child_tid is not None:
            # child auto-attached; its initial stop arrives later as NEW_TID_STOP.
            self.state.setdefault(ev.new_child_tid, RunState.RUNNING)
        self.state[ev.tid] = RunState.RUNNING
        return self._record(
            Step(ev.tid, PtraceOp.CONT, RunState.RUNNING,
                 f"EVENT_CLONE child={ev.new_child_tid}")
        )

    def _exec(self, ev: Ev) -> Step:
        self.exec_events += 1
        self.state[ev.tid] = RunState.RUNNING
        return self._record(Step(ev.tid, PtraceOp.CONT, RunState.RUNNING, "EVENT_EXEC -> CONT"))

    def _new_tid_stop(self, ev: Ev) -> Step:
        # Anti-park guard: a tid not yet known is always CONT'd, never parked.
        self.known_tids.add(ev.tid)
        self.state[ev.tid] = RunState.RUNNING
        return self._record(
            Step(ev.tid, PtraceOp.CONT, RunState.RUNNING,
                 "new-tid initial stop -> CONT (anti-park)")
        )

    def _group_stop_begin(self, ev: Ev) -> Step:
        # Known tid, GETSIGINFO==EINVAL, NOT already LISTENING -> a real group-stop
        # START. Park it with LISTEN. Any sibling that futex-waits on it now blocks.
        self.state[ev.tid] = RunState.LISTENING
        for waiter in ev.futex_waiters:
            self.state.setdefault(waiter, RunState.RUNNING)
            self.state[waiter] = RunState.FUTEX_WAIT
            self.futex_block[waiter] = ev.tid
        return self._record(
            Step(ev.tid, PtraceOp.LISTEN, RunState.LISTENING,
                 "group-stop BEGIN -> LISTEN (parked)")
        )

    def _group_stop_end(self, ev: Ev) -> Step:
        # The SIGCONT trailing edge: a LISTEN-parked thread re-reports an EVENT_STOP
        # whose GETSIGINFO ALSO returns EINVAL (still no queued siginfo). This is the
        # exact ambiguity the current C code mishandles.
        if not self.apply_fix:
            # CURRENT POLICY: indistinguishable from a group-stop BEGIN -> re-LISTEN.
            # The thread is re-parked: it never returns to RUNNING. THE BUG.
            self.state[ev.tid] = RunState.LISTENING
            return self._record(
                Step(ev.tid, PtraceOp.LISTEN, RunState.LISTENING,
                     "group-stop END misclassified -> re-LISTEN (DEADLOCK transition)")
            )
        # CANDIDATE FIX: a fresh EVENT_STOP on an ALREADY-LISTENING tid is the
        # SIGCONT trailing edge -> resume with CONT (signal 0), back to RUNNING.
        self.state[ev.tid] = RunState.RUNNING
        self._wake_futex_waiters_of(ev.tid)
        return self._record(
            Step(ev.tid, PtraceOp.CONT, RunState.RUNNING,
                 "group-stop END -> CONT (resumed; fix)")
        )

    def _sigsys(self, ev: Ev) -> Step:
        self.emulated_syscalls += 1
        if self.state.get(ev.tid) != RunState.DEAD:
            self.state[ev.tid] = RunState.RUNNING
        return self._record(
            Step(ev.tid, PtraceOp.EMULATE_CONT, RunState.RUNNING,
                 f"SIGSYS nr={ev.nr} emulate -ENOSYS + CONT")
        )

    # -- futex coupling ---------------------------------------------------- #

    def _wake_futex_waiters_of(self, target: int) -> None:
        """A tid that just became RUNNING wakes any sibling FUTEX_WAITing on it."""
        for waiter, tgt in list(self.futex_block.items()):
            if tgt == target:
                if self.state.get(waiter) == RunState.FUTEX_WAIT:
                    self.state[waiter] = RunState.RUNNING
                del self.futex_block[waiter]

    # -- liveness settle / deadlock detection ------------------------------ #

    def is_deadlocked(self) -> bool:
        """True iff the leader is alive but the run can make NO further progress:
        a tracee is stuck (LISTENING or FUTEX_WAIT) and nothing in the model can
        free it. This is precisely the state where waitpid(-1) blocks forever."""
        if self.leader_exited:
            return False
        stuck = self.stuck_tids()
        if not stuck:
            return False
        # A futex waiter is freeable iff its target is RUNNING (or dead). If every
        # stuck tid's only unblock path is itself blocked, the system is wedged.
        for t in stuck:
            if self.state[t] == RunState.FUTEX_WAIT:
                tgt = self.futex_block.get(t)
                if tgt is not None and self.state.get(tgt) == RunState.RUNNING:
                    return False  # this waiter will be woken -> progress possible
            # A LISTENING tid is only freeable by the FIX policy on group-stop-end;
            # under the current policy a re-LISTEN keeps it LISTENING forever.
        # No stuck tid has a live unblock path -> wedged.
        # (LISTENING with no group-stop-end resume + FUTEX_WAIT on a non-running
        #  target are both permanent under the current policy.)
        return True

    def assert_drains(self) -> None:
        """Raise SupervisorLivenessError iff the run deadlocked. Use after run()."""
        if self.is_deadlocked():
            stuck = {t: self.state[t].value for t in self.stuck_tids()}
            raise SupervisorLivenessError(
                "supervisor waitpid(-1) would block forever: leader alive, "
                f"stuck tracees={stuck}, futex_block={self.futex_block}, "
                f"apply_fix={self.apply_fix}"
            )


# --------------------------------------------------------------------------- #
# Scenario generator — the chromium clone storm + group-stop + cross-thread futex
# that reproduces the device deadlock. N workers, one of which group-stops while a
# sibling futex-waits on it; the group-stop then ENDS (SIGCONT) and the supervisor
# must resume the parked worker or its sibling hangs forever.
# --------------------------------------------------------------------------- #

def chromium_storm_events(
    leader_pid: int = 1000,
    workers: int = 20,
    *,
    group_stop_worker_index: int = 7,
    futex_sibling_index: int = 8,
) -> List[Ev]:
    """Build the event stream for a chromium-like bring-up:

    1. leader does single-thread init: a couple of path traps + a blocked-nr SIGSYS.
    2. clone storm: ``workers`` worker threads, each clone -> initial stop -> openat.
    3. one worker (``group_stop_worker_index``) hits a GROUP-STOP BEGIN while a
       sibling (``futex_sibling_index``) is FUTEX_WAITing on it (chromium's
       cross-thread condvar/seqlock pattern).
    4. the group-stop ENDS (SIGCONT) — the worker re-reports a group-stop-end
       EVENT_STOP. Under the current policy this re-LISTENs the worker; the
       FUTEX_WAITing sibling then never wakes, never exits.
    5. all (resumable) workers + leader try to EXIT.

    Returned in the serialized order waitpid(-1) would surface them.
    """
    assert 0 <= group_stop_worker_index < workers
    assert 0 <= futex_sibling_index < workers
    assert group_stop_worker_index != futex_sibling_index
    base = leader_pid + 1
    gs_tid = base + group_stop_worker_index
    sib_tid = base + futex_sibling_index

    ev: List[Ev] = [
        Ev(tid=leader_pid, kind=EvKind.SECCOMP, nr=NR_OPENAT),
        Ev(tid=leader_pid, kind=EvKind.SECCOMP, nr=79),  # newfstatat
        Ev(tid=leader_pid, kind=EvKind.SIGSYS_SECCOMP, nr=99),  # set_robust_list
    ]
    # clone storm + each worker's initial stop + a path trap.
    for i in range(workers):
        child = base + i
        ev.append(Ev(tid=leader_pid, kind=EvKind.CLONE, new_child_tid=child))
        ev.append(Ev(tid=child, kind=EvKind.NEW_TID_STOP))
        ev.append(Ev(tid=child, kind=EvKind.SECCOMP, nr=NR_OPENAT))
    # the group-stop on one worker, with a sibling futex-waiting on it.
    ev.append(Ev(tid=gs_tid, kind=EvKind.GROUP_STOP_BEGIN, futex_waiters=(sib_tid,)))
    # the group-stop ENDS (SIGCONT) — the worker re-reports.
    ev.append(Ev(tid=gs_tid, kind=EvKind.GROUP_STOP_END))
    # everyone tries to exit. The futex sibling can only exit if it was woken; we
    # append its EXIT too — the model will show it never actually became reapable
    # under the current policy (it stays FUTEX_WAIT, so is_deadlocked() trips
    # because the leader has not exited while the sibling is stuck).
    for i in range(workers):
        child = base + i
        if i == group_stop_worker_index or i == futex_sibling_index:
            continue  # these two are the deadlock pair; do not pre-exit them
        ev.append(Ev(tid=child, kind=EvKind.EXIT))
    return ev


__all__ = [
    "PATH_SYSCALL_NRS",
    "NR_FUTEX",
    "NR_CLONE",
    "NR_CLONE3",
    "NR_EXECVE",
    "NR_OPENAT",
    "RunState",
    "PtraceOp",
    "EvKind",
    "Ev",
    "Step",
    "SupervisorModel",
    "SupervisorLivenessError",
    "chromium_storm_events",
]
