"""Chromium GPU-child ring-binding decision model (host-testable decision spec).

Pure, host-testable (NO device, NO I/O, NO ptrace, NO Mali). This is the
executable decision spec for *why a chromium GPU child gets / does not get* the
ALR Mali GPU ring, the question behind the device `exit_code=5` of the GPU child:

  docs/research/chromium-gpu-child-plan.md  §1.2, §1.3, §2 (H1), §3.A, §6

WHAT THIS MODELS
----------------
A fresh-execve chromium child (`--type=gpu-process` etc.) is re-mapped in-process
by GATE-1 (runtime_report.cpp, read-only here). Its GLES/EGL shim
(alr_gpu/guest_shim) attaches to the host GPU ring ONLY if `ALR_GPU_RING_FD`
(+BYTES) is present in the child's environment. This module computes whether that
env reaches the child, under both:

  * CURRENT code (the device `d106c30` reality), and
  * the PROPOSED fix (chromium-gpu-child-plan §3.A-1/§3.A-2/§3.A-3).

GROUND TRUTH READ FROM THE BODY (NOT EDITED)
--------------------------------------------
  1. runtime_report.cpp L1729-1740: the ONLY site that attaches the ring +
     pushes ALR_GPU_RING_* into the parent env is gated on
     `config.program.find("glmark2") != npos`. chromium is NOT glmark2 → no ring,
     no executor, no ring env on the parent.                       [코드-실증]
  2. The fresh GPU child reads envp from x21 = a snapshot of the parent's execve
     envp, then B-3 (decide_exec_envp_injection, alr_exec.cpp L450/453) augments
     it with ONLY `LD_PRELOAD` + `ALR_ROOTFS`. ALR_GPU_RING_* is never injected.
                                                                    [코드-실증]
  3. The EGL shim opens NO /dev/dri // /dev/mali (eglGetDisplay returns a
     sentinel, alr_egl_shim.c L55) — device-node absence is NOT the shim's
     blocker. The shim goes ring-less iff ALR_GPU_RING_FD is absent
     (alr_shim_runtime.c L81-105), emitting
     "[alr-shim] ... running ring-less (no GPU)".                   [코드-실증]

So with CURRENT code a chromium GPU child is ALWAYS ring-less → primary
hypothesis H1 (ring-less shim → GLES/EGL init fails in GpuMain). The fix is to
make the parent attach a ring for accel-requesting chromium (§3.A-1) AND thread
ALR_GPU_RING_* through B-3 into the fresh child (§3.A-2) — OR drop the separate
GPU child entirely with `--in-process-gpu` (§3.A-3), so the ring lives in the
parent (browser) the loader launched directly.

darwin host cannot drive real seccomp/ptrace/Mali — only this *decision* lives
here. The ring-attach EFFECT, the actual GpuMain exit code, mojo, and ANGLE
dlopen are DEVICE-ONLY (DEVICE-REQ: ALR-CR3-diag-shim / -stepA / -stepB).
"""
from __future__ import annotations

from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Ring env keys — MUST stay byte-identical to alr_gpu/guest_shim/alr_shim_env.h
# (the guest shim reads these) and to alr_gpu_ring_hook.hpp gpu_ring_guest_env().
# ---------------------------------------------------------------------------
ENV_RING_FD = "ALR_GPU_RING_FD"
ENV_RING_BYTES = "ALR_GPU_RING_BYTES"
ENV_RING_DOORBELL_FD = "ALR_GPU_RING_DOORBELL_FD"

# The shim attaches iff BOTH of these are present and well-formed
# (alr_shim_runtime.c shim_init_once: ring_fd >= 0 && ring_bytes > 0).
RING_REQUIRED_KEYS = (ENV_RING_FD, ENV_RING_BYTES)

# B-3 (decide_exec_envp_injection) injects ONLY these into a fresh child today.
B3_INJECTED_KEYS_TODAY = ("LD_PRELOAD", "ALR_ROOTFS")

# Reasons (string vocabulary shared with the plan doc + a future device log).
REASON_PARENT_GLMARK2 = "parent-glmark2-ring"        # §1.3 current attach gate
REASON_NOT_ACCEL = "not-accel-no-ring"               # chromium, no accel request -> no parent ring
REASON_FRESH_CHILD_NO_PROPAGATE = "fresh-child-env-not-propagated"  # §1.2: B-3 drops ring env
REASON_IN_PROCESS_GPU = "in-process-gpu-no-child"    # §3.A-3: no separate GPU child at all
REASON_FIX_PROPAGATED = "fix-ring-env-propagated"    # §3.A-2 applied: B-3 carries ring env

# Primary `exit_code=5` hypothesis labels (chromium-gpu-child-plan §2).
H1_RINGLESS = "H1-ringless-gles-init-fail"
H_NONE = "none-ring-attached"                        # ring present -> H1 ruled out (other H may apply)
H_NO_GPU_CHILD = "no-gpu-child"                      # --in-process-gpu: no child to crash


def _is_glmark2(program: str) -> bool:
    """Mirror runtime_report.cpp L1731 `program.find("glmark2") != npos`."""
    return "glmark2" in (program or "")


