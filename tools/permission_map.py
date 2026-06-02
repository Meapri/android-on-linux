"""ALR (Android-on-Linux Runtime) 권한 매핑 모델 — 순수 로직.

게스트 Linux 앱이 요구하는 "권한 부류"(capability class)를 Android의 구체적인
권한/요청 모델로 매핑한다. UI/런타임에 의존하지 않는 순수 함수만 두어 host
pytest 로 검증한다.

설계 결정(찬우 확정 컨텍스트 반영)
--------------------------------
* 패키징 = "런타임 + 인앱 카탈로그": ALR 런타임 앱 하나가 모든 게스트 앱을
  대신 호스팅한다. 따라서 Android 권한은 *런타임 앱* 의 manifest 에 선언되고,
  게스트 앱별 권한은 ALR 가 런타임에 게이트한다(syscall/path 중재 = WS-1 의존).
* INTERNET 은 install-time(=normal) 권한이라 manifest 선언만으로 부여되고
  런타임 프롬프트가 없다. apt/카탈로그 다운로드에 필요.
* storage 는 SAF(Storage Access Framework, T5) 우선. 사용자가 문서/폴더를
  명시적으로 고르는 SAF 경로를 쓰면 READ_MEDIA_* / 광범위 저장소 권한이
  **불요**가 된다. SAF 로 못 덮는 잔여 케이스(예: 미디어 라이브러리 전체 열람
  앱)만 미디어 권한으로 강등(fallback)한다.
* camera/microphone/location/notifications 는 런타임 dangerous 권한 →
  앱 첫 사용 시 Android 시스템 프롬프트.
* API 레벨 분기: API 33(=TIRAMISU)+ 에서 저장소 미디어 권한이 세분화
  (READ_MEDIA_IMAGES/VIDEO/AUDIO)되고 POST_NOTIFICATIONS 가 런타임 권한이 됨.
  그 이전은 READ_EXTERNAL_STORAGE 단일 권한 + 알림은 권한 불요.

이 모듈은 **제안/모델**이다. 현재 manifest 를 바꾸지 않는다(MainActivity/
AndroidManifest 직접 수정 금지). androidmanifest-permissions-proposal.md 가
실제 선언 제안을 담는다.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable

# --- Android API 레벨 상수(분기 기준) ---------------------------------------
API_TIRAMISU = 33  # Android 13: READ_MEDIA_* 세분화 + POST_NOTIFICATIONS 런타임화
API_UPSIDE_DOWN_CAKE = 34  # Android 14: READ_MEDIA_VISUAL_USER_SELECTED(부분 선택)


class GuestPermission(str, Enum):
    """게스트 Linux 앱이 요구할 수 있는 권한 부류."""

    STORAGE_READ = "storage-read"
    STORAGE_WRITE = "storage-write"
    CAMERA = "camera"
    MICROPHONE = "microphone"
    LOCATION = "location"
    NETWORK = "network"
    NOTIFICATIONS = "notifications"


# 별칭(게스트 메타데이터에서 흔히 쓰는 표기) → 정규 부류.
_ALIASES = {
    "storage": GuestPermission.STORAGE_READ,
    "read-storage": GuestPermission.STORAGE_READ,
    "write-storage": GuestPermission.STORAGE_WRITE,
    "mic": GuestPermission.MICROPHONE,
    "record-audio": GuestPermission.MICROPHONE,
    "gps": GuestPermission.LOCATION,
    "geolocation": GuestPermission.LOCATION,
    "internet": GuestPermission.NETWORK,
    "net": GuestPermission.NETWORK,
    "notify": GuestPermission.NOTIFICATIONS,
    "notification": GuestPermission.NOTIFICATIONS,
    "webcam": GuestPermission.CAMERA,
}


class AndroidGrantKind(str, Enum):
    """Android 가 권한을 부여하는 방식."""

    INSTALL_TIME = "install-time"  # manifest 선언만으로 부여(normal). 프롬프트 없음.
    RUNTIME = "runtime"  # 첫 사용 시 시스템 프롬프트(dangerous).
    SAF = "saf"  # Storage Access Framework — 권한 대신 사용자 선택 인텐트.


@dataclass(frozen=True)
class AndroidPermission:
    """매핑 결과 한 항목."""

    android_name: str  # 빈 문자열이면 SAF(=manifest 권한 불필요).
    kind: AndroidGrantKind
    guest: GuestPermission
    rationale: str

    @property
    def needs_manifest_entry(self) -> bool:
        """AndroidManifest 에 <uses-permission> 선언이 필요한가."""
        return self.kind in (AndroidGrantKind.INSTALL_TIME, AndroidGrantKind.RUNTIME)

    @property
    def needs_runtime_prompt(self) -> bool:
        """첫 사용 시 런타임 권한 프롬프트가 필요한가."""
        return self.kind == AndroidGrantKind.RUNTIME


class UnknownGuestPermission(ValueError):
    """알 수 없는 게스트 권한 문자열."""


def normalize_guest_permission(token: str) -> GuestPermission:
    """게스트 권한 문자열을 정규 :class:`GuestPermission` 으로. 모르면 거부."""
    if not isinstance(token, str):
        raise UnknownGuestPermission(f"권한 토큰은 문자열이어야 함: {token!r}")
    key = token.strip().lower()
    if not key:
        raise UnknownGuestPermission("빈 권한 토큰")
    try:
        return GuestPermission(key)
    except ValueError:
        pass
    if key in _ALIASES:
        return _ALIASES[key]
    raise UnknownGuestPermission(f"알 수 없는 게스트 권한: {token!r}")


@dataclass(frozen=True)
class PermissionPlan:
    """``required_permissions`` 의 결과.

    Attributes
    ----------
    permissions:
        매핑된 :class:`AndroidPermission` 목록(부류 정규화 + 중복 제거 + 정렬).
    storage_via_saf:
        storage-read/write 를 SAF 로 처리했는가(= 미디어 권한 불요).
    """

    permissions: tuple[AndroidPermission, ...]
    storage_via_saf: bool

    @property
    def manifest_permissions(self) -> tuple[str, ...]:
        """manifest 에 선언할 Android 권한 이름(중복 제거, 정렬)."""
        names = {p.android_name for p in self.permissions if p.needs_manifest_entry and p.android_name}
        return tuple(sorted(names))

    @property
    def install_time_permissions(self) -> tuple[str, ...]:
        names = {
            p.android_name
            for p in self.permissions
            if p.kind == AndroidGrantKind.INSTALL_TIME and p.android_name
        }
        return tuple(sorted(names))

    @property
    def runtime_permissions(self) -> tuple[str, ...]:
        """첫 사용 시 requestPermissions() 로 요청할 권한 이름."""
        names = {p.android_name for p in self.permissions if p.needs_runtime_prompt and p.android_name}
        return tuple(sorted(names))

    @property
    def uses_saf(self) -> bool:
        return any(p.kind == AndroidGrantKind.SAF for p in self.permissions)


def _location_permissions(guest: GuestPermission) -> list[AndroidPermission]:
    # 위치는 거친(coarse) + 정밀(fine) 둘 다 선언하고 사용자가 정밀 허용을
    # 선택할 수 있게 한다(Android 12+ 부분 정밀 모델). 런타임 프롬프트.
    return [
        AndroidPermission(
            "android.permission.ACCESS_COARSE_LOCATION",
            AndroidGrantKind.RUNTIME,
            guest,
            "게스트 앱의 대략 위치 요청; 사용자가 정밀 거부 시 거친 위치로 graceful degrade.",
        ),
        AndroidPermission(
            "android.permission.ACCESS_FINE_LOCATION",
            AndroidGrantKind.RUNTIME,
            guest,
            "게스트 앱의 정밀 위치 요청(Android 12+ 사용자가 정밀/거친 선택).",
        ),
    ]


def _storage_read_media_permissions(api_level: int, guest: GuestPermission) -> list[AndroidPermission]:
    """SAF 로 못 덮는 잔여 storage-read 케이스의 미디어 권한 fallback."""
    if api_level >= API_TIRAMISU:
        return [
            AndroidPermission(
                "android.permission.READ_MEDIA_IMAGES",
                AndroidGrantKind.RUNTIME,
                guest,
                "API33+ 세분화 미디어 읽기(이미지). SAF 로 못 덮는 라이브러리-열람형 게스트 앱 전용.",
            ),
            AndroidPermission(
                "android.permission.READ_MEDIA_VIDEO",
                AndroidGrantKind.RUNTIME,
                guest,
                "API33+ 세분화 미디어 읽기(영상).",
            ),
            AndroidPermission(
                "android.permission.READ_MEDIA_AUDIO",
                AndroidGrantKind.RUNTIME,
                guest,
                "API33+ 세분화 미디어 읽기(오디오).",
            ),
        ]
    return [
        AndroidPermission(
            "android.permission.READ_EXTERNAL_STORAGE",
            AndroidGrantKind.RUNTIME,
            guest,
            "API32 이하 단일 외부저장 읽기 권한. SAF 로 못 덮는 잔여 케이스 전용.",
        ),
    ]


def _map_one(
    guest: GuestPermission,
    api_level: int,
    storage_via_saf: bool,
) -> list[AndroidPermission]:
    if guest is GuestPermission.NETWORK:
        return [
            AndroidPermission(
                "android.permission.INTERNET",
                AndroidGrantKind.INSTALL_TIME,
                guest,
                "apt/카탈로그/게스트 네트워크. normal 권한 → manifest 선언만으로 부여, 프롬프트 없음.",
            ),
        ]
    if guest in (GuestPermission.STORAGE_READ, GuestPermission.STORAGE_WRITE):
        if storage_via_saf:
            return [
                AndroidPermission(
                    "",  # SAF 는 manifest 권한이 아니라 인텐트(ACTION_OPEN_DOCUMENT 등).
                    AndroidGrantKind.SAF,
                    guest,
                    "저장소 접근을 SAF 로 처리 → 미디어/광범위 저장소 권한 불요(T5). "
                    "사용자가 문서/폴더를 명시 선택, ALR 가 그 URI 를 게스트 path 로 중재.",
                ),
            ]
        if guest is GuestPermission.STORAGE_WRITE:
            # 쓰기까지 SAF 없이 가는 케이스: API29+ 는 scoped storage 라
            # WRITE_EXTERNAL_STORAGE 가 사실상 무력. SAF(쓰기 가능 URI)로 강제.
            # 따라서 SAF 없는 storage-write 는 항상 SAF 권고로 강제 강등한다.
            return [
                AndroidPermission(
                    "",
                    AndroidGrantKind.SAF,
                    guest,
                    "scoped storage(API29+)에서 외부 쓰기 권한은 무력 → SAF 쓰기 URI 필수. "
                    "storage-write 는 SAF 미사용을 허용하지 않음.",
                ),
            ]
        return _storage_read_media_permissions(api_level, guest)
    if guest is GuestPermission.CAMERA:
        return [
            AndroidPermission(
                "android.permission.CAMERA",
                AndroidGrantKind.RUNTIME,
                guest,
                "게스트 앱 카메라 캡처. dangerous 런타임 권한.",
            ),
        ]
    if guest is GuestPermission.MICROPHONE:
        return [
            AndroidPermission(
                "android.permission.RECORD_AUDIO",
                AndroidGrantKind.RUNTIME,
                guest,
                "게스트 앱 마이크 캡처. dangerous 런타임 권한.",
            ),
        ]
    if guest is GuestPermission.LOCATION:
        return _location_permissions(guest)
    if guest is GuestPermission.NOTIFICATIONS:
        if api_level >= API_TIRAMISU:
            return [
                AndroidPermission(
                    "android.permission.POST_NOTIFICATIONS",
                    AndroidGrantKind.RUNTIME,
                    guest,
                    "API33+ 알림 게시는 런타임 권한. 첫 사용 시 프롬프트.",
                ),
            ]
        # API32 이하: 알림에 권한 불필요.
        return []
    raise UnknownGuestPermission(f"매핑 미정의 게스트 권한: {guest!r}")


def required_permissions(
    guest_permissions: Iterable[str | GuestPermission],
    *,
    api_level: int,
    storage_via_saf: bool = True,
) -> PermissionPlan:
    """게스트 권한셋 → Android 권한 계획.

    Parameters
    ----------
    guest_permissions:
        게스트 권한 부류 문자열(또는 :class:`GuestPermission`) iterable.
        알 수 없는 값이 있으면 :class:`UnknownGuestPermission`.
    api_level:
        대상 기기 ``Build.VERSION.SDK_INT``. 미디어 세분화/알림 런타임화 분기.
    storage_via_saf:
        storage 접근을 SAF(T5)로 처리하는가. 기본 True(권장). True 면 storage 는
        미디어/저장소 권한 없이 SAF 로만 처리한다.

    Returns
    -------
    PermissionPlan
        정규화·중복제거·정렬된 매핑 결과.
    """
    if not isinstance(api_level, int) or api_level < 1:
        raise ValueError(f"api_level 은 1 이상의 정수여야 함: {api_level!r}")

    normalized: list[GuestPermission] = []
    seen: set[GuestPermission] = set()
    for token in guest_permissions:
        guest = token if isinstance(token, GuestPermission) else normalize_guest_permission(token)
        if guest not in seen:
            seen.add(guest)
            normalized.append(guest)

    mapped: list[AndroidPermission] = []
    storage_used_saf = False
    for guest in normalized:
        entries = _map_one(guest, api_level, storage_via_saf)
        for e in entries:
            if e.kind == AndroidGrantKind.SAF:
                storage_used_saf = True
        mapped.extend(entries)

    # 같은 android_name+kind 중복 제거(예: storage-read + storage-write 가 둘 다
    # SAF 로 가는 경우). guest 출처는 첫 등장 것을 보존.
    deduped: list[AndroidPermission] = []
    keyseen: set[tuple[str, str]] = set()
    for p in mapped:
        k = (p.android_name, p.kind.value)
        if k not in keyseen:
            keyseen.add(k)
            deduped.append(p)

    deduped.sort(key=lambda p: (p.kind.value, p.android_name, p.guest.value))
    return PermissionPlan(permissions=tuple(deduped), storage_via_saf=storage_used_saf)


__all__ = [
    "API_TIRAMISU",
    "API_UPSIDE_DOWN_CAKE",
    "GuestPermission",
    "AndroidGrantKind",
    "AndroidPermission",
    "PermissionPlan",
    "UnknownGuestPermission",
    "normalize_guest_permission",
    "required_permissions",
]
