from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "app" / "build.gradle.kts"


def test_termux_proot_prebuilts_are_excluded_from_gradle_strip():
    text = BUILD.read_text()
    assert "keepDebugSymbols" in text
    assert "**/libalr_proot.so" in text
    assert "**/libtalloc.so" in text
    assert "**/libproot-loader.so" in text


def test_build_stamp_identifies_unstripped_proot_package():
    text = BUILD.read_text()
    assert 'versionName = "0.4.134-netsurf-launch-v134"' in text
