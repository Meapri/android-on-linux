/*
 * AlrApp — ALR 인앱 UI 의 네비게이션 루트(NavHost) + 화면 시그니처 계약.
 *
 * ADR-004 §3 화면 플로우(Launcher ⇄ Catalog → AppDetail → RunningSurface + Settings)를
 * Navigation-Compose NavHost 로 깐다. 4개 일반 화면(Launcher/Catalog/AppDetail/Settings)은
 * Compose, 실행화면(RunningSurface)은 별도 View Activity(ADR-004 §4-D2) — 여기서는
 * 그 진입을 콜백(onLaunchApp)으로 위임하고 placeholder 라우트만 둔다.
 *
 * ★ 이 파일이 *각 화면 @Composable 시그니처를 확정* 한다 — 아래 expect 형태의 시그니처를
 *   Screens 트랙이 그대로 구현한다(이미 존재하는 ui/LauncherScreen.kt 의 시그니처와 정합).
 *   AlrApp 은 화면을 *호출* 만 하고, 상태는 AlrRuntime(§5-F) 에서 StateFlow 로 끌어온다
 *   (단방향 데이터흐름). 본 골격은 ViewModel 을 두지 않고 runtime 을 직접 collect 하되,
 *   통합 세션이 화면별 ViewModel 을 끼울 수 있도록 화면 시그니처는 *순수 상태+콜백* 으로 둔다.
 *
 * 빌드 미통합: Compose/Navigation-Compose/Material3 의존성은 통합 세션이 build.gradle 에
 * 추가한다. 본 파일은 *소스 골격* 이며 의존성 추가 전까지 import 미해소는 의도된 상태.
 * build.gradle·MainActivity.kt·AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: 기반 트랙(ui/ 신규). 화면 4개의 *구현* 은 Screens 트랙(LauncherScreen.kt 패턴).
 */
package dev.chanwoo.androlinux.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import dev.chanwoo.androlinux.R
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.ViewModelProvider
import androidx.navigation.NavGraphBuilder
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.CatalogApp
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchRequest
import dev.chanwoo.androlinux.ui.theme.AlrTheme
import dev.chanwoo.androlinux.ui.viewmodel.AppDetailViewModel
import dev.chanwoo.androlinux.ui.viewmodel.CatalogViewModel
import dev.chanwoo.androlinux.ui.viewmodel.LauncherViewModel
import dev.chanwoo.androlinux.ui.viewmodel.SettingsViewModel

// --------------------------------------------------------------------------- //
// 라우트 (ADR-004 §3)
// --------------------------------------------------------------------------- //

/** 네비 라우트 — launcher/catalog/appDetail/{appId}/settings + runningSurface(placeholder). */
object AlrRoutes {
    const val LAUNCHER = "launcher"
    const val CATALOG = "catalog"
    const val APP_DETAIL = "appDetail" // 사용 시 "$APP_DETAIL/{appId}"
    const val SETTINGS = "settings"

    /** RunningSurface 는 실제로는 별도 View Activity(§4-D2). NavHost 안에서는 진입을
     *  콜백으로 위임하므로 라우트는 placeholder(전환 진단/디자인 Preview 용). */
    const val RUNNING_SURFACE = "runningSurface"

    fun appDetail(appId: String): String = "$APP_DETAIL/$appId"
    const val APP_DETAIL_ARG = "appId"
}

// --------------------------------------------------------------------------- //
// 앱 루트
// --------------------------------------------------------------------------- //

/**
 * 앱 진입 Composable — 테마 + NavHost. 통합 세션이 (Compose 진입점에서) 이걸 호출하고
 * AlrRuntime 실 구현(또는 Preview 의 FakeAlrRuntime)을 주입한다.
 *
 * @param runtime §5-F 런타임(설치앱/카탈로그/세션 + launch/install). UI 는 이것만 안다.
 * @param onLaunchApp 실행 요청 위임 — 통합 세션이 RunningSurfaceActivity 를 띄운다(§4-D2).
 *   AlrRuntime.launch 호출은 통합 측이 RunningSurface 결선과 함께 수행(여기선 요청만 전달).
 * @param navController 테스트/Preview 가 주입 가능(기본: rememberNavController).
 */
