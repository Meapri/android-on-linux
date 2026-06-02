# T3 — 앱 런처 / 선택 UI 설계 (홈 그리드)

상태: **설계 + host-검증 파서**. UI는 Compose 골격(`ui/LauncherScreen.kt`)만 제공
(빌드 미통합 — compose 의존성은 통합 세션이 추가). 런타임 실행 경로는 §5 계약으로 분리.

대상 기기: SM-X236N (Android 16, 1200×1920, 90Hz, Mali-G615). 비root, public API only.

관련 산출물
- 파서/모델 (host-검증): `tools/desktop_entry.py` + `tests/test_desktop_entry.py` (31 통과)
- UI 골격: `app/src/main/java/dev/chanwoo/androlinux/ui/LauncherScreen.kt`
- 자매 설계: 카탈로그/설치(다른 트랙), 권한 프롬프트(다른 트랙) — 본 문서는 **런처(홈)** 만.

---

## 1. 제품 맥락 — 런처가 무엇을 띄우는가

ALR = "런타임 + 인앱 카탈로그" 데스크탑형 모델. ALR 런타임 앱 하나가 설치되고,
그 안에서 리눅스 GUI 앱(GIMP, foot, firefox, …)을 카탈로그/apt로 설치한 뒤 **런처
홈 그리드**에서 아이콘을 눌러 실행한다. 현 프론트는 "검증 하니스"(MainActivity가
onCreate에서 게스트를 하드코딩 probe로 띄움)일 뿐 런처 UI가 0이다. 이 문서가 그
런처 레이어를 설계한다.

런처가 보여주는 "앱 목록"의 원천 = rootfs 안의 freedesktop `.desktop` 파일
(`/usr/share/applications/*.desktop`, `~/.local/share/applications/*.desktop`).
GIMP/foot/firefox 등 모든 Debian GUI 패키지가 설치 시 여기에 `.desktop`을 깐다.
런처는 이걸 파싱해 그리드 엔트리로 만든다 — 우리가 앱마다 카탈로그를 수기로
관리할 필요가 없다(이미 host-검증된 `tools/desktop_entry.py`가 그 일을 한다).

```
rootfs/usr/share/applications/*.desktop
        │  (RootfsInstaller가 overlay/apt로 깐 파일)
        ▼
  desktop_entry.parse  ──►  List<LauncherEntry>  ──►  홈 그리드 렌더
        │                          │
   (host-검증 순수로직)        app_id = 런치 콜백 키
                                   │
                                   ▼  onLaunch(appId)  → §5 런타임 계약
```

---

## 2. 화면 구조 (Material 3)

```
┌─────────────────────────────────────────────┐
│  AndroLinux            ⌕ [검색…]        ⋮     │   ← TopAppBar + 검색 토글
├─────────────────────────────────────────────┤
│  [전체] [그래픽] [시스템] [네트워크] [개발]    │   ← 카테고리 필터 칩 (수평 스크롤)
├─────────────────────────────────────────────┤
│   ┌────┐   ┌────┐   ┌────┐   ┌────┐          │
│   │ 🖌 │   │ 🖥 │   │ 🦊 │   │ 📊 │          │   ← LazyVerticalGrid
│   GIMP    foot   Firefox  htop             │      (적응형, 최소 96dp 셀)
│                                              │
│   ┌────┐   ┌────┐                            │
│   │ ⚙ │   │ 📝 │      …                       │
│  설정    텍스트편집                            │
│                                              │
└─────────────────────────────────────────────┘
        (FAB: "+ 앱 설치" → 카탈로그 화면, 다른 트랙)
```

- **TopAppBar**: 타이틀 + 검색 아이콘(누르면 인라인 SearchBar로 전환) + 오버플로
  메뉴(정렬: 이름/카테고리/최근, 새로고침, 정보).
- **카테고리 필터 칩** (`FilterChip` row): "전체" + rootfs에 실제로 존재하는 primary
  category만 (`desktop_entry.categories_present`). 칩 0개면 행 자체를 숨김.
