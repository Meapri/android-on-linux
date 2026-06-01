"""WS-5 guard: the compat matrix must record the drain#9 device-confirmed
package-manager / X11 rows.

drain#9 (v130) device-PROVED that dpkg-query, apt-get, AND Xwayland execute as
glibc guests through the ALR native loader (the `pkgfunc-*` markers, all
`ok=true exec=GUEST EXEC PASS`). Before drain#9 the matrix's "미지원" section
flatly claimed in-app apt/dpkg was WALL and X11 was wholly PENDING. This test
pins that the matrix now reflects the device-confirmed *runs+version* result for
each of the three, while staying honest that real `apt install` is still PENDING.

See docs/evidence/2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc.md.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"

# The three binaries drain#9 proved run on-device through the ALR loader.
PKG_BINARIES = ("dpkg-query", "apt-get", "Xwayland")


def _text() -> str:
    assert MATRIX.is_file(), f"compat matrix missing: {MATRIX}"
    return MATRIX.read_text(encoding="utf-8")


def test_matrix_records_pkgmgr_binaries_run_on_device():
    """Each of dpkg-query / apt-get / Xwayland must appear on a RUNS row."""
    lines = _text().splitlines()
    for bin in PKG_BINARIES:
        run_lines = [ln for ln in lines if bin in ln and "RUNS" in ln]
        assert run_lines, (
            f"compat matrix must carry a RUNS row for {bin!r} "
            "(drain#9 device-proved it executes via the ALR loader)"
        )


def test_matrix_cites_pkgfunc_evidence():
    text = _text()
    assert "2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc" in text, (
        "compat matrix must cite the drain#9 pkgfunc/getpwuid/renderer evidence doc"
    )


def test_matrix_stays_honest_apt_install_pending():
    """runs+version is proven, but full `apt install` (network fetch + unpack) is not.

    A matrix that upgraded apt/dpkg all the way to 'fully works' would be lying;
    pin that a concrete apt-install PENDING caveat survives.
    """
    text = _text()
    lines = text.splitlines()
    pending_install = [
        ln for ln in lines
        if ("apt install" in ln) and ("PENDING" in ln)
    ]
    assert pending_install, (
        "compat matrix must keep an honest 'apt install ... PENDING' caveat "
        "(only runs+version is device-proven, not real package installation)"
    )


def test_matrix_records_gl_renderer_passthrough():
    """drain#9 also proved the guest sees the real host Mali GL_RENDERER."""
    text = _text()
    assert "Mali-G615 MC2" in text, (
        "compat matrix must record the device-verified GL_RENDERER passthrough "
        "(guest reads host's real Mali-G615 MC2 string, not a synthetic one)"
    )
