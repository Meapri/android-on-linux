from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_PLAN = ROOT / "app/src/main/cpp/runtime_plan.cpp"
CPP_REPORT = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_cpp_runtime_plan_can_build_proot_launch_plan():
    text = CPP_PLAN.read_text()
    assert "build_proot_launch_plan" in text
    assert "libalr_proot.so" in text
    assert '"-R"' in text
    assert '"-w"' in text
    assert "PROOT_LOADER" in text
    assert "PROOT_TMP_DIR" in text
    assert "PROOT_NO_SECCOMP" in text
    assert "PROOT_VERBOSE" in text
    assert "LD_LIBRARY_PATH" in text


def test_jni_report_includes_proot_launch_plan_section():
    text = CPP_REPORT.read_text()
    assert "proot argv:" in text
    assert "PROOT_LOADER" in text
    assert "PROOT_TMP_DIR" in text
    assert "PROOT_NO_SECCOMP" in text
    assert "PROOT_VERBOSE" in text
    assert "LD_LIBRARY_PATH" in text


def test_main_activity_reports_proot_backend_candidate():
    text = MAIN.read_text()
    assert "proot backend candidate" in text
    assert "libalr_proot.so" in text
