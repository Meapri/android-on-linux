/*
 * CatalogScreen — ALR 인앱 카탈로그(설치 가능 리눅스 앱 목록) Jetpack Compose 화면.
 *
 * ADR-004 §3 화면 플로우의 Catalog 트랙. 사용자는 apt 인덱스 기반 카탈로그(번들 +
 * apt 합성)를 훑고(검색/카테고리 필터), 각 항목을 바로 설치하거나(InstallProgress
 * 진행률 인라인 표시) 상세로 진입한다. 설치는 ADR-004 §0-D3 v1 = stage-tar overlay
 * 풀기(dpkg fork-exec 0) — UI 는 진행률만 본다(§7).
 *
 * ★ 확정 시그니처(기반 ui/AlrApp.kt 가 정함, 그대로 구현):
 *     CatalogRoute(catalog, installedAppIds, onOpenAppDetail, onBack)
 *   AlrApp 이 runtime.catalog()/installedApps 를 collect 해 *순수 상태(데이터)+콜백* 만
 *   넘긴다(단방향 데이터흐름). 이 화면은 AlrRuntime 을 직접 알지 않는다 — 모델만 본다.
 *
 *   다만 "각 항목 설치 버튼(진행률 표시)"은 카탈로그 화면 안에서 install Flow 를
 *   구독해야 하므로, 시그니처는 보존하되 *설치 트리거*를 옵션 콜백
 *   `onInstall: (String) -> Flow<InstallProgress>` 로 추가 노출한다(기본값 제공 →
 *   기존 4-인자 호출부 무변경). 통합 세션이 runtime.install 을 이 람다로 주입하면
 *   인라인 설치가 살아나고, 주입 안 하면 설치 버튼이 상세로 위임(onOpenAppDetail)된다.
 *
 * 런타임 독립: 게스트를 직접 실행하지 않는다. 모든 동작은 콜백/Flow 로 위임.
 * 표시 모델(CatalogApp/InstallProgress/AppCategory/AppSource)은 runtime/AppModels.kt
 * — host-검증 파이썬(tools/alr_manifest.py·apt_catalog.py)과 1:1 대응.
 *
 * 빌드 미통합: Compose 의존성은 통합 세션이 build.gradle 에 추가(이 파일은 의존성
 * 추가 전까지 import 미해소가 의도된 상태 — 소스 골격). build.gradle·MainActivity.kt·
 * AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: Screens 트랙(ui/CatalogScreen.kt 신규). 기반 트랙의 인터페이스/모델/네비를 따름.
 */
package dev.chanwoo.androlinux.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import dev.chanwoo.androlinux.runtime.AppCategory
import dev.chanwoo.androlinux.runtime.AppPermission
import dev.chanwoo.androlinux.runtime.AppSource
import dev.chanwoo.androlinux.runtime.CatalogApp
import dev.chanwoo.androlinux.runtime.DisplaySpec
import dev.chanwoo.androlinux.runtime.InstallProgress
import dev.chanwoo.androlinux.runtime.LaunchEntry
import dev.chanwoo.androlinux.runtime.RootfsDep
import dev.chanwoo.androlinux.runtime.RootfsDepKind
import dev.chanwoo.androlinux.ui.theme.AlrTheme
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.launch

// --------------------------------------------------------------------------- //
// 항목별 설치 UI 상태 (화면 로컬 — runtime 은 Flow<InstallProgress> 만 흘림)
// --------------------------------------------------------------------------- //

/**
 * 한 카탈로그 항목의 *표시용* 설치 상태. runtime.install(appId) Flow 를 구독해 갱신.
 *
 *  NotInstalled : 미설치 — [설치] 버튼.
 *  Installing   : 설치 중 — 진행률(0..100) + 단계 라벨 + 인라인 LinearProgressIndicator.
 *  Installed    : 설치 완료 — [열기]/[설치됨] 표시(installedAppIds 또는 Flow Done).
 *  Failed       : 실패 — 사유 + [다시 시도].
 *
 * installedAppIds(상위 StateFlow)가 SSOT 이고, Installing/Failed 는 진행 중 임시상태다.
 * Done 도착 시 상위 installedApps 가 갱신되며 자연히 Installed 로 수렴한다.
 */
