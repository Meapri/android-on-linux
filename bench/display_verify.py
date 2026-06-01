"""Verify the ALR compositor's advertised display against the CP-1 device-exact
panel (Samsung SM-X236N: 1200x1920 @ 90Hz). Pure, no I/O — host-testable.

This module is intentionally SELF-CONTAINED: it owns its own regex for the
compositor's `client bound: wl_output ...` line and does NOT import
``bench.report_parse`` (that sibling module is under concurrent edit).

CP-1 device-exact display
-------------------------
The compositor advertises a ``wl_output`` mode whose refresh is carried in mHz
(90000 mHz == 90 Hz) and logs a line of the form::

    client bound: wl_output v2 (1200x1920 px, 70x111 mm, scale=2, dpi=440)

Resolution and DPI are recoverable from that logged line. The *refresh* value,
however, is sent to the wl client and is NOT echoed into that per-client line.

WS-3 / integration added a second, authoritative marker that DOES carry refresh::

    display: 1920x1200 @ 90000mHz density=213

This ``display:`` marker (emitted by the compositor once it has the real panel
geometry) carries BOTH resolution and refresh (90000 mHz == 90 Hz), so when it is
present refresh is **verified**. ``verify_from_report`` prefers it and only falls
back to the per-client ``client bound: wl_output`` line (refresh unverified) when
no ``display:`` marker is present. When refresh cannot be verified at all, this
module reports it as **unverified** (``refresh_verified=False``, ``refresh_ok=True``)
rather than failing — a missing refresh marker is never a hard fail. Resolution
matching is rotation-agnostic (unordered W/H compare).
"""
from __future__ import annotations

import re
from dataclasses import dataclass


@dataclass(frozen=True)
class DisplayExpectation:
    """The device-exact display we expect the compositor to advertise."""

    width_px: int
    height_px: int
    refresh_hz: int


#: CP-1: Samsung SM-X236N panel, device-exact.
DEVICE_EXACT = DisplayExpectation(1200, 1920, 90)


# Own regex (deliberately not shared with report_parse) for:
#   client bound: wl_output v2 (1200x1920 px, 70x111 mm, scale=2, dpi=440)
_WL_OUTPUT = re.compile(
    r"client bound: wl_output v(\d+) "
    r"\((\d+)x(\d+) px, (\d+)x(\d+) mm, scale=(\d+), dpi=([\d.]+)\)"
)

# Authoritative WS-3/integration marker carrying resolution AND refresh:
#   display: 1920x1200 @ 90000mHz density=213
# density is optional.
_DISPLAY_MARKER = re.compile(
    r"display:\s*(\d+)x(\d+)\s*@\s*(\d+)mHz(?:\s+density=(\d+))?"
)


def parse_display_marker(text: str) -> dict | None:
    """Extract the compositor's ``display:`` marker from a report/log dump.

    Matches lines of the form ``display: 1920x1200 @ 90000mHz density=213``
    (``density=`` optional). Returns a dict with keys ``width_px, height_px,
    refresh_mhz, density`` (ints; ``density`` is ``None`` when absent), or
    ``None`` if no ``display:`` marker is present.

    Unlike the per-client ``client bound: wl_output`` line, this marker carries
    the refresh value (in milli-Hz), so it lets refresh be *verified*.
    """
    m = _DISPLAY_MARKER.search(text)
    if m is None:
        return None
    density = m.group(4)
    return {
        "width_px": int(m.group(1)),
        "height_px": int(m.group(2)),
        "refresh_mhz": int(m.group(3)),
        "density": int(density) if density is not None else None,
    }


def parse_wl_output_line(text: str) -> dict | None:
    """Extract the compositor's advertised output geometry from a report/log dump.

    Returns a dict with keys ``version, width_px, height_px, width_mm,
    height_mm, scale, dpi`` (ints, except ``dpi`` which is a float), or ``None``
    if no ``client bound: wl_output`` line is present.
    """
    m = _WL_OUTPUT.search(text)
    if m is None:
        return None
    return {
        "version": int(m.group(1)),
        "width_px": int(m.group(2)),
        "height_px": int(m.group(3)),
        "width_mm": int(m.group(4)),
        "height_mm": int(m.group(5)),
        "scale": int(m.group(6)),
        "dpi": float(m.group(7)),
    }


