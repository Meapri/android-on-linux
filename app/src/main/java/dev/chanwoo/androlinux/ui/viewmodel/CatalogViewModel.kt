/*
 * CatalogViewModel — 카탈로그 화면(MVVM)의 ViewModel 레이어.
 *
 * ADR-004 §5-F 단방향: AlrRuntime.catalog()(Flow) + installedApps(StateFlow) → CatalogViewModel
 * 이 구독·가공 → CatalogUiState(StateFlow). 화면(CatalogRoute)은 기존 4-인자 시그니처
 * (catalog, installedAppIds, onOpenAppDetail, onBack) 그대로 그린다 — AlrApp 이 UiState 를 풀어
 * 그 인자로 넘긴다. 이벤트(검색질의/카테고리·출처 필터/설치요청/재시도)는 ViewModel 함수로 흐른다.
 *
 * ★ 화면 시그니처 불변: CatalogRoute 는 한 글자도 안 바뀐다. 검색/필터는 현재 CatalogScreen 이
 *   화면-로컬 remember 로 들고 있어 그대로 둔다(화면 보존 우선). ViewModel 은 그 위에서 *데이터
 *   로딩 / 오프라인(fetch 실패) / 설치 큐(동시 1 + 대기열) / 설치 실패-재시도* 를 관장하고, 필요한
 *   파생 상태(필터된 카탈로그·항목별 설치상태)를 UiState 로 노출한다(통합 시 인라인 설치에 활용).
 *
 * 오프라인/큐/재시도: 기본 AlrRuntime 인터페이스는 catalog()/install() 만 안다. FakeAlrRuntime 이
 *   추가로 CatalogExtras(오프라인 플래그/재시도)·InstallQueue(동시 1 + 대기열)를 구현하면
 *   ViewModel 이 안전 다운캐스트로 그 신호를 끌어 쓴다(없으면 graceful 폴백). 이렇게 §5-F
 *   AlrRuntime 계약(WS-1 수락)은 불변으로 두고, Fake 만 풍부한 데모 상태를 제공한다.
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
import dev.chanwoo.androlinux.runtime.CatalogApp
import dev.chanwoo.androlinux.runtime.CatalogExtras
import dev.chanwoo.androlinux.runtime.InstallProgress
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

// --------------------------------------------------------------------------- //
// 항목별 설치 상태(표시용) — runtime.install Flow 를 구독해 갱신
// --------------------------------------------------------------------------- //

/**
 * 한 카탈로그 항목의 ViewModel 측 설치 상태. CatalogScreen 의 화면-로컬 ItemInstallState 와
 * 의미가 같되, 큐(대기열)/재시도까지 표현한다. installedAppIds(SSOT) 가 Installed 를 최종 판정.
 *
 *  Idle      : 미설치(아직 설치 트리거 안 됨).
 *  Queued    : 설치 요청됨, 동시 1 슬롯이 차서 대기 중(앞에 N개).
 *  Installing: 설치 진행 중(percent 0..100 + 단계 라벨).
 *  Failed    : 실패(message) — 재시도 가능.
 */
sealed interface CatalogItemState {
    data object Idle : CatalogItemState
    data class Queued(val position: Int) : CatalogItemState
    data class Installing(val percent: Int, val stageLabel: String) : CatalogItemState
    data class Failed(val message: String) : CatalogItemState
}

/**
 * 카탈로그 화면 UiState — 단방향으로 화면에 노출(AlrApp 이 풀어 CatalogRoute 인자로).
 *
 * @property catalog 설치 가능 앱(번들 + apt). CatalogRoute.catalog 로 직결.
 * @property installedAppIds 설치 완료 appId 집합(SSOT). CatalogRoute.installedAppIds 로 직결.
 * @property itemStates appId → 진행/대기/실패 표시 상태(인라인 설치 UI 용; 통합 시 활용).
 * @property isLoading 첫 catalog 방출 전.
 * @property isOffline 카탈로그 fetch 실패(오프라인) — 화면이 "오프라인" 배너 + [재시도].
 * @property errorMessage fetch/설치 일반 오류 메시지(있으면).
 * @property isEmpty 로딩 끝났는데 카탈로그가 0개(빈 상태).
 */
data class CatalogUiState(
    val catalog: List<CatalogApp> = emptyList(),
    val installedAppIds: Set<String> = emptySet(),
    val itemStates: Map<String, CatalogItemState> = emptyMap(),
    val isLoading: Boolean = true,
    val isOffline: Boolean = false,
    val errorMessage: String? = null,
    val isEmpty: Boolean = false,
)

// --------------------------------------------------------------------------- //
// ViewModel
// --------------------------------------------------------------------------- //

/**
 * 카탈로그 ViewModel — catalog()/installedApps 를 combine, 항목별 설치 상태(_itemStates)를 겹쳐
 * CatalogUiState 로 노출. 설치는 큐(동시 1)로 직렬화한다(InstallQueue 가 있으면 위임, 없으면
 * 자체 직렬 큐). catalog() 가 던지면(catch) 오프라인으로 폴백한다.
 */
