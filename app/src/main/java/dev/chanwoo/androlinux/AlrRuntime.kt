package dev.chanwoo.androlinux

import java.io.File
import java.util.concurrent.TimeUnit

/**
 * The alr execution backend.
 *
 * alr runs a stock Ubuntu/Debian ARM64 **glibc** rootfs directly: the guest's
 * own `ld-linux-aarch64.so.1` is invoked explicitly with `--preload`, and a
 * signal-only ptrace supervisor rescues the SIGSYS that Android's zygote
 * seccomp filter raises for the 239 syscalls it blocks (`set_robust_list` is
 * the one that would otherwise kill every glibc program before `main`).
 *
 * It is not PRoot. PRoot mediates every syscall through ptrace; alr's
 * supervisor never issues `PTRACE_SYSCALL` at all, and reports
 * `path_traps=0 syscall_stops=0` to prove it. That invariant is why it is
 * fast, and it is checked on every run.
 *
 * TWO THINGS THIS DEPENDS ON, both measured on device:
 *
 *  1. `targetSdk = 28`, which puts us in SELinux domain `untrusted_app_27` --
 *     the only untrusted-app domain still granted
 *     `app_data_file:file execute_no_trans`. Without it the guest's ld.so
 *     cannot be exec'd at all. See the ALR DIRECT APP-DATA EXECVE probe.
 *
 *  2. The rootfs has been ADOPTED. The app downloads, verifies and extracts
 *     (RootfsInstaller); `alr adopt` then repairs /etc, installs the guest
 *     interposer and host wrappers, relativises absolute symlinks, and proves
 *     the tree boots. Running before adoption gets you a rootfs whose
 *     `/usr/bin/awk -> /etc/alternatives/awk` resolves against ANDROID.
 */
data class AlrResult(
    val exitCode: Int,
    val stdout: String,
    val stderr: String,
) {
    val ok: Boolean get() = exitCode == 0
    fun line(prefix: String): String =
        (stdout + "\n" + stderr).lineSequence().firstOrNull { it.startsWith(prefix) }.orEmpty()
}

/**
 * Variables alr owns and refuses via `-e`. Passing one fails the whole
 * invocation with env-reserved, so they are filtered rather than forwarded.
 *
 * Only the names alr actually owns -- NOT the whole ALR_ prefix. This app's GPU
 * bridge legitimately uses ALR_GPU_BRIDGE_*, and filtering the prefix made the
 * guest client miss its host and port and write frames to stdout.
 */
private val RESERVED = setOf(
    "LD_PRELOAD", "LD_LIBRARY_PATH", "LOCPATH", "GLIBC_TUNABLES",
    "ALR_COUNT", "ALR_DISTRO", "ALR_FAKEROOT", "ALR_GUEST_ARGV0",
    "ALR_GUEST_EXE", "ALR_GUEST_PATH", "ALR_LDSO", "ALR_LIBPATH",
    "ALR_LOG", "ALR_LOG_FD", "ALR_PRELOAD", "ALR_ROOT", "ALR_ROOT_DIR",
)

