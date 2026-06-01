"""Verify CP-4 zero-copy present (zwp_linux_dmabuf / AHardwareBuffer external-OES).

This module verifies the CP-4 checkpoint: an ``AHardwareBuffer`` imported as an
``EGLImage`` and sampled as an ``external-OES`` texture, then presented onto the
real device display with no intermediate copy (zero-copy dmabuf-style present).

Device evidence (drain #5, device-verified on SM-X236N / Mali-G615):
``docs/evidence/2026-06-01-drain5-cp4-dmabuf-present-single-gate.md``. The real
device markers it captured are:

  * ``ALR AHB ZEROCOPY IMPORT: PASS`` — AHardwareBuffer -> EGLImage ->
    external-OES import succeeded.
  * ``gtkdemo-result: rendered=true frames=12->13`` — gtk3demo's GUI was
    presented via the WS-3 WaylandPresenter -> compositor -> present path and
    the frame counter advanced (12 -> 13). The separator in the real log is a
    unicode arrow ``->``; this module's regex is tolerant of ``->`` / ``->`` /
    ``to``.

Verdict for CP-4 zero-copy present is: AHB zero-copy import PASS **and** the
gtkdemo result rendered=true **and** the frame counter advanced (end > start).

Note: the external-OES self-test line ``pixel=0,0,0`` is **info-only** here — it
is not part of the CP-4 pass/fail decision. v114 separately device-verified that
external-OES content actually displays; CP-4 reuses that proven display path.

This module is intentionally SELF-CONTAINED: it owns its own regexes and does
NOT import ``bench.report_parse`` (that sibling module may be under concurrent
edit). Pure, no I/O — host-testable.
"""
from __future__ import annotations

import re
from dataclasses import dataclass


# Own regexes (deliberately not shared with report_parse).
#
# AHB zero-copy import marker — PASS/FAIL:
#   ALR AHB ZEROCOPY IMPORT: PASS
_AHB_IMPORT = re.compile(r"ALR AHB ZEROCOPY IMPORT:\s*(PASS|FAIL)")

# gtkdemo present-result marker, tolerant of the arrow separator:
#   gtkdemo-result: rendered=true frames=12→13   (real log: unicode arrow)
#   gtkdemo-result: rendered=true frames=12->13    (ascii arrow)
#   gtkdemo-result: rendered=true frames=12 to 13  (word form)
_GTKDEMO_RESULT = re.compile(
    r"gtkdemo-result:\s*rendered=(true|false)\s+frames=(\d+)\s*(?:→|->|to)\s*(\d+)"
)


def parse_ahb_zerocopy_import(text: str) -> bool | None:
    """Return the AHB zero-copy import result from a report/log dump.

    ``True``  iff ``ALR AHB ZEROCOPY IMPORT: PASS`` is present;
    ``False`` iff ``ALR AHB ZEROCOPY IMPORT: FAIL`` is present;
    ``None``  if the marker is absent entirely.
    """
    m = _AHB_IMPORT.search(text)
    if m is None:
        return None
    return m.group(1) == "PASS"


def parse_gtkdemo_result(text: str) -> dict | None:
    """Extract the gtk3demo present result from a report/log dump.

    Matches ``gtkdemo-result: rendered=<true|false> frames=<start><arrow><end>``
    where ``<arrow>`` is a unicode arrow ``->``, an ascii ``->``, or the word
    ``to``. Returns a dict with keys ``rendered`` (bool), ``frames_start`` (int)
    and ``frames_end`` (int), or ``None`` if the marker is absent.
    """
    m = _GTKDEMO_RESULT.search(text)
    if m is None:
        return None
    return {
        "rendered": m.group(1) == "true",
        "frames_start": int(m.group(2)),
        "frames_end": int(m.group(3)),
    }


@dataclass(frozen=True)
class PresentVerdict:
    """Outcome of CP-4 zero-copy present verification.

    Each component is tri-state: ``None`` means "not present in the report"
    (so it could not be checked), ``True``/``False`` mean checked. ``passed`` is
    only ``True`` when all three of ``import_ok``, ``rendered`` and
    ``frames_advanced`` are ``True``; any missing or failing component makes it
    ``False``. ``detail`` is explicit about what was missing or failed.
    """

    import_ok: bool | None
    rendered: bool | None
    frames_advanced: bool | None
    passed: bool
    detail: str

    def to_markdown(self) -> str:
        def cell(v: bool | None) -> str:
            if v is None:
                return "MISSING"
            return "PASS" if v else "FAIL"

        overall = "PASS" if self.passed else "FAIL"
        return (
            f"### CP-4 zero-copy present verification: **{overall}**\n\n"
            f"| Check | Result |\n"
            f"| --- | --- |\n"
            f"| AHB zero-copy import | {cell(self.import_ok)} |\n"
            f"| gtkdemo rendered | {cell(self.rendered)} |\n"
            f"| Frames advanced | {cell(self.frames_advanced)} |\n\n"
            f"{self.detail}\n"
        )


def verify_present(text: str) -> PresentVerdict | None:
    """Parse the CP-4 markers from a report/log dump and produce a verdict.

    Looks for both the ``ALR AHB ZEROCOPY IMPORT`` marker and the
    ``gtkdemo-result`` marker. Returns ``None`` only if **neither** is present.

    ``passed`` is ``True`` iff ``import_ok is True`` AND ``rendered is True`` AND
    ``frames_advanced is True``, where ``frames_advanced = frames_end >
    frames_start``. When only one marker is present, the components belonging to
    the missing marker are set to ``None`` and ``passed`` is ``False`` (a partial
    verdict). In particular, an AHB import PASS with the gtkdemo marker absent
    still yields a (not-passed) partial verdict noting the gtkdemo result is
    missing.
    """
    import_ok = parse_ahb_zerocopy_import(text)
    gtk = parse_gtkdemo_result(text)

    if import_ok is None and gtk is None:
        return None

    if gtk is None:
        rendered = None
        frames_advanced = None
    else:
        rendered = gtk["rendered"]
        frames_advanced = gtk["frames_end"] > gtk["frames_start"]

    passed = (
        import_ok is True and rendered is True and frames_advanced is True
    )

    # Build an explicit detail string covering each component.
    notes: list[str] = []

    if import_ok is None:
        notes.append("AHB zero-copy import marker MISSING")
    elif import_ok:
        notes.append("AHB zero-copy import PASS")
    else:
        notes.append("AHB zero-copy import FAIL")

    if gtk is None:
        notes.append("gtkdemo-result marker MISSING")
    else:
        notes.append(
            f"gtkdemo rendered={'true' if rendered else 'false'}, "
            f"frames {gtk['frames_start']}->{gtk['frames_end']} "
            f"({'advanced' if frames_advanced else 'did NOT advance'})"
        )

    detail = "; ".join(notes)

    return PresentVerdict(
        import_ok=import_ok,
        rendered=rendered,
        frames_advanced=frames_advanced,
        passed=passed,
        detail=detail,
    )
