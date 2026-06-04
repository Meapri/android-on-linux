/*
 * AppDetailViewModel — 앱 상세 화면(MVVM)의 ViewModel 레이어.
 *
 * ADR-004 §5-F 단방향: appId 로 catalog()/installedApps 를 구독해 한 앱의 메타(설치/미설치)와
 * 설치·제거 트랜잭션 진행 상태를 AppDetailUiState(StateFlow)로 노출한다. 화면(AppDetailRoute)은
 * 기존 시그니처(appId, catalogApp, installedApp, installProgress(), uninstallProgress(), onOpen,
 * onBack) 그대로 그린다 — AlrApp 이 UiState 를 풀어 그 인자로 넘긴다.
 *
 * ★ 화면 시그니처 불변: AppDetailRoute 는 한 글자도 안 바뀐다. 현재 AppDetailScreen 은
 *   installProgress/uninstallProgress 지연 Flow 람다를 받아 *화면-로컬* produceState 로 구독한다.
 *   ViewModel 을 끼우면 AlrApp 이 그 두 람다를 `vm::installFlow`/`vm::uninstallFlow` 로 주입한다 —
 *   화면은 여전히 같은 시그니처로 동작하고, ViewModel 은 *설치 큐(동시 1)·재시도·CRASHED 표현*
 *   같은 상위 상태를 UiState 로 추가 노출한다(통합 시 활용; 기존 화면 동작은 그대로).
 *
 * 설치/제거 Flow 의 소유: 화면이 클릭 시 한 번 만드는 *지연 Flow 생성자* 를 ViewModel 이 제공한다
 *   (installFlow()/uninstallFlow()). 같은 install Flow 를 구독해 ViewModel 도 UiState(transaction)
 *   를 갱신하므로, 화면-로컬 표시와 ViewModel 표시가 같은 진행률로 수렴한다(중복 부작용 없음 —
 *   Fake 의 install 큐가 동시 1 을 직렬화).
 *
 * 빌드 미통합: Compose/lifecycle 의존성은 통합 세션(D2)이 build.gradle 에 추가. import 미해소는
 *   의도된 상태. build.gradle·MainActivity.kt·AndroidManifest.xml 은 본 트랙이 안 건드린다.
 *
 * 소유: B 트랙(ui/viewmodel/ 신규). 기반 인터페이스(runtime/AlrRuntime.kt §5-F)를 정확히 따른다.
 */
package dev.chanwoo.androlinux.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.AppSession
import dev.chanwoo.androlinux.runtime.BundledCatalog
import dev.chanwoo.androlinux.runtime.CatalogApp
import dev.chanwoo.androlinux.runtime.InstallProgress
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchRequest
import dev.chanwoo.androlinux.runtime.SessionState
import dev.chanwoo.androlinux.runtime.SurfaceProtocol
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn

// --------------------------------------------------------------------------- //
// UiState
// --------------------------------------------------------------------------- //

/** 상세 화면의 트랜잭션 상태(설치/제거 공통 표현). */
sealed interface DetailTransaction {
    data object None : DetailTransaction
    data class Installing(val percent: Int, val stageLabel: String) : DetailTransaction
    data class Uninstalling(val percent: Int, val stageLabel: String) : DetailTransaction
    data class Failed(val message: String, val wasInstall: Boolean) : DetailTransaction
}

/**
 * 상세 화면 UiState — AlrApp 이 풀어서 AppDetailRoute 인자로 넘긴다.
 *
 * @property appId 라우트 인자(안정 id).
 * @property catalogApp 카탈로그 메타(null 이면 카탈로그에 없음). AppDetailRoute.catalogApp 로 직결.
 * @property installedApp 설치 상태(null 이면 미설치). AppDetailRoute.installedApp 로 직결.
 * @property transaction 설치/제거 진행·실패(없으면 None). 화면-로컬 표시와 별개로 ViewModel 도 추적.
 * @property isRunning 이 앱이 현재 실행 중(RENDERING)인지 — "열기" 대신 "전환" 표현 가능.
 * @property crashed 이 앱의 마지막 세션이 CRASHED 인지 — "앱 종료됨 · 재시작" 훅.
 * @property isLoading 첫 메타 방출 전.
 */
data class AppDetailUiState(
    val appId: String,
    val catalogApp: CatalogApp? = null,
    val installedApp: InstalledApp? = null,
    val transaction: DetailTransaction = DetailTransaction.None,
    val isRunning: Boolean = false,
    val crashed: Boolean = false,
    val isLoading: Boolean = true,
) {
    val isInstalled: Boolean get() = installedApp != null
    val isInstallable: Boolean get() = catalogApp != null
    /** 메타가 둘 다 없으면 결손(화면이 MissingMeta 표시). */
    val isMissing: Boolean get() = catalogApp == null && installedApp == null
}

