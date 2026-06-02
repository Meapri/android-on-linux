/*
 * LauncherScreen — ALR 홈 그리드(앱 런처) Jetpack Compose 화면.
 *
 * 이 파일은 AlrApp.kt(기반 트랙)가 확정한 진입 시그니처 LauncherRoute(...)를 구현한다:
 *   @Composable fun LauncherRoute(
 *       installedApps: List<InstalledApp>,
 *       onLaunch: (InstalledApp) -> Unit,
 *       onOpenCatalog: () -> Unit,
 *       onOpenSettings: () -> Unit,
 *       onOpenAppDetail: (String) -> Unit,
 *   )
 * AlrApp 의 launcherDestination 이 runtime.installedApps(StateFlow)를 collectAsState 로
 * 받아 *순수 상태(List<InstalledApp>) + 콜백* 만 내려준다(ADR-004 §5-F 단방향 데이터흐름).
 * 이 화면은 런타임/게스트를 직접 알지 못하며, 실행/카탈로그/상세/설정은 전부 콜백 위임이다.
 *
 * 표시 모델은 기반 트랙의 runtime/AppModels.kt 를 그대로 쓴다(InstalledApp/AppCategory).
 * 카테고리 라벨은 AppCategory.label(한글) 을 직접 사용 — host-검증 tools/alr_manifest.py
 * CATEGORIES 와 1:1 인 닫힌 enum 이라 freedesktop 문자열 매핑이 불필요해졌다.
 *
 * 기능: 설치 앱 아이콘 그리드(LazyVerticalGrid) + 검색바 + 카테고리 필터 칩 +
 * 길게눌러 [정보]/[제거] 메뉴(DropdownMenu) + 카탈로그 진입 FAB + 빈 상태.
 *
 * 빌드 미통합: Compose 의존성은 통합 세션이 build.gradle 에 추가(이 파일은 그 전까지
 * 컴파일 대상이 아니며 import 미해소는 의도된 상태). MainActivity.kt /
 * AndroidManifest.xml / build.gradle / version stamp 는 본 트랙이 건드리지 않는다.
 *
 * 소유: T3(런처/선택 UI). ui/LauncherScreen.kt 만 본 화면 소유.
 */
package dev.chanwoo.androlinux.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import dev.chanwoo.androlinux.runtime.AppCategory
import dev.chanwoo.androlinux.runtime.AppPermission
import dev.chanwoo.androlinux.runtime.DisplaySpec
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchEntry
import dev.chanwoo.androlinux.ui.theme.AlrTheme

// --------------------------------------------------------------------------- //
// 진입(Route) — AlrApp.kt 가 확정한 시그니처. 그대로 구현(기반이 정함).
// --------------------------------------------------------------------------- //

