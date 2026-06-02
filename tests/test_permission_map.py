"""T4 host 검증: tools/permission_map 의 권한 매핑 순수 로직.

uvx --with pytest pytest tests/test_permission_map.py -q
"""
from __future__ import annotations

import pytest

from tools.permission_map import (
    API_TIRAMISU,
    AndroidGrantKind,
    GuestPermission,
    UnknownGuestPermission,
    normalize_guest_permission,
    required_permissions,
)

API32 = 32  # TIRAMISU 직전(미디어 세분화/알림 런타임화 이전)
API33 = API_TIRAMISU
API34 = 34


# --- 정규화 -----------------------------------------------------------------

def test_normalize_canonical_tokens():
    assert normalize_guest_permission("storage-read") is GuestPermission.STORAGE_READ
    assert normalize_guest_permission("network") is GuestPermission.NETWORK
    assert normalize_guest_permission("notifications") is GuestPermission.NOTIFICATIONS


def test_normalize_aliases_and_case_insensitive():
    assert normalize_guest_permission("MIC") is GuestPermission.MICROPHONE
    assert normalize_guest_permission(" GPS ") is GuestPermission.LOCATION
    assert normalize_guest_permission("internet") is GuestPermission.NETWORK
    assert normalize_guest_permission("write-storage") is GuestPermission.STORAGE_WRITE


def test_unknown_guest_permission_rejected():
    with pytest.raises(UnknownGuestPermission):
        normalize_guest_permission("bluetooth")
    with pytest.raises(UnknownGuestPermission):
        normalize_guest_permission("")
    with pytest.raises(UnknownGuestPermission):
        required_permissions(["telepathy"], api_level=API33)


# --- network = install-time(INTERNET) ---------------------------------------

def test_network_maps_to_install_time_internet():
    plan = required_permissions(["network"], api_level=API33)
    assert plan.install_time_permissions == ("android.permission.INTERNET",)
    # install-time 은 런타임 프롬프트 없음.
    assert plan.runtime_permissions == ()
    perm = plan.permissions[0]
    assert perm.kind is AndroidGrantKind.INSTALL_TIME
    assert perm.needs_manifest_entry
    assert not perm.needs_runtime_prompt


# --- storage → SAF 우선이면 권한 불요 ----------------------------------------

def test_storage_read_via_saf_needs_no_media_permission():
    plan = required_permissions(["storage-read"], api_level=API33, storage_via_saf=True)
    assert plan.uses_saf
    assert plan.storage_via_saf
    # manifest 권한/런타임 권한 모두 비어야 한다(SAF 는 인텐트).
    assert plan.manifest_permissions == ()
    assert plan.runtime_permissions == ()
    assert all(p.kind is AndroidGrantKind.SAF for p in plan.permissions)


def test_storage_read_without_saf_falls_back_to_media_perms_api33():
    plan = required_permissions(["storage-read"], api_level=API33, storage_via_saf=False)
    assert not plan.uses_saf
    assert plan.runtime_permissions == (
        "android.permission.READ_MEDIA_AUDIO",
        "android.permission.READ_MEDIA_IMAGES",
        "android.permission.READ_MEDIA_VIDEO",
    )


def test_storage_read_without_saf_api32_single_legacy_permission():
    plan = required_permissions(["storage-read"], api_level=API32, storage_via_saf=False)
    assert plan.runtime_permissions == ("android.permission.READ_EXTERNAL_STORAGE",)
    # API33 의 세분화 권한이 절대 새어나오면 안 됨.
    assert all("READ_MEDIA" not in n for n in plan.runtime_permissions)


def test_storage_write_always_forces_saf_even_when_saf_flag_false():
    # scoped storage(API29+)에서 외부 쓰기 권한은 무력 → 항상 SAF.
    plan = required_permissions(["storage-write"], api_level=API33, storage_via_saf=False)
    assert plan.uses_saf
    assert plan.manifest_permissions == ()
    assert "WRITE_EXTERNAL_STORAGE" not in "".join(plan.manifest_permissions)


# --- API33+ 미디어 세분화 vs 이전 -------------------------------------------

