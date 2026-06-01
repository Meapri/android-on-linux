from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "bootstrap-android-build-host.sh"


def test_bootstrap_script_has_x86_64_and_aarch64_paths():
    text = SCRIPT.read_text()

    assert "uname -m" in text
    assert "x86_64" in text
    assert "aarch64" in text
    assert "qemu-user-static" in text
    assert "libc6-amd64-cross" in text
    assert "ld-linux-x86-64.so.2" in text


def test_bootstrap_script_installs_required_android_packages():
    text = SCRIPT.read_text()

    assert "platforms;android-35" in text
    assert "build-tools;35.0.0" in text
    assert "platform-tools" in text
    assert "ndk;" in text
    assert "cmake;" in text


def test_bootstrap_script_writes_local_properties():
    text = SCRIPT.read_text()

    assert "local.properties" in text
    assert "sdk.dir=" in text
    assert "cmake.dir=/usr" in text
