"""Host regression tests for tools/supervisor_model.py (CR-1 deadlock repro).

These tests REPRODUCE — on darwin, with no kernel — the device-measured chromium
``--dump-dom`` ptrace-supervisor deadlock (evidence:
docs/evidence/2026-06-02-cr1-chromium-supervisor-deadlock.md) as a failing
liveness assertion, PINPOINT the offending supervisor transition, and prove a
candidate FIX clears it. They are the host-verifiable regression guard the device
fix (WS-1, runtime_report.cpp) is judged against.

HONESTY: a green bar proves the supervisor's RESUME POLICY admits a permanently
un-resumed tracee (the deadlock) and that the candidate policy closes it IN THE
MODEL. It does NOT by itself prove the real kernel no longer hangs — that needs a
device drain (DEVICE-REQ). See tools/supervisor_model.py module docstring.

Run with:
  uvx --with pytest pytest tests/test_supervisor_deadlock_model.py -q
"""
from __future__ import annotations

import pytest

from tools.supervisor_model import (
    NR_FUTEX,
    NR_OPENAT,
    PATH_SYSCALL_NRS,
    Ev,
    EvKind,
    PtraceOp,
    RunState,
    SupervisorLivenessError,
    SupervisorModel,
    chromium_storm_events,
)


# --------------------------------------------------------------------------- #
# Fact pins — the nr sets MUST match tests/mt_supervisor_model.py + PCGATE.
# A silent drift from runtime_report.cpp's 9 path nrs is a bug.
# --------------------------------------------------------------------------- #

def test_path_nrs_match_other_model_and_pcgate():
    from tests.mt_supervisor_model import PATH_SYSCALL_NRS as OTHER
    assert PATH_SYSCALL_NRS == OTHER
    assert PATH_SYSCALL_NRS == frozenset({34, 35, 48, 56, 78, 79, 291, 437, 439})


def test_futex_is_non_path_so_invisible_to_supervisor():
    # The entire deadlock rests on this: futex is RET_ALLOW (no trap, no event),
    # so a thread blocked in futex_wait is INVISIBLE to the supervisor — the
    # supervisor cannot itself resolve it; only a sibling running can.
    assert NR_FUTEX not in PATH_SYSCALL_NRS


# --------------------------------------------------------------------------- #
# THE DEADLOCK REPRO (apply_fix=False == current device policy).
# The chromium clone storm + a worker group-stop with a futex-waiting sibling +
# the group-stop END re-report must wedge the single-threaded waitpid(-1) loop.
# --------------------------------------------------------------------------- #

def test_chromium_storm_deadlocks_under_current_policy():
    sup = SupervisorModel(leader_pid=1000, apply_fix=False)
    sup.run(chromium_storm_events(leader_pid=1000, workers=20))
    # The 18 non-pair workers reaped; the leader is still alive; the group-stopped
    # worker is re-LISTEN'd forever and its futex sibling never woke.
    assert sup.guest_threads == 20
    assert not sup.leader_exited
    assert sup.is_deadlocked() is True
    # assert_drains must RAISE — this is the host repro of the device hang.
    with pytest.raises(SupervisorLivenessError, match="block forever"):
        sup.assert_drains()


def test_deadlock_pins_the_offending_transition():
    # Pinpoint: the LAST decision before the wedge is a group-stop-END EVENT_STOP
    # that the current policy re-LISTENs instead of resuming. WS-1 fixes THIS.
    sup = SupervisorModel(leader_pid=1000, apply_fix=False)
    sup.run(chromium_storm_events(leader_pid=1000, workers=20))
    relisten = [
        s for s in sup.history
        if s.op == PtraceOp.LISTEN and "END misclassified" in s.note
    ]
    assert len(relisten) == 1, "exactly one group-stop-end re-LISTEN is the bug"
    bad = relisten[0]
    assert bad.state_after == RunState.LISTENING
    assert "DEADLOCK transition" in bad.note
    # The re-LISTEN'd worker stays parked; the sibling stays futex-blocked on it.
    gs_tid = bad.tid
    assert sup.state[gs_tid] == RunState.LISTENING
    assert sup.futex_block  # at least one waiter still blocked on the parked tid
    assert all(
        sup.state[w] == RunState.FUTEX_WAIT for w in sup.futex_block
    )


