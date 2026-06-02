"""GATE-1 unit tests — `/proc/self/exe` → host_path substitution decision.

Host-side regression of tools/proc_self_exe_model.decide_exec_trap, the single
new decision GATE-1 adds for chromium multiprocess (CR-5). Pins:

  docs/research/chromium-multiprocess-plan.md §1.1, §3.4, §6.1/6.2, §6.4
  docs/design/chromium-multiprocess-reexec.md §2.1, §2.2 (경우 1/2/3)

NO device, NO ptrace — only the decision arithmetic (the substitution EFFECT
is DEVICE-ONLY: cancel-execve + PC-redirect + dynamic ld.so re-map + fd
preservation, DEVICE-REQ: ALR-CR5-stepA). If a future runtime_report.cpp patch
diverts the wrong /proc form, or substitutes when host_path is empty, or starts
touching argv/envp, these turn red on the host before any device build.
"""
from __future__ import annotations

import pytest

from tools.proc_self_exe_model import (
    ACTION_REMAP,
    ACTION_SKIP,
    ACTION_SUBSTITUTE,
    SKIP_NON_ROOTFS,
    SKIP_PROC_SELF_EXE,
    SKIP_STUB,
    decide_exec_trap,
    is_self_exe_target,
)

ROOTFS = "/data/rootfs"
CHROME_HOST = ROOTFS + "/opt/chromium/chrome"


def _decide(gp, *, should_rewrite=False, med_host_path="", med_reason="native",
            host_path=CHROME_HOST, inproc_reexec_on=True):
    return decide_exec_trap(
        gp,
        inproc_reexec_on=inproc_reexec_on,
        should_rewrite=should_rewrite,
        med_host_path=med_host_path,
        med_reason=med_reason,
        host_path=host_path,
    )


# ---------------------------------------------------------------------------
# is_self_exe_target — the narrowing of the over-broad /proc/* SKIP (plan §3.4)
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("gp", [
    "/proc/self/exe",
    "/proc/1/exe",
    "/proc/1234/exe",
    "/proc/999999/exe",
])
def test_self_exe_forms_recognized(gp):
    assert is_self_exe_target(gp) is True


@pytest.mark.parametrize("gp", [
    "/proc/self/maps",
    "/proc/self/cmdline",
    "/proc/cpuinfo",
    "/proc/1234/maps",
    "/proc/self/root/bin/sh",   # .../exe? no — not an exec-target form
    "/proc/self/exe/x",         # trailing component — not the symlink itself
    "/proc/self/fd/3",
    "/proc/selfexe",            # no slash — not /proc/self/exe
    "/procX/self/exe",          # not under /proc/
    "/usr/bin/dpkg-deb",
    "",
])
def test_non_self_exe_forms_rejected(gp):
    assert is_self_exe_target(gp) is False


def test_proc_pid_exe_requires_all_digits():
    """Only /proc/<digits>/exe — a non-numeric component is NOT a self-exe."""
    assert is_self_exe_target("/proc/12a4/exe") is False
    assert is_self_exe_target("/proc//exe") is False
    assert is_self_exe_target("/proc/self2/exe") is False


# ---------------------------------------------------------------------------
# GATE-1 core: /proc/self/exe → host_path SUBSTITUTE (plan §6.1/6.2)
# ---------------------------------------------------------------------------
def test_proc_self_exe_substitutes_to_host_path():
    d = _decide("/proc/self/exe", host_path=CHROME_HOST)
    assert d.action == ACTION_SUBSTITUTE
    assert d.skip_reason is None
    assert d.remap_host == CHROME_HOST
    assert d.is_self_exe_subst is True
    assert d.redirected is True


def test_proc_pid_exe_substitutes_to_host_path():
    """The /proc/<pid>/exe self variant also substitutes (plan §3.4)."""
    d = _decide("/proc/4242/exe", host_path=CHROME_HOST)
    assert d.action == ACTION_SUBSTITUTE
    assert d.remap_host == CHROME_HOST
    assert d.is_self_exe_subst is True


def test_proc_self_exe_empty_host_path_falls_back_to_skip():
    """Safe fallback: launch guest unknown → keep conservative proc-self-exe SKIP."""
    d = _decide("/proc/self/exe", host_path="")
    assert d.action == ACTION_SKIP
    assert d.skip_reason == SKIP_PROC_SELF_EXE
    assert d.remap_host is None
    assert d.is_self_exe_subst is False
    assert d.redirected is False


def test_substitute_target_is_host_path_not_med_host():
    """gp=/proc/self/exe is outside rootfs → med would NOT rewrite it; the
    substitution must use the LAUNCH host_path, never gp or an empty med."""
    d = _decide(
        "/proc/self/exe",
        should_rewrite=False,         # /proc/* is never should_rewrite
        med_host_path="",             # med has no host for /proc/self/exe
        med_reason="native",
        host_path=CHROME_HOST,
    )
    assert d.remap_host == CHROME_HOST
    assert d.remap_host != "/proc/self/exe"


# ---------------------------------------------------------------------------
# Non-exe /proc/* keep skipping (plan §3.4, reexec §2.2-경우1 주의)
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("gp", [
    "/proc/self/maps",
    "/proc/cpuinfo",
    "/proc/self/cmdline",
    "/proc/1234/maps",
    "/proc/self/root/bin/sh",
])
def test_non_exe_proc_paths_skip(gp):
    d = _decide(gp, host_path=CHROME_HOST)
    assert d.action == ACTION_SKIP
    assert d.skip_reason == SKIP_PROC_SELF_EXE
    assert d.remap_host is None
    assert d.redirected is False


