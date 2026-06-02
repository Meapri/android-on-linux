"""GATE-1 — `/proc/self/exe` → guest host-path substitution at the exec trap (host decision model).

Pure, host-testable (NO device, NO I/O, NO ptrace). This is the executable
decision spec for chromium multiprocess (CR-5) GATE-1, the *single* new code
piece the plan identifies:

  docs/research/chromium-multiprocess-plan.md  §1.1, §6.1/6.2, §8-(2)
  docs/design/chromium-multiprocess-reexec.md  §2.1, §2.2 (경우 1/2/3), §6.2

WHAT GATE-1 IS
--------------
When the in-process re-exec supervisor traps a guest `execve(<gp>, argv, envp)`
it must decide whether to (a) re-map the target in-process via the resident
trampoline (kernel execve 0, W^X-safe) or (b) SKIP and let the existing B-1/B-3
no-redirect path run. The decision today lives in runtime_report.cpp
L2617-2657 (read-only here — T1 owns that file):

  - `/proc/self/exe` OR any `/proc/*`            -> skip "proc-self-exe"
  - basename "alr-reentry" (re-entry stub)       -> skip "stub"
  - not should_rewrite AND reason != already-host -> skip "non-rootfs"
  - else                                          -> RE-MAP, host = should_rewrite
                                                     ? med.host_path : gp   (L2655-2657)

chromium's zygote/gpu/utility children re-exec **`/proc/self/exe`** (and maybe
`/proc/<pid>/exe`), which currently hits the "proc-self-exe" SKIP — so the
fresh-execve child is NOT mediated and the kernel would load the *Android*
linker64 image. GATE-1 narrows the `/proc/*` SKIP to **only** real exec-target
forms (`/proc/self/exe` | `/proc/<digits>/exe`) and, when the launch guest's
rootfs host path is known (`host_path`, runtime_report.cpp L1507, already
in-scope at the exec trap — no new threading needed), SUBSTITUTES that host
path and falls into the existing re-map path. Non-exe `/proc/*`
(`/proc/self/maps`, `/proc/cpuinfo`, ...) keep skipping. rootfs absolute paths
(dpkg→dpkg-deb chain) keep using the existing rewrite branch — GATE-1 does not
touch them.

DECISION TABLE (this module is its spec)
----------------------------------------
  gp                         host_path     should_rewrite  -> action        skip_reason     remap host
  /proc/self/exe             "<rootfs>/c"  (n/a)           -> SUBSTITUTE     None            host_path
  /proc/self/exe             ""            (n/a)           -> SKIP(fallback) proc-self-exe   —
  /proc/1234/exe (digits)    "<rootfs>/c"  (n/a)           -> SUBSTITUTE     None            host_path
  /proc/self/maps            (any)         (n/a)           -> SKIP           proc-self-exe   —   (non-exe /proc/*)
  /proc/cpuinfo              (any)         (n/a)           -> SKIP           proc-self-exe   —   (non-exe /proc/*)
  /proc/self/root/x          (any)         (n/a)           -> SKIP           proc-self-exe   —   (.../exe? no)
  /usr/bin/dpkg-deb          (any)         True            -> REMAP          None            med.host_path  (existing)
  /data/rootfs/usr/bin/sh    (any)         False/already   -> REMAP          None            gp             (existing already-host)
  /bin/alr-reentry           (any)         (any)           -> SKIP           stub            —   (idempotent)
  ./helper (relative)        (any)         False           -> SKIP           non-rootfs      —
  ""  (empty)                (any)         False           -> SKIP           non-rootfs      —

INVARIANTS (HARD CONSTRAINTS, plan §5.3-(d) / reexec §5.3-(c))
  - argv/envp are NEVER touched — substitution changes only the TARGET (x19),
    argv (x20) and envp (x21) stay the guest original. This model carries that
    as `argv_register`/`envp_register` metadata it asserts are not the target.
  - No new ptrace op, no new syscall, no kernel execve, no new execmem — the
    substitution only re-points the re-map host string already used at L2655.
  - host_path is a SINGLE scalar (all chromium children re-exec the same chrome
    binary) — no multi-binary tracking structure (reexec §2.1).

darwin host cannot drive real seccomp/ptrace/aarch64 — only the *decision*
arithmetic lives here. The substitution EFFECT (cancel-execve + PC-redirect +
dynamic ld.so re-map + fd preservation) is DEVICE-ONLY (DEVICE-REQ: ALR-CR5-*).
"""
from __future__ import annotations

