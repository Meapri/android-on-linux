/*
 * NativeAlrRuntime — the REAL AlrRuntime (replaces FakeAlrRuntime in AlrRuntimeHolder).
 *
 * Phase 1 of the Linux-app-launcher product: DISCOVER installed Linux GUI apps from the
 * rootfs (.desktop scan) and RUN them in a window on tap. It GENERALIZES the proven
 * MainActivity.runChromiumStandalone() wiring — compositor start + native loader exec —
 * parameterized by LaunchRequest, going through the class-neutral AlrNative JNI facade
 * (runChromiumStandalone + the MainActivity JNI exports are untouched).
 *
 * Single-foreground invariants (ADR-004 §5-F INV-1..3): launch()/requestForeground()
 * move any existing RENDERING session to BACKGROUND BEFORE promoting a new one, so
 * RENDERING ≤ 1 holds. The transition logic mirrors FakeAlrRuntime (the reference impl).
 *
 * Lifecycle ↔ real wiring (per session):
 *   launch(req)            STARTING; session added to sessions. No native work yet.
 *   bindSurface(holder)    (RunningSurfaceActivity.surfaceCreated) — the keystone:
 *                            1. resolve rootfsDir (RootfsInstaller, same as MainActivity),
 *                            2. stage required overlays best-effort (if their tars exist),
 *                            3. AlrNative.nativeWaylandCompositorStart(surface, metrics),
 *                            4. → RENDERING (compositor up; first guest frame follows),
 *                            5. on a background thread: set base env + req.env, then
 *                               AlrNative.nativeAlrNativeLoaderProbe(... program ...) which
 *                               BLOCKS until the guest exits → STOPPED(exit 0)/CRASHED.
 *   surfaceChanged         compositorResize (rotation / multi-window).
 *   unbindSurface          present stops (session may stay BACKGROUND).
 *   requestForeground      promote (INV-2 hand-off) — compositor is process-global, so a
 *                          backgrounded session re-binds its surface on resume.
 *   stop(reason)           STOPPING → compositor stop; the guest is left to exit (Phase 1
 *                          has no guest-kill channel — see HONEST GAPS).
 *
 * Phase-1 scope: install()/uninstall()/catalog() are intentionally minimal (DISCOVERY +
 * RUN is the goal). Online apt install (exec-re-entry-gated) is a later phase.
 */
package dev.chanwoo.androlinux.runtime

import android.content.Context
import android.util.DisplayMetrics
import android.util.Log
import android.view.SurfaceHolder
import dev.chanwoo.androlinux.RootfsInstaller
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

private const val TAG = "alr_runtime"

class NativeAlrRuntime(private val appContext: Context) : AlrRuntime {

    // installedApps — backed by .desktop discovery; refreshed after rootfs prep + install.
    private val _installedApps = MutableStateFlow<List<InstalledApp>>(emptyList())
    override val installedApps: StateFlow<List<InstalledApp>> = _installedApps.asStateFlow()

    private val _sessions = MutableStateFlow<List<AppSession>>(emptyList())
    override val sessions: StateFlow<List<AppSession>> = _sessions.asStateFlow()

    /** Serializes all session-state transitions — the INV-2 critical section. */
    private val sessionLock = Any()

    /** Lazily-resolved, extracted rootfs dir. prepareBundledTinyRootfs() is the SAME call
     *  MainActivity uses; rootfsName comes from its manifest ("debian-arm64"), not hardcoded. */
    @Volatile private var rootfsDirCached: File? = null
    @Volatile private var rootfsNameCached: String? = null
    private val rootfsPrepStarted = AtomicBoolean(false)

    init {
        // Prepare the rootfs + scan .desktop entries off the main thread so the launcher
        // grid populates as soon as extraction finishes (first frame may show empty).
        ensureRootfsAsync()
    }

    // ----------------------------------------------------------------------- //
    // Discovery
    // ----------------------------------------------------------------------- //

