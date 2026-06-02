/*
 * LauncherScreen — ALR 홈 그리드(앱 런처) Jetpack Compose 골격.
 *
 * 런타임 독립: 이 화면은 게스트를 직접 실행하지 않는다. 실행/제거/카탈로그는 모두
 * 콜백(LauncherCallbacks)으로 위임하고, 통합 세션이 실제 런타임 구현을 주입한다.
 * (계약은 docs/design/launcher-ui.md §5 — LauncherRuntime 인터페이스.)
 *
 * 빌드 미통합: compose 의존성(androidx.compose.*, activity-compose, Material3 BOM)은
 * 아직 build.gradle 에 없다 — 통합 세션이 추가한다. 본 파일은 *소스 골격*이며 현재
 * 모듈에서 컴파일 대상이 아니다(import 미해소는 의도된 상태).
 *
 * 표시 모델(AppEntry)은 host-검증 파서 tools/desktop_entry.py 의 LauncherEntry 와
 * 필드 1:1 대응 — .desktop 파싱은 런타임 측에서 그 규칙으로 수행하고, 여기서는 순수
 * 표시만 한다.
 *
 * 소유: T3(런처/선택 UI). MainActivity.kt / AndroidManifest.xml 은 건드리지 않는다.
 */
package dev.chanwoo.androlinux.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
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
import androidx.compose.ui.unit.dp

// --------------------------------------------------------------------------- //
// 표시 모델 + 콜백 계약 (런타임 독립 — docs/design/launcher-ui.md §5)
// --------------------------------------------------------------------------- //

/**
 * 그리드 셀 하나가 표시하는 앱. tools/desktop_entry.py 의 LauncherEntry 와 필드 대응:
 *  appId←app_id, name←name, genericName←generic_name, comment←comment,
 *  iconPath←resolve_icon_path(icon), category←primary_category, terminal←terminal.
 *
 * iconPath: 안드로이드가 디코드할 절대 경로(없으면 null → 기본 아이콘). exec argv 는
 * 런처가 알 필요 없다(런타임이 appId 로 조회) — UI는 식별자와 표시 텍스트만 안다.
 */
data class AppEntry(
    val appId: String,
    val name: String,
    val genericName: String = "",
    val comment: String = "",
    val iconPath: String? = null,
    val category: String = "Other",
    val terminal: Boolean = false,
)

/** 런처 화면의 UI 상태(docs/design/launcher-ui.md §4). */
sealed interface LauncherUiState {
    /** 최초 진입 / 재스캔 중 — 스켈레톤. */
    data object Loading : LauncherUiState

    /** .desktop 0개(앱 미설치) — 빈 상태 + 카탈로그 CTA. */
    data object Empty : LauncherUiState

    /** 표시할 엔트리 + (옵션) 진행 중 설치율(appId→0..100) + 오프라인 여부. */
    data class Ready(
        val entries: List<AppEntry>,
        val installProgress: Map<String, Int> = emptyMap(),
        val offline: Boolean = false,
    ) : LauncherUiState
}

/**
 * 런처가 호출하는 사용자 액션. 실제 바디(게스트 실행/제거/카탈로그 열기)는 통합
 * 세션이 LauncherRuntime(§5) 으로 주입한다. 프리뷰/테스트는 no-op fake 로 구동.
 */
interface LauncherCallbacks {
    fun onLaunch(appId: String)
    fun onShowInfo(appId: String) {}
    fun onUninstall(appId: String) {}
    fun onOpenCatalog() {}
    fun onRefresh() {}
}

// --------------------------------------------------------------------------- //
// 화면
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LauncherScreen(
    state: LauncherUiState,
    callbacks: LauncherCallbacks,
    modifier: Modifier = Modifier,
) {
    Scaffold(
        modifier = modifier,
        topBar = { TopAppBar(title = { Text("AndroLinux") }) },
    ) { padding ->
        when (state) {
            is LauncherUiState.Loading -> CenterMessage("앱 목록을 불러오는 중…", padding)
            is LauncherUiState.Empty -> EmptyState(callbacks, padding)
            is LauncherUiState.Ready -> ReadyGrid(state, callbacks, padding)
        }
    }
}

@Composable
private fun ReadyGrid(
    state: LauncherUiState.Ready,
    callbacks: LauncherCallbacks,
    padding: PaddingValues,
) {
    var query by remember { mutableStateOf("") }
    var selectedCategory by remember { mutableStateOf(ALL_CATEGORY) }

    val categories = remember(state.entries) {
        listOf(ALL_CATEGORY) + state.entries.map { it.category }.distinct().sorted()
    }
    // 검색 + 카테고리 AND 결합. 매칭 규칙은 desktop_entry.matches_query 와 동일 의도
    // (name/generic/comment 부분일치, 대소문자 무시).
    val filtered = remember(state.entries, query, selectedCategory) {
        state.entries.filter { entry ->
            (selectedCategory == ALL_CATEGORY || entry.category == selectedCategory) &&
                matchesQuery(entry, query)
        }
    }

    Column(modifier = Modifier.padding(padding).fillMaxSize()) {
        if (state.offline) {
            OfflineBanner()
        }
        InstallProgressBanner(state.installProgress, state.entries)
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("검색") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
        )
        if (categories.size > 1) {
            CategoryChips(categories, selectedCategory) { selectedCategory = it }
        }
        LazyVerticalGrid(
            columns = GridCells.Adaptive(minSize = 96.dp),
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(filtered, key = { it.appId }) { entry ->
                AppCell(
                    entry = entry,
                    onLaunch = { callbacks.onLaunch(entry.appId) },
                    onLongPress = { callbacks.onShowInfo(entry.appId) },
                )
            }
        }
    }
}

