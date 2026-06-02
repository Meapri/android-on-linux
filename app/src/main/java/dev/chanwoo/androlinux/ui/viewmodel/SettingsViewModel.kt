/*
 * SettingsViewModel — 설정 화면(MVVM)의 ViewModel 레이어.
 *
 * ADR-004 §5-F 단방향: installedApps(런타임 정보)와 SAF 마운트 등록부/진단 라인을 묶어
 * SettingsScreenUiState(StateFlow)로 노출한다. 화면(SettingsScreen / SettingsScreenRoute)은
 * 기존 시그니처 그대로 — AlrApp(settingsDestination)이 ViewModel.uiState 를 collectAsState 해
 * installedApps/mounts/diagnostics + onPickFolder/onRemoveMount/onBack 을 화면에 넘긴다.
 *
 * ★ 화면 시그니처 불변: SettingsScreen(installedApps, mounts, diagnostics, onPickFolder,
 *   onRemoveMount, onBack) 는 안 바뀐다. 현재 SettingsScreen.kt 는 화면-로컬 SettingsUiState
 *   클래스(SAF 등록부)를 remember 로 들었는데, MVVM 으로는 그 등록부를 *ViewModel 이* 소유하고
 *   화면엔 mounts:List + onRemoveMount 콜백만 내려준다(SettingsScreenRoute 의 위임 형태와 정합).
 *   진단 라인 주입(통합 세션의 executionSummary)도 ViewModel.setDiagnostics 로 받는다.
 *
 * 패키지 주의: 화면 쪽 ui.SettingsUiState(SAF 등록부)와 이름 충돌을 피하려고 본 ViewModel 의
 *   UiState 는 SettingsScreenUiState 로 둔다. SafMountEntry/DiagnosticLine 은 ui 패키지의 표시
 *   모델을 그대로 재사용한다(중복 정의 금지).
 *
 * SAF URI 영속/path-mediation 결선은 통합/런타임(T5·WS-1) 소관 — ViewModel 은 *등록부(인-메모리)
 *   StateFlow* 와 *등록/해제 명령* 만 안다. picker 진입(ACTION_OPEN_DOCUMENT_TREE)은 화면→AlrApp
 *   →통합 세션의 ActivityResult 런처가 맡고, 결과를 onFolderPicked(label, uri, name) 로 ViewModel 에 넣는다.
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
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.ui.DiagnosticLine
import dev.chanwoo.androlinux.ui.SafMountEntry
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn

// --------------------------------------------------------------------------- //
// UiState
// --------------------------------------------------------------------------- //

/**
 * 설정 화면 UiState — AlrApp 이 풀어 SettingsScreen 인자로 넘긴다.
 *
 * @property installedApps 런타임 정보 구획("설치 앱 N개 · 용량 합") 표시용.
 * @property mounts 등록된 SAF 마운트(D4). SettingsScreen.mounts 로 직결.
 * @property diagnostics 진단 라인(통합 세션 주입; 기본 미수집=빈 리스트). SettingsScreen.diagnostics 로.
 * @property isLoading 첫 installedApps 방출 전.
 */
data class SettingsScreenUiState(
    val installedApps: List<InstalledApp> = emptyList(),
    val mounts: List<SafMountEntry> = emptyList(),
    val diagnostics: List<DiagnosticLine> = emptyList(),
    val isLoading: Boolean = true,
)

// --------------------------------------------------------------------------- //
// ViewModel
// --------------------------------------------------------------------------- //

/**
 * 설정 ViewModel — installedApps(런타임) + SAF 등록부(_mounts) + 진단(_diagnostics)을 combine.
 *
 * SAF 등록부와 진단은 화면-로컬이 아니라 ViewModel-소유 StateFlow 다(MVVM 승격) — 회전/재구성에도
 * 등록부가 유지된다(화면-로컬 remember 보다 강한 생존). register/remove/setDiagnostics 가 명령.
 */
class SettingsViewModel(
    private val runtime: AlrRuntime,
) : ViewModel() {

    private val _mounts = MutableStateFlow<List<SafMountEntry>>(emptyList())
    private val _diagnostics = MutableStateFlow<List<DiagnosticLine>>(emptyList())

    val uiState: StateFlow<SettingsScreenUiState> =
        combine(
            runtime.installedApps,
            _mounts,
            _diagnostics,
        ) { installed, mounts, diagnostics ->
            SettingsScreenUiState(
                installedApps = installed,
                mounts = mounts,
                diagnostics = diagnostics,
                isLoading = false,
            )
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(STOP_TIMEOUT_MS),
            initialValue = SettingsScreenUiState(isLoading = true),
        )

    // ----------------------------------------------------------------------- //
    // 이벤트(화면 → ViewModel) — SAF 등록/해제 + 진단 주입.
    // ----------------------------------------------------------------------- //

    /**
     * picker 결과 등록 — 같은 label 이면 갱신(교체). saf_bridge_model.SafMount 규칙 미러:
     * label 은 ^[A-Za-z0-9._-]+$, guestPath 는 SafMountEntry 가 /mnt/android/<label> 로 합성.
     * 형식이 어긋나면 등록을 거부(방어 — 통합 picker 가 정규화하지만 ViewModel 도 가드).
     */
    fun onFolderPicked(label: String, treeUri: String, displayName: String = "") {
        if (!isValidLabel(label)) return
        val entry = SafMountEntry(label = label, displayName = displayName, treeUri = treeUri)
        _mounts.value = _mounts.value.filterNot { it.label == label } + entry
    }

    /** 마운트 해제. */
    fun removeMount(label: String) {
        _mounts.value = _mounts.value.filterNot { it.label == label }
    }

    /** 진단 라인 주입 — 통합 세션이 executionSummary 를 매핑해 넣는다(개발자/지원용). */
    fun setDiagnostics(lines: List<DiagnosticLine>) {
        _diagnostics.value = lines
    }

    private fun isValidLabel(label: String): Boolean =
        label.isNotEmpty() && label.all { it.isLetterOrDigit() || it == '.' || it == '_' || it == '-' }

    private companion object {
        const val STOP_TIMEOUT_MS = 5_000L
    }
}
