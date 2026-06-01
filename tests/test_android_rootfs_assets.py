from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "app" / "src" / "main" / "assets" / "rootfs" / "manifests" / "debian-arm64-bookworm-slim.json"
PAYLOAD = ROOT / "app" / "src" / "main" / "assets" / "rootfs" / "payloads" / "tiny-rootfs.tar"


def test_android_assets_include_rootfs_manifest_and_payload():
    assert MANIFEST.is_file()
    assert PAYLOAD.is_file()
    assert PAYLOAD.stat().st_size == 252743680


def test_android_asset_manifest_matches_host_manifest():
    host_manifest = ROOT / "rootfs" / "manifests" / "debian-arm64-bookworm-slim.json"
    assert MANIFEST.read_text() == host_manifest.read_text()


def test_android_asset_payload_matches_host_payload():
    host_payload = ROOT / "rootfs" / "tiny-rootfs.tar"
    assert PAYLOAD.read_bytes() == host_payload.read_bytes()


def test_tiny_rootfs_hello_is_static_arm64_elf_not_shell_script():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        member = archive.extractfile("./bin/hello")
        assert member is not None
        blob = member.read(4)
    assert blob == b"\x7fELF"


def test_tiny_rootfs_hello_remains_static_even_when_shell_is_available():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        hello = archive.extractfile("./bin/hello").read(4)
        shell = archive.extractfile("./bin/sh").read(4)
    assert "./bin/hello" in names
    assert "./bin/sh" in names
    assert hello == b"\x7fELF"
    assert shell == b"\x7fELF"


def test_tiny_rootfs_contains_static_busybox_shell_userland():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./bin/busybox" in names
        assert "./bin/sh" in names
        assert "./bin/cat" in names
        assert archive.extractfile("./bin/busybox").read(4) == b"\x7fELF"
        assert archive.extractfile("./bin/sh").read(4) == b"\x7fELF"
        assert archive.extractfile("./bin/cat").read(4) == b"\x7fELF"


def test_tiny_rootfs_contains_shell_script_smoke_fixture():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./bin/script-hello" in names
        script = archive.extractfile("./bin/script-hello").read().decode()
    assert script.startswith("#!/bin/sh\n")
    assert "hello from shell script rootfs" in script


def test_tiny_rootfs_contains_glibc_dynamic_smoke_files():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./bin/glibc-hello" in names
        assert "./lib/ld-linux-aarch64.so.1" in names
        assert "./lib/aarch64-linux-gnu/libc.so.6" in names
        assert archive.extractfile("./bin/glibc-hello").read(4) == b"\x7fELF"
        assert archive.extractfile("./lib/ld-linux-aarch64.so.1").read(4) == b"\x7fELF"
        assert archive.extractfile("./lib/aarch64-linux-gnu/libc.so.6").read(4) == b"\x7fELF"


def test_tiny_rootfs_glibc_hello_requests_rootfs_loader():
    import subprocess
    import tarfile
    import tempfile

    with tarfile.open(PAYLOAD) as archive, tempfile.NamedTemporaryFile() as tmp:
        tmp.write(archive.extractfile("./bin/glibc-hello").read())
        tmp.flush()
        program_headers = subprocess.check_output(["readelf", "-l", tmp.name], text=True)
    assert "Requesting program interpreter: /lib/ld-linux-aarch64.so.1" in program_headers


def test_tiny_rootfs_contains_real_distro_dynamic_userland_binaries():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./bin/dash" in names
        assert "./usr/bin/env" in names
        assert archive.extractfile("./bin/dash").read(4) == b"\x7fELF"
        assert archive.extractfile("./usr/bin/env").read(4) == b"\x7fELF"


def test_real_distro_dynamic_userland_binaries_request_rootfs_loader():
    import subprocess
    import tarfile
    import tempfile

    with tarfile.open(PAYLOAD) as archive:
        for member in ["./bin/dash", "./usr/bin/env"]:
            with tempfile.NamedTemporaryFile() as tmp:
                tmp.write(archive.extractfile(member).read())
                tmp.flush()
                program_headers = subprocess.check_output(["readelf", "-l", tmp.name], text=True)
            assert "Requesting program interpreter: /lib/ld-linux-aarch64.so.1" in program_headers


