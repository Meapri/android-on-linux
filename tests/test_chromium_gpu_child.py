"""Chromium GPU-child ring-binding unit tests — the `exit_code=5` decision spec.

Host-side regression of tools/chromium_gpu_child_model — *why* a chromium GPU
child gets / does not get the ALR Mali ring, the structural root behind the
device GPU-child `exit_code=5`. Pins:

  docs/research/chromium-gpu-child-plan.md §1.2, §1.3, §2 (H1), §3.A, §6

NO device, NO ptrace, NO Mali — only the env-propagation decision (the
ring-attach EFFECT, the real GpuMain exit code, ANGLE dlopen and mojo are
DEVICE-ONLY: DEVICE-REQ ALR-CR3-diag-shim / -stepA / -stepB). These tests turn
red on the host if a future patch claims chromium gets the ring without either
generalizing the glmark2 attach gate (§3.A-1) or threading ring env through B-3
(§3.A-2) / using --in-process-gpu (§3.A-3).
"""
from __future__ import annotations

import pytest

from tools.chromium_gpu_child_model import (
    ENV_RING_BYTES,
    ENV_RING_DOORBELL_FD,
    ENV_RING_FD,
    H1_RINGLESS,
    H_NO_GPU_CHILD,
    H_NONE,
    REASON_FIX_PROPAGATED,
    REASON_FRESH_CHILD_NO_PROPAGATE,
    REASON_IN_PROCESS_GPU,
    REASON_NOT_ACCEL,
    child_requests_accel,
    parent_attaches_ring,
    plan_child_env_injection,
    predict_gpu_child_ring,
)

CHROME = "/usr/lib/chromium/chromium-headless-shell"
GLMARK2 = "/usr/bin/glmark2-es2-wayland"

# The device d106c30 MP probe argv (software, --disable-gpu).
CRMP_ARGV = [
    CHROME, "--no-zygote", "--renderer-process-limit=1", "--no-sandbox",
    "--disable-gpu", "--disable-dev-shm-usage", "--user-data-dir=/tmp/crmp",
    "--dump-dom", "data:text/html,<h1>ALR-CRMP-OK</h1>",
]
GPU_CHILD_ARGV_SW = [CHROME, "--type=gpu-process", "--disable-gpu"]
GPU_CHILD_ARGV_ANGLE = [CHROME, "--type=gpu-process", "--use-gl=angle",
                        "--use-angle=gles-egl"]
GPU_CHILD_ARGV_INPROC = [CHROME, "--in-process-gpu", "--use-gl=angle",
                         "--use-angle=gles-egl"]


# ---------------------------------------------------------------------------
# child_requests_accel — only ANGLE gles-egl asks for the Mali shim path
# ---------------------------------------------------------------------------
def test_accel_request_angle_gles_egl():
    assert child_requests_accel(GPU_CHILD_ARGV_ANGLE) is True


def test_accel_request_use_gl_angle_alone():
    assert child_requests_accel([CHROME, "--use-gl=angle"]) is True


def test_disable_gpu_alone_is_not_accel():
    # --disable-gpu requests SOFTWARE — not the Mali shim path.
    assert child_requests_accel(GPU_CHILD_ARGV_SW) is False
    assert child_requests_accel(CRMP_ARGV) is False


def test_no_gl_flags_is_not_accel():
    assert child_requests_accel([CHROME, "--type=gpu-process"]) is False


# ---------------------------------------------------------------------------
# parent_attaches_ring — the glmark2 gate (current) vs the §3.A-1 generalization
# ---------------------------------------------------------------------------
def test_parent_ring_glmark2_current():
    # runtime_report.cpp L1731: glmark2 substring -> attach. [코드-실증]
    assert parent_attaches_ring(GLMARK2, [GLMARK2], fix_accel_gate=False) is True


def test_parent_ring_chromium_current_none():
    # chromium is NOT glmark2 -> NO ring on the parent, today.
    assert parent_attaches_ring(CHROME, CRMP_ARGV, fix_accel_gate=False) is False


def test_parent_ring_chromium_accel_fix_on():
    # §3.A-1: generalize the gate -> accel-requesting chromium attaches a ring.
    assert parent_attaches_ring(
        CHROME, GPU_CHILD_ARGV_ANGLE, fix_accel_gate=True
    ) is True


def test_parent_ring_chromium_sw_fix_on_still_none():
    # Even with the fix, a software (--disable-gpu, no angle) launch needs no ring.
    assert parent_attaches_ring(CHROME, CRMP_ARGV, fix_accel_gate=True) is False


# ---------------------------------------------------------------------------
# predict_gpu_child_ring — the four cases of chromium-gpu-child-plan §3.A
# ---------------------------------------------------------------------------
def test_current_chromium_gpu_child_is_ringless_H1():
    """CURRENT reality (d106c30): chromium GPU child gets NO ring -> H1.

    Structural root behind exit_code=5: parent never attached a ring (not
    glmark2), so the fresh GPU child env has no ALR_GPU_RING_FD -> shim ring-less
    -> GLES/EGL init fails in GpuMain. (§1.3 + §2 H1) [코드-실증 for ring absence]
    """
    rb = predict_gpu_child_ring(CHROME, GPU_CHILD_ARGV_SW, launch_argv=CRMP_ARGV)
    assert rb.attached is False
    assert rb.parent_ring_attached is False
    assert rb.reason == REASON_NOT_ACCEL
    assert rb.primary_hypothesis == H1_RINGLESS
    assert rb.has_separate_gpu_child is True


