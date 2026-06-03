/*
 * AlrNative — class-neutral JNI facade for the *product launcher* runtime path.
 *
 * The proven real-run wiring (MainActivity.runChromiumStandalone) drives the in-app
 * Wayland compositor + native loader through MainActivity-mangled JNI exports
 * (Java_dev_chanwoo_androlinux_MainActivity_native…). NativeAlrRuntime/NativeAppSession
 * live in the `runtime` package and CANNOT call those (wrong JNI class name). So
 * runtime_report.cpp ADDS neutral exports mangled to THIS object
 * (Java_dev_chanwoo_androlinux_runtime_AlrNative_native…) that forward to the SAME
 * shared C++ helpers the MainActivity exports use — identical native behavior, just a
 * different entry symbol. MainActivity's exports + runChromiumStandalone are untouched.
 *
 * Loads the SAME native library MainActivity loads ("alr_loader"). System.loadLibrary
 * is idempotent per-process, so loading here (launcher process) and in MainActivity
 * (probe harness) is safe — whichever Activity starts first triggers the one load.
 *
 * Surface size args mirror runChromiumStandalone exactly:
 *   compositorStart(cacheDir, surface, densityDpi, xdpi, ydpi, outW, outH, refreshMhz)
 *   compositorResize(surface, w, h, densityDpi, xdpi, ydpi, refreshMhz)
 *   nativeLoaderProbe(pkg, nativeLibDir, filesDir, cacheDir, rootfsName, program)
 *     program = NEWLINE-delimited argv (entryPath + "\n" + args.joinToString("\n")).
 *
 * Clipboard caveat: compositorStart forwards `this` (an AlrNative object) to the native
 * clip-sink installer, whose class lacks onGuestClipboard* up-call methods — so the
 * guest→Android clipboard bridge gracefully no-ops on the launcher path (a Phase-1 gap;
 * the bridge is a MainActivity feature). The compositor itself behaves identically.
 */
package dev.chanwoo.androlinux.runtime

import android.view.Surface

object AlrNative {
    init {
        // Same library MainActivity loads ("alr_loader"); loadLibrary is per-process
        // idempotent so this never double-loads.
        System.loadLibrary("alr_loader")
    }

    /** Start the in-app Wayland compositor presenting onto [surface]. Mirrors
     *  MainActivity.nativeWaylandCompositorStart. Returns a native status string. */
    external fun nativeWaylandCompositorStart(
        cacheDir: String,
        surface: Surface,
        densityDpi: Int,
        xdpi: Float,
        ydpi: Float,
        outWidthPx: Int,
        outHeightPx: Int,
        refreshMilliHz: Int,
    ): String

    /** Re-bind the compositor to a new surface + content size (rotation / resize). */
    external fun nativeWaylandCompositorResize(
        surface: Surface,
        widthPx: Int,
        heightPx: Int,
        densityDpi: Int,
        xdpi: Float,
        ydpi: Float,
        refreshMilliHz: Int,
    ): String

    /** Stop the compositor + clear native clipboard sinks (teardown). */
    external fun nativeWaylandCompositorStop(): String

    /** Exec a guest binary via the native loader (blocks until the guest exits).
     *  [program] is NEWLINE-delimited argv: first line = binary path, rest = args. */
    external fun nativeAlrNativeLoaderProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    // --- Input injection (compositor-global; routes to the focused guest client) ---
    external fun nativeWaylandInjectTouch(id: Int, x: Float, y: Float, phase: Int)
    external fun nativeWaylandInjectTouchFrame()
    external fun nativeWaylandInjectTouchCancel()
    external fun nativeWaylandInjectKey(evdevKey: Int, pressed: Int)
    external fun nativeWaylandInjectScroll(x: Float, y: Float, value: Double, axis: Int)
}
