package dev.chanwoo.androlinux.runtime

import android.util.Log
import dev.chanwoo.androlinux.AlrRuntime
import java.io.File

/**
 * The guest's desktop SESSION: provisioned once, shared by every app.
 *
 * WHY THIS REPLACES A PILE OF PER-APP CODE. The app used to carry a list of
 * which packages are "gnome-platform" so those could be wrapped in
 * `dbus-run-session`, a predicate deciding which appIds need X11, a
 * host-precompiled gschemas overlay because the guest had no
 * glib-compile-schemas, a maintainer-script neutralizer overlay, font and icon
 * overlays, and a hand-audited catalog of eighteen apps. Every one of those is
 * a symptom of the same absence: there was no session, so each app got its
 * missing piece patched in individually, and the list grew with the app count.
 *
 * A Linux desktop app does not need to be known about. It needs a session bus,
 * a display, fonts, an icon theme, a mime database and compiled schemas -- all
 * of which are ordinary packages and ordinary caches. Provide them ONCE and
 * `apt install <anything>` works, its .desktop appears, and it launches. No
 * entry in any list.
 *
 * What stays platform-specific is small, mechanical, and NOT per-app:
 *  - policy-rc.d, which is Debian's OWN documented mechanism for a chroot with
 *    no init, not a hack of ours;
 *  - the bwrap passthrough (see [GuestShims]), because Android denies user
 *    namespaces to an app;
 *  - fakeroot for dpkg, which alr already provides.
 */
object GuestSession {

    private const val TAG = "alr_runtime"

    /** Bump when the provisioning steps change; the marker carries it. */
    private const val VERSION = 1

    /**
     * The session's own packages. Not an app list -- these are what a desktop
     * session IS, and every one of them is a dependency some GUI app would
     * otherwise pull in halfway and leave half-configured.
     *
     *  dbus-x11            dbus-daemon + dbus-launch: the session bus.
     *  xwayland            X11 clients on a wayland compositor. Installed
     *                      always so nothing has to ask which apps need it.
     *  libglib2.0-bin      glib-compile-schemas + gio. Its dpkg trigger is what
     *                      compiles GSettings schemas IN the guest, which is
     *                      what the precompiled-gschemas overlay was standing in
     *                      for.
     *  shared-mime-info    update-mime-database's trigger; gdk-pixbuf and GTK
     *                      both consult the mime db before they decode anything.
     *  adwaita-icon-theme  the fallback icon theme GTK aborts without.
     *  fontconfig          + its cache trigger.
     *  fonts-dejavu-core   something to actually render with.
     *  xdg-utils           xdg-open, which apps shell out to for links/files.
     */
    private val SESSION_PACKAGES = listOf(
        "dbus-x11", "xwayland", "libglib2.0-bin", "shared-mime-info",
        "adwaita-icon-theme", "hicolor-icon-theme", "fontconfig",
        "fonts-dejavu-core", "xdg-utils",
    )

    /**
     * Debian's documented no-init contract: a maintainer script asks
     * policy-rc.d whether it may start a service, and 101 means "no".
     *
     * This is why the maintscript-shim overlay -- a set of hand-written stubs
     * for update-rc.d, invoke-rc.d, deb-systemd-helper, dpkg-maintscript-helper
     * and friends -- is not needed. Those stubs were re-implementing, badly and
     * per-symptom, the answer that this one file gives correctly and once. See
     * /usr/share/doc/init-system-helpers/README.policy-rc.d.gz.
     */
    private const val POLICY_RC_D = "#!/bin/sh\nexit 101\n"

    /**
     * apt options for a phone.
     *
     * Retries is not belt-and-braces here. archive.ubuntu.com publishes nine A
     * records and, on this device's network, the 91.189.9x block does not
     * answer on port 80 while 185.125.190.x does -- so a fetch either works or
     * dies with "Unable to connect ... [IP: 91.189.91.82 80]" depending on
     * which address it drew. On a mobile link that is the normal condition, not
     * an anomaly, and one retry usually lands on a reachable address.
     */
    private val APT_OPTS = listOf(
        "-o", "Acquire::ForceIPv4=true",
        "-o", "Acquire::Retries=5",
    )

    private fun marker(rootfsDir: File) = File(rootfsDir, "var/lib/alr/.session-v$VERSION")

    fun isProvisioned(rootfsDir: File): Boolean = marker(rootfsDir).isFile

