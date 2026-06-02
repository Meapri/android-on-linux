"""ADR-002 §3 P1 (M-R2) — Chromium raw-svc storm decomposition model.

Pure, host-testable (no device, no I/O). Parses the three logcat lines the
M-R2 supervisor emits (runtime_report.cpp, WS-1) and decomposes the syscall
storm into the numbers ADR-002 §2/§4 needs to *decide* between the two
substantive fixes (USER_NOTIF / svc-rewrite) BEFORE any device A/B:

    alr sc trace_hist nr:count nr:count ...   RET_TRACE'd syscalls by nr (top-16)
    alr sc emul_hist  nr:count nr:count ...   SIGSYS-emulated syscalls by nr
    alr sc stime_us=.. utime_us=.. nonvol_ctxt=.. traps=.. emul=..

The two questions this answers from a single device capture (ADR-002 §4 (c)):

  1. Of the RET_TRACE'd syscalls, how many are the 9 *path* syscalls the
     interposer already mediates (path_trap) vs everything else (nonpath_trap)?
     The non-path traps are the USER_NOTIF *absorb candidates* — traps that a
     notif channel could swallow without a ptrace round-trip. If that ratio is
     ~0, the current BPF already ALLOWs non-path raw-svc and M-R1's ROI must be
     re-evaluated before the device A/B even runs (ADR-002 §4 (c)).

  2. Is `nonvol_ctxt ≈ 2 × (traps + emul)`? Each ptrace/SIGSYS round-trip costs
     ~2 non-voluntary context switches (guest -> supervisor -> guest). A ratio
     near 2 means the stime is *round-trip dominated* (the substantive fixes
     have large ROI); a ratio near 0 means the stime is the weight of chromium's
     own ALLOW'd 24ns×N syscalls, which no model removes (ADR-002 §1, §5).

This module never edits the C++/Kotlin sources or `parse_report`. The marker
regexes live as ADD-only helpers in `bench.report_parse`.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from bench.report_parse import (
    _parse_emul_hist,
    _parse_stime,
    _parse_trace_hist,
)

# arm64 path-taking syscall numbers the interposer mediates (ADR-002 §2, the
# nine PATH_SYSCALLS). RET_TRACE'd hits on these are path_trap; everything else
# in trace_hist is nonpath_trap = a USER_NOTIF absorb candidate.
PATH_NRS: frozenset[int] = frozenset(
    {34, 35, 48, 56, 78, 79, 291, 437, 439}
)

# arm64 nr -> name, for readable markdown. Only the path set is named here; any
# unknown nr renders as `nr<N>`.
_PATH_NAMES = {
    34: "mkdirat",
    35: "unlinkat",
    48: "faccessat",
    56: "openat",
    78: "readlinkat",
    79: "newfstatat",
    291: "statx",
    437: "openat2",
    439: "faccessat2",
}

# The supervisor reports trace_hist truncated to the busiest syscalls; this is
# the documented cap (ADR-002 §2 M-R2: "top-16"). The model carries it so the
# markdown can flag a possibly-truncated distribution.
TOP_HIST = 16

# Classification thresholds for the round-trip back-calculation. A round-trip
# costs ~2 non-voluntary context switches; we band around that, leaving a wide
# `ambiguous` middle because chromium's own blocking syscalls (futex/epoll_wait)
# also generate ctxt switches and blur the signal (ADR-002 §2 risk note, §5.2).
ROUNDTRIP_DOMINATED_MIN = 1.5  # >= this -> round-trip dominated
SYSCALL_WEIGHT_MAX = 0.5  # <= this -> syscall-weight (24ns×N) dominated

STORM_ROUNDTRIP = "roundtrip-dominated"
STORM_SYSCALL_WEIGHT = "syscall-weight-dominated"
STORM_AMBIGUOUS = "ambiguous"


@dataclass(frozen=True)
class SyscallMix:
    """Decomposed M-R2 storm measurement. Histograms keep insertion order.

    `trace_hist`/`emul_hist` are None when their logcat line was absent (vs an
    empty {} for a present-but-empty histogram). `stime` is None when its line
    is absent; the derived ratios then fall back to None / 0 as documented.
    """

    trace_hist: dict[int, int] | None
    emul_hist: dict[int, int] | None
    stime: dict[str, int] | None = None

    # --- trace_hist split (path vs absorb-candidate non-path) ---------------- #
    @property
    def path_trap(self) -> int:
        """Sum of RET_TRACE'd path-syscall hits (0 if no trace_hist)."""
        h = self.trace_hist or {}
        return sum(c for nr, c in h.items() if nr in PATH_NRS)

    @property
    def nonpath_trap(self) -> int:
        """Sum of RET_TRACE'd non-path hits = USER_NOTIF absorb candidates."""
        h = self.trace_hist or {}
        return sum(c for nr, c in h.items() if nr not in PATH_NRS)

    @property
    def total_trap(self) -> int:
        return self.path_trap + self.nonpath_trap

    @property
    def path_trap_ratio(self) -> float:
        """path_trap / total_trap. 0.0 when there are no traps (guarded)."""
        total = self.total_trap
        return self.path_trap / total if total else 0.0

    @property
    def absorb_candidate_ratio(self) -> float:
        """nonpath_trap / total_trap = fraction of traps USER_NOTIF could absorb.

        0.0 when there are no traps (guarded). This is the device-direct answer
        to ADR-002 §4 (c): if ~0, the substantive notif fix has little to absorb.
        """
        total = self.total_trap
        return self.nonpath_trap / total if total else 0.0

    @property
    def emul_total(self) -> int:
        """Sum of SIGSYS-emulated hits (0 if no emul_hist)."""
        h = self.emul_hist or {}
        return sum(h.values())

    # --- round-trip back-calculation ---------------------------------------- #
    @property
    def traps(self) -> int:
        """RET_TRACE round-trips. Prefer the stime line's authoritative `traps`
        count; fall back to the trace_hist sum when the stime line is absent."""
        if self.stime is not None:
            return self.stime["traps"]
        return self.total_trap

    @property
    def emul(self) -> int:
        """SIGSYS-emulate round-trips. stime line authoritative, else emul_hist."""
        if self.stime is not None:
            return self.stime["emul"]
        return self.emul_total

    @property
    def nonvol_ctxt(self) -> int | None:
        """Non-voluntary context switches over the run (None if no stime line)."""
        return self.stime["nonvol_ctxt"] if self.stime is not None else None

    @property
    def ctxt_per_roundtrip(self) -> float | None:
        """nonvol_ctxt / max(1, traps+emul). ~2 => round-trips dominate stime.

        None when there is no stime line (no nonvol_ctxt to divide). The
        denominator is floored at 1 so a zero-round-trip run yields the raw
        nonvol_ctxt rather than a ZeroDivisionError (division-guard).
        """
        if self.nonvol_ctxt is None:
            return None
        return self.nonvol_ctxt / max(1, self.traps + self.emul)

    @property
    def truncated(self) -> bool:
        """True if trace_hist hit the supervisor's top-16 cap (distribution may
        be clipped — a caveat for any non-path-ratio read)."""
        return self.trace_hist is not None and len(self.trace_hist) >= TOP_HIST


