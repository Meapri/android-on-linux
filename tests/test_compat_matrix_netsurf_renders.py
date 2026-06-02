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
ROUND4_STEM = "2026-06-02-round4-milestones-drain"
ROUND6_STEM = "2026-06-02-round6-qt6-execreentry-vkrender"


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


def test_qt6_row_is_renders_round6():
    """qt6 graduated to RENDERS in round-6 v138 (EGL→wl_shm fix).

    The round-4/5 Qt-init SIGSEGV (EGL hwintegration) was fixed by excluding the EGL
    QPA/HwIntegration plugins + QT_WAYLAND_DISABLE_HW_INTEGRATION=1, so Qt6 now
    renders via the wl_shm backing store. The matrix's Qt6 row must be promoted to
    RENDERS with the rendered=true device proof and cite the round-6 evidence.
    """
    text = _text()
    lines = text.splitlines()
    rows = [ln for ln in lines if "Qt6" in ln and "|" in ln and "RENDERS" in ln]
    assert rows, "matrix must carry a Qt6 RENDERS row (round-6 v138 promotion)"
    joined = " ".join(rows)
    assert "rendered=true" in joined, (
        "Qt6 RENDERS row must carry the rendered=true device proof"
    )
    assert "2215" in joined and "2216" in joined, (
        "Qt6 RENDERS row must record the compositor frame advance 2215->2216"
    )
    assert ROUND6_STEM in joined, (
        "Qt6 RENDERS row must cite the round-6 evidence (EGL→wl_shm fix)"
    )


def test_sdl2_row_is_renders_drain13():
    """SDL2 testdraw2 was device-proved RENDERS in round-4 drain#13 (v135)."""
    text = _text()
    lines = text.splitlines()
    rows = [ln for ln in lines if "SDL2" in ln and "|" in ln and "RENDERS" in ln]
    assert rows, (
        "matrix must promote the SDL2 row to RENDERS (round-4 drain#13 testdraw2)"
    )
    joined = " ".join(rows)
    assert "rendered=true" in joined, (
        "SDL2 RENDERS row must carry the rendered=true device proof"
    )
    assert "testdraw2" in joined, (
        "SDL2 RENDERS row must name the testdraw2 demo binary"
    )
    assert ROUND4_STEM in joined, (
        "SDL2 RENDERS row must cite the round-4 drain#13 evidence"
    )


def test_babl_gegl_filter_use_still_pending():
    """babl/gegl modules STAGED + load-confirmed; actual GIMP filter output PENDING.

    round-4 drain#13 confirmed the modules *load* (`gimp-filter: ... ok=true`),
    but full filter output verification is still pending (it depends on
    exec-re-entry for GIMP plugin fork+exec). The row must stay honest.
    """
    text = _text()
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if (("babl" in ln.lower()) or ("gegl" in ln.lower()))
        and "STAGED" in ln
        and "PENDING" in ln
    ]
    assert rows, "matrix must keep a babl/gegl STAGED-but-filter-PENDING row"
    joined = " ".join(rows)
    assert "round-5" in joined or "exec-re-entry" in joined, (
        "babl/gegl filter device exercise must be marked in-progress (round-5) "
        "and/or attributed to the exec-re-entry loader gap"
    )


def test_apt_install_marked_exec_re_entry_wall():
    """Real apt install stays PENDING, attributed to maintainer-script exec-re-entry.

    round-4 drain#13 device-confirmed the wall (`apt-install: ... exec=GUEST EXEC
    FAIL`); the matrix must now record it as a WALL (not a speculative 'in
    progress'), keep the honest PENDING caveat, and attribute it to the
    exec-re-entry loader feature.
    """
    text = _text()
    lines = text.splitlines()
    rows = [
        ln for ln in lines
        if "apt install" in ln and ("PENDING" in ln or "부분" in ln)
    ]
    assert rows, "matrix must carry an 'apt install' PENDING row"
    joined = " ".join(rows)
    assert "WALL" in joined, (
        "apt install row must record the round-4 drain#13 device-confirmed WALL"
    )
    assert "maintainer-script" in joined and "exec-re-entry" in joined, (
        "apt install row must attribute the gap to maintainer-script exec-re-entry"
    )
    assert ROUND4_STEM in joined, (
        "apt install row must cite the round-4 drain#13 evidence (device-confirmed wall)"
    )
