"""Thin, OPT-IN adb helpers for device benchmark runs. No import side effects.

WARNING — shared hardware. The physical device is shared across all 5 sessions.
This module NEVER installs/uninstalls an APK and NEVER force-stops the app on import
or by accident. Device benchmark runs must be coordinated (see
docs/research/ws5-verification-bench-status.md "디바이스 사용 정책"): run them against a
merged integration build while holding the device, not from a per-WS worktree.

The native-baseline timer runs a binary inside a SINGLE `adb shell` invocation with an
in-shell loop so the fixed adb round-trip is amortized across `repeats` — the result is
a wall-clock-per-sample estimate, not a kernel-precise syscall latency.
"""
from __future__ import annotations

import shlex
import subprocess
import time

from .cpu_overhead import Measurement


def _adb(serial: str | None, args: list[str], timeout: float = 60.0) -> subprocess.CompletedProcess:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += args
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def list_devices() -> list[str]:
    """Return attached adb serials in `device` state (read-only)."""
    out = _adb(None, ["devices"]).stdout.splitlines()
    serials = []
    for line in out[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            serials.append(parts[0])
    return serials


def time_native(serial: str, guest_argv: list[str], repeats: int = 1000, *, label: str = "native") -> Measurement:
    """Wall-clock the given binary run `repeats` times in one adb shell loop.

    `guest_argv` is the on-device command (e.g. ["/data/local/tmp/microbench"]). Read-only:
    runs an existing binary, installs nothing. Caller is responsible for having staged it.
    """
    if repeats <= 0:
        raise ValueError("repeats must be positive")
    inner = " ".join(shlex.quote(a) for a in guest_argv)
    script = f"i=0; while [ $i -lt {repeats} ]; do {inner} >/dev/null 2>&1; i=$((i+1)); done"
    t0 = time.perf_counter_ns()
    cp = _adb(serial, ["shell", script], timeout=600.0)
    elapsed = time.perf_counter_ns() - t0
    if cp.returncode != 0:
        raise RuntimeError(f"adb shell native timing failed (rc={cp.returncode}): {cp.stderr.strip()}")
    return Measurement(label=label, wall_ns=elapsed, samples=repeats)


def dump_logcat_report(serial: str, *, tag: str = "AndroLinux", clear_first: bool = False) -> str:
    """Read the current logcat buffer filtered to the app tag (read-only).

    Does NOT clear by default (another session may be mid-capture). The caller decides
    when to `clear_first` after coordinating device ownership.
    """
    if clear_first:
        _adb(serial, ["logcat", "-c"])
    return _adb(serial, ["logcat", "-d", "-s", tag]).stdout
