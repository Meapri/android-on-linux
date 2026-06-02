"""apt 카탈로그 v1 — 선택 패키지의 설치 플랜(closure → stage-tar 지시).

찬우 확정 결정 (3) v1: 사용자가 카탈로그에서 패키지를 고르면, 그 패키지의 전이적
의존성 closure 를 풀어 **§5-E stage-tar 로 풀어 설치**한다 — 게스트에서 dpkg 를
fork-exec 하지 않는다. 그 이유는 ADR-003: ALR 로더는 single map+jump 라
게스트가 ``execve`` 하는 자식(dpkg→tar/dpkg-deb 등)은 커널이 새 ELF 로
주소공간을 교체하면서 로더 재진입이 안 되는 **exec re-entry 벽**에 부딪힌다.
그래서 v1 은 *호스트 측*에서 .deb closure 를 추출해 한 장의 overlay tar 로
만들고, 디바이스의 ``RootfsInstaller.extractOverlayTar`` 가 그걸 그냥 푼다
(dpkg 의 maintainer-script fork-exec 전부 우회). v2(인-게스트 apt/dpkg)는 exec
re-entry 가 ADR-003 (B)/(B-1) 경로로 풀린 뒤 — 그 전환 훅 자리는 §v2-hook.

이 모듈은 그 **플랜**을 만든다(다운로드/추출은 안 함 — 그건 deb_closure 의
network 경로 build_overlay/build_minimal_overlay 가 디바이스/CI 빌드 단계에서
한다). 입력: 선택 패키지 + 인덱스(또는 PackageEntry 맵) + **이미 base rootfs 에
있는 패키지 제외 목록**. 출력: ``InstallPlan`` = 받아야 할 .deb 목록 + 합산
다운로드/설치 크기 + stage-tar 생성 지시(어떤 패키지를 어떤 tar 로).

중복 구현 금지 — closure BFS 와 Provides/대안 해결은 ``tools.deb_closure`` 에
이미 있고 재사용한다. 다만 deb_closure.resolve_closure 는 *base 제외*를 모르므로
(overlay 빌드는 추출 후 SONAME/path 레벨에서 빼는 모델), 여기선 closure 단계에서
**base 패키지를 아예 안 따라가는** 변형을 한다(.deb 다운로드 자체를 줄임). 둘 다
같은 Provides 맵·대안 규칙을 쓴다.

host 검증: tests/test_install_plan.py (오프라인 픽스처).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field

from tools.apt_catalog import PackageEntry, parse_package_index
from tools.deb_closure import build_provides_map, parse_depends


# --------------------------------------------------------------------------- #
# closure (base 제외 인지) — deb_closure 의 BFS 를 base-aware 로 변형
# --------------------------------------------------------------------------- #

def _resolve_alt(
    alternatives: list[str],
    index: dict[str, PackageEntry],
    provides_map: dict[str, list[str]],
    base: set[str],
) -> str | None:
    """대안 그룹에서 첫 번째로 해결되는 패키지명, 없으면 None.

    deb_closure._resolve_alternative 과 같은 규칙(첫 후보 우선, Provides 폴백)이되
    *base 에 이미 있는* 대안이 먼저 만족하면 그걸 골라 closure 를 거기서 끊는다 —
    base 가 이미 그 의존을 충족하므로 더 받을 필요가 없다.
    """
    # 1) base 가 직접 만족하는 대안이 있으면 그것(받을 필요 없음 → 그대로 반환).
    for alt in alternatives:
        if alt in base:
            return alt
    # 2) base 가 그 가상 패키지의 어떤 제공자를 이미 갖고 있으면 거기서 끊음.
    for alt in alternatives:
        for provider in provides_map.get(alt, ()):  # Provides 폴백
            if provider in base:
                return provider
    # 3) 아니면 첫 실제/제공 후보(deb_closure 와 동일).
    for alt in alternatives:
        if alt in index:
            return alt
        providers = provides_map.get(alt)
        if providers:
            return providers[0]
    return None


def resolve_install_closure(
    targets: list[str],
    index: dict[str, PackageEntry],
    *,
    base: set[str] | None = None,
    provides_map: dict[str, list[str]] | None = None,
    log: list[str] | None = None,
) -> list[str]:
    """선택 패키지의 전이적 의존성 closure — **base 에 있는 건 제외**.

    deb_closure.resolve_closure 와 같은 BFS·대안·Provides 규칙을 쓰되:
      * base 에 이미 있는 패키지는 closure 에 넣지 않고 *따라가지도* 않는다
        (그 패키지 + 그 패키지의 하위 의존을 base 가 이미 제공).
      * 순환 의존은 ``seen`` 집합으로 안전(한 번만 방문).
      * 대안 의존(``a | b``)은 base 가 이미 만족하면 거기서 끊고, 아니면 첫 후보.
      * 인덱스에 없는 의존은 log 에 남기고 건너뜀(치명적 아님).

    Returns BFS 발견 순서의 중복 제거된 *새로 설치할* 패키지명 리스트.
    """
    base = base or set()
    if provides_map is None:
        provides_map = build_provides_map(
            {n: e.raw for n, e in index.items()}  # deb_closure 는 stanza dict 를 받음
        )

    ordered: list[str] = []
    seen: set[str] = set(base)  # base 는 처음부터 '본 것'으로 → 절대 enqueue 안 됨
    queue: list[str] = []

    def enqueue(name: str) -> None:
        if name not in seen:
            seen.add(name)
            queue.append(name)

    for target in targets:
        if target in base:
            if log is not None:
                log.append(f"target already in base: {target}")
            continue
        if target in index:
            enqueue(target)
        else:
            resolved = _resolve_alt([target], index, provides_map, base)
            if resolved is not None and resolved not in base:
                enqueue(resolved)
            elif log is not None:
                log.append(f"target not in index: {target}")

    while queue:
        name = queue.pop(0)
        ordered.append(name)
        entry = index.get(name)
        if entry is None:
            continue
        for group in entry.depends_groups:  # deb_closure.parse_depends 기반
            resolved = _resolve_alt(group, index, provides_map, base)
            if resolved is None:
                if log is not None:
                    log.append(f"unsatisfied dep group {group} (from {name})")
                continue
            if resolved in base:
                continue  # base 가 만족 → 끊음
            enqueue(resolved)

    return ordered


# --------------------------------------------------------------------------- #
# InstallPlan
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class DebToFetch:
    """closure 의 한 패키지 = 받아야 할 .deb 한 개."""

    name: str
    version: str
    filename: str               # pool/.../foo_1_arm64.deb (mirror 상대)
    download_size: int          # .deb 바이트 (Size:)
    installed_size_bytes: int   # 설치 후 바이트 (Installed-Size KiB→B)


# stage-tar basename 규약: §5-E 의 ``<name>-stage.tar``. RootfsInstaller 가
# ``.{name}-staged-<size>`` 마커를 그 stem 으로 만든다(alr_manifest.RootfsDep.
# stage_marker_stem 과 일치). 여기선 root 패키지명으로 tar 이름을 만든다.
def stage_tar_name(root_package: str) -> str:
    """설치 플랜의 stage-tar basename (예: ``gimp`` → ``gimp-stage.tar``)."""
    # alr_manifest.RootfsDep 의 stage-tar ref 규칙: 경로 없음 + .tar 로 끝남.
    safe = root_package.replace("/", "-").replace("\\", "-")
    return f"{safe}-stage.tar"


@dataclass(frozen=True)
class InstallPlan:
    """선택 패키지를 v1(stage-tar) 방식으로 설치하기 위한 플랜.

    UI 는 이걸로 "내려받기 X MB / 설치 후 Y MB" 를 보여주고, 빌드 단계
    (deb_closure.build_overlay/build_minimal_overlay)는 ``debs`` 의 Filename 을
    받아 ``stage_tar`` 이름의 overlay tar 를 만든다.
    """

    root_packages: tuple[str, ...]      # 사용자가 고른 것
    debs: tuple[DebToFetch, ...]        # 받을 .deb (closure, base 제외)
    stage_tar: str                      # 만들 overlay tar basename (§5-E)
    excluded_base: tuple[str, ...]      # base 라서 안 받은 (target 중) 패키지
    missing: tuple[str, ...]            # 인덱스에 없던 의존/타깃 (log)

    @property
    def download_size(self) -> int:
        """받을 .deb 총 바이트 (네트워크). UI '내려받기 N MB' 용."""
        return sum(d.download_size for d in self.debs)

    @property
    def installed_size(self) -> int:
        """설치 후 총 바이트(추출 전 추정). UI '설치 후 N MB' 용.

        주의: 이건 *전체 .deb 페이로드* 합이다. deb_closure 의 base-SONAME/path
        subtraction·prune 으로 실제 overlay 는 더 작아진다(중복/문서/로케일 제거).
        그래서 이 값은 *상한*이고, 정확한 on-device 크기는 build 후 file_count/
        tar 크기로 갱신된다(catalog-apt-v1.md §size-honesty).
        """
        return sum(d.installed_size_bytes for d in self.debs)

    @property
    def fetch_filenames(self) -> tuple[str, ...]:
        """build 단계가 mirror 에서 받을 Filename 목록(결정적 순서)."""
        return tuple(d.filename for d in self.debs)

    def as_dict(self) -> dict:
        return {
            "root_packages": list(self.root_packages),
            "debs": [
                {
                    "name": d.name,
                    "version": d.version,
                    "filename": d.filename,
                    "download_size": d.download_size,
                    "installed_size_bytes": d.installed_size_bytes,
                }
                for d in self.debs
            ],
            "stage_tar": self.stage_tar,
            "excluded_base": list(self.excluded_base),
            "missing": list(self.missing),
            "download_size": self.download_size,
            "installed_size": self.installed_size,
        }


def build_install_plan(
    targets: list[str],
    index: dict[str, PackageEntry],
    *,
    base: set[str] | None = None,
    stage_tar: str | None = None,
) -> InstallPlan:
    """선택 패키지 → InstallPlan (closure → 받을 .deb 목록 + stage-tar 지시).

    base 에 이미 있는 패키지는 closure 에서 제외(따라가지도 않음)되어 다운로드를
    줄인다. closure 의 각 패키지는 하나의 받을 .deb 가 되고, 전체가 하나의
    ``<root>-stage.tar`` overlay 로 합쳐진다(여러 root 면 첫 root 이름 기준,
    호출자가 ``stage_tar`` 로 덮어쓸 수 있음).
    """
    base = base or set()
    provides_map = build_provides_map({n: e.raw for n, e in index.items()})

    log: list[str] = []
    closure = resolve_install_closure(
        targets, index, base=base, provides_map=provides_map, log=log
    )

    debs: list[DebToFetch] = []
    for name in closure:
        entry = index.get(name)
        if entry is None:
            log.append(f"closure package not in index (skipped): {name}")
            continue
        if not entry.filename:
            log.append(f"no Filename for {name} (cannot fetch .deb)")
            continue
        debs.append(
            DebToFetch(
                name=name,
                version=entry.version,
                filename=entry.filename,
                download_size=entry.download_size,
                installed_size_bytes=entry.installed_size_bytes,
            )
        )

    excluded = tuple(sorted(t for t in targets if t in base))

    if stage_tar is None:
        root = targets[0] if targets else "alr-app"
        stage_tar = stage_tar_name(root)

    return InstallPlan(
        root_packages=tuple(targets),
        debs=tuple(debs),
        stage_tar=stage_tar,
        excluded_base=excluded,
        missing=tuple(log),
    )


# --------------------------------------------------------------------------- #
# v2 전환 훅 (인-게스트 apt/dpkg) — 자리만
# --------------------------------------------------------------------------- #
#
# §v2-hook: ADR-003 (B)/(B-1) 로 exec re-entry 가 풀리면(게스트 execve 자식이
# seccomp 필터 보존 + PTRACE_O_TRACEEXEC 로 자동 재포착되고, execve x0 path 가
# rootfs 로 rewrite), 게스트에서 직접 ``apt-get install`` 을 돌릴 수 있다 →
# stage-tar 사전빌드가 불필요해진다. 그 시점의 플랜은 closure 를 호스트가 풀지
# 않고 "게스트 apt 트랜잭션 명세"(설치할 패키지 + apt 옵션)로 바뀐다. 아래 훅은
# 그 전환점에서 build_install_plan 을 대체할 함수의 *시그니처 자리*만 표시한다 —
# v1 에서는 구현하지 않는다(NotImplementedError).
def build_apt_transaction_v2(targets: list[str], **kwargs):
    """v2(인-게스트 apt/dpkg) 전환 자리. ADR-003 exec re-entry 해금 전엔 미구현.

    풀리면 이 함수가 stage-tar 사전빌드 없이 게스트 apt 트랜잭션 명세를 만든다.
    """
    raise NotImplementedError(
        "in-guest apt/dpkg (v2) requires ADR-003 exec re-entry; v1 uses stage-tar "
        "via build_install_plan() — see docs/design/catalog-apt-v1.md §v2-transition"
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="install_plan",
        description="Resolve a package's dependency closure (excluding base packages) "
        "into a v1 stage-tar install plan.",
    )
    parser.add_argument("--index", help="path to a (decompressed) Packages index file")
    parser.add_argument(
        "--package", action="append", dest="packages",
        help="package to install (repeatable)",
    )
    parser.add_argument(
        "--base-package", action="append", dest="base",
        help="package already present in the base rootfs — excluded from closure "
        "(repeatable)",
    )
    parser.add_argument("--stage-tar", help="override the output stage-tar basename")
    parser.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.index or not args.packages:
        parser.error("--index and at least one --package are required (or --selftest)")

    text = open(args.index, encoding="utf-8", errors="replace").read()
    index = parse_package_index(text)
    plan = build_install_plan(
        args.packages, index, base=set(args.base or ()), stage_tar=args.stage_tar
    )
    print(f"install plan for {', '.join(plan.root_packages)}")
    print(f"  stage-tar:   {plan.stage_tar}")
    print(f"  fetch .debs: {len(plan.debs)}")
    for d in plan.debs:
        print(f"    - {d.name} {d.version}  ({d.download_size / 1024:.0f} KiB)")
    print(f"  download:    {plan.download_size / (1024 * 1024):.1f} MB")
    print(f"  installed≤:  {plan.installed_size / (1024 * 1024):.1f} MB (upper bound)")
    if plan.excluded_base:
        print(f"  base-excluded targets: {', '.join(plan.excluded_base)}")
    if plan.missing:
        print(f"  notes: {len(plan.missing)}")
        for m in plan.missing:
            print(f"    - {m}")
    return 0


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    check("stage_tar_name", stage_tar_name("gimp") == "gimp-stage.tar")

    fixture = (
        "Package: app\nVersion: 1.0\nDepends: liba\n"
        "Filename: pool/a/app.deb\nSize: 100\nInstalled-Size: 10\n\n"
        "Package: liba\nVersion: 2.0\nDepends: libbase\n"
        "Filename: pool/a/liba.deb\nSize: 50\nInstalled-Size: 5\n\n"
        "Package: libbase\nVersion: 3.0\nFilename: pool/b/libbase.deb\n"
        "Size: 999\nInstalled-Size: 99\n"
    )
    index = parse_package_index(fixture)
    plan = build_install_plan(["app"], index, base={"libbase"})
    names = [d.name for d in plan.debs]
    check("closure includes app+liba", names == ["app", "liba"])
    check("closure excludes base libbase", "libbase" not in names)
    check("download size = 150", plan.download_size == 150)
    check("stage tar name", plan.stage_tar == "app-stage.tar")

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
