> 생성 경위: T1(제품 UX 전체 아키텍처) 설계 산출. 격리 브랜치 `research/product-ux`(base `main` d749073), 읽기 전용으로 현 프론트(`MainActivity.kt`·`AndroidManifest.xml`·`RootfsInstaller.kt`·`RootfsInstallPlan.kt`·`NativeCommandRunner.kt`)를 확인하고 작성. `MainActivity.kt`/`AndroidManifest.xml`/`RootfsInstaller.kt`는 **수정 금지** — 본 ADR은 그 변경을 _제안_으로만 기술한다. 코드 사실은 위 파일 직접 독해, 패키징 결정(런타임+인앱 카탈로그)은 찬우 확정. 리뷰 대상: UX 4트랙(매니페스트/런처/권한/SAF), WS-1(런타임 §5 계약), 통합 세션. ADR-002/003 톤 계승(device 게이트·정직 섹션·constraint 불변).

# ADR-004 — 인앱 카탈로그 UX 아키텍처: "검증 하니스 MainActivity"를 RunningSurface로 강등하고 그 앞에 Launcher/Catalog를 둔다

- 상태: **Proposed** — 설계만. 코드 변경 없음(스켈레톤은 별도 UX 트랙이 신규 파일로). device 게이트 미통과.
- 워크스트림: **UX**(신규 product-ux). 의존 공개: 런타임 §5 계약(이 ADR이 정의, WS-1이 구현 측 수락), 매니페스트 스키마(T2)·런처/카탈로그 스켈레톤(T3)·권한 플로우(T4)·SAF 임포트(T5)가 본 ADR을 SSOT로 참조.
- 작성: 2026-06-02. 선행: 찬우 패키징 결정("런타임 앱 1개 설치 → 인앱 카탈로그/apt로 리눅스 앱 설치·런처 실행, 데스크탑형"), `docs/research/orchestration-5session-plan.md` §5(레이어 인터페이스 계약)·§5-E(stage tar 규약), `docs/design/adr-003-multiprocess-exec-reentry.md`(exec/멀티프로세스 한계).
- HARD CONSTRAINTS(불변): 비root(untrusted_app), public Android API only, W^X-safe, in-process(map+jump + fork/ptrace supervisor; PRoot fallback-only), 단일 APK + 인앱 rootfs, version stamp 불변, **소유 밖 파일은 읽기만**.

---

## 1. 한 줄 결론

**현 프론트는 "프로덕트"가 아니라 "검증 하니스"다 — `MainActivity.onCreate`가 약 40개의 네이티브 probe를 동기 실행하고 그 PASS/FAIL을 `TextView`에 덤프하며, 마지막에 단 하나의 `SurfaceView` + Wayland 컴포지터를 띄워 하드코딩된 게스트(GIMP/foot)를 그린다. UX 최적화의 핵심 동작은 _코드를 갈아엎는 게 아니라 화면을 재배치_하는 것이다: 그 `SurfaceView`+컴포지터 블록을 `RunningSurface`(앱 실행 화면)라는 _독립 Activity_로 떼어내고, 앱 시작점을 probe-덤프가 아니라 `LauncherActivity`(설치된 리눅스 앱 아이콘 그리드)로 바꾼 뒤, `Launcher → Catalog → AppDetail → RunningSurface`의 4-화면 네비게이션을 깐다. 런타임(게스트 실행/생애주기/렌더 콜백)과 UI는 _이미_ 코드 안에 사실상 존재하는 경계(`NativeCommandRunner`의 program-spec 실행 + `nativeWaylandCompositorStart`/`SurfaceHolder.Callback` 렌더 콜백)를 §5-F 인터페이스로 _명문화_해서 만난다. 그 계약만 고정하면 런타임이 device에서 덜 익어도 UI 4트랙은 mock 런타임으로 _지금_ 전부 만들 수 있다.** 미해결의 급소는 단 하나로 수렴한다: **현 컴포지터/`SurfaceView`/네이티브 입력 인젝션이 _프로세스당 1개 Surface_를 _순차_ 사용하도록 짜여 있어(§4-C), "동시 여러 리눅스 앱"은 런타임 측 멀티-surface 작업 없이는 못 준다 — 그래서 v1 멀티앱 정책은 "단일 포그라운드 실행 + 백그라운드 보류(suspend)"로 긋고, 동시-멀티는 device 측정 후 별 ADR로 승계한다.**

---

## 2. 현 프론트의 사실(코드 독해, load-bearing)

UX 설계는 _있는 것_ 위에 얹어야 한다. `MainActivity.kt`(2405줄) 독해 사실:

