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
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.flow
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
    // catalog / install / uninstall — Phase-1 minimal (DISCOVERY + RUN is the goal)
    // ----------------------------------------------------------------------- //

    /**
     * Phase 1: expose the discovered installed apps as a catalog (mapped to CatalogApp),
     * so the catalog screen shows what is present and offers [open]. A real apt/bundle
     * index is a later phase. TODO(phase-2): merge a bundled + apt-synthesized catalog.
     */
    override fun catalog(): Flow<List<CatalogApp>> = flow {
        emit(_installedApps.value.map { it.toCatalogApp() })
    }

    /**
     * Phase 1: online/overlay install is NOT wired here (it is gated on exec-re-entry +
     * the in-guest apt path, a later phase). Emit a clear terminal "not yet" so the UI
     * surfaces it instead of hanging. If the app is already present (discovered), report
     * Done idempotently. TODO(phase-2): delegate to the stage-tar / in-guest dpkg path.
     */
    override fun install(appId: String): Flow<InstallProgress> = flow {
        if (_installedApps.value.any { it.appId == appId }) {
            emit(InstallProgress.Done(appId))
        } else {
            emit(InstallProgress.Failed(appId, "설치는 다음 단계에서 지원됩니다 (Phase 1: 탐색·실행)"))
        }
    }

    /** Phase 1: uninstall (overlay/marker removal) is not wired. TODO(phase-2). */
    override fun uninstall(appId: String): Flow<InstallProgress> = flow {
        emit(InstallProgress.Failed(appId, "제거는 다음 단계에서 지원됩니다 (Phase 1: 탐색·실행)"))
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