def _has_flag(argv, *needles: str) -> bool:
    """True if any argv token equals or starts with any needle (flag match)."""
    for tok in argv or ():
        for n in needles:
            if tok == n or tok.startswith(n):
                return True
    return False


def child_requests_accel(child_argv) -> bool:
    """The GPU child 'wants' our Mali path iff it selects ANGLE GLES-EGL.

    chromium-gpu-child-plan §3.B: the shim is reached when chromium runs
    `--use-gl=angle --use-angle=gles-egl`. `--disable-gpu` alone does NOT request
    accel (it asks for software). This is the signal §3.A-1 generalizes the
    glmark2 gate to.
    """
    if _has_flag(child_argv, "--disable-gpu"):
        # --disable-gpu can coexist with a forced --use-gl in odd cmdlines; honor
        # an explicit gles-egl request over a bare --disable-gpu.
        return _has_flag(child_argv, "--use-angle=gles-egl") or _has_flag(
            child_argv, "--use-gl=angle"
        )
    return _has_flag(child_argv, "--use-angle=gles-egl") or _has_flag(
        child_argv, "--use-gl=angle"
    )


@dataclass(frozen=True)
class RingBinding:
    """Whether a chromium child's GLES/EGL shim binds the host Mali ring.

    attached            — True iff the shim will find ALR_GPU_RING_FD(+BYTES) in
                          the child env and mmap a valid ring (modeled; the ALRG
                          magic check itself is device-only).
    reason              — one of the REASON_* strings (decision provenance).
    primary_hypothesis  — the exit_code=5 hypothesis this binding implies:
                          H1_RINGLESS when ring-less, H_NONE when attached (H1
                          ruled out; H2..H5 may still apply but are device-only),
                          H_NO_GPU_CHILD when --in-process-gpu removes the child.
    has_separate_gpu_child — False under --in-process-gpu (no fresh GPU child).
    parent_ring_attached   — whether the PARENT (launch) process got a ring at all
                          (the precondition for any child propagation).
    """

    attached: bool
    reason: str
    primary_hypothesis: str
    has_separate_gpu_child: bool = True
    parent_ring_attached: bool = False


def parent_attaches_ring(launch_program: str, launch_argv, *, fix_accel_gate: bool) -> bool:
    """Does the loader attach a GPU ring for the LAUNCH (parent) process?

    CURRENT (fix_accel_gate=False): only when launch_program contains 'glmark2'
    (runtime_report.cpp L1731).                                         [코드-실증]

    PROPOSED (fix_accel_gate=True, chromium-gpu-child-plan §3.A-1): also when the
    launch is an accel-requesting chromium (argv has --use-gl=angle /
    --use-angle=gles-egl), so the browser process owns a ring the children (or an
    --in-process-gpu) can use.
    """
    if _is_glmark2(launch_program):
        return True
    if fix_accel_gate and child_requests_accel(launch_argv):
        return True
    return False


