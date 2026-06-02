"""추천 카탈로그 — Ubuntu noble GUI 큐레이션 셋 (device 증명 + 경량 인기).

C 트랙(추천 카탈로그). ``apt_catalog`` 가 *임의의* Packages 인덱스를 카탈로그로
바꾸는 일반 엔진이라면, 이 모듈은 **사람이 고른 추천 목록**을 정의한다 — ALR 이
디바이스에서 실제로 증명한 GUI 앱(GIMP/foot/netsurf-gtk/gtk3-demo/sdl2/qt6)과,
같은 Ubuntu noble arm64 저장소에서 받을 수 있는 경량·인기 GUI 앱들이다.

각 추천 항목은 **apt 패키지명**으로 식별되므로, 실제 카탈로그 빌드 단계에서
``install_plan.build_install_plan`` 으로 같은 인덱스를 풀면 그대로 closure 가
해석된다(이 모듈은 패키지명+표시 메타데이터만; closure/크기는 install_plan 이
인덱스에서 채움). 즉 추천셋은 "어떤 패키지를 추천하나 + 어떻게 보여주나"이고,
"무엇을 받아 설치하나"는 install_plan 이 인덱스로 결정한다 — 중복 없음.

**빌드 미통합: 순수 host 파이썬. Compose/lifecycle 의존성은 통합 세션.** 디바이스
카탈로그 화면은 ``apt_catalog.manifest_from_package`` 가 만든 AppManifest 를 읽고,
이 모듈의 ``category``/``summary`` 메타는 인덱스 Description 이 비거나 큐레이션이
더 정확할 때 그 위에 덮어쓴다(아래 ``to_app_manifest`` override 참조).

검증 가능성(host): 추천셋 스키마(카테고리 enum 정합·중복 id 없음·apt 패키지명
문법)는 alr_manifest 의 닫힌 어휘로 정적 검증한다 — tests/test_curated_catalog.py.
install_plan 으로의 해석은 오프라인 픽스처 인덱스로 확인한다.
"""

from __future__ import annotations

from dataclasses import dataclass

from tools.alr_manifest import (
    CATEGORIES,
    CATEGORY_DEVELOPMENT,
    CATEGORY_EDUCATION,
    CATEGORY_GRAPHICS,
    CATEGORY_INTERNET,
    CATEGORY_MULTIMEDIA,
    CATEGORY_OFFICE,
    CATEGORY_TERMINAL,
    CATEGORY_UTILITY,
    _APT_PKG_NAME,
    AppEntry,
    AppManifest,
    Catalog,
)
from tools.apt_catalog import PackageEntry, app_id_for_package


# --------------------------------------------------------------------------- #
# CuratedApp — 한 추천 항목
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class CuratedApp:
    """추천 카탈로그의 한 항목 — apt 패키지명 + 표시 메타 + device 증명 여부.

    package:    Ubuntu noble arm64 저장소의 apt 패키지명(install_plan 이 closure 를
                풀 키). alr_manifest._APT_PKG_NAME 문법을 따라야 한다.
    category:   alr_manifest 의 닫힌 CATEGORIES 중 하나.
    summary:    카탈로그 한 줄 요약(인덱스 Description 비었을 때/큐레이션 우선).
    est_install_mb: 설치 후 예상 크기(MB) — UI 미리보기용 *추정*. 정확한 값은
                install_plan 이 실제 인덱스로 계산(여기 값은 device/문서 근거의 힌트).
    device_proven: ALR 이 디바이스에서 실제 렌더/실행을 증명한 앱이면 True
                (GIMP/foot/netsurf-gtk/gtk3-demo/sdl2/qt6). 추천 정렬 상단 가중.
    binary:     주 실행 바이너리 경로(entry exec target). 없으면 /usr/bin/<package>.
    note:       증명/주의 메모(예: "Phase 6 GIMP 터치 사용 증명").
    """

    package: str
    category: str
    summary: str
    est_install_mb: int
    device_proven: bool = False
    binary: str = ""
    note: str = ""

    def __post_init__(self) -> None:
        if not _APT_PKG_NAME.fullmatch(self.package):
            raise ValueError(f"invalid apt package name: {self.package!r}")
        if self.category not in CATEGORIES:
            raise ValueError(
                f"category must be one of {sorted(CATEGORIES)}, got {self.category!r}"
            )
        if not self.summary.strip():
            raise ValueError(f"summary must be non-empty for {self.package!r}")
        if self.est_install_mb < 0:
            raise ValueError("est_install_mb must be >= 0")
        if self.binary and not self.binary.startswith("/"):
            raise ValueError(f"binary must be an absolute path: {self.binary!r}")

    @property
    def app_id(self) -> str:
        """apt_catalog 와 동일 규칙의 합성 reverse-DNS app_id(중복 검사 키)."""
        return app_id_for_package(self.package)

    @property
    def entry_target(self) -> str:
        return self.binary or f"/usr/bin/{self.package}"

    @property
    def est_install_bytes(self) -> int:
        return self.est_install_mb * 1024 * 1024


