> 생성 경위: 제품 UX 트랙 B(apt 인덱스 카탈로그 v1) 산출. 격리 브랜치 `research/product-ux`(base `main` d749073, 직전 커밋 2bf2f5d) 위에서 작성. 소유 신규 파일: `tools/apt_catalog.py`·`tools/install_plan.py`·`tests/test_apt_catalog.py`·`tests/test_install_plan.py`·본 문서. 기존 `tools/alr_manifest.py`·`tools/deb_closure.py`·`MainActivity.kt`·`RootfsInstaller.kt`는 **읽기 전용**(import/참조만). 패키징 결정(런타임+인앱 카탈로그, v1=stage-tar)은 찬우 확정. SSOT: `docs/design/adr-004-inapp-catalog-ux.md`(아키텍처)·`docs/design/adr-003-multiprocess-exec-reentry.md`(exec 벽). host 검증: `uvx --with pytest pytest tests/test_apt_catalog.py tests/test_install_plan.py -q` (49 passed).

# catalog-apt-v1 — apt 인덱스 카탈로그 v1: 인덱스로 목록 + closure를 stage-tar로 설치

- 상태: **Implemented (host-verified, device 게이트 미통과)** — 순수 호스트 로직(인덱스 파싱·카테고리 매핑·closure·플랜)은 pytest로 닫혔고, 실제 .deb 다운로드/추출/디바이스 설치는 device-only(§device-게이트).
- 워크스트림: **UX 트랙 B**(카탈로그 데이터/설치 플랜). 의존 공개: ADR-004 §7(4트랙 흐름)이 본 모듈을 `AlrRuntime.catalog()`/`install()`의 데이터 소스로 참조. `tools.alr_manifest`(T2 스키마)를 소비, `tools.deb_closure`(WS-4 빌드 엔진)를 재사용.
- HARD CONSTRAINTS(불변): 비root, 단일 APK + 앱-private rootfs/overlay, dpkg fork-exec 우회(ADR-003), `alr_manifest.py`/`deb_closure.py` 수정 0, version stamp 불변.

---

## 1. 한 줄 결론

**apt 카탈로그 v1은 "인덱스를 목록으로, closure를 한 장의 overlay tar로"이다 — 게스트에서 apt/dpkg를 *실행하지 않는다*.** Debian/Ubuntu `Packages` 인덱스(RFC822-식 stanza)를 파싱해 사용자가 고를 수 있는 카탈로그 목록(`alr_manifest.AppManifest`)을 만들고, 사용자가 하나를 고르면 그 패키지의 전이적 의존성 closure를 풀되 **이미 base rootfs에 있는 패키지는 제외**하고, 남은 .deb들을 (디바이스가 아니라 호스트/CI 빌드 단계가) 한 장의 `<name>-stage.tar` §5-E overlay로 합쳐 디바이스의 `RootfsInstaller.extractOverlayTar`가 그냥 푼다. dpkg의 maintainer-script fork-exec가 ADR-003의 exec re-entry 벽에 막히므로(§3), 그 fork-exec를 *전부 우회*하는 게 v1의 존재 이유다. 인-게스트 apt/dpkg(v2)는 exec 벽이 ADR-003 (B) 경로로 풀린 뒤로 미룬다(§v2-transition).

---

## 2. 모듈 분해 + 기존 인프라 연결점 (중복 구현 0)

이 트랙은 **새 파서/closure 엔진을 만들지 않는다**. 인덱스/Depends 파싱과 closure BFS는 이미 `tools/deb_closure.py`(WS-4 M2/M4 overlay 빌드 엔진)에 있고, 본 트랙은 그것을 **import해서 재사용**한다. 새로 만드는 건 deb_closure가 신경 안 쓰는 *제품 메타데이터 레이어*(카탈로그 표시·카테고리·설치 플랜)뿐.

| 신규 파일 | 하는 일 | 재사용(import) |
|---|---|---|
| `tools/apt_catalog.py` | 인덱스 → 구조화 `PackageEntry` → `alr_manifest.AppManifest` 카탈로그. Section→카테고리, Description split, KiB→byte. | `deb_closure.parse_packages`·`parse_depends`·`build_provides_map`·`_strip_dep_name`; `alr_manifest.AppManifest`/`AppEntry`/`RootfsDep`/`Catalog`/카테고리 enum |
| `tools/install_plan.py` | 선택 패키지 → **base-제외** closure → `InstallPlan`(받을 .deb + 합산 크기 + stage-tar 지시). | `deb_closure.build_provides_map`·`parse_depends`; `apt_catalog.PackageEntry`/`parse_package_index` |