    private fun ensureRootfsAsync() {
        if (!rootfsPrepStarted.compareAndSet(false, true)) return
        Thread({
            try {
                val status = RootfsInstaller(appContext).prepareBundledTinyRootfs()
                rootfsDirCached = status.rootfsDir
                rootfsNameCached = status.manifestName
                Log.i(TAG, "rootfs ready: ${status.manifestName} at ${status.rootfsDir} (extracted=${status.extracted})")
                refreshInstalledApps()
            } catch (e: Throwable) {
                Log.e(TAG, "rootfs prep failed: ${Log.getStackTraceString(e)}")
            }
        }, "alr-rootfs-prep").start()
    }

    /** Re-scan the rootfs `.desktop` dir and publish the result. Safe to call repeatedly. */
    fun refresh() = refreshInstalledApps()

    private fun refreshInstalledApps() {
        val dir = rootfsDirCached ?: return
        val apps = runCatching { DesktopEntryScanner.scan(dir) }
            .onFailure { Log.e(TAG, "desktop scan failed: ${Log.getStackTraceString(it)}") }
            .getOrDefault(emptyList())
        Log.i(TAG, "discovered ${apps.size} installed app(s): ${apps.joinToString { it.appId }}")
        _installedApps.value = apps
    }

    // ----------------------------------------------------------------------- //
    // launch — returns immediately; native work happens in bindSurface
    // ----------------------------------------------------------------------- //

    override fun launch(req: LaunchRequest): AppSession {
        val session = NativeAppSession(req, this)
        synchronized(sessionLock) {
            // INV-2: a fresh launch takes the foreground — demote any current RENDERING
            // session first so RENDERING never exceeds 1 (it goes RENDERING only once its
            // compositor is up, in bindSurface).
            demoteRenderingExcept(session)
            _sessions.value = _sessions.value + session
            session.setState(SessionState.STARTING)
            publish()
        }
        return session
    }

    // ----------------------------------------------------------------------- //
    // catalog / install / uninstall — ONLINE apt install wired (the in-app loop)
    // ----------------------------------------------------------------------- //

    /**
     * Catalog = the BUNDLED installable set (BundledCatalog — galculator/htop/… apt apps)
     * merged with whatever is already discovered installed (mapped to CatalogApp), so the
     * catalog screen shows BOTH "tap to install" apps and present "[open]" ones. Discovered
     * entries win on appId collision (they carry the resolved binary/icon). install(appId)
     * resolves the apt package from the bundled entry's RootfsDep(kind=APT).
     */
    override fun catalog(): Flow<List<CatalogApp>> = _installedApps.map { installed ->
        val byId = LinkedHashMap<String, CatalogApp>()
        for (c in BundledCatalog.apps) byId[c.appId] = c
        for (a in installed) byId[a.appId] = a.toCatalogApp()   // installed view wins
        byId.values.toList()
    }