- **시작점 = probe 하니스.** `onCreate`가 `RootfsInstaller(this).prepareBundledTinyRootfs()`로 인앱 rootfs를 풀고(`RootfsInstaller.kt` L27~), `/data/local/tmp/*-stage.tar` overlay들을 백그라운드 스레드로 스테이징(L59~207), 이어 `nativeCommandRunner.run*`/`nativeAlr*Probe(...)` 수십 개를 **동기**로 돌려(L221~566) 큰 `executionSummary` 문자열(L734~)을 만들고 `ScrollView(TextView)`에 붙인다.
- **유일한 표면 = 단일 `SurfaceView`.** L1138 `SurfaceView(this).apply{…}` 하나뿐. 그 `holder.addCallback(SurfaceHolder.Callback)`의 `surfaceCreated`(L1205~)에서 GPU 큐브 → `nativeWaylandCompositorStart(...)`(L1282) → 게스트 wl_shm 클라이언트 본드/present를 _그 콜백 안에서 순차_ 수행한다. 주석(L1213-1215)이 자인: "단일 Surface는 순차로 사용된다."
- **입력 = 네이티브 인젝션.** `setOnTouchListener`/`setOnKeyListener`/`setOnGenericMotionListener`가 터치·키·스크롤을 `nativeWaylandInjectTouch/Key/Scroll`로 _컴포지터에 직접_ 밀어넣는다(L1146~1203). 즉 입력은 "포커스된 Wayland 클라이언트" 1개를 향한다(L1156-1158 주석).
- **게스트 실행 인터페이스 = program-spec 문자열.** 네이티브 로더 진입점은 전부 `nativeAlrNativeLoaderProbe(pkg, nativeLibDir, filesDir, cacheDir, rootfsName, programSpec)` 형태이고, `programSpec`은 **개행-구분 argv**다(L423 `"/bin/dash\n-c\necho alr-shell-ok"`). 즉 "어떤 게스트를 어떤 인자로 띄울지"는 _이미_ 문자열 한 줄로 표현된다 — UX의 "앱 실행요청"이 여기에 그대로 매핑된다.
- **권한 = 2개뿐.** `AndroidManifest.xml`: `INTERNET`, `ACCESS_NETWORK_STATE`. 런처/카탈로그/실행은 _추가 권한 0_으로 가능(rootfs는 앱-private `filesDir`, overlay는 앱-private). 권한 프롬프트 UX(T4)는 _현재 불필요한 것을 정직히_ 다뤄야 한다(§9).
- **설치 단위 = stage tar overlay.** `RootfsInstaller.extractOverlayTar(tar, rootfsDir)`(L236~)가 base 다운그레이드 가드(M1) 하에 overlay를 풀고 `.{name}-staged-<size>` 마커를 남긴다(`MainActivity` L170-176). **"리눅스 앱 설치" = overlay tar 적용 + 카탈로그 엔트리 등록**이 자연 모델이다.

→ 결론: UX는 (a) `SurfaceView`+컴포지터를 `RunningSurface`로 외화, (b) program-spec 실행을 §5-F `LaunchRequest`로 명문화, (c) overlay 스테이징을 "설치"로 승격, (d) probe-덤프를 `Settings>Diagnostics`로 격리 — 이 4동작이면 큰 코드 재작성 없이 프로덕트 골격이 선다.

---

## 3. 화면 플로우

데스크탑형 "런타임+인앱 카탈로그"의 사용자 여정:

```
        ┌───────────────────────── ALR Runtime (단일 APK) ─────────────────────────┐
        │                                                                          │
첫실행 → [Onboarding] → [Launcher(홈)]  ⇄  [Catalog]  →  [AppDetail]               │
 (1회)    rootfs 준비      설치된 앱        설치가능 앱      설치/제거/실행            │
          진행 표시        아이콘 그리드     탐색·검색       메타+스크린샷             │
                              │  ▲                              │                   │
                       실행 탭 │  │ 복귀/종료                   실행 │                 │
                              ▼  │                              ▼                   │
                          [RunningSurface] ◀──────────────────────                  │
                          풀스크린 컴포지터(SurfaceView)                              │
                          앱 전환(Recents) · 키보드 · 종료                            │
                              │                                                     │
                       설정 ──┴──→ [Settings] (Storage / Permissions / Diagnostics) │
        └──────────────────────────────────────────────────────────────────────────┘
```