### deb_closure와의 책임 분리 (왜 closure를 두 번 풀지 않는가)
- `deb_closure.resolve_closure`는 closure를 **추출 후** SONAME/path 레벨에서 base를 뺀다(overlay 빌드 모델: 일단 다 받아 추출하고 base가 owns하는 .so/path를 지움). 그건 *overlay 산출물의 정확성*엔 맞지만, **다운로드 자체를 줄이진 못한다**.
- `install_plan.resolve_install_closure`는 **closure 단계에서** base 패키지를 *아예 안 따라간다* → base가 이미 제공하는 서브트리의 .deb 다운로드를 절약(§4-B). 둘은 **같은 Provides 맵·대안 규칙**(`build_provides_map`, 첫-후보 우선)을 쓰되 base-aware 종료 조건만 다르다. 즉 install_plan은 "무엇을 받을지"(네트워크 최소화)를, deb_closure는 "받은 걸 어떻게 정확한 overlay로 만들지"(SONAME 다운그레이드 가드)를 담당 — 한 closure를 두 번 푸는 게 아니라 *다른 질문에 답하는 두 단계*다.

---

## 3. dpkg fork-exec 우회 근거 (ADR-003)

v1이 "stage-tar"인 이유는 편의가 아니라 **물리적 제약**이다.

- ALR 로더는 single map+jump(`alr_enter_guest`, `runtime_report.cpp` ~L1147). 게스트가 `apt-get install`을 돌리면 apt→dpkg→(tar/dpkg-deb/maintainer-script `postinst` 등)로 **연쇄 fork-exec**한다. 그 `execve` 자식은 커널이 새 ELF로 주소공간을 교체하면서 로더 재호출이 안 되는 **exec re-entry 벽**(ADR-003 §1·§2-B)에 걸린다.
- ADR-003은 그 벽이 (B-1) execve x0 path mediation + seccomp 필터 보존 + `PTRACE_O_TRACEEXEC`로 *풀린다*고 설계하나, **device 게이트 미통과**(ADR-003 §4 미검증 가정: envp `LD_PRELOAD` 전파 등). 즉 v1 시점엔 인-게스트 dpkg를 신뢰할 수 없다.
- **v1의 해법**: closure 추출·flatten을 **호스트(또는 CI 빌드)**에서 끝낸다 — `deb_closure.build_overlay`/`build_minimal_overlay`가 .deb를 받아 extract하고 base를 빼서 `<name>-stage.tar`를 굽는다. 디바이스는 maintainer-script를 *안 돌린다*; `RootfsInstaller.extractOverlayTar`가 tar를 풀고 `.{name}-staged-<size>` 마커(§5-E)를 남길 뿐이다. **fork-exec 0 → exec 벽 0**.
- 트레이드오프(정직): maintainer-script가 하던 일(예: ld.so 캐시 갱신, alternatives 등록)은 v1에서 *안 일어난다*. 런타임 데이터 의존이 없는 대부분의 라이브러리/단일 바이너리 앱(foot, gimp 류 — 메모리상 이미 stage-tar로 device-증명)엔 무해하고, script 의존 앱은 v2 또는 별도 overlay 보정으로 처리한다.

---

## 4. v1 파이프라인 (인덱스 → 목록 → closure → stage-tar → 설치)

