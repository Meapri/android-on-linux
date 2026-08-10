package dev.chanwoo.androlinux

import java.io.File
import java.util.concurrent.TimeUnit

private const val COMMAND_TIMEOUT_SECONDS = 5L

data class NativeCommandResult(
    val command: File,
    val environment: Map<String, String>,
    val exitCode: Int,
    val stdout: String,
    val stderr: String,
)

class NativeCommandRunner(
    private val nativeLibraryDir: File,
    private val prootTmpDir: File,
    /**
     * Where alr lives. When non-null, every rootfs command runs through the
     * alr backend instead of PRoot; when null the PRoot path is used, so a
     * device without a built alr still behaves as before.
     */
    private val filesDir: File? = null,
) {
    private val alr: AlrRuntime? by lazy {
        filesDir?.let { AlrRuntime(nativeLibraryDir, it) }?.takeIf { it.isAvailable() }
    }

    /** Which backend actually served the last rootfs command. */
    var lastBackend: String = "none"
        private set

    fun runSmokeTest(): NativeCommandResult = runPackagedCommand("libalr_test_command.so", listOf("smoke"))

    fun runProotCandidateSmokeTest(): NativeCommandResult =
        runPackagedCommand("libalr_proot.so", listOf("--version"), prootEnvironment())

    fun runProotHelpProbe(): NativeCommandResult =
        runPackagedCommand("libalr_proot.so", listOf("--help"), prootEnvironment())

    fun runProotShortVersionProbe(): NativeCommandResult =
        runPackagedCommand("libalr_proot.so", listOf("-V"), prootEnvironment())

    fun runProotNoEnvVersionProbe(): NativeCommandResult =
        runPackagedCommand("libalr_proot.so", listOf("--version"))

    fun runProotViaLinkerVersionProbe(): NativeCommandResult =
        runAbsoluteCommand(
            File("/system/bin/linker64"),
            listOf(File(nativeLibraryDir, "libalr_proot.so").absolutePath, "--version"),
            prootEnvironment(),
        )

    fun runProotLoaderDirectProbe(): NativeCommandResult =
        runPackagedCommand("libproot-loader.so", emptyList(), prootEnvironment())

    fun runTallocViaLinkerProbe(): NativeCommandResult =
        runAbsoluteCommand(
            File("/system/bin/linker64"),
            listOf(File(nativeLibraryDir, "libtalloc.so").absolutePath),
            mapOf("LD_LIBRARY_PATH" to nativeLibraryDir.absolutePath),
        )

    fun runProotRootfsProgram(rootfsDir: File, program: String): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, program)

    fun runProotRootfsProgramAsRoot(rootfsDir: File, program: String): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, program, rootId = true)

    fun runProotRootfsIdAsRoot(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/id", rootId = true, rawRootfs = true)

    fun runProotRootfsDpkgVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/dpkg", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsDpkgPrintArchitecture(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/dpkg", listOf("--print-architecture"), rootId = true, rawRootfs = true)

    fun runProotRootfsDpkgQueryVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/dpkg-query", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsDpkgSplitVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/dpkg-split", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsAptVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/apt", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsAptGetVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/apt-get", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsAptCacheVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/apt-cache", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsAptConfigVersion(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/apt-config", listOf("--version"), rootId = true, rawRootfs = true)

    fun runProotRootfsDpkgInstallLocalSmoke(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(
            rootfsDir,
            "/usr/bin/dpkg",
            listOf("-i", "/var/cache/apt/archives/alr-smoke_1.0_arm64.deb"),
            rootId = true,
            rawRootfs = true,
            linkToSymlink = true,
            binds = minimalPackageManagerBinds(),
        )

    fun runProotRootfsInstalledPackageSmoke(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(
            rootfsDir,
            "/usr/local/bin/alr-package-smoke",
            rootId = true,
            rawRootfs = true,
            linkToSymlink = true,
            binds = minimalPackageManagerBinds(),
        )

    fun runProotRootfsGuestGpuClient(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/alr-gpu-client", rootId = true, rawRootfs = true)

    fun runProotRootfsGuestGpuClientIpc(rootfsDir: File, port: Int): NativeCommandResult =
        runProotRootfsCommand(
            rootfsDir,
            "/usr/bin/alr-gpu-client",
            rootId = true,
            rawRootfs = true,
            extraEnvironment = mapOf(
                "ALR_GPU_BRIDGE_HOST" to "127.0.0.1",
                "ALR_GPU_BRIDGE_PORT" to port.toString(),
                "ALR_GPU_BRIDGE_TRANSPORT" to "tcp-loopback",
            ),
        )

    fun runProotRootfsGuestGlesShimSmoke(rootfsDir: File): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/usr/bin/alr-gles-shim-smoke", rootId = true, rawRootfs = true)

    fun runProotRootfsGuestGuiClient(rootfsDir: File, protocol: String): NativeCommandResult =
        runProotRootfsCommand(
            rootfsDir,
            if (protocol == "X11") "/usr/bin/alr-x11-gpu-client" else "/usr/bin/alr-wayland-gpu-client",
            rootId = true,
            rawRootfs = true,
        )

    fun runProotRootfsGuestGuiClientIpc(rootfsDir: File, protocol: String, port: Int): NativeCommandResult =
        runProotRootfsCommand(
            rootfsDir,
            if (protocol == "X11") "/usr/bin/alr-x11-gpu-client" else "/usr/bin/alr-wayland-gpu-client",
            rootId = true,
            rawRootfs = true,
            extraEnvironment = mapOf(
                "ALR_GUI_BRIDGE_HOST" to "127.0.0.1",
                "ALR_GUI_BRIDGE_PORT" to port.toString(),
                "ALR_GUI_BRIDGE_PROTOCOL" to protocol,
                "ALR_GPU_BRIDGE_TRANSPORT" to "tcp-loopback-gui",
            ),
        )

    fun runProotRootfsProgramVerbose(rootfsDir: File, program: String): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, program, verbose = "9")

    fun runProotRootfsShell(rootfsDir: File, command: String): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/bin/sh", listOf("-c", command))

    fun runProotRootfsDash(rootfsDir: File, command: String): NativeCommandResult =
        runProotRootfsCommand(rootfsDir, "/bin/dash", listOf("-c", command))

    private fun runProotRootfsCommand(
        rootfsDir: File,
        program: String,
        arguments: List<String> = emptyList(),
        verbose: String = "-1",
        rootId: Boolean = false,
        rawRootfs: Boolean = false,
        binds: List<String> = emptyList(),
        // Android app-data filesystems reject hardlink() with EACCES, which breaks
        // dpkg's atomic status backup (status-old). PRoot's `--link2symlink`
        // extension emulates hardlinks via symlinks; enable it for package-manager
        // operations that create backup hardlinks. Confirmed on device: dpkg -i
        // fails without it and succeeds with it.
        linkToSymlink: Boolean = false,
        extraEnvironment: Map<String, String> = emptyMap(),
    ): NativeCommandResult {
        // THE BACKEND SWITCH. Every rootfs command in this class funnels
        // through here, so replacing this body replaces the backend for all
        // ~50 of them without touching a call site.
        //
        // alr when it is available. PRoot mediates every syscall through
        // ptrace; alr invokes the guest's own ld.so with --preload and runs a
        // signal-only supervisor that never issues PTRACE_SYSCALL. Measured in
        // this app's domain: Ubuntu 24.04 boots, networks, and reports
        // path_traps=0 syscall_stops=0.
        //
        // PRoot also stopped working here. At targetSdk 28 its void-syscall
        // cancellation lands in a SIGSYS handler that returns ENOSYS, so
        // `execve("/bin/hello")` fails with "Function not implemented" -- and
        // it could never `dpkg -i` in this domain even before that.
        //
        // The PRoot path stays for now as a fallback for a build without alr;
        // it goes when nothing selects it.
        val backend = alr
        if (backend != null) {
            lastBackend = "alr"
            val distro = rootfsDir.name
            val r = backend.run(
                distro = distro,
                program = program,
                arguments = arguments,
                fakeroot = rootId,
                timeoutSeconds = COMMAND_TIMEOUT_SECONDS * 4,
            )
            return NativeCommandResult(
                command = backend.binary,
                environment = sortedMapOf("ALR_BACKEND" to "1", "ALR_DISTRO" to distro),
                exitCode = r.exitCode,
                stdout = r.stdout.trim(),
                stderr = r.stderr.trim(),
            )
        }
        lastBackend = "proot"
        return runPackagedCommand(
            "libalr_proot.so",
            listOf(if (rawRootfs) "-r" else "-R", rootfsDir.absolutePath) +
                (if (linkToSymlink) listOf("-l") else emptyList()) +
                binds.flatMap { listOf("-b", it) } +
                (if (rootId) listOf("-0") else emptyList()) +
                listOf("-w", "/", program) + arguments,
            prootEnvironment(verbose = verbose, rootfsDir = rootfsDir, program = program) + extraEnvironment,
        )
    }

    private fun minimalPackageManagerBinds(): List<String> = listOf(
        "/dev/null:/dev/null",
        "/dev/zero:/dev/zero",
        "/dev/urandom:/dev/urandom",
    )

    private fun prootEnvironment(
        verbose: String = "-1",
        rootfsDir: File? = null,
        program: String? = null,
    ): Map<String, String> {
        prootTmpDir.mkdirs()
        val environment = mutableMapOf(
            "PROOT_LOADER" to File(nativeLibraryDir, "libproot-loader.so").absolutePath,
            "PROOT_TMP_DIR" to prootTmpDir.absolutePath,
            "PROOT_NO_SECCOMP" to "1",
            "PROOT_VERBOSE" to verbose,
            "LD_LIBRARY_PATH" to nativeLibraryDir.absolutePath,
            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
            "HOME" to "/root",
            "TMPDIR" to "/tmp",
            "PATH" to "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        )
        if (rootfsDir != null) {
            environment["ALR_ROOTFS"] = rootfsDir.absolutePath
        }
        if (program != null) {
            environment["ALR_PROGRAM"] = program
        }
        return environment
    }

    private fun runPackagedCommand(
        fileName: String,
        arguments: List<String>,
        environment: Map<String, String> = emptyMap(),
    ): NativeCommandResult = runAbsoluteCommand(File(nativeLibraryDir, fileName), arguments, environment)

    private fun runAbsoluteCommand(
        command: File,
        arguments: List<String>,
        environment: Map<String, String> = emptyMap(),
    ): NativeCommandResult {
        val processBuilder = ProcessBuilder(listOf(command.absolutePath) + arguments)
            .redirectErrorStream(false)
        processBuilder.environment().clear()
        processBuilder.environment().putAll(environment)
        val process = processBuilder.start()
        val completed = process.waitFor(COMMAND_TIMEOUT_SECONDS, TimeUnit.SECONDS)
        val exitCode = if (completed) {
            process.exitValue()
        } else {
            process.destroyForcibly()
            process.waitFor()
            -124
        }
        val stdout = process.inputStream.bufferedReader().use { it.readText() }.trim()
        val stderr = process.errorStream.bufferedReader().use { it.readText() }.trim()
        return NativeCommandResult(
            command = command,
            environment = environment.toSortedMap(),
            exitCode = exitCode,
            stdout = stdout,
            stderr = if (completed) stderr else listOf(stderr, "timeout after ${COMMAND_TIMEOUT_SECONDS}s").filter { it.isNotBlank() }.joinToString("\n"),
        )
    }
}
