"""Host-side (track C) structural verification of the MVVM ViewModel layer (B 트랙).

이 테스트는 device/Compose 빌드 없이 *소스 구조 불변식* 만 검증한다(다른 *_sources.py 와
동일 패턴 — Path 로 .kt 를 읽어 계약 토큰을 단언). 검증 대상:

  1) ViewModel 4개(Launcher/Catalog/AppDetail/Settings): ViewModel+viewModelScope 상속,
     AlrRuntime StateFlow 구독 → UiState StateFlow 노출, 이벤트 함수(단방향).
  2) FakeAlrRuntime 상태 고도화: 설치 큐(동시 1 + 대기열), 재시도 시뮬, 오프라인 플래그,
     CRASHED 훅 — INV-1~3(단일 포그라운드 불변식) 임계구역 보존.
  3) AlrApp.kt 배선: 각 destination 이 viewModel() 로 VM 생성 → uiState collectAsState →
     *기존 Route 시그니처 불변* 으로 넘김.
  4) 화면 Screen.kt 4개의 Route 시그니처가 안 바뀌었는지(읽기 전용 보존).

빌드 미통합 헤더가 각 신규 .kt 에 박혀 있는지도 확인(통합 세션 D2 가 의존성 추가).
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "app/src/main/java/dev/chanwoo/androlinux/ui"
VM = UI / "viewmodel"
RUNTIME = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime"

LAUNCHER_VM = VM / "LauncherViewModel.kt"
CATALOG_VM = VM / "CatalogViewModel.kt"
DETAIL_VM = VM / "AppDetailViewModel.kt"
SETTINGS_VM = VM / "SettingsViewModel.kt"
FAKE = RUNTIME / "FakeAlrRuntime.kt"
ALR_APP = UI / "AlrApp.kt"

ALL_VMS = [LAUNCHER_VM, CATALOG_VM, DETAIL_VM, SETTINGS_VM]


# --------------------------------------------------------------------------- #
# 0) 파일 존재 + 빌드-미통합 헤더
# --------------------------------------------------------------------------- #

def test_all_four_viewmodels_exist():
    for f in ALL_VMS:
        assert f.is_file(), f"missing ViewModel: {f}"


def test_viewmodels_carry_build_unintegrated_header():
    # 신규 .kt 는 "빌드 미통합: Compose/lifecycle 의존성은 통합 세션" 을 명시해야 한다.
    for f in ALL_VMS:
        text = f.read_text()
        assert "빌드 미통합" in text, f"{f.name} missing build-unintegrated note"
        assert "통합 세션" in text and "lifecycle" in text, f"{f.name} header incomplete"
        # 금지: build.gradle/MainActivity/AndroidManifest 를 건드리지 않는다고 명시.
        assert "build.gradle" in text and "MainActivity.kt" in text


def test_viewmodels_live_in_viewmodel_package():
    for f in ALL_VMS:
        assert "package dev.chanwoo.androlinux.ui.viewmodel" in f.read_text()


# --------------------------------------------------------------------------- #
# 1) MVVM 기본형 — ViewModel + viewModelScope + UiState StateFlow + 단방향
# --------------------------------------------------------------------------- #

def test_viewmodels_extend_androidx_viewmodel_and_use_scope():
    for f in ALL_VMS:
        text = f.read_text()
        assert "import androidx.lifecycle.ViewModel" in text, f"{f.name}"
        assert "import androidx.lifecycle.viewModelScope" in text, f"{f.name}"
        assert ": ViewModel()" in text, f"{f.name} must extend ViewModel"
        assert "viewModelScope" in text, f"{f.name} must use viewModelScope"


def test_viewmodels_expose_uistate_stateflow():
    # 각 VM 은 단일 uiState: StateFlow<...UiState> 를 노출(단방향 상태 출구).
    expectations = {
        LAUNCHER_VM: "LauncherUiState",
        CATALOG_VM: "CatalogUiState",
        DETAIL_VM: "AppDetailUiState",
        SETTINGS_VM: "SettingsScreenUiState",
    }
    for f, state_name in expectations.items():
        text = f.read_text()
        assert f"val uiState: StateFlow<{state_name}>" in text, f"{f.name} uiState type"
        assert f"data class {state_name}" in text, f"{f.name} state data class"
        assert "stateIn(" in text, f"{f.name} should stateIn the uiState"


def test_viewmodels_subscribe_to_alrruntime_flows():
    # AlrRuntime 의 §5-F StateFlow/Flow 를 구독(installedApps/sessions/catalog).
    launcher = LAUNCHER_VM.read_text()
    assert "runtime.installedApps" in launcher and "runtime.sessions" in launcher

    catalog = CATALOG_VM.read_text()
    assert "runtime.catalog()" in catalog and "runtime.installedApps" in catalog

    detail = DETAIL_VM.read_text()
    assert "runtime.catalog()" in detail and "runtime.installedApps" in detail
    assert "runtime.sessions" in detail

    settings = SETTINGS_VM.read_text()
    assert "runtime.installedApps" in settings


def test_uistates_carry_loading_empty_offline_error_fields():
    # UiState 데이터 + isLoading + 빈/오프라인/에러 (요구사항 (1)).
    assert "isLoading" in LAUNCHER_VM.read_text()
    assert "isEmpty" in LAUNCHER_VM.read_text()

    catalog = CATALOG_VM.read_text()
    for token in ("isLoading", "isOffline", "errorMessage", "isEmpty"):
        assert token in catalog, f"CatalogUiState missing {token}"

    assert "isLoading" in DETAIL_VM.read_text()
    assert "isLoading" in SETTINGS_VM.read_text()


def test_viewmodels_expose_event_functions_unidirectional():
    # 이벤트 함수(검색질의/카테고리필터/설치요청/실행/제거/재시도) — 명령만 위로.
    assert "fun launch(" in LAUNCHER_VM.read_text()

    catalog = CATALOG_VM.read_text()
    assert "fun requestInstall(" in catalog
    assert "fun retryInstall(" in catalog
    assert "fun retryCatalog(" in catalog

    detail = DETAIL_VM.read_text()
    assert "fun installFlow(" in detail
    assert "fun uninstallFlow(" in detail
    assert "fun open(" in detail

    settings = SETTINGS_VM.read_text()
    assert "fun onFolderPicked(" in settings
    assert "fun removeMount(" in settings
    assert "fun setDiagnostics(" in settings


# --------------------------------------------------------------------------- #
# 2) FakeAlrRuntime 상태 고도화 — 큐/재시도/오프라인/CRASHED, INV 보존
# --------------------------------------------------------------------------- #

def test_fake_runtime_declares_capability_interfaces():
    text = FAKE.read_text()
    # 선택 능력 인터페이스(오프라인/큐/크래시) — AlrRuntime 인터페이스는 불변, Fake 만 구현.
    assert "interface CatalogExtras" in text
    assert "interface InstallQueueInfo" in text
    assert "interface CrashSimulator" in text
    assert "AlrRuntime, CatalogExtras, InstallQueueInfo, CrashSimulator" in text


def test_fake_runtime_offline_mode_toggles_catalog_failure():
    text = FAKE.read_text()
    assert "override val offline: StateFlow<Boolean>" in text
    assert "override fun setOffline(" in text
    # 오프라인이면 catalog() 가 예외를 흘려 ViewModel 이 catch→offline 폴백.
    assert "throw java.io.IOException" in text
    # ViewModel 측 catch 로직도 정합.
    assert ".catch {" in CATALOG_VM.read_text()


def test_fake_runtime_install_queue_concurrency_one_with_waitlist():
    text = FAKE.read_text()
    assert "override val activeInstall: StateFlow<String?>" in text
    assert "override val installQueue: StateFlow<List<String>>" in text
    # 동시 1 슬롯 확보/반납 헬퍼.
    assert "acquireInstallSlot" in text
    assert "releaseInstallSlot" in text
    # 슬롯 반납은 finally(성공/실패/취소 공통)에서.
    assert "} finally {" in text


def test_fake_runtime_retry_simulation():
    text = FAKE.read_text()
    # 첫 시도 실패 → 집합에서 제거 → 재시도 성공.
    assert "pendingFailures" in text
    assert "failingInstalls" in text
    assert "pendingFailures.remove(appId)" in text
    assert "InstallProgress.Failed(appId" in text


def test_fake_runtime_crash_hook_preserves_invariants():
    text = FAKE.read_text()
    assert "override fun simulateCrash(" in text
    assert "fun dismissCrashed(" in text
    # CRASHED 전이는 sessionLock 임계구역 안에서(INV-2 직렬화).
    assert "SessionState.CRASHED" in text
    # 크래시 세션은 목록에 남겨 "앱 종료됨" 훅이 보이게(STOPPED 처럼 제거 X) — dismiss 가 제거.
    assert "removeSession" in text


def test_fake_runtime_keeps_inv1_inv2_critical_section():
    # INV-1~3 의 원자적 양도(promoteToForeground)는 그대로 — 큐/크래시 추가가 깨지 않았는지.
    text = FAKE.read_text()
    assert "private val sessionLock = Mutex()" in text
    assert "fun promoteToForeground(" in text
    # RENDERING 진입 전 기존 RENDERING 을 먼저 BACKGROUND 로 양도(INV-2).
    assert "SessionState.RENDERING" in text
    assert "SessionState.BACKGROUND" in text
    assert "sessionLock.withLock" in text
    # 설치 큐 임계구역은 세션 임계구역과 분리(서로 다른 Mutex).
    assert "private val installLock = Mutex()" in text


# --------------------------------------------------------------------------- #
# 3) AlrApp.kt 배선 — viewModel() → uiState → 기존 Route 시그니처 불변
# --------------------------------------------------------------------------- #

def test_alrapp_wires_each_destination_to_its_viewmodel():
    text = ALR_APP.read_text()
    for vm in ("LauncherViewModel", "CatalogViewModel", "AppDetailViewModel", "SettingsViewModel"):
        assert f"import dev.chanwoo.androlinux.ui.viewmodel.{vm}" in text, f"missing import {vm}"
        assert f"{vm}(" in text, f"AlrApp must construct {vm}"
    # viewModel(factory=...) 로 생성하고 uiState 를 collectAsState.
    assert "viewModel(factory =" in text
    assert "vm.uiState.collectAsState()" in text
    # ViewModel 팩토리(생성자 인자 주입).
    assert "alrViewModelFactory" in text
    assert "ViewModelProvider.Factory" in text


def test_alrapp_keeps_route_call_signatures_intact():
    text = ALR_APP.read_text()
    # 기존 Route 호출의 인자 키워드가 그대로(시그니처 불변 — 화면 수정 금지).
    assert "LauncherRoute(" in text
    assert "onOpenCatalog =" in text and "onOpenSettings =" in text and "onOpenAppDetail =" in text
    assert "CatalogRoute(" in text and "installedAppIds =" in text
    assert "AppDetailRoute(" in text
    assert "installProgress = { vm.installFlow() }" in text
    assert "uninstallProgress = { vm.uninstallFlow() }" in text
    assert "SettingsRoute(" in text and "installedApps =" in text


def test_appdetail_viewmodel_keyed_by_appid():
    # 라우트 인자 appId 별로 VM 을 분리(detail 은 appId 생성자 인자).
    text = ALR_APP.read_text()
    assert 'key = "appDetail:$appId"' in text
    assert "AppDetailViewModel(appId, runtime)" in text


# --------------------------------------------------------------------------- #
# 4) 화면 Screen.kt Route 시그니처 보존(읽기 전용 — 본 트랙이 수정 안 함)
# --------------------------------------------------------------------------- #

def test_screen_route_signatures_unchanged():
    launcher = (UI / "LauncherScreen.kt").read_text()
    assert "fun LauncherRoute(" in launcher
    assert "installedApps: List<InstalledApp>" in launcher
    assert "onLaunch: (InstalledApp) -> Unit" in launcher
    assert "onOpenCatalog: () -> Unit" in launcher
    assert "onOpenSettings: () -> Unit" in launcher
    assert "onOpenAppDetail: (String) -> Unit" in launcher

    catalog = (UI / "CatalogScreen.kt").read_text()
    assert "fun CatalogRoute(" in catalog
    assert "catalog: List<CatalogApp>" in catalog
    assert "installedAppIds: Set<String>" in catalog

    detail = (UI / "AppDetailScreen.kt").read_text()
    assert "fun AppDetailRoute(" in detail
    assert "installProgress: () -> Flow<InstallProgress>" in detail
    assert "uninstallProgress: () -> Flow<InstallProgress>" in detail

    settings = (UI / "SettingsScreen.kt").read_text()
    # SettingsRoute 자체는 AlrApp 가 SSOT(placeholder), 화면은 SettingsScreenRoute/SettingsScreen.
    assert "fun SettingsScreen(" in settings
    assert "fun SettingsScreenRoute(" in settings


def test_viewmodels_reuse_existing_display_models_no_redeclaration():
    # SettingsViewModel 은 ui 패키지의 SafMountEntry/DiagnosticLine 을 재사용(중복 정의 금지).
    settings = SETTINGS_VM.read_text()
    assert "import dev.chanwoo.androlinux.ui.SafMountEntry" in settings
    assert "import dev.chanwoo.androlinux.ui.DiagnosticLine" in settings
    # 화면 쪽 SettingsUiState 와 이름 충돌 회피: VM 은 SettingsScreenUiState 를 쓴다.
    assert "SettingsScreenUiState" in settings
    assert "class SettingsUiState" not in settings
