from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_command_runner_can_run_proot_candidate_smoke():
    text = RUNNER.read_text()
    assert "runProotCandidateSmokeTest" in text
    assert "libalr_proot.so" in text
    assert '"--version"' in text


def test_main_activity_reports_proot_candidate_smoke_result():
    text = MAIN.read_text()
    assert "proot candidate exit=" in text
    assert "proot candidate stdout=" in text
    assert "proot candidate stderr=" in text
