"""Host unit tests for tests/mt_supervisor_model.py (CP-6 variable-2).

These tests verify the PURE-LOGIC state machine that models the device
supervisor's multi-thread ptrace decision logic (runtime_report.cpp L2754-2920).
They CANNOT and DO NOT prove the device does not hang — darwin runs no real
seccomp/SEIZE/LISTEN/group-stop. They prove a narrower, decisive thing: that the
supervisor's CLASSIFY/BRANCH logic cannot, by construction, park a thread that
should run or run a thread that should park, and they encode the R1 deadlock
re-diagnosis (window-too-short as 1st hypothesis, SEIZE-induced park as 2nd) as
executable scenarios so the device probe knows exactly what to look for.

Run with:
  uvx --with pytest pytest tests/test_mt_supervisor_model.py -q
"""
from __future__ import annotations

import pytest

from tests.mt_supervisor_model import (
    EMUL_RUNAWAY_BACKSTOP,
    EVENT_GENERATING_NRS,
    NR_CLONE,
    NR_FUTEX,
    NR_GETPID,
    NR_MMAP,
    NR_SET_ROBUST_LIST,
    PATH_SYSCALL_NRS,
    Action,
    DeadlockCandidate,
    Event,
    ModelInvariantError,
    MtSupervisorModel,
    SeizeBranchInput,
    TidState,
    TraceEvent,
    TriageInput,
    classify_deadlock,
    seize_event_stop_decision,
)


# --------------------------------------------------------------------------- #
# Fact pins — the nr sets the model shares with storm_cost_model / PCGATE.
# A silent edit that drifts these from runtime_report.cpp must fail loudly.
# --------------------------------------------------------------------------- #

def test_path_nrs_match_storm_cost_model():
    # Identical frozenset to bench.storm_cost_model.PATH_SYSCALL_NRS.
    from bench.storm_cost_model import PATH_SYSCALL_NRS as SCM_PATH
    assert PATH_SYSCALL_NRS == SCM_PATH
    assert PATH_SYSCALL_NRS == frozenset({34, 35, 48, 56, 78, 79, 291, 437, 439})


def test_clone_futex_are_non_path():
    # The whole deadlock argument rests on this: clone/clone3/futex are NOT in the
    # 9 path nrs, so PCGATE RET_ALLOWs them — the supervisor never sees a clone or
    # futex *entry* trap.
    assert NR_CLONE not in PATH_SYSCALL_NRS
    assert NR_FUTEX not in PATH_SYSCALL_NRS
    assert NR_GETPID not in PATH_SYSCALL_NRS
    # clone/clone3/execve still reach the supervisor — but as EVENTs, not traps.
    assert NR_CLONE in EVENT_GENERATING_NRS


# --------------------------------------------------------------------------- #
# THE ANTI-PARK GUARD (L2783) — a freshly cloned tid is NEVER parked at birth.
# This is the core of "THE MULTI-THREAD FIX"; if the model can park a new tid,
# the device hang it claims to fix would be reproducible in logic.
# --------------------------------------------------------------------------- #

def test_new_tid_initial_stop_always_conts():
    sup = MtSupervisorModel(leader_pid=1000)
    # Parent clones a worker (tid 1001).
    sup.step(TraceEvent(tid=1000, kind=Event.CLONE, new_child_tid=1001))
    assert sup.guest_threads == 1
    # The new worker's INITIAL stop must CONT, never LISTEN.
    res = sup.step(TraceEvent(tid=1001, kind=Event.NEW_TID_STOP))
    assert res.action == Action.CONT
    assert res.new_state == TidState.RUNNING
    assert 1001 in sup.known_tids
    # And no tid is parked.
    assert sup.parked_tids() == []


