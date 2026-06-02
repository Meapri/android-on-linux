/*
 * AppDetailScreen — ALR 앱 상세 화면(설치 가능/설치됨 모두) Jetpack Compose 구현.
 *
 * 화면 책임(ADR-004 §3·§4 — AppDetail): 한 앱의 메타(아이콘/이름/설명/출처) + 크기
 * (정직 표기) + 권한 섹션(비root·앱-private 강점 설명) + .desktop/exec 진입점 정보를
 * 보이고, [설치]/[제거]/[열기] 액션을 제공한다. 진행률은 installProgress/uninstallProgress
 * 가 흘리는 Flow<InstallProgress>(§5-F) 를 collect 해 *인라인* 으로만 표시(ADR-004 §7
 * "UI는 진행률만 본다").
 *
 * 런타임 독립: 이 화면은 게스트를 직접 실행하지 않는다 — 설치/제거는 부모(AlrApp)가
 * 넘긴 지연 Flow 생성 람다로, 실행은 onOpen(InstalledApp) 콜백으로 위임한다. UI 는
 * runtime.AlrRuntime 인터페이스/모델(CatalogApp·InstalledApp·InstallProgress·AppPermission
 * 등 runtime/AppModels.kt)만 안다. 시그니처(AppDetailRoute)는 기반(ui/AlrApp.kt)이 확정한
 * 것을 그대로 구현한다.
 *
 * 권한 카피(정직): ADR-004 §9 + tools/permission_map.py 와 정합 — rootfs/overlay 는
 * 앱-private(filesDir)라 설치/실행/렌더는 *추가 권한 0*. 네트워크는 INTERNET(이미 선언,
 * 런타임 프롬프트 없음), 저장소는 SAF picker(권한 선언 불요), storage-write 는 SAF 강제.
 * 따라서 "이 앱은 추가 권한을 거의 요구하지 않습니다" 를 *근거와 함께* 정직히 보인다.
 *
 * 빌드 미통합: Compose 의존성(androidx.compose.*, Material3 BOM, kotlinx-coroutines)은
 * 통합 세션(D2)이 build.gradle 에 추가한다. 본 파일은 *소스 골격* 이며 의존성 추가
 * 전까지 import 미해소는 의도된 상태. build.gradle·MainActivity.kt·AndroidManifest.xml
 * 은 본 트랙이 건드리지 않는다.
 *
 * 소유: 화면 트랙(ui/AppDetailScreen.kt 신규 — 이 파일만). 시그니처 SSOT = ui/AlrApp.kt,
 * 데이터/계약 SSOT = ADR-004 §5-F + runtime/AppModels.kt.
 *
 * ★ 통합 세션(D2)에게 — placeholder 중복 제거: 기반 골격 ui/AlrApp.kt 가 같은 패키지에
 *   `fun AppDetailRoute(...)` placeholder 바디(ScreenPlaceholder)를 들고 있다. 본 파일이
 *   *같은 시그니처의 실 구현* 을 제공하므로, 두 top-level 선언이 충돌한다(redeclaration).
 *   해소 = 통합 세션이 ui/AlrApp.kt 의 AppDetailRoute placeholder 정의(현 L222–233)를
 *   삭제(또는 4개 placeholder 일괄 정리)한다 — AlrApp.kt 의 주석이 명시한 "Screens 트랙이
 *   시그니처를 그대로 구현" 의 마지막 절차다. AlrApp.kt 는 본 트랙 소유 밖이라 직접 못 지운다.
 */
package dev.chanwoo.androlinux.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.painter.ColorPainter
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import dev.chanwoo.androlinux.runtime.AppCategory
import dev.chanwoo.androlinux.runtime.AppPermission
import dev.chanwoo.androlinux.runtime.AppSource
import dev.chanwoo.androlinux.runtime.CatalogApp
import dev.chanwoo.androlinux.runtime.DisplaySpec
import dev.chanwoo.androlinux.runtime.InstallProgress
import dev.chanwoo.androlinux.runtime.InstallStage
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchEntry
import dev.chanwoo.androlinux.runtime.RootfsDep
import dev.chanwoo.androlinux.runtime.RootfsDepKind
import dev.chanwoo.androlinux.ui.theme.AlrTheme
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow

// --------------------------------------------------------------------------- //
// 화면 진입 시그니처 (기반 ui/AlrApp.kt 가 확정 — 그대로 구현)
// --------------------------------------------------------------------------- //

/**
 * 앱 상세 라우트 — ui/AlrApp.kt appDetailDestination 이 이 시그니처로 호출한다.
 *
 * @param appId 매니페스트 안정 id(라우트 인자). 메타 결손 시 폴백 제목.
 * @param catalogApp 카탈로그 메타(설명/스크린샷/출처/deps). null 이면 카탈로그에 없음.
 * @param installedApp 설치 상태(있으면 [열기]/[제거] 가능). null 이면 미설치.
 * @param installProgress [설치] 클릭 시 호출하는 *지연* Flow 생성자(runtime.install(appId)).
 *   클릭 전에는 구독하지 않아 부작용 없음 — 클릭 시점에 한 번 만들어 collect.
 * @param uninstallProgress [제거] 클릭 시 호출하는 지연 Flow 생성자(runtime.uninstall(appId)).
 * @param onOpen 실행 위임 — 설치된 앱을 [열기]. 부모가 LaunchRequest 로 RunningSurface 진입.
 * @param onBack 뒤로(네비 popBackStack).
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
    // 설치/제거 트랜잭션 상태 — 버튼 클릭 시 지연 Flow 를 *한 번* 구독(produceState).
    // null = 진행 중인 트랜잭션 없음. 클릭마다 새 Flow 인스턴스를 세팅해 재구독.
    var pendingInstall by remember(appId) { mutableStateOf<Flow<InstallProgress>?>(null) }
    var pendingUninstall by remember(appId) { mutableStateOf<Flow<InstallProgress>?>(null) }

    val installState = collectProgressOrNull(pendingInstall)
    val uninstallState = collectProgressOrNull(pendingUninstall)

    // 진행 중(트랜잭션이 살아 있고 아직 Done/Failed 가 아님) — 버튼 비활성/진행바.
    val installing = installState is InstallProgress.Running
    val uninstalling = uninstallState is InstallProgress.Running
    val busy = installing || uninstalling

    AppDetailContent(
        appId = appId,
        catalogApp = catalogApp,
        installedApp = installedApp,
        busy = busy,
        installState = installState,
        uninstallState = uninstallState,
        onInstall = { pendingInstall = installProgress() },
        onUninstall = { pendingUninstall = uninstallProgress() },
        onOpen = { installedApp?.let(onOpen) },
        onBack = onBack,
    )
}

/**
 * pending Flow 를 구독해 마지막 InstallProgress 를 돌려준다(없으면 null).
 *
 * produceState 호출은 *무조건* 일어나야 한다(Compose 호출부 안정성) — flow 가 null↔non-null
 * 로 토글돼도 호출 위치/개수가 안 바뀌게, nullable flow 를 그대로 key 로 넘긴다. flow 가
 * null 이면 collect 를 시작하지 않아 상태는 null 로 남는다. 새 클릭(새 Flow 인스턴스)이면
 * key 가 바뀌어 이전 수집이 취소되고 새로 구독한다.
 */
@Composable
private fun collectProgressOrNull(flow: Flow<InstallProgress>?): InstallProgress? {
    val state by produceState<InstallProgress?>(initialValue = null, key1 = flow) {
        value = null
        flow?.collect { value = it }
    }
    return state
}