@Composable
fun AlrApp(
    runtime: AlrRuntime,
    onLaunchApp: (LaunchRequest) -> Unit,
    navController: NavHostController = rememberNavController(),
) {
    AlrTheme {
        AlrNavHost(
            runtime = runtime,
            navController = navController,
            onLaunchApp = onLaunchApp,
        )
    }
}

@Composable
private fun AlrNavHost(
    runtime: AlrRuntime,
    navController: NavHostController,
    onLaunchApp: (LaunchRequest) -> Unit,
) {
    NavHost(navController = navController, startDestination = AlrRoutes.LAUNCHER) {
        launcherDestination(runtime, navController, onLaunchApp)
        catalogDestination(runtime, navController)
        appDetailDestination(runtime, navController, onLaunchApp)
        settingsDestination(runtime, navController)
        // RunningSurface placeholder — 실 진입은 onLaunchApp 위임(§4-D2). 디자인 Preview 용.
        composable(AlrRoutes.RUNNING_SURFACE) {
            RunningSurfacePlaceholder(onBack = { navController.popBackStack() })
        }
    }
}

// --------------------------------------------------------------------------- //
// 라우트별 목적지 — runtime StateFlow → 화면 상태(단방향). 화면 *구현* 은 Screens 트랙.
// --------------------------------------------------------------------------- //

private fun NavGraphBuilder.launcherDestination(
    runtime: AlrRuntime,
    navController: NavHostController,
    onLaunchApp: (LaunchRequest) -> Unit,
) = composable(AlrRoutes.LAUNCHER) {
    // MVVM: ViewModel 이 installedApps+sessions 를 가공해 LauncherUiState 로 노출 → 여기서
    // collectAsState 로 받아 *기존 Route 시그니처* 그대로 넘긴다(화면은 ViewModel 을 모름).
    val vm: LauncherViewModel = viewModel(factory = alrViewModelFactory { LauncherViewModel(runtime) })
    val state by vm.uiState.collectAsState()
    // 브랜드 정체성 헤더("Android on Linux") — 최상위 scaffold chrome(presentation only).
    // 앱-그리드 로직/런타임 결선은 불변; LauncherRoute 를 그대로 호출만 한다.
    BrandedLauncherScaffold {
        LauncherRoute(
            installedApps = state.installedApps,
            // 실행은 통합 측이 RunningSurface 결선과 함께 수행(onLaunchApp 위임) — ViewModel 의
            // launch 는 mock/세션 표현용이라 두 경로가 같은 launch 로 수렴한다.
            onLaunch = { app -> onLaunchApp(app.toLaunchRequest()) },
            onOpenCatalog = { navController.navigate(AlrRoutes.CATALOG) },
            onOpenSettings = { navController.navigate(AlrRoutes.SETTINGS) },
            onOpenAppDetail = { appId -> navController.navigate(AlrRoutes.appDetail(appId)) },
        )
    }
}

// --------------------------------------------------------------------------- //
// 브랜드 헤더 — "Android on Linux" 단일 정체성(아이덴티티 레이어, presentation only).
//   런처 홈 최상단에 브랜드 TopAppBar(제목 + 부제 "Linux apps, natively")를 깐다. 앱-그리드/
//   런타임은 LauncherScreen.kt(타 트랙 소유)가 그대로 그리며, 여기서는 브랜드 chrome 만 더한다.
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun BrandedLauncherScaffold(content: @Composable () -> Unit) {
    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(
                title = {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            text = stringResource(R.string.launcher_brand_title),
                            style = MaterialTheme.typography.titleLarge,
                            textAlign = TextAlign.Center,
                        )
                        Text(
                            text = stringResource(R.string.launcher_brand_subtitle),
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.Center,
                        )
                    }
                },
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer,
                    titleContentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                ),
            )
        },
    ) { padding ->
        Column(modifier = Modifier.fillMaxSize().padding(padding)) {
            content()
        }
    }
}