private sealed interface ItemInstallState {
    data object NotInstalled : ItemInstallState
    data class Installing(val percent: Int, val stageLabel: String) : ItemInstallState
    data object Installed : ItemInstallState
    data class Failed(val message: String) : ItemInstallState
}

// --------------------------------------------------------------------------- //
// Route (기반 ui/AlrApp.kt 확정 시그니처) — 그대로 구현
// --------------------------------------------------------------------------- //
//
// ★ 시그니처 정합: ui/AlrApp.kt 가 CatalogRoute(catalog, installedAppIds,
//   onOpenAppDetail, onBack) 를 placeholder 로 *이미 선언* 한다(같은 패키지). 본 파일이
//   그 placeholder 를 대체하는 실 구현이다 — 통합 세션이 AlrApp.kt 의 placeholder
//   CatalogRoute 바디를 제거(또는 본 구현 호출로 위임)하면 충돌 없이 본 화면이 산다.
//   따라서 CatalogRoute 는 *확정 4-인자 시그니처를 한 글자도 안 바꾸고* 그대로 둔다.
//
//   "각 항목 설치 버튼(인라인 진행률)"은 install Flow 구독이 필요하므로, 그 능력을
//   내부 CatalogScreen(onInstall: ...) 으로 분리한다. Route 는 onInstall=null 로 호출 →
//   [설치]가 상세(onOpenAppDetail)로 위임(ADR-004 §7: 설치 트랜잭션은 상세에서).
//   통합 세션이 인라인 설치를 원하면 AlrApp.catalogDestination 에서 CatalogScreen 을
//   runtime::install 주입해 직접 호출하면 된다(Route 시그니처는 불변 유지).

/**
 * 카탈로그 화면 진입점 — ADR-004 §3. AlrApp 이 호출하며 순수 상태+콜백만 받는다(단방향).
 *
 * @param catalog          설치 가능 앱 목록(runtime.catalog() collect 결과).
 * @param installedAppIds  설치 완료 appId 집합(installedApps collect → map.toSet()). SSOT.
 * @param onOpenAppDetail  상세 진입(메타/스크린샷 + 설치·열기·제거는 AppDetail 트랙).
 * @param onBack           뒤로(런처로 pop).
 */
@Composable
fun CatalogRoute(
    catalog: List<CatalogApp>,
    installedAppIds: Set<String>,
    onOpenAppDetail: (String) -> Unit,
    onBack: () -> Unit,
) {
    // 확정 시그니처 보존: 인라인 설치 능력은 내부 화면으로 위임(여기선 미주입 → 상세 설치).
    CatalogScreen(
        catalog = catalog,
        installedAppIds = installedAppIds,
        onOpenAppDetail = onOpenAppDetail,
        onBack = onBack,
        onInstall = null,
    )
}

