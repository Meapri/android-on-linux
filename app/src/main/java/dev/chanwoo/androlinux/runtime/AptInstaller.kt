/*
 * AptInstaller — the SHARED, device-proven online apt-install pipeline.
 *
 * This is the EXTRACTION of MainActivity.runAptInstallProbe (CR-2 keystone, device-proven
 * for `tree`/`galculator`) into one reusable place so BOTH callers drive identical native
 * behavior:
 *   1. MainActivity's marker path (`echo install:<pkg> > /data/local/tmp/.alr-aptdrain`),
 *      which now delegates here — its logcat proof lines are preserved verbatim.
 *   2. NativeAlrRuntime.install(appId) — the in-app launcher UI install button.
 *
 * The pipeline (unchanged from the proven probe):
 *   - stage the apt overlays (apt-mirror demo-trust + fakeroot + apt-dpkg + dpkg-db) via the
 *     lib-downgrade-guarded extractOverlayTar, keyed on tar size (idempotent),
 *   - wait for the interpose .so to finish staging (its path-mediation ctor must run before
 *     any guest LD_PRELOADs it, or apt reads literal Android paths),
 *   - normalize the bundled dpkg status to a deps-stripped consistent leaf set so apt's
 *     global solver does not refuse on the pre-existing broken closure,
 *   - set the apt env (ALR_FAKEROOT/ALR_REEXEC_INPROC/ALR_PERSIST_GUEST/ALR_TEE_GUEST_STDOUT),
 *   - `apt-get -o Acquire::ForceIPv4=true update` (fetch the pinned-mirror index), re-normalize,
 *   - `apt-get install -y --no-install-recommends <pkg>` (download the .deb CLOSURE from
 *     the pool into var/cache/apt/archives),
 *   - if apt's in-line unpack hit the nested re-entry edge (apt→dpkg→dpkg-split, exit 71),
 *     COMPLETE the whole closure OUTSIDE apt: one top-level `dpkg -i <every archived .deb>`
 *     (dpkg unpacks all, then configures in dependency order) + `dpkg --configure -a` to
 *     flush any deferred configure — so a MULTI-DEP app (not just a single leaf) installs
 *     end-to-end without relying on apt's blocked in-line unpack (GAP 2),
 *   - judge success by reading the dpkg admin DB FILE directly (var/lib/dpkg/status parsed
 *     in-process for the pkg's "Status: install ok installed" stanza — NO dpkg-query re-exec,
 *     which on-device is a non-PIE ELF that exits 73 EMPTY through the in-proc re-exec and
 *     would falsely report a PHYSICALLY-installed package as installed=false); the exec
 *     `dpkg --status` stdout is kept only as corroboration + for its error line, and any dep
 *     that did not reach "installed" (per the DB file) is logged so a partial closure shows.
 *
 * OFFLINE/STAGED path: [installStaged] installs a PRE-STAGED `<pkg>-stage.tar` (built by
 * tools/build_install_stage.py — it ships the leaf `.deb` at var/cache/apt/archives/ plus the
 * unpacked closure) with NO mirror fetch, so it works when the network/apt-key path is down.
 * It deliberately mirrors the online path's COMPLETION+RECOVERY shape (the part that made
 * online robust): `dpkg -i <staged .deb>` → `dpkg --configure -a` → judge ONLY by
 * `dpkg --status` ("install ok installed"), never by a single dpkg exit code or a stdout
 * marker. The pre-existing MainActivity.launchAptDrainProbe staged drain judged success by
 * the loader's GUEST-EXEC `ran` flag (child exit==0 && stdout non-empty) + stdout markers,
 * so a non-zero dpkg exit (a failed maintainer-script, or a dpkg journal left dirty by the
 * unconditional `launchPackageManagerProbes` alr-smoke `dpkg -i` that races it on the SAME
 * admindir) reported unpacked=false even when the package actually installed. This method
 * removes that fragility: it clears any stale dpkg journal/lock first, then trusts the admin
 * DB state — exactly what the online path does after its top-level dpkg.
 *
 * Host abstraction: the two callers differ only in WHICH JNI symbol runs the guest
 * (MainActivity's mangled export vs the runtime AlrNative export) and in their Context for
 * extractOverlayTar. [Host] captures exactly that, so the body below is shared 1:1.
 *
 * Progress: [onProgress] receives coarse [Phase] transitions parsed from apt's stdout so the
 * UI can show a monotonic percent. The marker path passes a no-op (it logs instead).
 *
 * Signature mode: this pipeline drives whatever the staged apt-mirror overlay configures.
 * With `--demo-trust` the overlay ships `Trusted: yes` (verification SKIPPED). With the
 * AUTHENTICATED default it ships `Signed-By: <staged keyring>` + pins Dir::Bin::apt-key and
 * Apt::Key::gpgvcommand; noble apt 2.7.14 then verifies the InRelease by exec'ing apt-key →
 * gpgv (a 2-deep fork chain), which needs the apt+dpkg `--self-contained` overlay staged
 * (it ships apt-key + methods/gpgv + gpgv + the coreutils apt-key shells out to). This
 * helper is signature-mode-agnostic — it just runs `apt-get update`/`install`; whether that
 * update is authenticated is decided by the overlays, not here.
 */
package dev.chanwoo.androlinux.runtime

import android.system.Os
import android.util.Log
import java.io.File

object AptInstaller {

    private const val TAG = "alr_loader"

    /** The overlays the apt path stages (same set + order as the proven probe). */
    private val APT_OVERLAYS = listOf("fakeroot", "apt-dpkg", "dpkg-db", "apt-mirror")

    /**
     * The dpkg-tooling overlays the OFFLINE/STAGED path stages — the apt-mirror overlay is
     * dropped (no fetch), but the ORDER for the other three is the proven admindir-scaffold
     * order: apt-dpkg FIRST (ships the empty var/lib/dpkg/{updates,triggers,…} scaffold dirs),
     * then dpkg-db LAST so its populated status overwrites the scaffold's zero-byte one while
     * the scaffold's updates/ dir survives. fakeroot provides the uid=0 shim. The app's own
     * `<pkg>-stage.tar` (the .deb + closure) is staged after these, by [installStaged].
     */
    private val STAGED_TOOLING_OVERLAYS = listOf("fakeroot", "apt-dpkg", "dpkg-db")

