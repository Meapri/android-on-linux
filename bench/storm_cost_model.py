"""WS-5 — Chromium raw-`svc` syscall-storm cost model (ADR-002 §3 P4).

Pure arithmetic, no I/O. This module quantifies the *wall-clock seconds* a
Chromium-class raw-`svc` syscall storm costs under each of the four mediation
strategies ADR-002 weighs, so that the moment M-R2 hands us a real measured
`(N, trap_count)` we can immediately render a usable / not-usable verdict
without another device round.

It does NOT execute any seccomp BPF, ptrace, or arm64 self-rewrite — the host
is darwin and cannot install NEW_LISTENER or self-modify aarch64 text. This is
the *cost model* (calculation/regression) only; the per-strategy ns constants
are estimates seeded by ADR-001/ADR-002, and the effect on a real workload is
DEVICE-ONLY (M-R2 / M-R5 / M-R1 gates).

----------------------------------------------------------------------------
The nail (ADR-002 §1, §5; libalr_interpose.c comment L63)
----------------------------------------------------------------------------
As long as seccomp is *installed*, every `svc` — including the one at the end
of ALR's own trampoline — is dispatched through the PCGATE BPF program once,
which costs `seccomp_dispatch` ns and returns RET_ALLOW. No model that keeps
seccomp on can erase that floor. So the realistic CP-6 goal is NOT "0% syscall
overhead" but "remove the ptrace round-trip so the storm becomes usable", and
whether it *is* usable hinges entirely on N (the absolute storm syscall count),
which is unmeasured until M-R2.

----------------------------------------------------------------------------
Cost constants (ns) — estimates, seeded by ADR-001 §1 / ADR-002 §3, §5
----------------------------------------------------------------------------
  seccomp_dispatch     = 24      floor that survives every seccomp-on model
  raw_svc_base         = 200     a bare `svc` with NO seccomp installed
  ptrace_roundtrip     = 100_000 (100 µs) EVENT_SECCOMP trap -> GETREGSET ->
                                 /proc/<tid>/mem rewrite -> CONT (supervisor)
  svc_hook             = 34      ASC-Hook trampoline body added on top of the
                                 surviving 24ns dispatch (trampoline ends in svc)
  user_notif_roundtrip estimate range: optimistic ~2000 / pessimistic ~5000
                                 (cross-process NOTIF_RECV + NOTIF_SEND ioctls
                                 + /proc/<pid>/mem); the absorbed (CONTINUE-able)
                                 non-path traps pay this instead of ptrace's 100µs

----------------------------------------------------------------------------
The four strategies (ADR-002 §1, §2, §4)
----------------------------------------------------------------------------
  current_ret_trace  N*seccomp_dispatch + trap_count*ptrace_roundtrip
       what HEAD does: every syscall pays the 24ns dispatch, and each RET_TRACE
       trap additionally pays the 100µs ptrace round-trip.

  svc_rewrite        N*(seccomp_dispatch + svc_hook)
       ASC-Hook style: binary-rewrite `svc` sites to the trampoline so there is
       NO ptrace trap, but the trampoline still ends in `svc` -> the 24ns
       dispatch floor REMAINS (ADR §1 nail), plus the 34ns hook body. The
       round-trip term vanishes entirely.

  user_notif         N*seccomp_dispatch
                       + absorbed*user_notif_roundtrip
                       + path_trap*ptrace_roundtrip
       SECCOMP_RET_USER_NOTIF: `absorbed` non-path hot traps move off ptrace
       onto the (cheaper, but still cross-process) notif round-trip; the 9 path
       syscalls STAY on ptrace (`path_trap`, CONTINUE is TOCTOU-unsafe for path
       rewrite). The 24ns floor still applies to every syscall.

  seccomp_off        N*raw_svc_base
       hypothetical seccomp-NOT-installed: the 24ns dispatch floor is gone (it
       only exists because a filter is loaded), so each syscall is a bare
       `svc` at raw_svc_base. Included as the theoretical lower bound; ALR
       cannot actually reach it (it needs the filter for path mediation + W^X),
       but it bounds how much the floor itself is costing.

----------------------------------------------------------------------------
ADR-002 §5 reproduction (50M storm, the numbers this module must reproduce)
----------------------------------------------------------------------------
  24ns floor alone (current, trap_count=0)         ~1.2 s
  svc_rewrite (+34ns hook)                          ~2.9 s
  user_notif optimistic (~2000ns absorbed)         ~100 s
  user_notif pessimistic (~5000ns absorbed)        ~250 s
"""
from __future__ import annotations

