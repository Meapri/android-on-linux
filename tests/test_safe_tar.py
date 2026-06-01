import io
import tarfile
from pathlib import Path

import pytest

from tools.safe_tar import inspect_tar_for_rootfs, UnsafeTarArchive


def _make_tar(path: Path, entries: dict[str, bytes], *, symlink: tuple[str, str] | None = None) -> Path:
    with tarfile.open(path, "w") as tar:
        for name, payload in entries.items():
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            tar.addfile(info, io.BytesIO(payload))
        if symlink:
            name, target = symlink
            info = tarfile.TarInfo(name)
            info.type = tarfile.SYMTYPE
            info.linkname = target
            tar.addfile(info)
    return path


def test_inspect_tar_accepts_safe_relative_files(tmp_path: Path):
    archive = _make_tar(
        tmp_path / "rootfs.tar",
        {
            "bin/hello": b"hello",
            "etc/os-release": b"ID=debian\n",
        },
    )

    result = inspect_tar_for_rootfs(archive)

    assert result.file_count == 2
    assert result.total_size == len(b"hello") + len(b"ID=debian\n")
    assert result.paths == ["bin/hello", "etc/os-release"]


def test_inspect_tar_rejects_parent_traversal(tmp_path: Path):
    archive = _make_tar(tmp_path / "evil.tar", {"../escape": b"x"})

    with pytest.raises(UnsafeTarArchive, match="unsafe path"):
        inspect_tar_for_rootfs(archive)


def test_inspect_tar_rejects_absolute_paths(tmp_path: Path):
    archive = _make_tar(tmp_path / "evil.tar", {"/system/bin/sh": b"x"})

    with pytest.raises(UnsafeTarArchive, match="unsafe path"):
        inspect_tar_for_rootfs(archive)


def test_inspect_tar_rejects_symlink_escape(tmp_path: Path):
    archive = _make_tar(tmp_path / "evil.tar", {}, symlink=("lib/escape", "../../outside"))

    with pytest.raises(UnsafeTarArchive, match="unsafe link target"):
        inspect_tar_for_rootfs(archive)