/**
 * 카탈로그 화면 본체 — Route 가 위임. 인라인 설치를 위해 install Flow 트리거를 받는다.
 *
 * @param onInstall (옵션) 설치 트리거 — appId 로 install Flow 를 *지연 생성*. 통합 세션이
 *   `runtime::install` 을 주입하면 카드 [설치] 버튼이 인라인 진행률을 구독한다. null 이면
 *   [설치]가 onOpenAppDetail 로 라우팅(상세에서 설치 — ADR-004 §7 기본 경로).
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CatalogScreen(
    catalog: List<CatalogApp>,
    installedAppIds: Set<String>,
    onOpenAppDetail: (String) -> Unit,
    onBack: () -> Unit,
    onInstall: ((String) -> Flow<InstallProgress>)? = null,
) {
    val scope = rememberCoroutineScope()

    var query by remember { mutableStateOf("") }
    var selectedCategory by remember { mutableStateOf<AppCategory?>(null) }
    var selectedSource by remember { mutableStateOf<AppSource?>(null) }

    // 항목별 인라인 설치 상태(appId → ItemInstallState). 진행 중인 설치만 들고,
    // 설치 완료/미설치는 installedAppIds(SSOT)로 판정한다.
    val installStates = remember { mutableStateMapOf<String, ItemInstallState>() }

    // 카탈로그에 존재하는 카테고리만 칩으로(빈 섹션 칩 방지).
    val presentCategories = remember(catalog) {
        AppCategory.entries.filter { cat -> catalog.any { it.category == cat } }
    }

    // 검색 + 카테고리 + 출처 AND 결합. 검색은 name/summary/description 부분일치(대소문자 무시).
    val filtered = remember(catalog, query, selectedCategory, selectedSource) {
        catalog.filter { app ->
            (selectedCategory == null || app.category == selectedCategory) &&
                (selectedSource == null || app.source == selectedSource) &&
                matchesCatalogQuery(app, query)
        }
    }

    // 카테고리별 섹션화(카테고리 필터가 걸리면 단일 섹션). enum 선언 순서를 따른다.
    val sections = remember(filtered) {
        filtered.groupBy { it.category }
            .toSortedMap(compareBy { it.ordinal })
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("앱 카탈로그") },
                navigationIcon = {
                    // 머터리얼 아이콘 아티팩트(material-icons-extended) 의존을 피해 텍스트
                    // 글리프로 둠 — 통합 세션이 Icons.AutoMirrored.Filled.ArrowBack 으로 교체 가능.
                    IconButton(onClick = onBack) {
                        Text("←", style = MaterialTheme.typography.titleLarge)
                    }
                },
            )
        },
    ) { padding ->
        Column(modifier = Modifier.padding(padding).fillMaxSize()) {
            OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                label = { Text("앱 검색") },
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 8.dp),
            )

            // 출처(번들/apt) + 카테고리 필터 칩 한 줄(가로 스크롤).
            FilterChipsRow(
                presentCategories = presentCategories,
                selectedCategory = selectedCategory,
                onSelectCategory = { selectedCategory = if (selectedCategory == it) null else it },
                selectedSource = selectedSource,
                onSelectSource = { selectedSource = if (selectedSource == it) null else it },
                hasBundled = remember(catalog) { catalog.any { it.source == AppSource.BUNDLED } },
                hasApt = remember(catalog) { catalog.any { it.source == AppSource.APT } },
            )

            if (filtered.isEmpty()) {
                EmptyResult(query = query)
            } else {
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    sections.forEach { (category, apps) ->
                        // 카테고리 필터가 걸려 단일 섹션이면 헤더 생략(중복).
                        if (selectedCategory == null) {
                            item(key = "section-${category.id}") {
                                SectionHeader(category, apps.size)
                            }
                        }
                        items(apps, key = { it.appId }) { app ->
                            val state = resolveItemState(app.appId, installedAppIds, installStates)
                            CatalogCard(
                                app = app,
                                state = state,
                                onOpenDetail = { onOpenAppDetail(app.appId) },
                                onInstallClick = {
                                    if (onInstall == null) {
                                        // 설치 트리거 미주입 → 상세에서 설치(위임).
                                        onOpenAppDetail(app.appId)
                                    } else {
                                        startInstall(
                                            appId = app.appId,
                                            installStates = installStates,
                                            launchInstall = onInstall,
                                            scope = scope,
                                        )
                                    }
                                },
                                onOpenInstalled = { onOpenAppDetail(app.appId) },
                            )
                        }
                    }
                }
            }
        }
    }
}

// --------------------------------------------------------------------------- //
// 설치 구동 — install Flow 구독 → installStates 갱신(단방향, 진행률만)
// --------------------------------------------------------------------------- //

private fun resolveItemState(
    appId: String,
    installedAppIds: Set<String>,
    installStates: Map<String, ItemInstallState>,
): ItemInstallState {
    // installedAppIds(SSOT) 우선 — 진행 중이라도 상위에서 설치 완료가 도착하면 Installed.
    if (appId in installedAppIds) return ItemInstallState.Installed
    return installStates[appId] ?: ItemInstallState.NotInstalled
}

private fun startInstall(
    appId: String,
    installStates: androidx.compose.runtime.snapshots.SnapshotStateMap<String, ItemInstallState>,
    launchInstall: (String) -> Flow<InstallProgress>,
    scope: kotlinx.coroutines.CoroutineScope,
) {
    // 이미 설치 중이면 무시(중복 트리거 방지).
    if (installStates[appId] is ItemInstallState.Installing) return
    installStates[appId] = ItemInstallState.Installing(0, "준비 중")
    scope.launch {
        launchInstall(appId).collect { progress ->
            installStates[appId] = when (progress) {
                is InstallProgress.Running ->
                    ItemInstallState.Installing(progress.percent, progress.stage.label)
                is InstallProgress.Done ->
                    // 상위 installedApps 갱신이 곧 도착 → SSOT(installedAppIds)가 Installed 로
                    // 수렴. 그 사이 표시는 Installed 로 둔다(레이스 시 깜빡임 방지).
                    ItemInstallState.Installed
                is InstallProgress.Failed ->
                    ItemInstallState.Failed(progress.message)
            }
        }
    }
}

// --------------------------------------------------------------------------- //
// 필터 칩
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FilterChipsRow(
    presentCategories: List<AppCategory>,
    selectedCategory: AppCategory?,
    onSelectCategory: (AppCategory) -> Unit,
    selectedSource: AppSource?,
    onSelectSource: (AppSource) -> Unit,
    hasBundled: Boolean,
    hasApt: Boolean,
) {
    LazyRow(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        // 출처 칩(번들 / apt) — 카탈로그에 해당 출처가 있을 때만.
        if (hasBundled) {
            item(key = "src-bundled") {
                FilterChip(
                    selected = selectedSource == AppSource.BUNDLED,
                    onClick = { onSelectSource(AppSource.BUNDLED) },
                    label = { Text(AppSource.BUNDLED.label) },
                    colors = FilterChipDefaults.filterChipColors(),
                )
            }
        }
        if (hasApt) {
            item(key = "src-apt") {
                FilterChip(
                    selected = selectedSource == AppSource.APT,
                    onClick = { onSelectSource(AppSource.APT) },
                    label = { Text(AppSource.APT.label) },
                    colors = FilterChipDefaults.filterChipColors(),
                )
            }
        }
        // 카테고리 칩.
        items(presentCategories, key = { "cat-${it.id}" }) { cat ->
            FilterChip(
                selected = selectedCategory == cat,
                onClick = { onSelectCategory(cat) },
                label = { Text(cat.label) },
                colors = FilterChipDefaults.filterChipColors(),
            )
        }
    }
}

// --------------------------------------------------------------------------- //
// 섹션 헤더
// --------------------------------------------------------------------------- //

@Composable
private fun SectionHeader(category: AppCategory, count: Int) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 6.dp, bottom = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = category.label,
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.primary,
        )
        Spacer(Modifier.width(8.dp))
        Text(
            text = "$count",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// --------------------------------------------------------------------------- //
// 카탈로그 카드 — 아이콘/이름/요약 + 상태별 설치/열기 액션 + 인라인 진행률
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CatalogCard(
    app: CatalogApp,
    state: ItemInstallState,
    onOpenDetail: () -> Unit,
    onInstallClick: () -> Unit,
    onOpenInstalled: () -> Unit,
) {
    Card(
        onClick = onOpenDetail,
        modifier = Modifier
            .fillMaxWidth()
            .semantics { contentDescription = "${app.name} ${statusContentDescription(state)}" },
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface,
        ),
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                AppIcon(app)
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            text = app.name,
                            style = MaterialTheme.typography.titleSmall,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Spacer(Modifier.width(6.dp))
                        SourceBadge(app.source)
                    }
                    Text(
                        text = app.summary,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        text = "다운로드 ${humanSize(app.totalInstallSizeBytes)}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Spacer(Modifier.width(8.dp))
                InstallAction(
                    state = state,
                    onInstallClick = onInstallClick,
                    onOpenInstalled = onOpenInstalled,
                )
            }

            // 인라인 진행률(설치 중일 때만).
            if (state is ItemInstallState.Installing) {
                Spacer(Modifier.height(8.dp))
                InstallingRow(state)
            }
            // 실패 줄.
            if (state is ItemInstallState.Failed) {
                Spacer(Modifier.height(6.dp))
                Text(
                    text = "설치 실패: ${state.message}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

@Composable
private fun InstallingRow(state: ItemInstallState.Installing) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(
                text = state.stageLabel,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = "${state.percent}%",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.height(4.dp))
        LinearProgressIndicator(
            progress = { (state.percent.coerceIn(0, 100)) / 100f },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

/** 상태별 우측 액션 버튼 — 미설치=[설치], 설치중=진행%, 설치됨=[열기], 실패=[다시 시도]. */
@Composable
private fun InstallAction(
    state: ItemInstallState,
    onInstallClick: () -> Unit,
    onOpenInstalled: () -> Unit,
) {
    when (state) {
        is ItemInstallState.NotInstalled ->
            OutlinedButton(onClick = onInstallClick) { Text("설치") }

        is ItemInstallState.Installing ->
            // 진행 중에는 비활성 표시(아래 줄의 진행바가 본체).
            Text(
                text = "${state.percent}%",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(horizontal = 12.dp),
            )

        is ItemInstallState.Installed ->
            TextButton(onClick = onOpenInstalled) { Text("열기") }

        is ItemInstallState.Failed ->
            OutlinedButton(onClick = onInstallClick) { Text("다시 시도") }
    }
}