- **그리드** (`LazyVerticalGrid(GridCells.Adaptive(96.dp))`): 셀 = 아이콘(64dp) +
  1–2줄 이름(중앙 정렬, ellipsis). 90Hz 패널이므로 스크롤 애니메이션은 기본
  fling으로 충분(추가 스로틀 불필요).
- **FAB**: "앱 설치" → 카탈로그/apt 화면으로 (별도 트랙 소유). 본 런처는 콜백
  `onOpenCatalog()` 만 노출.

---

## 3. 인터랙션

| 제스처 | 동작 |
| --- | --- |
| 셀 탭 | `onLaunch(appId)` — 런타임에 실행 위임 (§5). 즉시 "실행 중…" 오버레이/스피너. |
| 셀 롱프레스 | 컨텍스트 시트(ModalBottomSheet): **정보 / 제거 / 바로가기 추가**. |
| 검색 입력 | `desktop_entry.matches_query` 와 동일 규칙으로 실시간 필터 (name/generic/comment/keyword). |
| 칩 선택 | primary category 로 필터. "전체"는 모두. 검색과 AND 결합. |
| Pull-to-refresh | `.desktop` 디렉토리 재스캔(앱 apt 설치 후 새 아이콘 반영). |

**정보 시트**: 이름, GenericName, Comment, 카테고리, Exec(원문), MimeType, 패키지명.
**제거**: 카탈로그/패키지 트랙의 `onUninstall(appId)` 콜백으로 위임(런처는 UI만).

---

## 4. 상태 (state machine)

런처 화면은 다음 상태를 가진다. `LauncherUiState` 한 sealed type 으로 표현:

| 상태 | 트리거 | 화면 |
| --- | --- | --- |
| `Loading` | 최초 진입 / 재스캔 | 그리드 자리 셰이머(스켈레톤). |
| `Empty` | `.desktop` 0개 (rootfs 막 설치, 앱 0개) | 빈 상태 일러스트 + "앱을 설치해 시작하세요" + 카탈로그 CTA 버튼. |
| `Ready(entries)` | 파싱 성공, ≥1개 | 그리드. |
| `Launching(appId)` | 셀 탭 | 그리드 위 디밍 + 진행 인디케이터(이 앱 실행 중). |
| `LaunchError(appId, msg)` | 런타임이 실행 실패 보고 | 스낵바 + 재시도. |

부가 배너(상태 위에 겹침):
- **설치 진행**: 카탈로그에서 apt 설치 중인 앱이 있으면 상단 진행 배너
  (`LinearProgressIndicator` + "GIMP 설치 중 64%"). 설치 끝나면 그리드에 새 아이콘
  페이드-인. (진행률 소스 = 패키지 트랙; 런처는 `installProgress: Flow` 구독만.)
- **오프라인**: 네트워크 없음 → "오프라인 — 설치된 앱만 실행 가능" 비차단 배너.
  이미 설치된 앱 실행은 100% 로컬(인터넷 불필요)이므로 그리드는 정상 동작; 카탈로그/
  apt 만 비활성. (네트워크 상태 = `ACCESS_NETWORK_STATE`, 이미 선언됨.)

---

## 5. 런타임 인터페이스 계약 (UI ↔ 런타임 경계)

런처는 **런타임 독립**이다. 실행/설치/제거를 직접 하지 않고 콜백으로 위임한다.
mock 가능하도록 경계를 명시한다(테스트/프리뷰는 in-memory fake로 구동).

```kotlin
/** 런처가 호출하는 런타임 측 진입점(통합 세션이 실제 구현 주입). */
interface LauncherRuntime {
    /** rootfs의 .desktop 카탈로그(이미 desktop_entry 규칙으로 파싱된 모델). */
    fun catalog(): Flow<List<AppEntry>>           // 재스캔 시 새 emit
    /** appId 앱을 게스트로 실행. 결과(성공/실패)는 LaunchResult 로 비동기 보고. */
    suspend fun launch(appId: String): LaunchResult
    /** 진행 중 apt 설치 진행률(없으면 빈 맵). */
    fun installProgress(): Flow<Map<String, Int>> // appId -> 0..100
    /** appId 제거(패키지 트랙으로 위임). */
    suspend fun uninstall(appId: String): Result<Unit>
}
```