    /**
     * Bring the guest up to a usable desktop session. Idempotent and
     * marker-gated: the expensive part is one apt transaction, and it runs once
     * per rootfs per [VERSION].
     *
     * Returns true when the session is usable. A failure is reported, not
     * swallowed -- an app that launches into a half-provisioned session fails
     * later in a way that reads like the app's fault.
     */
    fun provision(alr: AlrRuntime, rootfsDir: File, distro: String): Boolean {
        if (isProvisioned(rootfsDir)) return true
        Log.i(TAG, "session: provisioning $distro (v$VERSION)")

        // BEFORE apt: a maintainer script that tries to start a service must be
        // answered, not left to fail. Written directly rather than through the
        // guest because it has to exist before the first package unpacks.
        runCatching {
            val f = File(rootfsDir, "usr/sbin/policy-rc.d")
            f.parentFile?.mkdirs()
            f.writeText(POLICY_RC_D)
            f.setExecutable(true, false)
        }.onFailure { Log.w(TAG, "session: policy-rc.d failed: ${it.message}") }

        GuestShims.install(rootfsDir)

        val missing = SESSION_PACKAGES.filterNot { pkgInstalled(alr, distro, it) }
        if (missing.isEmpty()) {
            Log.i(TAG, "session: all session packages already present")
        } else {
            Log.i(TAG, "session: installing ${missing.size} package(s): $missing")
            val up = alr.run(
                distro = distro, program = "/usr/bin/apt-get",
                arguments = APT_OPTS + "update",
                fakeroot = true, timeoutSeconds = 900,
            )
            if (!up.ok) Log.w(TAG, "session: apt-get update rc=${up.exitCode} (continuing)")
            val ins = alr.run(
                distro = distro, program = "/usr/bin/apt-get",
                arguments = APT_OPTS + listOf(
                    "install", "-y", "--no-install-recommends",
                ) + missing,
                fakeroot = true, timeoutSeconds = 3600,
                guestEnv = mapOf(
                    "DEBIAN_FRONTEND" to "noninteractive",
                    "DEBCONF_NONINTERACTIVE_SEEN" to "true",
                ),
            )
            if (!ins.ok) {
                Log.e(TAG, "session: apt install rc=${ins.exitCode}\n${ins.stderr.takeLast(2000)}")
                return false
            }
        }

        // The caches. dpkg triggers normally build these during the transaction;
        // running them again is cheap and covers a rootfs whose packages were
        // unpacked before their trigger tooling existed -- which is exactly the
        // state a base image is in.
        for ((prog, args) in listOf(
            "/usr/bin/update-mime-database" to listOf("/usr/share/mime"),
            "/usr/bin/gtk-update-icon-cache" to listOf("-f", "-t", "/usr/share/icons/Adwaita"),
            "/usr/bin/glib-compile-schemas" to listOf("/usr/share/glib-2.0/schemas"),
            "/usr/bin/fc-cache" to listOf("-f"),
        )) {
            val r = alr.run(distro, prog, args, fakeroot = true, timeoutSeconds = 600)
            Log.i(TAG, "session: ${File(prog).name} rc=${r.exitCode}")
        }

        return runCatching {
            marker(rootfsDir).apply { parentFile?.mkdirs() }.writeText("provisioned\n")
            true
        }.getOrDefault(false)
    }

    private fun pkgInstalled(alr: AlrRuntime, distro: String, pkg: String): Boolean =
        alr.run(
            distro, "/usr/bin/dpkg-query",
            listOf("-W", "-f=\${Status}", pkg), timeoutSeconds = 60,
        ).stdout.contains("install ok installed")

    /**
     * Start the per-session daemons and return the environment they publish.
     *
     * One bus for the whole session, not one per app wrapped in
     * `dbus-run-session`: apps that talk to each other (a file dialog and its
     * portal, an app and its own second instance) have to be on the SAME bus,
     * and a per-app bus quietly makes each app an island.
     *
     * The bus socket goes inside [xdgRuntimeDir], which already lives in the
     * rootfs so both the host and the guest can name it.
     */
    fun startSessionBus(
        alr: AlrRuntime,
        rootfsDir: File,
        distro: String,
        xdgRuntimeDir: String,
    ): Map<String, String> {
        val busGuest = "$xdgRuntimeDir/bus"
        val busHost = File(rootfsDir, busGuest.removePrefix("/"))
        if (busHost.exists()) {
            // A socket left by a previous session is not a live bus; connecting
            // to it hangs instead of failing, which is worse.
            runCatching { busHost.delete() }
        }
        val r = alr.run(
            distro = distro,
            program = "/usr/bin/dbus-daemon",
            arguments = listOf(
                "--session", "--fork", "--address=unix:path=$busGuest", "--print-address",
            ),
            timeoutSeconds = 60,
        )
        val addr = r.stdout.lineSequence().firstOrNull { it.startsWith("unix:") }?.trim()
        if (addr == null) {
            Log.w(TAG, "session: no dbus session bus (rc=${r.exitCode}) ${r.stderr.take(300)}")
            return emptyMap()
        }
        Log.i(TAG, "session: bus at $addr")
        return mapOf("DBUS_SESSION_BUS_ADDRESS" to addr)
    }
}