import re
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Actions / skip reasons — mirror runtime_report.cpp string constants exactly
# so a device log line and this model use the same vocabulary.
# ---------------------------------------------------------------------------
ACTION_SUBSTITUTE = "substitute"   # GATE-1: /proc[/self|/<pid>]/exe -> host_path, then re-map
ACTION_REMAP = "remap"             # existing re-map (rootfs absolute / already-host)
ACTION_SKIP = "skip"               # fall through to B-1/B-3 no-redirect path

SKIP_PROC_SELF_EXE = "proc-self-exe"   # runtime_report.cpp L2624
SKIP_STUB = "stub"                     # runtime_report.cpp L2632 (alr-reentry idempotency)
SKIP_NON_ROOTFS = "non-rootfs"         # runtime_report.cpp L2644

# /proc/<digits>/exe — chromium's self re-exec via its own pid (variant of
# /proc/self/exe). Anchored full match: only the *exe* symlink is an exec target.
_PROC_PID_EXE_RE = re.compile(r"/proc/\d+/exe\Z")

# The re-entry stub basename (idempotency guard, L2628-2633).
_REENTRY_STUB = "alr-reentry"


def _basename(path: str) -> str:
    i = path.rfind("/")
    return path[i + 1:] if i >= 0 else path


def is_self_exe_target(gp: str) -> bool:
    """True iff `gp` is an exec target form of "this process's own binary".

    GATE-1 narrows the current over-broad `strncmp(gp, "/proc/", 6)` SKIP
    (runtime_report.cpp L2623) to ONLY the forms chromium re-execs itself by:
      - exactly "/proc/self/exe", or
      - "/proc/<digits>/exe"  (self pid variant).
    Every other "/proc/*" (e.g. /proc/self/maps, /proc/cpuinfo,
    /proc/self/root/...) is NOT an exec target and must keep skipping.
    Mirrors reexec.md §2.2-경우1 주의 + plan §3.4.
    """
    if gp == "/proc/self/exe":
        return True
    return _PROC_PID_EXE_RE.fullmatch(gp) is not None


@dataclass(frozen=True)
class ExecTrapDecision:
    """The supervisor's per-exec-trap decision for the in-process re-map path.

    Fields mirror exactly what runtime_report.cpp L2617-2657 computes:
      action        — substitute | remap | skip
      skip_reason   — proc-self-exe | stub | non-rootfs | None (None unless SKIP)
      remap_host    — the host string fed to the trampoline (regs[19] target),
                      or None when SKIP. For SUBSTITUTE this is host_path; for
                      REMAP it is med.host_path (rewrite) or gp (already-host).
      is_self_exe_subst — the local bool the C++ patch (plan §6.2) threads from
                      the L2618 branch to the L2655 host-selection (so host is
                      host_path, not gp). True only for ACTION_SUBSTITUTE.
    """

    action: str
    skip_reason: str | None
    remap_host: str | None
    is_self_exe_subst: bool = False

    # Register-role metadata (exec ABI). The target lives in x19 after re-map;
    # argv=x20, envp=x21 are NEVER the substitution/rewrite target (plan §6.3).
    target_register: int = 19
    argv_register: int = 20
    envp_register: int = 21

    @property
    def redirected(self) -> bool:
        """Whether this exec enters the in-process re-map (substitute or remap)."""
        return self.action in (ACTION_SUBSTITUTE, ACTION_REMAP)