/**
 * 런처 화면 진입점 — AlrApp.launcherDestination 이 호출.
 *
 * @param installedApps    runtime.installedApps StateFlow 를 collect 한 *순수 상태*.
 * @param onLaunch         타일 탭 → 실행(AlrRuntime.launch 는 통합 측이 RunningSurface 결선과 수행).
 * @param onOpenCatalog    FAB / 빈 상태 CTA → 카탈로그로.
 * @param onOpenSettings   상단 톱니 → 설정으로.
 * @param onOpenAppDetail  길게눌러 [정보] → 상세로(appId 전달).
 *
 * 이 함수는 *상태+콜백* 만 받는다(단방향). 검색어/선택 카테고리/열린 메뉴 같은 *지역 UI
 * 상태* 만 remember 로 들고, 데이터 변경은 상위(runtime)에서 새 installedApps 로 흘러온다.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LauncherRoute(
    installedApps: List<InstalledApp>,
    onLaunch: (InstalledApp) -> Unit,
    onOpenCatalog: () -> Unit,
    onOpenSettings: () -> Unit,
    onOpenAppDetail: (String) -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("AndroLinux") },
                actions = {
                    IconButton(onClick = onOpenSettings) {
                        Icon(Icons.Filled.Settings, contentDescription = "설정")
                    }
                },
            )
        },
        floatingActionButton = {
            // 빈 상태에서는 본문 CTA 가 카탈로그로 안내하므로 FAB 는 숨겨 중복을 피한다.
            if (installedApps.isNotEmpty()) {
                ExtendedFloatingActionButton(
                    onClick = onOpenCatalog,
                    icon = { Icon(Icons.Filled.Add, contentDescription = null) },
                    text = { Text("앱 설치") },
                )
            }
        },
    ) { padding ->
        if (installedApps.isEmpty()) {
            EmptyState(onOpenCatalog = onOpenCatalog, padding = padding)
        } else {
            LauncherGrid(
                installedApps = installedApps,
                onLaunch = onLaunch,
                onOpenAppDetail = onOpenAppDetail,
                padding = padding,
            )
        }
    }
}

// --------------------------------------------------------------------------- //
// 그리드 본문 — 검색 + 카테고리 필터(AND) + LazyVerticalGrid
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LauncherGrid(
    installedApps: List<InstalledApp>,
    onLaunch: (InstalledApp) -> Unit,
    onOpenAppDetail: (String) -> Unit,
    padding: PaddingValues,
) {
    var query by remember { mutableStateOf("") }
    // null == 전체. 닫힌 enum AppCategory 라 "전체"는 별도 센티넬 대신 null 로 표현.
    var selectedCategory by remember { mutableStateOf<AppCategory?>(null) }

    // 설치된 앱이 실제로 가진 카테고리만 칩으로 — 빈 칩(0개)을 안 보이게.
    val categories = remember(installedApps) {
        installedApps.map { it.category }.distinct().sortedBy { it.label }
    }
    // 선택 카테고리가 사라지면(앱 제거 등) 전체로 폴백.
    if (selectedCategory != null && selectedCategory !in categories) {
        selectedCategory = null
    }

    // 검색 + 카테고리 AND 결합. 검색은 name/summary 부분일치(대소문자 무시).
    val filtered = remember(installedApps, query, selectedCategory) {
        installedApps.filter { app ->
            (selectedCategory == null || app.category == selectedCategory) &&
                matchesQuery(app, query)
        }
    }

    Column(modifier = Modifier.padding(padding).fillMaxSize()) {
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("검색") },
            leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null) },
            singleLine = true,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
        )
        // 카테고리가 둘 이상일 때만 필터 칩 노출(한 종류뿐이면 필터 무의미).
        if (categories.size > 1) {
            CategoryChips(
                categories = categories,
                selected = selectedCategory,
                onSelect = { cat -> selectedCategory = cat },
            )
        }
        if (filtered.isEmpty()) {
            NoResults()
        } else {
            LazyVerticalGrid(
                columns = GridCells.Adaptive(minSize = 96.dp),
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                items(filtered, key = { it.appId }) { app ->
                    AppCell(
                        app = app,
                        onLaunch = { onLaunch(app) },
                        onInfo = { onOpenAppDetail(app.appId) },
                    )
                }
            }
        }
    }
}

// --------------------------------------------------------------------------- //
// 셀 — 아이콘 + 이름 + 길게눌러 정보/제거 DropdownMenu
// --------------------------------------------------------------------------- //

/**
 * 그리드 셀 하나. 짧게 탭 → 실행, 길게 누르면 그 셀 위치에 [정보]/[제거] 메뉴를 띄운다.
 * combinedClickable 로 탭/롱프레스를 한 노드에서 처리하고, 메뉴 가시성은 셀-지역 상태.
 *
 * 제거([제거])는 onOpenAppDetail 로 상세 화면에 위임한다 — 실제 제거 트랜잭션
 * (AlrRuntime.uninstall + 진행률)은 AppDetailRoute 소관이며(ADR-004 §5-F), 런처는
 * 파괴적 동작을 인라인으로 즉시 실행하지 않고 확인 가능한 상세로 보낸다.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun AppCell(
    app: InstalledApp,
    onLaunch: () -> Unit,
    onInfo: () -> Unit,
) {
    var menuOpen by remember { mutableStateOf(false) }

    Box {
        Column(
            modifier = Modifier
                .combinedClickable(
                    onClick = onLaunch,
                    onLongClick = { menuOpen = true },
                )
                .padding(4.dp)
                .semantics { contentDescription = app.name },
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            AppIcon(app)
            Text(
                text = app.name,
                style = MaterialTheme.typography.labelMedium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
        }
        DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
            DropdownMenuItem(
                text = { Text("정보") },
                leadingIcon = { Icon(Icons.Filled.Info, contentDescription = null) },
                onClick = {
                    menuOpen = false
                    onInfo()
                },
            )
            DropdownMenuItem(
                text = { Text("제거") },
                leadingIcon = { Icon(Icons.Filled.Delete, contentDescription = null) },
                onClick = {
                    menuOpen = false
                    // 제거는 상세(AppDetailRoute)에서 확인 후 수행 — §5-F uninstall 소관.
                    onInfo()
                },
            )
        }
    }
}

/**
 * 앱 아이콘 자리표시. 통합 세션이 이미지 로더(Coil 등)로 iconPath 디코드를 주입한다 —
 * 예: rememberAsyncImagePainter(File(app.iconPath)), 경로 null/디코드 실패 시 카테고리
 * 색 폴백. 골격에서는 카테고리 머리글자 원형 배지로 결정적 Preview 를 보장한다.
 */