    /**
     * The GENERAL maintainer-script neutralizer overlay — applied to EVERY apt install
     * (TASK: generalized from gnome-only). `maintscript-shim` ships no-op stubs (confmodule
     * with db_* → exit 0, policy-rc.d=101, ucf/ucfr, update-rc.d/invoke-rc.d, deb-systemd-
     * helper/-invoke, dpkg-reconfigure, and a dpkg-maintscript-helper that answers
     * `supports`→0 + every rm_conffile/mv_conffile operation→0) so the DEBCONF / INIT-SCRIPT /
     * CONFFILE-MAINTSCRIPT class of postinst/preinst exits 0 instead of the device-proven
     * exit-73 cascade. This is the SAME stub set that already unblocked gnome-calculator's
     * appstream PREINST; generalizing WHEN it is staged (always, not gnome-gated) unblocks the
     * X11-image-viewer class (nsxiv/feh/qiv/xpdf: x11-common exit 127 + libpaper1 exit 2 +
     * xfonts-* postinsts) and the apt Qt-GUI class (libqt6gui6t64 → libsm6 → x11-common).
     *
     * SAFETY: every member is a successful no-op for state that is meaningless in a non-root,
     * no-systemd, single-process guest — there is no init to register into, no debconf db to
     * seed, no systemd unit to enable, and the conffile churn dpkg-maintscript-helper performs
     * is an upgrade-time cleanup (cosmetic for a from-scratch install). It changes NO syscall
     * behaviour and weakens NO sandbox (cf. build_maintscript_shim_overlay.py docstring). So it
     * is correct to stage for ALL installs: a galculator-class app (whose delta has none of the
     * neutralized triggers) simply never sources these stubs, so its install is byte-identical;
     * an app that DOES pull x11-common/libpaper1/appstream now configures clean instead of
     * cascading. No-op when the tar is absent (graceful degradation). Built by
     * tools/build_maintscript_shim_overlay.py.
     */
    private val MAINTSCRIPT_SHIM_OVERLAY = listOf("maintscript-shim")

    /**
     * GNOME-platform-SPECIFIC overlays — staged ON TOP of the always-applied maintscript-shim
     * ONLY when the package is a gnome-platform app. `gnome-schemas` is the host-precompiled
     * gschemas.compiled so the GTK4 app's GSettings schemas resolve at runtime (the schema
     * COMPILE that the base's missing libglib2.0-bin/glib-compile-schemas cannot do); the
     * runtime session-dbus shim (dbus-run-session) is wired separately in NativeAppSession.
     * These carry the gnome dbus/schema baggage a plain X11/Qt app must NOT get. Built by
     * tools/build_common_data_overlay.py --schemas. No-op when the tar is absent.
     */
    private val GNOME_CONFIGURE_OVERLAYS = listOf("gnome-schemas")

    /**
     * apt package names that are gnome-platform apps needing the gnome-SPECIFIC overlays
     * (precompiled gschemas) on top of the general shim. Kept in lock-step with
     * NativeAlrRuntime.BundledCatalog's gnome entries. Detection is by the apt package name
     * (what install() receives), not the appId.
     */
    private val GNOME_PLATFORM_PKGS = setOf(
        "gnome-calculator", "gnome-text-editor", "eog", "file-roller", "gedit",
    )

    /** True iff [pkg]'s configure needs the gnome-SPECIFIC overlays (precompiled schemas). */
    private fun isGnomePlatformPkg(pkg: String): Boolean =
        pkg in GNOME_PLATFORM_PKGS

    /** Coarse install phases parsed from apt stdout → the UI's monotonic percent. */
    enum class Phase { RESOLVING, DOWNLOADING, UNPACKING, CONFIGURING, REGISTERING }

    /**
     * Everything that differs between the two call sites: the four app dirs, the native
     * loader probe symbol, and the overlay extractor. Logging is shared (logcat tag above).
     */
    interface Host {
        val packageName: String
        val nativeLibraryDir: String
        val filesDir: String
        val cacheDir: String

        /** Run a guest program (NEWLINE-delimited argv); BLOCKS until it exits; returns stdout. */
        fun loaderProbe(rootfsName: String, program: String): String

        /**
         * True when the runner behind [loaderProbe] supplies uid-0 emulation and
         * path virtualization ITSELF, so the staged overlays are not needed.
         *
         * The alr backend does: `ALR_FAKEROOT=1` is its own fakeroot, its
         * interposer is the path layer, and its resolver bridge answers apt's
         * DNS -- so `libalr_fakeroot.so`, `libalr_interpose.so` and the
         * mirror-IP `.sources` pin are all redundant there. Under the legacy
         * loader they are load-bearing, which is why this is a property of the
         * host and not a constant.
         *
         * MEASURED on Ubuntu 26.04: `apt-get update` fetched 28.2 MB and
         * `apt-get install sakura` pulled, unpacked and configured 108 packages
         * through alr with NO overlay staged at all. The install UI was
         * refusing that same operation with "apt 오버레이가 준비되지 않았습니다"
         * because the four dev-time stage tars were absent -- they only ever
         * existed in /data/local/tmp on a developer's device, so a clean
         * install could never install anything.
         */
        val selfSufficient: Boolean get() = false

        /** Extract an overlay stage-tar with the lib-downgrade guard. Returns (extracted#, skipped#). */
        fun extractOverlay(tar: File, rootfsDir: File): Pair<Int, Int>
    }

    /** Terminal result of an install attempt — what install() turns into Done/Failed. */
    data class Result(
        val installed: Boolean,
        val downloaded: Boolean,
        val binaryPresent: Boolean,
        /** A short human-readable failure reason when !installed (apt's E: line if present). */
        val error: String? = null,
    )

