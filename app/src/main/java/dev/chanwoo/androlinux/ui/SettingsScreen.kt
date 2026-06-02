/*
 * SettingsScreen — ALR 설정 화면 Jetpack Compose 구현(Material3).
 *
 * 네 구획으로 구성(ADR-004 §3 화면 플로우 / §0 결정 기록):
 *   1) 진단(Diagnostics)   — 현 MainActivity 의 ~40개 네이티브 probe 덤프(executionSummary)를
 *                            여기로 *격리*(ADR-004 §8 개선-4). 일반 사용자 경로에선 안 돌고,
 *                            개발자/지원용으로 이 화면에서만 노출. UI 는 라인 리스트만 표시.
 *   2) SAF 마운트 등록     — ACTION_OPEN_DOCUMENT_TREE 로 안드로이드 폴더를 골라 게스트
 *                            /mnt/android/<label> 로 노출(D4 SAF 프록시/copy 폴백). 등록된
 *                            마운트 라벨 목록 표시. tools/saf_bridge_model.py 의 SafMount
 *                            (SAF_MOUNT_ROOT="/mnt/android", guest_mount_point)와 1:1.
 *   3) 런타임 정보/버전     — 런타임/빌드 메타(읽기 전용 표시).
 *   4) 백그라운드 정책      — D5 placeholder. 백그라운드 suspend 정책은 device 측정(M-UX)
 *                            후 확정(ADR-004 §0-D5 / §6). 지금은 안내만.
 *
 * ★ 확정 시그니처(기반 트랙이 정함 — ui/AlrApp.kt settingsDestination 이 그대로 호출):
 *     @Composable fun SettingsRoute(installedApps: List<InstalledApp>, onBack: () -> Unit)
 *   그 SettingsRoute *선언* 은 ui/AlrApp.kt(placeholder)가 이미 소유한다 — 같은 package 에
 *   재선언하면 redeclaration 충돌이라, 본 트랙은 그 placeholder 가 위임할 실제 화면을
 *   SettingsScreenRoute(결선) + SettingsScreen(순수 화면)으로 제공한다. 통합 세션(D2)이
 *   AlrApp.kt 의 SettingsRoute 바디를 SettingsScreenRoute 호출로 한 줄 교체한다(아래 NOTE).
 *
 * 단방향 데이터흐름: SAF 마운트 등록부는 화면-로컬 UI 상태(MutableStateFlow)로 들고
 * collectAsState() 로 구독한다. 실제 SAF 트리 URI 영속/path-mediation 결선은 통합/런타임
 * (T5·C 트랙·WS-1)이 주입한다 — 이 화면은 *등록 명령(onPickFolder)* 과 *표시* 만 한다.
 * 진단 라인 공급(diagnostics)도 콜백으로 위임(통합 세션이 executionSummary 를 주입),
 * 기본은 빈 리스트(아직 미수집) → "수집 안 됨" 안내. runtime 을 화면에 직접 노출하지 않는다.
 *
 * 빌드 미통합: Compose 의존성은 통합 세션이 build.gradle 에 추가(이 파일은 의존성 추가
 * 전까지 import 미해소가 의도된 상태). ACTION_OPEN_DOCUMENT_TREE 의 실제 ActivityResult
 * 런처 결선(rememberLauncherForActivityResult)도 통합 세션 소유 — 이 화면은 picker 진입을
 * onPickFolder 콜백으로 위임해 Preview/테스트가 fake 로 구동되게 한다.
 * build.gradle·MainActivity.kt·AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: Screens 트랙(ui/SettingsScreen.kt). 기반 트랙의 AlrRuntime/AppModels/FakeAlrRuntime/
 * AlrApp 계약을 따른다(SettingsRoute 시그니처는 AlrApp.kt 가 SSOT).
 */
package dev.chanwoo.androlinux.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Divider
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import dev.chanwoo.androlinux.runtime.InstalledApp
import dev.chanwoo.androlinux.runtime.LaunchEntry
import dev.chanwoo.androlinux.runtime.AppCategory
import dev.chanwoo.androlinux.ui.theme.AlrTheme
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

