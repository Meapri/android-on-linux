from tools.runtime_launch_plan import build_launch_plan


def test_proot_launch_plan_uses_packaged_proot_from_native_library_dir():
    plan = build_launch_plan(
        package_name="dev.chanwoo.androlinux",
        native_library_dir="/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64",
        app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
        rootfs_name="debian-arm64",
        program="/bin/hello",
        backend="proot",
    )

    assert plan.executable == "/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64/libalr_proot.so"
    assert plan.argv == [
        "/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64/libalr_proot.so",
        "-R",
        "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
        "-w",
        "/",
        "/bin/hello",
    ]
    assert plan.env["PROOT_NO_SECCOMP"] == "1"
    assert plan.env["ALR_BACKEND"] == "proot"


def test_proot_launch_plan_rejects_relative_programs():
    try:
        build_launch_plan(
            package_name="dev.chanwoo.androlinux",
            native_library_dir="/data/app/pkg/lib/arm64",
            app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
            rootfs_name="debian-arm64",
            program="bin/hello",
            backend="proot",
        )
    except ValueError as exc:
        assert "program must be an absolute path inside the rootfs" in str(exc)
    else:
        raise AssertionError("relative program should be rejected")