# ---------------------------------------------------------------------------
# Existing rootfs-absolute re-map is unchanged (dpkg chain, plan §6.4)
# ---------------------------------------------------------------------------
def test_rootfs_external_absolute_remaps_via_med_host():
    """dpkg→dpkg-deb: /usr/bin/dpkg-deb (rewrite) → med.host_path, NOT self-exe."""
    med_host = ROOTFS + "/usr/bin/dpkg-deb"
    d = _decide(
        "/usr/bin/dpkg-deb",
        should_rewrite=True,
        med_host_path=med_host,
        med_reason="rewrite",
    )
    assert d.action == ACTION_REMAP
    assert d.skip_reason is None
    assert d.remap_host == med_host
    assert d.is_self_exe_subst is False
    assert d.redirected is True


def test_already_host_rootfs_path_remaps_to_gp():
    """A path already under rootfs (already-host) re-maps to gp itself (idempotent)."""
    gp = ROOTFS + "/usr/bin/sh"
    d = _decide(gp, should_rewrite=False, med_reason="already-host")
    assert d.action == ACTION_REMAP
    assert d.skip_reason is None
    assert d.remap_host == gp


# ---------------------------------------------------------------------------
# Other skip reasons unchanged (stub idempotency, non-rootfs)
# ---------------------------------------------------------------------------
def test_reentry_stub_is_idempotent_skip():
    d = _decide(
        "/bin/alr-reentry",
        should_rewrite=True,
        med_host_path=ROOTFS + "/bin/alr-reentry",
        med_reason="rewrite",
    )
    assert d.action == ACTION_SKIP
    assert d.skip_reason == SKIP_STUB


def test_bare_reentry_stub_basename_skips():
    d = _decide("alr-reentry", should_rewrite=False, med_reason="relative")
    assert d.action == ACTION_SKIP
    assert d.skip_reason == SKIP_STUB


@pytest.mark.parametrize("gp", ["./helper", "helper", "", "relative/path"])
def test_relative_or_empty_is_non_rootfs_skip(gp):
    d = _decide(gp, should_rewrite=False, med_reason="relative")
    assert d.action == ACTION_SKIP
    assert d.skip_reason == SKIP_NON_ROOTFS


def test_sysdir_native_is_non_rootfs_skip():
    """A non-rootfs absolute that the classifier left native → non-rootfs skip."""
    d = _decide("/system/bin/linker64", should_rewrite=False, med_reason="native")
    assert d.action == ACTION_SKIP
    assert d.skip_reason == SKIP_NON_ROOTFS


# ---------------------------------------------------------------------------
# inproc OFF → no re-map at all (neither substitute nor remap)
# ---------------------------------------------------------------------------
def test_inproc_off_never_substitutes():
    d = _decide("/proc/self/exe", host_path=CHROME_HOST, inproc_reexec_on=False)
    assert d.action == ACTION_SKIP
    assert d.skip_reason is None      # not a re-map skip; inproc simply off
    assert d.redirected is False


def test_inproc_off_never_remaps_rootfs_abs():
    d = _decide(
        "/usr/bin/dpkg-deb",
        should_rewrite=True,
        med_host_path=ROOTFS + "/usr/bin/dpkg-deb",
        med_reason="rewrite",
        inproc_reexec_on=False,
    )
    assert d.action == ACTION_SKIP
    assert d.redirected is False


# ---------------------------------------------------------------------------
# INVARIANT: substitution touches only the TARGET register, never argv/envp
# (plan §6.3 / reexec §4.1 — x19=target, x20=argv, x21=envp)
# ---------------------------------------------------------------------------
def test_substitute_never_touches_argv_envp_registers():
    d = _decide("/proc/self/exe", host_path=CHROME_HOST)
    assert d.target_register == 19
    assert d.argv_register == 20
    assert d.envp_register == 21
    # The target register must differ from argv/envp — substitution re-points
    # only x19; argv/envp stay the guest original (no Option-S argv rewrite).
    assert d.target_register not in (d.argv_register, d.envp_register)


def test_remap_also_preserves_argv_envp_registers():
    d = _decide(
        "/usr/bin/dpkg-deb",
        should_rewrite=True,
        med_host_path=ROOTFS + "/usr/bin/dpkg-deb",
        med_reason="rewrite",
    )
    assert d.target_register == 19
    assert d.target_register not in (d.argv_register, d.envp_register)


# ---------------------------------------------------------------------------
# Convergence with v158 ALR_GUEST_EXE: substitute target == launch guest host
# (plan §3.3 — read-mediation and exec-substitution converge on same guest)
# ---------------------------------------------------------------------------
def test_substitute_target_matches_launch_guest_host_path():
    """All chromium children re-exec the SAME chrome binary, so the single
    host_path scalar is always the launch guest's rootfs host path."""
    launch_host = ROOTFS + "/opt/google/chrome/chrome"
    for gp in ("/proc/self/exe", "/proc/777/exe"):
        d = _decide(gp, host_path=launch_host)
        assert d.remap_host == launch_host
