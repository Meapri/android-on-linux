from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "app/build.gradle.kts"
PREBUILT_DIR = ROOT / "app/src/main/prebuiltNative/arm64-v8a"


def test_arm64_real_termux_proot_prebuilt_is_checked_in():
    assert (PREBUILT_DIR / "libalr_proot.so").is_file()
    assert (PREBUILT_DIR / "libtalloc.so").is_file()
    assert (PREBUILT_DIR / "TERMUX-PROOT-NOTICE.txt").is_file()


def test_gradle_prefers_prebuilt_proot_when_available():
    text = BUILD.read_text()
    assert "prebuiltNative" in text
    assert "prebuiltProot" in text
    assert "libtalloc.so" in text