    /**
     * Run the full online apt install of [pkg] into [rootfsDir]. Blocks (call off the UI
     * thread). [onProgress] is fed coarse [Phase] transitions as apt's stdout streams; it may
     * be a no-op. Returns the terminal [Result]. Prereq-missing or exceptions → installed=false.
     *
     * The body is a faithful copy of the proven runAptInstallProbe, with the only differences
     * being (a) host-provided probe/extract and (b) the onProgress emissions.
     */
    fun install(
        host: Host,
        rootfsDir: File,
        rootfsName: String,
        pkg: String,
        onProgress: (Phase) -> Unit = {},
    ): Result {
        Log.i(TAG, "aptinstall: armed pkg=$pkg — staging interpose/fakeroot/apt-dpkg/dpkg-db/apt-mirror")
        onProgress(Phase.RESOLVING)

        // --- stage the interpose overlay (path-mediation .so) FIRST -----------------------
        // The path interposer (libalr_interpose.so) is what rewrites in-rootfs absolute paths
        // (apt's CPU table, locale-archive, …) to the on-device rootfs; WITHOUT it apt reads
        // literal Android paths and dies "E: Error reading the CPU table". MainActivity's
        // onCreate stages this, but the LAUNCHER-only install path has no such onCreate — so
        // stage it here, with the CANONICAL `.interpose-staged-<len>` marker the settle-wait
        // below blocks on (the SAME convention MainActivity uses).
        stageInterpose(host, rootfsDir)

        // --- stage the apt overlays (size-keyed, idempotent) ------------------------------
        // The maintscript-shim (general postinst/preinst neutralizer) is staged for EVERY
        // install now (TASK: generalized from gnome-only) — it is a successful no-op for a
        // galculator-class app and is what lets the x11-common/libpaper1/appstream/maintscript
        // class configure clean (unblocks the X11-viewer + apt-Qt-GUI classes). The
        // GNOME-SPECIFIC overlays (precompiled gschemas) stay gated to gnome-platform pkgs so a
        // plain X11/Qt app does NOT pull the gnome schema/dbus baggage.
        val overlaysToStage = APT_OVERLAYS + MAINTSCRIPT_SHIM_OVERLAY + (
            if (isGnomePlatformPkg(pkg)) GNOME_CONFIGURE_OVERLAYS else emptyList()
        )
        Log.i(TAG, "aptinstall: staging general maintscript-shim (always) for pkg=$pkg")
        if (isGnomePlatformPkg(pkg)) {
            Log.i(TAG, "aptinstall: pkg=$pkg is gnome-platform — ALSO staging " +
                "$GNOME_CONFIGURE_OVERLAYS (precompiled gschemas on top of the shim)")
        }
        for (name in overlaysToStage) {
            val tar = File("/data/local/tmp/$name-stage.tar")
            val m = File(rootfsDir, ".aptdrain-$name-staged-${tar.length()}")
            if (tar.isFile && !m.isFile) {
                val (extracted, skipped) = host.extractOverlay(tar, rootfsDir)
                m.writeText("staged\n")
                Log.i(TAG, "aptinstall: $name-stage done (extracted=$extracted skipped=$skipped)")
            } else if (!tar.isFile) {
                Log.i(TAG, "aptinstall: $name-stage.tar absent (push it to /data/local/tmp)")
            }
        }

        val fakerootSo = File(rootfsDir, "usr/lib/androlinux/libalr_fakeroot.so")
        val aptGetBin = File(rootfsDir, "usr/bin/apt-get")
        val srcFile = File(rootfsDir, "etc/apt/sources.list.alr.d/alr-ports.sources")
        // Wait for libalr_interpose.so to be fully rewritten (its .interpose-staged-<len>
        // marker lands only AFTER extract completes) before any guest LD_PRELOADs it.
        val interposeSo = File(rootfsDir, "usr/lib/androlinux/libalr_interpose.so")
        val interposeStageTar = File("/data/local/tmp/interpose-stage.tar")
        val interposeStaging = {
            interposeStageTar.isFile &&
                (rootfsDir.listFiles { f -> f.name.startsWith(".interpose-staged-") }?.isEmpty() ?: true)
        }
        var w = if (host.selfSufficient) 40000 else 0
        while (w < 40000 &&
            !(fakerootSo.isFile && aptGetBin.isFile && srcFile.isFile &&
                interposeSo.isFile && !interposeStaging())
        ) { Thread.sleep(500); w += 500 }
        Log.i(
            TAG,
            "aptinstall: fakeroot.so=${fakerootSo.isFile} apt-get=${aptGetBin.isFile} " +
                "sources=${srcFile.isFile} (waited ${w}ms)",
        )
        if (!aptGetBin.isFile) {
            Log.w(TAG, "aptinstall: the rootfs has no /usr/bin/apt-get")
            return Result(installed = false, downloaded = false, binaryPresent = false,
                error = "이 rootfs 에는 apt 가 없습니다 (/usr/bin/apt-get)")
        }
        if (!host.selfSufficient && !(fakerootSo.isFile && srcFile.isFile)) {
            Log.w(TAG, "aptinstall: prerequisites missing — push fakeroot/apt-dpkg/dpkg-db/apt-mirror stage tars")
            return Result(installed = false, downloaded = false, binaryPresent = false,
                error = "apt 오버레이가 준비되지 않았습니다 (fakeroot/apt-dpkg/dpkg-db/apt-mirror)")
        }

        val candidateBins = listOf("usr/bin/$pkg", "bin/$pkg", "usr/games/$pkg")
        val preExisting = candidateBins.filter { File(rootfsDir, it).isFile }
        Log.i(TAG, "aptinstall: pre-install binaries present=$preExisting (expect empty for a fresh download proof)")

        // --- deps-stripped dpkg status normalize (reusable; re-run after `update`) --------
        val normalizeStatus = normalize@{
            runCatching {
                val statusFile = File(rootfsDir, "var/lib/dpkg/status")
                if (!statusFile.isFile) return@normalize
                val backup = File(rootfsDir, "var/lib/dpkg/status.alr-preinstall")
                if (!backup.isFile) statusFile.copyTo(backup, overwrite = false)
                val src = backup.takeIf { it.isFile } ?: statusFile
                val dropPrefixes = listOf("Depends:", "Pre-Depends:", "Recommends:", "Suggests:")
                val sb = StringBuilder()
                var keptPkgs = 0
                for (stanza in src.readText().split("\n\n")) {
                    if (stanza.isBlank()) continue
                    if (!stanza.contains("Status: install ok installed")) {
                        sb.append(stanza.trimEnd('\n')).append("\n\n"); continue
                    }
                    keptPkgs++
                    var dropping = false
                    for (line in stanza.split("\n")) {
                        if (line.isEmpty()) continue
                        val isContinuation = line.startsWith(" ") || line.startsWith("\t")
                        if (isContinuation) {
                            if (!dropping) sb.append(line).append("\n")
                            continue
                        }
                        dropping = dropPrefixes.any { line.startsWith(it) }
                        if (!dropping) sb.append(line).append("\n")
                    }
                    sb.append("\n")
                }
                statusFile.writeText(sb.toString())
                Log.i(TAG, "aptinstall: normalized dpkg status (deps-stripped, kept $keptPkgs installed stanzas, ${statusFile.length()}b) for a clean solver")
            }.onFailure { Log.w(TAG, "aptinstall: status normalize failed: $it") }
            Unit
        }
        normalizeStatus()

        Os.setenv("ALR_FAKEROOT", "1", true)
        Os.setenv("ALR_REEXEC_INPROC", "1", true)
        Os.setenv("ALR_PERSIST_GUEST", "1", true)
        Os.setenv("ALR_INTERPOSE_DIAG", "1", true)
        Os.setenv("ALR_TEE_GUEST_STDOUT", "1", true)
        // Run debconf in noninteractive mode for EVERY install now (the maintscript-shim's
        // stub confmodule is always staged): any postinst that sources confmodule + reads a
        // debconf default (libpaper1/x11-common, not just gnome apps) proceeds without blocking
        // on a prompt. Inert/harmless when no closure package uses debconf (apt/dpkg already
        // prefer noninteractive headless), so set unconditionally. `gnomeConfigure` now only
        // gates the gnome-SPECIFIC overlays/shim (precompiled gschemas), not this env.
        Os.setenv("DEBIAN_FRONTEND", "noninteractive", true)
        Os.setenv("DEBCONF_NONINTERACTIVE_SEEN", "true", true)
        val gnomeConfigure = isGnomePlatformPkg(pkg)
        try {
            // STEP 1 — apt-get update: fetch the index from the pinned mirror.
            val upOut = host.loaderProbe(
                rootfsName,
                "/usr/bin/apt-get\n-o\nAcquire::ForceIPv4=true\nupdate",
            )
            val idxOk = upOut.contains("Reading package lists") ||
                upOut.contains("Packages") || upOut.contains("Get:")
            Log.i(TAG, "aptinstall: pre-install `apt-get update` indexOk=$idxOk")
            Log.i(TAG, "aptinstall-update-out:\n$upOut")
            // `apt-get update` restored full Depends in the status — re-strip for a clean solver.
            normalizeStatus()
            onProgress(Phase.DOWNLOADING)

            // STEP 2 — apt-get install -y --no-install-recommends <pkg>.
            val out = host.loaderProbe(
                rootfsName,
                "/usr/bin/apt-get\n-o\nAcquire::ForceIPv4=true\n-o\n" +
                    "APT::Sandbox::User=root\n-o\nAPT::Get::Fix-Broken=false\n-o\n" +
                    "pkgProblemResolver::FixByInstall=false\n" +
                    "install\n-y\n--no-install-recommends\n--no-fix-broken\n" + pkg,
            )
            val downloaded = out.contains("Get:")
            val unpacking = out.contains("Unpacking $pkg")
            val settingUp = out.contains("Setting up $pkg")
            Log.i(TAG, "aptinstall: pkg=$pkg downloaded=$downloaded unpacking=$unpacking settingUp=$settingUp")
            // Quote each apt download line so the proof shows the EXACT pool URLs.
            for (line in out.lineSequence()) {
                if (line.startsWith("Get:") || line.startsWith("Fetched ") ||
                    line.contains("ports.ubuntu.com")) {
                    Log.i(TAG, "aptinstall-fetch: $line")
                }
            }
            Log.i(TAG, "aptinstall-out:\n$out")
            if (unpacking) onProgress(Phase.UNPACKING)
            if (settingUp) onProgress(Phase.CONFIGURING)

            val nowPresent = candidateBins.filter { File(rootfsDir, it).isFile }
            Log.i(TAG, "aptinstall: post-install binaries present=$nowPresent (was $preExisting)")

            // VERDICT (robust): the dpkg admin DB FILE is authoritative (read directly, no
            // guest exec) — `dpkg --status` stdout is only corroboration because on-device
            // dpkg-query exits 73 EMPTY through the in-proc re-exec. So an apt install that
            // unpacked+configured in-line is judged installed here straight off the DB stanza
            // even when statusOut is empty. (We still keep statusOut for the error line.)
            val statusOut = host.loaderProbe(rootfsName, "/usr/bin/dpkg\n--status\n" + pkg)
            var installed = verdictInstalled(
                rootfsDir, pkg, statusOut,
                binaryPresent = candidateBins.any { File(rootfsDir, it).isFile },
            )
            Log.i(TAG, "aptinstall: installed=$installed (DB-file verdict; dpkg --status corroboration)")
            Log.i(TAG, "aptinstall-status:\n$statusOut")

            // COMPLETION via top-level dpkg on the apt-downloaded .deb SET (proven re-entry).
            //
            // GAP 2 — MULTI-DEP closures. apt's own in-line unpack hits the nested re-entry
            // DEPTH edge (apt → dpkg → dpkg-split, exit 71), so we finish OUTSIDE apt with a
            // single top-level dpkg, which is the proven ONE-level re-entry shape. For a leaf
            // package (tree/galculator) one `.deb` sufficed; a package with deps not already
            // in the base (e.g. a GTK app pulling several libs) has apt download the WHOLE
            // closure into var/cache/apt/archives, so we must install ALL of them, in
            // dependency order. dpkg does that ITSELF: given many .debs on one command line it
            // unpacks them all first, then configures in topological dependency order (it
            // defers a Setting-up until that package's Depends are configured). So one
            // `dpkg -i <every archived .deb>` installs+configures a multi-dep set end-to-end
            // without relying on apt's blocked in-line unpack. A trailing `dpkg --configure -a`
            // then flushes any package left half-configured (deferred by an ordering or a
            // re-entry hiccup), so the closure ends fully "install ok installed".
            if (!installed) {
                onProgress(Phase.UNPACKING)
                val archives = File(rootfsDir, "var/cache/apt/archives")
                // Every .deb apt downloaded for THIS install (the closure), target first so a
                // single-leaf install is byte-identical to the old behavior. apt names them
                // <pkg>_<ver>_<arch>.deb in the flat archives dir; _arm64 (native) + _all
                // (arch-independent data pkgs) are the two arches a noble arm64 closure uses.
                val allDebs = archives.listFiles { f ->
                    f.isFile && (f.name.endsWith("_arm64.deb") || f.name.endsWith("_all.deb"))
                }?.sortedBy { it.name } ?: emptyList()
                val target = allDebs.filter { it.name.startsWith("${pkg}_") }
                val deps = allDebs.filter { !it.name.startsWith("${pkg}_") }
                // Configure order on the COMMAND LINE doesn't matter (dpkg reorders configure
                // by Depends), but list the target last so its Setting-up is the final line —
                // easier to assert. dpkg still unpacks all before configuring any.
                val debSet = deps + target
                if (debSet.isNotEmpty()) {
                    val debRels = debSet.map { "/var/cache/apt/archives/${it.name}" }
                    Log.i(TAG, "aptinstall: completing via top-level dpkg -i on ${debSet.size} apt-downloaded .deb(s) " +
                        "(target=${target.map { it.name }}, deps=${deps.size}): ${debSet.map { it.name }}")
                    // `dpkg -i deb1 deb2 …`: unpack-all-then-configure-in-dep-order. NEWLINE-
                    // delimited argv (loaderProbe contract). --force-not-root/--force-bad-path
                    // are the same forces the leaf path used (fakeroot uid=0 + rootfs paths).
                    val diArgv = buildString {
                        append("/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n-i")
                        for (r in debRels) { append('\n'); append(r) }
                    }
                    val diOut = host.loaderProbe(rootfsName, diArgv)
                    val diUnpack = diOut.contains("Unpacking $pkg")
                    val diConfig = diOut.contains("Setting up $pkg")
                    Log.i(TAG, "aptinstall: dpkg -i (set) unpacking=$diUnpack settingUp=$diConfig")
                    Log.i(TAG, "aptinstall-dpkgi-out:\n$diOut")
                    if (diConfig) onProgress(Phase.CONFIGURING)

                    // Flush any half-configured package (deferred Setting-up). For a multi-dep
                    // set a dep may be unpacked-but-not-yet-configured if its own deps weren't
                    // ready on the first pass; `dpkg --configure -a` (a)ll re-runs every
                    // pending configure in dependency order until none remain. No-op (exit 0,
                    // nothing to configure) for a fully-configured single leaf, so it's safe to
                    // always run.
                    val cfgOut = host.loaderProbe(
                        rootfsName,
                        "/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n--configure\n-a",
                    )
                    val cfgConfiguredTarget = cfgOut.contains("Setting up $pkg")
                    Log.i(TAG, "aptinstall: dpkg --configure -a (flush) settingUpTarget=$cfgConfiguredTarget")
                    Log.i(TAG, "aptinstall-dpkgconfig-out:\n$cfgOut")
                    if (cfgConfiguredTarget) onProgress(Phase.CONFIGURING)

                    val statusOut2 = host.loaderProbe(rootfsName, "/usr/bin/dpkg\n--status\n" + pkg)
                    val nowPresent2 = candidateBins.filter { File(rootfsDir, it).isFile }
                    // Robust verdict again: DB-file stanza authoritative, dpkg --status stdout
                    // (likely empty via the non-PIE dpkg-query) only corroborates. This is the
                    // KEYSTONE of FIX 1 — the top-level `dpkg -i` PHYSICALLY configures the
                    // package (DB gets "install ok installed"), and we must report that even
                    // though the subsequent `dpkg --status` re-exec returns empty.
                    installed = verdictInstalled(
                        rootfsDir, pkg, statusOut2, binaryPresent = nowPresent2.isNotEmpty(),
                    )
                    Log.i(TAG, "aptinstall: after dpkg -i(set)+configure-a installed=$installed binaries=$nowPresent2")
                    Log.i(TAG, "aptinstall-status2:\n$statusOut2")
                    // Honesty: report any dep that did NOT reach "installed" so a partial
                    // closure is visible, not silently treated as success on the leaf alone.
                    if (installed && deps.isNotEmpty()) {
                        val depNames = deps.map { it.name.substringBefore('_') }.distinct()
                        // Read each dep's state from the DB FILE (no per-dep dpkg-query exec,
                        // which would falsely report "not configured" when dpkg-query exits 73).
                        val unconfigured = depNames.filter { dn -> !dpkgDbInstalled(rootfsDir, dn) }
                        if (unconfigured.isNotEmpty()) {
                            Log.w(TAG, "aptinstall: closure target installed but these deps are NOT configured: $unconfigured")
                        } else {
                            Log.i(TAG, "aptinstall: full closure configured (${depNames.size} deps + target)")
                        }
                    }
                } else {
                    Log.w(TAG, "aptinstall: no *.deb in apt archives cache to complete via dpkg -i")
                }
            }
            val binaryPresent = candidateBins.any { File(rootfsDir, it).isFile }
            Log.i(TAG, "aptinstall: FINAL pkg=$pkg downloaded=$downloaded installed=$installed binary=$binaryPresent")
            if (installed) onProgress(Phase.REGISTERING)
            val err = if (installed) null else aptErrorLine(out) ?: aptErrorLine(statusOut)
                ?: "설치를 완료하지 못했습니다 (apt/dpkg)"
            return Result(installed = installed, downloaded = downloaded, binaryPresent = binaryPresent,
                error = if (installed) null else err)
        } finally {
            Os.unsetenv("ALR_FAKEROOT")
            Os.unsetenv("ALR_REEXEC_INPROC")
            Os.unsetenv("ALR_PERSIST_GUEST")
            Os.unsetenv("ALR_INTERPOSE_DIAG")
            Os.unsetenv("ALR_TEE_GUEST_STDOUT")
            Os.unsetenv("DEBIAN_FRONTEND")
            Os.unsetenv("DEBCONF_NONINTERACTIVE_SEEN")
        }
    }