1. **Onboarding(첫 실행 1회)** — rootfs 추출(`prepareBundledTinyRootfs`, 현재 `onCreate`에서 동기 실행 중)을 _진행 표시와 함께 백그라운드_로 옮긴다. "리눅스 환경 준비 중 (1/1)" 같은 결정적 진행. 완료 시 Launcher로. (현 코드는 이걸 침묵하며 UI 스레드에서 한다 — 제안 §8-개선-1.)
2. **Launcher(홈)** — 설치된 리눅스 앱을 아이콘 그리드로. 각 타일 = 매니페스트(T2) 엔트리. 탭 → RunningSurface로 실행. 빈 상태 → "카탈로그에서 앱 받기" CTA. 우상단 = Catalog/Settings 진입.
3. **Catalog(탐색)** — 설치 가능한 앱(번들 카탈로그 + apt 기반)을 카드 리스트로. 검색/카테고리. 카드 탭 → AppDetail.
4. **AppDetail** — 앱 메타(이름/설명/크기/출처/스크린샷) + [설치]/[열기]/[제거]. 설치 = overlay tar 스테이징(번들) 또는 apt 트랜잭션(네트워크). 진행/실패를 인라인으로.
5. **RunningSurface(앱 실행)** — **현 `SurfaceView`+Wayland 컴포지터 블록을 그대로 담는 풀스크린 Activity.** 실행 중 앱이 여기 그려진다. 시스템 back/제스처 = "홈으로(백그라운드 보류)" 또는 "종료" 정책(§5). 키보드/입력은 현 인젝션 경로 재사용.
6. **멀티태스킹/복귀/종료** — Launcher↔RunningSurface 토글이 "최근 앱" 역할. v1은 단일 포그라운드(§5). Android Recents에는 RunningSurface가 ALR의 한 task로 보인다.
7. **Settings** — Storage(rootfs/overlay 용량·정리), Permissions(현 2개 + 향후 SAF/알림 토글), **Diagnostics(현 probe-덤프를 여기로 격리** — 개발자/지원용, 일반 사용자엔 숨김).

---

## 4. 컴포넌트 분해 (Activity/Compose 매핑)

현 프로젝트는 **순수 View(코드) 기반**(`SurfaceView`/`TextView`/`LinearLayout`, Compose 미사용). 권고: **신규 일반 UI(Launcher/Catalog/AppDetail/Settings)는 Compose로, `RunningSurface`만 View 기반**(`SurfaceView`는 Compose의 `AndroidView`로 감싸도 되나, 입력 인젝션이 View 콜백에 강결합되어 있어 v1은 순수 Activity+View가 안전).

| UX 컴포넌트 | Android 매핑 | 소유 트랙 | 현 코드와의 관계 |
|---|---|---|---|
| **Onboarding** | `OnboardingActivity` 또는 Launcher 내 상태 | T3(런처) | `prepareBundledTinyRootfs`를 백그라운드로(제안 §8) |
| **Launcher(홈)** | `LauncherActivity`(Compose) + `LAUNCHER` intent-filter | **T3** | 신규. 현 `MainActivity`의 MAIN/LAUNCHER 필터를 _이리로 이전_(제안 §8-개선-2) |
| **Catalog** | `CatalogActivity` 또는 Launcher 내 탭(Compose) | **T3** | 신규. 데이터=매니페스트 카탈로그(T2) |
| **AppDetail** | `AppDetailActivity`/바텀시트(Compose) | **T3** | 신규 |
| **RunningSurface** | `RunningSurfaceActivity`(View) | 본 ADR(경계) / 런타임 통합 | **현 `MainActivity`의 `SurfaceView`+컴포지터 블록(L1138~)을 이리로 이동** |
| **Settings/Diagnostics** | `SettingsActivity`(Compose) | T3/T4 | **현 `executionSummary` TextView 덤프를 Diagnostics 탭으로 격리** |
| **권한 플로우** | runtime permission 헬퍼 + 설명 다이얼로그 | **T4** | 현 2권한 + 향후(§9) |
| **앱 import(SAF)** | `ACTION_OPEN_DOCUMENT` 결과 → overlay 스테이징 | **T5** | `extractOverlayTar` 재사용(읽기 전용 호출) |

### MainActivity 재배치(제안 — 코드 미수정, §8에 패치 형태 기술)
- 현 `MainActivity`는 **두 역할이 한 클래스에 엉켜** 있다: (1) probe 하니스, (2) 컴포지터 호스트. UX는 (2)를 `RunningSurfaceActivity`로 떼고, (1)을 `SettingsActivity > Diagnostics`로 격리한 뒤, **앱 시작점을 `LauncherActivity`로 교체**한다.
- _이 ADR은 `MainActivity.kt`/`AndroidManifest.xml`을 수정하지 않는다._ §8이 "어떤 라인이 어디로 가는지"의 이전 계획을 제시하고, 실제 이동은 통합 세션 또는 런타임 트랙이 수행한다(소유권 §2).

