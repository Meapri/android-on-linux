"""WS-5 guard: the compat matrix must reflect drain#10 (v132, 6-WS breadth fan-out).

drain#10 (`docs/evidence/2026-06-02-breadth-fanout-drain.md`) device-confirmed:
  - babl-gegl overlay STAGED (67 GIMP op .so, 0755 → dlopen-able); GIMP filter
    device exercise is still PENDING.
  - qt6 / sdl2 / netsurf overlays STAGED (runtime libs) but the *launchable* binary
    is still PENDING (the smoke probe reported "missing" — libs present, no CLI/demo
    binary). This refines the prior "staged-but-launch-pending" wording.
  - WS-1 CP-6 M2 reduced path-mediation traps (gtk3-widget-factory 135 -> 99).
  - GPU LIVE + THROUGHPUT + SCREEN-CUBE PASS, glmark2 1009, no regression.

These lenient checks pin those facts into the matrix without being brittle about
the exact phrasing, and keep the PENDING caveats honest (no premature "works").
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"
DRAIN10_STEM = "2026-06-02-breadth-fanout-drain"


def _text() -> str:
    assert MATRIX.is_file(), f"compat matrix missing: {MATRIX}"
    return MATRIX.read_text(encoding="utf-8")


def test_matrix_cites_drain10_evidence():
    text = _text()
    assert DRAIN10_STEM in text, (
        "compat matrix must cite the drain#10 breadth fan-out evidence doc "
        f"({DRAIN10_STEM}.md)"
    )
    # The cited evidence file must actually exist on disk.
    assert (ROOT / "docs" / "evidence" / f"{DRAIN10_STEM}.md").is_file(), (
        "drain#10 evidence doc cited by the matrix does not exist on disk"
    )


def test_matrix_records_babl_gegl_staged_but_filter_pending():
    """babl/gegl 67 op modules are STAGED (dlopen-able), filter device use PENDING."""
    text = _text()
    lines = text.splitlines()
    # A row mentioning babl/gegl that records the 67-module staging.
    staged = [
        ln for ln in lines
        if (("babl" in ln.lower()) or ("gegl" in ln.lower()))
        and ("67" in ln)
        and ("STAGED" in ln)
    ]
    assert staged, (
        "matrix must record the babl-gegl overlay STAGED (67 op .so) row from drain#10"
    )
    # And it must stay honest that the actual GIMP filter device exercise is PENDING.
    honest = [
        ln for ln in lines
        if (("babl" in ln.lower()) or ("gegl" in ln.lower())) and ("PENDING" in ln)
    ]
    assert honest, (
        "matrix must keep the babl/gegl GIMP-filter device exercise honestly PENDING"
    )


def test_toolkit_overlays_staged_launchable_pending():
    """qt6 / sdl2: overlay STAGED but GUI demo not yet launched on device.

    Each must appear on a row that says STAGED (the libs are there) yet stops short
    of a device render. Round-4 reworded the launch caveat from "launchable binary
    PENDING" to "GUI 데모 launch 진행중(round-4)", so accept either phrasing as long
    as the row does NOT overclaim a render. (netsurf was promoted to RENDERS in
    drain#12, so it is intentionally dropped from this still-staged set — see
    test_compat_matrix_netsurf_renders.py.)
    """
    text = _text()
    lines = text.splitlines()
    for toolkit in ("Qt6", "SDL2"):
        rows = [
            ln for ln in lines
            if toolkit in ln
            and "STAGED" in ln
            and ("PENDING" in ln or "진행중" in ln)
            and "RENDERS" not in ln
            and "USABLE" not in ln
        ]
        assert rows, (
            f"matrix must carry a '{toolkit}: overlay STAGED, GUI launch pending' "
            "row that does not overclaim a device render"
        )


def test_toolkit_rows_do_not_overclaim_render_or_usable():
    """The still-pending toolkits (qt6/sdl2) may not be marked RENDERS/USABLE.

    They have no device launch yet; claiming a render state would be dishonest.
    (netsurf is excluded — it was device-proven RENDERS in drain#12.)
    """
    text = _text()
    lines = text.splitlines()
    for toolkit in ("Qt6", "SDL2"):
        for ln in lines:
            if toolkit not in ln or "|" not in ln:
                continue
            if "STAGED" not in ln:
                continue
            for forbidden in ("RENDERS", "USABLE"):
                assert forbidden not in ln, (
                    f"{toolkit} row overclaims '{forbidden}' but launchable binary "
                    "is still PENDING per drain#10"
                )


def test_matrix_records_traps_reduction_135_to_99():
    """gtk3-widget-factory traps dropped 135 -> 99 (WS-1 CP-6 M2 in drain#10)."""
    text = _text()
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if "gtk3-widget-factory" in ln and "135" in ln and "99" in ln
    ]
    assert rows, (
        "matrix must record the gtk3-widget-factory traps reduction 135 -> 99 "
        "(WS-1 CP-6 M2, drain#10)"
    )


def test_matrix_records_gpu_throughput_pass():
    """drain#10 added the GPU THROUGHPUT PASS marker alongside LIVE/SCREEN-CUBE."""
    text = _text()
    assert "ALR GPU THROUGHPUT: PASS" in text, (
        "matrix must record the drain#10 GPU THROUGHPUT PASS device marker"
    )


def test_matrix_records_gles2_state_setter_coverage():
    """WS-2's 19 GLES2 state setters were promoted to real wire encodings."""
    text = _text()
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if ("19" in ln)
        and ("GLES2" in ln or "GLES" in ln)
        and ("wire" in ln.lower())
    ]
    assert rows, (
        "matrix must record the WS-2 GLES2 19-op state-setter wire coverage "
        "(wire-verified, off-device wire-check) from drain#10"
    )
