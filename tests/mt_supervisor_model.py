"""Host state-machine model of the ALR multi-thread ptrace supervisor.

CP-6 variable-2 (chromium multi-thread storm). This module models, as a PURE
PYTHON finite state machine, the per-tid decision logic of the device supervisor
in `app/src/main/cpp/runtime_report.cpp` L2754-2920 (the PTRACE_SEIZE +
PTRACE_EVENT_STOP / EVENT_CLONE / EVENT_SECCOMP / SIGSYS handling labelled
"THE MULTI-THREAD FIX"). Its single purpose is to let the host (darwin, no real
kernel) DECIDE — before the device probe is even built — whether the documented
"Chromium Threads=1 hang" is:

  (a) a clone the guest never issues (single-init-too-slow / window-too-short),
  (b) a clone that traps and wedges the supervisor,
  (c) a futex / group-stop deadlock the supervisor mis-classifies, or
  (d) a SIGSYS-emulate storm.

HONESTY / HOST LIMITATION (load-bearing, do NOT delete):
  darwin CANNOT run a real seccomp filter, PTRACE_SEIZE, PTRACE_LISTEN, a real
  group-stop, or arm64 self-rewrite. So this model verifies ONLY the *classify /
  branch* logic — "given this event from this tid in this state, which ptrace op
  does the supervisor pick, and can that pick PARK a thread that should run (or
  RUN a thread that should park)?". The *actual* group-stop / inheritance / futex
  behavior of the real kernel is DEVICE-ONLY (DEVICE-REQ gates in cp6-status.md).
  A model that proves "no branch can wrongly park a new tid" does NOT prove the
  device does not hang — it only removes the supervisor-classify hypothesis from
  the candidate set, narrowing what the device probe must look for.

The model is faithful to these code facts (verified against runtime_report.cpp):
  * clone/clone3/futex are NON-path nrs -> PCGATE BPF RET_ALLOW (no EVENT_SECCOMP
    trap). The supervisor therefore NEVER sees a clone *entry* trap; it only sees
    the kernel-generated PTRACE_EVENT_CLONE (parent side, ++guest_threads) and the
    new child's initial PTRACE_EVENT_STOP. (libalr_interpose.c L426-428.)
  * known_tids is pre-seeded with the leader pid (L2092). A new tid's FIRST
    EVENT_STOP hits `known_tids.insert(w).second == true` -> always CONT, never
    parked (L2783-2792). This is the documented anti-park guard.
  * a KNOWN tid's EVENT_STOP with GETSIGINFO==EINVAL is a real group-stop -> LISTEN
    (park); with siginfo -> CONT (L2793-2820).
  * SIGSYS with si_code==SYS_SECCOMP is emulated to -ENOSYS then CONT; a runaway
    backstop SIGKILLs at >1<<20 emulations (L2856-2896).

Pure-python, no deps. Run the tests with:
  uvx --with pytest pytest tests/test_mt_supervisor_model.py -q
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Dict, List, Optional, Tuple


# --------------------------------------------------------------------------- #
# Syscall-nr facts (must match libalr_interpose.c PCGATE + ADR-002).
# --------------------------------------------------------------------------- #

# The 9 arm64 path nrs the PCGATE filter RET_TRACEs (everything else RET_ALLOW).
# Frozen — divergence from runtime_report.cpp / storm_cost_model is a bug.
PATH_SYSCALL_NRS = frozenset({34, 35, 48, 56, 78, 79, 291, 437, 439})

# Non-path nrs that nonetheless reach the supervisor by a DIFFERENT mechanism:
#   - clone/clone3/fork/vfork/execve generate a PTRACE_EVENT_* (not EVENT_SECCOMP)
#     because of PTRACE_O_TRACECLONE/FORK/VFORK/EXEC, even though seccomp ALLOWs
#     their *entry*.
#   - set_robust_list(99)/rseq(293) etc. are SIGSYS-emulated (blocked nr set).
NR_CLONE = 220
NR_CLONE3 = 435
NR_FORK = 0  # arm64 has no fork(2); glibc uses clone — kept for completeness only
NR_FUTEX = 98
NR_EXECVE = 221
NR_SET_ROBUST_LIST = 99
NR_RSEQ = 293
NR_GETPID = 172
NR_MMAP = 222
NR_MPROTECT = 226

# Syscalls whose ENTRY produces a kernel PTRACE_EVENT_* (clone/fork/exec family).
# These are the ONLY non-path nrs the supervisor sees as an EVENT (not a trap).
EVENT_GENERATING_NRS = frozenset({NR_CLONE, NR_CLONE3, NR_EXECVE})


# --------------------------------------------------------------------------- #
# ptrace events / stop kinds the model understands (mirror of status>>16 codes
# plus the WSTOPSIG / GETSIGINFO disambiguation the C code performs).
# --------------------------------------------------------------------------- #

class Event(str, Enum):
    """A waitpid()-reported stop, already decoded the way the C loop decodes it.

    The C supervisor decodes `status` into (stopsig, event=status>>16) then
    further disambiguates EVENT_STOP via GETSIGINFO. We pass that already-decoded
    intent in directly so the model tests the BRANCH logic, not status-bit math.
    """
    SECCOMP = "seccomp"            # PTRACE_EVENT_SECCOMP (a RET_TRACE path/exec nr)
    CLONE = "clone"               # PTRACE_EVENT_CLONE (parent side; ++guest_threads)
    EXEC = "exec"                 # PTRACE_EVENT_EXEC
    NEW_TID_STOP = "new_tid_stop"  # EVENT_STOP from a tid not yet in known_tids
    GROUP_STOP = "group_stop"     # EVENT_STOP, known tid, GETSIGINFO==EINVAL
    INTERRUPT_STOP = "interrupt"  # EVENT_STOP, known tid, GETSIGINFO ok (siginfo)
    SIGSYS_SECCOMP = "sigsys_seccomp"  # SIGSYS, si_code==SYS_SECCOMP (emulate)
    SIGSYS_BAD = "sigsys_bad"     # SIGSYS, si_code!=SYS_SECCOMP (deliver, crash)
    SIGNAL = "signal"             # generic signal-delivery-stop (default branch)
    EXIT = "exit"                 # WIFEXITED
    KILLED = "killed"             # WIFSIGNALED


class Action(str, Enum):
    """The ptrace resume op the supervisor selects for an event."""
    CONT = "PTRACE_CONT"               # resume, signal 0 (or deliver for SIGNAL)
    CONT_DELIVER = "PTRACE_CONT_deliver"  # resume, deliver the carried signal
    LISTEN = "PTRACE_LISTEN"           # park a group-stopped thread (do NOT run)
    EMULATE_CONT = "emulate_then_CONT"  # patch x0=-ENOSYS, then CONT
    SIGKILL = "SIGKILL_runaway"        # emul backstop tripped
    REAP = "reap"                      # tid exited/killed; close fd, no resume
    NOOP_ESRCH = "noop_esrch"          # tid raced away; benign loop


class TidState(str, Enum):
    """The model's view of a tid between events (NOT the kernel's /proc state).

    NEW         : cloned, EVENT_CLONE seen on parent, child's initial stop pending
    RUNNING     : last resume was CONT; the tid is executing
    GROUP_STOPPED: parked via LISTEN; will re-report when the group-stop ends
    LISTENING   : alias kept for parity with task description's vocabulary
    DEAD        : reaped
    """
    NEW = "NEW"
    RUNNING = "RUNNING"
    GROUP_STOPPED = "GROUP_STOPPED"
    LISTENING = "LISTENING"  # synonym of GROUP_STOPPED in this model
    DEAD = "DEAD"


# Group-control signals the default branch must NEVER forward (suppress -> CONT 0).
GROUP_STOP_SIGNALS = frozenset({"SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU"})

# Runaway emulate backstop (runtime_report.cpp L2889: > (1 << 20)).
EMUL_RUNAWAY_BACKSTOP = 1 << 20


# --------------------------------------------------------------------------- #
# A single supervised-event record (what waitpid would return, pre-decoded).
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class TraceEvent:
    tid: int
    kind: Event
    nr: Optional[int] = None          # for SECCOMP / SIGSYS_* : the syscall nr
    signal: Optional[str] = None      # for SIGNAL : the carried signal name
    new_child_tid: Optional[int] = None  # for CLONE : the child tid (GETEVENTMSG)


@dataclass
class StepResult:
    """Outcome of feeding one TraceEvent to the supervisor model."""
    tid: int
    action: Action
    new_state: Optional[TidState]
    note: str = ""


# --------------------------------------------------------------------------- #
# The supervisor model itself — a faithful, side-effect-free state machine.
# --------------------------------------------------------------------------- #

class MtSupervisorModel:
    """Pure model of runtime_report.cpp's multi-tracee waitpid loop body.

    One instance == one supervised guest run. `leader_pid` is pre-seeded into
    known_tids exactly as L2092 does. Feed it TraceEvents in the order waitpid
    would return them; it picks the same Action the C code would, and tracks
    per-tid state + global counters (guest_threads, emulated_syscalls).

    The model is deliberately TOTAL: every (state, event) pair returns an Action
    and never raises for a reachable input, so a test can assert "no input parks a
    thread that should run". An *unreachable* input (e.g. a SECCOMP trap with a
    non-path nr, which PCGATE would never produce) raises ModelInvariantError so a
    mis-stated test fails loudly instead of silently passing.
    """

    def __init__(self, leader_pid: int, *, emul_backstop: int = EMUL_RUNAWAY_BACKSTOP):
        self.leader_pid = leader_pid
        self.known_tids: set[int] = {leader_pid}          # L2092 pre-seed
        self.state: Dict[int, TidState] = {leader_pid: TidState.RUNNING}
        self.guest_threads = 0                            # ++ on EVENT_CLONE
        self.emulated_syscalls = 0                        # ++ on SIGSYS emulate
        self.path_traps = 0
        self.exec_events = 0
        self.emul_backstop = emul_backstop
        self.trace_hist: Dict[int, int] = {}              # M-R2 mirror
        self.emul_hist: Dict[int, int] = {}               # M-R2 mirror
        self.history: List[StepResult] = []
        self._killed = False

    # -- helpers ----------------------------------------------------------- #

    def alive_tids(self) -> List[int]:
        return [t for t, s in self.state.items() if s != TidState.DEAD]

    def parked_tids(self) -> List[int]:
        return [
            t for t, s in self.state.items()
            if s in (TidState.GROUP_STOPPED, TidState.LISTENING)
        ]

    def running_tids(self) -> List[int]:
        return [t for t, s in self.state.items() if s == TidState.RUNNING]

    # -- the single-event transition (mirror of the C while(true) body) ---- #

    def step(self, ev: TraceEvent) -> StepResult:
        if self._killed:
            # After a SIGKILL backstop the C loop just drains reaps; model the
            # same — any further event from a still-live tid resolves to REAP.
            res = StepResult(ev.tid, Action.REAP, TidState.DEAD,
                             "post-backstop drain")
            self.state[ev.tid] = TidState.DEAD
            self.history.append(res)
            return res

        handler = {
            Event.EXIT: self._on_reap,
            Event.KILLED: self._on_reap,
            Event.SECCOMP: self._on_seccomp,
            Event.CLONE: self._on_clone,
            Event.EXEC: self._on_exec,
            Event.NEW_TID_STOP: self._on_new_tid_stop,
            Event.GROUP_STOP: self._on_group_stop,
            Event.INTERRUPT_STOP: self._on_interrupt_stop,
            Event.SIGSYS_SECCOMP: self._on_sigsys_seccomp,
            Event.SIGSYS_BAD: self._on_sigsys_bad,
            Event.SIGNAL: self._on_signal,
        }[ev.kind]
        res = handler(ev)
        self.history.append(res)
        return res

    def run(self, events: List[TraceEvent]) -> List[StepResult]:
        return [self.step(e) for e in events]

    # -- per-event handlers (each maps 1:1 to a C branch) ------------------ #

    def _on_reap(self, ev: TraceEvent) -> StepResult:
        # L2147-2160: WIFEXITED/WIFSIGNALED -> evict fd, no resume.
        self.state[ev.tid] = TidState.DEAD
        self.known_tids.discard(ev.tid)
        return StepResult(ev.tid, Action.REAP, TidState.DEAD, "tid reaped")

    def _on_seccomp(self, ev: TraceEvent) -> StepResult:
        # L2166: PTRACE_EVENT_SECCOMP. PCGATE only RET_TRACEs the 9 path nrs
        # (+ execve under PCGATE=1). A SECCOMP trap for any OTHER nr is
        # UNREACHABLE — guard it so a wrong test blows up.
        if ev.nr is None:
            raise ModelInvariantError("SECCOMP event needs a syscall nr")
        if ev.nr not in PATH_SYSCALL_NRS and ev.nr not in (NR_EXECVE,):
            raise ModelInvariantError(
                f"nr {ev.nr} cannot produce EVENT_SECCOMP under PCGATE "
                f"(only path nrs {sorted(PATH_SYSCALL_NRS)} + execve do)"
            )
        self.path_traps += 1
        self.trace_hist[ev.nr] = self.trace_hist.get(ev.nr, 0) + 1
        # The C code rewrites x1 then CONTs (path) or takes the exec branch then
        # CONTs. Either way the tid resumes RUNNING. No state change otherwise.
        self.state.setdefault(ev.tid, TidState.RUNNING)
        if self.state[ev.tid] != TidState.DEAD:
            self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.CONT, TidState.RUNNING,
                          f"path-trap nr={ev.nr} rewritten+resumed")

    def _on_clone(self, ev: TraceEvent) -> StepResult:
        # L2828-2831: PTRACE_EVENT_CLONE -> ++guest_threads, CONT the parent.
        # The CHILD is auto-attached (SEIZE inheritance) but is NOT yet known;
        # its initial stop arrives as a separate NEW_TID_STOP event later.
        self.guest_threads += 1
        if ev.new_child_tid is not None:
            # The C code does NOT pre-register the child here (it relies on the
            # NEW_TID_STOP anti-park guard). We mark the child NEW so the model
            # can DETECT whether a pre-registration race is even possible.
            self.state[ev.new_child_tid] = TidState.NEW
        self.state[ev.tid] = TidState.RUNNING  # parent resumes
        return StepResult(ev.tid, Action.CONT, TidState.RUNNING,
                          f"EVENT_CLONE child={ev.new_child_tid} "
                          f"guest_threads={self.guest_threads}")

    def _on_exec(self, ev: TraceEvent) -> StepResult:
        # L2744-2752: EVENT_EXEC -> evict stale fds, CONT, no re-map.
        self.exec_events += 1
        self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.CONT, TidState.RUNNING, "EVENT_EXEC resumed")

    def _on_new_tid_stop(self, ev: TraceEvent) -> StepResult:
        # L2783-2792: a tid NOT in known_tids -> insert + ALWAYS CONT (never park).
        # This is THE anti-park guard. If the tid was already known, this event
        # is mis-labelled (a known tid's EVENT_STOP must come in as GROUP_STOP or
        # INTERRUPT_STOP) -> flag it as the documented L2781 hang precursor.
        if ev.tid in self.known_tids:
            raise ModelInvariantError(
                f"tid {ev.tid} already known — its EVENT_STOP must be classified "
                f"as GROUP_STOP/INTERRUPT, not NEW_TID_STOP (the L2781 bug class)"
            )
        self.known_tids.add(ev.tid)
        self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.CONT, TidState.RUNNING,
                          "new-tid initial stop -> CONT (anti-park guard)")

    def _on_group_stop(self, ev: TraceEvent) -> StepResult:
        # L2799-2807: known tid, GETSIGINFO==EINVAL -> LISTEN (park). NEVER CONT.
        if ev.tid not in self.known_tids:
            raise ModelInvariantError(
                f"tid {ev.tid} not known — a true group-stop only fires for a tid "
                f"that has already stopped once (else it is a new-tid initial stop)"
            )
        self.state[ev.tid] = TidState.GROUP_STOPPED
        return StepResult(ev.tid, Action.LISTEN, TidState.GROUP_STOPPED,
                          "real group-stop -> LISTEN (parked, not run)")

    def _on_interrupt_stop(self, ev: TraceEvent) -> StepResult:
        # L2808-2820: known tid, GETSIGINFO ok -> CONT signal 0.
        if ev.tid not in self.known_tids:
            raise ModelInvariantError(
                f"tid {ev.tid} not known — an INTERRUPT stop is a known-tid event"
            )
        self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.CONT, TidState.RUNNING,
                          "INTERRUPT/known-tid stop -> CONT signal 0")

    def _on_sigsys_seccomp(self, ev: TraceEvent) -> StepResult:
        # L2856-2896: SYS_SECCOMP SIGSYS -> emulate -ENOSYS, CONT; backstop SIGKILL.
        if ev.nr is None:
            raise ModelInvariantError("SIGSYS_SECCOMP needs a syscall nr")
        self.emulated_syscalls += 1
        self.emul_hist[ev.nr] = self.emul_hist.get(ev.nr, 0) + 1
        if self.emulated_syscalls > self.emul_backstop:
            self._killed = True
            self.state[self.leader_pid] = TidState.DEAD
            return StepResult(ev.tid, Action.SIGKILL, TidState.DEAD,
                              f"emul runaway > {self.emul_backstop}: SIGKILL leader")
        self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.EMULATE_CONT, TidState.RUNNING,
                          f"SIGSYS nr={ev.nr} emulated -ENOSYS, resumed")

    def _on_sigsys_bad(self, ev: TraceEvent) -> StepResult:
        # L2862-2866: si_code != SYS_SECCOMP -> DELIVER the SIGSYS (guest crashes).
        self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.CONT_DELIVER, TidState.RUNNING,
                          "bad-syscall SIGSYS delivered (guest will crash visibly)")

    def _on_signal(self, ev: TraceEvent) -> StepResult:
        # L2915-2935: default branch. Group-stop signals suppressed (CONT 0);
        # everything else delivered.
        sig = ev.signal or "SIGUSR1"
        if sig in GROUP_STOP_SIGNALS:
            self.state[ev.tid] = TidState.RUNNING
            return StepResult(ev.tid, Action.CONT, TidState.RUNNING,
                              f"{sig} suppressed (never forwarded -> CONT 0)")
        self.state[ev.tid] = TidState.RUNNING
        return StepResult(ev.tid, Action.CONT_DELIVER, TidState.RUNNING,
                          f"{sig} delivered to tracee")


class ModelInvariantError(AssertionError):
    """Raised when a test feeds the model an input the real PCGATE+SEIZE supervisor
    could never produce — surfacing a mis-stated scenario instead of a false pass."""


# --------------------------------------------------------------------------- #
# Deadlock-candidate classifier — the R1 decision the host can make NOW.
# --------------------------------------------------------------------------- #

class DeadlockCandidate(str, Enum):
    """The four hypotheses for "Chromium Threads=1" the device probe must split."""
    SINGLE_INIT_SLOW = "a:single-init-slow"   # clone never reached (window-too-short)
    CLONE_TRAP_WEDGE = "b:clone-trap-wedge"   # clone trapped + supervisor wedged
    FUTEX_OR_PARK = "c:futex-or-group-stop-park"  # parked on futex / mis-LISTEN
    SIGSYS_EMUL_STORM = "d:sigsys-emul-storm"  # SIGSYS-emulate floods stime


@dataclass
class TriageInput:
    """Normalized device-probe signals (what bench/deadlock_triage.py would parse
    out of the supervisor's read-only log lines). All host-injectable for tests."""
    clone_reached: bool          # did ANY PTRACE_EVENT_CLONE fire?
    max_threads: int             # peak guest_threads observed
    parked_tids: int             # tids left in LISTEN at the deadline
    stuck_tid_last_nr: Optional[int]  # last nr the stuck tid was seen at
    emulated_syscalls: int       # total SIGSYS-emulated
    window_s: float              # the alarm budget that was in effect


def classify_deadlock(inp: TriageInput) -> DeadlockCandidate:
    """Map normalized probe signals to ONE candidate. Pure function.

    Decision order is the same the diagnosis argues:
      1. No clone ever -> single-init-slow (a). (The supervisor has no clone path
         to wedge; guest_threads stays 0/1.)
      2. SIGSYS-emulate at the runaway backstop -> emul storm (d).
      3. A tid parked in LISTEN, OR stuck on futex(98) -> futex/group-stop (c).
      4. clone reached but a tid is stuck mid-path-trap with no park -> (b).
    This is the host model the device gate (R1 device_probe) is judged against.
    """
    # (a) The guest never even issued a clone the supervisor could see.
    if not inp.clone_reached and inp.max_threads <= 1:
        return DeadlockCandidate.SINGLE_INIT_SLOW
    # (d) Emulate ran away (the >1<<20 backstop region) — stime is SIGSYS-bound.
    if inp.emulated_syscalls >= EMUL_RUNAWAY_BACKSTOP:
        return DeadlockCandidate.SIGSYS_EMUL_STORM
    # (c) Something is parked, or a worker is blocked in futex_wait.
    if inp.parked_tids > 0 or inp.stuck_tid_last_nr == NR_FUTEX:
        return DeadlockCandidate.FUTEX_OR_PARK
    # (b) clone reached, no park, but progress stuck at a path trap.
    return DeadlockCandidate.CLONE_TRAP_WEDGE


# --------------------------------------------------------------------------- #
# SEIZE-branch race analysis — host-reachable reproduction of R1 weakness-1
# ("did the v122-124 SEIZE switch INDUCE a new group-stop deadlock?").
# --------------------------------------------------------------------------- #

@dataclass
class SeizeBranchInput:
    """The three GETSIGINFO outcomes the EVENT_STOP branch must split (L2783-2820),
    plus whether the tid was already known. This is the exact race surface."""
    tid_already_known: bool
    getsiginfo_result: str  # "einval" | "esrch" | "ok"


def seize_event_stop_decision(inp: SeizeBranchInput) -> Action:
    """Reproduce L2783-2820's EVENT_STOP branch as a pure function.

    The hypothesis under test (R1 weakness-1): can a NEW tid's initial stop ever
    be LISTEN-parked? The C code guards this with `known_tids.insert(w).second`
    FIRST — a brand-new tid is CONT'd before GETSIGINFO is ever consulted. So the
    only way to reach LISTEN is `tid_already_known AND getsiginfo==einval`. This
    function makes that provable on the host: feed all 6 combinations, assert
    LISTEN is reachable ONLY from (known, einval).
    """
    # FIRST branch (L2783): a tid not yet known is ALWAYS resumed, never parked.
    if not inp.tid_already_known:
        return Action.CONT
    # KNOWN tid: GETSIGINFO splits group-stop (einval->LISTEN) from interrupt (ok).
    if inp.getsiginfo_result == "einval":
        return Action.LISTEN
    if inp.getsiginfo_result == "esrch":
        # tid raced away; CONT/LISTEN no-ops with ESRCH -> treated as benign loop.
        return Action.NOOP_ESRCH
    return Action.CONT  # "ok" siginfo -> a real interrupt-stop -> CONT signal 0