### (C) 단일-Surface 제약 — 멀티앱 정책의 물리적 근거
현 컴포지터는 입력(`nativeWaylandInject*`)을 _포커스된 1 클라이언트_에 보내고, present를 _1 SurfaceView_에 한다. 동시 2 리눅스 앱을 각자 윈도우로 보이게 하려면 런타임 측 (i) per-app surface 라우팅, (ii) 멀티-toplevel 합성/z-order, (iii) 입력 포커스 모델이 필요하다. **GIMP 다이얼로그(child toplevel) 합성은 이미 device-증명**(메모리: ALR-GUI-Android-native-polish — zorder_top 라우팅)됐으나 그건 _한 앱 내 멀티 toplevel_이지 _다중 독립 앱_이 아니다. → §5의 §5-F 계약은 멀티앱을 _표현은 가능_하게 두되(`appId`별 세션), v1 정책은 단일 포그라운드로 못 박는다(§7-정직).

---

## 5. UI↔런타임 §5 인터페이스 계약 (§5-F, 신규)

이 계약이 본 ADR의 _핵심 산출물_이다. UI 4트랙은 이 계약의 **mock 구현**에 대고 지금 개발하고, 런타임(WS-1/WS-3)이 device-ready되면 실 구현으로 교체한다. 계약은 §5-A~E(orchestration-5session-plan)의 후속 슬롯 **§5-F**로 등록.

```kotlin
// §5-F: UI ↔ 런타임 경계. UI는 이 인터페이스만 안다(컴포지터/로더 내부 불투명).
// 소유: 본 ADR 정의 → 런타임 트랙(WS-1) 구현 수락. UI 트랙(T3/T5)은 mock으로 선개발.

/** 앱 실행요청. program-spec(현 MainActivity의 개행-argv L423)을 구조화한 것. */
data class LaunchRequest(
    val appId: String,           // 매니페스트(T2) 안정 id. 세션 키.
    val entryPath: String,       // 게스트 절대경로, 예 "/usr/bin/gimp"
    val args: List<String>,      // argv[1..]
    val env: Map<String, String> = emptyMap(),  // 추가 게스트 env(런타임이 base env에 머지)
    val protocol: Surface = Surface.WAYLAND,     // WAYLAND | X11(Xwayland)
)
enum class Surface { WAYLAND, X11 }

/** 실행 세션 핸들. UI는 이걸로 생애주기를 제어. */
interface AppSession {
    val appId: String
    val state: StateFlow<SessionState>   // 관찰 가능한 상태
    fun bindSurface(holder: SurfaceHolder)   // RunningSurface가 자기 SurfaceView를 제공
    fun unbindSurface()                      // 백그라운드 전환 시
    fun requestForeground()                  // 포커스/입력 라우팅 획득
    fun requestBackground()                  // suspend(가능 시) 또는 보류
    fun stop(reason: StopReason)             // 정상 종료 요청
}

enum class SessionState { STARTING, RENDERING, BACKGROUND, STOPPING, STOPPED, CRASHED }
enum class StopReason { USER, SYSTEM_MEMORY, RUNTIME_ERROR }

/** 런타임 진입점. 현 nativeWaylandCompositorStart + nativeAlrNativeLoaderProbe를 감싼다. */
interface AlrRuntime {
    /** 세션 시작. 즉시 핸들 반환, 상태는 콜백/StateFlow로. */
    fun launch(req: LaunchRequest): AppSession
    /** 현재 활성 세션들(멀티앱 표현; v1은 0..1 RENDERING). */
    val sessions: StateFlow<List<AppSession>>
    /** 설치 트랜잭션(overlay/apt). UI는 진행률만 본다. */
    fun install(spec: InstallSpec): Flow<InstallProgress>
    fun uninstall(appId: String): Flow<InstallProgress>
    /** 설치된 앱 목록(매니페스트 T2가 채움). */
    fun installedApps(): List<AppManifestEntry>
    fun catalog(): Flow<List<CatalogEntry>>
}

// 상태 콜백 계약(요구사항 (3)): 렌더 시작/종료/크래시.
//  - STARTING → RENDERING: 첫 프레임이 SurfaceView에 present된 시점(현 코드의
//    surfaceReport "frames rendered>0" / wlFramePresented L1313에 대응).
//  - RENDERING → STOPPED: 게스트 프로세스 exit=0(현 NativeCommandResult.exitCode).
//  - * → CRASHED: 게스트 비정상 종료/SIGSEGV(현 supervisor가 SIGSYS 외 시그널 포착,
//    ADR-003 §2 EVENT 처리). UI는 "앱이 종료됨" + 재시작/로그 보기.
```

