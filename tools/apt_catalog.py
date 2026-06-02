"""apt 인덱스 → ALR 인앱 카탈로그 (v1, 목록 생성기).

찬우 확정 결정 (3): 카탈로그 = apt 저장소 인덱스 기반. **v1 = 인덱스로 목록을
만들고**, 선택한 패키지의 의존성 closure를 §5-E stage-tar로 풀어 설치(dpkg
fork-exec 우회, exec re-entry 벽 ADR-003 회피). v2 = exec re-entry가 풀린 뒤
인-게스트 apt/dpkg.

이 모듈은 v1의 **목록(카탈로그)** 절반을 담당한다: Debian/Ubuntu ``Packages``
인덱스(RFC822-식 stanza)를 파싱해 사람이 고를 수 있는 패키지 엔트리로 만들고,
그 엔트리를 ``tools.alr_manifest.AppManifest``(런처/카탈로그 UI가 그대로 읽는
스키마)로 자동 변환한다. closure 해결 + stage-tar 설치 지시는 자매 모듈
``tools.install_plan``이 담당한다.

중복 구현 금지 — 인덱스/Depends 파싱과 closure BFS는 이미
``tools.deb_closure`` (WS-4 overlay BUILD ENGINE)에 있고, 이 모듈은 그것을
**import 해서 재사용**한다:
  * ``deb_closure.parse_packages``  — Packages stanza → {name: 필드dict}
  * ``deb_closure.parse_depends``   — Depends 문자열 → 대안 그룹 리스트
  * ``deb_closure.build_provides_map`` — 가상 패키지 Provides 맵
  * ``deb_closure._strip_dep_name`` — 의존 토큰 → 바 패키지명 (private 재사용)

이 모듈이 **추가로** 하는 일은 deb_closure가 신경 안 쓰는 *제품 메타데이터*다:
패키지 엔트리(Description/Section/Size/Installed-Size/Architecture)를 구조화하고,
Debian ``Section`` → alr_manifest 카탈로그 **카테고리**로 매핑하고, 한 패키지를
런처에 보일 한 줄(요약/설명/크기)로 정규화한다.

alr_manifest.py 는 **수정하지 않는다** — import 만 한다. AppManifest 스키마가
요구하는 필드(reverse-DNS app_id, 절대경로 entry, 닫힌 category 집합)를 이 모듈이
인덱스에서 *합성*해 채운다.

host 검증: tests/test_apt_catalog.py (오프라인, 픽스처 stanza).
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass, field

from tools.alr_manifest import (
    CATEGORIES,
    CATEGORY_DEVELOPMENT,
    CATEGORY_EDUCATION,
    CATEGORY_GAMES,
    CATEGORY_GRAPHICS,
    CATEGORY_INTERNET,
    CATEGORY_MULTIMEDIA,
    CATEGORY_OFFICE,
    CATEGORY_SYSTEM,
    CATEGORY_TERMINAL,
    CATEGORY_UTILITY,
    AppEntry,
    AppManifest,
    Catalog,
    RootfsDep,
)
from tools.deb_closure import (
    _strip_dep_name,
    build_provides_map,
    parse_depends,
    parse_packages,
)

# --------------------------------------------------------------------------- #
# Section → 카탈로그 카테고리 매핑
# --------------------------------------------------------------------------- #
#
# Debian 정책의 ``Section`` 은 ``universe/graphics`` 처럼 component 접두가 붙기도
# 한다 → 마지막 '/' 뒤만 본다. 알려진 섹션은 alr_manifest 의 닫힌 CATEGORIES 로
# 사상하고, 모르는 섹션은 CATEGORY_UTILITY 로 폴백(닫힌 enum 위반 방지). 한
# Section 이 여러 alr 카테고리에 걸칠 수 있으나 카탈로그 그룹핑은 1:1 만 필요하므로
# 대표 카테고리 하나로 접는다.
_SECTION_TO_CATEGORY: dict[str, str] = {
    # graphics
    "graphics": CATEGORY_GRAPHICS,
    "x11": CATEGORY_GRAPHICS,
    # development / libraries / build
    "devel": CATEGORY_DEVELOPMENT,
    "libdevel": CATEGORY_DEVELOPMENT,
    "java": CATEGORY_DEVELOPMENT,
    "python": CATEGORY_DEVELOPMENT,
    "ruby": CATEGORY_DEVELOPMENT,
    "rust": CATEGORY_DEVELOPMENT,
    "ocaml": CATEGORY_DEVELOPMENT,
    "haskell": CATEGORY_DEVELOPMENT,
    "vcs": CATEGORY_DEVELOPMENT,
    # office / docs / editors
    "editors": CATEGORY_OFFICE,
    "text": CATEGORY_OFFICE,
    "tex": CATEGORY_OFFICE,
    "office": CATEGORY_OFFICE,
    "database": CATEGORY_OFFICE,
    # internet / network / mail / web
    "web": CATEGORY_INTERNET,
    "net": CATEGORY_INTERNET,
    "mail": CATEGORY_INTERNET,
    "news": CATEGORY_INTERNET,
    "comm": CATEGORY_INTERNET,
    "httpd": CATEGORY_INTERNET,
    # multimedia / sound / video
    "sound": CATEGORY_MULTIMEDIA,
    "video": CATEGORY_MULTIMEDIA,
    "multimedia": CATEGORY_MULTIMEDIA,
    # games
    "games": CATEGORY_GAMES,
    # education / science
    "education": CATEGORY_EDUCATION,
    "science": CATEGORY_EDUCATION,
    "math": CATEGORY_EDUCATION,
    "electronics": CATEGORY_EDUCATION,
    # system / admin / kernel
    "admin": CATEGORY_SYSTEM,
    "kernel": CATEGORY_SYSTEM,
    "fonts": CATEGORY_SYSTEM,
    "libs": CATEGORY_SYSTEM,
    "metapackages": CATEGORY_SYSTEM,
    "misc": CATEGORY_SYSTEM,
    # terminal / shells
    "shells": CATEGORY_TERMINAL,
    "utils": CATEGORY_UTILITY,
}


def section_to_category(section: str | None) -> str:
    """Debian ``Section`` 값을 alr_manifest 카탈로그 카테고리로 사상.

    component 접두(``universe/graphics``)와 대소문자를 정규화하고, 모르는 섹션은
    CATEGORY_UTILITY 로 폴백한다(닫힌 enum 절대 위반 안 함).

    >>> section_to_category("universe/graphics")
    'graphics'
    >>> section_to_category("unknown-thing")
    'utility'
    >>> section_to_category(None)
    'utility'
    """
    if not section:
        return CATEGORY_UTILITY
    tail = section.rsplit("/", 1)[-1].strip().lower()
    cat = _SECTION_TO_CATEGORY.get(tail, CATEGORY_UTILITY)
    # 방어: 매핑이 닫힌 집합 밖이면 안 됨(정적으로 보장되지만 명시적 가드).
    return cat if cat in CATEGORIES else CATEGORY_UTILITY


# --------------------------------------------------------------------------- #
# 패키지 엔트리 (인덱스 stanza의 구조화 뷰)
# --------------------------------------------------------------------------- #

# Debian 패키지명 grammar (policy 5.6.1): 소문자/숫자로 시작, 이후
# 소문자/숫자/'+'/'-'/'.'. app_id 합성·검증에 쓴다.
_PKG_NAME = re.compile(r"^[a-z0-9][a-z0-9+.\-]+$")


@dataclass(frozen=True)
class DependsAtom:
    """단일 의존 토큰 — 패키지명 + (있으면) 버전 제약 + arch 한정.

    parse_depends 는 바 패키지명만 돌려주지만, 카탈로그/플랜은 "(>= 2.34)" 같은
    제약과 "[arm64]" arch 한정을 *보존*해 보여줄 수 있어야 하므로 한 단계 더
    파싱한다. closure 해결은 ``name`` 만 쓰고, 제약은 표시/감사용.
    """

    name: str
    constraint: str = ""  # 예 ">= 2.34" (괄호 제거). 없으면 "".
    arch: str = ""        # 예 "arm64" ([..] 한정의 첫 토큰). 없으면 "".

    @property
    def applies_to_arch(self) -> bool:
        """arch 한정이 없거나 arm64 를 포함하면 True (ALR 타깃은 arm64).

        ``[!amd64]`` 같은 negation 한정은 '아님' 목록이므로 arm64 가 그 목록에
        없으면 적용된다. 단순 ``[arm64 amd64]`` 는 arm64 가 목록에 있으면 적용.
        """
        if not self.arch:
            return True
        tokens = self.arch.split()
        negated = any(t.startswith("!") for t in tokens)
        bare = {t.lstrip("!") for t in tokens}
        if negated:
            return "arm64" not in bare
        return "arm64" in bare or "any" in bare or "linux-any" in bare


def parse_depends_detailed(field_value: str) -> list[list[DependsAtom]]:
    """Depends/Pre-Depends → 제약·arch 까지 보존한 대안 그룹 리스트.

    ``deb_closure.parse_depends`` 가 [[name,...],...] 만 주는 데 비해, 여기선
    각 토큰의 버전 제약과 arch 한정을 보존한 DependsAtom 으로 만든다. closure
    해결에는 deb_closure.parse_depends 를 그대로 쓰고, 이 detailed 형태는
    카탈로그가 "이 패키지는 libc6 (>= 2.34) 를 요구" 같은 표시를 할 때 쓴다.

    >>> g = parse_depends_detailed("libc6 (>= 2.34), libfoo | libbar [arm64]")
    >>> g[0][0].name, g[0][0].constraint
    ('libc6', '>= 2.34')
    >>> g[1][1].name, g[1][1].arch
    ('libbar', 'arm64')
    """
    if not field_value:
        return []
    groups: list[list[DependsAtom]] = []
    for raw_group in field_value.replace("\n", " ").split(","):
        alts: list[DependsAtom] = []
        for raw_alt in raw_group.split("|"):
            atom = _parse_atom(raw_alt)
            if atom is not None:
                alts.append(atom)
        if alts:
            groups.append(alts)
    return groups


def _parse_atom(token: str) -> DependsAtom | None:
    """단일 의존 토큰 → DependsAtom (이름은 deb_closure._strip_dep_name 재사용)."""
    text = token.strip()
    if not text:
        return None
    name = _strip_dep_name(text)  # 재사용: 버전/arch/profile 제거 후 바 이름
    if not name:
        return None
    constraint = ""
    m = re.search(r"\(([^)]*)\)", text)
    if m:
        constraint = m.group(1).strip()
    arch = ""
    m = re.search(r"\[([^\]]*)\]", text)
    if m:
        arch = m.group(1).strip()
    return DependsAtom(name=name, constraint=constraint, arch=arch)


def _to_int(value: str | None) -> int:
    """인덱스 숫자 필드(Size/Installed-Size) → int, 결측/비숫자는 0."""
    if not value:
        return 0
    try:
        return int(value.strip())
    except (TypeError, ValueError):
        return 0


@dataclass(frozen=True)
class PackageEntry:
    """``Packages`` 인덱스 한 stanza의 구조화된 카탈로그 뷰.

    필드는 카탈로그/플랜이 실제로 쓰는 것만: Package/Version/Depends/Pre-Depends/
    Description/Section/Filename/Size(.deb 다운로드 바이트)/Installed-Size(설치 후
    KiB, 인덱스 규약)/Architecture. 원본 stanza dict 도 보관해 추가 필드가 필요해도
    재파싱 없이 닿는다.
    """

    name: str
    version: str
    section: str
    architecture: str
    description: str           # 짧은 요약(첫 줄)
    long_description: str      # 이어지는 본문(있으면)
    depends: str               # 원본 Depends 문자열(빈 문자열 가능)
    pre_depends: str
    filename: str              # pool/.../foo_1_arm64.deb
    download_size: int         # Size: (.deb 바이트)
    installed_size_kib: int    # Installed-Size: (KiB)
    raw: dict = field(default_factory=dict, repr=False, compare=False)

    @property
    def installed_size_bytes(self) -> int:
        """Installed-Size 는 인덱스 규약상 KiB → 바이트로 환산.

        AppManifest.install_size_bytes / RootfsDep.install_size_bytes 는 바이트
        단위이므로 여기서 *1024 한다.
        """
        return self.installed_size_kib * 1024

    @property
    def category(self) -> str:
        return section_to_category(self.section)

    @property
    def depends_groups(self) -> list[list[str]]:
        """closure 해결용 — deb_closure.parse_depends 그대로(이름만)."""
        combined = " , ".join(v for v in (self.pre_depends, self.depends) if v)
        return parse_depends(combined)

    @property
    def depends_detailed(self) -> list[list[DependsAtom]]:
        """표시용 — 제약/arch 보존."""
        combined = " , ".join(v for v in (self.pre_depends, self.depends) if v)
        return parse_depends_detailed(combined)


def _split_description(field_value: str) -> tuple[str, str]:
    """Debian Description: 첫 줄(요약) + 나머지(본문, 선행 ' ' 제거)로 분리."""
    if not field_value:
        return "", ""
    # parse_packages 가 이어지는 줄을 '\n' 으로 접어 둠.
    lines = field_value.split("\n")
    summary = lines[0].strip()
    rest = []
    for ln in lines[1:]:
        s = ln.strip()
        # Debian 본문의 ' .' 은 빈 줄을 의미.
        rest.append("" if s == "." else s)
    return summary, "\n".join(rest).strip()


def package_entry_from_stanza(fields: dict) -> PackageEntry | None:
    """파싱된 stanza dict → PackageEntry (Package 없으면 None)."""
    name = fields.get("Package")
    if not name:
        return None
    summary, body = _split_description(fields.get("Description", ""))
    return PackageEntry(
        name=name,
        version=fields.get("Version", ""),
        section=fields.get("Section", ""),
        architecture=fields.get("Architecture", ""),
        description=summary,
        long_description=body,
        depends=fields.get("Depends", ""),
        pre_depends=fields.get("Pre-Depends", ""),
        filename=fields.get("Filename", ""),
        download_size=_to_int(fields.get("Size")),
        installed_size_kib=_to_int(fields.get("Installed-Size")),
        raw=fields,
    )


def parse_package_index(text: str) -> dict[str, PackageEntry]:
    """``Packages`` 인덱스 텍스트 → {name: PackageEntry}.

    파싱은 deb_closure.parse_packages 재사용(중복 금지). 그 결과 stanza dict 를
    PackageEntry 로 구조화한다.
    """
    raw_index = parse_packages(text)
    entries: dict[str, PackageEntry] = {}
    for name, fields in raw_index.items():
        entry = package_entry_from_stanza(fields)
        if entry is not None:
            entries[name] = entry
    return entries


# --------------------------------------------------------------------------- #
# PackageEntry → AppManifest (카탈로그 엔트리 합성)
# --------------------------------------------------------------------------- #

# app_id 도메인 접두. apt 출처 패키지는 안정적 reverse-DNS 가 없으므로
# "org.debian.<pkg>" 형태로 합성한다(카탈로그 키/디렉토리명으로 안전).
_APP_ID_PREFIX = "org.debian"


def _sanitize_app_id_label(pkg: str) -> str:
    """패키지명을 alr_manifest._APP_ID 라벨 규칙에 맞게 정규화.

    AppManifest 의 app_id 라벨은 [a-zA-Z][a-zA-Z0-9_]*(-...)* 라 '+'/'.' 가 안
    된다(g++, gtk2.0 등). '+' → "plus", '.' → '-', 그 외 비허용 문자는 '-' 로,
    선행 비문자는 'p' 접두로 교정한다. 충돌 가능성은 카탈로그 dedup 이 잡는다.
    """
    s = pkg.replace("+", "plus").replace(".", "-")
    s = re.sub(r"[^A-Za-z0-9_-]", "-", s)
    s = re.sub(r"-{2,}", "-", s).strip("-")
    if not s:
        s = "pkg"
    if not s[0].isalpha():
        s = "p" + s
    return s


def app_id_for_package(pkg: str) -> str:
    """패키지명 → 합성 reverse-DNS app_id (예: ``g++`` → org.debian.gplusplus)."""
    return f"{_APP_ID_PREFIX}.{_sanitize_app_id_label(pkg)}"


def manifest_from_package(
    entry: PackageEntry,
    *,
    entry_path: str | None = None,
    argv: tuple[str, ...] = (),
    stage_tar_ref: str | None = None,
    min_runtime: str = "0.1",
) -> AppManifest:
    """한 PackageEntry → alr_manifest.AppManifest(카탈로그/런처가 읽는 형태).

    합성 규칙:
      * app_id   = org.debian.<sanitized pkg>
      * name     = 패키지명(그대로 — 사람이 아는 이름)
      * summary  = Description 첫 줄(없으면 "<pkg> (apt package)")
      * category = Section → alr 카테고리
      * entry    = exec /usr/bin/<pkg> (호출자가 entry_path 로 덮을 수 있음)
      * rootfs_deps = stage-tar 1개(install_plan 이 만든 closure tar) — 호출자가
        ``stage_tar_ref`` 를 주면 그 basename 으로, 안 주면 빈 deps(목록 전용).
      * install_size_bytes = Installed-Size(KiB→B)

    NOTE: entry_path 미지정 시 ``/usr/bin/<pkg>`` 를 가정한다 — 인덱스만으론 실제
    실행 경로를 모르므로(.desktop/closure 추출이 답함) 이는 *추정*이고, 실제
    카탈로그 구축 파이프라인은 desktop_entry 파서나 install_plan 의 추출 결과로
    이 경로를 교정해야 한다(catalog-apt-v1.md §entry-path-honesty).
    """
    target = entry_path or f"/usr/bin/{entry.name}"
    summary = entry.description or f"{entry.name} (apt package)"
    deps: tuple[RootfsDep, ...] = ()
    if stage_tar_ref is not None:
        deps = (
            RootfsDep(
                kind="stage-tar",
                ref=stage_tar_ref,
                install_size_bytes=entry.installed_size_bytes,
            ),
        )
    return AppManifest(
        app_id=app_id_for_package(entry.name),
        name=entry.name,
        summary=summary,
        entry=AppEntry(kind="exec", target=target, argv=tuple(argv)),
        category=entry.category,
        description=entry.long_description,
        rootfs_deps=deps,
        min_runtime=min_runtime,
        install_size_bytes=entry.installed_size_bytes,
    )


def build_catalog_from_index(
    text: str,
    *,
    include: list[str] | None = None,
    sections: list[str] | None = None,
) -> Catalog:
    """``Packages`` 인덱스 → alr_manifest.Catalog(자동 생성 목록).

    Parameters
    ----------
    include:
        주어지면 이 패키지명 집합만 카탈로그에 넣는다(큐레이션). 인덱스에 없는
        이름은 조용히 건너뛴다.
    sections:
        주어지면 이 Debian Section(또는 그 tail)에 속한 패키지만 넣는다(카탈로그가
        라이브러리/디버그 패키지로 폭발하지 않게). include 와 AND.

    인덱스 전체를 매니페스트로 바꾸는 건 비현실적(수만 개·대부분 라이브러리)이라,
    실 파이프라인은 ``include`` 큐레이션 목록을 준다. 둘 다 None 이면 인덱스의 모든
    패키지를 만든다(테스트/소형 인덱스용).
    """
    entries = parse_package_index(text)
    include_set = set(include) if include is not None else None
    section_set = {s.rsplit("/", 1)[-1].lower() for s in sections} if sections else None

    apps: list[AppManifest] = []
    seen_ids: set[str] = set()
    for name in sorted(entries):  # 결정적 순서
        entry = entries[name]
        if include_set is not None and name not in include_set:
            continue
        if section_set is not None:
            tail = entry.section.rsplit("/", 1)[-1].lower()
            if tail not in section_set:
                continue
        manifest = manifest_from_package(entry)
        # 합성 app_id 충돌(g++/gplusplus 등) 시 첫 번째만(결정적).
        if manifest.app_id in seen_ids:
            continue
        seen_ids.add(manifest.app_id)
        apps.append(manifest)
    return Catalog(apps=tuple(apps))


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="apt_catalog",
        description="Build an ALR in-app catalog (AppManifest list) from a Debian/Ubuntu "
        "'Packages' index.",
    )
    parser.add_argument("--index", help="path to a (decompressed) Packages index file")
    parser.add_argument(
        "--include", action="append", dest="include",
        help="only catalog this package (repeatable; default = all)",
    )
    parser.add_argument(
        "--section", action="append", dest="sections",
        help="only catalog packages in this Section tail (repeatable)",
    )
    parser.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.index:
        parser.error("--index is required (or use --selftest)")

    text = open(args.index, encoding="utf-8", errors="replace").read()
    catalog = build_catalog_from_index(text, include=args.include, sections=args.sections)
    print(f"catalog: {len(catalog.apps)} app(s)")
    for app in catalog.apps:
        size_mb = app.total_install_size_bytes / (1024 * 1024)
        print(f"  - {app.app_id}  [{app.category}]  {size_mb:.1f} MB  — {app.summary}")
    return 0


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    check("section graphics→graphics", section_to_category("graphics") == CATEGORY_GRAPHICS)
    check("section component-prefixed", section_to_category("universe/web") == CATEGORY_INTERNET)
    check("section unknown→utility", section_to_category("frobnicate") == CATEGORY_UTILITY)
    check("section None→utility", section_to_category(None) == CATEGORY_UTILITY)

    groups = parse_depends_detailed("libc6 (>= 2.34), libfoo | libbar [arm64]")
    check("detailed: constraint kept", groups[0][0].constraint == ">= 2.34")
    check("detailed: arch kept", groups[1][1].arch == "arm64")
    check("detailed: alt count", len(groups[1]) == 2)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