// --------------------------------------------------------------------------- //
// 화면 본문
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppDetailContent(
    appId: String,
    catalogApp: CatalogApp?,
    installedApp: InstalledApp?,
    busy: Boolean,
    installState: InstallProgress?,
    uninstallState: InstallProgress?,
    onInstall: () -> Unit,
    onUninstall: () -> Unit,
    onOpen: () -> Unit,
    onBack: () -> Unit,
) {
    // 표시용 메타: 카탈로그 우선, 없으면 설치 정보, 둘 다 없으면 appId 폴백.
    val title = catalogApp?.name ?: installedApp?.name ?: appId
    val installed = installedApp != null

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        // 뒤로 — 아이콘 리소스 의존을 피해 텍스트 글리프(통합 시 Icons 로 교체 가능).
                        Text("‹", style = MaterialTheme.typography.headlineMedium)
                    }
                },
            )
        },
    ) { padding ->
        if (catalogApp == null && installedApp == null) {
            // 메타 결손(카탈로그·설치 모두 없음) — 정직한 빈 상태.
            MissingMeta(appId, padding)
            return@Scaffold
        }

        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp),
        ) {
            Spacer(Modifier.height(8.dp))
            HeaderRow(catalogApp, installedApp, title)

            Spacer(Modifier.height(16.dp))
            ActionRow(
                installed = installed,
                installable = catalogApp != null,
                busy = busy,
                onInstall = onInstall,
                onUninstall = onUninstall,
                onOpen = onOpen,
            )

            // 진행/실패 인라인(설치/제거 공통). ADR-004 §7 "UI는 진행률만 본다".
            TransactionInline(installState, uninstallState)

            Spacer(Modifier.height(20.dp))
            SizeSection(catalogApp, installedApp)

            catalogApp?.description?.takeIf { it.isNotBlank() }?.let { desc ->
                Spacer(Modifier.height(20.dp))
                SectionHeader("설명")
                Text(desc, style = MaterialTheme.typography.bodyMedium)
            }

            Spacer(Modifier.height(20.dp))
            PermissionSection(
                permissions = catalogApp?.requiredPermissions
                    ?: installedApp?.requiredPermissions
                    ?: emptyList(),
            )

            Spacer(Modifier.height(20.dp))
            EntryPointSection(
                entry = catalogApp?.entry ?: installedApp?.entry,
                display = catalogApp?.display ?: installedApp?.display ?: DisplaySpec(),
            )

            Spacer(Modifier.height(32.dp))
        }
    }
}

// --------------------------------------------------------------------------- //
// 헤더(아이콘 + 이름 + 출처/카테고리/버전)
// --------------------------------------------------------------------------- //

