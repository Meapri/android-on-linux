from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "app/build.gradle.kts"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_gradle_packages_native_test_command_as_jni_lib_named_so():
    text = BUILD.read_text()
    assert "packageNativeTestCommand" in text
    assert "libalr_test_command.so" in text
    assert "libalr_runtime_trampoline.so" in text
    assert "outputs.upToDateWhen { false }" in text
    assert "mergeDebugJniLibFolders" in text


def test_main_activity_points_at_packaged_native_test_command_so():
    text = MAIN.read_text()
    assert "libalr_test_command.so" in text
