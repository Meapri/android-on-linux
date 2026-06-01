from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_command_runner_executes_packaged_command_with_process_builder():
    text = RUNNER.read_text()
    assert "ProcessBuilder" in text
    assert "libalr_test_command.so" in text
    assert "exitCode" in text
    assert "stdout" in text
    assert "stderr" in text


def test_main_activity_reports_native_command_result():
    text = MAIN.read_text()
    assert "NativeCommandRunner" in text
    assert "native command exit=" in text
    assert "native command stdout=" in text
