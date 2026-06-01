from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "app" / "src" / "main" / "java" / "dev" / "chanwoo" / "androlinux" / "RootfsInstaller.kt"
MAIN = ROOT / "app" / "src" / "main" / "java" / "dev" / "chanwoo" / "androlinux" / "MainActivity.kt"


def test_rootfs_installer_reads_bundled_manifest_and_payload_names():
    text = INSTALLER.read_text()

    assert "rootfs/manifests/debian-arm64-bookworm-slim.json" in text
    assert "rootfs/payloads/" in text
    assert "MessageDigest.getInstance(\"SHA-256\")" in text
    assert "verifyAsset" in text


def test_main_activity_uses_rootfs_installer_status():
    text = MAIN.read_text()

    assert "RootfsInstaller" in text
    assert "prepareBundledTinyRootfs" in text
    assert "rootfs status:" in text
