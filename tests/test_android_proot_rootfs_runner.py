from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_command_runner_can_attempt_proot_rootfs_program():
    text = RUNNER.read_text()
    assert "runProotRootfsProgram" in text
    assert '"-R"' in text
    assert '"-w"' in text
    assert '"/"' in text
    assert "runProotRootfsProgramVerbose" in text


def test_main_activity_reports_proot_hello_attempt_result():
    text = MAIN.read_text()
    assert "ROOTFS EXECUTION:" in text
    assert "proot hello quiet exit=" in text
    assert "proot hello quiet stdout=" in text
    assert "proot hello verbose on failure" in text


def test_native_command_runner_can_attempt_proot_shell_command():
    text = RUNNER.read_text()
    assert "runProotRootfsShell" in text
    assert '"/bin/sh"' in text
    assert '"-c"' in text


def test_main_activity_reports_shell_and_script_smoke_results():
    text = MAIN.read_text()
    assert "SHELL SCRIPT EXECUTION:" in text
    assert "proot script exit=" in text
    assert "proot script stdout=" in text
    assert "proot shell -c exit=" in text
    assert "proot shell -c stdout=" in text


def test_main_activity_reports_glibc_dynamic_smoke_result():
    text = MAIN.read_text()
    assert "GLIBC DYNAMIC EXECUTION:" in text
    assert "rootfs /bin/glibc-hello exists=" in text
    assert "rootfs glibc loader exists=" in text
    assert "rootfs libc exists=" in text
    assert "proot glibc exit=" in text
    assert "proot glibc stdout=" in text
    assert "proot glibc stderr=" in text


def test_native_command_runner_can_attempt_proot_dash_command():
    text = RUNNER.read_text()
    assert "runProotRootfsDash" in text
    assert '"/bin/dash"' in text
    assert '"-c"' in text


def test_main_activity_reports_real_distro_userland_smoke_result():
    text = MAIN.read_text()
    assert "DISTRO USERLAND EXECUTION:" in text
    assert "rootfs /bin/dash exists=" in text
    assert "rootfs /usr/bin/env exists=" in text
    assert "proot dash exit=" in text
    assert "proot dash stdout=" in text
    assert "proot dash stderr=" in text


def test_native_command_runner_clears_inherited_android_environment_before_launch():
    text = RUNNER.read_text()
    assert "processBuilder.environment().clear()" in text
    assert "processBuilder.environment().putAll(environment)" in text


def test_main_activity_reports_clean_guest_environment_smoke():
    text = MAIN.read_text()
    assert "CLEAN GUEST ENVIRONMENT:" in text
    assert "guest env leaked android vars=" in text
    assert "ANDROID_ROOT=" in text
    assert "BOOTCLASSPATH=" in text
    assert "DEX2OATBOOTCLASSPATH=" in text


def test_native_command_runner_can_attempt_proot_root_identity_command():
    text = RUNNER.read_text()
    assert "runProotRootfsProgramAsRoot" in text
    assert '"-0"' in text
    assert '"/usr/bin/id"' in text


def test_main_activity_reports_identity_nss_smoke_result():
    text = MAIN.read_text()
    assert "IDENTITY NSS EXECUTION:" in text
    assert "rootfs /etc/passwd exists=" in text
    assert "rootfs /etc/group exists=" in text
    assert "rootfs /etc/nsswitch.conf exists=" in text
    assert "rootfs /usr/bin/id exists=" in text
    assert "rootfs libselinux exists=" in text
    assert "rootfs libpcre2 exists=" in text
    assert "rootfs libnss_files exists=" in text
    assert "identity numeric root=" in text
    assert "identity named root=" in text
    assert "proot id exit=" in text
    assert "proot id stdout=" in text
    assert "proot id stderr=" in text


def test_identity_nss_smoke_uses_raw_rootfs_to_avoid_host_etc_binds():
    text = RUNNER.read_text()
    assert "runProotRootfsIdAsRoot" in text
    assert "rawRootfs = true" in text
    assert 'if (rawRootfs) "-r" else "-R"' in text


def test_native_command_runner_can_attempt_dpkg_version_smoke():
    text = RUNNER.read_text()
    assert "runProotRootfsDpkgVersion" in text
    assert '"/usr/bin/dpkg"' in text
    assert '"--version"' in text


def test_main_activity_reports_dpkg_version_smoke_result():
    text = MAIN.read_text()
    assert "DPKG VERSION EXECUTION:" in text
    assert "rootfs /usr/bin/dpkg exists=" in text
    assert "rootfs libmd exists=" in text
    assert "proot dpkg --version exit=" in text
    assert "proot dpkg --version stdout=" in text
    assert "proot dpkg --version stderr=" in text


def test_main_activity_reports_dpkg_arch_and_query_smoke_results():
    text = MAIN.read_text()
    assert "DPKG ARCH EXECUTION:" in text
    assert "DPKG QUERY EXECUTION:" in text
    assert "rootfs /usr/bin/dpkg-query exists=" in text
    assert "rootfs /usr/share/dpkg/cputable exists=" in text
    assert "rootfs /usr/share/dpkg/tupletable exists=" in text
    assert "proot dpkg --print-architecture exit=" in text
    assert "proot dpkg-query --version exit=" in text


def test_main_activity_reports_apt_base_bundle_smoke_results():
    text = MAIN.read_text()
    assert "APT VERSION EXECUTION:" in text
    assert "APT-GET VERSION EXECUTION:" in text
    assert "APT-CACHE VERSION EXECUTION:" in text
    assert "APT-CONFIG VERSION EXECUTION:" in text
    assert "rootfs /usr/bin/apt exists=" in text
    assert "rootfs /usr/bin/apt-get exists=" in text
    assert "rootfs libapt-pkg exists=" in text
    assert "rootfs apt http method exists=" in text
    assert "proot apt --version exit=" in text
    assert "proot apt-get --version exit=" in text
    assert "proot apt-cache --version exit=" in text
    assert "proot apt-config --version exit=" in text


def test_main_activity_reports_local_deb_install_smoke_results():
    text = MAIN.read_text()
    assert "DPKG LOCAL INSTALL EXECUTION:" in text
    assert "INSTALLED PACKAGE EXECUTION:" in text
    assert "rootfs local deb exists=" in text
    assert "rootfs /usr/bin/dpkg-deb exists=" in text
    assert "rootfs installed alr smoke exists=" in text
    assert "proot dpkg -i local deb exit=" in text
    assert "proot installed package smoke exit=" in text


def test_package_manager_install_uses_minimal_raw_rootfs_device_binds():
    text = RUNNER.read_text()
    assert "minimalPackageManagerBinds" in text
    assert "-b" in text
    assert "/dev/null:/dev/null" in text
    assert "/dev/zero:/dev/zero" in text
    assert "/dev/urandom:/dev/urandom" in text


def test_main_activity_reports_dpkg_install_state_placeholders():
    text = MAIN.read_text()
    assert "rootfs /dev/null placeholder exists=" in text
    assert "rootfs dpkg triggers File exists=" in text
    assert "rootfs dpkg triggers Unincorp exists=" in text


def test_package_manager_path_includes_sbin_helpers():
    text = RUNNER.read_text()
    assert "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" in text


def test_main_activity_reports_dpkg_helper_programs():
    text = MAIN.read_text()
    for label in [
        "rootfs helper rm exists=",
        "rootfs helper tar exists=",
        "rootfs helper diff exists=",
        "rootfs helper ldconfig exists=",
        "rootfs helper ldconfig.real exists=",
        "rootfs helper start-stop-daemon exists=",
    ]:
        assert label in text