def predict_gpu_child_ring(
    launch_program: str,
    child_argv,
    *,
    launch_argv=None,
    is_fresh_child: bool = True,
    fix_accel_gate: bool = False,
    fix_b3_propagates_ring: bool = False,
) -> RingBinding:
    """Predict whether a chromium GPU child binds the Mali ring, and the implied
    exit_code=5 hypothesis.

    Parameters mirror the body's data flow:
      launch_program          — config.program (e.g. ".../chromium-headless-shell").
      child_argv              — the GPU consumer's argv (carries --type=gpu-process,
                                --in-process-gpu, --use-gl=...). For the parent we
                                read launch_argv.
      launch_argv             — the parent (browser) argv; defaults to child_argv
                                when not given (single-process-ish cmdline).
      is_fresh_child          — True (default) when the GPU consumer is a SEPARATE
                                fresh-execve child re-mapped by GATE-1 (it inherits
                                its env from the parent's execve + B-3, §1.2). False
                                when the GPU consumer IS the launch guest itself
                                (the single-process in-process-jump path: glmark2,
                                and chromium under --in-process-gpu) — that process
                                gets the parent ring env DIRECTLY (no B-3 hop).
      fix_accel_gate          — §3.A-1 applied (parent attaches ring for accel
                                chromium, not only glmark2).
      fix_b3_propagates_ring  — §3.A-2 applied (B-3 carries ALR_GPU_RING_* into the
                                fresh child env).

    Decision (matches chromium-gpu-child-plan §1.2/1.3 + §3.A):
      0. NOT a fresh child (launch guest itself, or --in-process-gpu) -> the ring
         env is on its own environment directly -> attached iff parent_ring.
      1. --in-process-gpu          -> no separate GPU child; ring lives in parent.
                                      attached iff parent_ring_attached (§3.A-3).
      2. parent has no ring         -> child cannot inherit ring env -> ring-less.
      3. parent has ring, but B-3
         does NOT propagate ring    -> fresh child env lacks ALR_GPU_RING_* ->
                                       ring-less (the CURRENT-code trap, §1.2).
      4. parent has ring AND B-3
         propagates ring (§3.A-2)   -> attached.
    """
    if launch_argv is None:
        launch_argv = child_argv

    parent_ring = parent_attaches_ring(
        launch_program, launch_argv, fix_accel_gate=fix_accel_gate
    )

    # Case 0: the GPU consumer is the LAUNCH GUEST itself (single-process
    # in-process-jump — glmark2 today, or chromium with --in-process-gpu). That
    # process's env IS the parent guest_env, which already carries ALR_GPU_RING_*
    # when parent_ring (no fresh-execve, no B-3 hop). This is the CP-2 glmark2
    # path. --in-process-gpu also lands here (handled explicitly below for the
    # has_separate_gpu_child flag/reason).
    if not is_fresh_child and not _has_flag(child_argv, "--in-process-gpu"):
        return RingBinding(
            attached=parent_ring,
            reason=(REASON_PARENT_GLMARK2 if _is_glmark2(launch_program)
                    else REASON_FIX_PROPAGATED if parent_ring
                    else REASON_NOT_ACCEL),
            primary_hypothesis=(H_NONE if parent_ring else H1_RINGLESS),
            has_separate_gpu_child=False,
            parent_ring_attached=parent_ring,
        )

    # Case 1: --in-process-gpu removes the separate GPU child (§3.A-3). The GPU
    # runs in the browser process, which the loader launched directly — so the
    # parent ring (if any) is used in-process; no fresh-child env propagation.
    if _has_flag(child_argv, "--in-process-gpu"):
        return RingBinding(
            attached=parent_ring,
            reason=REASON_IN_PROCESS_GPU,
            primary_hypothesis=(H_NO_GPU_CHILD if parent_ring else H1_RINGLESS),
            has_separate_gpu_child=False,
            parent_ring_attached=parent_ring,
        )

    # Case 2: parent never attached a ring (chromium today, no glmark2, no accel
    # gate) -> there is no ring fd to inherit -> ring-less.
    if not parent_ring:
        reason = (
            REASON_PARENT_GLMARK2 if _is_glmark2(launch_program) else REASON_NOT_ACCEL
        )
        # (REASON_PARENT_GLMARK2 only reached if glmark2 yet parent_ring False,
        # which cannot happen; kept for symmetry/readability.)
        return RingBinding(
            attached=False,
            reason=REASON_NOT_ACCEL if not _is_glmark2(launch_program) else reason,
            primary_hypothesis=H1_RINGLESS,
            has_separate_gpu_child=True,
            parent_ring_attached=False,
        )

    # Parent has a ring. Does the fresh GPU child's env carry ALR_GPU_RING_*?
    if not fix_b3_propagates_ring:
        # CURRENT B-3 injects only LD_PRELOAD/ALR_ROOTFS -> ring env dropped (§1.2).
        return RingBinding(
            attached=False,
            reason=REASON_FRESH_CHILD_NO_PROPAGATE,
            primary_hypothesis=H1_RINGLESS,
            has_separate_gpu_child=True,
            parent_ring_attached=True,
        )

    # Case 4: §3.A-2 applied — B-3 threads ALR_GPU_RING_* into the child.
    return RingBinding(
        attached=True,
        reason=REASON_FIX_PROPAGATED,
        primary_hypothesis=H_NONE,
        has_separate_gpu_child=True,
        parent_ring_attached=True,
    )


@dataclass(frozen=True)
class EnvInjectionPlan:
    """What B-3 (decide_exec_envp_injection) must add to a fresh child env.

    Models chromium-gpu-child-plan §3.A-2: extend B-3 from {LD_PRELOAD, ALR_ROOTFS}
    to also carry the ring env when the parent attached a ring. Pure set logic over
    the child's existing env keys — host-testable; the device wiring is §3.A-2.
    """

    add_keys: tuple[str, ...]
    carries_ring: bool


def plan_child_env_injection(
    child_env_keys,
    *,
    parent_ring_attached: bool,
    fix_b3_propagates_ring: bool,
) -> EnvInjectionPlan:
    """Compute the keys B-3 should ADD to a fresh child env.

    Today B-3 adds LD_PRELOAD/ALR_ROOTFS if missing (modeled abstractly as "the
    body already handles those"); this function focuses on the GPU-ring delta the
    fix introduces: when the parent has a ring AND the fix is on, any RING_REQUIRED
    key absent from the child env must be added (+ the optional doorbell).
    """
    present = set(child_env_keys or ())
    add: list[str] = []
    carries = False
    if fix_b3_propagates_ring and parent_ring_attached:
        for k in (ENV_RING_FD, ENV_RING_BYTES, ENV_RING_DOORBELL_FD):
            if k not in present:
                add.append(k)
        carries = ENV_RING_FD not in present  # the required one drove the inject
        # If ring_fd was already present (a parent already injected), it's a no-op
        # for the required key but we still ensure bytes/doorbell are coherent.
        carries = carries or (ENV_RING_FD in present)
    return EnvInjectionPlan(add_keys=tuple(add), carries_ring=carries)