from dataclasses import dataclass, field

# --------------------------------------------------------------------------- #
# Path syscall set (arm64 NR). Identical to the PCGATE / interposer set. These
# are the traps that STAY on ptrace under user_notif (CONTINUE is TOCTOU-unsafe
# for in-`/proc/mem` path rewrite), so they are never "absorbed".
# --------------------------------------------------------------------------- #
PATH_SYSCALL_NRS: frozenset[int] = frozenset(
    {34, 35, 48, 56, 78, 79, 291, 437, 439}
)

VALID_STRATEGIES: tuple[str, ...] = (
    "current_ret_trace",
    "svc_rewrite",
    "user_notif",
    "seccomp_off",
)

# Default usable threshold (seconds). A storm under this is "usable".
DEFAULT_THRESHOLD_S: float = 10.0


@dataclass(frozen=True)
class StormCostParams:
    """Per-strategy ns cost constants (estimates; effect is device-only).

    All values are nanoseconds. Override any of them to explore the optimistic
    / pessimistic envelope (e.g. `user_notif_roundtrip_ns`) without touching the
    estimate functions.
    """

    seccomp_dispatch_ns: float = 24.0
    raw_svc_base_ns: float = 200.0
    ptrace_roundtrip_ns: float = 100_000.0  # 100 µs
    svc_hook_ns: float = 34.0
    user_notif_roundtrip_ns: float = 2_000.0  # optimistic default; ~5000 pessimistic

    # Named envelope endpoints for user_notif, so callers can sweep the range
    # ADR-002 §5 quotes (optimistic ~2000 / pessimistic ~5000) explicitly.
    user_notif_optimistic_ns: float = 2_000.0
    user_notif_pessimistic_ns: float = 5_000.0

    def with_user_notif(self, roundtrip_ns: float) -> "StormCostParams":
        """Return a copy with `user_notif_roundtrip_ns` swapped (envelope sweep)."""
        return StormCostParams(
            seccomp_dispatch_ns=self.seccomp_dispatch_ns,
            raw_svc_base_ns=self.raw_svc_base_ns,
            ptrace_roundtrip_ns=self.ptrace_roundtrip_ns,
            svc_hook_ns=self.svc_hook_ns,
            user_notif_roundtrip_ns=roundtrip_ns,
            user_notif_optimistic_ns=self.user_notif_optimistic_ns,
            user_notif_pessimistic_ns=self.user_notif_pessimistic_ns,
        )


def _check_counts(
    N: int,
    trap_count: int,
    *,
    absorbed: int,
    path_trap: int,
    enforce_trap_le_N: bool,
) -> None:
    if N < 0:
        raise ValueError(f"N must be >= 0, got {N}")
    if trap_count < 0:
        raise ValueError(f"trap_count must be >= 0, got {trap_count}")
    # The trap_count <= N relation is only meaningful for strategies that
    # actually consume trap_count (current_ret_trace / user_notif). svc_rewrite
    # and seccomp_off ignore it, so we do not constrain it there.
    if enforce_trap_le_N and trap_count > N:
        raise ValueError(f"trap_count ({trap_count}) cannot exceed N ({N})")
    if absorbed < 0 or path_trap < 0:
        raise ValueError(
            f"absorbed/path_trap must be >= 0, got absorbed={absorbed} path_trap={path_trap}"
        )
    if absorbed + path_trap > N:
        raise ValueError(
            f"absorbed+path_trap ({absorbed + path_trap}) cannot exceed N ({N})"
        )


