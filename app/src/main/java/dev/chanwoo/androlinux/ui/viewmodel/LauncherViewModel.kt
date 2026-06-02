/*
 * LauncherViewModel — 런처 화면(MVVM)의 ViewModel 레이어.
 *
 * ADR-004 §5-F 단방향 데이터흐름의 *UI 측 절반* 을 MVVM 으로 형식화한다:
 *   AlrRuntime(StateFlow: installedApps / sessions) → LauncherViewModel 이 구독·가공 →
 *   LauncherUiState(StateFlow) 하나로 화면에 노출. 화면(LauncherRoute)은 이 UiState 를
 *   collectAsState 로 받아 *기존 Route 시그니처 그대로* 그린다(화면은 ViewModel 을 모름 —
 *   AlrApp 이 UiState 를 풀어 Route 인자로 넘긴다). 이벤트(검색질의/카테고리필터/실행)는
 *   ViewModel 함수로 위로 흐른다. 화면 → ViewModel(이벤트), ViewModel → 화면(상태): 단방향.
 *
 * ★ 화면 시그니처 불변: LauncherRoute(installedApps, onLaunch, onOpenCatalog, onOpenSettings,
 *   onOpenAppDetail) 는 한 글자도 안 바뀐다. 검색어/선택 카테고리 같은 *지역 UI 상태* 는
 *   현재 LauncherScreen 이 remember 로 들고 있어 그대로 둔다 — ViewModel 은 그 위에서 데이터
 *   로딩/세션(최근앱·크래시) 상태와 빈 상태/오프라인을 관장한다(중복 강제 아님, 화면 보존 우선).
 *
 * INV-1~3 불변: ViewModel 은 세션 상태를 *읽기만* 한다(launch 만 명령). RENDERING ≤ 1 강제는
 *   런타임(FakeAlrRuntime.promoteToForeground 임계구역)이 소유 — ViewModel 은 그 결과(sessions
 *   StateFlow)를 관찰해 "현재 포그라운드 / 최근앱 / 크래시" 를 파생할 뿐 불변식을 건드리지 않는다.
 *
 * 빌드 미통합: Compose/lifecycle 의존성(androidx.lifecycle:lifecycle-viewmodel-ktx 의
 *   ViewModel/viewModelScope, kotlinx-coroutines)은 통합 세션(D2)이 build.gradle 에 추가한다.
 *   본 파일은 그 전까지 import 미해소가 의도된 상태(소스 골격). build.gradle·MainActivity.kt·
 *   AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: B 트랙(ui/viewmodel/ 신규). 기반 인터페이스(runtime/AlrRuntime.kt §5-F)를 정확히 따른다.
 */
package dev.chanwoo.androlinux.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.AppSession
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchRequest
import dev.chanwoo.androlinux.runtime.SessionState
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn

// --------------------------------------------------------------------------- //
// UiState (데이터 + 로딩 + 빈/오프라인 + 세션 파생)
// --------------------------------------------------------------------------- //

/**
 * 런처 화면의 단일 UiState — ViewModel 이 노출하고 AlrApp 이 풀어서 LauncherRoute 인자로 넘긴다.
 *
 * @property installedApps 설치된 앱(런처 그리드 SSOT). LauncherRoute 의 installedApps 로 직결.
 * @property foregroundAppId 현재 RENDERING 인 세션의 appId(없으면 null) — 그리드에서 "실행 중"
 *   배지에 쓸 수 있다(화면이 원할 때만; 시그니처 불변이라 v1 화면은 미사용, 통합 시 활용 가능).
 * @property recentAppIds 최근 사용 앱(가장 최근이 앞) — BACKGROUND/방금 종료 포함 D5 표현. INV-3
 *   에 따라 비-RENDERING 세션은 무상한이라 "최근앱"으로 안전히 표현된다.
 * @property crashedAppId 마지막으로 CRASHED 된 앱(있으면) — 화면이 "앱 종료됨" 스낵바를 띄울 훅.
 * @property isLoading 첫 installedApps 방출 전(초기 로딩). 비면 false.
 * @property isEmpty 설치 앱 0개(빈 상태 CTA — 화면의 EmptyState 와 정합).
 */
data class LauncherUiState(
    val installedApps: List<InstalledApp> = emptyList(),
    val foregroundAppId: String? = null,
    val recentAppIds: List<String> = emptyList(),
    val crashedAppId: String? = null,
    val isLoading: Boolean = true,
    val isEmpty: Boolean = false,
) {
    /** 현재 포그라운드 세션이 있는지(D1 단일 포그라운드 — 0 또는 1). */
    val hasForeground: Boolean get() = foregroundAppId != null
}

