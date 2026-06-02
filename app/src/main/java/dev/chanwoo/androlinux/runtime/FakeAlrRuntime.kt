/*
 * FakeAlrRuntime — AlrRuntime 의 mock 구현(host/Compose Preview/테스트 구동용).
 *
 * UI 4트랙은 device 런타임 없이 이 mock 에 대고 개발한다. 실제 게스트를 실행하지
 * 않고(in-process 로더/컴포지터 미호출), 상태 전이만 코루틴 타이머로 흘린다.
 *
 * ★ 핵심: ADR-004 §5-F INV-1~3(단일 포그라운드 불변식)을 *실제로* 구현한다 —
 *   launch()/requestForeground() 가 RENDERING 진입 전에 기존 RENDERING 세션을
 *   원자적으로 BACKGROUND 로 양도한다. 따라서 어느 시점에도 RENDERING ≤ 1 이 성립.
 *   이 전이 로직(양도)이 통합 런타임이 그대로 따라야 할 *불변식 레퍼런스* 다.
 *
 * 시드: 설치앱 = GIMP/foot/netsurf(device 증명 — 메모리 device-evidence/GUI-polish),
 *       카탈로그 = 위 셋 + gtk3-demo/sdl2(device 증명) + 몇 개 더(설치 가능).
 *
 * 빌드 미통합: Compose 의존성은 통합 세션이 build.gradle 에 추가(이 파일은 Compose
 * 비의존 — 순수 코루틴/StateFlow + android.view.SurfaceHolder no-op 바인드).
 * build.gradle·MainActivity.kt·AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: 기반 트랙(runtime/ 신규).
 */
package dev.chanwoo.androlinux.runtime

import android.view.SurfaceHolder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

// --------------------------------------------------------------------------- //
// 선택 능력(capability) 인터페이스 — 기본 AlrRuntime(§5-F) 계약 *밖* 의 데모/UX 신호.
//   ViewModel 이 `runtime as? CatalogExtras` / `as? InstallQueueInfo` 로 안전 다운캐스트해
//   끌어 쓰고, 없으면 graceful 폴백. 이렇게 AlrRuntime 인터페이스는 불변(WS-1 수락)으로 두고
//   FakeAlrRuntime 만 오프라인/큐 같은 풍부한 상태를 제공한다.
// --------------------------------------------------------------------------- //

/** 오프라인 모드 토글 — catalog() fetch 실패를 시뮬레이션(카탈로그 ViewModel 의 [재시도] 대상). */
interface CatalogExtras {
    /** 현재 오프라인 여부(StateFlow — 화면이 배너를 즉시 반영). */
    val offline: StateFlow<Boolean>

    /** 오프라인 토글. true 면 catalog() 가 빈 목록 대신 예외를 흘려 ViewModel 이 offline 로 폴백. */
    fun setOffline(value: Boolean)
}

/** 설치 큐 가시성 — 동시 1 + 대기열(ViewModel 자체 큐와 별개로 런타임 측 큐 상태를 노출). */
interface InstallQueueInfo {
    /** 현재 설치 중인 appId(없으면 null). */
    val activeInstall: StateFlow<String?>

    /** 설치 대기열(FIFO, 앞이 다음 차례). */
    val installQueue: StateFlow<List<String>>
}

/** 테스트/데모용 CRASHED 주입 — 실행 중 세션을 비정상 종료로 전이(런처/상세의 "앱 종료됨" 훅). */
interface CrashSimulator {
    /** appId 의 현재 세션을 CRASHED 로 전이(있을 때만). INV-1~3 유지(RENDERING 자리 비움). */
    fun simulateCrash(appId: String)
}

/**
 * AlrRuntime 의 인-메모리 mock.
 *
 * @param scope 상태 전이 코루틴이 도는 스코프(기본: SupervisorJob 독립 스코프).
 *   Compose Preview/테스트는 자기 TestScope 를 주입할 수 있다.
 * @param stepMillis 가짜 상태/진행 전이 간격(테스트는 0 으로 즉시 진행).
 * @param failingInstalls 설치가 *처음* 실패하도록 시드할 appId 집합(재시도 시뮬). 재시도하면 성공.
 */