### 계약↔현 코드 매핑(구현 시 무엇을 감싸는가)
| §5-F 요소 | 현 코드 백킹 |
|---|---|
| `LaunchRequest.entryPath+args` | `nativeAlrNativeLoaderProbe(...,"path\narg1\narg2")`(L423 개행-argv) |
| `AppSession.bindSurface` | `SurfaceHolder.Callback.surfaceCreated`(L1205) + `nativeWaylandCompositorStart`(L1282) |
| `state RENDERING` | `surfaceReport` "frames rendered>0"(L1229) / `wlFramePresented`(L1313) |
| 입력 라우팅(`requestForeground`) | `nativeWaylandInject*`가 포커스 클라이언트로(L1146~) |
| `state STOPPED/CRASHED` | `NativeCommandResult.exitCode`(`NativeCommandRunner` L230~) / supervisor 시그널 |
| `install(overlay)` | `RootfsInstaller.extractOverlayTar`(L236) + `.{name}-staged-<size>` 마커 |
| `install(apt)` | `runProotRootfsAptGet*` 경로(`NativeCommandRunner` L75~, 트랜잭션 래핑 필요) |

### Mock 런타임(UI 선개발용 — UX 트랙 소유, host 검증 가능)
- `FakeAlrRuntime : AlrRuntime` — `launch`가 타이머로 `STARTING→RENDERING`을 흘리고, `bindSurface`가 `SurfaceView`에 단색/테스트 패턴을 그린다. `install`은 가짜 진행률. `catalog/installedApps`는 매니페스트(T2) 파서가 읽은 JSON 픽스처.
- 이 mock의 _순수 로직 부분_(매니페스트 파싱→`AppManifestEntry`, 설치 상태 전이, 카탈로그 필터/검색)은 **host pytest로 검증 가능**(T2/T3가 소유). 본 ADR은 문서-only(host_verified=false)이고, 검증은 T2/T3 트랙이 자기 파서/모델로 green을 낸다.

---

## 6. 멀티앱 정책 (v1 결정 + 승계)

| 정책 | v1(지금) | 근거 / 승계 |
|---|---|---|
| 동시 실행 | **단일 포그라운드** 1앱(RENDERING) | §4-C 단일-Surface·단일-입력-포커스. 동시-멀티는 런타임 멀티-surface 작업 필요 → 별 ADR |
| 백그라운드 | **보류(suspend) 또는 정지** | 게스트 suspend 가능성은 device-측정(SIGSTOP/cgroup-free 환경) — §7 |
| 앱 전환 | Launcher 경유(홈→다른 앱) | 동시 2 RENDERING 없음 → 전환 시 이전 앱 background |
| 한 앱 내 멀티창 | **지원**(이미 device-증명) | GIMP child toplevel 합성(메모리: zorder_top) — 새 작업 아님 |
| 장기 목표 | 동시-멀티(데스크탑 멀티윈도우) | §5-F가 `sessions: List`로 _표현은 미리_ 열어둠. 실현은 WS-3 멀티윈도우(M3) device 후 |

→ `§5-F`의 `AppSession`/`sessions:List`는 멀티를 _표현_하되, v1 `AlrRuntime` 구현이 "RENDERING은 항상 0..1"을 _불변식_으로 강제. UI는 처음부터 리스트를 다루게 짜서, 멀티 해금이 UI 재작성이 아니라 불변식 완화가 되게 한다.

---

## 7. 4트랙(매니페스트/런처/권한/SAF) 데이터·제어 흐름

본 ADR이 SSOT인 큰 그림. 각 트랙은 자기 소유 파일만 만들고 이 흐름으로 만난다.

```
   [T2 매니페스트 스키마]                 [T5 SAF 임포트]
   AppManifestEntry/CatalogEntry          ACTION_OPEN_DOCUMENT → .tar
   (JSON 파서, host-검증)                   → InstallSpec(overlay)
        │  installedApps()/catalog()             │
        ▼                                         ▼
   ┌──────────────────── AlrRuntime (§5-F) ───────────────────┐
   │  installedApps()  catalog()  install()  launch()  sessions │
   └───┬───────────────┬───────────────┬──────────┬───────────┘
       │ 목록           │ 카탈로그       │ 진행률    │ 세션상태
       ▼               ▼               ▼          ▼
   [T3 Launcher]   [T3 Catalog]   [T3 AppDetail/진행]  [RunningSurface]
       │ 탭 실행 → LaunchRequest ───────────────────────▶ bindSurface
       │
   [T4 권한] ── 설치/네트워크/SAF 시점에 필요한 권한만 just-in-time 요청
```

