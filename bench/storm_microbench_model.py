"""WS-5 — N-thread raw-`svc` storm microbench cost model (ADR-002, CP-6 R2).

Pure arithmetic, no I/O. This extends the single-stream `bench.storm_cost_model`
to the *multi-thread* case the Chromium `--dump-dom` wall actually exhibits: a
guest that fans out to N worker threads, each issuing a syscall storm, all of
whose ptrace events are drained by ONE serial supervisor `waitpid(-1, __WALL)`
loop (runtime_report.cpp L2138). The headline question CP-6 R2 needs answered is
the same as the single-stream model — *is the storm usable, and which fix
(svc-rewrite / user_notif) has ROI?* — but now with the per-thread fan-out and
the supervisor's serial-drain pressure made explicit.

It does NOT execute any seccomp BPF, ptrace, clone, or aarch64 self-rewrite —
the host is darwin and cannot install NEW_LISTENER, run a real-kernel ptrace
round-trip, or self-modify aarch64 text. This is the *cost model*
(calculation/regression) only; the per-strategy ns constants are estimates
seeded by ADR-001/ADR-002 (and reused verbatim from `bench.storm_cost_model`,
which this module imports), and the effect on a real workload is DEVICE-ONLY
(M-R2 / M-R5 / M-R1 microbench gates). The multi-thread microbench *absolute*
ns/op, the deadlock reproduction, and the trap round-trip are all DEVICE-ONLY;
the host verifies only the arg-parsing / fan-out / cost-mapping arithmetic.

----------------------------------------------------------------------------
What the multi-thread case adds over the single-stream model (grounded in code)
----------------------------------------------------------------------------
The single-stream `storm_cost_model.estimate_storm_*` already prices one stream
of `N` syscalls with `trap_count` round-trips. The new facts the N-thread case
must encode, each tied to a concrete source site:

  (1) **path_ratio decides trap_count, per the PCGATE BPF.** The interposer's
      filter (libalr_interpose.c L457-510, "DECISION") returns RET_TRACE ONLY
      for the 9 path NRs issued at a *non-trampoline* PC; every non-path syscall
      (futex/getpid/clock_gettime/mmap/…) returns RET_ALLOW and pays only the
      24ns dispatch floor with ZERO ptrace round-trip. So a thread's
      round-trips = `syscalls_per_thread * path_ratio` (the raw-svc PATH share
      that escapes the trampoline), and its non-path share pays the floor only.
      This is exactly why the microbench needs BOTH a `getpid` (non-path,
      RET_ALLOW, floor-only) variant AND an `openat(path)` (RET_TRACE,
      round-trip) variant to separate the two costs on device.

  (2) **The supervisor drains ALL tracees SERIALLY.** runtime_report.cpp L2138
      is a single `while(true){ waitpid(-1, __WALL); … }` loop on ONE thread.
      N worker threads do issue their syscalls concurrently on N cores, but
      every RET_TRACE EVENT_SECCOMP stop, every EVENT_CLONE, and every new-tid
      EVENT_STOP is processed one-at-a-time by that loop. So the ROUND-TRIP work
      does NOT parallelize across threads — the supervisor-side round-trip time
      sums over all threads' traps (it is the serial bottleneck). The 24ns
      seccomp dispatch floor, by contrast, runs in-kernel on each worker's own
      core and DOES parallelize. The wall-clock is therefore the MAX of:
        * the parallel floor term (per-thread floor work / overlap), and
        * the serial supervisor term (Σ all threads' round-trips, one queue),
      plus a fixed per-thread bring-up cost (clone + initial EVENT_STOP).

  (3) **Per-thread bring-up is itself serialized supervisor work.** Each worker
      costs one EVENT_CLONE (L2828 ++guest_threads) + one initial EVENT_STOP
      (L2783 known_tids.insert -> CONT). Two serial supervisor handlings per
      thread, independent of the storm. Cheap per thread, but it is the term
      that the deadlock-rediagnose axis (R1/R5) cares about: if the guest never
      reaches even the first clone inside the alarm window, max_threads stays 1
      and NONE of the storm terms are measured.

----------------------------------------------------------------------------
Honest scope (what host CANNOT decide)
----------------------------------------------------------------------------
  * Whether the guest reaches N threads at all (deadlock vs window-too-short) is
    DEVICE-ONLY (see bench/deadlock_triage.py / the R1 axis). This model PRICES
    a storm assuming the threads ran; it does not predict that they do.
  * The absolute ns/op of the multi-thread microbench, the serial-drain
    contention factor, and the real round-trip cost are DEVICE-ONLY. The host
    fixes the *form* of the model and its regression behavior only.
  * Like the single-stream model, this keeps seccomp ON, so the 24ns dispatch
    floor survives every strategy including svc_rewrite (ADR §1 nail).
"""
from __future__ import annotations