    /**
     * OFFLINE/STAGED install of [pkg] from a pre-staged `<pkg>-stage.tar` (no mirror fetch).
     *
     * The tar (tools/build_install_stage.py) ships the leaf `.deb` at
     * `var/cache/apt/archives/<pkg>_<ver>_<arch>.deb` plus the unpacked base-subtracted
     * closure. We stage it (+ the fakeroot/apt-dpkg/dpkg-db tooling overlays the dpkg run
     * needs, + interpose), then run the SAME completion shape the online path uses for its
     * downloaded debs — `dpkg -i <staged deb>` → `dpkg --configure -a` → verify by
     * `dpkg --status`. This is the product's offline/bundled-catalog install capability; it
     * does NOT touch the mirror so it is unaffected by the apt-key/keyring gap that gates the
     * online path.
     *
     * ROBUSTNESS over the old MainActivity drain (the b020804-era staged failure): we (a) judge
     * success purely by the admin DB (`dpkg --status` == "install ok installed"), never by the
     * single dpkg exit code or a stdout "Unpacking <pkg>" marker — a maintainer-script that
     * exits non-zero no longer masks a real unpack+configure; and (b) clear any stale dpkg
     * journal under `var/lib/dpkg/updates/` BEFORE the run, so a prior aborted `dpkg -i` (e.g.
     * the unconditional no-fakeroot alr-smoke probe that races on the same admindir) cannot
     * make this `dpkg -i` replay/abort with a non-zero exit.
     *
     * Returns the same [Result] shape as [install]; `downloaded` is reported as the .deb being
     * present in the staged cache (nothing is fetched). Prereq-missing → installed=false with a
     * clear "push <pkg>-stage.tar" error. Blocks; call off the UI thread.
     */
    fun installStaged(
        host: Host,
        rootfsDir: File,
        rootfsName: String,
        pkg: String,
        onProgress: (Phase) -> Unit = {},
    ): Result {
        Log.i(TAG, "aptinstall-staged: armed pkg=$pkg — staging interpose/fakeroot/apt-dpkg/dpkg-db + $pkg-stage.tar")
        onProgress(Phase.RESOLVING)

        // --- stage interpose (path-mediation .so) FIRST, then the tooling + app overlays ---
        stageInterpose(host, rootfsDir)
        // The staged path needs only the dpkg tooling (fakeroot uid=0 + dpkg/tar admindir) plus
        // the app's own <pkg>-stage.tar (its .deb + closure). apt-mirror is deliberately NOT
        // staged — there is no fetch, so the offline install is unaffected by the mirror/apt-key
        // gap that can block the online path. The general maintscript-shim IS staged (after the
        // tooling so its dpkg-maintscript-helper stub overwrites apt-dpkg's real one) because the
        // staged `dpkg -i` runs the package's maintainer scripts too — a staged X11/Qt app gets
        // the same x11-common/libpaper1/appstream neutralizer the online path now gets. Order:
        // tooling (scaffold order) THEN maintscript-shim THEN <pkg>-stage.
        val stageNames = STAGED_TOOLING_OVERLAYS + MAINTSCRIPT_SHIM_OVERLAY + pkg
        for (name in stageNames) {
            val tar = File("/data/local/tmp/$name-stage.tar")
            val m = File(rootfsDir, ".aptdrain-$name-staged-${tar.length()}")
            if (tar.isFile && !m.isFile) {
                val (extracted, skipped) = host.extractOverlay(tar, rootfsDir)
                m.writeText("staged\n")
                Log.i(TAG, "aptinstall-staged: $name-stage done (extracted=$extracted skipped=$skipped)")
            } else if (!tar.isFile) {
                Log.i(TAG, "aptinstall-staged: $name-stage.tar absent (push it to /data/local/tmp)")
            }
        }

        // --- settle-wait: fakeroot.so + dpkg + the staged .deb must all be present ----------
        val fakerootSo = File(rootfsDir, "usr/lib/androlinux/libalr_fakeroot.so")
        val dpkgBin = File(rootfsDir, "usr/bin/dpkg")
        val interposeSo = File(rootfsDir, "usr/lib/androlinux/libalr_interpose.so")
        val interposeStageTar = File("/data/local/tmp/interpose-stage.tar")
        val interposeStaging = {
            interposeStageTar.isFile &&
                (rootfsDir.listFiles { f -> f.name.startsWith(".interpose-staged-") }?.isEmpty() ?: true)
        }
        val archives = File(rootfsDir, "var/cache/apt/archives")
        // Discover the staged leaf .deb by glob (<pkg>_<ver>_<arch>.deb) rather than a hardcoded
        // version — robust to a tar rebuilt at a newer pkg version (the MainActivity drain
        // hardcodes e.g. galculator_2.1.4-1.2build2 and silently skips if the version moved).
        fun stagedDeb(): File? = archives.listFiles { f ->
            f.isFile && f.name.startsWith("${pkg}_") &&
                (f.name.endsWith("_arm64.deb") || f.name.endsWith("_all.deb"))
        }?.minByOrNull { it.name }
        var w = 0
        while (w < 40000 &&
            !(fakerootSo.isFile && dpkgBin.isFile && interposeSo.isFile &&
                !interposeStaging() && stagedDeb() != null)
        ) { Thread.sleep(500); w += 500 }
        val deb = stagedDeb()
        Log.i(
            TAG,
            "aptinstall-staged: fakeroot.so=${fakerootSo.isFile} dpkg=${dpkgBin.isFile} " +
                "interpose=${interposeSo.isFile && !interposeStaging()} ${pkg}.deb=${deb?.name} (waited ${w}ms)",
        )
        if (!(fakerootSo.isFile && dpkgBin.isFile) || deb == null) {
            Log.w(TAG, "aptinstall-staged: prerequisites missing — push fakeroot/apt-dpkg/dpkg-db + $pkg-stage.tar")
            return Result(installed = false, downloaded = (deb != null), binaryPresent = false,
                error = "오프라인 설치 패키지가 준비되지 않았습니다 ($pkg-stage.tar)")
        }
        onProgress(Phase.DOWNLOADING) // (already-present: the staged .deb stands in for the fetch)

        // --- defensive: clear any stale dpkg journal left by a prior aborted dpkg run -------
        // A `dpkg -i` that died mid-transaction (e.g. the unconditional no-fakeroot alr-smoke
        // probe racing on this admindir) can leave entries under var/lib/dpkg/updates/; the next
        // dpkg REPLAYS them and may abort non-zero before it touches our deb. The scaffold dir
        // itself must survive (dpkg needs it), so we delete only its file entries.
        runCatching {
            val updates = File(rootfsDir, "var/lib/dpkg/updates")
            val stale = updates.listFiles { f -> f.isFile }?.toList().orEmpty()
            if (stale.isNotEmpty()) {
                stale.forEach { it.delete() }
                Log.i(TAG, "aptinstall-staged: cleared ${stale.size} stale dpkg journal file(s) under var/lib/dpkg/updates/")
            }
            // A leftover lock can also wedge dpkg; it is recreated on demand.
            File(rootfsDir, "var/lib/dpkg/lock").takeIf { it.isFile }?.delete()
            File(rootfsDir, "var/lib/dpkg/lock-frontend").takeIf { it.isFile }?.delete()
        }.onFailure { Log.w(TAG, "aptinstall-staged: journal cleanup skipped: $it") }

        val candidateBins = listOf("usr/bin/$pkg", "bin/$pkg", "usr/games/$pkg")
        Os.setenv("ALR_FAKEROOT", "1", true)
        Os.setenv("ALR_REEXEC_INPROC", "1", true)
        Os.setenv("ALR_PERSIST_GUEST", "1", true)
        Os.setenv("ALR_TEE_GUEST_STDOUT", "1", true)
        // Always noninteractive (the maintscript-shim's stub confmodule is staged for staged
        // installs too) — same rationale as install(). (The offline/staged path does not stage
        // the gnome-specific precompiled-gschemas overlay; gnome apps install via the online
        // path. The general maintscript-shim IS staged here — see stageNames above.)
        Os.setenv("DEBIAN_FRONTEND", "noninteractive", true)
        Os.setenv("DEBCONF_NONINTERACTIVE_SEEN", "true", true)
        try {
            val debRel = "/var/cache/apt/archives/${deb.name}"
            onProgress(Phase.UNPACKING)
            // `dpkg -i <staged deb>` — same forces the online completion uses (fakeroot uid=0 +
            // rootfs paths). We do NOT branch on its exit/markers; the verdict is dpkg --status.
            val diOut = host.loaderProbe(
                rootfsName,
                "/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n-i\n$debRel",
            )
            Log.i(TAG, "aptinstall-staged: dpkg -i ${deb.name} unpacking=${diOut.contains("Unpacking $pkg")} settingUp=${diOut.contains("Setting up $pkg")}")
            Log.i(TAG, "aptinstall-staged-dpkgi-out:\n$diOut")
            onProgress(Phase.CONFIGURING)
            // Flush any deferred/half-done configure — the RECOVERY step the online path has and
            // the old drain lacked. Safe no-op (exit 0) for an already-fully-configured leaf.
            val cfgOut = host.loaderProbe(
                rootfsName,
                "/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n--configure\n-a",
            )
            Log.i(TAG, "aptinstall-staged: dpkg --configure -a settingUpTarget=${cfgOut.contains("Setting up $pkg")}")
            Log.i(TAG, "aptinstall-staged-dpkgconfig-out:\n$cfgOut")

            // VERDICT: the admin DB, not the exit code — and read the DB FILE directly rather
            // than re-exec'ing dpkg-query (which exits 73 EMPTY on-device through the in-proc
            // re-exec, so the exec verdict would falsely report installed=false for a package
            // that physically unpacked+configured). The exec `dpkg --status` is kept only for
            // its stdout (error line + corroboration once the non-PIE dpkg-query fix lands).
            val statusOut = host.loaderProbe(rootfsName, "/usr/bin/dpkg\n--status\n" + pkg)
            val binaryPresent = candidateBins.any { File(rootfsDir, it).isFile }
            val installed = verdictInstalled(rootfsDir, pkg, statusOut, binaryPresent)
            Log.i(TAG, "aptinstall-staged: FINAL pkg=$pkg installed=$installed binary=$binaryPresent (DB-file verdict)")
            Log.i(TAG, "aptinstall-staged-status:\n$statusOut")
            if (installed) onProgress(Phase.REGISTERING)
            val err = if (installed) null
                else aptErrorLine(diOut) ?: aptErrorLine(cfgOut)
                    ?: "오프라인 설치를 완료하지 못했습니다 (dpkg)"
            return Result(installed = installed, downloaded = true, binaryPresent = binaryPresent,
                error = err)
        } finally {
            Os.unsetenv("ALR_FAKEROOT")
            Os.unsetenv("ALR_REEXEC_INPROC")
            Os.unsetenv("ALR_PERSIST_GUEST")
            Os.unsetenv("ALR_TEE_GUEST_STDOUT")
            Os.unsetenv("DEBIAN_FRONTEND")
            Os.unsetenv("DEBCONF_NONINTERACTIVE_SEEN")
        }
    }