class FakeAlrRuntime(
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob()),
    private val stepMillis: Long = 350,
    failingInstalls: Set<String> = emptySet(),
) : AlrRuntime, CatalogExtras, InstallQueueInfo, CrashSimulator {

    private val _installedApps = MutableStateFlow(seedInstalledApps())
    override val installedApps: StateFlow<List<InstalledApp>> = _installedApps.asStateFlow()

    private val _sessions = MutableStateFlow<List<FakeAppSession>>(emptyList())
    // 외부에는 AppSession 리스트로만 노출(불변식은 내부에서 강제).
    private val _sessionsPublic = MutableStateFlow<List<AppSession>>(emptyList())
    override val sessions: StateFlow<List<AppSession>> = _sessionsPublic.asStateFlow()

    private val _catalog = seedCatalog()

    /** 모든 세션 상태 전이를 직렬화 — INV-2(원자적 양도)의 임계구역. */
    private val sessionLock = Mutex()

    // --- CatalogExtras: 오프라인 ---------------------------------------------- //
    private val _offline = MutableStateFlow(false)
    override val offline: StateFlow<Boolean> = _offline.asStateFlow()
    override fun setOffline(value: Boolean) { _offline.value = value }

    // --- InstallQueueInfo: 동시 1 + 대기열 ----------------------------------- //
    private val _activeInstall = MutableStateFlow<String?>(null)
    override val activeInstall: StateFlow<String?> = _activeInstall.asStateFlow()
    private val _installQueue = MutableStateFlow<List<String>>(emptyList())
    override val installQueue: StateFlow<List<String>> = _installQueue.asStateFlow()

    /** 설치 큐 직렬화 — install() 진입/완료 시 active/queue 를 원자적으로 갱신. */
    private val installLock = Mutex()

    /** 재시도 시뮬: 여기 있는 appId 는 첫 install 에서 실패하고, 두 번째(재시도)에 성공한다. */
    private val pendingFailures = failingInstalls.toMutableSet()

    /**
     * catalog() — 오프라인이면 예외를 흘려(ViewModel 이 catch → offline 폴백) 데모. 온라인이면
     * 시드 카탈로그. offline StateFlow 를 map 으로 흘려 토글이 즉시 재방출되게 한다.
     */
    override fun catalog(): Flow<List<CatalogApp>> = _offline.map { isOffline ->
        if (isOffline) throw java.io.IOException("카탈로그 인덱스를 가져오지 못했습니다 (오프라인)")
        _catalog
    }

    // ----------------------------------------------------------------------- //
    // launch — INV-1~3 강제
    // ----------------------------------------------------------------------- //

    override fun launch(req: LaunchRequest): AppSession {
        val session = FakeAppSession(req.appId, this)
        scope.launch {
            sessionLock.withLock {
                addSession(session)
                session.setState(SessionState.STARTING)
            }
            // 가짜 기동 지연.
            delay(stepMillis)
            // RENDERING 진입 전, 기존 RENDERING 세션을 BACKGROUND 로 *먼저* 양도(INV-2).
            promoteToForeground(session)
        }
        return session
    }

    /**
     * 설치 트랜잭션 — *동시 1 + 대기열* 정책(InstallQueueInfo). install Flow 를 collect 하기
     * 시작할 때 큐에 들어가고, 앞선 설치가 끝나 active 슬롯이 비면 진행한다. 즉:
     *   - 슬롯이 비어 있으면 즉시 active 가 되어 단계별 진행률을 흘린다.
     *   - 차 있으면 _installQueue 에 들어가 대기(ViewModel 은 Queued(position) 로 표시), 앞이
     *     비면 자동 승격(여기 acquireSlot 의 polling 으로).
     * 재시도 시뮬: pendingFailures 에 있으면 첫 시도는 Failed 로 끝나고(슬롯 반납) 그 appId 를
     *   집합에서 지운다 → 같은 Flow 를 다시 collect(재시도)하면 성공한다.
     *
     * §5 계약(ADR-004 §5/D3, v2): 실 AlrRuntime.install() 은 진행 단계를
     *   RESOLVING→DOWNLOADING→UNPACKING→CONFIGURING→REGISTERING 으로 흘리되, 이를
     *   NativeCommandRunner 의 비root `dpkg -i <local.deb>` 트랜잭션에 매핑한다 —
     *   UNPACKING=dpkg unpack(unpacked=true), CONFIGURING=dpkg configure(maintainer
     *   script "Setting up …" → configured=true), 완료=installed. v2 device-증명
     *   (ws-1 7f45def, v163: hello.deb unpacked+configured+installed). 깊은 fork+exec
     *   체인(galculator)·네트워크 다중-패키지 apt 는 잔존 벽(PENDING). v1 stage-tar
     *   (extractOverlayTar) 는 오프라인/번들 폴백으로 같은 단계열을 재사용.
     *   이 mock 은 device 없이 그 단계열만 코루틴 타이머로 시뮬레이션한다.
     *
     * INV 와 무관(설치는 세션 불변식 임계구역과 별개) — 단 active/queue StateFlow 로 UI 가시.
     */
    override fun install(appId: String): Flow<InstallProgress> = flow {
        val catalogApp = _catalog.firstOrNull { it.appId == appId }
        if (catalogApp == null) {
            emit(InstallProgress.Failed(appId, "카탈로그에 없는 앱: $appId"))
            return@flow
        }
        if (_installedApps.value.any { it.appId == appId }) {
            // 이미 설치됨 — 멱등 완료.
            emit(InstallProgress.Done(appId))
            return@flow
        }
        // 동시 1 슬롯 확보(대기열 경유). 확보 전까지는 emit 없이 대기.
        acquireInstallSlot(appId)
        try {
            // 재시도 시뮬: 첫 시도 실패(슬롯 반납), 집합에서 제거 → 재시도 성공.
            if (pendingFailures.remove(appId)) {
                emit(InstallProgress.Running(appId, 15, InstallStage.RESOLVING))
                delay(stepMillis)
                emit(InstallProgress.Failed(appId, "오버레이 적용 실패 (재시도하세요)"))
                return@flow
            }
            // 단계별 진행률(단조 증가) — v2 dpkg/apt 파이프라인 반영:
            //   RESOLVING(의존성/closure) → DOWNLOADING(.deb 수신) →
            //   UNPACKING(dpkg unpack: unpacked=true) →
            //   CONFIGURING(dpkg configure: maintainer script → configured=true) →
            //   REGISTERING(installed; status="install ok installed").
            // 실 AlrRuntime 은 §5 계약대로 이 단계들을 NativeCommandRunner 의
            // 비root `dpkg -i <local.deb>` 진행에 매핑한다(device-증명 7f45def, v163:
            // unpacked=true configured=true installed=true). mock 은 타이머로 흘린다.
            val stages = listOf(
                InstallStage.RESOLVING to 15,
                InstallStage.DOWNLOADING to 55,
                InstallStage.UNPACKING to 80,    // dpkg unpack → unpacked=true
                InstallStage.CONFIGURING to 95,  // dpkg configure → configured=true
                InstallStage.REGISTERING to 99,
            )
            for ((stage, pct) in stages) {
                emit(InstallProgress.Running(appId, pct, stage))
                delay(stepMillis)
            }
            // 설치 완료(installed) → installedApps 에 등록(카탈로그 메타에서 InstalledApp 합성).
            _installedApps.value = _installedApps.value + catalogApp.toInstalledApp()
            emit(InstallProgress.Done(appId))
        } finally {
            // 성공/실패/취소 어느 경로든 슬롯 반납 + 다음 대기 항목 승격.
            releaseInstallSlot(appId)
        }
    }

    // --- 설치 큐 헬퍼(동시 1 + 대기열) --------------------------------------- //

    /**
     * active 슬롯을 잡을 때까지 대기. 비어 있으면 즉시 active 로 등록, 차 있으면 대기열에 넣고
     * 폴링으로 자기 차례(대기열 맨 앞 + active 비었음)를 기다린다. 폴링 간격은 stepMillis 의 일부.
     */
    private suspend fun acquireInstallSlot(appId: String) {
        installLock.withLock {
            if (_activeInstall.value == null && _installQueue.value.isEmpty()) {
                _activeInstall.value = appId
                return
            }
            if (appId !in _installQueue.value) {
                _installQueue.value = _installQueue.value + appId
            }
        }
        // 내 차례가 올 때까지 대기(대기열 맨 앞 + 슬롯 빔).
        while (true) {
            installLock.withLock {
                if (_activeInstall.value == null && _installQueue.value.firstOrNull() == appId) {
                    _installQueue.value = _installQueue.value.drop(1)
                    _activeInstall.value = appId
                    return
                }
            }
            delay(if (stepMillis > 0) stepMillis / 4 + 1 else 1)
        }
    }

    /** active 슬롯 반납 — 내가 active 였으면 비운다(다음 대기 항목이 acquire 폴링에서 승격). */
    private suspend fun releaseInstallSlot(appId: String) {
        installLock.withLock {
            if (_activeInstall.value == appId) {
                _activeInstall.value = null
            }
        }
    }

    override fun uninstall(appId: String): Flow<InstallProgress> = flow {
        if (_installedApps.value.none { it.appId == appId }) {
            emit(InstallProgress.Failed(appId, "설치되지 않은 앱: $appId"))
            return@flow
        }
        emit(InstallProgress.Running(appId, 50, InstallStage.REMOVING))
        delay(stepMillis)
        _installedApps.value = _installedApps.value.filterNot { it.appId == appId }
        emit(InstallProgress.Done(appId))
    }

    // ----------------------------------------------------------------------- //
    // 불변식 강제 헬퍼 (INV-1~3) — 통합 런타임이 따라야 할 레퍼런스 로직
    // ----------------------------------------------------------------------- //

    /**
     * `session` 을 RENDERING 으로 승격하기 전에, 현재 RENDERING 인 *다른* 세션을 모두
     * BACKGROUND 로 양도한다(INV-2). 임계구역 안에서 한 번에 처리해 RENDERING ≤ 1 보장.
     */
    private suspend fun promoteToForeground(session: FakeAppSession) {
        sessionLock.withLock {
            // 종료/크래시된 세션이면 승격하지 않음.
            if (session.state.value == SessionState.STOPPED ||
                session.state.value == SessionState.CRASHED ||
                session.state.value == SessionState.STOPPING
            ) {
                return
            }
            // 기존 RENDERING 세션 양도(INV-2: 새 세션 RENDERING 전에 *먼저*).
            _sessions.value
                .filter { it !== session && it.state.value == SessionState.RENDERING }
                .forEach { it.setState(SessionState.BACKGROUND) }
            // 이제 RENDERING 은 0 → 새 세션을 올림(INV-1: 결과적으로 정확히 1).
            session.setState(SessionState.RENDERING)
        }
    }

    /** AppSession.requestForeground() 진입점 — 같은 양도 규칙으로 포그라운드 획득. */
    internal fun onRequestForeground(session: FakeAppSession) {
        scope.launch { promoteToForeground(session) }
    }

    internal fun onRequestBackground(session: FakeAppSession) {
        scope.launch {
            sessionLock.withLock {
                if (session.state.value == SessionState.RENDERING) {
                    session.setState(SessionState.BACKGROUND)
                }
            }
        }
    }

    internal fun onStop(session: FakeAppSession, reason: StopReason) {
        scope.launch {
            sessionLock.withLock { session.setState(SessionState.STOPPING) }
            delay(stepMillis)
            sessionLock.withLock {
                session.setState(SessionState.STOPPED)
                removeSession(session)
            }
        }
    }

    // ----------------------------------------------------------------------- //
    // CrashSimulator — CRASHED 주입(데모/테스트). INV-1~3 유지.
    // ----------------------------------------------------------------------- //

    /**
     * appId 의 현재 세션을 CRASHED 로 전이. RENDERING 이던 세션이 크래시하면 RENDERING 은 0 이
     * 되어(여전히 ≤ 1, INV-1 유지) 포그라운드가 빈다 — 통합/런타임은 여기서 다음 후보를 올릴 수
     * 있으나 v1 mock 은 자동 승격하지 않는다(사용자가 최근앱에서 다시 띄움). CRASHED 세션은
     * *목록에 남겨* 런처/상세가 "앱 종료됨 · 재시작" 을 보이게 한다(STOPPED 처럼 제거하지 않음).
     */
    override fun simulateCrash(appId: String) {
        scope.launch {
            sessionLock.withLock {
                _sessions.value
                    .filter {
                        it.appId == appId &&
                            it.state.value != SessionState.STOPPED &&
                            it.state.value != SessionState.CRASHED
                    }
                    .forEach { it.setState(SessionState.CRASHED) }
            }
        }
    }

    /** CRASHED 세션 정리(사용자가 "앱 종료됨" 을 닫음) — 목록에서 제거해 훅을 끈다. */
    fun dismissCrashed(appId: String) {
        scope.launch {
            sessionLock.withLock {
                _sessions.value
                    .filter { it.appId == appId && it.state.value == SessionState.CRASHED }
                    .forEach { removeSession(it) }
            }
        }
    }

    private fun addSession(session: FakeAppSession) {
        _sessions.value = _sessions.value + session
        publishSessions()
    }

    private fun removeSession(session: FakeAppSession) {
        _sessions.value = _sessions.value.filterNot { it === session }
        publishSessions()
    }

    private fun publishSessions() {
        _sessionsPublic.value = _sessions.value.toList()
    }

    /** session.setState 가 상태 변경을 알릴 때 외부 리스트 재발행(StateFlow 트리거). */
    internal fun onSessionStateChanged() {
        publishSessions()
    }

    // ----------------------------------------------------------------------- //
    // 시드 데이터 (device 증명 앱)
    // ----------------------------------------------------------------------- //

    private fun seedInstalledApps(): List<InstalledApp> = listOf(
        InstalledApp(
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
        ),
        InstalledApp(
            appId = "org.foot.foot",
            name = "foot",
            summary = "가벼운 Wayland 터미널 에뮬레이터",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/foot"),
            category = AppCategory.TERMINAL,
            iconPath = null,
            requiredPermissions = emptyList(),
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
            iconPath = "/usr/share/icons/hicolor/64x64/apps/netsurf.png",
            requiredPermissions = listOf(AppPermission.NETWORK),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installedSizeBytes = 195L * 1024 * 1024,
            version = "3.11",
        ),
    )

    private fun seedCatalog(): List<CatalogApp> = listOf(
        // 이미 설치된 셋(카탈로그에도 보이며 [열기]로 표시) — device 증명.
        CatalogApp(
            appId = "org.gimp.GIMP",
            name = "GIMP",
            summary = "GNU 이미지 편집 프로그램",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gimp"),
            category = AppCategory.GRAPHICS,
            description = "레이어·필터·스크립트를 지원하는 풀기능 래스터 이미지 편집기. " +
                "터치로 캔버스 생성·브러시 그리기까지 device 검증됨(v111).",
            iconPath = "/usr/share/icons/hicolor/256x256/apps/gimp.png",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "gimp-stage.tar", 320L * 1024 * 1024)),
            requiredPermissions = listOf(AppPermission.STORAGE_READ, AppPermission.STORAGE_WRITE),
            display = DisplaySpec(DisplaySpec.DisplayMode.FULLSCREEN),
            minRuntime = "0.4.111",
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
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "foot-stage.tar", 6L * 1024 * 1024)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 6L * 1024 * 1024,
            source = AppSource.BUNDLED,
        ),
        CatalogApp(
            appId = "org.netsurf.netsurf",
            name = "NetSurf",
            summary = "경량 GTK 웹 브라우저",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/netsurf-gtk3"),
            category = AppCategory.INTERNET,
            description = "메모리 사용이 적은 GTK 기반 웹 브라우저.",
            iconPath = "/usr/share/icons/hicolor/64x64/apps/netsurf.png",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "netsurf-stage.tar", 195L * 1024 * 1024)),
            requiredPermissions = listOf(AppPermission.NETWORK),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 195L * 1024 * 1024,
            source = AppSource.BUNDLED,
        ),
        // 설치 가능(아직 미설치) — device 증명/검증 앱들.
        CatalogApp(
            appId = "org.gtk.gtk3-demo",
            name = "GTK3 Demo",
            summary = "GTK3 위젯 데모",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gtk3-demo"),
            category = AppCategory.DEVELOPMENT,
            description = "GTK3 위젯·렌더링을 시연하는 참조 앱. 툴킷 렌더 경로 device 검증용.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "gtk3-demo-stage.tar", 24L * 1024 * 1024)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 24L * 1024 * 1024,
            source = AppSource.BUNDLED,
        ),
        CatalogApp(
            appId = "org.libsdl.sdl2-demo",
            name = "SDL2 Demo",
            summary = "SDL2 렌더링 데모",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/sdl2-demo"),
            category = AppCategory.MULTIMEDIA,
            description = "SDL2 소프트웨어/가속 렌더 경로 검증 데모(wl_shm 표면).",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "sdl2-stage.tar", 31L * 1024 * 1024)),
            display = DisplaySpec(DisplaySpec.DisplayMode.FULLSCREEN),
            installSizeBytes = 31L * 1024 * 1024,
            source = AppSource.BUNDLED,
        ),
        // apt 인덱스 합성 출처(v1 = stage-tar 로 설치, 목록은 apt 메타).
        CatalogApp(
            appId = "org.debian.nano",
            name = "nano",
            summary = "작고 친절한 콘솔 텍스트 편집기",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/nano"),
            category = AppCategory.UTILITY,
            description = "터미널에서 도는 간단한 텍스트 편집기(foot 등에서 실행).",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "nano-stage.tar", 2L * 1024 * 1024)),
            installSizeBytes = 2L * 1024 * 1024,
            source = AppSource.APT,
        ),
        CatalogApp(
            appId = "org.debian.htop",
            name = "htop",
            summary = "인터랙티브 프로세스 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/htop"),
            category = AppCategory.SYSTEM,
            description = "컬러 ncurses UI 의 프로세스 모니터(터미널 앱).",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.STAGE_TAR, "htop-stage.tar", 3L * 1024 * 1024)),
            installSizeBytes = 3L * 1024 * 1024,
            source = AppSource.APT,
        ),
    )
}

