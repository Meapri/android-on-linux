"""WS-5 — path-mediation overhead model (pure math, no I/O).

Seeded by WS-1 M2 device numbers
(docs/evidence/2026-06-01-ws1-m2-cpu-mediation-overhead.md), device SM-X236N
(mt6878, Mali-G615, Android 16), APK 0.4.127-cp1-gui-baseline-v127:

    alr perf alr xlate       ns/op = 4334.727   (cold `translate_rootfs_path`)
    alr perf syscall getppid ns/op =  218.338   (raw syscall baseline)
    → cold xlate ≈ 19.9 "syscall units" (4334.727 / 218.338).

This module formalizes ONLY the PATH-MEDIATION portion of ALR's CPU overhead:

  * path syscalls — rewritten in-process by the PCGATE interposer, so the
    supervisor (ptrace) round-trip count is 0 (traps=0 rewrites=0 for general
    apps). The per-path CPU cost is the cold `translate_rootfs_path`, amortized
    by the 256-entry `xlate_cache` (guest_path → host_path memoize) so repeated
    paths cost ~the cache-hit price, not the cold 4.3 µs.

Explicitly OUT of scope here:
  * non-path syscalls — seccomp ALLOW, i.e. 0 mediation overhead.
  * syscall-storm raw `svc` (chromium-class) — not hookable by an in-process
    interposer; that is the separate RET_TRACE / out-of-process ptrace "wall"
    tracked elsewhere, not a path-mediation cost.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class MediationCost:
    """A single path-mediation cost point: cold path-translate vs raw syscall."""

    xlate_ns: float
    syscall_ns: float

    @property
    def syscall_units(self) -> float:
        """Cold path-translate cost expressed in raw-syscall units (xlate/syscall)."""
        if self.syscall_ns <= 0:
            raise ValueError(f"syscall_ns must be > 0, got {self.syscall_ns}")
        return self.xlate_ns / self.syscall_ns


def amortized_xlate_ns(
    xlate_ns: float,
    *,
    hit_ratio: float,
    hit_ns: float = 0.0,
) -> float:
    """Effective per-path cost under `xlate_cache` memoization.

    A fraction `hit_ratio` of path lookups hit the cache (cost `hit_ns`, ~0),
    the rest pay the cold `xlate_ns`:

        effective = (1 - hit_ratio) * xlate_ns + hit_ratio * hit_ns

    `hit_ratio` must be in [0, 1].
    """
    if not 0.0 <= hit_ratio <= 1.0:
        raise ValueError(f"hit_ratio must be in [0, 1], got {hit_ratio}")
    return (1.0 - hit_ratio) * xlate_ns + hit_ratio * hit_ns


@dataclass(frozen=True)
class MediationVerdict:
    """Supervisor round-trip verdict for a guest run."""

    traps: int
    supervisor_roundtrip_zero: bool
    detail: str


def assess_mediation(traps: int, rewrites: int) -> MediationVerdict:
    """Classify a guest run's path mediation by its `traps` / `rewrites` counts.

    The PCGATE interposer rewrites path syscalls in-process, so a healthy
    general app shows traps=0 (zero ptrace round-trip). traps>0 means that many
    syscalls escaped to the supervisor.
    """
    zero = traps == 0
    if zero:
        detail = (
            f"zero ptrace round-trip; path syscalls rewritten in-process "
            f"(rewrites={rewrites})"
        )
    else:
        detail = f"{traps} supervisor round-trips (rewrites={rewrites})"
    return MediationVerdict(traps=traps, supervisor_roundtrip_zero=zero, detail=detail)


def to_markdown(
    cost: MediationCost,
    *,
    hit_ratio: float | None = None,
    verdict: MediationVerdict | None = None,
) -> str:
    """Render a small markdown table for a path-mediation cost point."""
    lines = [
        "### Path-mediation overhead",
        "",
        "| metric | value |",
        "|--------|-------|",
        f"| xlate ns/op (cold) | {cost.xlate_ns:,.3f} |",
        f"| raw syscall ns/op | {cost.syscall_ns:,.3f} |",
        f"| syscall units (xlate/syscall) | {cost.syscall_units:.2f} |",
    ]
    if hit_ratio is not None:
        eff = amortized_xlate_ns(cost.xlate_ns, hit_ratio=hit_ratio)
        lines.append(
            f"| amortized ns/op @ hit_ratio={hit_ratio:.2f} | {eff:,.3f} |"
        )
    if verdict is not None:
        v = "ZERO" if verdict.supervisor_roundtrip_zero else f"{verdict.traps}"
        lines.append(f"| supervisor round-trip | **{v}** — {verdict.detail} |")
    return "\n".join(lines) + "\n"