    /**
     * ONLINE apt install of [appId]'s package: resolve appId → apt ref from the bundled
     * catalog, run the device-proven AptInstaller pipeline (apt-get update + install, with
     * the top-level dpkg -i completion), mapping apt's stdout phases to a monotonic percent.
     * On success rescan `.desktop` so the new app appears as a launcher tile WITHOUT a
     * restart; on failure emit Failed with the apt error. Idempotent if already installed.
     *
     * Demo-trust apt (the apt-mirror overlay ships `Trusted: yes`); authenticated gpgv is a
     * separate spawn-tasked gap — not blocked on here.
     */
    override fun install(appId: String): Flow<InstallProgress> = callbackFlow {
        // Already installed → Done idempotently (the SSOT is installedApps).
        if (_installedApps.value.any { it.appId == appId }) {
            trySend(InstallProgress.Done(appId)); close(); return@callbackFlow
        }
        val aptRef = BundledCatalog.aptRefFor(appId)
        if (aptRef == null) {
            trySend(InstallProgress.Failed(appId, "이 앱은 apt 설치 대상이 아닙니다: $appId")); close()
            return@callbackFlow
        }
        val worker = Thread({
            try {
                val rootfs = awaitRootfs()
                if (rootfs == null) {
                    trySend(InstallProgress.Failed(appId, "rootfs 준비 실패")); close(); return@Thread
                }
                val (rootfsDir, rootfsName) = rootfs
                trySend(InstallProgress.Running(appId, PCT_RESOLVING, InstallStage.RESOLVING))
                val result = AptInstaller.install(
                    host = aptHost(),
                    rootfsDir = rootfsDir,
                    rootfsName = rootfsName,
                    pkg = aptRef,
                ) { phase ->
                    // apt stdout phase → monotonic percent + UI stage label.
                    val (pct, stage) = when (phase) {
                        AptInstaller.Phase.RESOLVING -> PCT_RESOLVING to InstallStage.RESOLVING
                        AptInstaller.Phase.DOWNLOADING -> PCT_DOWNLOADING to InstallStage.DOWNLOADING
                        AptInstaller.Phase.UNPACKING -> PCT_UNPACKING to InstallStage.UNPACKING
                        AptInstaller.Phase.CONFIGURING -> PCT_CONFIGURING to InstallStage.CONFIGURING
                        AptInstaller.Phase.REGISTERING -> PCT_REGISTERING to InstallStage.REGISTERING
                    }
                    trySend(InstallProgress.Running(appId, pct, stage))
                }
                if (result.installed) {
                    // The keystone: re-scan .desktop so the newly-installed app becomes a tile
                    // in installedApps NOW (no app restart), then report Done.
                    refreshInstalledApps()
                    Log.i(TAG, "install($appId): installed=true; rescanned → ${_installedApps.value.size} app(s)")
                    trySend(InstallProgress.Done(appId))
                } else {
                    Log.w(TAG, "install($appId): failed: ${result.error}")
                    trySend(InstallProgress.Failed(appId, result.error ?: "설치 실패"))
                }
            } catch (e: Throwable) {
                Log.e(TAG, "install($appId) EXC: ${Log.getStackTraceString(e)}")
                trySend(InstallProgress.Failed(appId, e.message ?: "설치 중 오류"))
            } finally {
                close()
            }
        }, "alr-install-$appId")
        worker.start()
        awaitClose { /* the worker runs to completion; nothing to cancel mid-apt safely */ }
    }

    /**
     * Minimal `apt-get remove -y <pkg>` + rescan. Resolves the apt ref (from the bundled
     * catalog, else the appId itself as a fallback package name), removes, then rescans so
     * the tile disappears. Emits REMOVING progress then Done/Failed.
     */
    override fun uninstall(appId: String): Flow<InstallProgress> = callbackFlow {
        val aptRef = BundledCatalog.aptRefFor(appId) ?: appId
        val worker = Thread({
            try {
                val rootfs = awaitRootfs()
                if (rootfs == null) {
                    trySend(InstallProgress.Failed(appId, "rootfs 준비 실패")); close(); return@Thread
                }
                val (rootfsDir, rootfsName) = rootfs
                trySend(InstallProgress.Running(appId, 50, InstallStage.REMOVING))
                val removed = AptInstaller.remove(aptHost(), rootfsDir, rootfsName, aptRef)
                refreshInstalledApps()
                if (removed) trySend(InstallProgress.Done(appId))
                else trySend(InstallProgress.Failed(appId, "제거를 완료하지 못했습니다"))
            } catch (e: Throwable) {
                Log.e(TAG, "uninstall($appId) EXC: ${Log.getStackTraceString(e)}")
                trySend(InstallProgress.Failed(appId, e.message ?: "제거 중 오류"))
            } finally {
                close()
            }
        }, "alr-uninstall-$appId")
        worker.start()
        awaitClose { }
    }

    /** AptInstaller.Host backed by the runtime's AlrNative JNI facade + app dirs. */
    private fun aptHost() = object : AptInstaller.Host {
        override val packageName: String get() = this@NativeAlrRuntime.packageName
        override val nativeLibraryDir: String get() = this@NativeAlrRuntime.nativeLibraryDir
        override val filesDir: String get() = this@NativeAlrRuntime.filesDirPath
        override val cacheDir: String get() = this@NativeAlrRuntime.cacheDirPath
        override fun loaderProbe(rootfsName: String, program: String): String =
            AlrNative.nativeAlrNativeLoaderProbe(
                packageName, nativeLibraryDir, filesDir, cacheDir, rootfsName, program,
            )
        override fun extractOverlay(tar: File, rootfsDir: File): Pair<Int, Int> {
            val ovr = this@NativeAlrRuntime.extractOverlay(tar, rootfsDir)
            return ovr.extracted to ovr.skipped.size
        }
    }