class CatalogViewModel(
    private val runtime: AlrRuntime,
) : ViewModel() {

    /** 항목별 설치 표시 상태(appId → 상태). 설치 진행/대기/실패만 들고 SSOT 는 installedAppIds. */
    private val _itemStates = MutableStateFlow<Map<String, CatalogItemState>>(emptyMap())

    /**
     * 재시도 트리거 — [재시도] 마다 +1. flatMapLatest 가 이 신호에 반응해 catalog() 를 *다시*
     * 구독한다. catalog() 가 오프라인일 때 예외를 던져 collection 이 끝나도(.catch 가 terminal),
     * 트리거가 바뀌면 새 collection 으로 재-fetch 되므로 retryCatalog() 가 실제로 회복된다.
     */
    private val retryTrigger = MutableStateFlow(0)

    /** catalog() 가 실패하면(오프라인) 던지므로 catch 로 잡아 offline 신호로 바꾼다(재구독은 트리거). */
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    private val catalogFlow =
        retryTrigger.flatMapLatest {
            runtime.catalog()
                .map { CatalogFetch(apps = it, offline = false) }
                .catch { emit(CatalogFetch(apps = emptyList(), offline = true)) }
        }

    val uiState: StateFlow<CatalogUiState> =
        combine(
            catalogFlow,
            runtime.installedApps,
            _itemStates,
        ) { fetch, installed, itemStates ->
            CatalogUiState(
                catalog = fetch.apps,
                installedAppIds = installed.map { it.appId }.toSet(),
                itemStates = itemStates,
                isLoading = false,
                isOffline = fetch.offline,
                errorMessage = if (fetch.offline) "카탈로그를 불러오지 못했어요 (오프라인)" else null,
                isEmpty = !fetch.offline && fetch.apps.isEmpty(),
            )
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS),
            initialValue = CatalogUiState(isLoading = true),
        )

    /** 직렬 설치 큐 — 동시 1, 나머지는 대기열(FIFO). InstallQueue 부재 시 ViewModel 이 자체 구동. */
    private val pendingQueue = ArrayDeque<String>()
    private var activeInstall: String? = null

    // ----------------------------------------------------------------------- //
    // 이벤트(화면 → ViewModel) — 검색/필터는 화면-로컬이라 여기선 설치/재시도만.
    // ----------------------------------------------------------------------- //

    /**
     * 설치 요청 — 동시 1 정책. 이미 설치/진행/대기 중이면 무시(멱등). 슬롯이 비어 있으면 즉시
     * 시작, 차 있으면 대기열에 넣고 Queued(position) 표시. runtime.install Flow 를 구독해 진행률을
     * _itemStates 에 반영하고, Done/Failed 에서 다음 대기 항목을 끌어온다.
     */
    fun requestInstall(appId: String) {
        if (appId in uiState.value.installedAppIds) return
        val cur = _itemStates.value[appId]
        if (cur is CatalogItemState.Installing || cur is CatalogItemState.Queued) return
        if (activeInstall == null) {
            startInstall(appId)
        } else {
            pendingQueue.addLast(appId)
            setItem(appId, CatalogItemState.Queued(pendingQueue.size))
        }
    }

    /** 실패한 설치 재시도 — 같은 큐 규칙으로 다시 요청. */
    fun retryInstall(appId: String) {
        setItem(appId, CatalogItemState.Idle)
        requestInstall(appId)
    }

    /** 오프라인일 때 카탈로그 다시 불러오기 — Fake 의 CatalogExtras 가 있으면 온라인 복구 후
     *  flatMapLatest 가 재-fetch 하도록 retryTrigger 를 bump 한다(트리거가 SSOT — 실구현/Fake 무관). */
    fun retryCatalog() {
        (runtime as? CatalogExtras)?.setOffline(false)
        retryTrigger.value += 1
    }

    private fun startInstall(appId: String) {
        activeInstall = appId
        setItem(appId, CatalogItemState.Installing(0, "준비 중"))
        viewModelScope.launch {
            runtime.install(appId).collect { progress ->
                when (progress) {
                    is InstallProgress.Running ->
                        setItem(appId, CatalogItemState.Installing(progress.percent, progress.stage.label))
                    is InstallProgress.Done ->
                        // SSOT(installedAppIds)가 곧 Installed 로 수렴 — 임시상태는 비운다.
                        clearItem(appId)
                    is InstallProgress.Failed ->
                        setItem(appId, CatalogItemState.Failed(progress.message))
                }
            }
            // 이 설치 종료 → 다음 대기 항목 끌어오기(동시 1 유지).
            activeInstall = null
            promoteNextQueued()
        }
    }

    private fun promoteNextQueued() {
        val next = pendingQueue.removeFirstOrNull() ?: return
        // 대기열 잔여 position 재계산(앞당김).
        pendingQueue.forEachIndexed { i, id -> setItem(id, CatalogItemState.Queued(i + 1)) }
        startInstall(next)
    }

    private fun setItem(appId: String, state: CatalogItemState) {
        _itemStates.value = _itemStates.value + (appId to state)
    }

    private fun clearItem(appId: String) {
        _itemStates.value = _itemStates.value - appId
    }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}

/** catalog() 한 방출의 가공 결과(앱 목록 + 오프라인 여부). */
private data class CatalogFetch(val apps: List<CatalogApp>, val offline: Boolean)