def decide_exec_trap(
    gp: str,
    *,
    inproc_reexec_on: bool,
    should_rewrite: bool,
    med_host_path: str,
    med_reason: str,
    host_path: str,
) -> ExecTrapDecision:
    """Pure GATE-1 decision for one trapped guest exec target `gp`.

    Inputs (all already in-scope at runtime_report.cpp L2617-2657):
      gp                — the guest exec target (regs[0] string, pread'd).
      inproc_reexec_on  — ALR_REEXEC_INPROC gate (else no re-map at all).
      should_rewrite    — med.should_rewrite from the path classifier.
      med_host_path     — med.host_path (the rootfs-translated host for `gp`).
      med_reason        — med.reason ("rewrite" | "already-host" | ...).
      host_path         — the LAUNCH guest's rootfs host path
                          (config.rootfs_dir + guest_rel, L1507). The single
                          scalar GATE-1 substitutes for /proc/self/exe.

    Returns an ExecTrapDecision. Decision order matches the C++ branch order
    (self-exe handling first, then stub idempotency, then non-rootfs), so this
    function is a faithful, line-orderable spec of the patched supervisor.

    NOTE — this models ONLY the inproc-redirect decision. When
    `inproc_reexec_on` is False the supervisor does no re-map (B-1/B-3 path); we
    return ACTION_SKIP with skip_reason=None to denote "inproc off, not a
    re-map skip". Callers that only care about the GATE-1 lane pass
    inproc_reexec_on=True.
    """
    if not inproc_reexec_on:
        # No in-process re-map this build/exec — neither substitute nor remap.
        return ExecTrapDecision(
            action=ACTION_SKIP, skip_reason=None, remap_host=None
        )

    # ---- (GATE-1) self-exe target: /proc/self/exe or /proc/<digits>/exe ----
    if is_self_exe_target(gp):
        if host_path:
            # SUBSTITUTE: re-point the re-map host to the launch guest's rootfs
            # host path (chromium children all re-exec the same chrome binary).
            # SKIP is suppressed -> falls into the existing re-map path with
            # host = host_path (plan §6.1/6.2).
            return ExecTrapDecision(
                action=ACTION_SUBSTITUTE,
                skip_reason=None,
                remap_host=host_path,
                is_self_exe_subst=True,
            )
        # Safe fallback: launch guest unknown -> keep the conservative SKIP.
        return ExecTrapDecision(
            action=ACTION_SKIP, skip_reason=SKIP_PROC_SELF_EXE, remap_host=None
        )

    # ---- non-exe /proc/* (maps, cpuinfo, self/root, ...) keep skipping ----
    # These are NOT exec targets; the current code lumps them under
    # "proc-self-exe" and GATE-1 preserves that (only the exec-target forms
    # above are diverted to substitution).
    if gp.startswith("/proc/"):
        return ExecTrapDecision(
            action=ACTION_SKIP, skip_reason=SKIP_PROC_SELF_EXE, remap_host=None
        )

    # ---- (c) re-entry stub idempotency (L2627-2634) ----
    if _basename(gp) == _REENTRY_STUB:
        return ExecTrapDecision(
            action=ACTION_SKIP, skip_reason=SKIP_STUB, remap_host=None
        )

    # ---- (b) only re-map a real rootfs binary (L2641-2645) ----
    # Re-map iff the classifier rewrote (host under rootfs) or gp is already
    # under rootfs (already-host). Anything else is non-rootfs -> skip.
    if not should_rewrite and med_reason != "already-host":
        return ExecTrapDecision(
            action=ACTION_SKIP, skip_reason=SKIP_NON_ROOTFS, remap_host=None
        )

    # ---- existing re-map (L2654-2657): rootfs absolute / already-host ----
    remap_host = med_host_path if should_rewrite else gp
    return ExecTrapDecision(
        action=ACTION_REMAP,
        skip_reason=None,
        remap_host=remap_host,
        is_self_exe_subst=False,
    )


def render_decision_table(rootfs_dir: str = "/data/rootfs") -> str:
    """Render the GATE-1 decision table over representative inputs (docs aid)."""
    host = rootfs_dir + "/opt/chromium/chrome"
    rows = [
        # (gp, should_rewrite, med_host_path, med_reason, host_path)
        ("/proc/self/exe", False, "", "native", host),
        ("/proc/self/exe", False, "", "native", ""),
        ("/proc/1234/exe", False, "", "native", host),
        ("/proc/self/maps", False, "", "native", host),
        ("/proc/cpuinfo", False, "", "native", host),
        ("/proc/self/root/bin/sh", False, "", "native", host),
        ("/usr/bin/dpkg-deb", True, rootfs_dir + "/usr/bin/dpkg-deb", "rewrite", host),
        (rootfs_dir + "/usr/bin/sh", False, "", "already-host", host),
        ("/bin/alr-reentry", False, rootfs_dir + "/bin/alr-reentry", "rewrite", host),
        ("./helper", False, "", "relative", host),
    ]
    out = [
        "| gp | should_rewrite | host_path | action | skip_reason | remap_host |",
        "|----|----------------|-----------|--------|-------------|------------|",
    ]
    for gp, sr, mhp, mr, hp in rows:
        d = decide_exec_trap(
            gp,
            inproc_reexec_on=True,
            should_rewrite=sr,
            med_host_path=mhp,
            med_reason=mr,
            host_path=hp,
        )
        out.append(
            f"| `{gp}` | {sr} | `{hp or '∅'}` | {d.action} | "
            f"{d.skip_reason or '—'} | `{d.remap_host or '—'}` |"
        )
    return "\n".join(out)


if __name__ == "__main__":  # pragma: no cover — manual table dump
    print(render_decision_table())