    // ----------------------------------------------------------------------- //
    // INV-1..3 enforcement helpers (mirror FakeAlrRuntime)
    // ----------------------------------------------------------------------- //

    /** Demote every RENDERING session that is NOT [keep] to BACKGROUND. Caller holds lock. */
    private fun demoteRenderingExcept(keep: NativeAppSession?) {
        _sessions.value
            .filterIsInstance<NativeAppSession>()
            .filter { it !== keep && it.state.value == SessionState.RENDERING }
            .forEach { it.setState(SessionState.BACKGROUND) }
    }

    /**
     * Hand the foreground to [session]: demote any OTHER RENDERING session first (INV-2),
     * then promote this one to RENDERING — but only if its compositor is actually up
     * (its first frame can present). When called from requestForeground() before the
     * surface exists (onResume fires before surfaceCreated on first launch), the session
     * is still STARTING with no compositor; it stays STARTING and bindSurface promotes it
     * once the compositor starts. RENDERING therefore never precedes a live compositor,
     * and never exceeds 1 (INV-1).
     */
    internal fun promoteToForeground(session: NativeAppSession) {
        synchronized(sessionLock) {
            val s = session.state.value
            if (s == SessionState.STOPPED || s == SessionState.CRASHED || s == SessionState.STOPPING) return
            demoteRenderingExcept(session)
            if (session.isCompositorUp()) {
                session.setState(SessionState.RENDERING)
            }
            publish()
        }
    }

    internal fun onRequestBackground(session: NativeAppSession) {
        synchronized(sessionLock) {
            if (session.state.value == SessionState.RENDERING) {
                session.setState(SessionState.BACKGROUND)
                publish()
            }
        }
    }

    /** A session's loader call returned — settle to STOPPED(exit 0) or CRASHED. */
    internal fun onGuestExited(session: NativeAppSession, exitOk: Boolean) {
        synchronized(sessionLock) {
            session.setState(if (exitOk) SessionState.STOPPED else SessionState.CRASHED)
            if (exitOk) {
                // STOPPED sessions leave the list (launcher returns); CRASHED stay so the
                // host can show "app stopped / restart".
                _sessions.value = _sessions.value.filterNot { it === session }
            }
            publish()
        }
    }

    internal fun onStop(session: NativeAppSession) {
        synchronized(sessionLock) {
            val s = session.state.value
            if (s == SessionState.STOPPED || s == SessionState.CRASHED) return
            session.setState(SessionState.STOPPING)
            publish()
        }
        // Tear the compositor down (process-global). The guest is left to exit on its own
        // (no guest-kill channel in Phase 1); its loader thread will report STOPPED/CRASHED.
        session.teardownCompositor()
    }

    internal fun notifyStateChanged() = synchronized(sessionLock) { publish() }

    private fun publish() { _sessions.value = _sessions.value.toList() }

    // ----------------------------------------------------------------------- //
    // Shared run context for sessions
    // ----------------------------------------------------------------------- //

    /** Blocks the caller until the rootfs is extracted (used by bindSurface's worker). */
    internal fun awaitRootfs(): Pair<File, String>? {
        // Kick prep if it somehow hasn't started, then poll briefly for completion.
        ensureRootfsAsync()
        var waited = 0
        while (waited < ROOTFS_WAIT_MS) {
            val dir = rootfsDirCached
            val name = rootfsNameCached
            if (dir != null && name != null) return dir to name
            try { Thread.sleep(200) } catch (_: InterruptedException) { return null }
            waited += 200
        }
        return null
    }

    internal val packageName: String get() = appContext.packageName
    internal val nativeLibraryDir: String get() = appContext.applicationInfo.nativeLibraryDir
    internal val filesDirPath: String get() = appContext.filesDir.absolutePath
    internal val cacheDirPath: String get() = appContext.cacheDir.absolutePath

    /** Apply an overlay stage-tar with the lib-downgrade guard (same path as MainActivity). */
    internal fun extractOverlay(tar: File, rootfsDir: File) =
        RootfsInstaller(appContext).extractOverlayTar(tar, rootfsDir)

