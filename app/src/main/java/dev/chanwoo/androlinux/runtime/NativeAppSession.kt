/*
 * NativeAppSession — a single running Linux-guest session. Holds SessionState as a
 * StateFlow and drives the REAL compositor + loader wiring (the generalization of
 * MainActivity.runChromiumStandalone, parameterized by LaunchRequest) via AlrNative.
 *
 * The keystone is bindSurface(holder): on the SurfaceView's surfaceCreated the host hands
 * us the holder, and we (on a worker thread, never blocking the UI):
 *   1. await the extracted rootfs (RootfsInstaller, resolved once by NativeAlrRuntime),
 *   2. stage required overlays best-effort (only those whose tars are present),
 *   3. AlrNative.nativeWaylandCompositorStart(surface, device metrics)  → RENDERING,
 *   4. set base env (ALR_REEXEC_INPROC / ALR_PERSIST_GUEST / ALR_TEE_GUEST_STDOUT) + the
 *      request's env, then AlrNative.nativeAlrNativeLoaderProbe(... program ...), which
 *      BLOCKS until the guest exits → STOPPED(exit 0) / CRASHED.
 * program = NEWLINE-delimited argv: entryPath, then each arg on its own line — exactly the
 * program-spec runChromiumStandalone feeds the loader.
 *
 * surfaceChanged → compositorResize (rotation/multi-window). The compositor + injected
 * input are process-global, so a session that was backgrounded re-binds its surface on
 * resume (requestForeground) without restarting the guest.
 */
package dev.chanwoo.androlinux.runtime

import android.util.Log
import android.view.MotionEvent
import android.view.SurfaceHolder
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

private const val TAG = "alr_runtime"

