from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/RootfsInstaller.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"


def test_rootfs_installer_cleans_previous_extraction_before_unpacking():
    text = INSTALLER.read_text()
    assert "cleanRootfsDir(plan.rootfsDir)" in text
    assert "rootfsDir.deleteRecursively()" in text
    assert "removeStaleHostDpkgConfig(plan.rootfsDir)" in text
    assert "etc/dpkg/dpkg.cfg.d/needrestart" in text


def test_dpkg_split_direct_smoke_is_reported():
    assert "runProotRootfsDpkgSplitVersion" in RUNNER.read_text()
    text = MAIN.read_text()
    assert "DPKG SPLIT EXECUTION" in text
    assert "proot dpkg-split --version" in text
    assert "rootfs stale needrestart dpkg cfg exists=" in text
    assert "rootfs androlinux minimal dpkg cfg exists=" in text