private fun NavGraphBuilder.catalogDestination(
    runtime: AlrRuntime,
    navController: NavHostController,
) = composable(AlrRoutes.CATALOG) {
    val vm: CatalogViewModel = viewModel(factory = alrViewModelFactory { CatalogViewModel(runtime) })
    val state by vm.uiState.collectAsState()
    // 확정 4-인자 시그니처 그대로. isOffline/isLoading 등 UiState 의 부가 신호는 통합 시
    // CatalogScreen 의 인라인 설치(onInstall=vm::requestInstall)와 함께 활용할 수 있다.
    CatalogRoute(
        catalog = state.catalog,
        installedAppIds = state.installedAppIds,
        onOpenAppDetail = { appId -> navController.navigate(AlrRoutes.appDetail(appId)) },
        onBack = { navController.popBackStack() },
    )
}

private fun NavGraphBuilder.appDetailDestination(
    runtime: AlrRuntime,
    navController: NavHostController,
    onLaunchApp: (LaunchRequest) -> Unit,
) = composable("${AlrRoutes.APP_DETAIL}/{${AlrRoutes.APP_DETAIL_ARG}}") { backStackEntry ->
    val appId = backStackEntry.arguments?.getString(AlrRoutes.APP_DETAIL_ARG).orEmpty()
    val vm: AppDetailViewModel =
        viewModel(key = "appDetail:$appId", factory = alrViewModelFactory { AppDetailViewModel(appId, runtime) })
    val state by vm.uiState.collectAsState()
    // 지연 Flow 람다는 ViewModel 이 제공(installFlow/uninstallFlow) — 화면이 클릭 시 한 번
    // 호출해 produceState 로 구독한다. 시그니처는 불변.
    AppDetailRoute(
        appId = state.appId,
        catalogApp = state.catalogApp,
        installedApp = state.installedApp,
        installProgress = { vm.installFlow() },
        uninstallProgress = { vm.uninstallFlow() },
        onOpen = { app -> onLaunchApp(app.toLaunchRequest()) },
        onBack = { navController.popBackStack() },
    )
}

private fun NavGraphBuilder.settingsDestination(
    runtime: AlrRuntime,
    navController: NavHostController,
) = composable(AlrRoutes.SETTINGS) {
    val vm: SettingsViewModel = viewModel(factory = alrViewModelFactory { SettingsViewModel(runtime) })
    val state by vm.uiState.collectAsState()
    // SettingsRoute(installedApps, onBack) 확정 시그니처 그대로. SAF 등록부/진단은 ViewModel 이
    // 소유하며, 통합 세션이 SettingsScreenRoute(onPickFolder→vm.onFolderPicked, diagnostics=
    // state.diagnostics)로 결선할 때 mounts/diagnostics 를 끌어 쓴다.
    SettingsRoute(
        installedApps = state.installedApps,
        onBack = { navController.popBackStack() },
    )
}

// --------------------------------------------------------------------------- //
// ViewModel 팩토리 — 생성자 인자(runtime/appId)를 가진 ViewModel 을 viewModel() 로 만들기 위한
//   최소 Factory. lifecycle-viewmodel-compose 의 viewModel(factory=...) 에 넘긴다.
// --------------------------------------------------------------------------- //

/** 람다 하나로 ViewModel 을 만드는 Factory(타입 무관). 각 destination 이 자기 VM 생성을 캡처. */
private inline fun <reified T : ViewModel> alrViewModelFactory(
    crossinline create: () -> T,
): ViewModelProvider.Factory = object : ViewModelProvider.Factory {
    override fun <U : ViewModel> create(modelClass: Class<U>, extras: CreationExtras): U {
        @Suppress("UNCHECKED_CAST")
        return create() as U
    }
}

