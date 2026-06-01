"""Parse an ALR APK execution report (MainActivity summary / logcat dump) into
structured markers. Pure, no I/O — host-testable without a device.

The marker strings mirror what `runtime_report.cpp` emits on-device. This module
only READS them; it never edits the C++/Kotlin sources (WS-1/2/3 ownership). If a
marker string changes upstream, update the regexes here only.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field


@dataclass(frozen=True)
class GuestExec:
    """`ALR NATIVE LOADER GUEST EXEC: PASS` + `child exit=N signal=M` + stdout."""

    passed: bool
    exit_code: int | None = None
    signal: int | None = None
    stdout: str | None = None


@dataclass(frozen=True)
class PathMediation:
    """`alr native loader path-mediation traps=N rewrites=M` + pcgate/interpose flags."""

    traps: int | None = None
    rewrites: int | None = None
    pcgate: bool | None = None
    interpose: bool | None = None


@dataclass(frozen=True)
class GpuBoundary:
    """`alr gpu boundary <kind> ns/op=N` (None when skipped/absent)."""

    inproc_dispatch_ns: int | None = None
    socket_per_cmd_ns: int | None = None
    shmem_ring_ns: int | None = None


@dataclass(frozen=True)
class GuestProbe:
    """A single `gimp-probe guest=<path> exit=N` line."""

    guest: str
    exit_code: int


@dataclass(frozen=True)
class WlOutput:
    """Compositor `client bound: wl_output vN (WxH px, WxH mm, scale=S, dpi=D)`."""

    version: int | None = None
    width_px: int | None = None
    height_px: int | None = None
    width_mm: int | None = None
    height_mm: int | None = None
    scale: int | None = None
    dpi: float | None = None


@dataclass(frozen=True)
class ParsedReport:
    build_stamp: str | None
    guest_exec: GuestExec | None
    path_mediation: PathMediation
    gpu_boundary: GpuBoundary
    seccomp_trace_events: int | None
    sc_path_rewrites: int | None
    perf_harness_pass: bool | None
    guest_probes: tuple[GuestProbe, ...] = field(default_factory=tuple)
    raw: str = ""
    alr_markers: dict[str, str] = field(default_factory=dict)
    wl_output: "WlOutput | None" = None


_BUILD = re.compile(r"^build:\s*(\S+)", re.MULTILINE)
_GUEST_EXEC = re.compile(r"ALR NATIVE LOADER GUEST EXEC:\s*(PASS|FAIL)")
_CHILD = re.compile(r"child exit=(\d+)\s+signal=(\d+)")
_GUEST_STDOUT = re.compile(r"^guest stdout=(.*)$", re.MULTILINE)
_PATH_MED = re.compile(r"alr native loader path-mediation traps=(\d+)\s+rewrites=(\d+)")
# Authoritative gimp-probe summary line, e.g.
# "all: pcgate=1 interpose=1 traps=0 rewrites=0"
_ALL_SUMMARY = re.compile(
    r"all:\s*pcgate=(\d+)\s+interpose=(\d+)\s+traps=(\d+)\s+rewrites=(\d+)"
)
_PCGATE = re.compile(r"pcgate=(\d+)")
_INTERPOSE = re.compile(r"interpose=(\d+)")
_SC_TRACE = re.compile(r"alr sc seccomp_trace_events=(\d+)")
_SC_REWRITES = re.compile(r"alr sc path_rewrites=(\d+)")
_GPU_INPROC = re.compile(r"alr gpu boundary inproc dispatch ns/op=(\d+)")
_GPU_SOCKET = re.compile(r"alr gpu boundary socket per-cmd ns/op=(\d+)")
_GPU_SHMEM = re.compile(r"alr gpu boundary shmem-ring ns/op=(\d+)")
_PERF_HARNESS = re.compile(r"ALR PERF HARNESS:\s*(\S+)")
_GIMP_PROBE = re.compile(r"gimp-probe guest=(\S+)\s+exit=(\d+)")
# Generic device marker: `ALR <name>: <status>`. Names may carry spaces/parens
# but no internal colon (the first colon ends the name). Future-proofs the parser
# against the growing list of `ALR <name>: PASS` markers runtime_report.cpp emits.
_ALR_MARKER = re.compile(r"^(ALR [^:]+):\s*(.+?)\s*$", re.MULTILINE)
_WL_OUTPUT = re.compile(
    r"client bound: wl_output v(\d+) "
    r"\((\d+)x(\d+) px, (\d+)x(\d+) mm, scale=(\d+), dpi=([\d.]+)\)"
)


def _int(pat: re.Pattern[str], text: str, group: int = 1) -> int | None:
    m = pat.search(text)
    return int(m.group(group)) if m else None


def _flag(pat: re.Pattern[str], text: str) -> bool | None:
    m = pat.search(text)
    return None if m is None else (m.group(1) != "0")


def parse_report(text: str) -> ParsedReport:
    """Parse a full report dump into a ParsedReport. Missing markers stay None."""
    build = _BUILD.search(text)

    guest_exec: GuestExec | None = None
    m_exec = _GUEST_EXEC.search(text)
    if m_exec is not None:
        m_child = _CHILD.search(text)
        m_out = _GUEST_STDOUT.search(text)
        guest_exec = GuestExec(
            passed=m_exec.group(1) == "PASS",
            exit_code=int(m_child.group(1)) if m_child else None,
            signal=int(m_child.group(2)) if m_child else None,
            stdout=m_out.group(1).strip() if m_out else None,
        )

    # The `all:` gimp-probe summary line is authoritative for the regression invariant
    # (it carries pcgate/interpose/traps/rewrites together). Fall back to the standalone
    # `alr native loader path-mediation` line for traps/rewrites when no summary is present.
    m_all = _ALL_SUMMARY.search(text)
    if m_all is not None:
        path_med = PathMediation(
            traps=int(m_all.group(3)),
            rewrites=int(m_all.group(4)),
            pcgate=m_all.group(1) != "0",
            interpose=m_all.group(2) != "0",
        )
    else:
        m_pm = _PATH_MED.search(text)
        path_med = PathMediation(
            traps=int(m_pm.group(1)) if m_pm else None,
            rewrites=int(m_pm.group(2)) if m_pm else None,
            pcgate=_flag(_PCGATE, text),
            interpose=_flag(_INTERPOSE, text),
        )

    gpu = GpuBoundary(
        inproc_dispatch_ns=_int(_GPU_INPROC, text),
        socket_per_cmd_ns=_int(_GPU_SOCKET, text),
        shmem_ring_ns=_int(_GPU_SHMEM, text),
    )

    m_perf = _PERF_HARNESS.search(text)

    probes = tuple(
        GuestProbe(guest=g, exit_code=int(e)) for g, e in _GIMP_PROBE.findall(text)
    )

    alr_markers = {
        name.strip(): status for name, status in _ALR_MARKER.findall(text)
    }

    wl_output: WlOutput | None = None
    m_wl = _WL_OUTPUT.search(text)
    if m_wl is not None:
        wl_output = WlOutput(
            version=int(m_wl.group(1)),
            width_px=int(m_wl.group(2)),
            height_px=int(m_wl.group(3)),
            width_mm=int(m_wl.group(4)),
            height_mm=int(m_wl.group(5)),
            scale=int(m_wl.group(6)),
            dpi=float(m_wl.group(7)),
        )

    return ParsedReport(
        build_stamp=build.group(1) if build else None,
        guest_exec=guest_exec,
        path_mediation=path_med,
        gpu_boundary=gpu,
        seccomp_trace_events=_int(_SC_TRACE, text),
        sc_path_rewrites=_int(_SC_REWRITES, text),
        perf_harness_pass=(m_perf.group(1) == "PASS") if m_perf else None,
        guest_probes=probes,
        raw=text,
        alr_markers=alr_markers,
        wl_output=wl_output,
    )