class NativeAppSession internal constructor(
    private val request: LaunchRequest,
    private val runtime: NativeAlrRuntime,
) : AppSession {

    override val appId: String get() = request.appId

    private val _state = MutableStateFlow(SessionState.STARTING)
    override val state: StateFlow<SessionState> = _state.asStateFlow()

    /** Guest binary launched exactly once (surface may be recreated on rotation). */
    private val launched = AtomicBoolean(false)
    /** Compositor stood up at least once (gates resize + teardown). */
    private val compositorUp = AtomicBoolean(false)

    /** Last content-area surface size, shared by start + resize (mirror of runChromium). */
    private val surfaceW = AtomicInteger(0)
    private val surfaceH = AtomicInteger(0)
    private val refreshMhz = AtomicInteger(60000)

    @Volatile private var boundHolder: SurfaceHolder? = null

    internal fun setState(next: SessionState) {
        if (_state.value != next) _state.value = next
    }

    /** True once nativeWaylandCompositorStart has returned for this session (frame can
     *  present). The runtime gates RENDERING promotion on this so RENDERING never
     *  precedes a live compositor. */
    internal fun isCompositorUp(): Boolean = compositorUp.get()

    // ----------------------------------------------------------------------- //
    // bindSurface — compositor start + guest launch (the proven wiring)
    // ----------------------------------------------------------------------- //

    override fun bindSurface(holder: SurfaceHolder) {
        boundHolder = holder
        // surfaceCreated re-fire (rotation): rebind is handled by surfaceChanged; do NOT
        // start a second compositor/guest. The first bind owns the launch.
        if (!launched.compareAndSet(false, true)) {
            Log.i(TAG, "[$appId] surface re-bound; guest already launched")
            return
        }
        // All native work off the UI thread (it includes a blocking loader call that runs
        // for the whole guest lifetime).
        Thread({ runGuest(holder) }, "alr-session-$appId").start()
    }

    private fun runGuest(holder: SurfaceHolder) {
        try {
            val (rootfsDir, rootfsName) = runtime.awaitRootfs() ?: run {
                Log.e(TAG, "[$appId] rootfs not ready; aborting launch")
                runtime.onGuestExited(this, exitOk = false)
                return
            }
            stageOverlaysBestEffort(rootfsDir)

            // --- compositor output size from device metrics + the surface frame -------
            val dm = runtime.displayMetrics()
            val frame = holder.surfaceFrame
            val outW = if (frame.width() > 0) frame.width() else dm.widthPixels
            val outH = if (frame.height() > 0) frame.height() else dm.heightPixels
            // refreshRate is Activity-bound; the application Context can't read the active
            // mode, so default 60Hz (compositor clamps anyway). surfaceChanged carries the
            // live size on rotation.
            val refresh = 60000
            surfaceW.set(outW); surfaceH.set(outH); refreshMhz.set(refresh)

            val wlStart = AlrNative.nativeWaylandCompositorStart(
                runtime.cacheDirPath, holder.surface, dm.densityDpi, dm.xdpi, dm.ydpi,
                outW, outH, refresh,
            )
            compositorUp.set(true)
            Log.i(TAG, "[$appId] compositor: ${wlStart.lineSequence().firstOrNull()} content=${outW}x$outH")

            // Compositor is up → the session is the foreground RENDERING one (INV-1/2 via
            // the runtime). The first guest frame follows once the loader maps the binary.
            runtime.promoteToForeground(this)

            // --- base env (mirror runChromiumStandalone) + request env ----------------
            setEnv("ALR_REEXEC_INPROC", "1")
            setEnv("ALR_PERSIST_GUEST", "1")     // standalone: no SIGALRM lifetime cap
            setEnv("ALR_TEE_GUEST_STDOUT", "1")  // stream guest stderr/stdout to logcat
            for ((k, v) in request.env) setEnv(k, v)

            // --- GNOME-platform session-dbus shim (TASK-A2; gated, no-op otherwise) ----
            // GTK4/GNOME apps register a unique GtkApplication name on the SESSION bus and
            // read GSettings; the base ships only the libdbus CLIENT (no dbus-daemon). For a
            // gnome-platform appId we (a) point GSETTINGS_SCHEMA_DIR at the rootfs schemas
            // dir (the host-precompiled gschemas.compiled from the common-data `schemas`
            // overlay), and (b) WRAP the launch in `dbus-run-session --` (from the
            // dbus-daemon overlay) so a private session bus is started, DBUS_SESSION_BUS_
            // ADDRESS exported, and torn down on exit — all in the ONE blocking guest call,
            // no loader change. Non-GNOME apps and a missing overlay degrade to the plain
            // launch (GTK4 then falls back to a non-unique app, GSETTINGS_BACKEND=memory
            // already set natively). See GnomePlatformShim.
            for ((k, v) in GnomePlatformShim.envFor(appId, request.entryPath)) setEnv(k, v)
            val launch = GnomePlatformShim.wrap(appId, rootfsDir, request.entryPath, request.args)

            // --- program-spec: NEWLINE-delimited argv ---------------------------------
            val program = buildString {
                append(launch.first)
                for (a in launch.second) { append('\n'); append(a) }
            }
            Log.i(TAG, "[$appId] launching guest: ${program.replace('\n', ' ')}")

            // BLOCKS for the whole guest lifetime.
            val out = AlrNative.nativeAlrNativeLoaderProbe(
                runtime.packageName,
                runtime.nativeLibraryDir,
                runtime.filesDirPath,
                runtime.cacheDirPath,
                rootfsName,
                program,
            )
            Log.i(TAG, "[$appId] guest exited:\n$out")
            // Exit classification from the loader report's authoritative status line
            // "alr native loader child exit=<code> signal=<sig>": a non-zero terminating
            // SIGNAL (segfault/abort/bus) → CRASHED; a clean signal=0 exit → STOPPED. We
            // do NOT key on exit code alone (a GUI app may exit non-zero on user-close yet
            // not crash), nor on GUEST EXEC PASS/FAIL (FAIL also fires for a clean exit
            // with empty stdout). Absent the line (early loader failure) → CRASHED.
            runtime.onGuestExited(this, exitOk = exitedCleanly(out))
        } catch (e: Throwable) {
            Log.e(TAG, "[$appId] launch EXC: ${Log.getStackTraceString(e)}")
            runtime.onGuestExited(this, exitOk = false)
        }
    }

    /**
     * Stage the overlays a GUI guest may need, best-effort: only extract a `<name>-stage.tar`
     * that has been pushed to /data/local/tmp (the integration session pushes these). The
     * BASE rootfs already ships the GTK/X/font stack + GIMP, so a bundled app needs NO
     * overlay — this just makes overlay-dependent apps work when their tar is present.
     * Mirrors the staging loop in runChromiumStandalone (marker-gated, idempotent).
     */
    private fun stageOverlaysBestEffort(rootfsDir: File) {
        for (name in OVERLAY_NAMES) {
            try {
                val tar = File("/data/local/tmp/$name-stage.tar")
                val marker = File(rootfsDir, ".$name-staged-${tar.length()}")
                if (tar.isFile && !marker.isFile) {
                    Log.i(TAG, "[$appId] staging $name-stage (${tar.length()} bytes)")
                    val ovr = runtime.extractOverlay(tar, rootfsDir)
                    marker.writeText("staged\n")
                    Log.i(TAG, "[$appId] $name-stage done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                }
            } catch (e: Throwable) {
                Log.e(TAG, "[$appId] $name-stage EXC: ${Log.getStackTraceString(e)}")
            }
        }
    }

    // ----------------------------------------------------------------------- //
    // surfaceChanged / unbind / foreground / background / stop
    // ----------------------------------------------------------------------- //

    /** RunningSurfaceActivity.surfaceChanged → reconfigure the live compositor output. */
    fun onSurfaceChanged(holder: SurfaceHolder, width: Int, height: Int) {
        if (!compositorUp.get()) return
        if (width <= 0 || height <= 0) return
        if (width == surfaceW.get() && height == surfaceH.get()) return
        surfaceW.set(width); surfaceH.set(height)
        boundHolder = holder
        val dm = runtime.displayMetrics()
        val r = AlrNative.nativeWaylandCompositorResize(
            holder.surface, width, height, dm.densityDpi, dm.xdpi, dm.ydpi, refreshMhz.get(),
        )
        Log.i(TAG, "[$appId] resize ${width}x$height -> $r")
    }

    override fun unbindSurface() {
        boundHolder = null
        // Present stops when the Android surface is destroyed; the compositor keeps the
        // guest alive (BACKGROUND). No native call needed here for Phase 1.
    }

    override fun requestForeground() {
        // Re-bind the (process-global) compositor to this session's surface, then promote.
        boundHolder?.let { h ->
            if (compositorUp.get()) onSurfaceChanged(h, surfaceW.get(), surfaceH.get())
        }
        runtime.promoteToForeground(this)
    }

    override fun requestBackground() = runtime.onRequestBackground(this)

    override fun stop(reason: StopReason) = runtime.onStop(this)

    /** Stop the process-global compositor (called by the runtime on stop). */
    internal fun teardownCompositor() {
        if (!compositorUp.compareAndSet(true, false)) return
        try {
            val r = AlrNative.nativeWaylandCompositorStop()
            Log.i(TAG, "[$appId] compositor stop: ${r.lineSequence().firstOrNull()}")
        } catch (e: Throwable) {
            Log.e(TAG, "[$appId] compositor stop EXC: ${e.message}")
        }
    }

    // ----------------------------------------------------------------------- //
    // Input routing (RunningSurfaceActivity forwards SurfaceView touch here)
    // ----------------------------------------------------------------------- //

    /** Forward one Android MotionEvent's contacts to the focused guest as wl_touch. */
    fun injectTouch(ev: MotionEvent) {
        if (!compositorUp.get()) return
        when (ev.actionMasked) {
            MotionEvent.ACTION_CANCEL -> { AlrNative.nativeWaylandInjectTouchCancel(); return }
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = ev.actionIndex
                AlrNative.nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 0)
                AlrNative.nativeWaylandInjectTouchFrame()
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until ev.pointerCount) {
                    AlrNative.nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 1)
                }
                AlrNative.nativeWaylandInjectTouchFrame()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val i = ev.actionIndex
                AlrNative.nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 2)
                AlrNative.nativeWaylandInjectTouchFrame()
            }
        }
    }

    // ----------------------------------------------------------------------- //
    // Internals
    // ----------------------------------------------------------------------- //

    private fun setEnv(key: String, value: String) {
        try {
            android.system.Os.setenv(key, value, true)
        } catch (e: Throwable) {
            Log.w(TAG, "[$appId] setenv $key failed: ${e.message}")
        }
    }

    private companion object {
        // Overlay tars a GUI guest may use (superset; only present ones are staged).
        // Mirrors runChromiumStandalone's staging set, minus chromium-only overlays.
        // vk-icd ships the guest Mali ICD (libalr_mali_icd.so, RENAMED from libvulkan.so.1)
        // the loader binds when a launch opts into ALR_VK_ICD=1; vk-loader ships the real
        // Khronos Vulkan-Loader (libvulkan.so.1) + alr_icd.json for the GPU Part B ICD
        // discovery redirect (ANGLE dlopen("libvulkan.so.1") → loader → our ICD). Both are
        // distinct files in /usr/lib/androlinux (no collision); staged best-effort like the
        // rest (no-op when their tars are absent), so a normal GUI launch is unaffected.
        //
        // NOTE: the ANGLE overlay (angle-stage.tar, a SYSTEM ANGLE libEGL.so.1/
        // libGLESv2.so.2 → /usr/lib/androlinux for the GL→Vulkan→our-ICD breadth path)
        // is deliberately NOT in this always-on set. It and gpushim ship the SAME
        // androlinux SONAMEs (private-dir, so the frozen guard does not arbitrate), so
        // ANGLE is an explicit OPT-IN gated on /data/local/tmp/.alr-angle in
        // MainActivity — never auto-staged here where it could shadow gpushim.
        // gnome-schemas ships the host-precompiled gschemas.compiled (+ new .gschema.xml)
        // and dbus-daemon ships /usr/bin/dbus-daemon + dbus-run-session — the two overlays
        // the gnome-platform launch shim (GnomePlatformShim) needs. Both staged best-effort
        // (no-op when their tars are absent), so a non-GNOME launch is unaffected.
        private val OVERLAY_NAMES =
            listOf("interpose", "nss", "xkb-gegl", "babl-gegl", "x11", "pulse", "vk-icd",
                   "vk-loader", "gnome-schemas", "dbus-daemon")

        // The loader report's status line, e.g. "alr native loader child exit=0 signal=0".
        private val SIGNAL_RE = Regex("""child exit=(-?\d+) signal=(\d+)""")

        /**
         * True iff the guest terminated NORMALLY (signal == 0). A non-zero terminating
         * signal (SIGSEGV/SIGABRT/SIGBUS/…) means a crash. If the status line is missing
         * (the loader failed before launching), treat it as NOT clean → CRASHED.
         */
        fun exitedCleanly(report: String): Boolean {
            val m = SIGNAL_RE.find(report) ?: return false
            return m.groupValues[2].toIntOrNull() == 0
        }
    }
}

