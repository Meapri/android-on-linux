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
            // Touch-calibrated guest DPI (product UX BUG-1): match the GTK/Qt/X11 toolkits to
            // Android's already-finger-sized densityDpi so the Linux app's default buttons/
            // menus/fonts are tappable. Derived (not hardcoded) and double-scale-safe w.r.t.
            // the compositor's wl_output buffer scale — see TouchDpiEnv. Set BEFORE request.env
            // so a per-app launch can still override any single knob.
            for ((k, v) in TouchDpiEnv.envFor(dm.densityDpi)) setEnv(k, v)
            Log.i(TAG, "[$appId] touch-dpi: density=${dm.densityDpi} env=${TouchDpiEnv.envFor(dm.densityDpi)}")
            for ((k, v) in request.env) setEnv(k, v)

            // === X11-only routing via ROOTFUL Xwayland (TASK-A) ======================= //
            // KEPT IN A SEPARATE REGION from the env-flag block above (so a concurrent
            // ALR_REEXEC_INPROC edit there merges cleanly): the whole X11 launch path is
            // delegated to XwaylandLaunch (a self-contained helper object at the bottom of
            // this file, sibling to GnomePlatformShim). For an X11-only app (one that links
            // libX11 but NOT libwayland-client — e.g. xzgv/xli, marked needsXwayland in the
            // catalog, or launched with protocol=X11) it preps the X11 sockets, starts a
            // ROOTFUL Xwayland :0 as a persistent wl client (Xwayland → wl_shm → SurfaceView,
            // the xcalc device-proven path), waits for the X0 socket, and yields DISPLAY=:0
            // for the guest env. For a Wayland-capable app it is a complete no-op (byte-
            // identical to before). Gated entirely inside the helper.
            if (XwaylandLaunch.needsX11(appId, request.protocol, request.entryPath)) {
                // Pass densityDpi so the helper seeds the X resource DB with the touch DPI
                // (Xft.dpi) for pure-Xlib clients that read xrdb rather than the env (BUG-1).
                val xReady = XwaylandLaunch.ensureUp(
                    runtime, rootfsDir, rootfsName, outW, outH, dm.densityDpi,
                )
                for ((k, v) in XwaylandLaunch.envFor()) setEnv(k, v)
                Log.i(TAG, "[$appId] X11 routing: Xwayland :0 ready=$xReady (DISPLAY=:0)")
            }

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
        // xwayland ships /usr/bin/Xwayland (the rootful X server XwaylandLaunch starts for
        // X11-only catalog apps, TASK-A); staged best-effort like the rest (no-op when its
        // tar is absent), so a Wayland-only launch is unaffected. qt6-gui ships the Qt6
        // Quick + qtwayland(generic wl_shm) GUI stack (tools/build_toolkit_overlays.py
        // `qt6-gui` recipe → qmleasing) — the lightest REACHABLE Qt-on-Wayland app-class
        // (the apt Qt-GUI path is closure-blocked by libqt6gui6→libice6→x11-common; see
        // docs/research). Both are presence-guarded best-effort stages.
        private val OVERLAY_NAMES =
            listOf("interpose", "nss", "xkb-gegl", "babl-gegl", "x11", "pulse", "vk-icd",
                   "vk-loader", "gnome-schemas", "dbus-daemon", "xwayland", "qt6-gui")

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

// --------------------------------------------------------------------------- //
// XwaylandLaunch — ROOTFUL Xwayland routing for X11-only apps (TASK-A)
// --------------------------------------------------------------------------- //