# --------------------------------------------------------------------------- #
# THE FIX CLEARS IT (apply_fix=True == candidate resume-on-group-stop-end policy).
# The identical scenario must now drain: the parked worker resumes on its SIGCONT
# re-report, wakes its futex sibling, and the run can reach a clean leader exit.
# --------------------------------------------------------------------------- #

def test_chromium_storm_drains_under_candidate_fix():
    sup = SupervisorModel(leader_pid=1000, apply_fix=True)
    sup.run(chromium_storm_events(leader_pid=1000, workers=20))
    # The group-stopped worker resumed (RUNNING), its futex sibling was woken.
    assert sup.guest_threads == 20
    assert sup.is_deadlocked() is False
    # No tracee remains LISTENING; the futex sibling is no longer blocked.
    assert sup.listening_tids() == []
    assert sup.futex_waiting_tids() == []
    assert sup.futex_block == {}
    # assert_drains must NOT raise.
    sup.assert_drains()


def test_fix_resumes_the_parked_worker_on_group_stop_end():
    sup = SupervisorModel(leader_pid=1000, apply_fix=True)
    sup.run(chromium_storm_events(leader_pid=1000, workers=20))
    resumes = [
        s for s in sup.history
        if s.op == PtraceOp.CONT and "group-stop END -> CONT" in s.note
    ]
    assert len(resumes) == 1
    assert resumes[0].state_after == RunState.RUNNING
    # No group-stop-end was ever re-LISTEN'd under the fix.
    assert not any(
        "END misclassified" in s.note for s in sup.history
    )


def test_fix_then_leader_can_exit_cleanly():
    # Drive the full deadlock-pair through to exit under the fix and confirm the
    # leader reaches DEAD (waitpid(-1) returns ECHILD -> loop ends -> guest reaped).
    sup = SupervisorModel(leader_pid=1000, apply_fix=True)
    ev = chromium_storm_events(leader_pid=1000, workers=20)
    sup.run(ev)
    # the deadlock pair (workers 7 and 8) now exit, then the leader.
    base = 1001
    sup.run([
        Ev(tid=base + 8, kind=EvKind.EXIT),  # futex sibling (woken) exits
        Ev(tid=base + 7, kind=EvKind.EXIT),  # the formerly-parked worker exits
        Ev(tid=1000, kind=EvKind.EXIT),      # leader exits
    ])
    assert sup.leader_exited is True
    assert sup.state[1000] == RunState.DEAD
    assert sup.is_deadlocked() is False


# --------------------------------------------------------------------------- #
# ISOLATING THE TRANSITION — a single group-stop BEGIN then END, no storm. Proves
# the bug is purely the group-stop-end resume policy, not the clone fan-out.
# --------------------------------------------------------------------------- #

def test_minimal_group_stop_cycle_current_policy_wedges():
    sup = SupervisorModel(leader_pid=1, apply_fix=False)
    sup.run([
        Ev(tid=1, kind=EvKind.CLONE, new_child_tid=2),
        Ev(tid=2, kind=EvKind.NEW_TID_STOP),
        Ev(tid=1, kind=EvKind.CLONE, new_child_tid=3),
        Ev(tid=3, kind=EvKind.NEW_TID_STOP),
        # tid 2 group-stops; tid 3 futex-waits on it.
        Ev(tid=2, kind=EvKind.GROUP_STOP_BEGIN, futex_waiters=(3,)),
        Ev(tid=2, kind=EvKind.GROUP_STOP_END),
    ])
    assert sup.state[2] == RunState.LISTENING   # re-parked
    assert sup.state[3] == RunState.FUTEX_WAIT   # sibling stuck
    with pytest.raises(SupervisorLivenessError):
        sup.assert_drains()