    /**
     * Minimal `apt-get remove -y <pkg>` + status check. Reuses the SAME staged overlays +
     * env as install (assumes install ran at least once; stages defensively anyway). Returns
     * true when dpkg no longer reports the package as installed.
     */
    fun remove(host: Host, rootfsDir: File, rootfsName: String, pkg: String): Boolean {
        Log.i(TAG, "aptremove: pkg=$pkg")
        stageInterpose(host, rootfsDir)
        // Stage the general maintscript-shim too: a package's prerm/postrm can also call
        // dpkg-maintscript-helper (rm_conffile on remove) under set -e — the stub keeps remove
        // from aborting the same way install would. Idempotent + cheap (markers gate re-extract).
        for (name in APT_OVERLAYS + MAINTSCRIPT_SHIM_OVERLAY) {
            val tar = File("/data/local/tmp/$name-stage.tar")
            val m = File(rootfsDir, ".aptdrain-$name-staged-${tar.length()}")
            if (tar.isFile && !m.isFile) {
                val (extracted, skipped) = host.extractOverlay(tar, rootfsDir)
                m.writeText("staged\n")
                Log.i(TAG, "aptremove: $name-stage done (extracted=$extracted skipped=$skipped)")
            }
        }
        Os.setenv("ALR_FAKEROOT", "1", true)
        Os.setenv("ALR_REEXEC_INPROC", "1", true)
        Os.setenv("ALR_PERSIST_GUEST", "1", true)
        Os.setenv("ALR_TEE_GUEST_STDOUT", "1", true)
        try {
            val out = host.loaderProbe(
                rootfsName,
                "/usr/bin/apt-get\n-o\nAPT::Sandbox::User=root\nremove\n-y\n" + pkg,
            )
            Log.i(TAG, "aptremove-out:\n$out")
            // Read the DB FILE directly (not a dpkg-query re-exec, which exits 73 EMPTY): a
            // removed package's stanza is gone or flips to a non-"install ok installed" state
            // (e.g. "deinstall ok config-files"), so dpkgDbInstalled returns false → removed.
            val stillInstalled = dpkgDbInstalled(rootfsDir, pkg)
            Log.i(TAG, "aptremove: stillInstalled=$stillInstalled (DB-file verdict)")
            return !stillInstalled
        } finally {
            Os.unsetenv("ALR_FAKEROOT")
            Os.unsetenv("ALR_REEXEC_INPROC")
            Os.unsetenv("ALR_PERSIST_GUEST")
            Os.unsetenv("ALR_TEE_GUEST_STDOUT")
        }
    }

