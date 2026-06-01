from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
KOTLIN = ROOT / "app" / "src" / "main" / "java" / "dev" / "chanwoo" / "androlinux" / "RootfsInstallPlan.kt"
MAIN = ROOT / "app" / "src" / "main" / "java" / "dev" / "chanwoo" / "androlinux" / "MainActivity.kt"


def test_android_rootfs_install_plan_model_exists_and_uses_app_files_rootfs():
    text = KOTLIN.read_text()

    assert "data class RootfsManifest" in text
    assert "data class RootfsAsset" in text
    assert "data class RootfsInstallPlan" in text
    assert "files/rootfs" not in text  # app supplies filesDir dynamically; do not hardcode Android path
    assert '"rootfs"' in text
    assert '".downloads"' in text
    assert ".alr-installed-" in text


def test_main_activity_uses_rootfs_install_plan_before_native_report():
    text = MAIN.read_text()

    assert "RootfsManifest(" in text
    assert "buildRootfsInstallPlan(" in text
    assert "rootfsPlan.rootfsDir" in text
    assert "rootfsPlan.markerPath" in text