def test_new_tid_stop_on_already_known_tid_is_flagged():
    # If a KNOWN tid's EVENT_STOP were ever mis-fed as a NEW_TID_STOP, that is the
    # exact L2781 mis-classification bug class — the model must reject it, not
    # silently CONT (which would mask a real-code regression).
    sup = MtSupervisorModel(leader_pid=1000)
    sup.step(TraceEvent(tid=1000, kind=Event.CLONE, new_child_tid=1001))
    sup.step(TraceEvent(tid=1001, kind=Event.NEW_TID_STOP))  # now known
    with pytest.raises(ModelInvariantError, match="already known"):
        sup.step(TraceEvent(tid=1001, kind=Event.NEW_TID_STOP))


# --------------------------------------------------------------------------- #
# GROUP-STOP handling — a real group-stop parks (LISTEN), never CONTs; and only
# a KNOWN tid can group-stop (a new tid's first stop is the anti-park case).
# --------------------------------------------------------------------------- #

def test_known_tid_group_stop_parks_with_listen():
    sup = MtSupervisorModel(leader_pid=1000)
    sup.step(TraceEvent(tid=1000, kind=Event.CLONE, new_child_tid=1001))
    sup.step(TraceEvent(tid=1001, kind=Event.NEW_TID_STOP))  # known now
    res = sup.step(TraceEvent(tid=1001, kind=Event.GROUP_STOP))
    assert res.action == Action.LISTEN
    assert res.new_state == TidState.GROUP_STOPPED
    assert 1001 in sup.parked_tids()


def test_known_tid_interrupt_stop_conts_signal0():
    sup = MtSupervisorModel(leader_pid=1000)
    sup.step(TraceEvent(tid=1000, kind=Event.CLONE, new_child_tid=1001))
    sup.step(TraceEvent(tid=1001, kind=Event.NEW_TID_STOP))
    res = sup.step(TraceEvent(tid=1001, kind=Event.INTERRUPT_STOP))
    assert res.action == Action.CONT
    assert 1001 not in sup.parked_tids()


def test_group_stop_on_unknown_tid_is_impossible():
    # A true group-stop only fires for a tid that has already stopped at least
    # once; classifying a never-seen tid as group-stop is the very mis-order the
    # anti-park guard prevents. Model rejects it.
    sup = MtSupervisorModel(leader_pid=1000)
    with pytest.raises(ModelInvariantError, match="not known"):
        sup.step(TraceEvent(tid=2002, kind=Event.GROUP_STOP))


# --------------------------------------------------------------------------- #
# CLONE RACE SCENARIO — the documented "22 workers" fan-out. Drive a full storm
# of clone -> initial-stop pairs and assert NONE wedge or park.
# --------------------------------------------------------------------------- #

def test_22_worker_clone_storm_no_park_no_wedge():
    sup = MtSupervisorModel(leader_pid=1)
    events = []
    for i in range(22):
        child = 100 + i
        events.append(TraceEvent(tid=1, kind=Event.CLONE, new_child_tid=child))
        events.append(TraceEvent(tid=child, kind=Event.NEW_TID_STOP))
    results = sup.run(events)
    # Every action is a CONT (parent resume or new-tid resume); zero LISTEN.
    assert all(r.action == Action.CONT for r in results)
    assert sup.guest_threads == 22
    assert sup.parked_tids() == []
    # All 22 workers + leader are known and running.
    assert len(sup.running_tids()) == 23


def test_clone_race_interleaved_initial_stops_then_path_traps():
    # Interleave: clone, clone, child2-stop, child1-stop, then each hammers a path
    # syscall (openat=56). Mirrors "22 threads hammering path syscalls" (L2076).
    sup = MtSupervisorModel(leader_pid=1)
    sup.run([
        TraceEvent(tid=1, kind=Event.CLONE, new_child_tid=10),
        TraceEvent(tid=1, kind=Event.CLONE, new_child_tid=11),
        TraceEvent(tid=11, kind=Event.NEW_TID_STOP),   # out-of-order stop
        TraceEvent(tid=10, kind=Event.NEW_TID_STOP),
        TraceEvent(tid=10, kind=Event.SECCOMP, nr=56),  # openat path-trap
        TraceEvent(tid=11, kind=Event.SECCOMP, nr=56),
    ])
    assert sup.guest_threads == 2
    assert sup.path_traps == 2
    assert sup.trace_hist[56] == 2
    assert sup.parked_tids() == []


