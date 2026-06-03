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
 *   - `apt-get install -y --no-install-recommends <pkg>` (download .deb from the pool pool),
 *   - if apt's in-line unpack hit the nested re-entry edge, COMPLETE via a top-level
 *     `dpkg -i <apt-downloaded .deb>` (the proven single-level re-entry unpack+configure),
 *   - verify via `dpkg --status <pkg>` ("Status: install ok installed").
 *
 * Host abstraction: the two callers differ only in WHICH JNI symbol runs the guest
 * (MainActivity's mangled export vs the runtime AlrNative export) and in their Context for
 * extractOverlayTar. [Host] captures exactly that, so the body below is shared 1:1.
 *
 * Progress: [onProgress] receives coarse [Phase] transitions parsed from apt's stdout so the
 * UI can show a monotonic percent. The marker path passes a no-op (it logs instead).
 *
 * Demo-trust apt: the apt-mirror overlay ships `Trusted: yes` (no gpgv). Authenticated apt
 * (Signed-By keyring + gpgv) is a separate, spawn-tasked gap — NOT blocked on here.
 */
package dev.chanwoo.androlinux.runtime

import android.system.Os
import android.util.Log
import java.io.File

object AptInstaller {

    private const val TAG = "alr_loader"

    /** The overlays the apt path stages (same set + order as the proven probe). */
    private val APT_OVERLAYS = listOf("fakeroot", "apt-dpkg", "dpkg-db", "apt-mirror")

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
        for (name in APT_OVERLAYS) {
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
        var w = 0
        while (w < 40000 &&
            !(fakerootSo.isFile && aptGetBin.isFile && srcFile.isFile &&
                interposeSo.isFile && !interposeStaging())
        ) { Thread.sleep(500); w += 500 }
        Log.i(
            TAG,
            "aptinstall: fakeroot.so=${fakerootSo.isFile} apt-get=${aptGetBin.isFile} " +
                "sources=${srcFile.isFile} (waited ${w}ms)",
        )
        if (!(fakerootSo.isFile && aptGetBin.isFile && srcFile.isFile)) {
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

            val statusOut = host.loaderProbe(rootfsName, "/usr/bin/dpkg\n--status\n" + pkg)
            var installed = statusOut.contains("Status: install ok installed")
            Log.i(TAG, "aptinstall: installed=$installed (dpkg --status $pkg)")
            Log.i(TAG, "aptinstall-status:\n$statusOut")

            // COMPLETION via top-level dpkg -i on the apt-downloaded .deb (proven re-entry).
            if (!installed) {
                onProgress(Phase.UNPACKING)
                val archives = File(rootfsDir, "var/cache/apt/archives")
                val deb = archives.listFiles { f ->
                    f.name.startsWith("${pkg}_") &&
                        (f.name.endsWith("_arm64.deb") || f.name.endsWith("_all.deb"))
                }?.firstOrNull()
                if (deb != null && deb.isFile) {
                    val debRel = "/var/cache/apt/archives/${deb.name}"
                    Log.i(TAG, "aptinstall: completing via top-level dpkg -i $debRel (apt-downloaded .deb)")
                    val diOut = host.loaderProbe(
                        rootfsName,
                        "/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n-i\n" + debRel,
                    )
                    val diUnpack = diOut.contains("Unpacking $pkg")
                    val diConfig = diOut.contains("Setting up $pkg")
                    Log.i(TAG, "aptinstall: dpkg -i unpacking=$diUnpack settingUp=$diConfig")
                    Log.i(TAG, "aptinstall-dpkgi-out:\n$diOut")
                    if (diConfig) onProgress(Phase.CONFIGURING)
                    val statusOut2 = host.loaderProbe(rootfsName, "/usr/bin/dpkg\n--status\n" + pkg)
                    installed = statusOut2.contains("Status: install ok installed")
                    val nowPresent2 = candidateBins.filter { File(rootfsDir, it).isFile }
                    Log.i(TAG, "aptinstall: after dpkg -i installed=$installed binaries=$nowPresent2")
                    Log.i(TAG, "aptinstall-status2:\n$statusOut2")
                } else {
                    Log.w(TAG, "aptinstall: no ${pkg}_*.deb in apt archives cache to complete via dpkg -i")
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
        for (name in APT_OVERLAYS) {
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
            val statusOut = host.loaderProbe(rootfsName, "/usr/bin/dpkg\n--status\n" + pkg)
            // dpkg --status of a removed package: "not installed" / "Status: ... not-installed",
            // or an error that the package is unknown. Treat any non-"install ok installed" as gone.
            val stillInstalled = statusOut.contains("Status: install ok installed")
            Log.i(TAG, "aptremove: stillInstalled=$stillInstalled")
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
}