    /**
     * Stage the interpose overlay (libalr_interpose.so) with the CANONICAL
     * `.interpose-staged-<tarlen>` marker — the SAME name+convention MainActivity's onCreate
     * uses and that the install() settle-wait blocks on. Idempotent (size-keyed). No-op if
     * the tar is absent (then the install relies on a pre-staged interpose .so, e.g. from a
     * prior MainActivity launch).
     */
    private fun stageInterpose(host: Host, rootfsDir: File) {
        val tar = File("/data/local/tmp/interpose-stage.tar")
        if (!tar.isFile) {
            Log.i(TAG, "aptinstall: interpose-stage.tar absent (relying on pre-staged interpose .so)")
            return
        }
        val marker = File(rootfsDir, ".interpose-staged-${tar.length()}")
        if (marker.isFile) return
        val (extracted, skipped) = host.extractOverlay(tar, rootfsDir)
        marker.writeText("staged\n")
        Log.i(TAG, "aptinstall: interpose-stage done (extracted=$extracted skipped=$skipped)")
    }

    /** First apt error line ("E: …") in [out], trimmed, or null. */
    private fun aptErrorLine(out: String): String? =
        out.lineSequence().firstOrNull { it.startsWith("E:") }?.trim()

    /**
     * ROBUST verdict signal #1 — read the dpkg admin DB FILE directly (no guest exec).
     *
     * `<rootfs>/var/lib/dpkg/status` is a plain RFC822-ish text file: blank-line-separated
     * stanzas, each with `Package: <name>` and `Status: <want> <eflag> <state>`. dpkg writes
     * the installed package's stanza with `Status: install ok installed` once `dpkg -i` (or
     * `--configure`) has unpacked+configured it — exactly the state the verdict needs. We
     * parse it HERE in Kotlin (a host-side file read) instead of re-exec'ing `dpkg --status`
     * because on-device `dpkg-query` (which `dpkg --status` execs) is a non-PIE ELF that exits
     * 73 with EMPTY stdout through the in-proc re-exec — so the exec-based verdict reports
     * installed=FALSE for a package that PHYSICALLY installed (DB stanza present + binary on
     * disk). This file read is independent of that loader gap, so the product reports success
     * correctly while the non-PIE dpkg-query fix lands separately.
     *
     * Stanza matching is EXACT on the `Package:` field (so `galculator` never matches
     * `galculator-common`). Returns true iff [pkg]'s stanza exists AND its `Status:` line
     * ends in `install ok installed`. Any IO/parse failure → false (caller falls back to the
     * exec-based corroboration). Mirrors the stanza split the in-file normalizeStatus uses.
     */
    fun dpkgDbInstalled(rootfsDir: File, pkg: String): Boolean = runCatching {
        val statusFile = File(rootfsDir, "var/lib/dpkg/status")
        if (!statusFile.isFile) return@runCatching false
        for (stanza in statusFile.readText().split("\n\n")) {
            if (stanza.isBlank()) continue
            var isThisPkg = false
            var installedOk = false
            for (line in stanza.split("\n")) {
                // Field lines start at column 0; continuation lines are indented — skip those.
                if (line.startsWith(" ") || line.startsWith("\t")) continue
                val colon = line.indexOf(':')
                if (colon <= 0) continue
                val key = line.substring(0, colon).trim()
                val value = line.substring(colon + 1).trim()
                when (key) {
                    "Package" -> isThisPkg = (value == pkg)
                    // dpkg Status is "<want> <eflag> <state>"; "installed" is the 3rd word.
                    // Match the canonical fully-installed line exactly.
                    "Status" -> installedOk = (value == "install ok installed")
                }
            }
            if (isThisPkg) return@runCatching installedOk
        }
        false
    }.getOrElse {
        Log.w(TAG, "aptinstall: dpkgDbInstalled($pkg) read failed: $it")
        false
    }