// --------------------------------------------------------------------------- //
// GnomePlatformShim — session-dbus + GSettings launch shaping for GNOME apps
// --------------------------------------------------------------------------- //

/**
 * The runtime half of the GNOME-platform unlock (TASK-A2). GTK4 / GNOME apps differ from
 * the galculator-class GTK3 apps in two startup requirements the base does not satisfy:
 *
 *   1. **GSettings schemas** — they read keys from many `org.gnome.*` schemas. The base
 *      ships only ~4 GTK schemas compiled and has NO `glib-compile-schemas`. The
 *      common-data `schemas` overlay (tools/build_common_data_overlay.py `--schemas`) ships
 *      a host-precompiled `gschemas.compiled` that is a SUPERSET (base GTK ∪ gnome-desktop
 *      ∪ the app's own). We point `GSETTINGS_SCHEMA_DIR` at that rootfs dir so GLib finds it
 *      even if XDG_DATA_DIRS path-mediation is flaky. (The native runtime already sets
 *      `GSETTINGS_BACKEND=memory`, so settings WRITES go to memory — no dconf/system bus
 *      needed; only the compiled schema DEFAULTS must be present, which this provides.)
 *
 *   2. **Session D-Bus** — `GtkApplication` registers a unique bus name on the SESSION bus;
 *      the base ships only the libdbus client (no `dbus-daemon`). When the dbus-daemon
 *      overlay is staged we WRAP the launch in `dbus-run-session -- <app>`: it starts a
 *      private session bus, exports `DBUS_SESSION_BUS_ADDRESS`, runs the app, and tears the
 *      bus down on exit — entirely inside the ONE blocking guest call the loader already
 *      runs (no loader / sandbox change). If `dbus-run-session` is absent (overlay not
 *      staged) we launch the app directly; GTK4 then falls back to a non-unique application
 *      (a warning, not a crash).
 *
 * Strictly GATED on the appId being a known GNOME-platform app (or its binary living under
 * `/usr/bin/gnome-*` / `/usr/bin/org.gnome.*`), so every non-GNOME launch is byte-identical
 * to before. PURE/deterministic given the rootfs dir + appId (the only IO is a `File.exists`
 * probe of the staged `dbus-run-session`), which makes it host-unit-testable.
 */