// --------------------------------------------------------------------------- //
// ViewModel
// --------------------------------------------------------------------------- //

/**
 * 런처 ViewModel — installedApps + sessions 를 combine 해 LauncherUiState 로 가공.
 *
 * 데이터는 전부 runtime StateFlow 에서 흘러오고(단방향), 사용자 명령은 launch() 하나뿐이다
 * (검색/필터는 화면-로컬 UI 상태라 ViewModel 이 강제하지 않음 — 화면 보존). launch 는 INV-2 에
 * 따라 런타임이 기존 포그라운드를 먼저 양도하므로, ViewModel 은 LaunchRequest 만 만들어 내려보낸다.
 *
 * @param runtime §5-F 런타임(인터페이스만 의존 — Fake/실구현 무관).
 */
class LauncherViewModel(
    private val runtime: AlrRuntime,
) : ViewModel() {

    val uiState: StateFlow<LauncherUiState> =
        combine(runtime.installedApps, runtime.sessions) { installed, sessions ->
            buildState(installed, sessions)
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS),
            initialValue = LauncherUiState(isLoading = true),
        )

    // ----------------------------------------------------------------------- //
    // 이벤트(화면 → ViewModel) — 단방향. 명령만 내려보낸다.
    // ----------------------------------------------------------------------- //

    /**
     * 앱 실행 — InstalledApp.entry 로 LaunchRequest 합성 후 launch. RENDERING 진입(INV-2 양도)은
     * 런타임이 임계구역에서 처리하므로 여기서는 핸들을 받아 흘려보낸다(ViewModel 은 보관 불필요 —
     * sessions StateFlow 가 SSOT). AlrApp 이 onLaunch 콜백에서 호출하거나, 통합 측이 RunningSurface
     * 결선과 함께 호출할 수 있다(둘 다 같은 launch 경로).
     */
    fun launch(app: InstalledApp): AppSession =
        runtime.launch(app.toLauncherRequest())

    /** 최근앱(또는 그리드)에서 appId 로 바로 실행 — installedApps 에서 찾아 launch. */
    fun launchById(appId: String): AppSession? {
        val app = uiState.value.installedApps.firstOrNull { it.appId == appId } ?: return null
        return launch(app)
    }

    private companion object {
        /** 구독자 없을 때 upstream 유지 시간(회전/일시 백그라운드 동안 재구독 비용 회피). */
        const val STOP_TIMEOUT_MS = 5_000L
    }
}

// --------------------------------------------------------------------------- //
// 순수 가공 (테스트 가능 — runtime/Android 비의존)
// --------------------------------------------------------------------------- //

/**
 * installedApps + sessions → LauncherUiState. INV-1(RENDERING ≤ 1)을 *읽어* foreground 를 뽑고,
 * 비-RENDERING 살아있는 세션을 최근앱으로, CRASHED 를 크래시 훅으로 파생한다.
 *
 * foreground 는 RENDERING 세션의 appId(INV-1 보장으로 0/1개 — firstOrNull 안전).
 * recents 는 RENDERING 을 제외한(=BACKGROUND 등) 세션 + 그 외 앱 순서를 유지하되 중복 제거.
 */
internal fun buildState(
    installed: List<InstalledApp>,
    sessions: List<AppSession>,
): LauncherUiState {
    val foreground = sessions.firstOrNull { it.state.value == SessionState.RENDERING }
    val crashed = sessions.firstOrNull { it.state.value == SessionState.CRASHED }
    // 최근앱: 포그라운드가 아닌, 살아있는(STOPPED/CRASHED 아님) 세션 — BACKGROUND/STARTING 등.
    val recents = sessions
        .filter { it !== foreground && it.state.value == SessionState.BACKGROUND }
        .map { it.appId }
        .distinct()
    return LauncherUiState(
        installedApps = installed,
        foregroundAppId = foreground?.appId,
        recentAppIds = recents,
        crashedAppId = crashed?.appId,
        isLoading = false,
        isEmpty = installed.isEmpty(),
    )
}

/** InstalledApp.entry → LaunchRequest(런처 실행 경로). AlrApp.toLaunchRequest 와 같은 규약. */
internal fun InstalledApp.toLauncherRequest(): LaunchRequest = LaunchRequest(
    appId = appId,
    entryPath = entry.target,
    args = entry.args,
)