@Composable
private fun HeaderRow(
    catalogApp: CatalogApp?,
    installedApp: InstalledApp?,
    title: String,
) {
    val summary = catalogApp?.summary ?: installedApp?.summary ?: ""
    val category = catalogApp?.category ?: installedApp?.category ?: AppCategory.UTILITY
    val iconPath = catalogApp?.iconPath ?: installedApp?.iconPath
    val source = catalogApp?.source
    val version = installedApp?.version?.takeIf { it.isNotBlank() }

    Row(verticalAlignment = Alignment.CenterVertically) {
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = MaterialTheme.colorScheme.primaryContainer,
            modifier = Modifier.size(72.dp),
        ) {
            Box(contentAlignment = Alignment.Center) {
                // 아이콘: 통합 세션이 iconPath 디코드 Painter 로 교체(Coil 등). 골격은 자리표시.
                Icon(
                    painter = appIconPainter(iconPath),
                    contentDescription = null,
                    modifier = Modifier.size(40.dp),
                )
            }
        }
        Spacer(Modifier.width(16.dp))
        Column(modifier = Modifier.semantics { contentDescription = title }) {
            Text(title, style = MaterialTheme.typography.headlineSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (summary.isNotBlank()) {
                Text(
                    summary,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(6.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MetaChip(category.label)
                source?.let { MetaChip(it.label) }
                version?.let { MetaChip("v$it") }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MetaChip(label: String) {
    AssistChip(
        onClick = {},
        enabled = false,
        label = { Text(label, style = MaterialTheme.typography.labelSmall) },
        colors = AssistChipDefaults.assistChipColors(
            disabledLabelColor = MaterialTheme.colorScheme.onSurfaceVariant,
        ),
    )
}

// --------------------------------------------------------------------------- //
// 액션(설치 / 제거 / 열기)
// --------------------------------------------------------------------------- //

@Composable
private fun ActionRow(
    installed: Boolean,
    installable: Boolean,
    busy: Boolean,
    onInstall: () -> Unit,
    onUninstall: () -> Unit,
    onOpen: () -> Unit,
) {
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
        if (installed) {
            // 설치됨 — [열기] 주 액션 + [제거] 보조.
            Button(
                onClick = onOpen,
                enabled = !busy,
                modifier = Modifier.weight(1f),
            ) { Text("열기") }
            OutlinedButton(
                onClick = onUninstall,
                enabled = !busy,
                modifier = Modifier.weight(1f),
            ) { Text("제거") }
        } else {
            // 미설치 — [설치] 주 액션(카탈로그에 있을 때만).
            Button(
                onClick = onInstall,
                enabled = installable && !busy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(if (busy) "설치 중…" else "설치") }
        }
    }
}

/** 설치/제거 진행·실패 인라인 — 둘 중 살아 있는 트랜잭션을 보인다. */
@Composable
private fun TransactionInline(
    installState: InstallProgress?,
    uninstallState: InstallProgress?,
) {
    // 진행 중인 쪽 우선, 그다음 실패. 정상 Done 은 (목록 갱신으로 화면이 바뀌므로) 표시 생략.
    val active = installState ?: uninstallState ?: return
    when (active) {
        is InstallProgress.Running -> {
            Spacer(Modifier.height(12.dp))
            Column(modifier = Modifier.fillMaxWidth()) {
                Text(
                    "${active.stage.label} · ${active.percent}%",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(4.dp))
                LinearProgressIndicator(
                    progress = { active.percent.coerceIn(0, 100) / 100f },
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
        is InstallProgress.Failed -> {
            Spacer(Modifier.height(12.dp))
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = MaterialTheme.colorScheme.errorContainer,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    "실패: ${active.message}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                    modifier = Modifier.padding(12.dp),
                )
            }
        }
        is InstallProgress.Done -> Unit // 완료 — 화면 상태(설치됨/미설치)가 자연히 갱신.
    }
}

// --------------------------------------------------------------------------- //
// 크기(정직 표기 — "약 ≤ N MB")
// --------------------------------------------------------------------------- //

@Composable
private fun SizeSection(catalogApp: CatalogApp?, installedApp: InstalledApp?) {
    // 설치됨이면 실제 설치 크기, 아니면 카탈로그 다운로드 크기 합(totalInstallSizeBytes).
    val bytes = installedApp?.installedSizeBytes?.takeIf { it > 0 }
        ?: catalogApp?.totalInstallSizeBytes
        ?: 0L
    val installed = installedApp != null
    SectionHeader(if (installed) "설치 크기" else "다운로드 크기")
    Text(
        // 정직: 디스크/네트워크 변동·압축 차이를 숨기지 않고 "약 ≤" 로 상한 근사를 명시.
        text = if (bytes > 0) "약 ${humanSizeApprox(bytes)} 이하" else "크기 정보 없음",
        style = MaterialTheme.typography.bodyMedium,
    )
    catalogApp?.rootfsDeps?.takeIf { it.isNotEmpty() && !installed }?.let { deps ->
        Spacer(Modifier.height(6.dp))
        Text(
            text = deps.joinToString(" · ") { depLabel(it) },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun depLabel(dep: RootfsDep): String {
    val kind = when (dep.kind) {
        RootfsDepKind.STAGE_TAR -> "오버레이"
        RootfsDepKind.APT -> "apt"
    }
    return if (dep.installSizeBytes > 0) "$kind ${dep.ref}(${humanSizeApprox(dep.installSizeBytes)})"
    else "$kind ${dep.ref}"
}

// --------------------------------------------------------------------------- //
// 권한 섹션 (정직 — 비root·앱-private 강점, ADR-004 §9 + permission_map.py)
// --------------------------------------------------------------------------- //

@Composable
private fun PermissionSection(permissions: List<AppPermission>) {
    SectionHeader("권한")
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            // 헤드라인 — 비root/앱-private 강점을 정직히. (요구 권한이 0이면 더 강하게.)
            val headline = if (permissions.isEmpty()) {
                "이 앱은 추가 권한을 요구하지 않습니다"
            } else {
                "이 앱은 추가 권한을 거의 요구하지 않습니다"
            }
            Text(headline, style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(6.dp))
            Text(
                // 정직 근거: rootfs/overlay 는 앱-private(filesDir) → 설치·실행·렌더는 권한 0.
                "리눅스 환경은 이 앱의 개인 저장소(앱-private) 안에서 돌아갑니다. " +
                    "설치·실행·화면 표시에는 안드로이드 추가 권한이 필요 없습니다.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            if (permissions.isNotEmpty()) {
                Spacer(Modifier.height(12.dp))
                HorizontalDivider()
                Spacer(Modifier.height(12.dp))
                Text(
                    "이 앱이 기능을 쓸 때 요청할 수 있는 항목",
                    style = MaterialTheme.typography.labelLarge,
                )
                Spacer(Modifier.height(8.dp))
                // 각 권한 부류를 *정직한 안드로이드 처리 방식* 으로 설명(permission_map.py 정합).
                permissions.forEach { perm ->
                    PermissionRow(perm)
                    Spacer(Modifier.height(8.dp))
                }
            }
        }
    }
}

@Composable
private fun PermissionRow(perm: AppPermission) {
    Column {
        Text("• ${perm.label}", style = MaterialTheme.typography.bodyMedium)
        Text(
            permissionHonestNote(perm),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 14.dp),
        )
    }
}

/**
 * 권한 부류별 *정직한* 안드로이드 처리 설명 — tools/permission_map.py 매핑과 정합:
 *  - NETWORK   → INTERNET(install-time normal): 프롬프트 없음.
 *  - STORAGE_* → SAF picker: 권한 선언 불요, 사용자가 파일/폴더를 직접 고름. write 는 SAF 강제.
 *  - CAMERA/MIC/LOCATION → 런타임(dangerous) 권한: 처음 쓸 때만 시스템 프롬프트.
 *  - NOTIFICATIONS → Android 13+ 런타임 권한(그 이전은 불요).
 */
private fun permissionHonestNote(perm: AppPermission): String = when (perm) {
    AppPermission.NETWORK ->
        "네트워크는 설치 시 부여되는 일반 권한입니다 — 별도 허용 창이 뜨지 않습니다."
    AppPermission.STORAGE_READ ->
        "파일을 열 때 시스템 파일 선택기로 직접 고릅니다 — 저장소 권한을 요구하지 않습니다."
    AppPermission.STORAGE_WRITE ->
        "저장은 시스템 파일 선택기로 위치를 직접 지정합니다 — 광범위한 저장소 권한이 필요 없습니다."
    AppPermission.CAMERA ->
        "카메라는 처음 사용할 때 한 번만 안드로이드가 허용을 묻습니다."
    AppPermission.MICROPHONE ->
        "마이크는 처음 사용할 때 한 번만 안드로이드가 허용을 묻습니다."
    AppPermission.LOCATION ->
        "위치는 처음 사용할 때만 묻고, 대략/정밀 중 직접 선택할 수 있습니다."
    AppPermission.NOTIFICATIONS ->
        "알림은 진행 상황을 보여줄 때만 쓰며, Android 13 이상에서 한 번 묻습니다."
}

// --------------------------------------------------------------------------- //
// 진입점 섹션 (.desktop / exec — LaunchEntry)
// --------------------------------------------------------------------------- //

@Composable
private fun EntryPointSection(entry: LaunchEntry?, display: DisplaySpec) {
    if (entry == null) return
    SectionHeader("실행 정보")
    val kindLabel = when (entry.kind) {
        LaunchEntry.EntryKind.EXEC -> "실행 파일"
        LaunchEntry.EntryKind.DESKTOP -> ".desktop 진입점"
    }
    InfoLine(kindLabel, entry.target, mono = true)
    if (entry.kind == LaunchEntry.EntryKind.EXEC && entry.args.isNotEmpty()) {
        InfoLine("인자", entry.args.joinToString(" "), mono = true)
    }
    val modeLabel = when (display.mode) {
        DisplaySpec.DisplayMode.WINDOWED -> "창 모드"
        DisplaySpec.DisplayMode.FULLSCREEN -> "전체 화면"
    }
    val sizeHint = if (display.width > 0 && display.height > 0) {
        " · ${display.width}×${display.height}"
    } else {
        ""
    }
    InfoLine("표시", modeLabel + sizeHint)
}

@Composable
private fun InfoLine(label: String, value: String, mono: Boolean = false) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Text(
            label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(72.dp),
        )
        Text(
            value,
            style = if (mono) {
                MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace)
            } else {
                MaterialTheme.typography.bodySmall
            },
            modifier = Modifier.weight(1f),
        )
    }
}

// --------------------------------------------------------------------------- //
// 공통 소품
// --------------------------------------------------------------------------- //

@Composable
private fun SectionHeader(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium)
    Spacer(Modifier.height(8.dp))
}

@Composable
private fun MissingMeta(appId: String, padding: androidx.compose.foundation.layout.PaddingValues) {
    Column(
        modifier = Modifier.padding(padding).fillMaxSize().padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("앱 정보를 찾을 수 없어요", style = MaterialTheme.typography.titleMedium)
        Text(
            "‘$appId’ 항목이 카탈로그·설치 목록에 없습니다.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * 바이트 → 사람이 읽는 근사 크기(정직 표기의 숫자 부분). MB/GB 만 쓰고 한 자리 반올림.
 * 1 MiB = 1024*1024(설치 크기는 디스크 단위). "약 ≤" 접두는 호출부가 붙인다.
 */
private fun humanSizeApprox(bytes: Long): String {
    if (bytes <= 0) return "0 MB"
    val mib = bytes.toDouble() / (1024.0 * 1024.0)
    return if (mib >= 1024.0) {
        val gib = mib / 1024.0
        "%.1f GB".format(gib)
    } else if (mib >= 10.0) {
        "%.0f MB".format(mib)
    } else {
        "%.1f MB".format(mib)
    }
}

/*
 * NOTE(통합 세션): appIconPainter 는 LauncherScreen.kt 와 동일한 *골격 자리표시* 다
 * (중복 정의 회피 위해 여기서는 private 로 두지 않고 LauncherScreen 의 동명 헬퍼와
 * 충돌하지 않도록 파일-로컬 이름을 쓴다). compose 이미지 로더 의존성 추가 시 iconPath
 * 디코드 Painter(Coil 등)로 교체.
 */
@Composable
private fun appIconPainter(@Suppress("UNUSED_PARAMETER") path: String?): Painter =
    ColorPainter(MaterialTheme.colorScheme.onPrimaryContainer)

// --------------------------------------------------------------------------- //
// Preview (더미 데이터 — 의존성 통합 후 Android Studio 에서 렌더)
// --------------------------------------------------------------------------- //

private val PreviewGimp = CatalogApp(
    appId = "org.gimp.GIMP",
    name = "GIMP",
    summary = "GNU 이미지 편집 프로그램",
    entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gimp"),
    category = AppCategory.GRAPHICS,
    description = "레이어·필터·스크립트를 지원하는 풀기능 래스터 이미지 편집기. " +
        "터치로 캔버스 생성·브러시 그리기까지 검증되었습니다.",
    iconPath = "/usr/share/icons/hicolor/256x256/apps/gimp.png",
    rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "gimp-stage.tar", 320L * 1024 * 1024)),
    requiredPermissions = listOf(AppPermission.STORAGE_READ, AppPermission.STORAGE_WRITE),
    display = DisplaySpec(DisplaySpec.DisplayMode.FULLSCREEN),
    installSizeBytes = 320L * 1024 * 1024,
    source = AppSource.BUNDLED,
)

private val PreviewGimpInstalled = InstalledApp(
    appId = "org.gimp.GIMP",
    name = "GIMP",
    summary = "GNU 이미지 편집 프로그램",
    entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gimp"),
    category = AppCategory.GRAPHICS,
    iconPath = "/usr/share/icons/hicolor/256x256/apps/gimp.png",
    requiredPermissions = listOf(AppPermission.STORAGE_READ, AppPermission.STORAGE_WRITE),
    display = DisplaySpec(DisplaySpec.DisplayMode.FULLSCREEN),
    installedSizeBytes = 320L * 1024 * 1024,
    version = "3.0.2",
)

private val PreviewFoot = CatalogApp(
    appId = "org.foot.foot",
    name = "foot",
    summary = "가벼운 Wayland 터미널 에뮬레이터",
    entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/foot"),
    category = AppCategory.TERMINAL,
    description = "GPU 가속이 필요 없는 작고 빠른 Wayland 네이티브 터미널.",
    rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "foot-stage.tar", 6L * 1024 * 1024)),
    requiredPermissions = emptyList(),
    display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
    installSizeBytes = 6L * 1024 * 1024,
    source = AppSource.BUNDLED,
)

/** 미설치 + 권한 요구 앱(설치 버튼 + 권한 섹션). */
@Preview(name = "AppDetail · 미설치(GIMP)", showBackground = true)
@Composable
private fun PreviewAppDetailCatalog() {
    AlrTheme {
        AppDetailRoute(
            appId = PreviewGimp.appId,
            catalogApp = PreviewGimp,
            installedApp = null,
            installProgress = { emptyFlow() },
            uninstallProgress = { emptyFlow() },
            onOpen = {},
            onBack = {},
        )
    }
}

/** 설치됨(열기/제거) — 권한 요구 앱. */
@Preview(name = "AppDetail · 설치됨(GIMP)", showBackground = true)
@Composable
private fun PreviewAppDetailInstalled() {
    AlrTheme {
        AppDetailRoute(
            appId = PreviewGimp.appId,
            catalogApp = PreviewGimp,
            installedApp = PreviewGimpInstalled,
            installProgress = { emptyFlow() },
            uninstallProgress = { emptyFlow() },
            onOpen = {},
            onBack = {},
        )
    }
}

/** 권한 0 앱(foot) — "추가 권한을 요구하지 않습니다" 강한 카피. */
@Preview(name = "AppDetail · 권한 0(foot)", showBackground = true)
@Composable
private fun PreviewAppDetailNoPerms() {
    AlrTheme {
        AppDetailRoute(
            appId = PreviewFoot.appId,
            catalogApp = PreviewFoot,
            installedApp = null,
            installProgress = { emptyFlow() },
            uninstallProgress = { emptyFlow() },
            onOpen = {},
            onBack = {},
        )
    }
}

/** 설치 진행 중 인라인 진행바(Running 상태 고정). */
@Preview(name = "AppDetail · 설치 진행", showBackground = true)
@Composable
private fun PreviewAppDetailInstalling() {
    AlrTheme {
        // 진행 상태를 직접 주입하기 위해 본문 컴포저블을 고정 상태로 호출.
        AppDetailContent(
            appId = PreviewGimp.appId,
            catalogApp = PreviewGimp,
            installedApp = null,
            busy = true,
            installState = InstallProgress.Running(PreviewGimp.appId, 60, InstallStage.DOWNLOADING),
            uninstallState = null,
            onInstall = {},
            onUninstall = {},
            onOpen = {},
            onBack = {},
        )
    }
}
