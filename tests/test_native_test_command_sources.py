from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
TEST_COMMAND = ROOT / "app/src/main/cpp/test_command.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_test_command_target_exists():
    cmake = CMAKE.read_text()
    command = TEST_COMMAND.read_text()
    assert "add_executable(alr_test_command" in cmake
    assert "OUTPUT_NAME \"alr-test-command\"" in cmake
    assert "alr-test-command ok" in command


def test_main_activity_mentions_native_test_command_backend():
    text = MAIN.read_text()
    assert "android-native-test-command" in text
    assert "libalr_test_command.so" in text