// --------------------------------------------------------------------------- //
// 표시 모델 (런타임 독립)
// --------------------------------------------------------------------------- //

/**
 * 화면에 보이는 SAF 마운트 한 개 — tools/saf_bridge_model.py 의 SafMount 와 대응.
 *
 *  label       : 게스트에서의 마운트 이름(예 "downloads"). 형식 ^[A-Za-z0-9._-]+$.
 *  displayName : picker 가 돌려준 사람이 읽는 폴더명(없으면 label 표시).
 *  treeUri     : SAF tree URI 문자열(ACTION_OPEN_DOCUMENT_TREE 결과; 영속은 통합 측).
 *
 * guestPath 는 항상 /mnt/android/<label>(SAF_MOUNT_ROOT) — UI 가 합성해 표시한다
 * (saf_bridge_model.SafMount.guest_mount_point 와 동일 규칙).
 */
data class SafMountEntry(
    val label: String,
    val displayName: String = "",
    val treeUri: String = "",
) {
    /** 게스트 절대 마운트 경로(saf_bridge_model SAF_MOUNT_ROOT 미러). */
    val guestPath: String get() = "$SAF_MOUNT_ROOT/$label"
}

/** 게스트 SAF 마운트 부모 — saf_bridge_model.SAF_MOUNT_ROOT 와 동일(SSOT 미러). */
private const val SAF_MOUNT_ROOT = "/mnt/android"

/**
 * 설정 화면의 화면-로컬 상태 홀더(ViewModel 자리). 통합 세션이 이걸 화면별 ViewModel 로
 * 승격하거나 런타임에 결선한다 — 본 화면은 *읽기(StateFlow)* 와 *명령(register/remove)* 만 안다.
 *
 * SAF 마운트 등록부는 여기 화면-로컬로 둔다(D4 v1: 등록 명령은 UI, URI 영속/프록시는 런타임).
 * register() 는 picker 결과(label/uri)를 받아 등록부에 더한다(같은 label 이면 갱신).
 */
class SettingsUiState(
    initialMounts: List<SafMountEntry> = emptyList(),
) {
    private val _mounts = MutableStateFlow(initialMounts)
    val mounts: StateFlow<List<SafMountEntry>> = _mounts.asStateFlow()

    fun register(entry: SafMountEntry) {
        _mounts.value = _mounts.value.filterNot { it.label == entry.label } + entry
    }

    fun remove(label: String) {
        _mounts.value = _mounts.value.filterNot { it.label == label }
    }
}

// --------------------------------------------------------------------------- //
// Route — AlrApp.kt settingsDestination 이 호출하는 확정 시그니처
// --------------------------------------------------------------------------- //
//
// ★ 확정 시그니처 SettingsRoute(installedApps, onBack) 의 *선언* 은 기반 트랙의
//   ui/AlrApp.kt(line 237, placeholder 바디)가 이미 소유한다 — 같은 package 에 동일
//   2-인자 top-level 함수를 여기서 또 선언하면 redeclaration(컴파일 충돌)이다. 그래서
//   본 트랙은 *그 시그니처를 재선언하지 않고*, 그 placeholder 가 위임할 실제 화면
//   구현을 SettingsScreenRoute(상태/콜백 결선) + SettingsScreen(순수 화면)으로 제공한다.
//   통합 세션(D2)이 AlrApp.kt 의 SettingsRoute placeholder 바디를 아래 SettingsScreenRoute
//   호출로 *한 줄 교체* 한다(= 화면 결선). 시그니처 계약은 AlrApp.kt 가 그대로 SSOT.

/**
 * 설정 화면 결선 진입 — 기반 SettingsRoute(installedApps, onBack) placeholder 가
 * 위임할 실제 구현. 화면-로컬 SettingsUiState 를 remember 로 들고, picker 진입은
 * onPickFolder 로 위임한다(통합 세션이 ACTION_OPEN_DOCUMENT_TREE 런처를 결선; 등록 콜백만).
 *
 * @param installedApps 설치된 앱(런타임 정보 구획의 "설치 앱 N개·용량 합계" 표시용).
 * @param diagnostics 진단 라인(통합 세션이 executionSummary 로 주입; 기본 미수집=빈 리스트).
 * @param onPickFolder picker 진입 위임 — 통합 세션이 결과를 uiState.register 로 넘긴다.
 * @param onBack 상단 back — settingsDestination 이 navController.popBackStack() 을 넘긴다.
 */