- **제어 흐름(설치)**: T3 AppDetail [설치] → `AlrRuntime.install(InstallSpec)` → (번들이면 `extractOverlayTar`, apt면 트랜잭션) → `InstallProgress` Flow → 완료 시 T2 매니페스트에 엔트리 등록 → Launcher 그리드 갱신.
- **제어 흐름(실행)**: T3 Launcher 탭 → `LaunchRequest(appId, entryPath, args)` → `AlrRuntime.launch` → `AppSession` → `RunningSurfaceActivity` 띄움 → `bindSurface(holder)` → `STARTING→RENDERING` → 사용자 상호작용(입력 인젝션) → back → `requestBackground`/`stop`.
- **데이터 흐름(매니페스트)**: T2가 `AppManifestEntry`(id/name/icon/entryPath/args/protocol/source)와 `CatalogEntry`(설치가능 메타)의 JSON 스키마+파서를 소유. RunningSurface의 `entryPath`/`args`/`protocol`은 _전부 매니페스트에서_ 온다(하드코딩 GIMP 경로 제거).
- **SAF(T5)**: 사용자가 외부 `.tar`(또는 미래의 `.alrpkg`)를 `ACTION_OPEN_DOCUMENT`로 고름 → 앱-private로 복사 → `InstallSpec(overlay=그 tar)` → 같은 `install()` 경로. _SAF는 "앱 추가"의 한 source일 뿐_, 설치 파이프라인은 카탈로그와 공유.

---

## 8. MainActivity 재배치 제안 (코드 미수정 — 이전 계획)

본 ADR은 `MainActivity.kt`/`AndroidManifest.xml`/`RootfsInstaller.kt`를 _건드리지 않는다_. 아래는 통합 세션/런타임 트랙이 수행할 이전 계획(소유권 §2). 각 항목은 _최소 침습_(블록 이동·필터 이전)이고 동작 로직은 보존.

- **개선-1(Onboarding 비차단화)**: `prepareBundledTinyRootfs()`(현 `onCreate` L42 동기)를 워커로 옮기고 진행 콜백 추가. 현재 UI 스레드에서 rootfs 추출이 끝날 때까지 침묵(콜드스타트 ANR 위험). 단 이건 _런타임/통합 트랙_ 작업이지 UX 트랙이 `MainActivity`를 고치는 게 아님.
- **개선-2(시작점 교체)**: `AndroidManifest.xml`의 MAIN/LAUNCHER intent-filter를 `MainActivity`→`LauncherActivity`로 이전(L8-15). `MainActivity`는 `exported=false`의 내부 Diagnostics 호스트로 남기거나 `SettingsActivity`에 흡수. **이 매니페스트 편집은 통합 세션 소유**(UX 트랙은 제안만).
- **개선-3(RunningSurface 추출)**: `SurfaceView`+`SurfaceHolder.Callback`+`nativeWaylandCompositorStart`+입력 인젝션 블록(L1138~끝)을 `RunningSurfaceActivity`로 _그대로_ 이동. 진입 시 `LaunchRequest`를 Intent extra로 받아 하드코딩 GIMP/foot 대신 그 program-spec을 실행.
- **개선-4(Diagnostics 격리)**: `executionSummary` 빌드(L221~566, L734~)와 `TextView` 덤프를 `SettingsActivity > Diagnostics`로. 일반 사용자 경로에서 probe는 안 돈다(콜드스타트 가속 + UX 정돈).
- **개선-5(stage-tar 스테이징의 명시화)**: 현 `/data/local/tmp/*-stage.tar` 자동 스테이징(L59~207)은 _개발 편의_(adb push)다. 프로덕트에선 "설치"가 카탈로그/SAF에서 와야 하므로, 이 자동 경로는 Diagnostics/dev 모드로 게이트(런타임 트랙).

→ 이 5개는 전부 _기존 로직 보존·위치 이동_이라 device 회귀 위험이 낮다. 그러나 **UX 트랙은 이 중 어느 것도 직접 커밋하지 않는다** — 제안으로 남기고, 통합 세션이 device 게이트(§10) 하에 수행한다.

---

## 9. 권한 UX (T4 SSOT 입력 — 정직히 "지금 거의 필요 없음")