    /** Display metrics for the compositor output (same fields runChromiumStandalone reads). */
    internal fun displayMetrics(): DisplayMetrics = appContext.resources.displayMetrics

    companion object {
        /** Max wait for rootfs extraction before a launch gives up (extraction is one-time). */
        private const val ROOTFS_WAIT_MS = 120_000

        // Monotonic percents for the apt install phases (UI shows a rising bar).
        private const val PCT_RESOLVING = 10
        private const val PCT_DOWNLOADING = 35
        private const val PCT_UNPACKING = 65
        private const val PCT_CONFIGURING = 85
        private const val PCT_REGISTERING = 97
    }
}

// --------------------------------------------------------------------------- //
// Mapping helpers
// --------------------------------------------------------------------------- //

/** Discovered InstalledApp → CatalogApp (Phase-1 catalog = what is installed). */
internal fun InstalledApp.toCatalogApp(): CatalogApp = CatalogApp(
    appId = appId,
    name = name,
    summary = summary,
    entry = entry,
    category = category,
    iconPath = iconPath,
    requiredPermissions = requiredPermissions,
    display = display,
    installSizeBytes = installedSizeBytes,
    source = AppSource.BUNDLED,
)

// --------------------------------------------------------------------------- //
// BundledCatalog — the installable apt apps the launcher offers (appId → apt ref)
// --------------------------------------------------------------------------- //

/**
 * The set of apps the in-app catalog can INSTALL via `apt-get install` (online, from the
 * pinned ports.ubuntu.com mirror). appId is the stable launch key = the `.desktop` basename
 * the package ships, so after a successful install the SAME appId surfaces as an InstalledApp
 * tile (DesktopEntryScanner reads the now-present `.desktop`). The RootfsDep(kind=APT) `ref`
 * is the exact apt package name install() hands to AptInstaller.
 *
 * SSOT for the entrypoint/.desktop/apt-name is tools/build_app_stage.py APPS (galculator:
 * apt=galculator, /usr/bin/galculator, /usr/share/applications/galculator.desktop). The
 * heavier GUI stack (GTK3) is already in the base rootfs (it powers GIMP), so galculator's
 * apt closure is essentially just its own leaf — a clean one-package install + launch, the
 * in-app loop's proving app (dpkg configured=true device-proven per project memory).
 *
 * CURATION BAR (post Wave-2): every entry must `dpkg --configure` exit-0 on a non-root device
 * with NO systemd — i.e. its real noble closure must be base-GTK3-provided with no daemon /
 * dbus-activation / appstream / policykit / accountsservice / libpam-with-postinst /
 * dconf-service leaf (those postinsts need a running init → the `dpkg --configure -a` exit-73
 * cascade). mousepad was REMOVED here: its real closure drags perl/libpam/dbus/systemd/dconf
 * and was device-proven to fail that cascade (installed=false). Proof status per entry:
 *   DEVICE-PROVEN (installs+configures exit-0 on device): galculator, l3afpad.
 *   AUDITED (noble closure verified base-GTK3-only, no exit-73 leaf; device-test pending):
 *     gpicview, xarchiver, sakura.  (htop = proven-class ncurses leaf.)
 */
object BundledCatalog {