from dataclasses import dataclass

from bench.storm_cost_model import (
    DEFAULT_THRESHOLD_S,
    PATH_SYSCALL_NRS,
    VALID_STRATEGIES,
    StormCostParams,
    estimate_storm_ns,
    is_usable,
)

# Re-export so callers can `from bench.storm_microbench_model import ...` the
# shared constants without also importing the single-stream module.
__all__ = [
    "PATH_SYSCALL_NRS",
    "VALID_STRATEGIES",
    "DEFAULT_THRESHOLD_S",
    "StormCostParams",
    "MicrobenchVariant",
    "VALID_VARIANTS",
    "MICROBENCH_GETPID",
    "MICROBENCH_OPENAT",
    "estimate",
    "estimate_seconds",
    "MtStormResult",
    "MicrobenchMeasurement",
    "inject_microbench",
    "alarm_window_max_threads_usable",
    "format_microbench_arg",
]

# --------------------------------------------------------------------------- #
# Microbench variants. These mirror the two binary variants the device probe
# needs (R2 host_buildable P-R2a): a non-path RET_ALLOW storm (getpid) that
# measures the 24ns dispatch floor, and a path RET_TRACE storm (openat on a
# rootfs path) that measures the ptrace round-trip. A variant's `path_ratio`
# is the share of its syscalls that take the RET_TRACE round-trip lane.
# --------------------------------------------------------------------------- #
MICROBENCH_GETPID = "syscall-mt"   # getpid: non-path, RET_ALLOW, floor-only
MICROBENCH_OPENAT = "openat-mt"    # openat(rootfs path): RET_TRACE, round-trip

VALID_VARIANTS: tuple[str, ...] = (MICROBENCH_GETPID, MICROBENCH_OPENAT)

# getpid is NR 172 (non-path -> RET_ALLOW); openat is NR 56 (a path NR ->
# RET_TRACE at a non-trampoline PC). Pinned so a silent NR edit fails loudly and
# so the variant<->path_ratio mapping is self-documenting against PATH_SYSCALL_NRS.
MICROBENCH_NR: dict[str, int] = {
    MICROBENCH_GETPID: 172,
    MICROBENCH_OPENAT: 56,
}


@dataclass(frozen=True)
class MicrobenchVariant:
    """One microbench variant's syscall character (the path_ratio source)."""

    name: str
    nr: int
    path_ratio: float  # share of this variant's syscalls that RET_TRACE

    @property
    def is_path(self) -> bool:
        return self.nr in PATH_SYSCALL_NRS


# Canonical variants. getpid -> path_ratio 0.0 (pure floor); openat -> 1.0
# (every op is a path RET_TRACE round-trip in the raw-svc case). A caller can
# still pass an arbitrary `path_ratio` to `estimate` to model a mixed guest
# (e.g. chromium render: mostly futex/non-path with a thin openat tail).
VARIANT_GETPID = MicrobenchVariant(MICROBENCH_GETPID, MICROBENCH_NR[MICROBENCH_GETPID], 0.0)
VARIANT_OPENAT = MicrobenchVariant(MICROBENCH_OPENAT, MICROBENCH_NR[MICROBENCH_OPENAT], 1.0)
CANONICAL_VARIANTS: dict[str, MicrobenchVariant] = {
    VARIANT_GETPID.name: VARIANT_GETPID,
    VARIANT_OPENAT.name: VARIANT_OPENAT,
}

# --------------------------------------------------------------------------- #
# Supervisor-side per-thread bring-up cost (ns). Each worker costs two SERIAL
# supervisor handlings: EVENT_CLONE (L2828) + the new-tid EVENT_STOP CONT
# (L2783). These are ptrace stop/continue ops on the single waitpid loop, so we
# price them at ~one round-trip's order of magnitude each. Estimate; effect is
# DEVICE-ONLY. Override via `bringup_ns_per_thread`.
# --------------------------------------------------------------------------- #
DEFAULT_BRINGUP_NS_PER_THREAD: float = 2.0 * 100_000.0  # 2 serial ptrace ops


def _check_inputs(n_threads: int, syscalls_per_thread: int, path_ratio: float) -> None:
    if n_threads < 0:
        raise ValueError(f"n_threads must be >= 0, got {n_threads}")
    if syscalls_per_thread < 0:
        raise ValueError(
            f"syscalls_per_thread must be >= 0, got {syscalls_per_thread}"
        )
    if not (0.0 <= path_ratio <= 1.0):
        raise ValueError(f"path_ratio must be in [0,1], got {path_ratio}")


