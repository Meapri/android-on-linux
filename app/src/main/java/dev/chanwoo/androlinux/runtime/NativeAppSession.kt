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

            // --- program-spec: NEWLINE-delimited argv ---------------------------------
            val program = buildString {
                append(request.entryPath)
                for (a in request.args) { append('\n'); append(a) }
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
        // vk-icd ships the guest Vulkan ICD (libvulkan.so.1) the loader binds when a
        // launch opts into ALR_VK_ICD=1; staged best-effort like the rest (no-op when
        // its tar is absent), so a normal GUI launch is unaffected.
        //
        // NOTE: the ANGLE overlay (angle-stage.tar, a SYSTEM ANGLE libEGL.so.1/
        // libGLESv2.so.2 → /usr/lib/androlinux for the GL→Vulkan→our-ICD breadth path)
        // is deliberately NOT in this always-on set. It and gpushim ship the SAME
        // androlinux SONAMEs (private-dir, so the frozen guard does not arbitrate), so
        // ANGLE is an explicit OPT-IN gated on /data/local/tmp/.alr-angle in
        // MainActivity — never auto-staged here where it could shadow gpushim.
        private val OVERLAY_NAMES =
            listOf("interpose", "nss", "xkb-gegl", "babl-gegl", "x11", "pulse", "vk-icd")

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
