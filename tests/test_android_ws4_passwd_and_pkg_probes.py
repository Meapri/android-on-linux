"""WS-4 §10(a)(b): runtime passwd/nss entry + apt/dpkg/X11 functional logcat probes.

(a) getpwuid fix: the guest runs under the Android app uid (e.g. 10326), but the base
    /etc/passwd only ships root(0)/nobody(65534), so getpwuid_r(geteuid()) fails. The
    installer must append a real passwd/group entry for the *runtime* uid so the lookup
    succeeds through nsswitch files -> libnss_files.

(b) functional probes: the dpkg-db/x11/apt-config overlays only proved "staged" on
    device (exec-summary TextView). MainActivity must actually RUN dpkg-query/apt-get/
    Xwayland through the ALR native loader and Log.i(tag=alr_loader, ...) the result so a
    device drain can capture functional evidence.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/RootfsInstaller.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_installer_writes_runtime_passwd_entry_for_app_uid():
    text = INSTALLER.read_text()
    # A dedicated, public, idempotent installer step keyed on the *runtime* uid.
    assert "fun writeRuntimeUserEntry(" in text
    # uid/gid come from the runtime app uid (public Android API), not a baked constant.
    assert "android.os.Process.myUid()" in text
    # It writes both passwd and group so getpwuid_r AND getgrgid_r succeed.
    assert 'File(etc, "passwd")' in text
    assert 'File(etc, "group")' in text
    # A real entry: name, uid, gid, HOME=/root, a shell that exists in the base.
    assert "androlinux" in text
    assert "/bin/dash" in text


def test_installer_runs_passwd_step_during_prepare():
    text = INSTALLER.read_text()
    # Called from the install path AFTER extraction, before the install marker, so the
    # entry survives every cold-start re-extraction.
    assert "writeRuntimeUserEntry(plan.rootfsDir" in text
    extract_idx = text.index("extractVerifiedTar(stagedArchive, plan.rootfsDir)")
    passwd_idx = text.index("writeRuntimeUserEntry(plan.rootfsDir")
    marker_idx = text.index("writeInstallMarker(plan.markerPath)")
    assert extract_idx < passwd_idx < marker_idx


def test_installer_passwd_upsert_is_idempotent():
    text = INSTALLER.read_text()
    # Idempotent merge: drop a prior record with the same key, keep the rest, append once.
    assert "upsertColonRecord(" in text
    assert "filterNot" in text


def test_main_activity_wires_package_manager_functional_probes():
    text = MAIN.read_text()
    # Wiring lives in a private fun (hotspot rule), called from onCreate.
    assert "private fun launchPackageManagerProbes(" in text
    assert "launchPackageManagerProbes(rootfsStatus.rootfsDir, rootfsManifest.name)" in text


def test_functional_probes_run_dpkg_apt_x11_through_loader():
    text = MAIN.read_text()
    # Each probe runs the real binary through the SAME native-loader probe the
    # foot/gtkdemo/glmark2 launches use (newline-delimited argv).
    assert "nativeAlrNativeLoaderProbe(" in text
    assert "/usr/bin/dpkg-query\\n--version" in text
    assert "/usr/bin/apt-get\\n--version" in text
    assert "/usr/bin/Xwayland\\n-version" in text


def test_functional_probes_emit_logcat_markers():
    text = MAIN.read_text()
    # Results go to logcat tag alr_loader with a stable "pkgfunc-<label>" marker a drain
    # greps. The marker is built as "pkgfunc-$label"; the labels are passed per probe.
    assert 'android.util.Log.i(' in text
    assert '"pkgfunc-$label' in text
    assert '"dpkg-query-version"' in text
    assert '"apt-get-version"' in text
    assert '"xwayland-version"' in text