@dataclass(frozen=True)
class MtStormResult:
    """Decomposed N-thread storm cost (all ns), plus the headline seconds.

    The four ns terms make the parallel-vs-serial split auditable:
      * floor_ns       — the 24ns dispatch on every syscall; runs in-kernel on
                         each worker's own core, so it PARALLELIZES (we report
                         the per-core term, i.e. one thread's floor work).
      * roundtrip_ns   — Σ over ALL threads of the ptrace round-trips; drained
                         SERIALLY by the single waitpid loop, so it does NOT
                         parallelize. This is the multi-thread bottleneck.
      * bringup_ns     — Σ per-thread clone+initial-stop supervisor work (serial).
      * total_ns       — the modeled wall-clock: max(parallel floor, serial
                         supervisor) — i.e. the slower of the two lanes — with
                         the serial lane = roundtrip_ns + bringup_ns.
    """

    strategy: str
    n_threads: int
    syscalls_per_thread: int
    path_ratio: float
    total_syscalls: int
    trap_count: int          # Σ over all threads of RET_TRACE round-trips
    floor_ns: float          # per-core floor work (parallelizes)
    roundtrip_ns: float      # serial supervisor round-trip work
    bringup_ns: float        # serial per-thread bring-up
    total_ns: float

    @property
    def seconds(self) -> float:
        return self.total_ns / 1e9


def estimate(
    strategy: str,
    n_threads: int,
    syscalls_per_thread: int,
    path_ratio: float,
    *,
    params: StormCostParams | None = None,
    bringup_ns_per_thread: float = DEFAULT_BRINGUP_NS_PER_THREAD,
    absorb_ratio: float = 0.0,
) -> MtStormResult:
    """Model an N-thread raw-`svc` storm under `strategy`.

    Args:
      strategy: one of VALID_STRATEGIES (current_ret_trace / svc_rewrite /
        user_notif / seccomp_off) — the same four ADR-002 weighs.
      n_threads: number of guest worker threads issuing the storm. Each is one
        EVENT_CLONE + one initial EVENT_STOP on the serial supervisor.
      syscalls_per_thread: syscalls each thread issues.
      path_ratio: share of each thread's syscalls that take the RET_TRACE
        round-trip lane (the 9 path NRs at a non-trampoline raw-svc PC). 0.0 for
        the getpid variant (all non-path -> RET_ALLOW, floor only); 1.0 for the
        openat variant; in-between for a mixed guest.
      params: ns cost constants (shared with bench.storm_cost_model; estimates).
      bringup_ns_per_thread: serial supervisor cost to bring up one worker.
      absorb_ratio: for user_notif only, the share of the NON-path traps a notif
        channel absorbs onto the cheaper cross-process round-trip. Path traps
        always stay on ptrace (CONTINUE is TOCTOU-unsafe for path rewrite).

    Returns an MtStormResult with the parallel-floor vs serial-supervisor split.

    The wall-clock model: the 24ns dispatch floor runs per-worker-core in
    parallel (we charge ONE thread's floor work as the critical path); the
    ptrace/notif round-trips are drained by ONE serial waitpid loop, so they sum
    over all threads. The reported wall-clock is the slower lane,
    max(parallel-floor, serial-supervisor). This is the multi-thread structure
    the single-stream `storm_cost_model` could not express.
    """
    _check_inputs(n_threads, syscalls_per_thread, path_ratio)
    if strategy not in VALID_STRATEGIES:
        raise ValueError(
            f"unknown strategy {strategy!r}; expected one of {VALID_STRATEGIES}"
        )
    if not (0.0 <= absorb_ratio <= 1.0):
        raise ValueError(f"absorb_ratio must be in [0,1], got {absorb_ratio}")
    if bringup_ns_per_thread < 0:
        raise ValueError(
            f"bringup_ns_per_thread must be >= 0, got {bringup_ns_per_thread}"
        )
    p = params or StormCostParams()

    total_syscalls = n_threads * syscalls_per_thread
    # Per-thread RET_TRACE round-trips under the CURRENT strategy = the path
    # share. floor() keeps trap_count an integer and <= syscalls_per_thread.
    path_per_thread = int(syscalls_per_thread * path_ratio)
    trap_count = n_threads * path_per_thread

    # --- Per-core (parallel) floor: ONE thread's dispatch-floor work. ------- #
    # estimate_storm_ns(current_ret_trace, ..., trap=0) is exactly N*floor (no
    # round-trip term), which is the floor cost of a single thread's stream.
    # svc_rewrite adds the hook ns on top of that same floor; seccomp_off drops
    # the floor entirely. We reuse the single-stream pricer for the floor of one
    # thread so the per-syscall constants never diverge between the two models.
    if strategy == "seccomp_off":
        floor_ns = estimate_storm_ns("seccomp_off", syscalls_per_thread, 0, params=p)
    elif strategy == "svc_rewrite":
        floor_ns = estimate_storm_ns("svc_rewrite", syscalls_per_thread, 0, params=p)
    else:
        # current_ret_trace and user_notif both keep the bare 24ns floor.
        floor_ns = estimate_storm_ns("current_ret_trace", syscalls_per_thread, 0, params=p)

    # --- Serial supervisor lane: round-trips summed over ALL threads. ------- #
    if strategy in ("svc_rewrite", "seccomp_off"):
        # No ptrace round-trip at all: svc_rewrite branches the raw-svc PATH site
        # to the trampoline (RET_ALLOW, no supervisor stop); seccomp_off has no
        # filter. The serial lane is just per-thread bring-up.
        roundtrip_ns = 0.0
    elif strategy == "user_notif":
        # Path traps STAY on ptrace (100µs); the absorbed share of the NON-path
        # traps moves to the cheaper notif round-trip. With path_ratio modeling
        # the path share, the non-path share is (1-path_ratio); absorb_ratio of
        # THAT moves to notif. Everything still serializes on the supervisor.
        nonpath_per_thread = syscalls_per_thread - path_per_thread
        absorbed_per_thread = int(nonpath_per_thread * absorb_ratio)
        absorbed = n_threads * absorbed_per_thread
        roundtrip_ns = (
            trap_count * p.ptrace_roundtrip_ns
            + absorbed * p.user_notif_roundtrip_ns
        )
    else:  # current_ret_trace
        roundtrip_ns = trap_count * p.ptrace_roundtrip_ns

    bringup_ns = n_threads * bringup_ns_per_thread
    serial_ns = roundtrip_ns + bringup_ns

    # Wall-clock = the slower of the two lanes. The floor parallelizes across
    # cores; the supervisor work serializes on one thread. (We do NOT add them:
    # while the supervisor drains traps, the workers' own floor work overlaps it.)
    total_ns = max(floor_ns, serial_ns)

    return MtStormResult(
        strategy=strategy,
        n_threads=n_threads,
        syscalls_per_thread=syscalls_per_thread,
        path_ratio=path_ratio,
        total_syscalls=total_syscalls,
        trap_count=trap_count,
        floor_ns=floor_ns,
        roundtrip_ns=roundtrip_ns,
        bringup_ns=bringup_ns,
        total_ns=total_ns,
    )


