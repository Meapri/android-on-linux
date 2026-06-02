"""SAF 파일 브리지 순수 모델 검증.

실행: cd /Users/naen/Documents/alr-product-ux && \
  PATH="$HOME/.local/bin:$PATH" uvx --with pytest pytest tests/test_saf_bridge_model.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# tools/ 를 import path 에 올린다(다른 host 테스트와 동일 관례).
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from saf_bridge_model import (  # noqa: E402
    SAF_MOUNT_ROOT,
    SHARE_INBOX,
    AccessMode,
    BridgeError,
    BridgeErrorKind,
    IncomingShare,
    MediaStoreTarget,
    Operation,
    ResolvedPath,
    SafBridge,
    SafMount,
    export_to_mediastore,
    normalize_guest_path,
    route_incoming_share,
    sanitize_share_name,
)


DL_URI = "content://com.android.externalstorage.documents/tree/primary%3ADownload"
DCIM_URI = "content://com.android.externalstorage.documents/tree/primary%3ADCIM"


def make_bridge() -> SafBridge:
    b = SafBridge()
    b.add_mount(SafMount(DL_URI, "downloads", AccessMode.READ_WRITE))
    b.add_mount(SafMount(DCIM_URI, "dcim", AccessMode.READ_ONLY))
    return b


# --- 마운트 모델 / 검증 --------------------------------------------------------


def test_mount_guest_point_is_under_saf_mount_root():
    m = SafMount(DL_URI, "downloads", AccessMode.READ_WRITE)
    assert m.guest_mount_point == f"{SAF_MOUNT_ROOT}/downloads"


def test_bad_label_rejected():
    with pytest.raises(BridgeError) as ei:
        SafMount(DL_URI, "../etc", AccessMode.READ_WRITE)
    assert ei.value.kind is BridgeErrorKind.BAD_LABEL


def test_non_tree_uri_rejected():
    with pytest.raises(BridgeError) as ei:
        SafMount("content://media/external/images", "imgs")
    assert ei.value.kind is BridgeErrorKind.BAD_URI

    with pytest.raises(BridgeError) as ei2:
        SafMount("file:///sdcard/Download", "dl")
    assert ei2.value.kind is BridgeErrorKind.BAD_URI


# --- 경로 정규화 --------------------------------------------------------------


def test_normalize_strips_dot_and_resolves_dotdot():
    assert normalize_guest_path("/a/./b/../c") == ["a", "c"]
    assert normalize_guest_path("/a//b/") == ["a", "b"]
    assert normalize_guest_path("a/b/c") == ["a", "b", "c"]


def test_normalize_escape_above_root_raises():
    with pytest.raises(BridgeError) as ei:
        normalize_guest_path("/a/../..")
    assert ei.value.kind is BridgeErrorKind.ESCAPE


def test_normalize_empty_raises():
    with pytest.raises(BridgeError) as ei:
        normalize_guest_path("")
    assert ei.value.kind is BridgeErrorKind.NOT_MAPPED


# --- URI <-> 경로 매핑 (핵심) -------------------------------------------------


def test_resolve_maps_guest_path_to_tree_uri_and_rel():
    b = make_bridge()
    r = b.resolve(f"{SAF_MOUNT_ROOT}/downloads/proj/report.txt", Operation.READ)
    assert isinstance(r, ResolvedPath)
    assert r.tree_uri == DL_URI
    assert r.label == "downloads"
    assert r.mode is AccessMode.READ_WRITE
    assert r.rel_components == ("proj", "report.txt")
    assert r.rel_path == "proj/report.txt"
    assert r.is_mount_root is False


def test_resolve_mount_root_itself_has_empty_rel():
    b = make_bridge()
    r = b.resolve(f"{SAF_MOUNT_ROOT}/downloads", Operation.LIST)
    assert r.is_mount_root is True
    assert r.rel_components == ()


def test_is_saf_path_classification():
    b = make_bridge()
    assert b.is_saf_path(f"{SAF_MOUNT_ROOT}/downloads/x") is True
    assert b.is_saf_path("/etc/passwd") is False
    assert b.is_saf_path("/root/notes.txt") is False
    # 마운트 루트 자체(label만)는 SAF 경로로 인정(루트 listing).
    assert b.is_saf_path(f"{SAF_MOUNT_ROOT}/downloads") is True
    # SAF_MOUNT_ROOT 그 자체는 어떤 label 도 아님 -> not mapped.
    assert b.is_saf_path(SAF_MOUNT_ROOT) is False


def test_resolve_outside_mount_root_is_not_mapped():
    b = make_bridge()
    with pytest.raises(BridgeError) as ei:
        b.resolve("/etc/passwd", Operation.READ)
    assert ei.value.kind is BridgeErrorKind.NOT_MAPPED


def test_resolve_unknown_label():
    b = make_bridge()
    with pytest.raises(BridgeError) as ei:
        b.resolve(f"{SAF_MOUNT_ROOT}/music/song.mp3", Operation.READ)
    assert ei.value.kind is BridgeErrorKind.UNKNOWN_LABEL


# --- traversal / rootfs escape 차단 -------------------------------------------


def test_resolve_traversal_out_of_mount_is_blocked():
    b = make_bridge()
    # downloads 밖(../../etc)으로 빠져나가려는 시도.
    with pytest.raises(BridgeError) as ei:
        b.resolve(f"{SAF_MOUNT_ROOT}/downloads/../../etc/passwd", Operation.READ)
    # ../../etc -> /mnt/etc 로 정규화되어 SAF 마운트 밖 = NOT_MAPPED
    assert ei.value.kind in (BridgeErrorKind.NOT_MAPPED, BridgeErrorKind.ESCAPE)


def test_resolve_traversal_into_sibling_mount_does_not_inherit_rw():
    b = make_bridge()
    # dcim(ro) 에서 .. 로 downloads(rw) 로 넘어가 쓰기? 정규화하면
    # /mnt/android/downloads 로 바뀌므로 downloads 의 rw 정책이 적용되는 것은
    # "경로가 실제로 그 마운트로 정규화"된 결과 — 의도된 동작.
    # 핵심: ro 마운트 경로 그대로는 절대 쓰기 못 함(아래 별도 테스트).
    r = b.resolve(f"{SAF_MOUNT_ROOT}/dcim/../downloads/x", Operation.WRITE)
    assert r.label == "downloads"
    assert r.mode is AccessMode.READ_WRITE


def test_resolve_escape_above_everything_blocked():
    b = make_bridge()
    with pytest.raises(BridgeError) as ei:
        b.resolve(f"{SAF_MOUNT_ROOT}/downloads/../../../..", Operation.READ)
    assert ei.value.kind in (BridgeErrorKind.NOT_MAPPED, BridgeErrorKind.ESCAPE)


# --- read-only 위반 거부 ------------------------------------------------------


@pytest.mark.parametrize("op", [Operation.WRITE, Operation.CREATE, Operation.DELETE])
def test_read_only_mount_rejects_mutations(op):
    b = make_bridge()
    with pytest.raises(BridgeError) as ei:
        b.resolve(f"{SAF_MOUNT_ROOT}/dcim/photo.jpg", op)
    assert ei.value.kind is BridgeErrorKind.READ_ONLY_VIOLATION


@pytest.mark.parametrize("op", [Operation.READ, Operation.LIST, Operation.STAT])
def test_read_only_mount_allows_reads(op):
    b = make_bridge()
    r = b.resolve(f"{SAF_MOUNT_ROOT}/dcim/photo.jpg", op)
    assert r.mode is AccessMode.READ_ONLY
    assert r.rel_components == ("photo.jpg",)


def test_read_write_mount_allows_mutations():
    b = make_bridge()
    r = b.resolve(f"{SAF_MOUNT_ROOT}/downloads/out.png", Operation.CREATE)
    assert r.mode is AccessMode.READ_WRITE


def test_operation_mutates_flag():
    assert Operation.WRITE.mutates
    assert Operation.CREATE.mutates
    assert Operation.DELETE.mutates
    assert not Operation.READ.mutates
    assert not Operation.LIST.mutates
    assert not Operation.STAT.mutates


# --- 마운트 등록/해제 ---------------------------------------------------------


def test_add_remove_mount():
    b = SafBridge()
    b.add_mount(SafMount(DL_URI, "downloads"))
    assert len(b.mounts()) == 1
    assert b.mount_for_label("downloads").tree_uri == DL_URI
    b.remove_mount("downloads")
    assert b.mounts() == ()
    with pytest.raises(BridgeError) as ei:
        b.mount_for_label("downloads")
    assert ei.value.kind is BridgeErrorKind.UNKNOWN_LABEL


def test_add_mount_same_label_replaces():
    b = SafBridge()
    b.add_mount(SafMount(DL_URI, "downloads", AccessMode.READ_ONLY))
    b.add_mount(SafMount(DL_URI, "downloads", AccessMode.READ_WRITE))
    assert len(b.mounts()) == 1
    assert b.mount_for_label("downloads").mode is AccessMode.READ_WRITE


# --- share intent 라우팅 ------------------------------------------------------


def test_share_routes_into_inbox():
    share = IncomingShare("content://x/doc/1", "report.pdf", "application/pdf")
    dest = route_incoming_share(share)
    assert dest == f"{SHARE_INBOX}/report.pdf"
    assert dest.startswith(SHARE_INBOX + "/")


def test_share_name_traversal_sanitized():
    share = IncomingShare("content://x/doc/1", "../../etc/passwd")
    dest = route_incoming_share(share)
    # 경로 구분자 제거 -> 마지막 컴포넌트만, 그것도 안전화.
    assert ".." not in dest
    assert dest == f"{SHARE_INBOX}/passwd"
    assert dest.startswith(SHARE_INBOX + "/")


def test_share_name_weird_chars_become_underscore():
    assert sanitize_share_name("a*b?c.txt") == "a_b_c.txt"
    assert sanitize_share_name("") == "shared-file"
    assert sanitize_share_name("..") == "shared-file"
    assert sanitize_share_name("/") == "shared-file"
    assert sanitize_share_name("good name (1).png") == "good name (1).png"


def test_share_backslash_path_sanitized():
    assert sanitize_share_name("C:\\Users\\x\\evil.exe") == "evil.exe"


# --- MediaStore export --------------------------------------------------------


def test_export_to_mediastore_builds_target():
    t = export_to_mediastore(
        "/root/out/result.png",
        collection="images",
        display_name="result.png",
        relative_path="Pictures/AndroLinux",
        mime_type="image/png",
    )
    assert isinstance(t, MediaStoreTarget)
    assert t.collection == "images"
    assert t.display_name == "result.png"
    assert t.relative_path == "Pictures/AndroLinux"
    assert t.mime_type == "image/png"


def test_export_unknown_collection_rejected():
    with pytest.raises(BridgeError) as ei:
        export_to_mediastore("/root/x", "trash", "x")
    assert ei.value.kind is BridgeErrorKind.NOT_MAPPED


def test_export_sanitizes_display_name():
    t = export_to_mediastore("/root/x", "documents", "../../evil.txt")
    assert ".." not in t.display_name
    assert t.display_name == "evil.txt"


def test_export_default_relative_path():
    t = export_to_mediastore("/root/x", "downloads", "a.txt")
    assert t.relative_path == "Download/AndroLinux"
