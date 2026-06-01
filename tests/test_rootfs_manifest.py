import hashlib
from pathlib import Path

import pytest

from tools.rootfs_manifest import RootfsManifest, RootfsAsset, plan_rootfs_install


def test_rootfs_manifest_computes_install_paths_under_app_files_rootfs():
    manifest = RootfsManifest(
        name="debian-arm64",
        version="bookworm-slim-2026-05-gui-gpu-v35",
        assets=[
            RootfsAsset(path="rootfs.tar.zst", sha256="a" * 64, size_bytes=123),
        ],
    )

    plan = plan_rootfs_install(
        manifest,
        app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
    )

    assert plan.rootfs_dir == "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64"
    assert plan.marker_path == "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64/.alr-installed-bookworm-slim-2026-05-gui-gpu-v35"
    assert plan.asset_destinations == {
        "rootfs.tar.zst": "/data/user/0/dev.chanwoo.androlinux/files/rootfs/.downloads/debian-arm64/bookworm-slim-2026-05-gui-gpu-v35/rootfs.tar.zst"
    }


def test_rootfs_manifest_rejects_path_traversal_asset_names():
    with pytest.raises(ValueError, match="asset path must be relative and safe"):
        RootfsManifest(
            name="debian-arm64",
            version="bad",
            assets=[RootfsAsset(path="../escape.tar", sha256="a" * 64, size_bytes=1)],
        )


def test_rootfs_asset_verifies_sha256(tmp_path: Path):
    payload = b"tiny rootfs payload"
    archive = tmp_path / "rootfs.tar"
    archive.write_bytes(payload)
    asset = RootfsAsset(
        path="rootfs.tar",
        sha256=hashlib.sha256(payload).hexdigest(),
        size_bytes=len(payload),
    )

    assert asset.verify_file(archive) is True


def test_rootfs_asset_rejects_wrong_sha256(tmp_path: Path):
    archive = tmp_path / "rootfs.tar"
    archive.write_bytes(b"tampered")
    asset = RootfsAsset(path="rootfs.tar", sha256="0" * 64, size_bytes=8)

    assert asset.verify_file(archive) is False