@dataclass(frozen=True)
class DisplayVerdict:
    """Outcome of comparing an advertised display against an expectation.

    ``refresh_verified`` distinguishes "refresh checked and correct" from
    "refresh not yet verifiable" (no logged marker). When refresh is
    unverified, ``refresh_ok`` is ``True`` so it does not drag ``passed`` down.
    """

    resolution_ok: bool
    refresh_ok: bool
    refresh_verified: bool
    dpi: float | None
    detail: str
    passed: bool

    def to_markdown(self) -> str:
        res = "PASS" if self.resolution_ok else "FAIL"
        if not self.refresh_verified:
            ref = "UNVERIFIED"
        else:
            ref = "PASS" if self.refresh_ok else "FAIL"
        overall = "PASS" if self.passed else "FAIL"
        dpi = f"{self.dpi:g}" if self.dpi is not None else "n/a"
        return (
            f"### CP-1 display verification: **{overall}**\n\n"
            f"| Check | Result |\n"
            f"| --- | --- |\n"
            f"| Resolution | {res} |\n"
            f"| Refresh | {ref} |\n"
            f"| DPI | {dpi} |\n\n"
            f"{self.detail}\n"
        )


def verify_display(
    width_px: int,
    height_px: int,
    *,
    refresh_mhz: int | None = None,
    dpi: float | None = None,
    expected: DisplayExpectation = DEVICE_EXACT,
) -> DisplayVerdict:
    """Compare an advertised display against ``expected`` (default: CP-1 device-exact).

    Resolution match is rotation-agnostic: the unordered pair {width, height}
    must equal the expected pair. Refresh (``refresh_mhz``, milli-Hz) is verified
    only when supplied; absent, it is reported as unverified (never a hard fail).
    """
    resolution_ok = {width_px, height_px} == {expected.width_px, expected.height_px}

    if refresh_mhz is None:
        refresh_verified = False
        refresh_ok = True
        refresh_note = "refresh unverified — no report marker"
    else:
        refresh_verified = True
        refresh_hz = round(refresh_mhz / 1000)
        refresh_ok = refresh_hz == expected.refresh_hz
        refresh_note = (
            f"refresh {refresh_hz}Hz "
            f"({'==' if refresh_ok else '!='} expected {expected.refresh_hz}Hz)"
        )

    passed = resolution_ok and refresh_ok

    res_note = (
        f"resolution {width_px}x{height_px} "
        f"({'matches' if resolution_ok else 'does NOT match'} "
        f"expected {expected.width_px}x{expected.height_px}, rotation-agnostic)"
    )
    dpi_note = f"; dpi={dpi:g}" if dpi is not None else ""
    detail = f"{res_note}; {refresh_note}{dpi_note}"

    return DisplayVerdict(
        resolution_ok=resolution_ok,
        refresh_ok=refresh_ok,
        refresh_verified=refresh_verified,
        dpi=dpi,
        detail=detail,
        passed=passed,
    )


def verify_from_report(
    text: str, expected: DisplayExpectation = DEVICE_EXACT
) -> DisplayVerdict | None:
    """Parse a report/log dump's display marker and verify it.

    Tries the authoritative ``display: WxH @ RmHz density=D`` marker FIRST: it
    carries refresh, so the resulting verdict has ``refresh_verified=True``.
    If that marker is absent, falls back to the per-client ``client bound:
    wl_output`` line (which carries no refresh → ``refresh_verified=False``).

    Returns ``None`` if neither marker is present.
    """
    display = parse_display_marker(text)
    if display is not None:
        return verify_display(
            display["width_px"],
            display["height_px"],
            refresh_mhz=display["refresh_mhz"],
            dpi=None,
            expected=expected,
        )

    parsed = parse_wl_output_line(text)
    if parsed is None:
        return None
    return verify_display(
        parsed["width_px"],
        parsed["height_px"],
        dpi=parsed["dpi"],
        expected=expected,
    )