def test_glmark2_is_ring_attached_as_launch_guest():
    """glmark2 is the ONE program that gets the ring today (the CP-2 path).

    glmark2 is NOT a separate fresh-execve GPU child — it is the LAUNCH GUEST
    itself (single-process in-process-jump), so it gets ALR_GPU_RING_* directly
    on its own env (no B-3 hop). is_fresh_child=False models that.
    """
    rb = predict_gpu_child_ring(
        GLMARK2, [GLMARK2], launch_argv=[GLMARK2], is_fresh_child=False
    )
    assert rb.attached is True
    assert rb.has_separate_gpu_child is False
    assert rb.primary_hypothesis == H_NONE


def test_accel_gate_alone_insufficient_without_b3_propagation():
    """§3.A-1 WITHOUT §3.A-2: parent gets a ring but the FRESH GPU child still

    cannot see ALR_GPU_RING_* (B-3 injects only LD_PRELOAD/ALR_ROOTFS, §1.2) ->
    still ring-less. This is the precise trap the plan calls out: attaching a
    parent ring is necessary but NOT sufficient for an out-of-process GPU child.
    """
    rb = predict_gpu_child_ring(
        CHROME, GPU_CHILD_ARGV_ANGLE, launch_argv=GPU_CHILD_ARGV_ANGLE,
        fix_accel_gate=True, fix_b3_propagates_ring=False,
    )
    assert rb.parent_ring_attached is True
    assert rb.attached is False
    assert rb.reason == REASON_FRESH_CHILD_NO_PROPAGATE
    assert rb.primary_hypothesis == H1_RINGLESS


def test_full_fix_out_of_process_gpu_child_attaches():
    """§3.A-1 + §3.A-2: parent attaches a ring AND B-3 propagates ring env into

    the fresh GPU child -> the child's shim binds the ring -> H1 ruled out
    (CR-3 stepB target).
    """
    rb = predict_gpu_child_ring(
        CHROME, GPU_CHILD_ARGV_ANGLE, launch_argv=GPU_CHILD_ARGV_ANGLE,
        fix_accel_gate=True, fix_b3_propagates_ring=True,
    )
    assert rb.attached is True
    assert rb.reason == REASON_FIX_PROPAGATED
    assert rb.primary_hypothesis == H_NONE
    assert rb.has_separate_gpu_child is True


def test_in_process_gpu_removes_child_and_uses_parent_ring():
    """§3.A-3: --in-process-gpu -> no separate GPU child; the GPU runs in the

    browser (parent) the loader launched directly, so the parent ring (§3.A-1)
    is used in-process. This is the fastest accel path (CR-3 stepA) and sidesteps
    fresh-child env propagation + the single-ring multi-renderer interleave
    (riskA-3) entirely.
    """
    rb = predict_gpu_child_ring(
        CHROME, GPU_CHILD_ARGV_INPROC, launch_argv=GPU_CHILD_ARGV_INPROC,
        fix_accel_gate=True,
    )
    assert rb.has_separate_gpu_child is False
    assert rb.attached is True            # parent ring used in-process
    assert rb.reason == REASON_IN_PROCESS_GPU
    assert rb.primary_hypothesis == H_NO_GPU_CHILD


def test_in_process_gpu_without_parent_ring_is_ringless():
    """--in-process-gpu but NO parent ring (fix off) -> the in-process GPU is

    still ring-less (no accel). Confirms --in-process-gpu alone doesn't conjure a
    ring; §3.A-1 (parent attach) is the precondition.
    """
    rb = predict_gpu_child_ring(
        CHROME, GPU_CHILD_ARGV_INPROC, launch_argv=GPU_CHILD_ARGV_INPROC,
        fix_accel_gate=False,
    )
    assert rb.has_separate_gpu_child is False
    assert rb.attached is False
    assert rb.primary_hypothesis == H1_RINGLESS


# ---------------------------------------------------------------------------
# plan_child_env_injection — the §3.A-2 B-3 delta (set logic)
# ---------------------------------------------------------------------------
def test_b3_injection_adds_ring_keys_when_absent():
    """Fresh child env lacks ring keys; parent has a ring; fix on -> add all 3."""
    plan = plan_child_env_injection(
        ["LD_PRELOAD", "ALR_ROOTFS", "PATH", "HOME"],
        parent_ring_attached=True, fix_b3_propagates_ring=True,
    )
    assert set(plan.add_keys) == {ENV_RING_FD, ENV_RING_BYTES, ENV_RING_DOORBELL_FD}
    assert plan.carries_ring is True


def test_b3_injection_noop_when_fix_off():
    plan = plan_child_env_injection(
        ["LD_PRELOAD"], parent_ring_attached=True, fix_b3_propagates_ring=False,
    )
    assert plan.add_keys == ()
    assert plan.carries_ring is False


def test_b3_injection_noop_when_no_parent_ring():
    # Nothing to propagate if the parent never attached a ring.
    plan = plan_child_env_injection(
        ["LD_PRELOAD"], parent_ring_attached=False, fix_b3_propagates_ring=True,
    )
    assert plan.add_keys == ()
    assert plan.carries_ring is False


def test_b3_injection_idempotent_when_child_already_has_ring_fd():
    """A child whose parent already injected ALR_GPU_RING_FD must not re-add it

    (idempotent — matches B-3's existing 'already satisfied -> no-op' contract).
    """
    plan = plan_child_env_injection(
        [ENV_RING_FD, ENV_RING_BYTES, ENV_RING_DOORBELL_FD, "LD_PRELOAD"],
        parent_ring_attached=True, fix_b3_propagates_ring=True,
    )
    assert plan.add_keys == ()        # all present -> nothing added
    assert plan.carries_ring is True  # ring is carried (already in env)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-q"]))
