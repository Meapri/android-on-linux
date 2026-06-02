from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "app/build.gradle.kts"
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"
CPP_PLAN = ROOT / "app/src/main/cpp/runtime_plan.cpp"
CPP_REPORT = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
PREBUILT_DIR = ROOT / "app/src/main/prebuiltNative/arm64-v8a"


def test_arm64_proot_loader_prebuilt_is_packaged():
    assert (PREBUILT_DIR / "libproot-loader.so").is_file()
    text = BUILD.read_text()
    assert "libproot-loader.so" in text


def test_proot_runner_sets_loader_and_tmp_environment():
    text = RUNNER.read_text()
    assert "PROOT_LOADER" in text
    assert "PROOT_TMP_DIR" in text
    assert "PROOT_VERBOSE" in text
    assert "libproot-loader.so" in text


def test_proot_launch_plan_and_report_show_actual_loader_environment():
    for path in (CPP_PLAN, CPP_REPORT, MAIN):
        text = path.read_text()
        assert "PROOT_LOADER" in text
        assert "PROOT_TMP_DIR" in text
        assert "PROOT_VERBOSE" in text


def test_visible_build_stamp_bumped_for_termux_pair_probe():
    text = MAIN.read_text()
    assert "build: 0.4.161-sd-v161" in text