def test_api33_media_granularity_present_api32_absent():
    p33 = required_permissions(["storage-read"], api_level=API33, storage_via_saf=False)
    p32 = required_permissions(["storage-read"], api_level=API32, storage_via_saf=False)
    assert "android.permission.READ_MEDIA_IMAGES" in p33.runtime_permissions
    assert "android.permission.READ_MEDIA_IMAGES" not in p32.runtime_permissions


# --- 알림: API33+ 런타임, 이전엔 권한 불요 ------------------------------------

def test_notifications_runtime_on_api33():
    plan = required_permissions(["notifications"], api_level=API33)
    assert plan.runtime_permissions == ("android.permission.POST_NOTIFICATIONS",)


def test_notifications_no_permission_pre_api33():
    plan = required_permissions(["notifications"], api_level=API32)
    assert plan.permissions == ()
    assert plan.manifest_permissions == ()
    assert plan.runtime_permissions == ()


# --- camera / microphone / location 런타임 dangerous ------------------------

def test_camera_microphone_runtime():
    plan = required_permissions(["camera", "microphone"], api_level=API33)
    assert plan.runtime_permissions == (
        "android.permission.CAMERA",
        "android.permission.RECORD_AUDIO",
    )


def test_location_requests_both_coarse_and_fine():
    plan = required_permissions(["location"], api_level=API33)
    assert plan.runtime_permissions == (
        "android.permission.ACCESS_COARSE_LOCATION",
        "android.permission.ACCESS_FINE_LOCATION",
    )


# --- 결합/중복 제거/정렬 -----------------------------------------------------

def test_combined_set_dedup_and_split_install_vs_runtime():
    plan = required_permissions(
        ["network", "camera", "storage-read", "notifications"],
        api_level=API34,
        storage_via_saf=True,
    )
    # install-time = INTERNET 만.
    assert plan.install_time_permissions == ("android.permission.INTERNET",)
    # 런타임 = CAMERA + POST_NOTIFICATIONS (storage 는 SAF 라 빠짐).
    assert plan.runtime_permissions == (
        "android.permission.CAMERA",
        "android.permission.POST_NOTIFICATIONS",
    )
    assert plan.uses_saf  # storage-read → SAF


def test_storage_read_and_write_both_saf_dedup_single_saf_class():
    plan = required_permissions(
        ["storage-read", "storage-write"],
        api_level=API33,
        storage_via_saf=True,
    )
    saf_entries = [p for p in plan.permissions if p.kind is AndroidGrantKind.SAF]
    # 둘 다 SAF(android_name 빈 문자열, kind SAF) → 키 중복 제거되어 1개.
    assert len(saf_entries) == 1


def test_duplicate_guest_tokens_normalized_once():
    plan = required_permissions(["network", "internet", "net"], api_level=API33)
    assert plan.install_time_permissions == ("android.permission.INTERNET",)
    assert len(plan.permissions) == 1


def test_manifest_permissions_sorted_and_unique():
    plan = required_permissions(
        ["network", "camera", "microphone", "location"],
        api_level=API33,
    )
    names = plan.manifest_permissions
    assert list(names) == sorted(set(names))


# --- 입력 검증 ---------------------------------------------------------------

def test_invalid_api_level_rejected():
    with pytest.raises(ValueError):
        required_permissions(["network"], api_level=0)
    with pytest.raises(ValueError):
        required_permissions(["network"], api_level=-1)


def test_empty_guest_set_yields_empty_plan():
    plan = required_permissions([], api_level=API33)
    assert plan.permissions == ()
    assert plan.manifest_permissions == ()
    assert plan.runtime_permissions == ()
    assert not plan.uses_saf


def test_each_permission_has_rationale():
    plan = required_permissions(
        ["network", "camera", "microphone", "location", "notifications", "storage-read"],
        api_level=API33,
        storage_via_saf=True,
    )
    assert all(p.rationale.strip() for p in plan.permissions)


# --- GuestPermission enum 직접 입력도 허용 -----------------------------------

def test_enum_input_accepted():
    plan = required_permissions([GuestPermission.NETWORK], api_level=API33)
    assert plan.install_time_permissions == ("android.permission.INTERNET",)