/**
 * AppSession 의 mock — 상태를 StateFlow 로 들고, 명령을 FakeAlrRuntime 로 위임.
 * 실제 게스트/컴포지터는 없다(bindSurface 는 no-op 기록). 상태 전이는 런타임이
 * 불변식 임계구역 안에서만 호출(setState internal).
 */
class FakeAppSession internal constructor(
    override val appId: String,
    private val runtime: FakeAlrRuntime,
) : AppSession {

    private val _state = MutableStateFlow(SessionState.STARTING)
    override val state: StateFlow<SessionState> = _state.asStateFlow()

    /** mock: 바인드된 holder(검증/Preview 용 — present 없음). */
    @Volatile
    var boundHolder: SurfaceHolder? = null
        private set

    internal fun setState(next: SessionState) {
        if (_state.value != next) {
            _state.value = next
            runtime.onSessionStateChanged()
        }
    }

    override fun bindSurface(holder: SurfaceHolder) {
        boundHolder = holder
        // 실 런타임은 여기서 nativeWaylandCompositorStart + present. mock 은 기록만.
    }

    override fun unbindSurface() {
        boundHolder = null
    }

    override fun requestForeground() = runtime.onRequestForeground(this)
    override fun requestBackground() = runtime.onRequestBackground(this)
    override fun stop(reason: StopReason) = runtime.onStop(this, reason)
}

// --------------------------------------------------------------------------- //
// 변환 헬퍼
// --------------------------------------------------------------------------- //

/** 설치 완료 시 카탈로그 메타 → InstalledApp 합성(런처 그리드 등록). */
internal fun CatalogApp.toInstalledApp(): InstalledApp = InstalledApp(
    appId = appId,
    name = name,
    summary = summary,
    entry = entry,
    category = category,
    iconPath = iconPath,
    requiredPermissions = requiredPermissions,
    display = display,
    installedSizeBytes = totalInstallSizeBytes,
    version = "",
)