@Composable
fun SettingsScreenRoute(
    installedApps: List<InstalledApp>,
    onBack: () -> Unit,
    diagnostics: List<DiagnosticLine> = emptyList(),
    uiState: SettingsUiState = remember { SettingsUiState() },
    onPickFolder: () -> Unit = {},
) {
    val mounts by uiState.mounts.collectAsState()
    SettingsScreen(
        installedApps = installedApps,
        mounts = mounts,
        diagnostics = diagnostics,
        onPickFolder = onPickFolder,
        onRemoveMount = { label -> uiState.remove(label) },
        onBack = onBack,
    )
}

// --------------------------------------------------------------------------- //
// 진단 라인 모델 (probe 덤프 격리)
// --------------------------------------------------------------------------- //

/**
 * 진단 한 줄 — 현 executionSummary 의 PASS/FAIL probe 라인을 구조화한 표시 모델.
 * 통합 세션이 네이티브 probe 결과(MainActivity L734~ executionSummary)를 이 형태로 매핑해
 * 주입한다. 본 화면은 라벨/상태/세부만 표시(평가는 안 함).
 */
data class DiagnosticLine(
    val label: String,
    val status: DiagnosticStatus,
    val detail: String = "",
)

enum class DiagnosticStatus(val label: String) {
    PASS("PASS"),
    FAIL("FAIL"),
    SKIP("SKIP"),
    INFO("INFO"),
}

// --------------------------------------------------------------------------- //
// 화면 본체
// --------------------------------------------------------------------------- //

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    installedApps: List<InstalledApp>,
    mounts: List<SafMountEntry>,
    diagnostics: List<DiagnosticLine>,
    onPickFolder: () -> Unit,
    onRemoveMount: (String) -> Unit,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text("설정") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        // 골격: 텍스트 백 글리프(통합 세션이 Icons.AutoMirrored.Filled.ArrowBack 으로 교체).
                        Text("‹", style = MaterialTheme.typography.headlineSmall)
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            SafMountSection(
                mounts = mounts,
                onPickFolder = onPickFolder,
                onRemoveMount = onRemoveMount,
            )
            RuntimeInfoSection(installedApps = installedApps)
            BackgroundPolicySection()
            DiagnosticsSection(diagnostics = diagnostics)
        }
    }
}

// --------------------------------------------------------------------------- //
// 구획 1: SAF 마운트 등록 (D4 — ACTION_OPEN_DOCUMENT_TREE)
// --------------------------------------------------------------------------- //

@Composable
private fun SafMountSection(
    mounts: List<SafMountEntry>,
    onPickFolder: () -> Unit,
    onRemoveMount: (String) -> Unit,
) {
    SettingsCard(
        title = "파일 연동 (SAF)",
        subtitle = "안드로이드 폴더를 골라 리눅스 앱에 $SAF_MOUNT_ROOT/<라벨> 로 노출합니다.",
    ) {
        FilledTonalButton(
            onClick = onPickFolder,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("+ 폴더 추가 (안드로이드 문서 선택)")
        }
        // ACTION_OPEN_DOCUMENT_TREE 는 권한 *선언 불요* — picker 위임(ADR-004 §9). 안내 명시.
        Text(
            text = "폴더 선택은 시스템 picker 가 맡아요 — 별도 권한 요청이 없습니다.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
        )
        Spacer(Modifier.height(4.dp))
        if (mounts.isEmpty()) {
            EmptyHint("아직 등록된 폴더가 없어요.")
        } else {
            Text(
                text = "등록된 마운트 ${mounts.size}개",
                style = MaterialTheme.typography.labelLarge,
            )
            mounts.forEach { mount ->
                SafMountRow(mount = mount, onRemove = { onRemoveMount(mount.label) })
            }
        }
    }
}