- **현 사실**: 설치(overlay·apt 네트워크 제외)·실행·렌더는 _추가 권한 0_. rootfs는 `filesDir`(앱-private), overlay도 앱-private. 카탈로그 네트워크 다운로드만 `INTERNET`(이미 있음).
- **just-in-time만**: 권한은 _그 기능을 처음 쓸 때_ 설명 다이얼로그와 함께. 후보:
  - `INTERNET`(이미 선언, 런타임 권한 아님) — 카탈로그/apt 다운로드. 프롬프트 불필요.
  - **SAF(T5)** — `ACTION_OPEN_DOCUMENT`는 _권한 선언 불요_(시스템 picker가 위임). T4는 "SAF는 권한 프롬프트가 아니라 picker UX"임을 정직히.
  - **알림(POST_NOTIFICATIONS, Android 13+)** — 설치/실행 진행을 알림으로 보일 때만. v1 옵션.
  - **저장소 광범위(MANAGE_EXTERNAL_STORAGE)** — `MainActivity`가 _이미 "선언 안 됨"을 체크_(L570 `broadStoragePermissionDeclared`)하며 **안 쓰는 게 정책**. T4는 이걸 _거부 목록_으로 문서화(비root·최소권한 원칙).
- → **T4의 진짜 산출물은 "권한 프롬프트 난사"가 아니라 "왜 ALR이 권한을 거의 안 요구하는지"의 설명 UX + 미래 권한의 just-in-time 패턴**이다. 이게 비root/최소권한이라는 제품 차별점과 정합.

---

## 10. device 게이트 + DEVICE-REQ 마커

UI 골격·매니페스트 파서·mock 런타임은 host에서 끝나지만, _실 런타임 결선_은 device가 답한다.

### M-UX-runningsurface (1순위 — RunningSurface 추출이 무회귀)
- **무엇**: 개선-3(컴포지터 블록을 별 Activity로 이동) 후, 기존 GIMP/foot 렌더·입력이 _그대로_ 동작하는지. UX 재배치가 device 동작을 깨지 않음을 확정.
- **DEVICE-REQ**: `DEVICE-REQ: ALR-M-UX-runningsurface — SM-X236N (am force-stop first), Launcher→앱 탭→RunningSurface에서 GIMP File>New>OK 캔버스 + 브러시 1획; gate = 렌더 present AND 터치 입력 도달(현 메모리 v111 동등). probe-덤프는 Diagnostics에만.`

### M-UX-launch-contract (2순위 — §5-F 실 결선)
- **무엇**: `AlrRuntime.launch(LaunchRequest)` 실 구현이 program-spec 경로(L423)를 감싸 `STARTING→RENDERING` 콜백을 정확한 시점(첫 present)에 낸다.
- **DEVICE-REQ**: `DEVICE-REQ: ALR-M-UX-launch — SM-X236N, LaunchRequest(appId=gimp, entryPath=/usr/bin/gimp)로 실행; gate = state RENDERING이 첫 프레임 present와 ±1프레임 내 발화 AND exit=0이 STOPPED로.`

### M-UX-install-overlay (3순위 — 설치 파이프라인)
- **무엇**: AppDetail [설치]가 `extractOverlayTar`를 호출해 한 앱(예: foot)을 카탈로그→Launcher 그리드로 올리고 실행까지.
- **DEVICE-REQ**: `DEVICE-REQ: ALR-M-UX-install — SM-X236N, Catalog>foot>설치→Launcher에 타일 등장→실행 렌더; gate = overlay extracted>0(가드 skip 정상) AND 실행 RENDERING.`

(주의: M-UX-*는 _UI 결선_ 게이트로, 런타임 내부(supervisor/컴포지터)는 ADR-001/002/003·WS 게이트가 별도 담당. 멀티앱 동시-실행은 본 ADR _범위 밖_ — §6대로 별 ADR로 승계, device-REQ 미발급.)

---

## 11. host 프로토타입 가능범위 (device 불요)

본 ADR은 문서-only이나, 의존 트랙이 _지금_ host-검증할 수 있는 순수 로직을 짚는다(각 트랙 소유):
- **(T2)** 매니페스트/카탈로그 JSON 파서 → `AppManifestEntry`/`CatalogEntry` 모델. pytest로 round-trip·검색·필터·버전정렬·악성입력 거부.
- **(T3)** 설치 상태 머신(`NOT_INSTALLED→INSTALLING→INSTALLED→…`) + 카탈로그 필터/정렬 + Launcher 그리드 정렬 규칙 = 순수 함수. pytest로.
- **(T5)** SAF로 받은 tar의 사전검증(엔트리 traversal/`..`/절대경로/디바이스노드 거부)을 `RootfsInstaller.validateTarEntry`(L393) 규칙의 _순수 미러_로 재구현·테스트(실제 추출은 런타임).
- **(공통/mock)** `FakeAlrRuntime`의 상태 전이(STARTING→RENDERING→STOPPED, install 진행률 단조성, sessions 0..1 불변식)를 pytest 모델로.