def parse_syscall_mix(text: str) -> SyscallMix:
    """Parse the three M-R2 logcat lines out of a report/logcat dump.

    Missing lines stay None (trace_hist/emul_hist/stime independently). Never
    raises on absent lines; a present-but-empty histogram parses to {}.
    """
    return SyscallMix(
        trace_hist=_parse_trace_hist(text),
        emul_hist=_parse_emul_hist(text),
        stime=_parse_stime(text),
    )


def classify_storm(mix: SyscallMix) -> str:
    """Judge what dominates the storm's supervisor-side stime (ADR-002 §2 branch).

    Returns one of:
      'roundtrip-dominated'        nonvol_ctxt/(traps+emul) >= ROUNDTRIP_DOMINATED_MIN
                                   -> substantive fix (USER_NOTIF/svc-rewrite) ROI is large
      'syscall-weight-dominated'   ratio <= SYSCALL_WEIGHT_MAX
                                   -> stime is chromium's own 24ns×N ALLOW weight; no model removes it
      'ambiguous'                  in-between, OR no stime line, OR no round-trips
                                   to divide (the signal is unresolved on host data alone)
    """
    ratio = mix.ctxt_per_roundtrip
    if ratio is None:
        return STORM_AMBIGUOUS
    # No round-trips at all: there is nothing for a round-trip fix to remove, but
    # we cannot positively attribute stime to syscall weight either -> ambiguous.
    if mix.traps + mix.emul == 0:
        return STORM_AMBIGUOUS
    if ratio >= ROUNDTRIP_DOMINATED_MIN:
        return STORM_ROUNDTRIP
    if ratio <= SYSCALL_WEIGHT_MAX:
        return STORM_SYSCALL_WEIGHT
    return STORM_AMBIGUOUS


def _nr_name(nr: int) -> str:
    return _PATH_NAMES.get(nr, f"nr{nr}")


def _fmt_hist(hist: dict[int, int] | None) -> str:
    if hist is None:
        return "_(line absent)_"
    if not hist:
        return "_(empty)_"
    parts = [f"{_nr_name(nr)}={c}" for nr, c in sorted(
        hist.items(), key=lambda kv: (-kv[1], kv[0])
    )]
    return ", ".join(parts)


def summarize_markdown(mix: SyscallMix) -> str:
    """Render a markdown decomposition summary for the M-R2 evidence doc."""
    verdict = classify_storm(mix)
    ratio = mix.ctxt_per_roundtrip
    ratio_s = f"{ratio:.2f}" if ratio is not None else "n/a (no stime line)"
    path_pct = mix.path_trap_ratio * 100.0
    absorb_pct = mix.absorb_candidate_ratio * 100.0

    lines = [
        "## M-R2 syscall-mix decomposition",
        "",
        f"- **storm verdict**: `{verdict}`",
        f"- **round-trip back-calc**: nonvol_ctxt / max(1, traps+emul) = {ratio_s}"
        "  _(≈2 ⇒ round-trips dominate stime)_",
        f"- traps (RET_TRACE) = {mix.traps}, emul (SIGSYS) = {mix.emul}",
        f"- nonvol_ctxt = "
        f"{mix.nonvol_ctxt if mix.nonvol_ctxt is not None else 'n/a'}",
        "",
        "### trace_hist split (path vs absorb-candidate)",
        f"- path_trap = {mix.path_trap}  ({path_pct:.1f}% of {mix.total_trap} traps)",
        f"- nonpath_trap = {mix.nonpath_trap}  "
        f"({absorb_pct:.1f}% absorb-candidate for USER_NOTIF)",
        f"- trace_hist: {_fmt_hist(mix.trace_hist)}",
        f"- emul_hist: {_fmt_hist(mix.emul_hist)}",
    ]
    if mix.truncated:
        lines.append(
            f"- _note: trace_hist at the top-{TOP_HIST} cap — distribution may be "
            "clipped; non-path ratio is a lower bound._"
        )
    return "\n".join(lines)
