from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"
DOC = ROOT / "docs/architecture/execution-backend.md"


def test_proot_command_supports_link2symlink():
    text = RUNNER.read_text()
    assert "linkToSymlink" in text
    # The flag maps to PRoot's `-l` (--link2symlink) extension.
    assert 'listOf("-l")' in text


def test_package_manager_smokes_enable_link2symlink():
    text = RUNNER.read_text()
    install = text.split("runProotRootfsDpkgInstallLocalSmoke", 1)[1].split("fun ", 1)[0]
    assert "linkToSymlink = true" in install
    installed = text.split("runProotRootfsInstalledPackageSmoke", 1)[1].split("fun ", 1)[0]
    assert "linkToSymlink = true" in installed


def test_docs_record_hardlink_root_cause_and_fix():
    text = DOC.read_text()
    # Root cause: Android app-data filesystems reject hardlink(); fix is
    # PRoot --link2symlink. Confirmed on device.
    assert "link2symlink" in text
    assert "hardlink" in text or "hard link" in text