def estimate_storm_ns(
    strategy: str,
    N: int,
    trap_count: int,
    *,
    params: StormCostParams | None = None,
    absorbed: int | None = None,
    path_trap: int | None = None,
) -> float:
    """Total storm cost in *nanoseconds* for `strategy` over `N` syscalls.

    `trap_count` is the number of RET_TRACE ptrace round-trips the storm incurs
    under the CURRENT (HEAD) strategy — the M-R2 measured input.

    For `user_notif`, the storm's `trap_count` is split into:
      * `path_trap`  — path-syscall traps that STAY on ptrace (default: the
                       smaller of `trap_count` and the path share; if not given
                       we conservatively keep ALL traps on ptrace, i.e.
                       path_trap = trap_count, absorbed = 0, which is the
                       no-benefit floor).
      * `absorbed`   — non-path hot traps moved onto the notif round-trip.
    Pass them explicitly (e.g. from M-R2's trace_hist path-vs-non-path split) to
    model the real absorption. They are ignored for the other three strategies.

    strategy ∈ {current_ret_trace, svc_rewrite, user_notif, seccomp_off}.
    """
    p = params or StormCostParams()
    if strategy not in VALID_STRATEGIES:
        raise ValueError(
            f"unknown strategy {strategy!r}; expected one of {VALID_STRATEGIES}"
        )

    if strategy == "user_notif":
        # Default split: keep everything on ptrace (no absorption) unless told
        # otherwise — the honest "we measured nothing yet" floor.
        pt = trap_count if path_trap is None else path_trap
        ab = 0 if absorbed is None else absorbed
        _check_counts(N, trap_count, absorbed=ab, path_trap=pt, enforce_trap_le_N=True)
        return (
            N * p.seccomp_dispatch_ns
            + ab * p.user_notif_roundtrip_ns
            + pt * p.ptrace_roundtrip_ns
        )

    # The other three ignore absorbed/path_trap. Only current_ret_trace consumes
    # trap_count, so it alone enforces trap_count <= N.
    _check_counts(
        N,
        trap_count,
        absorbed=0,
        path_trap=0,
        enforce_trap_le_N=(strategy == "current_ret_trace"),
    )
    if strategy == "current_ret_trace":
        return N * p.seccomp_dispatch_ns + trap_count * p.ptrace_roundtrip_ns
    if strategy == "svc_rewrite":
        # Trampoline endpoint is still `svc` -> dispatch floor survives (ADR §1).
        return N * (p.seccomp_dispatch_ns + p.svc_hook_ns)
    # seccomp_off: no filter installed -> the 24ns floor disappears entirely.
    return N * p.raw_svc_base_ns


def estimate_storm_seconds(
    strategy: str,
    N: int,
    trap_count: int,
    *,
    params: StormCostParams | None = None,
    absorbed: int | None = None,
    path_trap: int | None = None,
) -> float:
    """`estimate_storm_ns(...) / 1e9` — wall-clock seconds (the headline number)."""
    return estimate_storm_ns(
        strategy,
        N,
        trap_count,
        params=params,
        absorbed=absorbed,
        path_trap=path_trap,
    ) / 1e9


def is_usable(seconds: float, threshold_s: float = DEFAULT_THRESHOLD_S) -> bool:
    """A storm is usable if it finishes under `threshold_s` (default 10 s)."""
    return seconds < threshold_s


@dataclass(frozen=True)
class StrategyVerdict:
    """One row of the judgment table: a strategy's cost + usable flag."""

    strategy: str
    seconds: float
    usable: bool
    note: str = ""


@dataclass(frozen=True)
class StormMeasurement:
    """An M-R2 measured (or estimated) storm input to evaluate.

    `N` and `trap_count` come from the supervisor's in-process counters
    (DEVICE-ONLY; the host can only inject hypothetical values). `absorbed` /
    `path_trap` come from M-R2's trace_hist path-vs-non-path split; when None,
    user_notif is evaluated at its no-absorption floor.
    """

    N: int
    trap_count: int
    label: str = ""
    absorbed: int | None = None
    path_trap: int | None = None
    measured: bool = False  # True only when device-sourced (host injects False)


def split_traps_by_path(
    trace_hist: dict[int, int],
) -> tuple[int, int]:
    """Split an M-R2 `trace_hist` {nr: count} into (path_trap, non_path_trap).

    path_trap stays on ptrace under user_notif; non_path_trap is the upper
    bound on what user_notif could *absorb* (whether it actually absorbs them
    depends on the whitelist — device-only).
    """
    path = sum(c for nr, c in trace_hist.items() if nr in PATH_SYSCALL_NRS)
    non_path = sum(c for nr, c in trace_hist.items() if nr not in PATH_SYSCALL_NRS)
    return path, non_path


