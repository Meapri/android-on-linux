/*
 * AlrRuntime — ADR-004 §5-F "UI ↔ 런타임 경계" 의 Kotlin 인터페이스.
 *
 * UI 4트랙(Launcher/Catalog/AppDetail/Settings)은 *이 인터페이스만* 안다 —
 * 컴포지터/네이티브 로더 내부는 불투명. UI 트랙은 FakeAlrRuntime(host/Preview 구동)에
 * 대고 지금 개발하고, 통합/런타임 트랙(WS-1)이 nativeWaylandCompositorStart +
 * nativeAlrNativeLoaderProbe 를 감싼 실 구현으로 device-ready 시 교체한다.
 *
 * 단방향 데이터흐름: 상태는 전부 StateFlow/Flow 로 흐르고(installedApps/sessions/
 * SessionState/InstallProgress), UI 는 명령(launch/install/uninstall/요청 메서드)만
 * 내려보낸다. INV-1~3(단일 포그라운드 불변식)은 §5-F 상태기계로 박혀 있다.
 *
 * 빌드 미통합: Compose 의존성은 통합 세션이 build.gradle 에 추가(이 파일은 Compose
 * 비의존). kotlinx-coroutines(StateFlow/Flow)는 안드로이드 표준 의존이라 가정.
 * build.gradle·MainActivity.kt·AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: 기반 트랙(runtime/ 신규). ADR-004 §5-F 가 SSOT — 이 인터페이스가 그 계약의
 * 코틀린 표현이고, WS-1 이 구현 측에서 수락한다.
 */
package dev.chanwoo.androlinux.runtime

import android.view.SurfaceHolder
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow

// --------------------------------------------------------------------------- //
// 실행요청 / 세션 상태 (ADR-004 §5-F)
// --------------------------------------------------------------------------- //

/** 표면 프로토콜 — WAYLAND(native) | X11(Xwayland 경유). */
enum class SurfaceProtocol {
    WAYLAND,
    X11,
}

/**
 * 앱 실행요청 — 현 MainActivity 의 program-spec(개행-argv, L423)을 구조화한 것.
 *
 * `appId` 는 매니페스트 안정 id(= 세션 키). `entryPath`/`args` 는 InstalledApp.entry
 * 에서 온다(하드코딩 제거). `env` 는 런타임이 base env 에 머지하는 추가 게스트 환경변수.
 */
data class LaunchRequest(
    val appId: String,
    val entryPath: String,
    val args: List<String> = emptyList(),
    val env: Map<String, String> = emptyMap(),
    val protocol: SurfaceProtocol = SurfaceProtocol.WAYLAND,
)

/**
 * 세션 상태 — ADR-004 §5-F 상태기계.
 *
 *   STARTING  : launch() 직후, 첫 프레임 present 전.
 *   RENDERING : 첫 프레임이 SurfaceView 에 present 된 시점(현 wlFramePresented 대응).
 *               ★ INV-1: 전 세션 통틀어 RENDERING 은 항상 0개 또는 1개.
 *   BACKGROUND: requestBackground()/unbindSurface() 로 포그라운드 양도(suspend or 보류).
 *   STOPPING  : stop() 또는 exit 진행 중.
 *   STOPPED   : 게스트 정상 종료(exit=0).
 *   CRASHED   : 비정상 종료(SIGSEGV 등) — UI 는 "앱 종료됨" + 재시작/로그.
 */
enum class SessionState {
    STARTING,
    RENDERING,
    BACKGROUND,
    STOPPING,
    STOPPED,
    CRASHED,
}

/** stop() 사유 — 사용자/시스템 메모리 압력/런타임 오류. */
enum class StopReason {
    USER,
    SYSTEM_MEMORY,
    RUNTIME_ERROR,
}

/**
 * 실행 세션 핸들 — RunningSurface(View)가 이걸로 생애주기를 제어한다.
 *
 * `state` 를 관찰해 STARTING→RENDERING 전이에 SurfaceView 를 띄우고, CRASHED/STOPPED
 * 에 종료 UI 를 보인다. bindSurface 가 컴포지터에 SurfaceHolder 를 넘기는 지점이다.
 */
interface AppSession {
    val appId: String
    val state: StateFlow<SessionState>

    /** RunningSurface 가 자기 SurfaceView 의 holder 를 제공(컴포지터 본드 + present). */
    fun bindSurface(holder: SurfaceHolder)

    /** 백그라운드 전환 시 표면 해제(present 중단). */
    fun unbindSurface()

    /** 포커스/입력 라우팅 획득 — RENDERING 진입. INV-2 에 따라 런타임이 기존
     *  RENDERING 세션을 먼저 BACKGROUND/STOPPING 으로 양도한 뒤 이 세션을 올린다. */
    fun requestForeground()

    /** suspend(가능 시) 또는 보류 — BACKGROUND 로 전이(D5 정책은 device 측정 후 확정). */
    fun requestBackground()

    /** 정상 종료 요청 — STOPPING 경유 STOPPED. */
    fun stop(reason: StopReason)
}

// --------------------------------------------------------------------------- //
// 런타임 진입점 (ADR-004 §5-F AlrRuntime)
// --------------------------------------------------------------------------- //

/**
 * UI ↔ 런타임 진입점 — 현 nativeWaylandCompositorStart + nativeAlrNativeLoaderProbe 를
 * 감싼다(실 구현). UI 는 이 인터페이스만 의존.
 *
 * ★ 단일 포그라운드 불변식(ADR-004 §0-D1, §5-F INV-1~3) — 구현이 강제:
 *   INV-1: ∀ 시점, RENDERING 상태 세션 수 ≤ 1.
 *   INV-2: 새 세션이 RENDERING 진입 전, 기존 RENDERING 세션은 반드시 BACKGROUND
 *          (suspend 가능 시) 또는 STOPPING 으로 *먼저* 원자적으로 전이(포커스·입력·
 *          present 표면을 새 세션으로 양도). launch()/requestForeground() 가 보장.
 *   INV-3: 비-RENDERING(BACKGROUND/STARTING/STOPPING/STOPPED/CRASHED) 세션 수에는
 *          상한 없음 — 멀티앱 "표현"은 sessions:List 로 미리 열림.
 */
interface AlrRuntime {
    /** 설치된 앱 목록(매니페스트 T2 가 채움). StateFlow — 설치/제거 시 갱신. */
    val installedApps: StateFlow<List<InstalledApp>>

    /** 활성 세션들(멀티앱 표현; v1 은 RENDERING 0..1, 그 외 무상한). */
    val sessions: StateFlow<List<AppSession>>

    /** 설치 가능한 앱 카탈로그(번들 + apt 인덱스). 갱신 가능 → Flow. */
    fun catalog(): Flow<List<CatalogApp>>

    /**
     * 세션 시작 — 즉시 핸들 반환, 상태는 AppSession.state(StateFlow)로.
     * INV-2 에 따라 기존 RENDERING 세션을 먼저 양도한 뒤 이 세션을 RENDERING 으로.
     */
    fun launch(req: LaunchRequest): AppSession

    /** 설치 트랜잭션(stage-tar overlay v1 / 인-게스트 apt v2). UI 는 진행률만 본다. */
    fun install(appId: String): Flow<InstallProgress>

    /** 제거 트랜잭션 — overlay/마커 제거. 진행률 Flow. */
    fun uninstall(appId: String): Flow<InstallProgress>
}