// --------------------------------------------------------------------------- //
// 화면 @Composable 시그니처 계약 (Screens 트랙 구현 대상)
// --------------------------------------------------------------------------- //
//
// 아래 4개 Route Composable 의 *시그니처가 곧 Screens 트랙의 구현 계약* 이다. AlrApp 은
// runtime StateFlow 를 collect 해 *순수 상태(데이터)* 와 *콜백* 만 화면에 넘긴다(단방향).
// Screens 트랙은 이 시그니처를 그대로 구현하고 내부에서 LauncherScreen.kt 같은 골격
// Composable 로 위임한다. 본 골격은 컴파일 가능한 placeholder 바디만 둔다(BasicText).
//
//   LauncherRoute(installedApps, onLaunch, onOpenCatalog, onOpenSettings, onOpenAppDetail)
//   CatalogRoute(catalog, installedAppIds, onOpenAppDetail, onBack)
//   AppDetailRoute(appId, catalogApp, installedApp, installProgress, uninstallProgress, onOpen, onBack)
//   SettingsRoute(installedApps, onBack)
//
// install/uninstall 은 Flow<InstallProgress> 를 *지연 생성* 하는 람다로 넘긴다 — 화면이
// collectAsState 로 진행률을 구독(설치 버튼 클릭 시점에 호출). runtime 을 화면에 직접
// 노출하지 않아 경계가 §5-F 데이터/콜백으로 닫힌다.

// LauncherRoute / CatalogRoute / AppDetailRoute 의 실 구현은 각각 LauncherScreen.kt /
// CatalogScreen.kt / AppDetailScreen.kt 에 *같은 package·같은 시그니처* 로 존재한다. 따라서
// AlrApp.kt 에는 그 3개의 placeholder 를 두지 않는다(두면 redeclaration). 위 destination
// 함수들의 LauncherRoute(...)/CatalogRoute(...)/AppDetailRoute(...) 호출은 그 실 구현으로
// 해석된다. Settings 만은 실 구현이 SettingsScreenRoute 라는 *다른 이름* 이라 충돌이 없어
// 아래 위임 래퍼를 둔다(destination 이 부르는 이름 = SettingsRoute).

/**
 * 설정 화면 진입 — 실 구현 [SettingsScreenRoute](SettingsScreen.kt)로 위임. diagnostics/
 * uiState/onPickFolder 는 기본값이 있어 2-인자 호출로 컴파일/동작한다(진단·SAF 결선은
 * 통합 세션이 인자를 더해 붙임 — integration-guide §4-D/§5-B).
 */
@Composable
fun SettingsRoute(
    installedApps: List<InstalledApp>,
    onBack: () -> Unit,
) {
    SettingsScreenRoute(
        installedApps = installedApps,
        onBack = onBack,
    )
}

// --------------------------------------------------------------------------- //
// placeholder (RUNNING_SURFACE 라우트 — 디자인 Preview 용; 실 진입은 onLaunchApp 위임)
// --------------------------------------------------------------------------- //

@Composable
private fun RunningSurfacePlaceholder(onBack: () -> Unit) {
    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text("RunningSurface (View Activity — ADR-004 §4-D2)")
    }
}

// --------------------------------------------------------------------------- //
// 변환 — InstalledApp/CatalogApp → LaunchRequest (entry → program-spec, ADR-004 §5-F)
// --------------------------------------------------------------------------- //

/** InstalledApp.entry → LaunchRequest. DESKTOP 진입은 런타임이 Exec= 를 해석하므로
 *  target(.desktop 경로)을 entryPath 로 넘기고 args 는 비운다(매니페스트 규약). */
fun InstalledApp.toLaunchRequest(): LaunchRequest = LaunchRequest(
    appId = appId,
    entryPath = entry.target,
    args = entry.args,
)

/** CatalogApp.entry → LaunchRequest(설치된 앱을 카탈로그/상세에서 바로 열 때). */
fun CatalogApp.toLaunchRequest(): LaunchRequest = LaunchRequest(
    appId = appId,
    entryPath = entry.target,
    args = entry.args,
)
