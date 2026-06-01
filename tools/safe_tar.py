from __future__ import annotations

import tarfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


class UnsafeTarArchive(ValueError):
    pass


@dataclass(frozen=True)
class TarInspectionResult:
    file_count: int
    total_size: int
    paths: list[str]


def inspect_tar_for_rootfs(path: str | Path) -> TarInspectionResult:
    archive_path = Path(path)
    paths: list[str] = []
    total_size = 0

    with tarfile.open(archive_path, "r:*") as tar:
        for member in tar.getmembers():
            _validate_member(member)
            if member.isfile():
                paths.append(member.name)
                total_size += member.size
            elif member.isdir():
                continue
            elif member.issym() or member.islnk():
                paths.append(member.name)
            else:
                raise UnsafeTarArchive(f"unsupported tar member type: {member.name}")

    return TarInspectionResult(
        file_count=len(paths),
        total_size=total_size,
        paths=paths,
    )


def _validate_member(member: tarfile.TarInfo) -> None:
    if not _is_safe_relative_path(member.name):
        raise UnsafeTarArchive(f"unsafe path: {member.name}")
    if member.issym() or member.islnk():
        if not _is_safe_relative_path(member.linkname):
            raise UnsafeTarArchive(f"unsafe link target: {member.name} -> {member.linkname}")
    if member.ischr() or member.isblk() or member.isfifo():
        raise UnsafeTarArchive(f"device-like tar member not allowed: {member.name}")


def _is_safe_relative_path(value: str) -> bool:
    candidate = PurePosixPath(value)
    return bool(value) and not candidate.is_absolute() and ".." not in candidate.parts
