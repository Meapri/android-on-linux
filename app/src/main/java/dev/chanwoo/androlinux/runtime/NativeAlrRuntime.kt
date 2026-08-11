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
import kotlinx.coroutines.channels.ProducerScope
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
                // The bundled tree is still extracted -- it carries the staged
                // interposer and the GTK closure the bundled demos use -- but it
                // is not automatically the guest. A provisioned Ubuntu tree wins
                // when there is one: the launcher was running the PoC rootfs
                // while the probe harness measured Ubuntu, so "it works in the
                // app" and "it works in the report" were claims about two
                // different filesystems.
                val picked = dev.chanwoo.androlinux.GuestRootfs.pick(appContext.filesDir)
                rootfsDirCached = picked?.first ?: status.rootfsDir
                rootfsNameCached = picked?.second ?: status.manifestName
                Log.i(
                    TAG,
                    "rootfs ready: ${rootfsNameCached} at ${rootfsDirCached} " +
                        "(bundled ${status.manifestName} extracted=${status.extracted}" +
                        "${if (picked == null) "; no provisioned Ubuntu tree" else ""})",
                )
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
        val scanned = runCatching { DesktopEntryScanner.scan(dir) }
            .onFailure { Log.e(TAG, "desktop scan failed: ${Log.getStackTraceString(it)}") }
            .getOrDefault(emptyList())
        // OVERLAY-provisioned apps (chromium) ship NO .desktop the scanner reads (the browser
        // lives under /usr/lib/chromium, not /usr/share/applications). So if such an app's
        // overlay binary is present in the rootfs (= overlay extracted = "installed"), synthesize
        // its InstalledApp here so the catalog tile reads installed AND the launcher grid can tap
        // it. The scanned set wins on appId collision (it carries the resolved icon/binary).
        val scannedIds = scanned.map { it.appId }.toSet()
        val overlayInstalled = BundledCatalog.overlayApps.values.mapNotNull { c ->
            if (c.appId in scannedIds) null
            else if (overlayBinaryPresent(dir, c)) c.toInstalledOverlayApp() else null
        }
        val apps = scanned + overlayInstalled
        Log.i(TAG, "discovered ${apps.size} installed app(s): ${apps.joinToString { it.appId }}" +
            if (overlayInstalled.isNotEmpty()) " (overlay: ${overlayInstalled.joinToString { it.appId }})" else "")
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
        for (a in installed) {
            // Installed view wins on appId collision (it carries the resolved binary/icon) —
            // EXCEPT OVERLAY apps (chromium), whose richer BundledCatalog entry (provision/
            // launchActivity/overlayBinaryPath) must survive; its installed-ness is already in
            // `installed` (so the tile reads installed), so we keep the catalog entry as-is.
            if (BundledCatalog.isOverlayProvisioned(a.appId)) continue
            byId[a.appId] = a.toCatalogApp()
        }
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
        // OVERLAY-provisioned apps (chromium) do NOT go through AptInstaller: their "install" is
        // extracting a pre-built overlay stage-tar, and "installed" = the binary is present. The
        // worker closes the channel on completion; awaitClose keeps the callbackFlow open until
        // then (the worker runs to completion — nothing to cancel mid-extract safely).
        val overlayApp = BundledCatalog.overlayAppFor(appId)
        if (overlayApp != null) {
            installOverlayApp(appId, overlayApp, this)
            awaitClose { }
            return@callbackFlow
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
                // apt/dpkg stdout phase → monotonic percent + UI stage label (shared by the
                // online and the offline/staged attempts so progress is identical either way).
                val onPhase: (AptInstaller.Phase) -> Unit = { phase ->
                    val (pct, stage) = when (phase) {
                        AptInstaller.Phase.RESOLVING -> PCT_RESOLVING to InstallStage.RESOLVING
                        AptInstaller.Phase.DOWNLOADING -> PCT_DOWNLOADING to InstallStage.DOWNLOADING
                        AptInstaller.Phase.UNPACKING -> PCT_UNPACKING to InstallStage.UNPACKING
                        AptInstaller.Phase.CONFIGURING -> PCT_CONFIGURING to InstallStage.CONFIGURING
                        AptInstaller.Phase.REGISTERING -> PCT_REGISTERING to InstallStage.REGISTERING
                    }
                    trySend(InstallProgress.Running(appId, pct, stage))
                }
                // ONLINE first (the product path: download the closure from the mirror). If it
                // fails (e.g. the mirror/apt-key path is down) AND a pre-staged <pkg>-stage.tar
                // is on the device, fall back to the OFFLINE/staged install — the bundled/offline
                // capability — which needs no fetch. The staged method judges success by
                // `dpkg --status`, so it is robust where the old MainActivity drain was not.
                var result = AptInstaller.install(
                    host = aptHost(),
                    rootfsDir = rootfsDir,
                    rootfsName = rootfsName,
                    pkg = aptRef,
                    onProgress = onPhase,
                )
                if (!result.installed && stagedTarPresent(aptRef)) {
                    Log.w(TAG, "install($appId): online failed (${result.error}); trying OFFLINE staged $aptRef-stage.tar")
                    result = AptInstaller.installStaged(
                        host = aptHost(),
                        rootfsDir = rootfsDir,
                        rootfsName = rootfsName,
                        pkg = aptRef,
                        onProgress = onPhase,
                    )
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

    // ----------------------------------------------------------------------- //
    // OVERLAY-provisioned install (chromium) — extractOverlay, NOT AptInstaller
    // ----------------------------------------------------------------------- //

    /**
     * Install an OVERLAY-provisioned app (chromium): NOT an apt download. Order:
     *   1. binary already present in the rootfs → already provisioned, Done (rescan to surface
     *      the tile);
     *   2. else a matching overlay stage-tar is on the device (/data/local/tmp/<...>-stage.tar) →
     *      extractOverlay() it (same guarded path MainActivity uses), rescan; Done iff the binary
     *      is now present;
     *   3. else NO tar present → report HONESTLY that the (large, pre-built) component must be
     *      provided. We do NOT claim a fake apt install and do NOT touch AptInstaller.
     * Runs on a worker thread like the apt path; emits RESOLVING → UNPACKING progress.
     */
    private fun installOverlayApp(
        appId: String,
        app: CatalogApp,
        scope: ProducerScope<InstallProgress>,
    ) {
        val worker = Thread({
            try {
                val rootfs = awaitRootfs()
                if (rootfs == null) {
                    scope.trySend(InstallProgress.Failed(appId, "rootfs 준비 실패")); scope.close(); return@Thread
                }
                val (rootfsDir, _) = rootfs
                scope.trySend(InstallProgress.Running(appId, PCT_RESOLVING, InstallStage.RESOLVING))
                // (1) already extracted?
                if (overlayBinaryPresent(rootfsDir, app)) {
                    refreshInstalledApps()
                    Log.i(TAG, "install($appId): overlay already present; Done")
                    scope.trySend(InstallProgress.Done(appId)); return@Thread
                }
                // (2) a staged overlay tar present → extract it.
                val tar = overlayStageTarFor(app)
                if (tar == null) {
                    // (3) honest: the component is a large pre-built browser, not an apt download.
                    Log.w(TAG, "install($appId): no overlay tar staged; component must be provided")
                    scope.trySend(
                        InstallProgress.Failed(
                            appId,
                            "Chromium 구성요소(미리 빌드된 브라우저 오버레이)가 기기에 없습니다. " +
                                "apt 로 받는 앱이 아니며, chromium-gui-stage.tar 를 " +
                                "/data/local/tmp 에 제공해야 설치됩니다.",
                        ),
                    )
                    return@Thread
                }
                scope.trySend(InstallProgress.Running(appId, PCT_UNPACKING, InstallStage.UNPACKING))
                Log.i(TAG, "install($appId): extracting overlay ${tar.name} (${tar.length()} bytes)")
                val ovr = extractOverlay(tar, rootfsDir)
                Log.i(TAG, "install($appId): overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                refreshInstalledApps()
                if (overlayBinaryPresent(rootfsDir, app)) {
                    scope.trySend(InstallProgress.Done(appId))
                } else {
                    scope.trySend(
                        InstallProgress.Failed(appId, "오버레이 추출 후에도 chromium 바이너리를 찾지 못했습니다"),
                    )
                }
            } catch (e: Throwable) {
                Log.e(TAG, "install($appId) overlay EXC: ${Log.getStackTraceString(e)}")
                scope.trySend(InstallProgress.Failed(appId, e.message ?: "설치 중 오류"))
            } finally {
                scope.close()
            }
        }, "alr-install-overlay-$appId")
        worker.start()
    }

    /**
     * True iff [app]'s overlay binary is present in the rootfs (= overlay extracted = installed).
     * Accepts the declared overlayBinaryPath AND, for chromium, the alternate binaries either
     * overlay tar can ship (the full browser `chromium` from chromium-gui-stage.tar, or the older
     * `chromium-headless-shell` from chromium-stage.tar) — a device that staged either reads as
     * installed. The launch path (runChromiumStandalone) gates on the full browser, but the
     * catalog's *installed* signal is honest for either staged component.
     */
    internal fun overlayBinaryPresent(rootfsDir: File, app: CatalogApp): Boolean {
        val candidates = buildList {
            app.overlayBinaryPath?.let { add(it) }
            // chromium-specific alternates (full browser vs headless shell).
            add("usr/lib/chromium/chromium")
            add("usr/lib/chromium/chromium-headless-shell")
        }.distinct()
        return candidates.any { File(rootfsDir, it).isFile }
    }

    /**
     * Locate a pre-built overlay stage-tar on the device for [app]. For chromium we prefer the
     * FULL-browser overlay (chromium-gui-stage.tar — what runChromiumStandalone launches), then
     * fall back to the older headless chromium-stage.tar. Returns the first present tar, or null
     * if none is staged (→ the honest "component must be provided" path).
     */
    internal fun overlayStageTarFor(app: CatalogApp): File? {
        val names = if (app.appId == "org.chromium.Chromium") {
            listOf("chromium-gui-stage.tar", "chromium-stage.tar")
        } else {
            // generic fallback: <last entry-binary segment>-stage.tar
            listOf("${app.appId}-stage.tar")
        }
        return names.map { File("/data/local/tmp/$it") }.firstOrNull { it.isFile }
    }

    /** AptInstaller.Host backed by the runtime's AlrNative JNI facade + app dirs. */
    private fun aptHost() = object : AptInstaller.Host {
        override val packageName: String get() = this@NativeAlrRuntime.packageName
        override val nativeLibraryDir: String get() = this@NativeAlrRuntime.nativeLibraryDir
        override val filesDir: String get() = this@NativeAlrRuntime.filesDirPath
        override val cacheDir: String get() = this@NativeAlrRuntime.cacheDirPath
        // apt runs through alr, not the legacy native loader.
        //
        // alr already is what the four staged overlays were emulating:
        // ALR_FAKEROOT=1 is the uid-0 shim dpkg needs to chown its unpacked
        // files, its interposer is the path layer, and its resolver bridge is
        // what lets apt resolve the mirror without raw UDP/53. Routing here
        // means a clean device can install; the overlay path required stage
        // tars that only existed in /data/local/tmp on a dev machine.
        override val selfSufficient: Boolean get() = true

        override fun loaderProbe(rootfsName: String, program: String): String {
            val argv = program.split("\n").filter { it.isNotEmpty() }
            if (argv.isEmpty()) return ""
            val alr = dev.chanwoo.androlinux.AlrRuntime(
                java.io.File(nativeLibraryDir), java.io.File(filesDir),
            )
            val r = alr.run(
                distro = rootfsName,
                program = argv.first(),
                arguments = argv.drop(1),
                fakeroot = true,
                // apt/dpkg on a cold index fetches tens of MB and runs every
                // maintainer script in the closure; the 60s default expired
                // mid-`Setting up` and reported a timeout as an install failure.
                timeoutSeconds = 1800,
                guestEnv = mapOf(
                    "DEBIAN_FRONTEND" to "noninteractive",
                    "DEBCONF_NONINTERACTIVE_SEEN" to "true",
                ),
            )
            // The caller greps stdout for apt's own phrases ("Get:",
            // "Setting up <pkg>"), and dpkg writes some of them to stderr.
            return r.stdout + "\n" + r.stderr
        }
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

    /**
     * True iff a pre-staged `<pkg>-stage.tar` (built by tools/build_install_stage.py, shipping
     * the leaf .deb + closure) has been pushed to /data/local/tmp — the gate for the OFFLINE
     * staged-install fallback. We only attempt the offline path when its payload is actually
     * present, so a normal device with no staged tar just sees the online failure as before.
     */
    internal fun stagedTarPresent(pkg: String): Boolean =
        File("/data/local/tmp/$pkg-stage.tar").isFile

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

/**
 * OVERLAY-provisioned CatalogApp (chromium) → synthesized InstalledApp, used when the overlay
 * binary is present in the rootfs but no `.desktop` exists for the scanner to find. Carries the
 * catalog entry's launch entry/category/display so the launcher tile + AppDetail render and tap
 * works; the launchActivity routing is keyed by appId in LauncherActivity (not on InstalledApp).
 */
internal fun CatalogApp.toInstalledOverlayApp(): InstalledApp = InstalledApp(
    appId = appId,
    name = name,
    summary = summary,
    entry = entry,
    category = category,
    iconPath = iconPath,
    requiredPermissions = requiredPermissions,
    display = display,
    installedSizeBytes = installSizeBytes,
)

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
 * CURATION BAR (corrected post host-audit, tools/app_closure_audit.py): every entry must
 * `dpkg --configure` exit-0 on a non-root device with NO systemd. The discriminator is NOT
 * "does the closure mention systemd/dbus" — it does for EVERY GTK3 app: galculator's own
 * device-PROVEN closure (157 pkgs) drags systemd + dbus + dconf-service + libpam-systemd
 * (via libgtk-3-0t64's Depends), and galculator STILL installs+configures exit-0. Those
 * packages are inert here (ldconfig runs, no daemon is started, configure exits 0). The real
 * exit-73 trigger split into TWO classes (tools/app_closure_audit.py — re-classified for the
 * GENERAL maintscript-shim): (a) NEUTRALIZED_BY_SHIM — the DEBCONF / INIT-SCRIPT / CONFFILE-
 * MAINTSCRIPT / registration class the now-ALWAYS-applied maintscript-shim drives to exit 0:
 * x11-common (was exit 127) / libpaper1 (was exit 2) / xfonts-* / appstream (rm_conffile
 * PREINST) / session-migration / gsettings-desktop-schemas / glib-networking* — these NO
 * LONGER make an app HEAVY; (b) the GENUINELY-unsatisfiable class the shim CANNOT fake:
 * perl-base / dictionaries-common / emacsen-common (mousepad/gedit), bubblewrap / ghostscript
 * (eog/evince/nautilus), gstreamer1.0-plugins-* and the live daemons (polkit / accountsservice
 * / cups / avahi / colord). An entry is LIKELY-PASS iff its delta has ZERO class-(b) triggers
 * (class-(a) members in the delta are fine — the shim handles them); a BASE-PROVIDED target
 * (gimp) is ALREADY-INSTALLED (apt no-op, its postinsts never run). This re-classified model
 * still reproduces the device PASS/FAIL ground truth (galculator/l3afpad/htop/gimp/foot/
 * netsurf-gtk PASS; mousepad/gedit/eog STAY HEAVY for their real perl/sandbox reasons) AND now
 * FLIPS the shim-unlocked classes: gnome-calculator + xpdf/nsxiv/feh/qiv (x11-common/libpaper1
 * only) + qpdfview (x11-common only) → LIKELY-PASS. Proof status per entry:
 *   DEVICE-PROVEN (installs+configures exit-0 on device): galculator, l3afpad.
 *   HOST-AUDITED LIKELY-PASS (delta has 0 genuinely-heavy triggers; device-test pending):
 *     gpicview, xarchiver, sakura, viewnior, qalculate-gtk, mate-calc, geany.
 *     (htop = proven-class ncurses leaf, 5-pkg delta, 0 triggers.)
 *   X11-ONLY + LIKELY-PASS → ROUTED THROUGH XWAYLAND (needsXwayland=true): xzgv (delta 16),
 *     xli (delta 4), and — NOW RE-ADDED via the GENERAL maintscript-shim — nsxiv (delta 33),
 *     feh (delta 37), qiv (delta 40), xpdf (delta 18). Their EXEC binaries link libX11 (or a
 *     Motif/Xt closure for xpdf) but NOT libwayland-client (host: tools/elf_needed), so they
 *     cannot bind the native Wayland compositor directly; NativeAppSession.XwaylandLaunch
 *     starts a ROOTFUL Xwayland :0 and injects DISPLAY=:0 (Xwayland → wl_shm → SurfaceView,
 *     the xcalc device-proven path). Install is now clean: their ONLY blockers were the
 *     x11-common(exit 127)+libpaper1(exit 2)+xfonts-* debconf/init postinsts, which the
 *     ALWAYS-applied maintscript-shim (AptInstaller.MAINTSCRIPT_SHIM_OVERLAY, generalized from
 *     gnome-only) drives to exit 0 → app_closure_audit.py --live now audits them LIKELY-PASS.
 *   apt Qt-GUI + LIKELY-PASS → ROUTED THROUGH XWAYLAND (needsXwayland=true): qpdfview (delta
 *     77) — a Qt6 PDF viewer whose ONLY install blocker was x11-common (libqt6gui6t64 → libsm6
 *     → x11-common), now shim-neutralized. It ships NO qt6-wayland platform plugin in its
 *     closure, so Qt's xcb plugin connects to the rootful Xwayland (DISPLAY=:0). This is the
 *     apt-Qt-GUI install class unlocked by the general shim (distinct from the OVERLAY-delivered
 *     qmleasing Qt-on-Wayland demo below — that one needs no apt at all).
 *   GNOME-PLATFORM, shim-unlocked (host artifacts built, device-verify pending):
 *     org.gnome.Calculator — its blockers (appstream/session-migration/gsettings-desktop-
 *     schemas/glib-networking*) are now ALL maintscript-shim-neutralized → it FLIPS to
 *     LIKELY-PASS (app_closure_audit.py --live). The gnome-SPECIFIC overlays stay gnome-gated:
 *     host-precompiled gschemas (gnome-schemas overlay) + runtime session-dbus shim
 *     (dbus-daemon overlay + GnomePlatformShim). See AptInstaller + NativeAppSession.
 * The general maintscript-shim is the keystone of this catalog's breadth: it is cosmetic-safe
 * (no init/systemd in the guest → conffile/init/registration churn is a no-op) and is staged
 * for EVERY install, so the X11-image-viewer (nsxiv/feh/qiv/xpdf) + apt-Qt-GUI (qpdfview) +
 * gnome (gnome-calculator) classes all install cleanly. Apps that stay HEAVY do so for REAL
 * reasons the shim cannot fake: perl/dict registration (mousepad/gedit), a setuid/namespace
 * sandbox (eog/evince/nautilus → bubblewrap), or a live daemon (gstreamer-codecs, polkit,
 * accountsservice). The OVERLAY-delivered Qt-on-Wayland demo (tools/build_toolkit_overlays.py
 * `qt6-gui` → qmleasing + qtwayland generic wl_shm plugin, launched via MainActivity's qt6 GUI
 * probe) remains a SEPARATE no-apt path. See docs/research/maintscript-shim-generalization.md +
 * docs/research/qt-toolkit-app-class.md.
 * appId == the `.desktop` basename so a successful install self-reconciles a launcher tile via
 * DesktopEntryScanner — EXCEPT Terminal=true entries (htop, sakura) and the no-.desktop X11
 * viewer xli, which the scanner drops/never sees; those still install+launch via this catalog's
 * explicit appId→apt map but do not auto-surface a scanned tile (noted on each such entry). The
 * GNOME entry's appId is the reverse-DNS .desktop basename (org.gnome.Calculator) while its apt
 * pkg/binary is gnome-calculator.
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
        // HOST-AUDITED LIKELY-PASS (tools/app_closure_audit.py): 설치 DELTA 는 galculator
        // 의 53-패키지 envelope 에 sakura + libvte-2.91-0 + libvte-2.91-common 셋만 더한
        // 것이고, exit-73 maintainer-script 트리거가 0 이다(libvte 의 postinst 는 ldconfig
        // 뿐). 닫힘이 systemd/dbus/dconf-service 를 (모든 GTK3 앱처럼) 끌지만 galculator 가
        // 그것들과 함께 device configure exit-0 을 증명했다 → 동급 통과(device 미검증).
        // ⚠ sakura.desktop 은 Terminal=true → DesktopEntryScanner 가 (Phase-1 TTY 미지원
        // 정책상) 드롭하므로 설치 후 자동 타일 재조정은 안 된다; 본 카탈로그의 명시
        // appId→apt 맵으로 설치/실행은 가능. appId 는 .desktop basename(sakura.desktop) 과
        // 일치. Exec=`sakura`, 바이너리 /usr/bin/sakura. htop 이 "터미널 안의 ncurses"
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
        // gpicview — LXDE 의 가벼운 GTK3 이미지 뷰어(독립형, GNOME 플랫폼 비의존).
        // HOST-AUDITED LIKELY-PASS (tools/app_closure_audit.py): gpicview 의 noble 닫힘은
        // 157 패키지로 galculator 와 동일하게 systemd/dbus/dconf-service/libpam-systemd 를
        // 끌지만(이는 libgtk-3-0t64 의 Depends 라 모든 GTK3 앱이 동일 — galculator 가
        // device 에서 그 패키지들과 함께 configure exit-0 을 증명), 설치 DELTA(닫힘 −
        // base 재구성-dpkg-DB installed-set)가 galculator 와 글자 그대로 동일한 53-패키지
        // (gpicview leaf 만 다름)이고 exit-73 트리거(perl-base/dictionaries-common/
        // gsettings-desktop-schemas/appstream/session-migration/glib-networking/bubblewrap/
        // ghostscript/gstreamer)가 0 이다. eog 와 달리 libgnome-desktop-3/gsettings-
        // desktop-schemas/appstream 트리거가 닫힘에 없다 → galculator 동급 configure 통과
        // (device 미검증). appId 는 .desktop basename(gpicview.desktop) 과 일치(NoDisplay=
        // false → 설치 후 DesktopEntryScanner 가 타일 재조정). Exec=`gpicview %U`(strip),
        // 바이너리 /usr/bin/gpicview — 푸시한 test.png 를 인자로 연다.
        CatalogApp(
            appId = "gpicview",
            name = "GPicView",
            summary = "가벼운 LXDE 이미지 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gpicview"),
            category = AppCategory.GRAPHICS,
            description = "LXDE 의 가벼운 GTK3 이미지 뷰어(독립형, GNOME 플랫폼 비의존). " +
                "apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다(GDK Wayland " +
                "백엔드, 소프트웨어 렌더). eog 와 달리 gsettings-desktop-schemas/appstream/" +
                "session-migration 같은 exit-73 트리거가 설치 delta 에 없어(galculator 동급) " +
                "dpkg configure 가 끝까지 통과한다(host-audited). " +
                "noble 패키지 gpicview → /usr/share/applications/gpicview.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "gpicview", 700_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 700_000L,
            source = AppSource.APT,
        ),
        // xarchiver — GTK3 아카이브 관리자(독립형, 데스크톱 비종속).
        // HOST-AUDITED LIKELY-PASS (tools/app_closure_audit.py): xarchiver 의 설치 DELTA 는
        // galculator 의 53-패키지 envelope 에 자기 leaf(xarchiver) 하나만 더한 것이고,
        // exit-73 maintainer-script 트리거(perl-base/dictionaries-common/gsettings-desktop-
        // schemas/appstream/session-migration/glib-networking/bubblewrap/ghostscript/
        // gstreamer)가 0 이다. 닫힘이 systemd/dbus/dconf-service 를 (모든 GTK3 앱처럼)
        // 끌긴 하나 그것들은 galculator 가 device 에서 inert 임을 증명했다 → galculator
        // 동급 configure 통과(device 미검증). 압축 CLI(tar/zip/unzip)는 Depends 가 아닌
        // Recommends 라 install 을 막지 않음(런타임에 필요). appId 는 .desktop basename
        // (xarchiver.desktop) 과 일치(NoDisplay=false → 타일 재조정).
        // Exec=`xarchiver %F`(%F strip), 바이너리 /usr/bin/xarchiver.
        CatalogApp(
            appId = "xarchiver",
            name = "Xarchiver",
            summary = "가벼운 GTK 압축 관리자",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/xarchiver"),
            category = AppCategory.UTILITY,
            description = "GTK3 아카이브(zip/tar/…) 관리자 — 독립형, 데스크톱 환경 비종속. " +
                "apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다. 설치 delta 가 " +
                "galculator 와 동급(exit-73 maintainer-script 트리거 0)이라 dpkg configure 가 " +
                "끝까지 통과한다(host-audited). noble 패키지 xarchiver → " +
                "/usr/share/applications/xarchiver.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "xarchiver", 1_300_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 1_300_000L,
            source = AppSource.APT,
        ),
        // viewnior — GTK3 경량 이미지 뷰어(독립형, GNOME 비의존). gpicview 와 같은 부류로,
        // HOST-AUDITED LIKELY-PASS (tools/app_closure_audit.py): 설치 DELTA 가 galculator
        // envelope ⊕ {viewnior + 소수 이미지 leaf(libexif 등)}이고 exit-73 maintainer-script
        // 트리거(perl-base/gsettings-desktop-schemas/appstream/session-migration/glib-
        // networking/bubblewrap/ghostscript/gstreamer)가 0 이다. 닫힘이 systemd/dbus/dconf-
        // service 를 (모든 GTK3 앱처럼) 끌지만 galculator 가 device 에서 inert 임을 증명 →
        // 동급 configure 통과(device 미검증). gpicview 보다 회전/슬라이드쇼/EXIF 등 기능이
        // 풍부. appId 는 .desktop basename(viewnior.desktop) 과 일치(NoDisplay=false →
        // 설치 후 DesktopEntryScanner 가 타일 재조정). Exec=`viewnior %F`(strip), 바이너리
        // /usr/bin/viewnior.
        CatalogApp(
            appId = "viewnior",
            name = "Viewnior",
            summary = "가벼운 GTK 이미지 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/viewnior"),
            category = AppCategory.GRAPHICS,
            description = "GTK3 기반의 빠르고 가벼운 이미지 뷰어(회전·슬라이드쇼·EXIF). " +
                "apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다(GDK Wayland 백엔드). " +
                "설치 delta 가 galculator 와 동급(exit-73 maintainer-script 트리거 0)이라 dpkg " +
                "configure 가 끝까지 통과한다(host-audited). noble 패키지 viewnior → " +
                "/usr/share/applications/viewnior.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "viewnior", 703_488L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 703_488L,
            source = AppSource.APT,
        ),
        // xzgv — GTK2 썸네일 이미지 뷰어(독립형, X11-ONLY). HOST-AUDITED LIKELY-PASS
        // (tools/app_closure_audit.py): 설치 DELTA 16-패키지, exit-73 maintainer-script
        // 트리거 0 — 카탈로그 이미지 뷰어 중 가장 작은 닫힘에 속한다(썸네일 그리드 + 단일
        // 뷰). 닫힘이 GTK 경유로 systemd/dbus 를 끌지만 galculator 가 inert 증명 → 동급
        // 통과(device 미검증). appId 는 .desktop basename(xzgv.desktop) 과 일치(NoDisplay=
        // false → 타일 재조정). Exec=`xzgv %F`(strip), 바이너리 /usr/bin/xzgv.
        // ★ needsXwayland=true (TASK-A): xzgv 의 EXEC 바이너리는 libgtk-x11-2.0.so.0 +
        // libX11.so.6 만 링크하고 libwayland-client.so.0 가 없는 **X11-only** 클라이언트라
        // (host: tools/elf_needed) ALR 의 네이티브 Wayland 컴포지터에 직접 붙을 수 없다 —
        // DISPLAY 없이 보내면 "cannot open display" 로 죽는다. 런치 경로(NativeAppSession.
        // XwaylandLaunch)가 이 앱에 한해 ROOTFUL Xwayland :0 을 띄우고 DISPLAY=:0 를 주입해
        // Xwayland→wl_shm→SurfaceView 로 렌더되게 한다(xcalc device-proven 경로와 동일).
        CatalogApp(
            appId = "xzgv",
            name = "xzgv",
            summary = "가벼운 썸네일 이미지 뷰어(X11)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/xzgv"),
            category = AppCategory.GRAPHICS,
            description = "GTK2/X11 기반의 가벼운 썸네일/단일 이미지 뷰어. libX11 만 링크하는 " +
                "X11-only 앱이라 ROOTFUL Xwayland :0 경유로(DISPLAY=:0 → wl_shm → SurfaceView) " +
                "ALR 컴포지터 위 창으로 실행됩니다(xcalc 와 동일 경로). 설치 delta(16 패키지)가 " +
                "galculator 동급(exit-73 트리거 0)이라 dpkg configure 가 끝까지 통과한다" +
                "(host-audited). noble 패키지 xzgv → /usr/share/applications/xzgv.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "xzgv", 326_656L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 326_656L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // xli — 고전 Xlib 이미지 뷰어(독립형, X11-ONLY). TASK-C: delta-cascade 검증
        // (tools/app_closure_audit.py --live) LIKELY-PASS — 설치 DELTA 단 4-패키지
        // (libjpeg8/libpng16/libx11-6/libxext6 중 base 미제공분), exit-73 maintainer-script
        // 트리거 0. 카탈로그 전체에서 가장 작은 닫힘(16-패키지)에 속한다. EXEC 바이너리
        // /usr/bin/xli 는 libX11.so.6 만 링크하는 X11-only 클라이언트라(host: tools/elf_needed,
        // libwayland-client 없음) needsXwayland=true 로 표시 → XwaylandLaunch 가 ROOTFUL
        // Xwayland :0 경유로 렌더한다(xzgv 와 동일). ⚠ xli 패키지는 .desktop 을 ship 하지
        // 않으므로(htop/sakura 처럼) 설치 후 DesktopEntryScanner 가 타일을 자동 재조정하지
        // 않는다; 본 카탈로그의 명시 appId→apt 맵으로 설치/실행만 가능. appId=apt=바이너리
        // basename(xli) 일치. xli 는 인자로 이미지 경로를 받는다(예 `xli test.png`).
        CatalogApp(
            appId = "xli",
            name = "xli",
            summary = "고전 Xlib 이미지 뷰어(X11)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/xli"),
            category = AppCategory.GRAPHICS,
            description = "X11(Xlib) 기반의 초경량 이미지 뷰어 — JPEG/PNG/GIF/TIFF 등을 연다. " +
                "libX11 만 링크하는 X11-only 앱이라 ROOTFUL Xwayland :0 경유로(DISPLAY=:0 → " +
                "wl_shm → SurfaceView) ALR 컴포지터 위 창으로 실행됩니다. 설치 delta 가 단 4 " +
                "패키지(exit-73 트리거 0)로 카탈로그 최소 닫힘에 속해 dpkg configure 가 끝까지 " +
                "통과한다(host-audited LIKELY-PASS). ⚠ .desktop 미동봉(터미널 호출형)이라 설치 " +
                "후 타일 자동 재조정은 안 되며 명시 맵으로 실행. noble 패키지 xli → /usr/bin/xli.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "xli", 406_528L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 406_528L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // ✅ xpdf — RE-ADDED (was DROPPED for device-proven x11-common exit 127 + libpaper1
        // exit 2). Those are EXACTLY the debconf/init-script postinsts the GENERAL maintscript-
        // shim now neutralizes (AptInstaller stages it for EVERY install), so xpdf's delta(18)
        // has 0 genuinely-heavy triggers → app_closure_audit.py --live audits it LIKELY-PASS.
        // xpdf is a Motif/Xt PDF viewer: its EXEC binary pulls libXm/libXt/libX11 (no libwayland-
        // client; host: tools/elf_needed) → X11-only → needsXwayland=true (ROOTFUL Xwayland :0,
        // DISPLAY=:0 → wl_shm → SurfaceView, the xcalc path). ⚠ ships NO .desktop (terminal-call
        // style, like htop/xli) → no auto-tile; install/launch via the explicit appId→apt map.
        // Takes a PDF path arg (`xpdf doc.pdf`). appId=apt=binary basename(xpdf).
        CatalogApp(
            appId = "xpdf",
            name = "Xpdf",
            summary = "가벼운 X11 PDF 뷰어(Motif)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/xpdf"),
            category = AppCategory.GRAPHICS,
            description = "Motif/Xt(X11) 기반의 가벼운 PDF 뷰어. libXm/libX11 만 링크하는 " +
                "X11-only 앱이라 ROOTFUL Xwayland :0 경유로(DISPLAY=:0 → wl_shm → SurfaceView) " +
                "ALR 컴포지터 위 창으로 실행됩니다(xcalc 와 동일 경로). 설치 delta(18 패키지)의 " +
                "유일한 차단 요인이던 x11-common(exit 127)+libpaper1(exit 2) debconf/init-script " +
                "postinst 가 GENERAL maintscript-shim 으로 무력화되어 dpkg configure 가 끝까지 " +
                "통과한다(host-audited LIKELY-PASS). ⚠ .desktop 미동봉 → 명시 맵으로 실행. " +
                "noble 패키지 xpdf → /usr/bin/xpdf.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "xpdf", 6_950_912L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 6_950_912L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // qalculate-gtk — GTK3 강력 계산기(단위/통화/대수/플롯). galculator 보다 기능이
        // 월등하나 닫힘 부류는 같다. HOST-AUDITED LIKELY-PASS (tools/app_closure_audit.py):
        // 설치 DELTA 62-패키지가 galculator envelope ⊕ {qalculate leaf + libqalculate}이고
        // exit-73 maintainer-script 트리거 0 — appstream/gsettings-desktop-schemas/
        // session-migration 미의존(gnome-calculator 와 결정적 차이). 닫힘이 GTK 경유로
        // systemd/dbus 를 끌지만 galculator 가 inert 증명 → 동급 통과(device 미검증).
        // appId 는 .desktop basename(qalculate-gtk.desktop) 과 일치(NoDisplay=false → 타일
        // 재조정). Exec=`qalculate-gtk`(인자 없음), 바이너리 /usr/bin/qalculate-gtk.
        CatalogApp(
            appId = "qalculate-gtk",
            name = "Qalculate!",
            summary = "강력한 GTK 계산기(단위·통화·대수)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/qalculate-gtk"),
            category = AppCategory.UTILITY,
            description = "GTK3 기반의 강력한 데스크톱 계산기 — 단위 변환·통화·기호 대수· " +
                "함수 플롯을 지원한다. apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 " +
                "실행됩니다. gnome-calculator 와 달리 appstream/gsettings-desktop-schemas/" +
                "session-migration 같은 exit-73 트리거가 설치 delta 에 없어(galculator 동급) " +
                "dpkg configure 가 끝까지 통과한다(host-audited). noble 패키지 qalculate-gtk " +
                "→ /usr/share/applications/qalculate-gtk.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "qalculate-gtk", 6_804_480L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 6_804_480L,
            source = AppSource.APT,
        ),
        // mate-calc — MATE 데스크톱의 GTK3 계산기(독립형, GNOME 플랫폼 비의존). TASK-C:
        // delta-cascade 검증(tools/app_closure_audit.py --live) LIKELY-PASS — 설치 DELTA
        // 56-패키지, exit-73 maintainer-script 트리거 0. gnome-calculator 와 결정적 차이는
        // appstream/gsettings-desktop-schemas/session-migration 미의존(galculator 동급).
        // EXEC 바이너리 /usr/bin/mate-calc 는 순수 libgtk-3.so.0 만 링크하고 libX11 가 없는
        // **Wayland-가능** 앱이라(host: tools/elf_needed) GDK Wayland 백엔드로 ALR 컴포지터에
        // 직접 붙는다 — needsXwayland 불필요(기본 false). appId=apt=바이너리 basename(mate-calc)
        // 일치 + mate-calc.desktop ship(NoDisplay=false → 설치 후 타일 재조정). 인자 없음.
        CatalogApp(
            appId = "mate-calc",
            name = "MATE Calculator",
            summary = "MATE GTK3 계산기(기본·과학·금융)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/mate-calc"),
            category = AppCategory.UTILITY,
            description = "MATE 데스크톱의 GTK3 계산기 — 기본·과학·금융·프로그래밍 모드. apt 로 " +
                "설치되어 ALR Wayland 컴포지터 위 창으로 실행됩니다(순수 GTK3, GDK Wayland " +
                "백엔드 — libX11 비링크). gnome-calculator 와 달리 appstream/gsettings-desktop-" +
                "schemas/session-migration 같은 exit-73 트리거가 설치 delta(56 패키지)에 없어 " +
                "galculator 동급으로 dpkg configure 가 끝까지 통과한다(host-audited LIKELY-PASS). " +
                "noble 패키지 mate-calc → /usr/share/applications/mate-calc.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "mate-calc", 542_720L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 542_720L,
            source = AppSource.APT,
        ),
        // geany — GTK3 경량 IDE/프로그래머 텍스트 편집기(독립형). TASK-C: delta-cascade
        // 검증(tools/app_closure_audit.py --live) LIKELY-PASS — 설치 DELTA 54-패키지,
        // exit-73 maintainer-script 트리거 0. l3afpad 보다 무겁지만(코드 폴딩·플러그인·
        // 빌드 실행) 닫힘 부류는 galculator 동급(perl/appstream/gsettings 트리거 0). EXEC
        // 바이너리 /usr/bin/geany 는 libgeany.so.0(→GTK3 전이) 경유라 libX11 직접 링크가
        // 없는 **Wayland-가능** 앱(host: tools/elf_needed) → GDK Wayland 백엔드로 ALR
        // 컴포지터에 직접 붙는다(needsXwayland 불필요). appId=apt=바이너리 basename(geany)
        // 일치 + geany.desktop ship(NoDisplay=false → 타일 재조정). Exec=`geany %F`(strip).
        CatalogApp(
            appId = "geany",
            name = "Geany",
            summary = "가벼운 GTK IDE/코드 편집기",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/geany"),
            category = AppCategory.UTILITY,
            description = "GTK3 기반의 가벼운 IDE — 구문 강조·코드 폴딩·심볼 목록·빌드 실행을 " +
                "지원하는 프로그래머 편집기. apt 로 설치되어 ALR Wayland 컴포지터 위 창으로 " +
                "실행됩니다(libgeany→GTK3, GDK Wayland 백엔드, libX11 직접 비링크). 설치 " +
                "delta(54 패키지)가 galculator 동급(exit-73 트리거 0)이라 dpkg configure 가 " +
                "끝까지 통과한다(host-audited LIKELY-PASS). noble 패키지 geany → " +
                "/usr/share/applications/geany.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "geany", 4_129_792L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 4_129_792L,
            source = AppSource.APT,
        ),
        // ✅ nsxiv — RE-ADDED (was DROPPED for x11-common+libpaper1+xfonts-*). nsxiv IS
        // X11-only (EXEC binary links libX11.so.6, no libwayland-client — host: tools/
        // elf_needed) so needsXwayland=true. Its apt delta(33) was HEAVY ONLY for the
        // x11-common(exit 127)+libpaper1(exit 2)+xfonts-encodings+xfonts-utils debconf/init
        // postinsts — EXACTLY the class the GENERAL maintscript-shim now neutralizes (staged
        // for every install in AptInstaller) → app_closure_audit.py --live now audits it
        // LIKELY-PASS. ⚠ nsxiv ships a .desktop with NoDisplay/MimeType so the scanner may not
        // surface a tile; install/launch via the explicit appId→apt map. Takes an image path
        // arg (`nsxiv test.png`). appId=apt=binary basename(nsxiv).
        CatalogApp(
            appId = "nsxiv",
            name = "nsxiv",
            summary = "초경량 X11 이미지 뷰어(suckless)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/nsxiv"),
            category = AppCategory.GRAPHICS,
            description = "suckless 계열의 초경량 X11 이미지 뷰어(neo simple X image viewer). " +
                "libX11 만 링크하는 X11-only 앱이라 ROOTFUL Xwayland :0 경유로(DISPLAY=:0 → " +
                "wl_shm → SurfaceView) ALR 컴포지터 위 창으로 실행됩니다. 설치 delta(33)의 유일한 " +
                "차단 요인이던 x11-common+libpaper1+xfonts-* debconf/init postinst 가 GENERAL " +
                "maintscript-shim 으로 무력화되어 dpkg configure 가 끝까지 통과한다(host-audited " +
                "LIKELY-PASS). noble 패키지 nsxiv → /usr/bin/nsxiv.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "nsxiv", 1_011_712L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 1_011_712L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // ✅ feh — RE-ADDED (same shim unlock). Classic X11 image viewer/wallpaper setter.
        // EXEC binary links libX11.so.6 (no libwayland-client) → needsXwayland=true. apt
        // delta(37) HEAVY ONLY for x11-common+libpaper1+xfonts-* → maintscript-shim-neutralized
        // → app_closure_audit.py --live LIKELY-PASS. ⚠ no .desktop (terminal-call) → explicit
        // map. Takes an image path arg (`feh test.png`). appId=apt=binary basename(feh).
        CatalogApp(
            appId = "feh",
            name = "feh",
            summary = "고전 X11 이미지 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/feh"),
            category = AppCategory.GRAPHICS,
            description = "Imlib2 기반의 빠르고 가벼운 X11 이미지 뷰어(슬라이드쇼·몽타주·배경 " +
                "설정). libX11 만 링크하는 X11-only 앱이라 ROOTFUL Xwayland :0 경유로(DISPLAY=:0 " +
                "→ wl_shm → SurfaceView) ALR 컴포지터 위 창으로 실행됩니다. 설치 delta(37)의 " +
                "유일한 차단 요인이던 x11-common+libpaper1+xfonts-* postinst 가 GENERAL " +
                "maintscript-shim 으로 무력화되어 dpkg configure 가 통과한다(host-audited " +
                "LIKELY-PASS). ⚠ .desktop 미동봉 → 명시 맵으로 실행. noble 패키지 feh → /usr/bin/feh.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "feh", 1_400_832L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 1_400_832L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // ✅ qiv — RE-ADDED (same shim unlock). Quick Image Viewer (GTK2/Imlib2, X11). EXEC
        // binary links libgdk-x11-2.0+libX11 (no libwayland-client) → needsXwayland=true. apt
        // delta(40) HEAVY ONLY for x11-common+libpaper1+xfonts-* → maintscript-shim-neutralized
        // → app_closure_audit.py --live LIKELY-PASS. ⚠ no .desktop → explicit map. Takes an
        // image path arg (`qiv test.png`). appId=apt=binary basename(qiv).
        CatalogApp(
            appId = "qiv",
            name = "qiv",
            summary = "빠른 X11 이미지 뷰어(GTK2)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/qiv"),
            category = AppCategory.GRAPHICS,
            description = "GTK2/Imlib2 기반의 빠른 X11 이미지 뷰어(Quick Image Viewer). " +
                "libgdk-x11/libX11 을 링크하는 X11-only 앱이라 ROOTFUL Xwayland :0 경유로 " +
                "(DISPLAY=:0 → wl_shm → SurfaceView) ALR 컴포지터 위 창으로 실행됩니다. 설치 " +
                "delta(40)의 유일한 차단 요인이던 x11-common+libpaper1+xfonts-* postinst 가 " +
                "GENERAL maintscript-shim 으로 무력화되어 dpkg configure 가 통과한다(host-audited " +
                "LIKELY-PASS). ⚠ .desktop 미동봉 → 명시 맵으로 실행. noble 패키지 qiv → /usr/bin/qiv.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "qiv", 401_408L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 401_408L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // ✅ qpdfview — NEW (apt Qt-GUI class, unlocked by the GENERAL shim). A Qt6 PDF viewer
        // (tabbed, poppler). Its ONLY install blocker was x11-common (libqt6gui6t64 → libsm6 →
        // x11-common, the device-proven exit-127 postinst), now maintscript-shim-neutralized →
        // app_closure_audit.py --live audits it LIKELY-PASS (delta 77, 0 genuinely-heavy
        // triggers). It ships NO qt6-wayland platform plugin in its closure (host: only
        // libqt6gui6t64; the libwayland-client0 it drags is a transitive .so, not the Qt
        // wayland plugin), so Qt's xcb platform plugin connects to the ROOTFUL Xwayland :0 →
        // needsXwayland=true (DISPLAY=:0 → wl_shm → SurfaceView). This is the apt-Qt-GUI
        // install class, distinct from the OVERLAY-delivered qmleasing Qt-on-Wayland demo
        // (no apt). qpdfview ships qpdfview.desktop(NoDisplay=false) → tile auto-reconciles.
        // Takes a PDF path arg (`qpdfview doc.pdf`). appId=apt=binary basename(qpdfview).
        CatalogApp(
            appId = "qpdfview",
            name = "qpdfview",
            summary = "탭형 Qt PDF 뷰어",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/qpdfview"),
            category = AppCategory.GRAPHICS,
            description = "Qt6/poppler 기반의 탭형 PDF 뷰어. apt 로 설치되며, 유일한 설치 차단 " +
                "요인이던 x11-common(libqt6gui6t64→libsm6→x11-common, exit 127) postinst 가 " +
                "GENERAL maintscript-shim 으로 무력화되어 dpkg configure 가 통과한다(host-audited " +
                "LIKELY-PASS, delta 77). qt6-wayland 플랫폼 플러그인을 닫힘에 포함하지 않으므로 " +
                "Qt 의 xcb 플러그인이 ROOTFUL Xwayland :0 에 연결된다 → needsXwayland=true " +
                "(DISPLAY=:0 → wl_shm → SurfaceView). noble 패키지 qpdfview → " +
                "/usr/share/applications/qpdfview.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "qpdfview", 5_500_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 5_500_000L,
            source = AppSource.APT,
            needsXwayland = true,
        ),
        // ----------------------------------------------------------------------------- //
        // gnome-calculator — GNOME-platform GTK4 계산기. TASK-A: gnome-platform 클래스의
        // 첫 해금 대상. 설치 DELTA(78)가 gsettings-desktop-schemas + libappstream5 +
        // session-migration + glib-networking* 를 끌어 기본 dpkg --configure 가 exit-73
        // 로 실패한다(mate-calc/qalculate-gtk 와 결정적 차이). 이를 두 갈래로 푼다:
        //   (1) 설치-configure: AptInstaller 가 GNOME 패키지 설치 시 (a) policy-rc.d=101 +
        //       DEBIAN_FRONTEND=noninteractive 로 x11-common/session-migration 의 init/
        //       systemd 등록 postinst 를 무력화하고, (b) gschemas 를 HOST 에서 미리 컴파일한
        //       common-data `schemas` 그룹 오버레이(gschemas.compiled)를 깔아 gsettings
        //       스키마 런타임 요구를 만족시킨다 → installed=true 도달(host-analyzable).
        //   (2) 런타임 session-dbus: 실행 직전 게스트에서 `dbus-daemon --session` 을 띄우고
        //       DBUS_SESSION_BUS_ADDRESS / GSETTINGS_SCHEMA_DIR 를 export(GNOME 앱에만,
        //       NativeAppSession 의 gnome-platform 게이트). dbus-daemon 은 dbus-daemon-
        //       stage.tar 오버레이로 제공(base 는 libdbus-1.so.3 만 가짐).
        // appId 는 .desktop basename(org.gnome.Calculator.desktop) 과 일치 → 설치 후
        // DesktopEntryScanner 가 타일 재조정. Exec=`gnome-calculator`, 바이너리
        // /usr/bin/gnome-calculator. ⚠ HONEST: device 미검증(소유 에이전트 점유) — 본
        // 변경은 host-analyzable 한 installed=true 경로 + 런타임 shim 배선까지이며, 최종
        // 창 렌더는 DEVICE-VERIFY 체크리스트 항목이다.
        CatalogApp(
            appId = "org.gnome.Calculator",
            name = "GNOME Calculator",
            summary = "GNOME 계산기(GTK4, 단위·통화·프로그래머 모드)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/gnome-calculator"),
            category = AppCategory.UTILITY,
            description = "GNOME 플랫폼 GTK4 계산기 — 기본/고급/금융/프로그래밍 모드. " +
                "gnome-platform 클래스라 설치 시 gsettings-desktop-schemas/appstream/" +
                "session-migration postinst 가 exit-73 로 실패하던 것을, (1) AptInstaller 의 " +
                "GNOME 설치-configure 무력화(policy-rc.d=101 + noninteractive) + HOST 사전 " +
                "컴파일 gschemas 오버레이, (2) 실행 시 guest dbus-daemon --session 셔임으로 " +
                "해금한다. noble 패키지 gnome-calculator → " +
                "/usr/share/applications/org.gnome.Calculator.desktop.",
            rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, "gnome-calculator", 4_300_000L)),
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 4_300_000L,
            source = AppSource.APT,
        ),
        // ----------------------------------------------------------------------------- //
        // Chromium — the SPECIAL case (NOT a normal apt app). Two differences from every
        // entry above, both carried as catalog fields so the launcher routes it correctly:
        //
        //   (1) PROVISION = OVERLAY (not apt). noble's `chromium`/`chromium-browser` apt
        //       package is a 121KiB SNAP STUB — apt cannot fetch a real ELF browser. The full
        //       chromium is provisioned by a pre-built OVERLAY (chromium-gui-stage.tar, built
        //       by tools/build_chromium_gui_stage.py: Debian bookworm chromium under
        //       /usr/lib/chromium/, base-subtracted closure, NSS dlopen plugins, flat
        //       libpulsecommon). "Installed" therefore = the chromium binary is PRESENT in the
        //       rootfs (overlay extracted), NOT a dpkg status → overlayBinaryPath =
        //       usr/lib/chromium/chromium. install() for an OVERLAY app does NOT touch
        //       AptInstaller: if the binary is already present it is Done; else if the overlay
        //       tar is on the device it extractOverlay()s it; else it reports honestly that the
        //       (large, pre-built) component must be provided (no fake apt install). The older
        //       chromium-stage.tar shipped chromium-headless-shell (no window backend); the
        //       installed-probe also accepts that binary so a device that staged either tar
        //       reads as installed (NativeAlrRuntime.chromiumInstalledBinary).
        //
        //   (2) LAUNCH via launchActivity = ".ui.ChromiumStandalone" (not RunningSurfaceActivity).
        //       The ChromiumStandalone activity-alias (AndroidManifest, targetActivity=
        //       .MainActivity, exported=false) runs the LEAN MainActivity.runChromiumStandalone
        //       path, which SKIPS the heavy onCreate (GPU/probe/GIMP/toolkit) to give chromium
        //       the memory headroom it needs — browser + renderer are each ~800MB in-process
        //       re-maps, and the full GUI baseline OOMs it. Routing it through the generic
        //       RunningSurfaceActivity would OOM, so the launcher special-cases this appId and
        //       starts the alias by explicit Intent (same-app → exported=false is fine).
        //       runChromiumStandalone itself is UNCHANGED — the catalog only ROUTES to its alias.
        //
        // ⚠ HONEST device-pending: the owning chromium-Vulkan agent holds the device, so this
        //   change is host-implemented + compile-green + host-pytest; the on-compositor render
        //   is a DEVICE-VERIFY checklist item (push the overlay tar → tile shows installed →
        //   tap → ChromiumStandalone → chromium window). appId="org.chromium.Chromium" is the
        //   stable launch key; it is NOT a .desktop the scanner sees (chromium is an overlay
        //   browser, no /usr/share/applications entry in the scan path), so NativeAlrRuntime
        //   SYNTHESIZES its InstalledApp when overlayBinaryPath is present (refreshInstalledApps).
        CatalogApp(
            appId = "org.chromium.Chromium",
            name = "Chromium",
            summary = "오픈소스 웹 브라우저(Ozone-Wayland)",
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/lib/chromium/chromium"),
            category = AppCategory.INTERNET,
            description = "Chromium 웹 브라우저 — ALR Wayland 컴포지터 위 창으로 실행됩니다 " +
                "(--ozone-platform=wayland). noble 의 chromium apt 패키지는 121KiB snap 스텁이라 " +
                "apt 로 받을 수 없어, 미리 빌드된 overlay(chromium-gui-stage.tar, Debian bookworm " +
                "chromium → /usr/lib/chromium/)로 제공된다. 설치됨 = 그 바이너리가 rootfs 에 존재 " +
                "(overlay 추출됨)이며, 다른 앱보다 메모리를 많이 써(browser+renderer 각 ~800MB " +
                "in-proc re-map) LEAN runChromiumStandalone 경로(.ui.ChromiumStandalone alias)로 " +
                "띄운다 — 일반 RunningSurfaceActivity 로 띄우면 OOM. 빌더 " +
                "tools/build_chromium_gui_stage.py.",
            rootfsDeps = emptyList(), // OVERLAY-provisioned: NOT an apt closure.
            display = DisplaySpec(DisplaySpec.DisplayMode.WINDOWED),
            installSizeBytes = 337_000_000L, // ~337MB full-browser overlay (download-size hint).
            source = AppSource.BUNDLED,
            provision = ProvisionKind.OVERLAY,
            overlayBinaryPath = "usr/lib/chromium/chromium",
            launchActivity = ".ui.ChromiumStandalone",
        ),
    )

    private val aptRefByAppId: Map<String, String> =
        apps.mapNotNull { c -> c.rootfsDeps.firstOrNull { it.kind == RootfsDepKind.APT }?.ref?.let { c.appId to it } }
            .toMap()

    /** appId → apt package name, or null if [appId] is not a bundled apt-installable app. */
    fun aptRefFor(appId: String): String? = aptRefByAppId[appId]

    /**
     * SSOT for the X11-only routing flag, DERIVED from the catalog entries' `needsXwayland`
     * (so the set can never drift from the entries). An X11-only app (libX11/Motif, no
     * libwayland-client) must be launched through a ROOTFUL Xwayland :0 — the UI layer reads
     * this by appId to set `LaunchRequest.protocol = SurfaceProtocol.X11`, which
     * NativeAppSession.XwaylandLaunch.needsX11 already honors. Kept here (the catalog file) as
     * the single source so adding an X11 entry above is the ONLY edit needed to route it.
     */
    val xwaylandAppIds: Set<String> =
        apps.filter { it.needsXwayland }.map { it.appId }.toSet()

    /** True iff [appId] is an X11-only catalog app that must be routed through Xwayland. */
    fun needsXwayland(appId: String): Boolean = appId in xwaylandAppIds

    /**
     * SSOT for the per-app launch-Activity override, DERIVED from the entries' `launchActivity`
     * (so the set can never drift from the catalog). An app with a non-null launchActivity
     * (chromium → ".ui.ChromiumStandalone") must NOT be launched through the generic
     * RunningSurfaceActivity — LauncherActivity.startRunningSurface reads this by appId and
     * starts the named Activity by explicit Intent instead. Kept here so adding a launchActivity
     * entry above is the ONLY edit needed to route it.
     */
    private val launchActivityByAppId: Map<String, String> =
        apps.mapNotNull { c -> c.launchActivity?.let { c.appId to it } }.toMap()

    /** Per-app launch-Activity FQCN override, or null if [appId] uses RunningSurfaceActivity. */
    fun launchActivityFor(appId: String): String? = launchActivityByAppId[appId]

    /**
     * SSOT for the OVERLAY-provisioned apps (chromium): appId → its `CatalogApp` (carrying
     * overlayBinaryPath). NativeAlrRuntime uses this to (a) synthesize the InstalledApp when the
     * overlay binary is present and (b) drive install() down the extractOverlay path instead of
     * AptInstaller. Derived from the entries (provision==OVERLAY) so it can never drift.
     */
    val overlayApps: Map<String, CatalogApp> =
        apps.filter { it.provision == ProvisionKind.OVERLAY }.associateBy { it.appId }

    /** The OVERLAY-provisioned catalog entry for [appId], or null if it is a normal apt app. */
    fun overlayAppFor(appId: String): CatalogApp? = overlayApps[appId]

    /** True iff [appId] is an OVERLAY-provisioned app (chromium) — NOT an apt install. */
    fun isOverlayProvisioned(appId: String): Boolean = appId in overlayApps
}
