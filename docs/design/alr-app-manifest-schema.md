# ALR 리눅스 앱 패키징 매니페스트 스키마 (T2)

상태: **host-검증됨** (`tools/alr_manifest.py` + `tests/test_alr_manifest.py`, 75 tests green).
트랙: product-ux / T2. 런타임/Android 의존 0 — 순수 파서·모델만. UI·설치기·권한프롬프트는 이 스키마를 **소비**한다.

## 0. 왜 매니페스트인가 (제품 컨텍스트)

패키징 모델 = **"런타임 + 인앱 카탈로그"** (찬우 확정). ALR 런타임 앱 하나를 설치하면, 그 안에서
리눅스 앱을 **카탈로그/런처**로 보고 설치·실행한다(데스크탑형). 각 리눅스 앱은 하나의 **매니페스트(JSON)**
로 기술된다. 매니페스트는 런타임에게 다음을 알려준다:

- 카탈로그/런처에 **어떻게 보여줄지** (이름·요약·설명·카테고리·아이콘),
- 실행 시 **무엇을 띄울지** (`entry`: 바이너리+argv 또는 `.desktop`),
- 설치에 **무엇이 필요한지** (`rootfs_deps`: overlay stage-tar 또는 apt 패키지 + 예상 크기),
- **어떤 권한**을 요구하는지 (`required_permissions`, 닫힌 enum — **T4 권한프롬프트 트랙과 공유 계약**),
- **어떻게 표시**할지 (`display`: windowed/fullscreen + 해상도 힌트),
- 최소 런타임 버전 (`min_runtime`), 설치 크기 (`install_size`).

이 스키마는 **런타임이 없어도** host에서 검증·동결할 수 있도록 순수 로직으로 구현했다. Kotlin/Android
측(카탈로그 화면, `RootfsInstaller` overlay 적용, 권한 프롬프트)은 **동일한 필드 형태**를 소비한다.

## 1. 최상위 필드

| 필드 | 타입 | 필수 | 기본 | 의미 |
|---|---|:--:|---|---|
| `app_id` | string | ✔ | — | 역DNS id (카탈로그 키·디렉터리명·intent 타깃). `^label(.label)+$` |
| `name` | string | ✔ | — | 런처 표시명 (비어있을 수 없음) |
| `summary` | string | ✔ | — | 한 줄 요약 (카탈로그 카드) |
| `entry` | object | ✔ | — | 실행 진입점 (§4) |
| `description` | string | | `""` | 긴 설명 (상세 화면) |
| `category` | enum | | `utility` | 런처 섹션 (§2) |
| `icon` | string | | `""` | rootfs 절대경로(png/svg) **또는** `.desktop` 참조 |
| `rootfs_deps` | array | | `[]` | 필요 overlay/패키지 목록 (§5) |
| `required_permissions` | array&lt;enum&gt; | | `[]` | 요구 권한 (§3, 닫힌 enum, 중복 금지) |
| `display` | object | | `{mode:windowed}` | 표시 모드 + 해상도 힌트 (§6) |
| `min_runtime` | version | | `"0.1"` | 최소 ALR 런타임 버전 (§7) |
| `install_size_bytes` | int(≥0) | | `0` | 설치 후 예상 크기(전체). 0이면 deps 합으로 폴백 |

알 수 없는 최상위 필드는 **무시**(전방호환). 알 수 없는 enum 값(category/permission/display.mode/
entry.kind/dep.kind)은 **하드 에러**(오타·악성 매니페스트가 미인식 capability를 프롬프트 UI에 밀어넣지
못하게).

## 2. `category` (닫힌 enum)

런처가 앱을 묶는 섹션. 닫힌 집합이라 섹션 목록이 유한·로컬라이즈 가능:

```
graphics  development  office  internet  multimedia
games  system  utility  education  terminal
```

## 3. `required_permissions` — T4 공유 계약 (닫힌 enum)

이 enum이 **T2↔T4의 SSOT**다. T2는 매니페스트가 의도를 선언하고 검증이 enum 밖 값을 거부하게만 하면
된다. 런타임이 각 값을 적절한 Android 게이트로 매핑한다(아래는 **권고 매핑** — 최종 매핑/프롬프트 카피는
T4가 확정):