// --------------------------------------------------------------------------- //
// 아이콘 / 배지 / 빈 상태
// --------------------------------------------------------------------------- //

/*
 * NOTE(통합 세션): AppIcon 은 *골격 자리표시* 다(LauncherScreen.appIconPainter 와 동일
 * 정책). compose + 이미지 로더 의존성 추가 시 app.iconPath 디코드 Painter 로 교체할 것
 * — 예: Coil rememberAsyncImagePainter(File(iconPath)), null/실패 시 카테고리 단색.
 */
@Composable
private fun AppIcon(app: CatalogApp) {
    Surface(
        modifier = Modifier.size(48.dp),
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.primaryContainer,
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = app.name.firstOrNull()?.uppercase() ?: "?",
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onPrimaryContainer,
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SourceBadge(source: AppSource) {
    AssistChip(
        onClick = {},
        enabled = false,
        label = { Text(source.label, style = MaterialTheme.typography.labelSmall) },
        colors = AssistChipDefaults.assistChipColors(
            disabledLabelColor = MaterialTheme.colorScheme.onSurfaceVariant,
        ),
        modifier = Modifier.height(24.dp),
    )
}

@Composable
private fun EmptyResult(query: String) {
    Box(
        modifier = Modifier.fillMaxSize().padding(32.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = if (query.isBlank()) "표시할 앱이 없어요" else "\"$query\" 검색 결과가 없어요",
                style = MaterialTheme.typography.titleMedium,
            )
            Text(
                text = "다른 검색어나 필터를 시도해 보세요",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

// --------------------------------------------------------------------------- //
// 순수 헬퍼 (런타임/리소스 독립)
// --------------------------------------------------------------------------- //

/**
 * 카탈로그 검색 규칙 — name/summary/description 부분일치(대소문자 무시).
 * apt 인덱스 메타(이름/요약/설명)를 클라이언트에서 거른다(파서 의도와 동일).
 */
private fun matchesCatalogQuery(app: CatalogApp, query: String): Boolean {
    val q = query.trim().lowercase()
    if (q.isEmpty()) return true
    return listOf(app.name, app.summary, app.description)
        .any { it.lowercase().contains(q) }
}

/** 바이트 → 사람이 읽는 크기(다운로드 X MB). totalInstallSizeBytes 표시용. */
private fun humanSize(bytes: Long): String {
    if (bytes <= 0) return "—"
    val kb = 1024.0
    val mb = kb * 1024
    val gb = mb * 1024
    return when {
        bytes >= gb -> String.format("%.1f GB", bytes / gb)
        bytes >= mb -> String.format("%.0f MB", bytes / mb)
        bytes >= kb -> String.format("%.0f KB", bytes / kb)
        else -> "$bytes B"
    }
}

/** 접근성 라벨용 상태 문구. */
private fun statusContentDescription(state: ItemInstallState): String = when (state) {
    is ItemInstallState.NotInstalled -> "미설치"
    is ItemInstallState.Installing -> "설치 중 ${state.percent}퍼센트"
    is ItemInstallState.Installed -> "설치됨"
    is ItemInstallState.Failed -> "설치 실패"
}

// --------------------------------------------------------------------------- //
// @Preview (더미 데이터 — FakeAlrRuntime 시드와 정합한 형태)
// --------------------------------------------------------------------------- //

private fun previewCatalog(): List<CatalogApp> = listOf(
    CatalogApp(
        appId = "org.gimp.GIMP",
        name = "GIMP",
        summary = "GNU 이미지 편집 프로그램",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gimp"),
        category = AppCategory.GRAPHICS,
        description = "레이어·필터·스크립트를 지원하는 풀기능 래스터 이미지 편집기.",
        rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "gimp-stage.tar", 320L * 1024 * 1024)),
        requiredPermissions = listOf(AppPermission.STORAGE_READ, AppPermission.STORAGE_WRITE),
        display = DisplaySpec(DisplaySpec.DisplayMode.FULLSCREEN),
        installSizeBytes = 320L * 1024 * 1024,
        source = AppSource.BUNDLED,
    ),
    CatalogApp(
        appId = "org.foot.foot",
        name = "foot",
        summary = "가벼운 Wayland 터미널 에뮬레이터",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/foot"),
        category = AppCategory.TERMINAL,
        description = "GPU 가속이 필요 없는 작고 빠른 Wayland 네이티브 터미널.",
        installSizeBytes = 6L * 1024 * 1024,
        source = AppSource.BUNDLED,
    ),
    CatalogApp(
        appId = "org.gtk.gtk3-demo",
        name = "GTK3 Demo",
        summary = "GTK3 위젯 데모",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gtk3-demo"),
        category = AppCategory.DEVELOPMENT,
        description = "GTK3 위젯·렌더링을 시연하는 참조 앱.",
        installSizeBytes = 24L * 1024 * 1024,
        source = AppSource.BUNDLED,
    ),
    CatalogApp(
        appId = "org.debian.nano",
        name = "nano",
        summary = "작고 친절한 콘솔 텍스트 편집기",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/nano"),
        category = AppCategory.UTILITY,
        description = "터미널에서 도는 간단한 텍스트 편집기.",
        installSizeBytes = 2L * 1024 * 1024,
        source = AppSource.APT,
    ),
    CatalogApp(
        appId = "org.debian.htop",
        name = "htop",
        summary = "인터랙티브 프로세스 뷰어",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/htop"),
        category = AppCategory.SYSTEM,
        description = "컬러 ncurses UI 의 프로세스 모니터.",
        installSizeBytes = 3L * 1024 * 1024,
        source = AppSource.APT,
    ),
)

/** 설치 진행을 흉내내는 Preview/데모용 install Flow(단조 증가 → Done). */
private fun fakeInstallFlow(appId: String): Flow<InstallProgress> = flow {
    emit(InstallProgress.Running(appId, 15, dev.chanwoo.androlinux.runtime.InstallStage.RESOLVING))
    emit(InstallProgress.Running(appId, 60, dev.chanwoo.androlinux.runtime.InstallStage.DOWNLOADING))
    emit(InstallProgress.Running(appId, 90, dev.chanwoo.androlinux.runtime.InstallStage.EXTRACTING))
    emit(InstallProgress.Done(appId))
}

// 기본/다크 Preview 는 인라인 설치를 시연하므로 CatalogScreen(onInstall 주입)을 직접 부른다.
// (CatalogRoute 는 확정 4-인자 시그니처라 onInstall 을 안 받음 — 상세 설치 위임 경로.)
@Preview(name = "Catalog — 기본(GIMP 설치됨, 인라인 설치)", showBackground = true)
@Composable
private fun CatalogScreenPreview() {
    AlrTheme {
        CatalogScreen(
            catalog = previewCatalog(),
            installedAppIds = setOf("org.gimp.GIMP"),
            onOpenAppDetail = {},
            onBack = {},
            onInstall = ::fakeInstallFlow,
        )
    }
}

@Preview(name = "Catalog — 다크(인라인 설치)", showBackground = true)
@Composable
private fun CatalogScreenDarkPreview() {
    AlrTheme(darkTheme = true) {
        CatalogScreen(
            catalog = previewCatalog(),
            installedAppIds = setOf("org.foot.foot"),
            onOpenAppDetail = {},
            onBack = {},
            onInstall = ::fakeInstallFlow,
        )
    }
}

@Preview(name = "Catalog — 빈 결과", showBackground = true)
@Composable
private fun CatalogRouteEmptyPreview() {
    AlrTheme {
        CatalogRoute(
            catalog = emptyList(),
            installedAppIds = emptySet(),
            onOpenAppDetail = {},
            onBack = {},
        )
    }
}

@Preview(name = "Catalog — 카드(설치 중) 단독", showBackground = true)
@Composable
private fun CatalogCardInstallingPreview() {
    AlrTheme {
        CatalogCard(
            app = previewCatalog().first { it.appId == "org.gtk.gtk3-demo" },
            state = ItemInstallState.Installing(60, "내려받는 중"),
            onOpenDetail = {},
            onInstallClick = {},
            onOpenInstalled = {},
        )
    }
}