def estimate_seconds(
    strategy: str,
    n_threads: int,
    syscalls_per_thread: int,
    path_ratio: float,
    *,
    params: StormCostParams | None = None,
    bringup_ns_per_thread: float = DEFAULT_BRINGUP_NS_PER_THREAD,
    absorb_ratio: float = 0.0,
) -> float:
    """`estimate(...).seconds` — the headline wall-clock seconds."""
    return estimate(
        strategy,
        n_threads,
        syscalls_per_thread,
        path_ratio,
        params=params,
        bringup_ns_per_thread=bringup_ns_per_thread,
        absorb_ratio=absorb_ratio,
    ).seconds


# --------------------------------------------------------------------------- #
# Device-measurement injection. When the multi-thread microbench runs on device
# it prints `MICROBENCH mode=<v> threads=<N> iters=<i> ns_per_op=<x>` and the
# supervisor prints its trap/thread counters. This struct carries a measured
# ns/op (the REAL per-op cost, which folds in serial-drain contention the model
# can only estimate) so the cost model can be CALIBRATED instead of assumed.
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class MicrobenchMeasurement:
    """A device microbench result (DEVICE-ONLY source; host injects estimates).

    `ns_per_op` is the measured per-syscall wall-clock for ONE variant at a
    given thread count. `guest_threads` is the supervisor's observed
    EVENT_CLONE count (max_threads reached — answers the deadlock-vs-window
    question alongside bench/deadlock_triage.py). `measured` is False whenever
    the host injects a hypothetical value.
    """

    variant: str
    n_threads: int
    syscalls_per_thread: int
    ns_per_op: float
    guest_threads: int = 0
    measured: bool = False

    @property
    def total_ns(self) -> float:
        return self.ns_per_op * self.n_threads * self.syscalls_per_thread

    @property
    def total_seconds(self) -> float:
        return self.total_ns / 1e9

    @property
    def path_ratio(self) -> float:
        """The variant's path_ratio (canonical variants only; else raises)."""
        v = CANONICAL_VARIANTS.get(self.variant)
        if v is None:
            raise ValueError(
                f"unknown microbench variant {self.variant!r}; "
                f"expected one of {VALID_VARIANTS}"
            )
        return v.path_ratio