```
 (A) 카탈로그(목록)                          (B) 설치(선택 시)
 ─────────────────────                       ──────────────────────────────────
 Packages.gz (mirror)                        사용자: "foot 설치"
   │ fetch_packages_index (deb_closure)         │
   ▼                                            ▼  build_install_plan(targets, index, base)
 parse_package_index ───────────────┐        resolve_install_closure  (base 제외, cycle-safe)
   │ {name: PackageEntry}            │           │  [foot, libutf8proc2]  (libc6=base → 제외)
   ▼                                 │           ▼
 build_catalog_from_index           │        InstallPlan{ debs[], download_size,
   │ Section→category, app_id 합성   │                     installed_size, stage_tar }
   ▼                                 │           │ fetch_filenames
 alr_manifest.Catalog (목록 UI) ◀────┘           ▼
   │                                          deb_closure.build_overlay/build_minimal_overlay
   │ AlrRuntime.catalog() (ADR-004 §5-F)         │  (.deb 받아 extract, base SONAME/path subtract,
   ▼                                             │   prune man/doc/locale, build_stage_tar)
 [T3 Catalog/AppDetail UI]                       ▼
                                              <name>-stage.tar  (§5-E, ./-rooted, sidecar)
                                                 │
                                                 ▼  device
                                              RootfsInstaller.extractOverlayTar
                                                 → overlay 풀림 + .{name}-staged-<size> 마커
                                                 → Launcher 그리드에 AppManifest 타일 등장
```

### (A) 목록 파이프라인 — `apt_catalog.py`
1. **인덱스 fetch**는 `deb_closure.fetch_packages_index`(Ubuntu noble = `ports.ubuntu.com/ubuntu-ports`, `main`+`universe`)가 이미 한다 — v1 카탈로그는 그 텍스트만 받으면 된다.
2. `parse_package_index` → `{name: PackageEntry}`. `PackageEntry`는 인덱스 stanza의 구조화 뷰: Package/Version/Section/Architecture/Description(요약+본문 split)/Depends/Pre-Depends/Filename/Size(.deb 바이트)/Installed-Size(KiB). 파싱 자체는 `deb_closure.parse_packages` 재사용.
3. `section_to_category(Section)` → `alr_manifest` 닫힌 카테고리. component 접두(`universe/graphics`)·대소문자 정규화, **모르는 섹션은 `utility` 폴백**(닫힌 enum 절대 위반 안 함).
4. `manifest_from_package` → `AppManifest`. app_id는 `org.debian.<sanitized>`로 합성(`g++`→`gplusplus`, `.`→`-`, '+'→'plus' — `alr_manifest._APP_ID` 라벨 규칙 충족). entry는 `exec /usr/bin/<pkg>`를 *추정*(§entry-path-honesty).
5. `build_catalog_from_index(include=…, sections=…)` → `alr_manifest.Catalog`. 인덱스 전체(수만 개·대부분 lib)는 비현실적이라 실 파이프라인은 큐레이션 `include` 목록을 준다. 결정적 순서(패키지명 정렬), 합성 app_id 충돌은 첫 항목만.

### (B) 설치 플랜 — `install_plan.py`
1. `build_install_plan(targets, index, base=…)`:
2. `resolve_install_closure`가 BFS로 전이적 Depends+Pre-Depends를 따라가되 — **base에 있는 패키지는 closure에 안 넣고 따라가지도 않음**, 순환 의존은 `seen` 집합으로 안전, 대안 의존(`a | b`)은 base가 만족하면 거기서 끊고 아니면 첫 후보, 인덱스에 없는 의존은 log에 남기고 건너뜀(치명적 아님).
3. closure의 각 패키지 = 받을 `DebToFetch`(Filename + download/installed 크기). `InstallPlan`이 `download_size`(.deb 합)·`installed_size`(설치 후 상한, §size-honesty)·`stage_tar`(§5-E basename)·`excluded_base`·`missing`을 들고 있음.
4. `InstallPlan.fetch_filenames` → 빌드 단계(`deb_closure.build_overlay`)가 mirror에서 받을 Filename 목록. 즉 install_plan은 *무엇을 받을지*를 정하고, 실제 받기·추출·flatten·base subtract는 deb_closure가 한다.

---

## 5. 정직 섹션

### §size-honesty — `installed_size`는 상한
`InstallPlan.installed_size`는 closure의 **전체 .deb Installed-Size 합**이다. 그런데 `deb_closure.build_overlay`가 (i) base가 owns하는 SONAME/path subtract, (ii) man/doc/locale/systemd prune을 하므로 **실제 on-device overlay는 더 작다**. 따라서 이 값은 UI '설치 후 N MB'의 *상한*이고, 정확한 크기는 build 후 tar 크기/`BuildResult.file_count`로 갱신해야 한다. UI는 "약 N MB 이하"로 보이는 게 정직하다.