internal object GnomePlatformShim {

    /** Rootfs path of the session-bus wrapper shipped by the dbus-daemon overlay. */
    private const val DBUS_RUN_SESSION = "usr/bin/dbus-run-session"
    /** Rootfs dir holding the host-precompiled gschemas.compiled (schemas overlay). */
    private const val SCHEMAS_DIR = "/usr/share/glib-2.0/schemas"

    /**
     * Known GNOME-platform appIds (= their `.desktop` basename, the catalog launch key).
     * These are the gnome-platform-ONLY apps the feasibility doc marks reachable with this
     * shim (no perl / sandbox / gstreamer): gnome-calculator and its close siblings.
     */
    private val GNOME_APP_IDS = setOf(
        "org.gnome.Calculator",
        "org.gnome.TextEditor",
        "org.gnome.gedit",      // gedit's app-id form (older)
        "gedit",
        "org.gnome.eog",
        "org.gnome.Eog",
        "eog",
        "org.gnome.FileRoller",
        "file-roller",
    )

    /** True iff [appId] (or [entryPath]) identifies a GNOME-platform app this shim targets. */
    fun isGnomePlatform(appId: String, entryPath: String = ""): Boolean {
        if (appId in GNOME_APP_IDS) return true
        // Fallback: gnome-* / org.gnome.* binaries under /usr/bin (covers discovered apps
        // launched by .desktop Exec whose appId we didn't enumerate).
        val bin = entryPath.substringAfterLast('/')
        return bin.startsWith("gnome-") || appId.startsWith("org.gnome.")
    }