@Composable
private fun SafMountRow(
    mount: SafMountEntry,
    onRemove: () -> Unit,
) {
    Surface(
        tonalElevation = 1.dp,
        shape = RoundedCornerShape(10.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = mount.displayName.ifBlank { mount.label },
                    style = MaterialTheme.typography.bodyLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                // 게스트가 보는 경로(/mnt/android/<label>) — saf_bridge_model 규칙 미러.
                Text(
                    text = mount.guestPath,
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    color = MaterialTheme.colorScheme.primary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            OutlinedButton(onClick = onRemove) { Text("해제") }
        }
    }
}

// --------------------------------------------------------------------------- //
// 구획 2: 런타임 정보 / 버전
// --------------------------------------------------------------------------- //

@Composable
private fun RuntimeInfoSection(installedApps: List<InstalledApp>) {
    val totalBytes = remember(installedApps) { installedApps.sumOf { it.installedSizeBytes } }
    SettingsCard(title = "런타임 정보") {
        InfoRow("런타임", "ALR (Android Linux Runtime)")
        InfoRow("실행 환경", "Ubuntu noble 24.04 · glibc 2.39 · arm64")
        InfoRow("컴포지터", "Wayland (in-process)")
        InfoRow("설치 앱", "${installedApps.size}개 · ${formatBytes(totalBytes)}")
        // 빌드/버전 스탬프는 통합 세션이 BuildConfig 로 채운다(이 트랙은 version stamp 불변).
        InfoRow("빌드", "통합 세션 주입 (BuildConfig)")
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f))
        Spacer(Modifier.width(12.dp))
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f, fill = false),
            textAlign = androidx.compose.ui.text.style.TextAlign.End,
        )
    }
}

// --------------------------------------------------------------------------- //
// 구획 3: 백그라운드 정책 (D5 placeholder)
// --------------------------------------------------------------------------- //

@Composable
private fun BackgroundPolicySection() {
    SettingsCard(title = "백그라운드 정책") {
        Row(verticalAlignment = Alignment.CenterVertically) {
            AssistChip(onClick = {}, enabled = false, label = { Text("측정 후 확정") })
        }
        Text(
            text = "지금은 포그라운드 1개 앱만 실행되고, 나머지는 보류(suspend)되거나 정지합니다. " +
                "백그라운드 유지 정책은 device 측정(M-UX) 후 확정됩니다.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
        )
    }
}

// --------------------------------------------------------------------------- //
// 구획 4: 진단 (probe 덤프 격리 — 개발자/지원용)
// --------------------------------------------------------------------------- //

@Composable
private fun DiagnosticsSection(diagnostics: List<DiagnosticLine>) {
    SettingsCard(
        title = "진단",
        subtitle = "네이티브 probe 결과 — 개발자/지원용. 일반 실행 경로에선 수집하지 않습니다.",
    ) {
        if (diagnostics.isEmpty()) {
            EmptyHint("진단이 아직 수집되지 않았어요.")
        } else {
            val passCount = remember(diagnostics) {
                diagnostics.count { it.status == DiagnosticStatus.PASS }
            }
            Text(
                text = "$passCount/${diagnostics.size} PASS",
                style = MaterialTheme.typography.labelLarge,
            )
            // probe 덤프는 길어질 수 있으니 자체 스크롤 영역으로 가둔다(바깥 verticalScroll 과
            // 충돌 없게 heightIn 으로 상한). 통합 세션이 executionSummary 라인을 주입.
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(max = 320.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                diagnostics.forEach { line -> DiagnosticRow(line) }
            }
        }
    }
}

@Composable
private fun DiagnosticRow(line: DiagnosticLine) {
    val color = when (line.status) {
        DiagnosticStatus.PASS -> MaterialTheme.colorScheme.primary
        DiagnosticStatus.FAIL -> MaterialTheme.colorScheme.error
        DiagnosticStatus.SKIP -> MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f)
        DiagnosticStatus.INFO -> MaterialTheme.colorScheme.secondary
    }
    Row(verticalAlignment = Alignment.Top) {
        Text(
            text = line.status.label,
            style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
            color = color,
            modifier = Modifier.width(40.dp),
        )
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = line.label,
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            )
            if (line.detail.isNotBlank()) {
                Text(
                    text = line.detail,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f),
                )
            }
        }
    }
}

