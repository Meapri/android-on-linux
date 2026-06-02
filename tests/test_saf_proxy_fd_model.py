"""SAF 직통 프록시 v2 — fd-주입 강등 정책 순수 모델 검증.

실행: cd /Users/naen/Documents/alr-product-ux && \
  PATH="$HOME/.local/bin:$PATH" uvx --with pytest pytest tests/test_saf_proxy_fd_model.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from saf_bridge_model import (  # noqa: E402
    SAF_MOUNT_ROOT,
    AccessMode,
    BridgeMode,
    SafBridge,
    SafMount,
)
from saf_proxy_fd_model import (  # noqa: E402
    EACCES,
    EROFS,
    DirSynth,
    FdBacking,
    FdInjection,
    GuestAccess,
    ProxyAction,
    ProxyFdResolution,
    decide_proxy,
)


DL_URI = "content://com.android.externalstorage.documents/tree/primary%3ADownload"
DCIM_URI = "content://com.android.externalstorage.documents/tree/primary%3ADCIM"
DOCS_URI = "content://com.android.externalstorage.documents/tree/primary%3ADocuments"


def make_bridge() -> SafBridge:
    """downloads = proxy+rw, dcim = proxy+ro, docs = copy+rw."""
    b = SafBridge()
    b.add_mount(SafMount(DL_URI, "downloads", AccessMode.READ_WRITE, BridgeMode.PROXY))
    b.add_mount(SafMount(DCIM_URI, "dcim", AccessMode.READ_ONLY, BridgeMode.PROXY))
    b.add_mount(SafMount(DOCS_URI, "docs", AccessMode.READ_WRITE, BridgeMode.COPY))
    return b


DL = f"{SAF_MOUNT_ROOT}/downloads"
DCIM = f"{SAF_MOUNT_ROOT}/dcim"
DOCS = f"{SAF_MOUNT_ROOT}/docs"


# --- FdBacking 능력 프로파일 --------------------------------------------------


def test_fd_backing_capability_flags():
    assert FdBacking.REGULAR_FILE.is_seekable
    assert FdBacking.REGULAR_FILE.is_mmappable
    assert FdBacking.SEEKABLE.is_seekable
    assert not FdBacking.SEEKABLE.is_mmappable  # mmap 은 실파일만 보장
    assert not FdBacking.PIPE.is_seekable
    assert not FdBacking.PIPE.is_mmappable
    # UNKNOWN 은 보수적으로 pipe 취급(seek/mmap 불가).
    assert not FdBacking.UNKNOWN.is_seekable
    assert not FdBacking.UNKNOWN.is_mmappable


# --- 라이브 fd 직통(최적 경로) ------------------------------------------------


def test_regular_file_sequential_substitutes_live_fd():
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/a.bin",
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.SEQUENTIAL,
    )
    assert isinstance(res, ProxyFdResolution)
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD
    assert isinstance(res.injection, FdInjection)
    assert res.injection.source == "saf_pfd"
    assert res.injection.writeback_on_close is False  # 라이브 = 즉시 반영
    assert res.degraded_from_live is False
    assert res.dir_synth is None


def test_regular_file_random_access_still_live():
    # 실파일은 seekable → 임의접근도 라이브 fd 로 충분.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/db.sqlite",
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.RANDOM,
    )
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD
    assert res.injection.requires_seek is True
    assert res.injection.requires_mmap is False


def test_regular_file_mmap_is_live():
    # 실파일만 mmap 보장 → 라이브 fd.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/img.dat",
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.MMAP,
    )
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD
    assert res.injection.requires_mmap is True


def test_pipe_sequential_is_live_fd():
    # pipe-backed 라도 순차 read/write 만이면 라이브 fd 로 충분(강등 불요).
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/stream.log",
        fd_backing=FdBacking.PIPE, access=GuestAccess.SEQUENTIAL,
    )
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD
    assert res.degraded_from_live is False


# --- pipe-backed + seek/mmap 요구 → memfd 강등 -------------------------------


def test_pipe_random_access_degrades_to_memfd():
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/db.sqlite",
        fd_backing=FdBacking.PIPE, access=GuestAccess.RANDOM,
    )
    assert res.action is ProxyAction.MEMFD_CLONE
    assert res.degraded_from_live is True
    assert res.injection.source == "memfd"
    # rw 마운트 + 변형 의도 아님(read) → write-back 불요.
    assert res.injection.writeback_on_close is False


def test_pipe_mmap_degrades_to_memfd():
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/mapped.bin",
        fd_backing=FdBacking.PIPE, access=GuestAccess.MMAP,
    )
    assert res.action is ProxyAction.MEMFD_CLONE
    assert res.degraded_from_live is True


def test_seekable_but_mmap_required_degrades_to_memfd():
    # SEEKABLE 은 pread/pwrite OK 지만 mmap 보장 안 됨 → mmap 요구 시 memfd.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/x.bin",
        fd_backing=FdBacking.SEEKABLE, access=GuestAccess.MMAP,
    )
    assert res.action is ProxyAction.MEMFD_CLONE
    assert res.degraded_from_live is True
    assert res.injection.requires_mmap is True


def test_seekable_random_is_live_fd():
    # SEEKABLE + 임의접근(mmap 아님) → 라이브 fd 충분.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/x.bin",
        fd_backing=FdBacking.SEEKABLE, access=GuestAccess.RANDOM,
    )
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD


def test_unknown_backing_treated_as_pipe_when_seek_needed():
    # UNKNOWN 은 보수적으로 pipe → seek 요구면 memfd 강등.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/x.bin",
        fd_backing=FdBacking.UNKNOWN, access=GuestAccess.RANDOM,
    )
    assert res.action is ProxyAction.MEMFD_CLONE


# --- memfd 강등의 write-back(rw + 쓰기 의도일 때만) --------------------------


def test_memfd_writeback_on_rw_write_intent():
    # pipe + 쓰기 의도 + rw 마운트 → memfd 강등 + close write-back.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/out.bin",
        write=True,
        fd_backing=FdBacking.PIPE, access=GuestAccess.RANDOM,
    )
    assert res.action is ProxyAction.MEMFD_CLONE
    assert res.injection.writeback_on_close is True


def test_memfd_no_writeback_on_read_only_intent():
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/in.bin",
        write=False,
        fd_backing=FdBacking.PIPE, access=GuestAccess.RANDOM,
    )
    assert res.action is ProxyAction.MEMFD_CLONE
    assert res.injection.writeback_on_close is False


# --- 디렉터리 open → getdents 합성(fd 치환 불가) ----------------------------


def test_directory_open_synthesizes_getdents():
    b = make_bridge()
    res = decide_proxy(b, f"{DL}/subdir", is_dir=True)
    assert res.action is ProxyAction.GETDENTS_SYNTH
    assert isinstance(res.dir_synth, DirSynth)
    assert res.dir_synth.mount_prefix == DL
    assert res.dir_synth.rel_path == "subdir"
    assert res.dir_synth.tree_uri == DL_URI
    assert res.dir_synth.read_only is False
    assert res.injection is None  # fd 치환 아님


def test_directory_open_at_mount_root():
    b = make_bridge()
    res = decide_proxy(b, DL, is_dir=True)
    assert res.action is ProxyAction.GETDENTS_SYNTH
    assert res.dir_synth.rel_path == ""  # 마운트 루트 listing


def test_directory_on_ro_mount_carries_read_only():
    b = make_bridge()
    res = decide_proxy(b, f"{DCIM}/2024", is_dir=True)
    assert res.action is ProxyAction.GETDENTS_SYNTH
    assert res.dir_synth.read_only is True


def test_directory_takes_priority_over_backing():
    # is_dir 면 fd_backing/access 와 무관하게 getdents 합성.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DL}/d", is_dir=True,
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.MMAP,
    )
    assert res.action is ProxyAction.GETDENTS_SYNTH


# --- ro 마운트 읽기는 라이브 fd, read_only 전파 ------------------------------


def test_ro_proxy_read_substitutes_live_fd_read_only():
    b = make_bridge()
    res = decide_proxy(
        b, f"{DCIM}/photo.jpg",
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.SEQUENTIAL,
    )
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD
    assert res.injection.read_only is True


# --- ro/escape/unknown-label 정책이 강등보다 먼저 (불변식 3) ----------------


@pytest.mark.parametrize("write,create", [(True, False), (False, True)])
def test_ro_mount_write_denies_erofs_before_any_degradation(write, create):
    # dcim = proxy+ro. 쓰기/생성 의도면 backing 무관 DENY(EROFS) — fd-주입 비대상.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DCIM}/x.jpg",
        write=write, create=create,
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.SEQUENTIAL,
    )
    assert res.action is ProxyAction.DENY
    assert res.deny_errno == EROFS
    assert res.injection is None
    assert res.dir_synth is None


def test_unknown_label_under_mount_root_denies_eacces():
    b = make_bridge()
    res = decide_proxy(b, f"{SAF_MOUNT_ROOT}/music/song.mp3")
    assert res.action is ProxyAction.DENY
    assert res.deny_errno == EACCES
    assert res.injection is None


def test_escape_out_of_proxy_mount_is_copy_fallback_not_fd_injection():
    # downloads 밖(../../etc)으로 정규화 → /mnt/etc = NOT_MAPPED → FALLTHROUGH.
    # fd-주입 절대 비대상(런타임 rootfs 중재가 다시 막음).
    b = make_bridge()
    res = decide_proxy(b, f"{DL}/../../etc/passwd")
    assert res.action is ProxyAction.COPY_FALLBACK
    assert res.injection is None
    assert res.dir_synth is None
    assert "fallthrough" in res.reason


def test_substitute_fd_target_never_escapes_prefix():
    # 불변식 2: 라이브/ memfd 주입 대상은 항상 mount prefix 안.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DCIM}/./sub/../x.jpg",
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.SEQUENTIAL,
    )
    assert res.action is ProxyAction.SUBSTITUTE_LIVE_FD
    # rel 정규화 후 prefix 안에 머문다(reason 으로 backing/access 추적 가능).
    assert res.injection.read_only is True


# --- copy 마운트는 2차 강등 없이 copy 폴백 ----------------------------------


def test_copy_mount_yields_copy_fallback():
    # docs = copy+rw. 1차가 REWRITE_PATH → 2차 강등 없이 COPY_FALLBACK.
    b = make_bridge()
    res = decide_proxy(
        b, f"{DOCS}/notes.txt",
        fd_backing=FdBacking.REGULAR_FILE, access=GuestAccess.MMAP,
    )
    assert res.action is ProxyAction.COPY_FALLBACK
    assert res.injection is None
    assert res.dir_synth is None
    assert "copy-mode" in res.reason


def test_copy_mount_write_is_copy_fallback():
    b = make_bridge()
    res = decide_proxy(b, f"{DOCS}/out.txt", write=True)
    assert res.action is ProxyAction.COPY_FALLBACK


# --- SAF 밖 경로(프록시 마운트 아님) -----------------------------------------


def test_non_saf_path_is_copy_fallback_fallthrough():
    b = make_bridge()
    res = decide_proxy(b, "/etc/passwd")
    assert res.action is ProxyAction.COPY_FALLBACK
    assert res.injection is None
    assert "fallthrough" in res.reason


# --- 강등 결정의 결정성(같은 입력 → 같은 액션) ------------------------------


@pytest.mark.parametrize(
    "backing,access,expected",
    [
        (FdBacking.REGULAR_FILE, GuestAccess.SEQUENTIAL, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.REGULAR_FILE, GuestAccess.RANDOM, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.REGULAR_FILE, GuestAccess.MMAP, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.SEEKABLE, GuestAccess.SEQUENTIAL, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.SEEKABLE, GuestAccess.RANDOM, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.SEEKABLE, GuestAccess.MMAP, ProxyAction.MEMFD_CLONE),
        (FdBacking.PIPE, GuestAccess.SEQUENTIAL, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.PIPE, GuestAccess.RANDOM, ProxyAction.MEMFD_CLONE),
        (FdBacking.PIPE, GuestAccess.MMAP, ProxyAction.MEMFD_CLONE),
        (FdBacking.UNKNOWN, GuestAccess.SEQUENTIAL, ProxyAction.SUBSTITUTE_LIVE_FD),
        (FdBacking.UNKNOWN, GuestAccess.RANDOM, ProxyAction.MEMFD_CLONE),
        (FdBacking.UNKNOWN, GuestAccess.MMAP, ProxyAction.MEMFD_CLONE),
    ],
)
def test_degradation_matrix_is_deterministic(backing, access, expected):
    b = make_bridge()
    res = decide_proxy(b, f"{DL}/file.bin", fd_backing=backing, access=access)
    assert res.action is expected