    /** Extra guest env for a GNOME app (empty for non-GNOME). */
    fun envFor(appId: String, entryPath: String = ""): Map<String, String> =
        if (isGnomePlatform(appId, entryPath))
            mapOf("GSETTINGS_SCHEMA_DIR" to SCHEMAS_DIR)
        else
            emptyMap()

    /**
     * Shape the launch for [appId]. Returns (entryPath, args) — UNCHANGED for a non-GNOME
     * app, or `dbus-run-session -- <entryPath> <args…>` for a GNOME app WHEN the dbus-daemon
     * overlay is staged (so a session bus hosts GtkApplication). If the wrapper is absent the
     * original launch is returned (graceful degradation — the app still starts).
     */
    fun wrap(
        appId: String,
        rootfsDir: File,
        entryPath: String,
        args: List<String>,
    ): Pair<String, List<String>> {
        if (!isGnomePlatform(appId, entryPath)) return entryPath to args
        val wrapper = File(rootfsDir, DBUS_RUN_SESSION)
        if (!wrapper.isFile) {
            Log.i(TAG, "[$appId] gnome-shim: $DBUS_RUN_SESSION not staged — launching " +
                "without a session bus (GtkApplication falls back to non-unique)")
            return entryPath to args
        }
        Log.i(TAG, "[$appId] gnome-shim: wrapping launch in dbus-run-session (session bus + " +
            "GSETTINGS_SCHEMA_DIR=$SCHEMAS_DIR)")
        // dbus-run-session [--] <program> <args…>. The `--` guards against the app's own
        // args being parsed as dbus-run-session options.
        val wrapped = buildList {
            add("--")
            add(entryPath)
            addAll(args)
        }
        return "/$DBUS_RUN_SESSION" to wrapped
    }
}