# --------------------------------------------------------------------------- #
# BOOTSTRAP FUTEX SCENARIO — futex is RET_ALLOW (non-path), so a worker blocking
# on futex_wait does NOT produce any supervisor event. The model must show the
# supervisor literally has no event to mishandle here (so "futex deadlock in the
# supervisor" is impossible; a futex deadlock would be a GUEST-internal block,
# invisible to and uncaused by the supervisor).
# --------------------------------------------------------------------------- #

def test_futex_produces_no_supervisor_event():
    # There is no Event for a futex_wait: it is RET_ALLOW, no trap, no EVENT.
    # We assert the model has no handler that a futex entry could reach — i.e.
    # feeding a SECCOMP with nr=futex is rejected as unreachable under PCGATE.
    sup = MtSupervisorModel(leader_pid=1)
    with pytest.raises(ModelInvariantError, match="cannot produce EVENT_SECCOMP"):
        sup.step(TraceEvent(tid=1, kind=Event.SECCOMP, nr=NR_FUTEX))


def test_set_robust_list_sigsys_emulated_not_a_deadlock():
    # set_robust_list(99) IS blocked -> SIGSYS -> emulate -ENOSYS -> CONT. glibc
    # ignores the result. This must resume, never park (the R4 alt-explanation
    # "robust-futex init fails -> pthread_create hangs" is a GUEST consequence,
    # not a supervisor park; the supervisor's own action is a clean resume).
    sup = MtSupervisorModel(leader_pid=1)
    res = sup.step(TraceEvent(tid=1, kind=Event.SIGSYS_SECCOMP, nr=NR_SET_ROBUST_LIST))
    assert res.action == Action.EMULATE_CONT
    assert sup.emulated_syscalls == 1
    assert sup.emul_hist[NR_SET_ROBUST_LIST] == 1
    assert sup.parked_tids() == []


# --------------------------------------------------------------------------- #
# SIGSYS-EMULATE STORM (candidate d) — the runaway backstop SIGKILLs at >1<<20.
# --------------------------------------------------------------------------- #

def test_emul_runaway_backstop_sigkills():
    sup = MtSupervisorModel(leader_pid=1, emul_backstop=4)  # tiny cap for the test
    out = [
        sup.step(TraceEvent(tid=1, kind=Event.SIGSYS_SECCOMP, nr=NR_SET_ROBUST_LIST))
        for _ in range(6)
    ]
    # First 4 emulate; the 5th trips > cap and SIGKILLs.
    actions = [r.action for r in out]
    assert actions[:4] == [Action.EMULATE_CONT] * 4
    assert Action.SIGKILL in actions
    assert sup.state[1] == TidState.DEAD


def test_default_backstop_is_one_meg():
    assert EMUL_RUNAWAY_BACKSTOP == (1 << 20)


# --------------------------------------------------------------------------- #
# BAD-SYSCALL SIGSYS is delivered (guest crashes visibly), not silently faked.
# --------------------------------------------------------------------------- #

def test_bad_sigsys_delivered():
    sup = MtSupervisorModel(leader_pid=1)
    res = sup.step(TraceEvent(tid=1, kind=Event.SIGSYS_BAD, nr=999))
    assert res.action == Action.CONT_DELIVER
    assert sup.emulated_syscalls == 0  # NOT counted as an emulation


# --------------------------------------------------------------------------- #
# GROUP-STOP SIGNAL SUPPRESSION (default branch L2926-2929) — SIGSTOP/TSTP/TTIN/
# TTOU are never forwarded (would re-stop the group -> sibling futex deadlock).
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("sig", ["SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU"])
def test_group_stop_signals_suppressed(sig):
    sup = MtSupervisorModel(leader_pid=1)
    res = sup.step(TraceEvent(tid=1, kind=Event.SIGNAL, signal=sig))
    assert res.action == Action.CONT  # suppressed -> CONT signal 0


