"""WS-5 guard: the compat matrix must record netsurf-gtk as device-proven RENDERS.

drain#12 (`docs/evidence/2026-06-02-netsurf-browser-renders.md`, v134) device-proved
that the real GTK3 web browser netsurf-gtk renders on the compositor:
  `netsurf-result: rendered=true frames=2214->2217`, 5 guest threads, 25s full
  survival (sig14=SIGALRM, not a crash). It is the first browser in the
  device-proven universal-GUI set (after GIMP/gtk3-widget-factory/gtk3-demo/foot).

Round-4 promotes netsurf in `alr-compat-matrix.md` from "overlay STAGED, launchable
binary PENDING" (drain#10 status) to RENDERS. These lenient checks pin that
promotion + keep the remaining browser polish (page assets/network/input) honestly
incremental, and pin the round-4 reword of the qt6/sdl2/babl-gegl/apt-install rows.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"
NETSURF_STEM = "2026-06-02-netsurf-browser-renders"


def _text() -> str:
    assert MATRIX.is_file(), f"compat matrix missing: {MATRIX}"
    return MATRIX.read_text(encoding="utf-8")


def test_matrix_cites_netsurf_drain12_evidence():
    text = _text()
    assert NETSURF_STEM in text, (
        "compat matrix must cite the drain#12 netsurf render evidence "
        f"({NETSURF_STEM}.md)"
    )
    assert (ROOT / "docs" / "evidence" / f"{NETSURF_STEM}.md").is_file(), (
        "netsurf drain#12 evidence doc cited by the matrix does not exist on disk"
    )


def test_netsurf_row_is_renders_not_pending():
    """The netsurf-gtk row must be RENDERS and carry the rendered=true frame proof."""
    text = _text()
    lines = text.splitlines()
    rows = [ln for ln in lines if "netsurf-gtk" in ln and "|" in ln]
    assert rows, "matrix must carry a netsurf-gtk row"
    # The browser row(s) that record the device result must say RENDERS, not the old
    # "launchable binary PENDING" status.
    renders = [
        ln for ln in rows
        if "RENDERS" in ln and "rendered=true" in ln
    ]
    assert renders, (
        "matrix's netsurf-gtk row must be promoted to RENDERS with the "
        "rendered=true device proof (drain#12)"
    )
    for ln in renders:
        assert "launchable binary PENDING" not in ln, (
            "netsurf RENDERS row must not still carry the old "
            "'launchable binary PENDING' status"
        )


def test_netsurf_row_records_multithread_and_frames():
    """Honest detail: 5 guest threads + the frame-counter advance must be recorded."""
    text = _text()
    lines = text.splitlines()
    rows = [ln for ln in lines if "netsurf-gtk" in ln and "RENDERS" in ln]
    assert rows, "expected a netsurf-gtk RENDERS row"
    joined = " ".join(rows)
    assert "5" in joined and ("thread" in joined.lower()), (
        "netsurf RENDERS row must record the 5 guest threads (multithreaded in-process)"
    )
    assert "2214" in joined and "2217" in joined, (
        "netsurf RENDERS row must record the compositor frame advance 2214->2217"
    )


def test_qt6_sdl2_are_round4_in_progress_not_renders():
    """qt6/sdl2 stay STAGED with a round-4 launch caveat — never RENDERS/USABLE."""
    text = _text()
    lines = text.splitlines()
    for toolkit in ("Qt6", "SDL2"):
        rows = [ln for ln in lines if toolkit in ln and "|" in ln and "STAGED" in ln]
        assert rows, f"matrix must carry a {toolkit} STAGED row"
        for ln in rows:
            assert "RENDERS" not in ln and "USABLE" not in ln, (
                f"{toolkit} STAGED row must not overclaim a device render"
            )
        # Must explicitly mark the launch as in-progress (round-4), not just silent.
        assert any("round-4" in ln or "진행중" in ln for ln in rows), (
            f"{toolkit} row must mark its GUI demo launch as round-4 in-progress"
        )


def test_babl_gegl_filter_use_marked_round4():
    """babl/gegl modules STAGED; actual GIMP filter device use marked round-4."""
    text = _text()
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if (("babl" in ln.lower()) or ("gegl" in ln.lower()))
        and "STAGED" in ln
        and "PENDING" in ln
    ]
    assert rows, "matrix must keep a babl/gegl STAGED-but-filter-PENDING row"
    assert any("round-4" in ln for ln in rows), (
        "babl/gegl filter device exercise must be marked round-4 in-progress"
    )


def test_apt_install_marked_round4_maintainer_script():
    """Real apt install stays PENDING, attributed to maintainer-script exec-re-entry."""
    text = _text()
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if "apt install" in ln and ("PENDING" in ln or "부분" in ln)
    ]
    assert rows, "matrix must carry an 'apt install' PENDING row"
    joined = " ".join(rows)
    assert "round-4" in joined, (
        "apt install row must mark progress as round-4 in-progress"
    )
    assert "maintainer-script" in joined and "exec-re-entry" in joined, (
        "apt install row must attribute the gap to maintainer-script exec-re-entry"
    )