    /**
     * The ROBUST install verdict, decoupled from re-exec'ing dpkg-query. True iff EITHER
     * (a) the dpkg admin DB FILE reports [pkg] as `install ok installed` (the authoritative
     * signal, read directly — see [dpkgDbInstalled]), OR (b) the exec-based `dpkg --status`
     * stdout still says so (corroboration that works once the non-PIE dpkg-query loader fix
     * lands). The package's main binary being present in the rootfs is an ADDITIONAL
     * confirmation we log, but a configured package with its `install ok installed` stanza is
     * authoritatively installed even if its binary lives at a non-standard path the candidate
     * list misses — so the DB-file stanza alone suffices for the verdict (binary presence is
     * corroboration, not a gate). [statusOut] is the captured `dpkg --status` stdout (possibly
     * empty when dpkg-query exited 73); pass "" when no exec was run.
     */
    private fun verdictInstalled(
        rootfsDir: File,
        pkg: String,
        statusOut: String,
        binaryPresent: Boolean,
    ): Boolean {
        val dbOk = dpkgDbInstalled(rootfsDir, pkg)
        val execOk = statusOut.contains("Status: install ok installed")
        val installed = dbOk || execOk
        Log.i(TAG, "aptinstall: verdict pkg=$pkg → installed=$installed " +
            "(dbFile=$dbOk execStatus=$execOk binaryPresent=$binaryPresent)")
        return installed
    }
}