class AlrRuntime(
    private val nativeLibraryDir: File,
    private val filesDir: File,
) {
    /** The alr CLI. jniLibs is how an APK ships an executable. */
    val binary: File get() = File(nativeLibraryDir, "libalr.so")

    /**
     * alr's `$PREFIX`. It looks for its guest interposer at
     * `$PREFIX/share/alr/libalr_preload.so`, and defaults `paths.root` under
     * it. Pointing this at our filesDir is the whole of what "not Termux"
     * means to alr -- everything else already keys off `$PREFIX`.
     */
    private val prefix: File get() = File(filesDir, "usr")

    /** Matches the app's own convention, `filesDir/rootfs/<name>`. */
    private val rootfsBase: File get() = File(filesDir, "rootfs")

    fun isAvailable(): Boolean = binary.canExecute()

    /**
     * Copy the packaged interposer to where alr looks for it.
     *
     * It ships as an ASSET rather than in jniLibs on purpose: it is a glibc
     * object for the guest, and jniLibs would hand it to bionic's linker.
     * Re-copied whenever the size differs so an app update refreshes it --
     * a stale interposer against a new alr is exactly what `alr version`
     * reports as `preload-stale`.
     */
    fun installPreload(assets: android.content.res.AssetManager): Boolean = runCatching {
        val dest = File(prefix, "share/alr/libalr_preload.so")
        dest.parentFile?.mkdirs()
        assets.open("alr/libalr_preload.so").use { input ->
            val bytes = input.readBytes()
            // Compare CONTENT, not length. Two builds of the same source tree
            // differ by a few instructions and land on the same byte count more
            // often than you would guess -- MEASURED: a rebuilt interposer with
            // a real behaviour change was skipped because it was the same size,
            // and the device kept running the old one while every hash I
            // printed said the build was fine.
            val same = dest.isFile && dest.length() == bytes.size.toLong() &&
                dest.readBytes().contentEquals(bytes)
            if (!same) dest.writeBytes(bytes)
        }
        runCatching {
            assets.open("alr/manifest.json").use { m ->
                File(prefix, "share/alr/manifest.json").writeBytes(m.readBytes())
            }
        }
        // The libdl.so.2 stub, staged where alr looks for it. alr installs it
        // into a rootfs only when that rootfs has none of its own.
        runCatching {
            assets.open("alr/libdl.so.2").use { s ->
                File(prefix, "share/alr/libdl.so.2").writeBytes(s.readBytes())
            }
        }
        dest.isFile
    }.getOrDefault(false)

    /**
     * Stage alr's acceptance suite into the guest at `/opt/alr-tests`.
     *
     * It has to live INSIDE the rootfs because that is where it runs: the suite
     * is bash and calls awk, and Android has neither, so the only shell on this
     * device that can execute it is the guest's. That costs nothing in
     * validity -- a guest process forked from the app still carries the app's
     * uid and seccomp filter, which is exactly what the suite's own gate
     * checks before it will report a number.
     *
     * Returns the count of files staged; 0 means the caller should not pretend
     * it ran a suite.
     */
    fun installTests(assets: android.content.res.AssetManager, distro: String): Int {
        val root = File(rootfsBase, distro)
        if (!root.isDirectory) return 0
        val dest = File(root, "opt/alr-tests")
        var n = 0
        fun walk(assetDir: String, into: File) {
            val entries = runCatching { assets.list(assetDir) }.getOrNull() ?: return
            if (entries.isEmpty()) {
                into.parentFile?.mkdirs()
                runCatching {
                    assets.open(assetDir).use { s -> into.writeBytes(s.readBytes()) }
                    // The suite and its helpers are invoked as `bash file`, but
                    // acceptance.sh also re-execs helpers by path.
                    if (into.name.endsWith(".sh")) into.setExecutable(true, false)
                    n++
                }
                return
            }
            into.mkdirs()
            entries.forEach { walk("$assetDir/$it", File(into, it)) }
        }
        walk("alr/tests", dest)
        return n
    }

    /** `alr adopt <distro>` -- take the app-extracted tree and make it bootable. */
    fun adopt(distro: String, timeoutSeconds: Long = 120): AlrResult =
        exec(listOf("adopt", distro), timeoutSeconds)

    /**
     * Adopt a rootfs the first time we are asked to run something in it.
     *
     * An unadopted tree has no guest interposer and an unrepaired /etc, so
     * every command in it fails in a way that looks like the runtime is
     * broken. MEASURED: routing the app's rootfs probes through alr without
     * this took the report from 166 PASS / 5 FAIL to 154 / 17 -- twelve
     * checks that failed only because nobody had adopted the tree they ran in.
     *
     * The marker records WHICH alr adopted it, so an app update with a new
     * runtime re-adopts rather than running against an interposer built
     * against different sources.
     */
    private fun ensureAdopted(distro: String) {
        val root = File(rootfsBase, distro)
        if (!root.isDirectory) return
        val stamp = File(root, ".alr-adopted")
        val want = runCatching { binary.length().toString() + ":" + binary.lastModified() }
            .getOrDefault("unknown")
        if (stamp.isFile && runCatching { stamp.readText() }.getOrNull() == want) return
        val r = adopt(distro)
        // Record the attempt either way. Retrying a failed adoption on every
        // command turns one bad rootfs into a 120-second stall per call --
        // MEASURED as a cascade of timeouts across the whole report. A failure
        // is recorded with its exit code so the next run can see it was tried.
        runCatching { stamp.writeText(if (r.ok) want else "failed:${r.exitCode}:$want") }
    }

    /** `alr run <program> [args]` inside the guest. */
    fun run(
        distro: String,
        program: String,
        arguments: List<String> = emptyList(),
        fakeroot: Boolean = false,
        timeoutSeconds: Long = 60,
        /** ALR_LOG=1 makes alr print its supervisor counters on exit. */
        verbose: Boolean = false,
        /**
         * Extra variables for the GUEST, passed as `alr -e KEY=VAL`.
         *
         * The GUI probes carry their contract here -- XDG_RUNTIME_DIR,
         * WAYLAND_DISPLAY, DISPLAY -- and alr builds a DELIBERATE environment,
         * so anything the caller needs has to be handed over explicitly. The
         * PRoot path merged this map and the alr path dropped it, which is why
         * alr-wl-test reported "XDG_RUNTIME_DIR is invalid or not set".
         */
        guestEnv: Map<String, String> = emptyMap(),
    ): AlrResult {
        ensureAdopted(distro)
        // PROOT_* means nothing to alr, and alr REFUSES -e for the variables it
        // owns (ALR_*, LD_PRELOAD, LD_LIBRARY_PATH, LOCPATH, GLIBC_TUNABLES) --
        // passing those would fail the whole invocation with env-reserved.
        // alr refuses -e only for the variables it OWNS, not for the whole ALR_
        // prefix -- this app's GPU bridge legitimately uses ALR_GPU_BRIDGE_*,
        // and filtering the prefix here made the guest client miss its host and
        // port and write frames to stdout instead of the socket.
        val eArgs = guestEnv.entries
            .filterNot { it.key.startsWith("PROOT_") || it.key in RESERVED }
            .flatMap { listOf("-e", "${it.key}=${it.value}") }
        return exec(
        listOf("-d", distro, "run") + eArgs + listOf(program) + arguments,
        timeoutSeconds,
        extraEnv = buildMap {
            if (fakeroot) put("ALR_FAKEROOT", "1")
            if (verbose) put("ALR_LOG", "1")
        },
        )
    }

    /**
     * `alr install <distro>` -- fetch, verify and unpack an Ubuntu base image.
     *
     * alr discovers the current image from cdimage's SHA256SUMS and checks the
     * hash, so this is a real provisioning path and not a download-and-hope.
     * It must run from the APP process: `run-as` has no network on Android 16
     * (a bare-IP connect times out there while the same request from the shell
     * uid returns 200), the same way it has no seccomp filter -- run-as is not
     * the app, in one more respect.
     */
    fun installDistro(distro: String, timeoutSeconds: Long = 3600): AlrResult =
        exec(listOf("install", distro), timeoutSeconds)

    fun version(): AlrResult = exec(listOf("version"), 30)

    /**
     * Like [run], but streams the guest's output to [onLine] as it arrives and
     * blocks until the guest exits.
     *
     * [run] reads stdout to EOF, then stderr, then waits -- fine for a command
     * that finishes, and wrong twice over for a GUI session that lives for
     * minutes: nothing is visible until the app is closed, and a guest that
     * writes more to stderr than the pipe buffer holds blocks forever, because
     * nobody is draining it while stdout is being read. A GTK app that logs a
     * warning per frame does exactly that.
     */
    fun runStreaming(
        distro: String,
        program: String,
        arguments: List<String> = emptyList(),
        guestEnv: Map<String, String> = emptyMap(),
        onLine: (String) -> Unit,
    ): AlrResult {
        ensureAdopted(distro)
        val eArgs = guestEnv.entries
            .filterNot { it.key.startsWith("PROOT_") || it.key in RESERVED }
            .flatMap { listOf("-e", "${it.key}=${it.value}") }
        if (!isAvailable()) return AlrResult(-1, "", "alr binary not found at ${binary.absolutePath}")
        rootfsBase.mkdirs()
        val tmp = File(prefix, "tmp").apply { mkdirs() }
        val builder = ProcessBuilder(
            listOf(binary.absolutePath, "-d", distro, "run") + eArgs + listOf(program) + arguments,
        )
        builder.redirectErrorStream(true)
        builder.environment().apply {
            put("PREFIX", prefix.absolutePath)
            put("ALR_ROOT_DIR", rootfsBase.absolutePath)
            put("TMPDIR", tmp.absolutePath)
            put("HOME", filesDir.absolutePath)
        }
        return runCatching {
            val process = builder.start()
            val tail = ArrayDeque<String>()
            process.inputStream.bufferedReader().forEachLine { line ->
                onLine(line)
                // Keep only the tail: the caller classifies the exit from it, and
                // a long-lived GUI app can log megabytes.
                tail.addLast(line)
                if (tail.size > 200) tail.removeFirst()
            }
            AlrResult(process.waitFor(), tail.joinToString("\n"), "")
        }.getOrElse { AlrResult(-1, "", "alr exec failed: ${it.message}") }
    }

    private fun exec(
        arguments: List<String>,
        timeoutSeconds: Long,
        extraEnv: Map<String, String> = emptyMap(),
    ): AlrResult {
        if (!isAvailable()) {
            return AlrResult(-1, "", "alr binary not found at ${binary.absolutePath}")
        }
        rootfsBase.mkdirs()
        val tmp = File(prefix, "tmp").apply { mkdirs() }
        val builder = ProcessBuilder(listOf(binary.absolutePath) + arguments)
        builder.environment().apply {
            // Everything alr needs to forget Termux is these three.
            put("PREFIX", prefix.absolutePath)
            put("ALR_ROOT_DIR", rootfsBase.absolutePath)
            put("TMPDIR", tmp.absolutePath)
            put("HOME", filesDir.absolutePath)
            putAll(extraEnv)
        }
        return runCatching {
            val process = builder.start()
            val out = process.inputStream.bufferedReader().readText()
            val err = process.errorStream.bufferedReader().readText()
            val finished = process.waitFor(timeoutSeconds, TimeUnit.SECONDS)
            if (!finished) {
                process.destroyForcibly()
                AlrResult(-2, out, err + "\nalr: timed out after ${timeoutSeconds}s")
            } else {
                AlrResult(process.exitValue(), out, err)
            }
        }.getOrElse { AlrResult(-1, "", "alr exec failed: ${it.message}") }
    }
}
