from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/RootfsInstaller.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
BUILD = ROOT / "app/build.gradle.kts"


def test_rootfs_installer_has_safe_tar_extraction_guards():
    text = INSTALLER.read_text()
    assert "extractVerifiedTar" in text
    assert "canonicalPath" in text
    assert "startsWith(rootfsDir.canonicalPath" in text
    assert "TarArchiveInputStream" in text
    assert "writeInstallMarker" in text
    assert "extracted" in text


def test_main_activity_reports_extraction_status():
    text = MAIN.read_text()
    assert "extracted=${rootfsStatus.extracted}" in text
    assert "marker=${rootfsStatus.markerPath.absolutePath}" in text


def test_android_build_includes_commons_compress_for_tar_extraction():
    text = BUILD.read_text()
    assert "org.apache.commons:commons-compress" in text