def test_minimal_group_stop_cycle_fix_resumes():
    sup = SupervisorModel(leader_pid=1, apply_fix=True)
    sup.run([
        Ev(tid=1, kind=EvKind.CLONE, new_child_tid=2),
        Ev(tid=2, kind=EvKind.NEW_TID_STOP),
        Ev(tid=1, kind=EvKind.CLONE, new_child_tid=3),
        Ev(tid=3, kind=EvKind.NEW_TID_STOP),
        Ev(tid=2, kind=EvKind.GROUP_STOP_BEGIN, futex_waiters=(3,)),
        Ev(tid=2, kind=EvKind.GROUP_STOP_END),
    ])
    assert sup.state[2] == RunState.RUNNING      # resumed
    assert sup.state[3] == RunState.RUNNING      # sibling woken
    sup.assert_drains()


# --------------------------------------------------------------------------- #
# SANITY: a group-stop BEGIN alone (no END event yet) is legitimately parked and
# is NOT yet a deadlock if its sibling is still progressing — guards against the
# detector over-firing on a transient, correctly-handled LISTEN.
# --------------------------------------------------------------------------- #

def test_group_stop_begin_without_futex_sibling_is_not_a_deadlock():
    # A worker group-stops but NO sibling futex-waits on it, and it later resumes.
    sup = SupervisorModel(leader_pid=1, apply_fix=True)
    sup.run([
        Ev(tid=1, kind=EvKind.CLONE, new_child_tid=2),
        Ev(tid=2, kind=EvKind.NEW_TID_STOP),
        Ev(tid=2, kind=EvKind.GROUP_STOP_BEGIN),  # no futex_waiters
        Ev(tid=2, kind=EvKind.GROUP_STOP_END),
    ])
    assert sup.state[2] == RunState.RUNNING
    sup.assert_drains()


def test_no_clone_no_group_stop_is_trivially_live():
    # The chromium --version path: few threads, no group-stop. Never deadlocks.
    sup = SupervisorModel(leader_pid=1, apply_fix=False)
    sup.run([
        Ev(tid=1, kind=EvKind.SECCOMP, nr=NR_OPENAT),
        Ev(tid=1, kind=EvKind.SECCOMP, nr=79),
        Ev(tid=1, kind=EvKind.EXIT),
    ])
    assert sup.leader_exited is True
    assert sup.is_deadlocked() is False
    sup.assert_drains()


# --------------------------------------------------------------------------- #
# DETECTOR INTEGRITY — is_deadlocked() must NOT fire while the leader is alive but
# a futex waiter's target is RUNNING (progress is still possible), and MUST fire
# once that target is permanently parked.
# --------------------------------------------------------------------------- #

def test_detector_not_fired_when_futex_target_is_running():
    sup = SupervisorModel(leader_pid=1, apply_fix=False)
    # tid 3 futex-waits on tid 2, but tid 2 is RUNNING -> resolvable -> not wedged.
    sup.state[2] = RunState.RUNNING
    sup.state[3] = RunState.FUTEX_WAIT
    sup.futex_block[3] = 2
    assert sup.is_deadlocked() is False


def test_detector_fires_when_futex_target_permanently_parked():
    sup = SupervisorModel(leader_pid=1, apply_fix=False)
    sup.state[2] = RunState.LISTENING       # parked forever (current policy)
    sup.state[3] = RunState.FUTEX_WAIT
    sup.futex_block[3] = 2
    assert sup.is_deadlocked() is True


# --------------------------------------------------------------------------- #
# HONESTY GUARD — the module must state its host limitation so nobody reads a
# green bar as "the device deadlock is fixed".
# --------------------------------------------------------------------------- #

def test_module_states_host_limitation():
    import tools.supervisor_model as mod
    doc = mod.__doc__ or ""
    collapsed = " ".join(doc.split())
    assert "darwin runs no real seccomp" in collapsed
    assert "does NOT by itself prove the device no longer hangs" in collapsed
    # It must also name the candidate fix WS-1 is to implement.
    assert "CANDIDATE FIX" in doc
