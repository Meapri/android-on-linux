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
            if (!dest.isFile || dest.length() != bytes.size.toLong()) dest.writeBytes(bytes)
        }
        runCatching {
            assets.open("alr/manifest.json").use { m ->
                File(prefix, "share/alr/manifest.json").writeBytes(m.readBytes())
            }
        }
        dest.isFile
    }.getOrDefault(false)

    /** `alr adopt <distro>` -- take the app-extracted tree and make it bootable. */
    fun adopt(distro: String, timeoutSeconds: Long = 120): AlrResult =
        exec(listOf("adopt", distro), timeoutSeconds)

    /** `alr run <program> [args]` inside the guest. */
    fun run(
        distro: String,
        program: String,
        arguments: List<String> = emptyList(),
        fakeroot: Boolean = false,
        timeoutSeconds: Long = 60,
        /** ALR_LOG=1 makes alr print its supervisor counters on exit. */
        verbose: Boolean = false,
    ): AlrResult = exec(
        listOf("-d", distro, "run", program) + arguments,
        timeoutSeconds,
        extraEnv = buildMap {
            if (fakeroot) put("ALR_FAKEROOT", "1")
            if (verbose) put("ALR_LOG", "1")
        },
    )

    fun version(): AlrResult = exec(listOf("version"), 30)

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