/**
 * The launch-routing half of the X11-app unlock (TASK-A). ALR's compositor speaks
 * Wayland only; an X11-ONLY guest app (one whose EXEC binary links libX11/libxcb but
 * NOT libwayland-client — e.g. xzgv, xli) therefore has no Wayland display to bind and,
 * sent down the normal path with no DISPLAY, dies "cannot open display". The product fix
 * is to AUTO-ROUTE such apps through a ROOTFUL Xwayland the SAME way the device-proven
 * xcalc demo did (Xwayland :0 → wl_shm → SurfaceView), but driven by a catalog flag
 * instead of a `/data/local/tmp` marker.
 *
 * This object is the runtime counterpart to that flag. [needsX11] decides whether an app
 * must be routed (by the catalog's `needsXwayland` set — kept in lock-step with
 * NativeAlrRuntime.BundledCatalog — OR an explicit `protocol == X11` request). When it
 * does, [ensureUp] performs EXACTLY the proven MainActivity Xwayland sequence:
 *
 *   1. **X11 socket prep** — create `<rootfs>/tmp` and `<rootfs>/tmp/.X11-unix` at the
 *      sticky `01777` mode the X server's `MkdirIfNeeded` ownership check expects, OWNED
 *      by the app uid (== the guest euid). The base ships neither dir at a usable mode,
 *      and Xwayland refuses `-nolock` for non-root, so without this the server aborts at
 *      `/tmp/.X0-lock` create (EPERM) before binding its socket. `Os.chmod` (not File.set*)
 *      because Java cannot set the sticky bit. Idempotent + best-effort (failure only logs).
 *   2. **Start ROOTFUL Xwayland :0** on its OWN thread — it is a *persistent* wl client
 *      (it does not exit), so it must not block the guest launch. argv (verified vs
 *      Xwayland(1)): `:0 -shm -geometry WxH`. `-shm` pins the shared-memory backend (the
 *      compositor is wl_shm-only) so glamor/DRI3/EGL stay inert (software X), `-geometry`
 *      sizes the rootful screen to the panel. No `-rootless` ⇒ rootful ⇒ no X window
 *      manager needed. `ALR_REEXEC_INPROC=1` is required so Xwayland's fork+exec(xkbcomp)
 *      keymap compile re-enters the in-process loader (device-proven: without it the
 *      keymap compile fails); the session already sets that flag for every launch.
 *   3. **Wait (bounded) for the X0 socket** — `<rootfs>/tmp/.X11-unix/X0`. A bound AF_UNIX
 *      socket is a SPECIAL file, so readiness is probed with `exists()` (NOT `isFile()`,
 *      which is false for a socket and would spin the full timeout). The interposer's X11
 *      `sun_path` transform rewrites `/tmp/.X11-unix/X0` → `<rootfs>/tmp/.X11-unix/X0` in
 *      both the server bind and the client connect, so they meet on the same node.
 *
 * [envFor] then yields `DISPLAY=:0` for the guest env. The X app, launched right after on
 * the session's normal blocking loader call, connects to Xwayland and renders.
 *
 * GATED entirely on [needsX11]: a Wayland-capable app never enters any of this (no socket
 * prep, no Xwayland process, no DISPLAY), so its launch is byte-identical to before. The
 * Xwayland process is process-global and started at most once across sessions (a single
 * compositor backs all of them), guarded by [started]. PURE except the documented IO
 * (mkdir/chmod + the persistent loader-probe thread), so [needsX11]/[envFor] are
 * host-unit-testable and the whole shape is source-assertable.
 */
internal object XwaylandLaunch {

    /** Rootfs bin path of the X server shipped by the xwayland overlay (xwayland-stage.tar). */
    private const val XWAYLAND_BIN = "usr/bin/Xwayland"
    /** The X display the rootful server owns; injected into the guest env as DISPLAY. */
    private const val DISPLAY_VALUE = ":0"
    /** Sticky world-writable mode (01777) the X server requires on /tmp + /tmp/.X11-unix. */
    private const val STICKY_1777 = 0x3FF
    /** Bounded waits (ms): overlay extraction, then the X0 socket appearing. */
    private const val XWAYLAND_STAGE_WAIT_MS = 60_000
    private const val X0_SOCKET_WAIT_MS = 20_000

    /**
     * appIds that are X11-only and must be routed through Xwayland. Kept in lock-step with
     * NativeAlrRuntime.BundledCatalog's `needsXwayland = true` entries. Detection is by the
     * catalog launch key (appId), mirroring GnomePlatformShim.GNOME_APP_IDS. Membership was
     * established host-side from each app's EXEC-binary DT_NEEDED (libX11 present,
     * libwayland-client absent) — see CatalogApp.needsXwayland.
     */
    private val X11_ONLY_APP_IDS = setOf(
        "xzgv",   // GTK2-x11 thumbnail/image viewer: libgtk-x11-2.0 + libX11, no wayland
        "xli",    // classic Xlib image viewer: libX11 only, no wayland
    )

    /** Process-global guard: the rootful Xwayland is started at most once (one compositor). */
    private val started = java.util.concurrent.atomic.AtomicBoolean(false)

    /**
     * True iff this launch must be routed through Xwayland: the appId is a known X11-only
     * catalog app, OR the request explicitly asked for the X11 protocol. [entryPath] is
     * accepted for symmetry with GnomePlatformShim (and future binary-name heuristics) but
     * the authoritative signal is the catalog flag / explicit protocol — never a guess.
     */
    fun needsX11(appId: String, protocol: SurfaceProtocol, entryPath: String = ""): Boolean =
        protocol == SurfaceProtocol.X11 || appId in X11_ONLY_APP_IDS

    /** Guest env for an X11-routed app: point DISPLAY at the rootful Xwayland. */
    fun envFor(): Map<String, String> = mapOf("DISPLAY" to DISPLAY_VALUE)