@pytest.mark.parametrize("sig", ["SIGUSR1", "SIGCHLD", "SIGPIPE"])
def test_non_group_signals_delivered(sig):
    sup = MtSupervisorModel(leader_pid=1)
    res = sup.step(TraceEvent(tid=1, kind=Event.SIGNAL, signal=sig))
    assert res.action == Action.CONT_DELIVER


# --------------------------------------------------------------------------- #
# SEIZE-BRANCH RACE ANALYSIS (R1 weakness-1): prove LISTEN is reachable ONLY from
# (known tid, GETSIGINFO==einval) — a NEW tid can NEVER be parked. This is the
# host-side refutation of "the v122-124 SEIZE switch induced a NEW group-stop
# deadlock by LISTEN-parking a fresh worker".
# --------------------------------------------------------------------------- #

def test_seize_branch_new_tid_never_parks_any_siginfo():
    for res in ("einval", "esrch", "ok"):
        action = seize_event_stop_decision(
            SeizeBranchInput(tid_already_known=False, getsiginfo_result=res)
        )
        assert action == Action.CONT, f"new tid with {res} must CONT, not park"


def test_seize_branch_listen_only_from_known_einval():
    listen_cases = []
    for known in (True, False):
        for res in ("einval", "esrch", "ok"):
            a = seize_event_stop_decision(
                SeizeBranchInput(tid_already_known=known, getsiginfo_result=res)
            )
            if a == Action.LISTEN:
                listen_cases.append((known, res))
    # Exactly ONE of the six combinations parks: (known=True, einval).
    assert listen_cases == [(True, "einval")]


def test_seize_branch_esrch_is_benign_noop():
    a = seize_event_stop_decision(
        SeizeBranchInput(tid_already_known=True, getsiginfo_result="esrch")
    )
    assert a == Action.NOOP_ESRCH


# --------------------------------------------------------------------------- #
# DEADLOCK CANDIDATE CLASSIFIER (R1) — the host decision the device probe is
# judged against. Each candidate must be uniquely produced by its signature.
# --------------------------------------------------------------------------- #

def test_classify_single_init_slow_when_no_clone():
    # The R1 1st hypothesis: clone never reached, guest stuck at Threads=1.
    inp = TriageInput(
        clone_reached=False, max_threads=1, parked_tids=0,
        stuck_tid_last_nr=NR_MMAP, emulated_syscalls=3, window_s=25.0,
    )
    assert classify_deadlock(inp) == DeadlockCandidate.SINGLE_INIT_SLOW


def test_classify_futex_park_when_worker_blocked_on_futex():
    inp = TriageInput(
        clone_reached=True, max_threads=8, parked_tids=0,
        stuck_tid_last_nr=NR_FUTEX, emulated_syscalls=10, window_s=180.0,
    )
    assert classify_deadlock(inp) == DeadlockCandidate.FUTEX_OR_PARK


def test_classify_futex_park_when_a_tid_is_listen_parked():
    inp = TriageInput(
        clone_reached=True, max_threads=8, parked_tids=1,
        stuck_tid_last_nr=NR_MMAP, emulated_syscalls=10, window_s=180.0,
    )
    assert classify_deadlock(inp) == DeadlockCandidate.FUTEX_OR_PARK


def test_classify_sigsys_emul_storm_at_backstop():
    inp = TriageInput(
        clone_reached=True, max_threads=4, parked_tids=0,
        stuck_tid_last_nr=NR_SET_ROBUST_LIST,
        emulated_syscalls=EMUL_RUNAWAY_BACKSTOP, window_s=25.0,
    )
    assert classify_deadlock(inp) == DeadlockCandidate.SIGSYS_EMUL_STORM