def evaluate(
    m: StormMeasurement,
    *,
    params: StormCostParams | None = None,
    threshold_s: float = DEFAULT_THRESHOLD_S,
    user_notif_pessimistic: bool = False,
) -> list[StrategyVerdict]:
    """Cost + usable verdict for every strategy over one measurement."""
    p = params or StormCostParams()
    if user_notif_pessimistic:
        p_un = p.with_user_notif(p.user_notif_pessimistic_ns)
    else:
        p_un = p.with_user_notif(p.user_notif_optimistic_ns)

    rows: list[StrategyVerdict] = []
    for strat in VALID_STRATEGIES:
        sp = p_un if strat == "user_notif" else p
        secs = estimate_storm_seconds(
            strat,
            m.N,
            m.trap_count,
            params=sp,
            absorbed=m.absorbed if strat == "user_notif" else None,
            path_trap=m.path_trap if strat == "user_notif" else None,
        )
        note = ""
        if strat == "user_notif":
            note = (
                "pessimistic ~5000ns" if user_notif_pessimistic else "optimistic ~2000ns"
            )
            if m.absorbed is None:
                note += " (no-absorption floor)"
        elif strat == "seccomp_off":
            note = "theoretical lower bound; ALR cannot install it"
        elif strat == "svc_rewrite":
            note = "24ns dispatch floor survives (ADR §1 nail)"
        rows.append(
            StrategyVerdict(strategy=strat, seconds=secs, usable=is_usable(secs, threshold_s), note=note)
        )
    return rows


def judgment_table_markdown(
    m: StormMeasurement,
    *,
    params: StormCostParams | None = None,
    threshold_s: float = DEFAULT_THRESHOLD_S,
) -> str:
    """Render a markdown judgment table for one measurement.

    Shows both the optimistic and pessimistic user_notif endpoints so the
    envelope ADR-002 §5 quotes is visible at a glance.
    """
    p = params or StormCostParams()
    opt = evaluate(m, params=p, threshold_s=threshold_s, user_notif_pessimistic=False)
    pess = evaluate(m, params=p, threshold_s=threshold_s, user_notif_pessimistic=True)
    pess_un = {r.strategy: r for r in pess}

    src = "measured (device)" if m.measured else "estimated (host-injected, NOT measured)"
    label = m.label or "storm"
    lines: list[str] = []
    lines.append(f"### Storm usability — {label}")
    lines.append("")
    lines.append(
        f"- input: N={m.N:,} trap_count={m.trap_count:,} "
        f"absorbed={m.absorbed} path_trap={m.path_trap}  [{src}]"
    )
    lines.append(f"- threshold: < {threshold_s:g}s = usable")
    lines.append("")
    lines.append("| strategy | seconds | usable | note |")
    lines.append("| --- | ---: | :---: | --- |")
    for r in opt:
        usable_mark = "yes" if r.usable else "NO"
        if r.strategy == "user_notif":
            # Show the full envelope on one row.
            ro = r
            rp = pess_un["user_notif"]
            env_usable = "yes" if (ro.usable and rp.usable) else (
                "split" if (ro.usable != rp.usable) else "NO"
            )
            lines.append(
                f"| user_notif | {ro.seconds:.3g} (opt) .. {rp.seconds:.3g} (pess) "
                f"| {env_usable} | absorbed={m.absorbed} path_trap={m.path_trap} |"
            )
        else:
            lines.append(
                f"| {r.strategy} | {r.seconds:.3g} | {usable_mark} | {r.note} |"
            )
    lines.append("")
    lines.append(
        "_Host computes the MODEL only; the per-strategy ns constants are "
        "estimates and the real effect is DEVICE-ONLY (M-R2/M-R5/M-R1 gates). "
        "darwin host cannot run aarch64 self-rewrite or a real-kernel "
        "NEW_LISTENER._"
    )
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# ADR-002 §5 canonical scenario: a 50M-syscall storm. Exposed as a module
# constant so both the docs renderer and the regression test reference the same
# numbers (no drift between the prose table and the model).
# --------------------------------------------------------------------------- #
ADR_SCENARIO_N: int = 50_000_000


def adr_scenario_table_markdown(
    *, params: StormCostParams | None = None, threshold_s: float = DEFAULT_THRESHOLD_S
) -> str:
    """The ADR-002 §5 canonical 50M storm, trap_count=0 (pure floor comparison)."""
    m = StormMeasurement(
        N=ADR_SCENARIO_N,
        trap_count=0,
        label=f"ADR-002 §5 canonical {ADR_SCENARIO_N:,} storm (trap_count=0)",
        absorbed=ADR_SCENARIO_N,  # hypothetically absorb the whole storm via notif
        path_trap=0,
        measured=False,
    )
    return judgment_table_markdown(m, params=params, threshold_s=threshold_s)