    /**
     * Ensure a ROOTFUL Xwayland :0 is up for X11 clients, replicating the proven sequence.
     * Returns true if the X0 socket is present (server reachable) within the bounded wait.
     * Best-effort + idempotent: safe to call on every X11 launch; the server is started at
     * most once. Must be called BEFORE the guest's blocking loader call (Xwayland runs on
     * its own thread; this call returns once the socket is ready or the wait elapses).
     */
    fun ensureUp(
        runtime: NativeAlrRuntime,
        rootfsDir: File,
        rootfsName: String,
        outW: Int,
        outH: Int,
        densityDpi: Int = 0,
    ): Boolean {
        prepX11Sockets(rootfsDir)
        seedXftDpi(rootfsDir, densityDpi)

        val xSock = File(rootfsDir, "tmp/.X11-unix/X0")
        // Already up (a prior session started it) → just confirm the socket.
        if (xSock.exists()) return true

        val xwBin = File(rootfsDir, XWAYLAND_BIN)
        var waited = 0
        while (waited < XWAYLAND_STAGE_WAIT_MS && !xwBin.isFile) {
            try { Thread.sleep(1000) } catch (_: InterruptedException) { return false }
            waited += 1000
        }
        if (!xwBin.isFile) {
            Log.w(TAG, "xwayland: /usr/bin/Xwayland not staged (push xwayland-stage.tar) — " +
                "X11 app will have no display")
            return false
        }

        if (started.compareAndSet(false, true)) {
            // Persistent wl client: own thread, never joins (Xwayland does not exit).
            Thread({
                try {
                    val xwServer = AlrNative.nativeAlrNativeLoaderProbe(
                        runtime.packageName,
                        runtime.nativeLibraryDir,
                        runtime.filesDirPath,
                        runtime.cacheDirPath,
                        rootfsName,
                        "/$XWAYLAND_BIN\n$DISPLAY_VALUE\n-shm\n-geometry\n${outW}x$outH",
                    )
                    Log.i(TAG, "xwayland-server exited:\n$xwServer")
                } catch (e: Throwable) {
                    Log.e(TAG, "xwayland-server EXC: ${Log.getStackTraceString(e)}")
                }
            }, "alr-xwayland-server").start()
        }

        var sockWaited = 0
        while (sockWaited < X0_SOCKET_WAIT_MS && !xSock.exists()) {
            try { Thread.sleep(500) } catch (_: InterruptedException) { break }
            sockWaited += 500
        }
        val ready = xSock.exists()
        Log.i(TAG, "xwayland: X0 socket=$ready (waited ${sockWaited}ms, geometry=${outW}x$outH)")
        return ready
    }

    /**
     * Pre-create `<rootfs>/tmp` + `<rootfs>/tmp/.X11-unix` at sticky 01777 (the X server's
     * required mode), via direct UNMEDIATED host-rootfs access from the app side. Idempotent
     * + best-effort: a failure only logs (the launch still attempts). This is the ONLY
     * Xwayland-launch-specific filesystem prep; it does not touch the shared loader env.
     */
    private fun prepX11Sockets(rootfsDir: File) {
        try {
            val xTmp = File(rootfsDir, "tmp")
            val xUnix = File(xTmp, ".X11-unix")
            xTmp.mkdirs()
            xUnix.mkdirs()
            android.system.Os.chmod(xTmp.absolutePath, STICKY_1777)
            android.system.Os.chmod(xUnix.absolutePath, STICKY_1777)
            Log.i(TAG, "xwayland: prepped /tmp(1777)=${xTmp.isDirectory} " +
                "/tmp/.X11-unix(1777)=${xUnix.isDirectory}")
        } catch (e: Throwable) {
            Log.w(TAG, "xwayland: /tmp prep EXC: ${e.message}")
        }
    }

    /**
     * Seed the touch DPI into the X resource DB (BUG-1): write `Xft.dpi: <densityDpi>` to the
     * rootfs `/root/.Xresources` so a pure-Xlib client that consults xrdb (rather than the
     * `Xft.dpi` env TouchDpiEnv also sets) still renders fonts at the device density. X11 has
     * no per-output scale and Xwayland presents at 1× to the (buffer-scaled) compositor, so the
     * X client needs the FULL density as its font DPI. Best-effort + idempotent; a missing
     * density (0) or any IO failure just logs (the launch still proceeds with the env value).
     */
    private fun seedXftDpi(rootfsDir: File, densityDpi: Int) {
        if (densityDpi <= 0) return
        try {
            val xres = File(rootfsDir, "root/.Xresources")
            xres.parentFile?.mkdirs()
            xres.writeText("Xft.dpi: $densityDpi\n")
            Log.i(TAG, "xwayland: seeded Xft.dpi=$densityDpi -> ${xres.absolutePath}")
        } catch (e: Throwable) {
            Log.w(TAG, "xwayland: Xft.dpi seed EXC: ${e.message}")
        }
    }
}