→ **단 host(darwin)는 실 컴포지터/seccomp/Mali 거동 불가**(§10). UI 골격의 _상호작용_·_렌더_는 device-only. 본 ADR이 host로 닫는 건 _계약의 형태와 순수 로직_이지 _런타임 효과_가 아니다.

---

## 12. 정직 섹션

**지금 미리 가능 vs 런타임 의존:**
- **미리 가능(런타임 무관)**: 4-화면 네비게이션, Launcher/Catalog/AppDetail/Settings UI 골격, §5-F 계약 정의, mock 런타임, 매니페스트/카탈로그 파서(T2), 설치 상태머신(T3), SAF 사전검증(T5) — 전부 host pytest + mock으로 _오늘_ 만든다.
- **런타임 의존(device-only)**: 실 `launch`→첫-프레임 RENDERING 타이밍, 입력 인젝션 도달, overlay 설치 후 실 실행, 게스트 suspend(백그라운드) 가부, CRASHED 신호 정확도. 전부 §10 DEVICE-REQ로 격리.

**자가 적대검증(핵심 주장 1개 자기공격)**: §1 주장 — "코드 재작성 없이 화면 재배치로 프로덕트가 선다" — 를 공격한다. _constraint는 안 깨진다_(새 권한 0, 새 네이티브 진입점 0; `RunningSurface`는 기존 `SurfaceView`+`nativeWaylandCompositorStart`를 _그대로_ 옮길 뿐, 비root/W^X/in-process 불변). _그러나 "재배치만으로 충분"의 급소는 §4-C다_: 현 컴포지터·입력 인젝션이 _프로세스 1개·Surface 1개·포커스 1개_ 가정 위에 짜여 있어, "데스크탑형 멀티윈도우"라는 제품 비전의 _완성형_은 재배치가 아니라 **런타임 멀티-surface 신작업**을 요구한다. 즉 _v1(단일 포그라운드)은 재배치로 100% 도달하지만, 멀티앱 동시 데스크탑은 UI 트랙만으로 못 닫는다_ — 그래서 §6이 그걸 v1 밖으로 정직히 긋고 §5-F가 표현만 미리 연다. 또 하나: §8의 MainActivity 이전(특히 MAIN/LAUNCHER 필터 교체·rootfs 비차단화)은 _UX 트랙 소유 밖_이라, 통합/런타임 트랙이 수행하기 전까지 UX 트랙의 Launcher는 "설치는 됐으나 시작점이 아직 probe 하니스"인 _과도기_를 산다. 이건 결함이 아니라 _소유권 분리의 정직한 비용_이고, §10 M-UX-runningsurface device 게이트가 그 이전의 무회귀를 확정하는 순간 해소된다.

**constraint_violations**: 없음(확정). 새 syscall/권한/ptrace op/execmem 매핑 0. RunningSurface는 기존 표면·진입점 재배치. 깨질 위험이 있는 것(멀티-surface, MainActivity 이전, suspend 거동)은 전부 §6/§8/§10에 _범위 밖_ 또는 _device-only_로 격리.

---

관련 파일(절대경로):
- 본 ADR: `/Users/naen/Documents/alr-product-ux/docs/design/adr-004-inapp-catalog-ux.md`
- 현 프론트(읽기 전용 — 본 ADR 미수정): `/Users/naen/Documents/alr-product-ux/app/src/main/AndroidManifest.xml`, `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`(SurfaceView/컴포지터 L1138~, program-spec 실행 L337~/L423, executionSummary L734~), `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/RootfsInstaller.kt`(extractOverlayTar L236, validateTarEntry L393), `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/RootfsInstallPlan.kt`, `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt`(program-spec/argv L155~, apt 경로 L72~)
- 선행/계약: `/Users/naen/Documents/alr-product-ux/docs/research/orchestration-5session-plan.md` §5(레이어 계약)·§5-E(stage tar 규약 — 본 ADR이 §5-F를 후속 등록), `/Users/naen/Documents/alr-product-ux/docs/design/adr-003-multiprocess-exec-reentry.md`(exec/멀티프로세스 한계), `/Users/naen/Documents/alr-product-ux/docs/design/adr-001-syscall-overhead-user-notif.md`
- 의존 UX 트랙(본 ADR이 SSOT): T2 매니페스트 스키마, T3 런처/카탈로그 스켈레톤, T4 권한 플로우, T5 SAF 임포트