# --------------------------------------------------------------------------- #
# 추천셋 — device 증명 그룹
# --------------------------------------------------------------------------- #
#
# ALR 이 실제 디바이스(Mali-G615 / Android16, 1200×1920@90Hz)에서 렌더/실행을
# 증명한 앱들. MEMORY: Phase 6 (v111) GIMP 터치 사용 증명, glmark2-es2 Mali 렌더,
# netsurf/qt6/sdl2/foot/gtk3-demo GUI 보편성 경로. 추천 상단 가중.
DEVICE_PROVEN: tuple[CuratedApp, ...] = (
    CuratedApp(
        package="gimp",
        category=CATEGORY_GRAPHICS,
        summary="전문 래스터 이미지 편집기(GNU Image Manipulation Program)",
        est_install_mb=240,
        device_proven=True,
        binary="/usr/bin/gimp",
        note="Phase 6 (v111): 디바이스에서 터치로 New>1920x1080 캔버스 브러시 사용 증명",
    ),
    CuratedApp(
        package="foot",
        category=CATEGORY_TERMINAL,
        summary="빠르고 가벼운 Wayland 터미널 에뮬레이터",
        est_install_mb=2,
        device_proven=True,
        binary="/usr/bin/foot",
        note="경량 Wayland 터미널 — 컴포지터 입력/렌더 경로 증명",
    ),
    CuratedApp(
        package="netsurf-gtk",
        category=CATEGORY_INTERNET,
        summary="가벼운 GTK 웹 브라우저(NetSurf)",
        est_install_mb=195,
        device_proven=True,
        binary="/usr/bin/netsurf-gtk",
        note="GUI 보편성 경로: GTK 렌더 디바이스 증명(software wl_shm)",
    ),
    CuratedApp(
        package="gtk-3-examples",
        category=CATEGORY_DEVELOPMENT,
        summary="GTK3 위젯 데모(gtk3-demo) — 툴킷 검증용",
        est_install_mb=8,
        device_proven=True,
        binary="/usr/bin/gtk3-demo",
        note="gtk3-demo 위젯/다이얼로그 렌더 증명(GTK 바인딩 검증)",
    ),
    CuratedApp(
        package="libsdl2-2.0-0",
        category=CATEGORY_DEVELOPMENT,
        summary="SDL2 런타임 — 게임/멀티미디어 앱 기반 라이브러리",
        est_install_mb=31,
        device_proven=True,
        binary="/usr/bin/sdl2-config",
        note="SDL2 오버레이(~31MB) 빌드/렌더 증명 — 게임 토대",
    ),
    CuratedApp(
        package="qt6-wayland",
        category=CATEGORY_DEVELOPMENT,
        summary="Qt6 Wayland 플랫폼 플러그인 — Qt 앱 렌더 기반",
        est_install_mb=60,
        device_proven=True,
        binary="/usr/lib/aarch64-linux-gnu/qt6/bin/qmake6",
        note="qt6-wayland 디바이스 probe(no-decoration env) 증명",
    ),
)


# --------------------------------------------------------------------------- #
# 추천셋 — 경량·인기 GUI (같은 noble 저장소, install_plan 해석 가능)
# --------------------------------------------------------------------------- #

POPULAR_LIGHTWEIGHT: tuple[CuratedApp, ...] = (
    CuratedApp(
        package="inkscape",
        category=CATEGORY_GRAPHICS,
        summary="벡터 그래픽 편집기(SVG)",
        est_install_mb=420,
        binary="/usr/bin/inkscape",
        note="GTK 기반 — netsurf/gimp 와 같은 GTK 렌더 경로",
    ),
    CuratedApp(
        package="galculator",
        category=CATEGORY_UTILITY,
        summary="GTK 계산기(과학/공학 모드)",
        est_install_mb=3,
        binary="/usr/bin/galculator",
        note="경량 GTK 유틸 — 빠른 첫 추천 후보",
    ),
    CuratedApp(
        package="mpv",
        category=CATEGORY_MULTIMEDIA,
        summary="가벼운 동영상/오디오 플레이어",
        est_install_mb=45,
        binary="/usr/bin/mpv",
        note="GPU 가속 비디오 출력 후보(VK/GLES 경로)",
    ),
    CuratedApp(
        package="audacity",
        category=CATEGORY_MULTIMEDIA,
        summary="다중 트랙 오디오 편집기",
        est_install_mb=120,
        binary="/usr/bin/audacity",
        note="wxWidgets GUI — 오디오 편집 인기 앱",
    ),
    CuratedApp(
        package="xterm",
        category=CATEGORY_TERMINAL,
        summary="고전적 X 터미널 에뮬레이터",
        est_install_mb=5,
        binary="/usr/bin/xterm",
        note="Xwayland(rootful) 경로 검증용 X11 터미널",
    ),
    CuratedApp(
        package="nano",
        category=CATEGORY_TERMINAL,
        summary="간단한 콘솔 텍스트 편집기",
        est_install_mb=1,
        binary="/usr/bin/nano",
        note="터미널 내 편집기 — foot/xterm 안에서 동작",
    ),
    CuratedApp(
        package="abiword",
        category=CATEGORY_OFFICE,
        summary="가벼운 워드 프로세서",
        est_install_mb=40,
        binary="/usr/bin/abiword",
        note="GTK 워드프로세서 — LibreOffice 대비 경량",
    ),
    CuratedApp(
        package="gnumeric",
        category=CATEGORY_OFFICE,
        summary="가벼운 스프레드시트",
        est_install_mb=70,
        binary="/usr/bin/gnumeric",
        note="GTK 스프레드시트 — 경량 오피스 셋",
    ),
    CuratedApp(
        package="geany",
        category=CATEGORY_DEVELOPMENT,
        summary="가벼운 IDE/프로그래머 에디터(GTK)",
        est_install_mb=15,
        binary="/usr/bin/geany",
        note="GTK 코드 에디터 — 개발 카테고리 경량 후보",
    ),
    CuratedApp(
        package="gnome-mahjongg",
        category=CATEGORY_EDUCATION,
        summary="마작 솔리테어 퍼즐 게임",
        est_install_mb=8,
        binary="/usr/bin/gnome-mahjongg",
        note="GTK 게임 — 가벼운 캐주얼(games 섹션은 education 그룹에 둠)",
    ),
)