| 권한 값 | 의미 | 런타임 매핑 권고 (T4 확정) |
|---|---|---|
| `storage-read` | 사용자 파일 읽기 | SAF(`ACTION_OPEN_DOCUMENT[_TREE]`) 또는 `READ_MEDIA_*` |
| `storage-write` | 사용자 파일 쓰기 | SAF write grant / `MANAGE_EXTERNAL_STORAGE`는 지양 |
| `camera` | 카메라 | `android.permission.CAMERA` 런타임 요청 |
| `microphone` | 마이크 | `android.permission.RECORD_AUDIO` |
| `location` | 위치 | `ACCESS_FINE/COARSE_LOCATION` |
| `network` | 네트워크 | `INTERNET` (현재 Manifest에 이미 선언됨) — 프롬프트 불요/순수 인앱 그랜트 |
| `notifications` | 알림 | `POST_NOTIFICATIONS` (Android 13+) |

규칙: 알 수 없는 권한 = 에러, 중복 = 에러. **이 표를 바꾸면 `tools/alr_manifest.py`의 `PERMISSIONS`
와 T4 프롬프트 매핑을 같이 바꿔야 한다.**

> **열린 결정(T4/통합):** 위 "런타임 매핑 권고" 열의 최종화 + 프롬프트 카피 + 거부 시 폴백 동작.

## 4. `entry` — 실행 진입점

```jsonc
// kind="exec": 게스트 바이너리 + argv
"entry": { "kind": "exec", "target": "/usr/bin/gimp", "argv": ["--no-splash"] }
// kind="desktop": rootfs 내 .desktop 참조 (런타임이 Exec=/Icon= 해석)
"entry": { "kind": "desktop", "target": "/usr/share/applications/org.gimp.GIMP.desktop" }
```

- `target`는 항상 **절대 in-rootfs 경로**(`/`로 시작), `..` 금지.
- `kind="desktop"`이면 `target`은 `.desktop`으로 끝나야 하고 `argv`는 **비어야** 한다(.desktop이 커맨드라인 소유).
- `kind` enum: `exec` | `desktop`.

런타임 측에서 `exec` argv는 `NativeCommandRunner`/native-loader가 쓰는 **개행구분 argv**(path 다음 각
arg)로 변환된다 — 이미 MainActivity의 프로브들이 그 형식을 쓴다.

## 5. `rootfs_deps` — §5-E stage-tar/overlay 규약과의 정합

각 dep은 **overlay stage-tar** 또는 **apt 패키지** 하나다:

```jsonc
{ "kind": "stage-tar", "ref": "gimp-stage.tar", "install_size_bytes": 220000000 }
{ "kind": "apt",       "ref": "gimp-data",      "install_size_bytes": 30000000 }
```

### 5.1 `kind="stage-tar"` — 기존 `RootfsInstaller.extractOverlayTar` 규약 직접 매핑
- `ref`는 **bare 파일명**(경로구분자·`..` 금지), `.tar`로 끝나야 함 — `tools/STAGE_TAR_SPEC.md` §5-E의
  `<name>-stage.tar` 모양. 런타임은 이를 자기 stage-tar 소스 디렉터리에서 해석(공격자 경로 아님).
- 디바이스 추출은 `.{name}-staged-<size>` 마커로 재추출을 게이트한다(MainActivity 패턴). 파서가
  `RootfsDep.stage_marker_stem`(=`ref`에서 `-stage.tar`/`.tar` 제거)을 노출해 host 툴/UI가 그 마커
  파일명을 예측할 수 있다 — 예: `gimp-stage.tar` → 마커 stem `gimp` → `.gimp-staged-<len>`.
- overlay는 §5-E를 따른다(`./`-rooted 상대경로, 안전 심링크, flat-SONAME, **base-lib 다운그레이드
  금지** — `extractOverlayTar`의 frozen-SONAME 가드가 디바이스에서 강제, `tools/overlay_guard.py`가
  빌드에서 강제). 즉 매니페스트가 가리키는 stage-tar는 이미 가드를 통과한 산출물이어야 한다.

### 5.2 `kind="apt"`
- `ref`는 Debian/Ubuntu-noble 패키지명(policy 5.6.1 문법). 런타임이 rootfs에 `apt install` 한다.
- 주의(런타임 의존, 미해결): rootfs `apt install`의 maintainer-script fork/exec는 **exec-re-entry 벽**
  (ADR-003)에 걸린다 — 현재 dpkg/apt는 **실행+버전**까지 증명됨. 따라서 apt-dep 앱은 그 로더 기능이
  열린 뒤 실제 설치 가능. 매니페스트 스키마 자체는 그와 무관(선언만).

### 5.3 설치 크기
- 각 dep의 `install_size_bytes`는 카탈로그의 "다운로드 X MB" 표시용 추정.
- 앱 최상위 `install_size_bytes`가 0이면 `total_install_size_bytes`가 dep 크기 합으로 폴백한다.