    val apps: List<CatalogApp> = listOf(
        // galculator — the in-app loop demo: a light GTK3 calculator. GTK3 is base-provided,
        // so `apt-get install galculator` is a single-leaf install, then it runs windowed on
        // the ALR Wayland compositor (GDK Wayland backend).
        CatalogApp(
            appId = "galculator",
            name = "Galculator",
            summary = "가벼운 GTK 계산기",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/galculator"),
            category = AppCategory.UTILITY,
            description = "GTK3 기반의 가벼운 데스크톱 계산기. apt 로 설치되어 ALR Wayland " +
                "컴포지터 위 창으로 실행됩니다 — 인앱 설치→런처 등장→실행 루프의 데모 앱.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "galculator", 1_200_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 1_200_000L,
            source = AppSource.APT,
        ),
        // htop — ncurses process viewer; ships a .desktop. Lightweight apt install proof.
        CatalogApp(
            appId = "htop",
            name = "htop",
            summary = "인터랙티브 프로세스 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/htop"),
            category = AppCategory.SYSTEM,
            description = "터미널에서 도는 인터랙티브 프로세스 모니터(ncurses). apt 로 설치.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "htop", 1_100_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 1_100_000L,
            source = AppSource.APT,
        ),
        // sakura — VTE 기반 초경량 GTK3 터미널 에뮬레이터(독립형, 데스크톱 환경 비종속).
        // noble depends 닫힘: libc6, libglib2.0-0t64, libgtk-3-0t64, libpango-1.0-0,
        // libvte-2.91-0 — GTK3 코어는 base rootfs(GIMP) 제공이라 새로 까는 건 사실상
        // libvte-2.91-0(+libvte-2.91-common) leaf 뿐. libvte 가 끄는 유일한 "systemd
        // 계열" 토큰은 libsystemd0 인데, 이는 journald *클라이언트 라이브러리*(.so)일
        // 뿐 systemd PID-1/데몬이 아니다 — postinst 는 ldconfig 뿐, 데몬·dbus-activation·
        // PAM 유지보수 스크립트가 없어 mousepad 가 터졌던 `dpkg --configure -a` exit-73
        // 캐스케이드(systemd/dbus/libpam postinst 가 동작 중 init 요구)와 무관하고,
        // libsystemd0 자체는 base 에 이미 깔려 있다. 따라서 galculator/l3afpad 처럼
        // configure 가 끝까지 통과한다(AUDITED — 아직 device 미검증). appId 는 .desktop
        // basename(sakura.desktop) 과 일치 → 설치 후 DesktopEntryScanner 가 같은 타일로
        // 재조정. Exec=`sakura`, 바이너리 /usr/bin/sakura. htop 이 "터미널 안의 ncurses"
        // 라면 sakura 는 터미널 그 자체 — 실사용 가치 높은 추가.
        CatalogApp(
            appId = "sakura",
            name = "Sakura",
            summary = "초경량 VTE 터미널",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/sakura"),
            category = AppCategory.TERMINAL,
            description = "VTE 기반의 가벼운 GTK3 터미널 에뮬레이터(독립형, 데스크톱 환경 " +
                "비종속). apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다(GDK " +
                "Wayland 백엔드). depends 가 GTK3/pango/libvte 뿐 — libsystemd0 은 " +
                "journald 클라이언트 .so 일 뿐 systemd 데몬이 아니라, systemd/dbus/" +
                "appstream 유지보수 스크립트가 없어 galculator 처럼 dpkg configure 가 " +
                "끝까지 통과한다. noble 패키지 sakura → /usr/share/applications/sakura.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "sakura", 300_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 300_000L,
            source = AppSource.APT,
        ),
        // l3afpad — leafpad 의 GTK3 포크. 카탈로그에서 가장 작은 GUI 닫힘:
        // libc6, libcairo2, libglib2.0-0t64, libgtk-3-0t64, libpango-1.0-0,
        // libpangocairo-1.0-0 만 — systemd/dbus/appstream/policykit 전무. 전부 base
        // rootfs(GTK3 스택)에 이미 있어 사실상 단일-leaf 설치 → galculator 동급으로
        // configure 통과. appId 는 .desktop basename(l3afpad.desktop) 과 일치.
        // Exec=`l3afpad %f`(%f strip), 바이너리 /usr/bin/l3afpad.
        CatalogApp(
            appId = "l3afpad",
            name = "L3afpad",
            summary = "초경량 GTK 텍스트 편집기",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/l3afpad"),
            category = AppCategory.UTILITY,
            description = "leafpad 의 GTK3 포크 — 카탈로그에서 가장 작은 닫힘(cairo/pango/" +
                "glib/gtk3 6개 라이브러리뿐). apt 로 설치되어 ALR Wayland 컴포지터 위 " +
                "창으로 실행됩니다. systemd/dbus/appstream 유지보수 스크립트가 없어 " +
                "galculator 처럼 dpkg configure 가 끝까지 통과한다. noble 패키지 l3afpad " +
                "→ /usr/share/applications/l3afpad.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "l3afpad", 600_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 600_000L,
            source = AppSource.APT,
        ),
        // gpicview — LXDE 의 가벼운 GTK3 이미지 뷰어. eog 가 끌던 GNOME-desktop 닫힘
        // (libgnome-desktop-3, gsettings-desktop-schemas, shared-mime-info, librsvg2-
        // common, webp-pixbuf-loader, peas/gir introspection) 대신, gpicview 닫힘은
        // libcairo2, libgdk-pixbuf-2.0-0, libglib2.0-0t64, libgtk-3-0t64, libjpeg8,
        // libx11-6 만(libjpeg8 은 libjpeg-turbo8 .so 로 가는 얇은 shim — postinst·데몬
        // 없음). systemd/dbus/appstream/policykit/accountsservice/dconf-service 전무,
        // GNOME 플랫폼 비의존 — GTK3 코어는 base(GIMP) 제공이라 새 패키지는 leaf 라이브러리
        // 뿐이라 galculator 처럼 configure 통과(AUDITED — 닫힘 검증 완료, 아직 device
        // 미검증). appId 는 .desktop basename(gpicview.desktop) 과 일치. Exec=`gpicview
        // %U`(strip), 바이너리 /usr/bin/gpicview — 푸시한 test.png 를 인자로 연다.
        CatalogApp(
            appId = "gpicview",
            name = "GPicView",
            summary = "가벼운 LXDE 이미지 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gpicview"),
            category = AppCategory.GRAPHICS,
            description = "LXDE 의 가벼운 GTK3 이미지 뷰어(독립형, GNOME 플랫폼 비의존). " +
                "apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다(GDK Wayland " +
                "백엔드, 소프트웨어 렌더). eog 와 달리 systemd/dbus/appstream/gnome-" +
                "desktop 닫힘이 없어 galculator 처럼 dpkg configure 가 끝까지 통과한다. " +
                "noble 패키지 gpicview → /usr/share/applications/gpicview.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "gpicview", 700_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 700_000L,
            source = AppSource.APT,
        ),
        // xarchiver — GTK3 아카이브 관리자(독립형, 데스크톱 비종속). noble depends 닫힘:
        // libc6, libgdk-pixbuf-2.0-0, libglib2.0-0t64, libgtk-3-0t64 만 — 비-GTK leaf 가
        // 0(닫힘 전체가 base GTK3 스택). systemd/dbus/appstream/policykit/accountsservice/
        // dconf-service 전무. 새로 까는 패키지가 사실상 xarchiver 자기 leaf 뿐 →
        // galculator 동급 configure 통과(AUDITED — 닫힘 검증 완료, 아직 device 미검증).
        // 압축 CLI(tar/zip/unzip)는 Depends 가 아닌 Recommends 라 install 을 막지 않음
        // (런타임에 필요). appId 는 .desktop basename(xarchiver.desktop) 과 일치.
        // Exec=`xarchiver %F`(%F strip), 바이너리 /usr/bin/xarchiver.
        CatalogApp(
            appId = "xarchiver",
            name = "Xarchiver",
            summary = "가벼운 GTK 압축 관리자",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/xarchiver"),
            category = AppCategory.UTILITY,
            description = "GTK3 아카이브(zip/tar/…) 관리자 — 독립형, 데스크톱 환경 비종속. " +
                "apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다. depends 가 " +
                "gtk3/glib/gdk-pixbuf 뿐이라 systemd/dbus/appstream 유지보수 스크립트가 " +
                "없어 galculator 처럼 dpkg configure 가 끝까지 통과한다. noble 패키지 " +
                "xarchiver → /usr/share/applications/xarchiver.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "xarchiver", 1_300_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 1_300_000L,
            source = AppSource.APT,
        ),
    )

    private val aptRefByAppId: Map<String, String> =
        apps.mapNotNull { c -> c.rootfsDeps.firstOrNull { it.kind == RootfsDepKind.APT }?.ref?.let { c.appId to it } }
            .toMap()

    /** appId → apt package name, or null if [appId] is not a bundled apt-installable app. */
    fun aptRefFor(appId: String): String? = aptRefByAppId[appId]
}