// --------------------------------------------------------------------------- //
// ViewModel
// --------------------------------------------------------------------------- //

/**
 * 상세 ViewModel — 한 appId 에 대해 catalog/installed/sessions 를 묶어 UiState 로.
 *
 * @param appId 상세 대상(라우트 인자). AlrApp.appDetailDestination 의 backStackEntry 에서.
 * @param runtime §5-F 런타임(인터페이스만 의존).
 */
class AppDetailViewModel(
    private val appId: String,
    private val runtime: AlrRuntime,
) : ViewModel() {

    private val _transaction = MutableStateFlow<DetailTransaction>(DetailTransaction.None)

    val uiState: StateFlow<AppDetailUiState> =
        combine(
            runtime.catalog(),
            runtime.installedApps,
            runtime.sessions,
            _transaction,
        ) { catalog, installed, sessions, transaction ->
            val session = sessions.firstOrNull { it.appId == appId }
            AppDetailUiState(
                appId = appId,
                catalogApp = catalog.firstOrNull { it.appId == appId },
                installedApp = installed.firstOrNull { it.appId == appId },
                transaction = transaction,
                isRunning = session?.state?.value == SessionState.RENDERING,
                crashed = session?.state?.value == SessionState.CRASHED,
                isLoading = false,
            )
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS),
            initialValue = AppDetailUiState(appId = appId, isLoading = true),
        )

    // ----------------------------------------------------------------------- //
    // 지연 Flow 생성자 — 화면(AppDetailRoute)의 installProgress/uninstallProgress 람다로 주입.
    //   화면이 [설치]/[제거] 클릭 시 *한 번* 호출 → produceState 로 구독. ViewModel 도 같은 Flow 를
    //   onEach 로 엿보며 _transaction 을 갱신(상위 상태 — 큐/재시도/스낵바 등에 쓸 수 있게).
    // ----------------------------------------------------------------------- //

    /** [설치] 트리거용 지연 Flow — runtime.install(appId) 에 ViewModel transaction 추적을 덧댄다. */
    fun installFlow(): Flow<InstallProgress> =
        runtime.install(appId).onEach { p -> _transaction.value = p.toInstallTransaction() }

    /** [제거] 트리거용 지연 Flow — runtime.uninstall(appId) + transaction 추적. */
    fun uninstallFlow(): Flow<InstallProgress> =
        runtime.uninstall(appId).onEach { p -> _transaction.value = p.toUninstallTransaction() }

    /** 실패 후 [다시 시도] — transaction 을 초기화(화면이 새 install Flow 를 다시 만들게). */
    fun clearTransaction() {
        _transaction.value = DetailTransaction.None
    }

    // ----------------------------------------------------------------------- //
    // 실행(설치된 앱 열기) — onOpen 위임 경로의 ViewModel 측 진입(통합이 RunningSurface 결선).
    // ----------------------------------------------------------------------- //

    /** 설치된 앱 실행 — INV-2 양도는 런타임 소관. installedApp 이 있을 때만. X11-only 앱
     *  (BundledCatalog.needsXwayland by appId)은 protocol=X11 로 라우팅(상세 화면 실행 경로). */
    fun open(): AppSession? {
        val app = uiState.value.installedApp ?: return null
        return runtime.launch(
            LaunchRequest(
                appId = app.appId, entryPath = app.entry.target, args = app.entry.args,
                protocol = if (BundledCatalog.needsXwayland(app.appId)) SurfaceProtocol.X11
                else SurfaceProtocol.WAYLAND,
            ),
        )
    }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}

// --------------------------------------------------------------------------- //
// 순수 매핑 (InstallProgress → DetailTransaction)
// --------------------------------------------------------------------------- //

internal fun InstallProgress.toInstallTransaction(): DetailTransaction = when (this) {
    is InstallProgress.Running -> DetailTransaction.Installing(percent, stage.label)
    is InstallProgress.Done -> DetailTransaction.None
    is InstallProgress.Failed -> DetailTransaction.Failed(message, wasInstall = true)
}

internal fun InstallProgress.toUninstallTransaction(): DetailTransaction = when (this) {
    is InstallProgress.Running -> DetailTransaction.Uninstalling(percent, stage.label)
    is InstallProgress.Done -> DetailTransaction.None
    is InstallProgress.Failed -> DetailTransaction.Failed(message, wasInstall = false)
}