# 전체 추천셋(증명 우선, 그 다음 인기 경량). 정렬은 to_catalog 가 안정 처리.
CURATED: tuple[CuratedApp, ...] = DEVICE_PROVEN + POPULAR_LIGHTWEIGHT


# --------------------------------------------------------------------------- #
# 조회 / 변환
# --------------------------------------------------------------------------- #

def curated_packages() -> list[str]:
    """추천셋의 apt 패키지명 목록(install_plan/closure 빌드 입력)."""
    return [c.package for c in CURATED]


def by_category() -> dict[str, list[CuratedApp]]:
    """카테고리별 추천 그룹(런처 섹션 표시용, device 증명 우선 정렬)."""
    groups: dict[str, list[CuratedApp]] = {}
    for app in CURATED:
        groups.setdefault(app.category, []).append(app)
    for apps in groups.values():
        apps.sort(key=lambda a: (not a.device_proven, a.package))
    return groups


def to_app_manifest(
    app: CuratedApp,
    *,
    entry: PackageEntry | None = None,
    stage_tar_ref: str | None = None,
) -> AppManifest:
    """한 CuratedApp → alr_manifest.AppManifest(런처/카탈로그가 읽는 형태).

    인덱스 ``entry`` 가 주어지면 install_size 는 그 Installed-Size(정확)를 쓰고,
    summary 는 큐레이션이 비었을 때만 인덱스 Description 으로 채운다(큐레이션이
    더 정확/한국어이므로 우선). entry 가 없으면 est_install_mb 추정을 쓴다.

    apt_catalog.manifest_from_package 와 같은 app_id/카테고리 규칙을 따르되, 이
    함수는 *큐레이션 메타*(요약/카테고리/실행 경로)를 권위로 삼는다는 점이 다르다.
    """
    from tools.alr_manifest import RootfsDep  # 지역 import(선택적 dep)

    summary = app.summary.strip()
    install_bytes = app.est_install_bytes
    description = ""
    if entry is not None:
        if entry.installed_size_bytes:
            install_bytes = entry.installed_size_bytes
        description = entry.long_description

    deps: tuple[RootfsDep, ...] = ()
    if stage_tar_ref is not None:
        deps = (
            RootfsDep(
                kind="stage-tar",
                ref=stage_tar_ref,
                install_size_bytes=install_bytes,
            ),
        )

    return AppManifest(
        app_id=app.app_id,
        name=app.package,
        summary=summary,
        entry=AppEntry(kind="exec", target=app.entry_target, argv=()),
        category=app.category,
        description=description,
        rootfs_deps=deps,
        install_size_bytes=install_bytes,
    )


def to_catalog(
    *,
    index: dict[str, PackageEntry] | None = None,
) -> Catalog:
    """추천셋 전체 → alr_manifest.Catalog(중복 app_id 검사 포함).

    ``index`` 가 주어지면(실 파이프라인) 각 추천 패키지의 인덱스 엔트리로 크기/
    설명을 보강한다. 인덱스에 없는 추천은 추정치로 그대로 둔다(카탈로그엔 남김 —
    빌드 단계가 mirror 에서 확인). device 증명 → 패키지명 순으로 안정 정렬.
    """
    ordered = sorted(CURATED, key=lambda a: (not a.device_proven, a.package))
    apps: list[AppManifest] = []
    seen: set[str] = set()
    for c in ordered:
        if c.app_id in seen:
            continue  # 합성 app_id 충돌 방어(결정적 — 첫 항목 우선)
        seen.add(c.app_id)
        entry = index.get(c.package) if index else None
        apps.append(to_app_manifest(c, entry=entry))
    return Catalog(apps=tuple(apps))


def device_proven_packages() -> list[str]:
    """디바이스 증명된 추천 패키지명(추천 상단/배지용)."""
    return [c.package for c in CURATED if c.device_proven]
