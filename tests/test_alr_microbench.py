"""Host tests for the M1 microbench source, README, and compat-matrix v127 bump.

These are static/text assertions only — no compilation or device run — so they
run on the CI host without an arm64 toolchain or adb.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MICROBENCH_C = ROOT / "bench" / "microbench" / "microbench.c"
MICROBENCH_README = ROOT / "bench" / "microbench" / "README.md"
COMPAT_MATRIX = ROOT / "docs" / "research" / "alr-compat-matrix.md"


def test_microbench_c_exists_and_has_both_modes():
    assert MICROBENCH_C.exists(), f"missing {MICROBENCH_C}"
    text = MICROBENCH_C.read_text(encoding="utf-8")
    assert '"compute"' in text
    assert '"syscall"' in text


def test_microbench_c_uses_clock_gettime_and_getpid_syscall():
    text = MICROBENCH_C.read_text(encoding="utf-8")
    assert "clock_gettime" in text
    assert "SYS_getpid" in text


def test_microbench_c_prints_expected_result_line():
    text = MICROBENCH_C.read_text(encoding="utf-8")
    assert "MICROBENCH mode=" in text


def test_readme_exists_and_documents_arm64_and_harness():
    assert MICROBENCH_README.exists(), f"missing {MICROBENCH_README}"
    text = MICROBENCH_README.read_text(encoding="utf-8")
    assert "aarch64" in text
    assert "bench overhead" in text


def test_compat_matrix_bumped_to_v127():
    assert COMPAT_MATRIX.exists(), f"missing {COMPAT_MATRIX}"
    text = COMPAT_MATRIX.read_text(encoding="utf-8")
    assert "v127" in text


def test_compat_matrix_gtk3_widget_factory_renders():
    text = COMPAT_MATRIX.read_text(encoding="utf-8")
    gtk_lines = [
        line for line in text.splitlines() if "gtk3-widget-factory" in line
    ]
    assert gtk_lines, "no gtk3-widget-factory line found"
    assert any("RENDERS" in line for line in gtk_lines), (
        "gtk3-widget-factory line is not marked RENDERS"
    )