## 6. `display`

```jsonc
"display": { "mode": "windowed", "width": 1280, "height": 1024 }
```
- `mode` enum: `windowed` | `fullscreen`.
- `width`/`height`는 **선호 픽셀 힌트**(0=미설정). 컴포지터가 1200×1920 디바이스 surface로 클램프할 수 있음.

## 7. `min_runtime` — 버전 형식

`MAJOR.MINOR[.PATCH][-tag]` (숫자 점구분 코어 + 선택 `-tag`). 예: `0.4.137`, `0.4.137-cp6`, `1.0`.
런타임의 빌드 스탬프(`0.4.137-cp6-mr2-v137`)와 비교 가능한 형식이 되도록 코어는 숫자만 둔다.
런타임 < `min_runtime`이면 카탈로그가 "업데이트 필요"로 표시(비교/게이트는 런타임/통합 측).

## 8. 카탈로그

카탈로그 = 매니페스트의 **순서 있는 목록**(`app_id` 유일). JSON은 두 형태 허용:
- bare 리스트: `[ {manifest}, ... ]`
- 객체: `{ "apps": [ {manifest}, ... ], ... }` (향후 카탈로그 메타데이터 수용 여지)

중복 `app_id`는 에러. `Catalog.get(app_id)`로 조회.

## 9. API (`tools/alr_manifest.py`)

| 함수/타입 | 용도 |
|---|---|
| `parse_manifest(dict) -> AppManifest` | 디코드된 JSON 객체 검증·구축 |
| `load_manifest(path) -> AppManifest` | 파일에서 로드(+JSON 에러 래핑) |
| `parse_catalog(list\|dict) -> Catalog` | 카탈로그 구축(중복 id 거부) |
| `load_catalog(path) -> Catalog` | 카탈로그 파일 로드 |
| `AppManifest / AppEntry / RootfsDep / DisplaySpec / Catalog` | frozen dataclass 모델(직접 생성도 검증) |
| `ManifestError` | 모든 파싱/검증 실패의 단일 예외(필드명 포함) |
| `CATEGORIES / PERMISSIONS / DISPLAY_MODES / DEP_KINDS / ENTRY_KINDS` | 닫힌 enum 집합 |

dataclass 직접 생성도 `__post_init__`에서 검증한다 — Kotlin/설치기 글루가 JSON을 거치지 않고
모델을 구성하는 경로도 동일하게 막힌다.

## 10. 예시 매니페스트 (GIMP)

```json
{
  "app_id": "org.gimp.GIMP",
  "name": "GIMP",
  "summary": "GNU Image Manipulation Program",
  "description": "Raster graphics editor for image retouching and editing.",
  "category": "graphics",
  "icon": "/usr/share/icons/hicolor/256x256/apps/gimp.png",
  "entry": { "kind": "exec", "target": "/usr/bin/gimp", "argv": ["--no-splash"] },
  "rootfs_deps": [
    { "kind": "stage-tar", "ref": "gimp-stage.tar", "install_size_bytes": 220000000 },
    { "kind": "apt", "ref": "gimp-data", "install_size_bytes": 30000000 }
  ],
  "required_permissions": ["storage-read", "storage-write"],
  "display": { "mode": "windowed", "width": 1280, "height": 1024 },
  "min_runtime": "0.4.137",
  "install_size_bytes": 250000000
}
```

## 11. 경계: 미리 가능한 것 vs 런타임 의존

- **미리(host) 완결:** 스키마·파서·검증·카탈로그 로더·§5-E 마커 stem 예측 — 전부 이 트랙에서 끝남(green).
- **런타임 의존(다른 트랙/통합):**
  - 권한 enum → Android 게이트 매핑/프롬프트 카피 = **T4**(§3 열린 결정).
  - stage-tar 실제 추출 = `RootfsInstaller.extractOverlayTar`(소유 외, 읽기만 함). 본 스키마는 그 입력
    형태(bare `.tar` 파일명 + 마커 stem)에 정합.
  - apt-dep 실제 설치 = exec-re-entry(ADR-003) 열린 뒤 가능.
  - `min_runtime` 비교·게이트 = 런타임/통합.
  - `entry.exec` argv → 개행구분 argv 변환·런치 = native-loader/MainActivity(소유 외).

## 12. 검증

```
cd /Users/naen/Documents/alr-product-ux
PATH="$HOME/.local/bin:$PATH" uvx --with pytest pytest tests/test_alr_manifest.py -q
# 75 passed
```
