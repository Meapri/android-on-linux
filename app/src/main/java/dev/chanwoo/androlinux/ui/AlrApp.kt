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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.navigation.NavGraphBuilder
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.CatalogApp
import dev.chanwoo.androlinux.runtime.InstallProgress
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchRequest
import dev.chanwoo.androlinux.ui.theme.AlrTheme
import kotlinx.coroutines.flow.Flow

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
    val installed by runtime.installedApps.collectAsState()
    LauncherRoute(
        installedApps = installed,
        onLaunch = { app -> onLaunchApp(app.toLaunchRequest()) },
        onOpenCatalog = { navController.navigate(AlrRoutes.CATALOG) },
        onOpenSettings = { navController.navigate(AlrRoutes.SETTINGS) },
        onOpenAppDetail = { appId -> navController.navigate(AlrRoutes.appDetail(appId)) },
    )
}

private fun NavGraphBuilder.catalogDestination(
    runtime: AlrRuntime,
    navController: NavHostController,
) = composable(AlrRoutes.CATALOG) {
    val catalog by runtime.catalog().collectAsState(initial = emptyList())
    val installed by runtime.installedApps.collectAsState()
    CatalogRoute(
        catalog = catalog,
        installedAppIds = remember(installed) { installed.map { it.appId }.toSet() },
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
    val catalog by runtime.catalog().collectAsState(initial = emptyList())
    val installed by runtime.installedApps.collectAsState()
    val catalogApp = remember(catalog, appId) { catalog.firstOrNull { it.appId == appId } }
    val installedApp = remember(installed, appId) { installed.firstOrNull { it.appId == appId } }
    AppDetailRoute(
        appId = appId,
        catalogApp = catalogApp,
        installedApp = installedApp,
        installProgress = { runtime.install(appId) },
        uninstallProgress = { runtime.uninstall(appId) },
        onOpen = { app -> onLaunchApp(app.toLaunchRequest()) },
        onBack = { navController.popBackStack() },
    )
}

private fun NavGraphBuilder.settingsDestination(
    runtime: AlrRuntime,
    navController: NavHostController,
) = composable(AlrRoutes.SETTINGS) {
    val installed by runtime.installedApps.collectAsState()
    SettingsRoute(
        installedApps = installed,
        onBack = { navController.popBackStack() },
    )
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

/** 런처 화면 진입 시그니처 — Screens 트랙이 LauncherScreen.kt(AppEntry/LauncherUiState)로 구현. */
@Composable
fun LauncherRoute(
    installedApps: List<InstalledApp>,
    onLaunch: (InstalledApp) -> Unit,
    onOpenCatalog: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenAppDetail: (String) -> Unit,
) {
    ScreenPlaceholder("Launcher", "${installedApps.size}개 설치됨")
}

/** 카탈로그 화면 진입 시그니처 — Screens 트랙이 구현(카드 리스트 + 검색/카테고리). */
@Composable
fun CatalogRoute(
    catalog: List<CatalogApp>,
    installedAppIds: Set<String>,
    onOpenAppDetail: (String) -> Unit,
    onBack: () -> Unit,
) {
    ScreenPlaceholder("Catalog", "${catalog.size}개 / 설치 ${installedAppIds.size}")
}

/**
 * 앱 상세 화면 진입 시그니처 — Screens 트랙이 구현(메타+스크린샷 + 설치/열기/제거).
 * installProgress/uninstallProgress 는 설치/제거 버튼 클릭 시 호출하는 지연 Flow 생성자.
 */
@Composable
fun AppDetailRoute(
    appId: String,
    catalogApp: CatalogApp?,
    installedApp: InstalledApp?,
    installProgress: () -> Flow<InstallProgress>,
    uninstallProgress: () -> Flow<InstallProgress>,
    onOpen: (InstalledApp) -> Unit,
    onBack: () -> Unit,
) {
    ScreenPlaceholder("AppDetail", catalogApp?.name ?: installedApp?.name ?: appId)
}

/** 설정 화면 진입 시그니처 — Screens 트랙이 구현(Storage/Permissions/Diagnostics 탭). */
@Composable
fun SettingsRoute(
    installedApps: List<InstalledApp>,
    onBack: () -> Unit,
) {
    ScreenPlaceholder("Settings", "${installedApps.size}개 앱")
}

// --------------------------------------------------------------------------- //
// placeholder (Screens 트랙이 실 화면으로 교체)
// --------------------------------------------------------------------------- //

@Composable
private fun ScreenPlaceholder(title: String, subtitle: String) {
    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text(text = title, style = MaterialTheme.typography.headlineSmall)
        Text(text = subtitle, style = MaterialTheme.typography.bodyMedium)
    }
}

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