@Composable
private fun AppCell(
    entry: AppEntry,
    onLaunch: () -> Unit,
    onLongPress: () -> Unit,
) {
    Column(
        modifier = Modifier
            .clickable(onClick = onLaunch)
            .padding(4.dp)
            .semantics { contentDescription = entry.name },
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        // 아이콘: iconPath 의 PNG/SVG 디코드는 통합 세션이 이미지 로더(Coil 등)로 주입.
        // 골격에서는 자리표시 박스만 둔다.
        Box(
            modifier = Modifier.size(64.dp),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                painter = appIconPainter(entry.iconPath),
                contentDescription = null,
            )
        }
        Text(
            text = entry.name,
            style = MaterialTheme.typography.labelMedium,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center,
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CategoryChips(
    categories: List<String>,
    selected: String,
    onSelect: (String) -> Unit,
) {
    LazyRow(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(categories, key = { it }) { cat ->
            FilterChip(
                selected = cat == selected,
                onClick = { onSelect(cat) },
                label = { Text(categoryLabel(cat)) },
                colors = FilterChipDefaults.filterChipColors(),
            )
        }
    }
}

@Composable
private fun InstallProgressBanner(progress: Map<String, Int>, entries: List<AppEntry>) {
    val inFlight = progress.entries.firstOrNull { it.value in 0..99 } ?: return
    val name = entries.firstOrNull { it.appId == inFlight.key }?.name ?: inFlight.key
    Column(modifier = Modifier.fillMaxWidth().padding(12.dp)) {
        Text("$name 설치 중 ${inFlight.value}%", style = MaterialTheme.typography.bodySmall)
        LinearProgressIndicator(
            progress = { inFlight.value / 100f },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
private fun OfflineBanner() {
    Text(
        text = "오프라인 — 설치된 앱만 실행할 수 있어요",
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.fillMaxWidth().padding(12.dp),
    )
}

@Composable
private fun EmptyState(callbacks: LauncherCallbacks, padding: PaddingValues) {
    Column(
        modifier = Modifier.fillMaxSize().padding(padding).padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("설치된 리눅스 앱이 없어요", style = MaterialTheme.typography.titleMedium)
        Text(
            "카탈로그에서 앱을 설치해 시작하세요",
            style = MaterialTheme.typography.bodyMedium,
        )
        // CTA: 카탈로그 열기(별도 트랙). clickable 텍스트로 골격 처리.
        Text(
            "+ 앱 설치",
            modifier = Modifier.clickable(onClick = callbacks::onOpenCatalog).padding(8.dp),
            style = MaterialTheme.typography.labelLarge,
        )
    }
}

@Composable
private fun CenterMessage(message: String, padding: PaddingValues) {
    Box(
        modifier = Modifier.fillMaxSize().padding(padding),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            CircularProgressIndicator()
            Text(message, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

// --------------------------------------------------------------------------- //
// 순수 헬퍼 (런타임/리소스 독립) — 통합 세션이 일부를 실제 구현으로 교체
// --------------------------------------------------------------------------- //

private const val ALL_CATEGORY = "*"

/** 카테고리 칩 한글 라벨. freedesktop Main Category → 표시명. */
private fun categoryLabel(cat: String): String = when (cat) {
    ALL_CATEGORY -> "전체"
    "Graphics" -> "그래픽"
    "Network" -> "네트워크"
    "System" -> "시스템"
    "Development" -> "개발"
    "Office" -> "오피스"
    "AudioVideo", "Audio", "Video" -> "미디어"
    "Game" -> "게임"
    "Utility" -> "유틸리티"
    "Settings" -> "설정"
    else -> cat
}

/**
 * desktop_entry.matches_query 와 동일 규칙(name/generic/comment 부분일치, 대소문자
 * 무시)의 클라이언트측 재현. 검색 필터는 host-검증된 파이썬 규칙과 의도를 일치시킨다.
 */
private fun matchesQuery(entry: AppEntry, query: String): Boolean {
    val q = query.trim().lowercase()
    if (q.isEmpty()) return true
    return listOf(entry.name, entry.genericName, entry.comment)
        .any { it.lowercase().contains(q) }
}

/*
 * NOTE(통합 세션): appIconPainter 는 *골격 자리표시* 다. compose + 이미지 로더 의존성
 * 추가 시 실제 구현으로 교체할 것 — 예: Coil 의 rememberAsyncImagePainter(File(path)),
 * 경로 null/디코드 실패 시 기본 앱 아이콘(painterResource). SVG 아이콘(.svg)은 svg
 * 디코더(coil-svg/AndroidSVG)를 등록해야 한다(파서는 largest-size PNG 를 우선 해석).
 */
@Composable
private fun appIconPainter(path: String?): androidx.compose.ui.graphics.painter.Painter {
    // 골격: 카테고리 색 단색 자리표시. 통합 시 path 디코드 Painter 로 교체.
    return androidx.compose.ui.graphics.painter.ColorPainter(MaterialTheme.colorScheme.primary)
}
