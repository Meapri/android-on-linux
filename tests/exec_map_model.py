"""ADR-003 §6 P1 (M-R4-execmap) — chromium multiprocess exec/clone decomposition.

Pure, host-testable (no device, no I/O). Parses the two logcat lines the
M-R4-execmap supervisor instrumentation emits (runtime_report.cpp, WS-1 —
EVENT_EXEC/EVENT_CLONE counters + each exec's x0 path, read-only, new ptrace
op 0) and classifies chromium's child-launch graph so the moment a device
capture lands we can render ADR-003's "auto-mediated (clone) vs exec-wall
(exec)" split *without another device round*:

    alr exec clone_events=<n> exec_events=<m>
    alr exec x0=<path>            (one per fresh-execve child)

What this answers from a single device capture (ADR-003 §2, §4 가정-3):

  1. clone:exec ratio. zygote-fork (clone, no exec) renderers are ALREADY
     mediated (address-space copy + inherited seccomp + SEIZE trace, ADR-003
     §2-A). Only fresh-execve children (zygote/gpu) are the real (B) exec wall.
     A high clone:exec ratio means most of chromium is auto-mediated.

  2. Each exec x0 path bucket: {rootfs-내 절대, rootfs-외 절대, /proc/self/exe,
     상대}. ADR-003 가정-3: if chromium execs /proc/self/exe the exec target is
     a host image (not the ALR guest map) and the §3 x0-rewrite design breaks.
     The (B-1) x0-rewrite implementation is gated on this bucket showing the
     exec children use rootfs-내 절대경로 (NOT /proc/self/exe).

This module never edits the C++/Kotlin sources. It is the host-side, unit-tested
core of the ADR-003 P1 prototype; the darwin host cannot exercise real
seccomp-across-execve / SEIZE-EVENT_EXEC inheritance, so only the classification
arithmetic lives here (effects are DEVICE-ONLY: M-R4-execmap gate).

Location note: the ADR-003 §6 sketch placed this parser under ``bench/`` but the
WS-5 round-6 scope is tests/+docs/research only, so the pure helper lives beside
``tests/svc_match.py`` (the ADR-002 svc-rewrite core, same pattern) as
``tests/exec_map_model.py`` and is imported by ``tests/test_exec_map.py``.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

# Supervisor marker regexes (ADD-only, local to this module so a WS-1 format
# tweak cannot silently break report_parse's shared helpers).
_CLONE_EXEC_RE = re.compile(
    r"alr exec clone_events=(\d+)\s+exec_events=(\d+)"
)
_EXEC_X0_RE = re.compile(r"alr exec x0=(\S+)")

# Path buckets for an exec target (ADR-003 §3 / §4 가정-3).
BUCKET_PROC_SELF_EXE = "proc_self_exe"   # /proc/self/exe — design-breaking (host image)
BUCKET_ROOTFS_ABS = "rootfs_abs"         # absolute path inside rootfs mapping (x0-rewritable)
BUCKET_OTHER_ABS = "other_abs"           # absolute, but outside rootfs (needs rewrite/synthesis)
BUCKET_RELATIVE = "relative"             # relative path (resolved vs cwd)

_KERNEL_VIRTUAL_PREFIXES = ("/proc", "/sys", "/dev")


def classify_exec_path(path: str, rootfs_dir: str = "/data/rootfs") -> str:
    """Bucket a single exec x0 path the way the supervisor's mediation would see it.

    Mirrors runtime_report.cpp's path-mediation predicates (L2122-2135):
      - /proc/self/exe is special: kernel virtual symlink → host image (가정-3).
      - absolute path under rootfs_dir = already-host (idempotency, rewritable).
      - other absolute = rootfs-외 절대 (translate_rootfs_path candidate).
      - anything else = relative.
    """
    if not path:
        return BUCKET_RELATIVE
    # /proc/self/exe (and any /proc/<pid>/exe) is the design-breaking case.
    if path == "/proc/self/exe" or re.fullmatch(r"/proc/\d+/exe", path):
        return BUCKET_PROC_SELF_EXE
    if not path.startswith("/"):
        return BUCKET_RELATIVE

    def _under(p: str, d: str) -> bool:
        if not p.startswith(d):
            return False
        tail = p[len(d):]
        return tail == "" or tail.startswith("/")

    # rootfs-내 절대경로 = the rootfs-host mapping (x0-rewrite target / idempotent).
    if _under(path, rootfs_dir):
        return BUCKET_ROOTFS_ABS
    # Other kernel-virtual dirs stay native (not rewritten) but are not the
    # design-breaking /proc/self/exe; classify by their absoluteness as other_abs.
    return BUCKET_OTHER_ABS


@dataclass(frozen=True)
class ExecMap:
    """Decomposition of a chromium multiprocess launch graph (ADR-003 §2)."""

    clone_events: int
    exec_events: int
    exec_paths: tuple[str, ...] = ()
    buckets: dict[str, int] = field(default_factory=dict)

    @property
    def total_children(self) -> int:
        return self.clone_events + self.exec_events

    @property
    def auto_mediated_fraction(self) -> float:
        """Fraction of child launches that are clone (already mediated, ADR-003 §2-A)."""
        total = self.total_children
        if total == 0:
            return 0.0
        return self.clone_events / total

    @property
    def has_proc_self_exe(self) -> bool:
        """True if any exec target is /proc/self/exe → ADR-003 §3 design breaks (가정-3)."""
        return self.buckets.get(BUCKET_PROC_SELF_EXE, 0) > 0

    @property
    def x0_rewrite_unblocked(self) -> bool:
        """ADR-003 §5 gate for (B-1) x0-rewrite *implementation*.

        Unblocked only if there is at least one fresh-execve child AND none of
        the exec targets is /proc/self/exe (가정-3 must pass): the exec children
        must use rootfs-내 절대경로 so x0-rewrite points at an ALR guest image.
        """
        if self.exec_events == 0:
            return False  # nothing to rewrite (e.g. --single-process)
        if self.has_proc_self_exe:
            return False  # exec target is a host image, not the guest
        return self.buckets.get(BUCKET_ROOTFS_ABS, 0) > 0


def parse_exec_map(text: str, rootfs_dir: str = "/data/rootfs") -> ExecMap:
    """Parse the M-R4-execmap supervisor lines into an ExecMap decomposition.

    Tolerant: missing counter line → zeros; multiple x0 lines accumulate.
    """
    clone_events = 0
    exec_events = 0
    m = _CLONE_EXEC_RE.search(text)
    if m:
        clone_events = int(m.group(1))
        exec_events = int(m.group(2))

    paths = tuple(_EXEC_X0_RE.findall(text))
    buckets: dict[str, int] = {}
    for p in paths:
        b = classify_exec_path(p, rootfs_dir)
        buckets[b] = buckets.get(b, 0) + 1

    return ExecMap(
        clone_events=clone_events,
        exec_events=exec_events,
        exec_paths=paths,
        buckets=buckets,
    )


def render_markdown(em: ExecMap) -> str:
    """Render the decomposition as a small markdown block (ADR-003 §6 P1 output)."""
    lines = [
        "## chromium exec/clone decomposition (ADR-003 M-R4-execmap)",
        "",
        f"- clone_events (auto-mediated, ADR-003 §2-A): **{em.clone_events}**",
        f"- exec_events (fresh-execve wall, ADR-003 §2-B): **{em.exec_events}**",
        f"- auto-mediated fraction: **{em.auto_mediated_fraction:.2%}**",
        "",
        "| exec x0 bucket | count |",
        "|----------------|-------|",
    ]
    for b in (BUCKET_ROOTFS_ABS, BUCKET_OTHER_ABS, BUCKET_PROC_SELF_EXE, BUCKET_RELATIVE):
        lines.append(f"| {b} | {em.buckets.get(b, 0)} |")
    lines.append("")
    verdict = (
        "x0-rewrite UNBLOCKED (exec children use rootfs 절대경로)"
        if em.x0_rewrite_unblocked
        else (
            "x0-rewrite BLOCKED: /proc/self/exe exec → host image (ADR-003 §4 가정-3)"
            if em.has_proc_self_exe
            else "x0-rewrite n/a (no fresh-execve child — e.g. --single-process)"
        )
    )
    lines.append(f"verdict: {verdict}")
    return "\n".join(lines)
