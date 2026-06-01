import pytest

from tools.runtime_launch_plan import LaunchPlan, build_launch_plan


def test_launch_plan_executes_only_from_package_native_lib_dir():
    plan = build_launch_plan(
        package_name="dev.chanwoo.androlinux",
        native_library_dir="/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64",
        app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
        rootfs_name="debian-arm64",
        program="/bin/bash",
    )

    assert isinstance(plan, LaunchPlan)
    assert plan.executable == "/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64/libalr-loader.so"
    assert plan.rootfs_dir == "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64"
    assert plan.argv == [
        "/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64/libalr-loader.so",
        "--rootfs",
        "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
        "--program",
        "/bin/bash",
    ]


def test_launch_plan_rejects_writable_app_data_executable():
    with pytest.raises(ValueError, match="executable must live in native_library_dir"):
        LaunchPlan(
            executable="/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64/bin/bash",
            native_library_dir="/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64",
            rootfs_dir="/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
            argv=["/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64/bin/bash"],
            env={},
        )


def test_launch_plan_env_points_tools_inside_rootfs_without_claiming_chroot():
    plan = build_launch_plan(
        package_name="dev.chanwoo.androlinux",
        native_library_dir="/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64",
        app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
        rootfs_name="debian-arm64",
        program="/usr/bin/python3",
    )

    assert plan.env["ALR_ROOTFS"] == "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64"
    assert plan.env["ALR_PROGRAM"] == "/usr/bin/python3"
    assert plan.env["PATH"] == "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    assert "CHROOT" not in plan.env


def test_alr_runtime_launch_plan_is_packaged_dry_run_skeleton():
    plan = build_launch_plan(
        package_name="dev.chanwoo.androlinux",
        native_library_dir="/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64",
        app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
        rootfs_name="debian-arm64",
        program="/bin/hello",
        backend="alr-runtime",
    )

    assert plan.executable == "/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64/libalr_runtime_launcher.so"
    assert plan.argv == [
        "/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64/libalr_runtime_launcher.so",
        "--rootfs",
        "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
        "--cwd",
        "/",
        "--program",
        "/bin/hello",
        "--dry-run",
    ]
    assert plan.env["ALR_BACKEND"] == "alr-runtime"
    assert plan.env["ALR_HOOK_PATH"].endswith("/libalr_runtime_hook.so")
    assert plan.env["ALR_INTERPOSER_PATH"].endswith("/libalr_runtime_interposer.so")
    assert plan.env["ALR_BRIDGE_PATH"].endswith("/libalr_runtime_bridge.so")
    assert plan.env["ALR_CONFIG_FORMAT"] == "alr-config-v1"
    assert plan.env["ALR_FAKE_ROOT"] == "0"
    assert plan.env["ALR_VERBOSE"] == "0"
    assert plan.env["ALR_TRACE_PATH"] == "0"
    assert plan.env["ALR_TRACE_EXEC"] == "0"