def inject_microbench(
    m: MicrobenchMeasurement,
    *,
    params: StormCostParams | None = None,
) -> dict[str, float]:
    """Compare a device microbench against the model across all strategies.

    Returns {strategy: modeled_seconds} for the measurement's
    (n_threads, syscalls_per_thread, path_ratio), plus the special key
    "measured" -> the device wall-clock seconds. The caller can then see how the
    measured cost lands relative to each strategy's prediction (e.g. a measured
    ns/op far above current_ret_trace's model implies serial-drain contention
    the floor-vs-serial split under-counts — a device-only effect to flag).
    """
    out: dict[str, float] = {"measured": m.total_seconds}
    for strat in VALID_STRATEGIES:
        out[strat] = estimate_seconds(
            strat,
            m.n_threads,
            m.syscalls_per_thread,
            m.path_ratio,
            params=params,
        )
    return out


# --------------------------------------------------------------------------- #
# Alarm-window back-calc (ties the storm model to the R1/R5 deadlock axes).
# The guest dies at the dynamic alarm (25s for chromium). If the storm cannot
# even FINISH bringing up N threads + draining their traps inside that window,
# the snapshot shows Threads<N and the storm N is never measured. This helper
# answers "how many threads' worth of this storm fits inside `window_s`?".
# --------------------------------------------------------------------------- #
def alarm_window_max_threads_usable(
    strategy: str,
    syscalls_per_thread: int,
    path_ratio: float,
    *,
    window_s: float,
    params: StormCostParams | None = None,
    bringup_ns_per_thread: float = DEFAULT_BRINGUP_NS_PER_THREAD,
    max_threads_cap: int = 1024,
) -> int:
    """Largest n_threads whose modeled storm finishes strictly under `window_s`.

    Monotone in n_threads (every term is non-decreasing in N), so a simple scan
    up to `max_threads_cap` suffices. Returns 0 if even a single thread's storm
    exceeds the window. This is the host estimate of "does the alarm window even
    let the storm reach N threads" — the device-only deadlock-vs-window question
    that bench/deadlock_triage.py decides for real, here bounded by arithmetic.
    """
    if window_s <= 0:
        return 0
    if max_threads_cap < 0:
        raise ValueError(f"max_threads_cap must be >= 0, got {max_threads_cap}")
    last_ok = 0
    for n in range(1, max_threads_cap + 1):
        s = estimate_seconds(
            strategy,
            n,
            syscalls_per_thread,
            path_ratio,
            params=params,
            bringup_ns_per_thread=bringup_ns_per_thread,
        )
        if is_usable(s, window_s):
            last_ok = n
        else:
            break  # monotone: once it overshoots it never comes back under
    return last_ok


# --------------------------------------------------------------------------- #
# Device-probe arg helper. The R2 probe drives the microbench via the native
# loader with a newline-joined arg blob (see test_android_alr_native_loader_probe
# conventions). This renders the exact arg string for a variant so the probe and
# the model never drift on the contract.
# --------------------------------------------------------------------------- #
def format_microbench_arg(
    variant: str,
    n_threads: int,
    syscalls_per_thread: int,
    path: str | None = None,
) -> str:
    """Render the newline-joined microbench arg blob for the device probe.

    e.g. format_microbench_arg("openat-mt", 22, 200000, "/etc/alr-probe.txt")
         -> "/bin/microbench\\nopenat-mt\\n22\\n200000\\n/etc/alr-probe.txt"
    The getpid variant omits the trailing path. Raises on an unknown variant or
    a missing path for the path-taking openat variant.
    """
    if variant not in VALID_VARIANTS:
        raise ValueError(
            f"unknown microbench variant {variant!r}; expected one of {VALID_VARIANTS}"
        )
    if n_threads < 1:
        raise ValueError(f"n_threads must be >= 1, got {n_threads}")
    if syscalls_per_thread < 1:
        raise ValueError(
            f"syscalls_per_thread must be >= 1, got {syscalls_per_thread}"
        )
    parts = ["/bin/microbench", variant, str(n_threads), str(syscalls_per_thread)]
    if variant == MICROBENCH_OPENAT:
        if not path or not path.startswith("/"):
            raise ValueError(
                f"openat-mt variant requires an absolute path, got {path!r}"
            )
        parts.append(path)
    return "\n".join(parts)