### §entry-path-honesty — `entry.target`은 추정
인덱스만으론 실행 바이너리 경로를 모른다(`/usr/bin/<pkg>`는 흔하나 `gimp`→`gimp-2.10`처럼 어긋남). v1 `manifest_from_package`는 `/usr/bin/<pkg>`를 *가정*하고, 호출자가 `entry_path`로 덮을 수 있게 했다. 실 카탈로그 구축 파이프라인은 closure 추출 결과의 `.desktop`(`tools.desktop_entry` 파서)이나 추출된 `usr/bin/*`로 이 경로를 **교정**해야 한다 — 그건 추출(네트워크/디바이스) 단계의 일이라 이 host 모듈 범위 밖이다.

### §maintainer-script — v1은 안 돌림
§3대로 dpkg postinst/preinst 등을 *실행하지 않는다*. 런타임 스크립트 부작용(ldconfig, alternatives, 사용자/그룹 생성)이 필요한 앱은 v1 stage-tar로 부족할 수 있다. 그건 결함이 아니라 *exec 벽 우회의 정직한 비용*이고, 대부분의 self-contained GUI 앱엔 무해(메모리: GIMP/foot가 이미 stage-tar로 device-usable). script-의존 앱은 별도 보정 overlay 또는 v2로.

### 자가 적대검증 (핵심 주장 1개 자기공격)
주장 — "closure를 호스트 stage-tar로 구우면 게스트 fork-exec가 0이라 exec 벽이 안 걸린다". 공격: *closure 해결이 base를 잘못 빼면* 런타임에 미싱 라이브러리로 게스트가 죽거나, *반대로 base가 owns하는 lib를 overlay가 다운그레이드*하면 ABI가 깨진다. 방어: (i) base 제외는 `resolve_install_closure`가 closure에서 끊되, **최종 SONAME 다운그레이드 가드는 `deb_closure`의 `overlay_guard`(WS-4 M1, frozen-SONAME)가 build 단계에서 다시 검증** → install_plan의 base 집합이 불완전해도 overlay_guard가 다운그레이드를 막는다(이중 안전망). (ii) install_plan의 base 제외는 *다운로드 최소화*가 목적이지 *정확성의 단독 보증*이 아니다 — 정확성은 deb_closure의 추출-후 subtract가 SSOT. 따라서 install_plan이 base를 *덜* 빼면(보수적) 다운로드만 늘 뿐 정확성은 안 깨지고, *더* 빼면(공격적) deb_closure의 missing_soname/violations가 build 단계에서 잡는다. → 주장은 "fork-exec 0"에 대해선 성립(constraint 불변), "정확성"은 install_plan 단독이 아니라 deb_closure와 합쳐 보증.

**constraint_violations**: 없음. 새 syscall/권한/ptrace op 0. host 순수 로직(파싱·매핑·closure·플랜)만 추가, 네트워크/추출/디바이스 효과는 기존 deb_closure·RootfsInstaller 재사용.

---

## 6. host 검증

```
cd /Users/naen/Documents/alr-product-ux
PATH="$HOME/.local/bin:$PATH" uvx --with pytest pytest \
  tests/test_apt_catalog.py tests/test_install_plan.py -q
# → 49 passed
```

커버: Packages stanza 파싱(연속줄 folding, Size/Installed-Size, Description split), Section 매핑(known/component-prefix/unknown-fallback/shells→terminal), Depends 파싱(버전제약 `(>= x)`·대안 `a | b`·arch 한정 `[arm64]`·negation `[!amd64]`), `PackageEntry`→`AppManifest` 합성(app_id sanitize `g++`→gplusplus, 카테고리, KiB→byte, stage-tar dep + 마커 stem), closure(전이·base 제외·순환 안전·첫 대안·base가 만족하는 대안 끊기·base가 서브트리 끊기·missing 비치명), 크기 합산(download/installed, base 제외 확인), stage-tar 이름/override, `InstallPlan.as_dict` round-trip, v2 훅 `NotImplementedError`.

모듈 selftest(오프라인): `python3 -m tools.apt_catalog --selftest`, `python3 -m tools.install_plan --selftest`.

