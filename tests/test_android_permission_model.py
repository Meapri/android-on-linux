from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "app/src/main/AndroidManifest.xml"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_manifest_declares_normal_network_permissions_for_apt_downloads():
    text = MANIFEST.read_text()
    assert 'android.permission.INTERNET' in text
    assert 'android.permission.ACCESS_NETWORK_STATE' in text


def test_manifest_does_not_request_broad_storage_or_unneeded_dangerous_permissions():
    text = MANIFEST.read_text()
    forbidden = [
        'android.permission.MANAGE_EXTERNAL_STORAGE',
        'android.permission.READ_EXTERNAL_STORAGE',
        'android.permission.WRITE_EXTERNAL_STORAGE',
        'android.permission.SYSTEM_ALERT_WINDOW',
        'android.permission.REQUEST_INSTALL_PACKAGES',
    ]
    for permission in forbidden:
        assert permission not in text


def test_main_activity_reports_permission_model_in_summary():
    text = MAIN.read_text()
    assert 'ANDROID PERMISSION MODEL:' in text
    assert 'permission INTERNET declared=' in text
    assert 'permission ACCESS_NETWORK_STATE declared=' in text
    assert 'permission broad storage declared=' in text
    assert 'permission runtime dangerous requested=false' in text
    assert 'PackageManager.GET_PERMISSIONS' in text
