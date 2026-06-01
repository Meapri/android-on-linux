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


@dataclass(frozen=True)
class TarMember:
    """A safety-validated tar member, normalized for overlay-guard reasoning.

    ``name``/``linkname`` are normalized to rootfs-relative POSIX paths (a single
    leading ``./`` stripped). ``kind`` is one of file/dir/symlink/hardlink.
    """

    name: str
    kind: str
    linkname: str
    size: int
    mode: int


def _normalize_member_path(value: str) -> str:
    text = value
    while text.startswith("./"):
        text = text[2:]
    return text


def inspect_tar_members(path: str | Path) -> list[TarMember]:
    """Return per-member metadata (kind/linkname/size/mode) for an archive.

    Reuses the same safety validation as :func:`inspect_tar_for_rootfs` (rejects
    parent traversal, absolute paths, escaping links, device nodes) but exposes
    the member structure the overlay guard needs to detect base-lib downgrades.
    """
    archive_path = Path(path)
    members: list[TarMember] = []

    with tarfile.open(archive_path, "r:*") as tar:
        for member in tar.getmembers():
            # Host-side analysis: enforce member-name safety and reject device
            # nodes, but tolerate absolute symlink TARGETS — a real Debian base
            # rootfs legitimately ships them (fontconfig conf.d, etc.). In-tree
            # link safety is enforced separately by the device extractor.
            if not _is_safe_relative_path(member.name):
                raise UnsafeTarArchive(f"unsafe path: {member.name}")
            if member.ischr() or member.isblk() or member.isfifo():
                raise UnsafeTarArchive(f"device-like tar member not allowed: {member.name}")
            if member.isdir():
                kind = "dir"
            elif member.issym():
                kind = "symlink"
            elif member.islnk():
                kind = "hardlink"
            elif member.isfile():
                kind = "file"
            else:
                raise UnsafeTarArchive(f"unsupported tar member type: {member.name}")
            members.append(
                TarMember(
                    name=_normalize_member_path(member.name),
                    kind=kind,
                    linkname=_normalize_member_path(member.linkname) if member.linkname else "",
                    size=member.size if member.isfile() else 0,
                    mode=member.mode,
                )
            )

    return members


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