---

## 7. device 게이트 (host로 못 닫는 것)

본 모듈은 *순수 데이터 로직*만 host로 닫는다. 실제 설치는 ADR-004 §10 **M-UX-install-overlay**가 답한다:

- **DEVICE-REQ: ALR-catalog-apt-v1 — SM-X236N (am force-stop first)**: install_plan이 만든 `fetch_filenames`를 `deb_closure.build_overlay`로 받아 구운 `<name>-stage.tar`를 디바이스 `RootfsInstaller.extractOverlayTar`로 풀고, 그 앱이 Launcher 타일로 떠 실행(RENDERING)까지. gate = (i) overlay extracted>0 AND overlay_guard skip 정상(다운그레이드 0), (ii) 게스트 미싱 라이브러리 0(closure 정확), (iii) RENDERING 도달. fork-exec 0 확인(인-게스트 dpkg 미사용).
- 미검증(device): 실 mirror Packages 규모에서의 closure 시간/정확도, maintainer-script 미실행으로 인한 앱별 누락, `entry.target` 추정의 적중률(§entry-path-honesty).

---

## §v2-transition — 인-게스트 apt/dpkg 전환 조건

`install_plan.build_apt_transaction_v2`는 시그니처 자리만 있고 v1에선 `NotImplementedError`. **전환 조건**: ADR-003 (B)/(B-1) — 게스트 `execve` 자식이 (i) seccomp 필터를 execve 너머로 보존하고 (ii) `PTRACE_O_TRACEEXEC`로 자동 재포착되며 (iii) execve x0 path가 rootfs로 rewrite되고 (iv) `LD_PRELOAD`/`ALR_ROOTFS` envp가 자식에 전파됨 — 이 4가지가 **device로 증명**되면, 게스트에서 직접 `apt-get install`을 돌릴 수 있다. 그 시점에 v2는:
- closure를 호스트가 풀지 않고 게스트 apt가 푼다(`build_apt_transaction_v2`가 "게스트 apt 트랜잭션 명세"를 만듦 — 설치 패키지 + apt 옵션 + 사전구축된 dpkg admin DB(`tools/build_dpkg_db.py`)·noble sources(`tools/build_apt_overlay.py`) 전제).
- stage-tar 사전빌드가 불필요 → 다운로드/추출이 디바이스에서 점진적, maintainer-script도 (exec 벽이 풀렸으므로) 정상 실행.
- v1→v2는 **카탈로그 목록 코드(`apt_catalog.py`) 변경 없이** install 경로만 교체된다(목록은 둘 다 같은 인덱스→AppManifest). 즉 v2 전환은 `install_plan`의 한 함수 교체이지 카탈로그 재작성이 아니다.

---

관련 파일(절대경로):
- 본 문서: `/Users/naen/Documents/alr-product-ux/docs/design/catalog-apt-v1.md`
- 신규 소유: `/Users/naen/Documents/alr-product-ux/tools/apt_catalog.py`, `/Users/naen/Documents/alr-product-ux/tools/install_plan.py`, `/Users/naen/Documents/alr-product-ux/tests/test_apt_catalog.py`, `/Users/naen/Documents/alr-product-ux/tests/test_install_plan.py`
- 재사용(읽기 전용): `/Users/naen/Documents/alr-product-ux/tools/deb_closure.py`(parse_packages/parse_depends/build_provides_map/build_overlay), `/Users/naen/Documents/alr-product-ux/tools/alr_manifest.py`(AppManifest/RootfsDep/카테고리), `/Users/naen/Documents/alr-product-ux/tools/build_stage_tar.py`(§5-E flatten), `/Users/naen/Documents/alr-product-ux/tools/overlay_guard.py`(frozen-SONAME 가드)
- SSOT/선행: `/Users/naen/Documents/alr-product-ux/docs/design/adr-004-inapp-catalog-ux.md`(아키텍처·§5-F·§10 device 게이트), `/Users/naen/Documents/alr-product-ux/docs/design/adr-003-multiprocess-exec-reentry.md`(exec re-entry 벽·v2 전환 조건), `/Users/naen/Documents/alr-product-ux/tools/STAGE_TAR_SPEC.md`(§5-E 규약·extractOverlayTar 마커)