def test_tiny_rootfs_contains_identity_nss_files_and_id_binary():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        expected = [
            "./etc/passwd",
            "./etc/group",
            "./etc/nsswitch.conf",
            "./usr/bin/id",
            "./lib/aarch64-linux-gnu/libselinux.so.1",
            "./lib/aarch64-linux-gnu/libpcre2-8.so.0",
            "./lib/aarch64-linux-gnu/libnss_files.so.2",
        ]
        for member in expected:
            assert member in names
        passwd = archive.extractfile("./etc/passwd").read().decode()
        group = archive.extractfile("./etc/group").read().decode()
        nsswitch = archive.extractfile("./etc/nsswitch.conf").read().decode()
        assert "root:x:0:0:root:/root:/bin/dash" in passwd
        assert "root:x:0:" in group
        assert "passwd: files" in nsswitch
        assert "group: files" in nsswitch
        assert archive.extractfile("./usr/bin/id").read(4) == b"\x7fELF"
        assert archive.extractfile("./lib/aarch64-linux-gnu/libselinux.so.1").read(4) == b"\x7fELF"
        assert archive.extractfile("./lib/aarch64-linux-gnu/libpcre2-8.so.0").read(4) == b"\x7fELF"
        assert archive.extractfile("./lib/aarch64-linux-gnu/libnss_files.so.2").read(4) == b"\x7fELF"


def test_real_distro_id_requests_rootfs_loader():
    import subprocess
    import tarfile
    import tempfile

    with tarfile.open(PAYLOAD) as archive, tempfile.NamedTemporaryFile() as tmp:
        tmp.write(archive.extractfile("./usr/bin/id").read())
        tmp.flush()
        program_headers = subprocess.check_output(["readelf", "-l", tmp.name], text=True)
        dynamic = subprocess.check_output(["readelf", "-d", tmp.name], text=True)
    assert "Requesting program interpreter: /lib/ld-linux-aarch64.so.1" in program_headers
    assert "libselinux.so.1" in dynamic
    assert "libc.so.6" in dynamic


def test_tiny_rootfs_contains_dpkg_version_smoke_files():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./usr/bin/dpkg" in names
        assert "./lib/aarch64-linux-gnu/libmd.so.0" in names
        assert "./etc/dpkg" in names
        assert "./etc/dpkg/dpkg.cfg" in names
        assert "./etc/dpkg/dpkg.cfg.d" in names
        assert "./usr/bin/dpkg-query" in names
        assert "./usr/share/dpkg/abitable" in names
        assert "./usr/share/dpkg/cputable" in names
        assert "./usr/share/dpkg/ostable" in names
        assert "./usr/share/dpkg/tupletable" in names
        assert "./var/lib/dpkg/info" in names
        assert "./var/lib/dpkg/updates" in names
        assert archive.extractfile("./usr/bin/dpkg").read(4) == b"\x7fELF"
        assert archive.extractfile("./usr/bin/dpkg-query").read(4) == b"\x7fELF"
        assert archive.extractfile("./lib/aarch64-linux-gnu/libmd.so.0").read(4) == b"\x7fELF"


def test_real_distro_dpkg_requests_rootfs_loader_and_libmd():
    import subprocess
    import tarfile
    import tempfile

    with tarfile.open(PAYLOAD) as archive, tempfile.NamedTemporaryFile() as tmp:
        tmp.write(archive.extractfile("./usr/bin/dpkg").read())
        tmp.flush()
        program_headers = subprocess.check_output(["readelf", "-l", tmp.name], text=True)
        dynamic = subprocess.check_output(["readelf", "-d", tmp.name], text=True)
    assert "Requesting program interpreter: /lib/ld-linux-aarch64.so.1" in program_headers
    assert "libmd.so.0" in dynamic
    assert "libselinux.so.1" in dynamic
    assert "libc.so.6" in dynamic


def test_real_distro_dpkg_query_requests_rootfs_loader_and_libmd():
    import subprocess
    import tarfile
    import tempfile

    with tarfile.open(PAYLOAD) as archive, tempfile.NamedTemporaryFile() as tmp:
        tmp.write(archive.extractfile("./usr/bin/dpkg-query").read())
        tmp.flush()
        program_headers = subprocess.check_output(["readelf", "-l", tmp.name], text=True)
        dynamic = subprocess.check_output(["readelf", "-d", tmp.name], text=True)
    assert "Requesting program interpreter: /lib/ld-linux-aarch64.so.1" in program_headers
    assert "libmd.so.0" in dynamic
    assert "libc.so.6" in dynamic