def test_classify_clone_trap_wedge_default():
    # clone reached, no park, not futex, not emul storm -> (b).
    inp = TriageInput(
        clone_reached=True, max_threads=5, parked_tids=0,
        stuck_tid_last_nr=56, emulated_syscalls=2, window_s=180.0,
    )
    assert classify_deadlock(inp) == DeadlockCandidate.CLONE_TRAP_WEDGE


def test_classify_window_too_short_distinguished_from_real_block():
    # The decisive R1 split (device gate (a)/(b)): with a LONGER window, if clone
    # IS eventually reached the verdict flips OFF single-init-slow. The classifier
    # encodes that "clone_reached" — not the window length — is the discriminator,
    # so the device probe's alarm-extension experiment is what flips it.
    short = TriageInput(
        clone_reached=False, max_threads=1, parked_tids=0,
        stuck_tid_last_nr=NR_MMAP, emulated_syscalls=1, window_s=25.0,
    )
    extended = TriageInput(
        clone_reached=True, max_threads=4, parked_tids=0,
        stuck_tid_last_nr=NR_MMAP, emulated_syscalls=1, window_s=180.0,
    )
    assert classify_deadlock(short) == DeadlockCandidate.SINGLE_INIT_SLOW
    assert classify_deadlock(extended) != DeadlockCandidate.SINGLE_INIT_SLOW


# --------------------------------------------------------------------------- #
# END-TO-END: a faithful "chromium-like" bring-up that reaches multi-thread and
# exits cleanly — proving the supervisor LOGIC supports the multi-thread render
# path (so any device hang is NOT in this classify logic).
# --------------------------------------------------------------------------- #

def test_endtoend_multithread_bringup_clean_exit():
    sup = MtSupervisorModel(leader_pid=1)
    ev = [
        # single-thread init: a few path traps + a couple blocked-nr emulations.
        TraceEvent(tid=1, kind=Event.SECCOMP, nr=56),   # openat
        TraceEvent(tid=1, kind=Event.SECCOMP, nr=79),   # newfstatat
        TraceEvent(tid=1, kind=Event.SIGSYS_SECCOMP, nr=NR_SET_ROBUST_LIST),
    ]
    # fan out 22 workers, each issues an openat.
    for i in range(22):
        c = 200 + i
        ev += [
            TraceEvent(tid=1, kind=Event.CLONE, new_child_tid=c),
            TraceEvent(tid=c, kind=Event.NEW_TID_STOP),
            TraceEvent(tid=c, kind=Event.SECCOMP, nr=56),
        ]
    # workers exit, then leader exits 0.
    for i in range(22):
        ev.append(TraceEvent(tid=200 + i, kind=Event.EXIT))
    ev.append(TraceEvent(tid=1, kind=Event.EXIT))
    sup.run(ev)
    assert sup.guest_threads == 22
    assert sup.path_traps == 2 + 22  # 2 init + 22 worker openats
    assert sup.emulated_syscalls == 1
    assert sup.parked_tids() == []
    assert sup.state[1] == TidState.DEAD
    # No worker was ever LISTEN-parked: the multi-thread path is logically clean.
    assert all(
        r.action != Action.LISTEN for r in sup.history
    )


# --------------------------------------------------------------------------- #
# HONESTY GUARD — the module must state its host limitation in the docstring so
# nobody reads a green bar as "the device deadlock is fixed".
# --------------------------------------------------------------------------- #

def test_module_states_host_limitation():
    import tests.mt_supervisor_model as mod
    doc = mod.__doc__ or ""
    assert "DEVICE-ONLY" in doc
    assert "darwin" in doc
    # It must NOT overclaim: explicitly says proving the branch logic does not
    # prove the device cannot hang. (Collapse whitespace so the line-wrapped
    # docstring still matches the phrase.)
    collapsed = " ".join(doc.split())
    assert "does NOT prove the device does not hang" in collapsed