데이터 계약 (UI가 아는 전부):
- `AppEntry(appId, name, genericName, comment, iconPath, category, terminal)` —
  `tools/desktop_entry.LauncherEntry` 의 1:1 사본(필드명 동일). 파싱은 **런타임 측**
  에서 `desktop_entry` 규칙(이 트랙이 host-검증)으로 수행 → UI는 순수 표시.
- `iconPath`: `desktop_entry.resolve_icon_path` 가 해석한 절대 경로(없으면 null →
  기본 아이콘). 런처는 이 경로의 PNG/SVG 를 디코드만 함(GIMP의 gdk-pixbuf 경로와
  무관, 안드로이드 측 디코더 사용).
- `LaunchResult = Started | Failed(reason)`. UI는 `Started`면 `Launching` 상태 유지
  (컴포지터가 SurfaceView로 전환), `Failed`면 `LaunchError`.

**경계 요지**: 본 트랙이 *미리 가능*한 것 = 파서/모델(`desktop_entry.py`, host-검증) +
UI 골격 + 상태기계 + 위 계약. *런타임 의존*(통합/실행 세션 결정 필요) = `launch()`가
실제 게스트를 어떻게 띄우는지(현 MainActivity의 nativeAlr…Probe / WaylandPresenter
경로를 `LauncherRuntime.launch`로 일반화), `catalog()`의 `.desktop` 실제 스캔 위치,
installProgress 의 apt 진행 소스. 이들은 본 문서가 계약만 고정하고 구현은 위임.

---

## 6. 접근성 / 폴리시

- 모든 셀에 `contentDescription = name` (TalkBack). 카테고리 칩도 라벨링.
- 터치 타겟 ≥ 48dp(셀 96dp 충족). 폰트 스케일/다크모드는 Material 3 토큰으로 자동.
- 권한: 런처 자체는 추가 권한 0(실행은 in-process, 인터넷은 카탈로그만 사용).
  → AndroidManifest 변경 **제안만** (별도 권한 트랙) — 본 트랙은 manifest 안 건드림.

---

## 7. 검증 (이 트랙)

- 순수 로직: `tests/test_desktop_entry.py` (31 통과) — 필드코드 strip, NoDisplay/
  Hidden/Type 필터, 멀티 카테고리, 로케일 Name(ko), 아이콘 경로 해석, 검색 매칭.
- UI 골격: `@Preview` 로 fake 카탈로그(GIMP/foot/firefox) 렌더 — 빌드 통합 후
  compose 프리뷰로 확인(현 단계는 소스 골격만, 빌드 미통합).
- 통합 게이트(다른 세션): `LauncherRuntime` 실제 구현 주입 후 device 스모크
  (그리드 → 탭 → GIMP 실행 → 컴포지터 전환).

---

## 8. 통합 세션에게 (열린 결정)

1. compose 의존성(`androidx.compose.*`, `activity-compose`, BOM)을 `build.gradle` 에
   추가하고 `MainActivity` 를 `ComponentActivity`/`setContent` 로 전환할지 — 현
   MainActivity 는 단일 `Activity` + View 기반. 런처는 별도 `LauncherActivity` 또는
   `setContent` 분기 중 택1 (제안: 검증 하니스 보존 위해 별도 LauncherActivity).
2. `LauncherRuntime.launch(appId)` 의 실제 바디 = 현 `nativeAlrNativeLoaderProbe` +
   WaylandPresenter 경로를 appId→exec_argv 로 일반화. argv 는 `desktop_entry` 가
   이미 제공(`exec_argv`).
3. `.desktop` 스캔 위치 — base rootfs `/usr/share/applications` + 앱 overlay 가 까는
   위치. overlay stage-tar 규약(§5-E) 과 정합 확인.
4. 아이콘 디코드 — `iconPath` 의 SVG 는 안드로이드 기본 디코더가 SVG 미지원 →
   AndroidSVG 또는 Coil-svg 필요 여부 결정(PNG 우선 해석은 파서가 이미 largest-size
   png 선호).
