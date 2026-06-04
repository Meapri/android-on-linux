/*
 * TouchDpiEnv — derive the guest-toolkit DPI/scale ENVIRONMENT from the Android display
 * density so a Linux GUI app's default buttons/menus/fonts are FINGER-sized on the device
 * (product UX BUG-1).
 *
 * WHY. ALR launches stock Linux GUI toolkits (GTK3/4, Qt6, X11-via-Xwayland) with no scale
 * env, so they render at their desktop default (~96 dpi, 1× scale). On a touch device that is
 * finger-UNFRIENDLY: a 1× GTK button is far smaller than Android's 48dp minimum touch target.
 * Android's `DisplayMetrics.densityDpi` is ALREADY touch-calibrated (48dp == a finger at the
 * device density), so the fix is to MATCH the guest toolkits to that density. Target effective
 * UI scale ≈ densityDpi / 160 (160 = mdpi baseline): 213dpi → 1.33×, 320dpi → 2.0×.
 *
 * THE BUFFER-SCALE INTERACTION (do NOT double-scale). The ALR Wayland compositor ALREADY
 * advertises an INTEGER `wl_output` buffer scale of `round(densityDpi/160)` (clamped so the
 * logical width stays >= 1024 px) — see runtime_report.cpp `alr_jni_wayland_compositor_start`
 * and alr_compositor.cpp `apply_output_resize`. A Wayland-native GTK/Qt client reads that and
 * already renders HiDPI buffers for the INTEGER part. So the env here must contribute only the
 * FRACTIONAL REMAINDER on Wayland; setting `GDK_SCALE` (an integer device-pixel multiplier)
 * would stack ON TOP of the advertised buffer scale and DOUBLE-scale. Hence:
 *   - GTK (Wayland): set GDK_DPI_SCALE = target / bufferScale (the sub-integer font/UI bump),
 *     and DO NOT set GDK_SCALE (the compositor's buffer scale already gives the integer factor).
 *   - Qt6: QT_FONT_DPI = densityDpi (logical font DPI; orthogonal to device-pixel-ratio so it
 *     does not stack with the buffer scale) + QT_AUTO_SCREEN_SCALE_FACTOR=1 (Qt honors the
 *     output scale for the integer part).
 *   - X11 (via Xwayland): Xft.dpi = densityDpi. X11 has no per-output scale and Xwayland
 *     presents at 1× to the compositor (the whole X surface is then buffer-scaled), so X clients
 *     need the FULL density as their font DPI — set as an env the toolkits read, and seeded into
 *     the X resource DB by XwaylandLaunch.
 *
 * On THIS device (Samsung SM-X236N, 1200×1920, densityDpi=213) bufferScale = (213+80)/160 = 1,
 * so the residual == the full 1.33× and every path converges; the helper stays correct for any
 * density (e.g. 320dpi → bufferScale 2, GDK_DPI_SCALE 1.0, QT_FONT_DPI/Xft.dpi 320).
 *
 * PURE + DEVICE-ADAPTIVE. `envFor(densityDpi)` is a pure Int→Map function (no Android types),
 * so it is host-unit-testable and is the SINGLE source of the derivation — every launch path
 * (NativeAppSession, MainActivity chromium/probe) calls it instead of hardcoding scale values.
 */
package dev.chanwoo.androlinux.runtime

/** Touch-calibrated guest-toolkit DPI/scale env, derived from Android's display density. */
object TouchDpiEnv {

    /** mdpi baseline: 160 dpi == scale 1.0 (Android's density reference). */
    private const val BASELINE_DPI = 160

    /** Guard against an absurd/zero density report (fall back to no scaling). */
    private const val MIN_SANE_DPI = 100

    /**
     * The INTEGER wl_output buffer scale the compositor advertises for [densityDpi]:
     * `round(dpi/160)`, min 1. MIRRORS runtime_report.cpp / alr_compositor.cpp
     * (`(density_dpi + 80) / 160`) so the residual GDK_DPI_SCALE below never double-scales.
     *
     * NOTE: the compositor additionally clamps this DOWN if the logical width would fall below
     * 1024 px; the helper cannot see the panel width at env-build time, so it uses the
     * UN-clamped round. The only effect of a clamp the helper didn't see is a slightly smaller
     * GDK_DPI_SCALE (mild under-scale), never a crash or a double-scale. At the device density
     * (213 → 1) there is no clamp.
     */
    fun bufferScale(densityDpi: Int): Int {
        if (densityDpi < MIN_SANE_DPI) return 1
        val s = (densityDpi + BASELINE_DPI / 2) / BASELINE_DPI
        return if (s < 1) 1 else s
    }

    /** Target effective UI scale for [densityDpi] (densityDpi/160), floored at 1.0. */
    fun targetScale(densityDpi: Int): Double {
        if (densityDpi < MIN_SANE_DPI) return 1.0
        val t = densityDpi.toDouble() / BASELINE_DPI
        return if (t < 1.0) 1.0 else t
    }

    /**
     * The guest launch env that makes GTK3/4, Qt6 and X11(via Xwayland) toolkits render
     * finger-sized at [densityDpi]. Empty (no scaling) for a non-sensible/low density so a
     * desktop-DPI panel is unaffected. Deterministic — see the class doc for the per-toolkit
     * rationale and the explicit no-GDK_SCALE (no-double-scale) decision.
     */
    fun envFor(densityDpi: Int): Map<String, String> {
        if (densityDpi < MIN_SANE_DPI) return emptyMap()
        val target = targetScale(densityDpi)
        val buffer = bufferScale(densityDpi)
        // Fractional residual GTK applies ON TOP of the compositor's integer buffer scale.
        val gdkDpiScale = target / buffer
        return buildMap {
            // GTK3/4 on Wayland: fractional font/UI bump only (integer factor = buffer scale).
            put("GDK_DPI_SCALE", fmt(gdkDpiScale))
            // Qt6: logical font DPI + let Qt read the output scale for the integer part.
            put("QT_FONT_DPI", densityDpi.toString())
            put("QT_AUTO_SCREEN_SCALE_FACTOR", "1")
            // X11 toolkits (Xwayland): font DPI = full device density (no per-output scale in X).
            put("Xft.dpi", densityDpi.toString())
        }
    }

    /** Format a scale with at most 2 decimals, trimming trailing zeros (1.0 -> "1", 1.33). */
    private fun fmt(v: Double): String {
        val rounded = Math.round(v * 100.0) / 100.0
        if (rounded == Math.floor(rounded)) return rounded.toLong().toString()
        var s = String.format(java.util.Locale.ROOT, "%.2f", rounded)
        s = s.trimEnd('0').trimEnd('.')
        return s
    }
}