@Composable
private fun AppIcon(app: InstalledApp) {
    Surface(
        modifier = Modifier.size(64.dp),
        shape = CircleShape,
        color = MaterialTheme.colorScheme.primaryContainer,
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = app.name.firstOrNull()?.uppercase() ?: "?",
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onPrimaryContainer,
            )
        }
    }
}

// --------------------------------------------------------------------------- //
// 카테고리 필터 칩 — AppCategory.label(한글) 직접 사용. null == 전체.
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CategoryChips(
    categories: List<AppCategory>,
    selected: AppCategory?,
    onSelect: (AppCategory?) -> Unit,
) {
    LazyRow(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            FilterChip(
                selected = selected == null,
                onClick = { onSelect(null) },
                label = { Text("전체") },
            )
        }
        items(categories, key = { it.id }) { cat ->
            FilterChip(
                selected = cat == selected,
                onClick = { onSelect(cat) },
                label = { Text(cat.label) },
            )
        }
    }
}

// --------------------------------------------------------------------------- //
// 빈 상태 / 결과 없음
// --------------------------------------------------------------------------- //

@Composable
private fun EmptyState(onOpenCatalog: () -> Unit, padding: PaddingValues) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(padding)
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("설치된 리눅스 앱이 없어요", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Text(
            "카탈로그에서 앱을 설치해 시작하세요",
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(24.dp))
        ExtendedFloatingActionButton(
            onClick = onOpenCatalog,
            icon = { Icon(Icons.Filled.Add, contentDescription = null) },
            text = { Text("앱 설치") },
        )
    }
}

@Composable
private fun NoResults() {
    Box(
        modifier = Modifier.fillMaxSize().padding(32.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            "조건에 맞는 앱이 없어요",
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
        )
    }
}

// --------------------------------------------------------------------------- //
// 순수 헬퍼 (런타임/리소스 독립)
// --------------------------------------------------------------------------- //

/**
 * 검색 매칭 — name/summary 부분일치, 대소문자 무시. host-검증 파서
 * tools/desktop_entry.matches_query 와 동일 의도(부분일치)이되, 표시 모델
 * InstalledApp 이 가진 필드(name/summary)에 맞춘다.
 */
private fun matchesQuery(app: InstalledApp, query: String): Boolean {
    val q = query.trim().lowercase()
    if (q.isEmpty()) return true
    return app.name.lowercase().contains(q) || app.summary.lowercase().contains(q)
}

// --------------------------------------------------------------------------- //
// @Preview (더미 데이터) — FakeAlrRuntime 시드와 정합하는 device-증명 앱들
// --------------------------------------------------------------------------- //

private val previewApps = listOf(
    InstalledApp(
        appId = "org.gimp.GIMP",
        name = "GIMP",
        summary = "GNU 이미지 편집 프로그램",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gimp"),
        category = AppCategory.GRAPHICS,
        requiredPermissions = listOf(AppPermission.STORAGE_READ, AppPermission.STORAGE_WRITE),
        display = DisplaySpec(DisplaySpec.DisplayMode.FULLSCREEN),
        installedSizeBytes = 320L * 1024 * 1024,
        version = "3.0.2",
    ),
    InstalledApp(
        appId = "org.foot.foot",
        name = "foot",
        summary = "가벼운 Wayland 터미널 에뮬레이터",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/foot"),
        category = AppCategory.TERMINAL,
        display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
        installedSizeBytes = 6L * 1024 * 1024,
        version = "1.16.2",
    ),
    InstalledApp(
        appId = "org.netsurf.netsurf",
        name = "NetSurf",
        summary = "경량 GTK 웹 브라우저",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/netsurf-gtk3"),
        category = AppCategory.INTERNET,
        requiredPermissions = listOf(AppPermission.NETWORK),
        display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
        installedSizeBytes = 195L * 1024 * 1024,
        version = "3.11",
    ),
)

@Preview(name = "Launcher — 설치 앱 그리드", showBackground = true)
@Composable
private fun LauncherRoutePreview() {
    AlrTheme {
        LauncherRoute(
            installedApps = previewApps,
            onLaunch = {},
            onOpenCatalog = {},
            onOpenSettings = {},
            onOpenAppDetail = {},
        )
    }
}

@Preview(name = "Launcher — 다크", showBackground = true)
@Composable
private fun LauncherRouteDarkPreview() {
    AlrTheme(darkTheme = true) {
        LauncherRoute(
            installedApps = previewApps,
            onLaunch = {},
            onOpenCatalog = {},
            onOpenSettings = {},
            onOpenAppDetail = {},
        )
    }
}

@Preview(name = "Launcher — 빈 상태", showBackground = true)
@Composable
private fun LauncherRouteEmptyPreview() {
    AlrTheme {
        LauncherRoute(
            installedApps = emptyList(),
            onLaunch = {},
            onOpenCatalog = {},
            onOpenSettings = {},
            onOpenAppDetail = {},
        )
    }
}