// --------------------------------------------------------------------------- //
// 공통 구획 컴포넌트
// --------------------------------------------------------------------------- //

@Composable
private fun SettingsCard(
    title: String,
    subtitle: String? = null,
    content: @Composable () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(),
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            if (subtitle != null) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
                )
            }
            Divider()
            content()
        }
    }
}

@Composable
private fun EmptyHint(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.5f),
        modifier = Modifier.padding(vertical = 4.dp),
    )
}

// --------------------------------------------------------------------------- //
// 순수 헬퍼
// --------------------------------------------------------------------------- //

/** 바이트 → 사람이 읽는 용량(런처/카탈로그와 일관된 표시). */
private fun formatBytes(bytes: Long): String {
    if (bytes <= 0) return "0 B"
    val units = listOf("B", "KB", "MB", "GB")
    var value = bytes.toDouble()
    var unit = 0
    while (value >= 1024.0 && unit < units.lastIndex) {
        value /= 1024.0
        unit++
    }
    return if (unit == 0) "${bytes} B" else String.format("%.1f %s", value, units[unit])
}

// --------------------------------------------------------------------------- //
// Preview (더미 데이터) — @Preview 포함, FakeAlrRuntime 비의존(순수 더미)
// --------------------------------------------------------------------------- //

private fun previewInstalledApps(): List<InstalledApp> = listOf(
    InstalledApp(
        appId = "org.gimp.GIMP",
        name = "GIMP",
        summary = "GNU 이미지 편집 프로그램",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gimp"),
        category = AppCategory.GRAPHICS,
        installedSizeBytes = 320L * 1024 * 1024,
        version = "3.0.2",
    ),
    InstalledApp(
        appId = "org.foot.foot",
        name = "foot",
        summary = "가벼운 Wayland 터미널",
        entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/foot"),
        category = AppCategory.TERMINAL,
        installedSizeBytes = 6L * 1024 * 1024,
        version = "1.16.2",
    ),
)

private fun previewMounts(): List<SafMountEntry> = listOf(
    SafMountEntry(label = "downloads", displayName = "다운로드", treeUri = "content://…/downloads"),
    SafMountEntry(label = "dcim", displayName = "DCIM", treeUri = "content://…/dcim"),
)

private fun previewDiagnostics(): List<DiagnosticLine> = listOf(
    DiagnosticLine("native-loader: glibc static exec", DiagnosticStatus.PASS, "v76"),
    DiagnosticLine("seccomp-trace path rewrite", DiagnosticStatus.PASS, "rootfs /etc"),
    DiagnosticLine("wayland compositor start", DiagnosticStatus.PASS),
    DiagnosticLine("Vulkan device probe", DiagnosticStatus.INFO, "Mali-G615 (VK 1.3)"),
    DiagnosticLine("PRoot dpkg clone3", DiagnosticStatus.SKIP, "in-process loader 사용"),
    DiagnosticLine("qt6 EGL gap", DiagnosticStatus.FAIL, "device-pending"),
)

@Preview(name = "Settings — 데이터 있음", showBackground = true)
@Composable
private fun SettingsScreenPreview() {
    AlrTheme {
        SettingsScreen(
            installedApps = previewInstalledApps(),
            mounts = previewMounts(),
            diagnostics = previewDiagnostics(),
            onPickFolder = {},
            onRemoveMount = {},
            onBack = {},
        )
    }
}

@Preview(name = "Settings — 빈 상태", showBackground = true)
@Composable
private fun SettingsScreenEmptyPreview() {
    AlrTheme {
        SettingsScreen(
            installedApps = emptyList(),
            mounts = emptyList(),
            diagnostics = emptyList(),
            onPickFolder = {},
            onRemoveMount = {},
            onBack = {},
        )
    }
}

@Preview(name = "Settings — 다크", showBackground = true)
@Composable
private fun SettingsScreenDarkPreview() {
    AlrTheme(darkTheme = true) {
        SettingsScreen(
            installedApps = previewInstalledApps(),
            mounts = previewMounts(),
            diagnostics = previewDiagnostics(),
            onPickFolder = {},
            onRemoveMount = {},
            onBack = {},
        )
    }
}