def test_tiny_rootfs_contains_bulk_apt_base_bundle():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        required = [
            "./usr/bin/apt",
            "./usr/bin/apt-get",
            "./usr/bin/apt-cache",
            "./usr/bin/apt-config",
            "./usr/lib/apt/apt-helper",
            "./usr/lib/apt/methods/http",
            "./usr/lib/apt/methods/https",
            "./lib/aarch64-linux-gnu/libapt-pkg.so.6.0",
            "./lib/aarch64-linux-gnu/libapt-private.so.0.0",
            "./lib/aarch64-linux-gnu/libstdc++.so.6",
            "./lib/aarch64-linux-gnu/libzstd.so.1",
            "./lib/aarch64-linux-gnu/libsystemd.so.0",
            "./etc/apt/apt.conf.d",
            "./var/lib/apt/lists/partial",
            "./var/cache/apt/archives/partial",
        ]
        for member in required:
            assert member in names
        for member in ["./usr/bin/apt", "./usr/bin/apt-get", "./usr/bin/apt-cache", "./usr/bin/apt-config", "./lib/aarch64-linux-gnu/libapt-pkg.so.6.0"]:
            assert archive.extractfile(member).read(4) == b"\x7fELF"


def test_tiny_rootfs_contains_local_deb_install_smoke_package():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./var/cache/apt/archives/alr-smoke_1.0_arm64.deb" in names
        assert "./usr/bin/dpkg-deb" in names
        assert "./var/log" in names
        deb = archive.extractfile("./var/cache/apt/archives/alr-smoke_1.0_arm64.deb").read()
        assert deb.startswith(b"!<arch>\n")
        assert archive.extractfile("./usr/bin/dpkg-deb").read(4) == b"\x7fELF"


def test_tiny_rootfs_contains_dpkg_install_state_and_dev_placeholders():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        for member in [
            "./dev",
            "./dev/null",
            "./var/lib/dpkg/triggers",
            "./var/lib/dpkg/triggers/File",
            "./var/lib/dpkg/triggers/Unincorp",
            "./var/lib/dpkg/triggers/Lock",
            "./var/lib/dpkg/lock",
            "./var/lib/dpkg/lock-frontend",
            "./var/lib/dpkg/available",
        ]:
            assert member in names


def test_tiny_rootfs_contains_dpkg_install_helper_programs():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        for member in [
            "./usr/bin/rm",
            "./usr/bin/tar",
            "./usr/bin/diff",
            "./usr/sbin/ldconfig",
            "./sbin/ldconfig.real",
            "./usr/sbin/start-stop-daemon",
            "./usr/bin/dpkg-trigger",
            "./lib/aarch64-linux-gnu/libacl.so.1",
        ]:
            assert member in names


def test_tiny_rootfs_cleans_host_dpkg_needrestart_and_contains_dpkg_split():
    import tarfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./etc/dpkg/dpkg.cfg.d/needrestart" not in names
        assert "./etc/dpkg/dpkg.cfg.d/00-androlinux-minimal" in names
        assert "./usr/bin/dpkg-split" in names
        minimal = archive.extractfile("./etc/dpkg/dpkg.cfg.d/00-androlinux-minimal").read().decode()
        assert "needrestart" in minimal


def test_tiny_rootfs_contains_alr_path_interposer():
    """The LD_PRELOAD path-mediation interposer must ship as a valid aarch64
    shared object so dynamic guests (GIMP) rewrite filesystem paths to the
    rootfs in-process, without a ptrace round-trip per file op."""
    import struct
    import subprocess
    import tarfile
    import tempfile

    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        assert "./usr/lib/androlinux/libalr_interpose.so" in names
        blob = archive.extractfile("./usr/lib/androlinux/libalr_interpose.so").read()
    assert blob[:4] == b"\x7fELF"
    assert blob[4] == 2  # ELFCLASS64
    e_type = struct.unpack_from("<H", blob, 16)[0]
    e_machine = struct.unpack_from("<H", blob, 18)[0]
    assert e_type == 3  # ET_DYN — shared object suitable for LD_PRELOAD
    assert e_machine == 183  # EM_AARCH64
    with tempfile.NamedTemporaryFile() as tmp:
        tmp.write(blob)
        tmp.flush()
        dynamic = subprocess.check_output(["readelf", "-d", tmp.name], text=True)
    assert "libc.so.6" in dynamic
