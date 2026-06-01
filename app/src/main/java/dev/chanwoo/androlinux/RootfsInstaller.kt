package dev.chanwoo.androlinux

import android.content.Context
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

data class RootfsInstallStatus(
    val manifestName: String,
    val assetPath: String,
    val verified: Boolean,
    val extracted: Boolean,
    val stagedArchive: File,
    val rootfsDir: File,
    val markerPath: File,
)

/** Outcome of applying an overlay stage tar with the lib-downgrade guard. */
data class OverlayExtractResult(
    val extracted: Int,
    val skipped: List<String>,
)

class RootfsInstaller(private val context: Context) {
    fun prepareBundledTinyRootfs(): RootfsInstallStatus {
        val manifestText = context.assets
            .open("rootfs/manifests/debian-arm64-bookworm-slim.json")
            .bufferedReader()
            .use { it.readText() }
        val manifestJson = JSONObject(manifestText)
        val assetJson = manifestJson.getJSONArray("assets").getJSONObject(0)
        val manifest = RootfsManifest(
            name = manifestJson.getString("name"),
            version = manifestJson.getString("version"),
            assets = listOf(
                RootfsAsset(
                    path = assetJson.getString("path"),
                    sha256 = assetJson.getString("sha256"),
                    sizeBytes = assetJson.getLong("size_bytes"),
                ),
            ),
        )
        val plan = buildRootfsInstallPlan(manifest, context.filesDir)
        val asset = manifest.assets.first()
        val stagedArchive = plan.assetDestinations.getValue(asset.path)
        stagedArchive.parentFile?.mkdirs()
        context.assets.open("rootfs/payloads/${asset.path}").use { input ->
            stagedArchive.outputStream().use { output -> input.copyTo(output) }
        }
        val verified = verifyAsset(stagedArchive, asset)
        val extracted = if (verified) {
            cleanRootfsDir(plan.rootfsDir)
            extractVerifiedTar(stagedArchive, plan.rootfsDir)
            removeStaleHostDpkgConfig(plan.rootfsDir)
            writeInstallMarker(plan.markerPath)
            isExtracted(plan)
        } else {
            false
        }
        return RootfsInstallStatus(
            manifestName = manifest.name,
            assetPath = asset.path,
            verified = verified,
            extracted = extracted,
            stagedArchive = stagedArchive,
            rootfsDir = plan.rootfsDir,
            markerPath = plan.markerPath,
        )
    }

    fun verifyAsset(file: File, asset: RootfsAsset): Boolean {
        if (!file.isFile || file.length() != asset.sizeBytes) return false
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val read = input.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString(separator = "") { byte -> "%02x".format(byte) } == asset.sha256.lowercase()
    }

    fun cleanRootfsDir(rootfsDir: File) {
        val rootfsParent = rootfsDir.parentFile?.canonicalFile
        if (rootfsDir.exists()) {
            require(rootfsParent != null && rootfsDir.canonicalFile.parentFile == rootfsParent) {
                "unsafe rootfs clean target: ${rootfsDir.absolutePath}"
            }
            rootfsDir.deleteRecursively()
        }
        rootfsDir.mkdirs()
    }

    fun removeStaleHostDpkgConfig(rootfsDir: File) {
        File(rootfsDir, "etc/dpkg/dpkg.cfg.d/needrestart").delete()
    }

    fun extractVerifiedTar(archive: File, rootfsDir: File) {
        rootfsDir.mkdirs()
        val rootCanonical = rootfsDir.canonicalPath
        TarArchiveInputStream(archive.inputStream().buffered()).use { tar ->
            while (true) {
                val entry = tar.getNextEntry() ?: break
                validateTarEntry(entry)
                extractEntry(tar, entry, rootfsDir, rootCanonical)
            }
        }
    }

    private fun extractEntry(
        tar: TarArchiveInputStream,
        entry: TarArchiveEntry,
        rootfsDir: File,
        rootCanonical: String,
    ) {
        val target = File(rootfsDir, entry.name.removePrefix("./"))
        val targetCanonical = target.canonicalPath
        if (!targetCanonical.startsWith(rootfsDir.canonicalPath + File.separator) && targetCanonical != rootCanonical) {
            throw IllegalArgumentException("unsafe tar extraction target: ${entry.name}")
        }
        when {
            entry.isDirectory -> target.mkdirs()
            entry.isSymbolicLink -> {
                // Real rootfses need symlinks (SONAME links libfoo.so.N ->
                // libfoo.so.N.M.P, multiarch dirs, etc.). Create ONLY links
                // whose target resolves WITHIN the rootfs — a relative,
                // in-tree target is followed safely by the kernel during the
                // guest's (path-mediated) open. Absolute or escaping targets
                // would resolve against the Android root, so they are skipped.
                val linkTarget = entry.linkName
                val resolved = File(target.parentFile, linkTarget).canonicalPath
                val safe = !linkTarget.startsWith("/") &&
                    (resolved == rootCanonical ||
                        resolved.startsWith(rootfsDir.canonicalPath + File.separator))
                if (safe) {
                    target.parentFile?.mkdirs()
                    if (target.exists()) target.delete()
                    android.system.Os.symlink(linkTarget, target.absolutePath)
                } else {
                    android.util.Log.w(
                        "alr-rootfs",
                        "skip unsafe symlink ${entry.name} -> $linkTarget",
                    )
                }
            }
            entry.isLink -> {
                // Hard link: copy the already-extracted in-rootfs original.
                val src = File(rootfsDir, entry.linkName.removePrefix("./"))
                if (src.canonicalPath.startsWith(rootfsDir.canonicalPath) && src.isFile) {
                    target.parentFile?.mkdirs()
                    src.copyTo(target, overwrite = true)
                    target.setReadable(true, true)
                }
            }
            entry.isFile -> {
                target.parentFile?.mkdirs()
                target.outputStream().use { output -> tar.copyTo(output) }
                target.setReadable(true, true)
                // Set the exec bit when the tar entry is executable OR the file is a
                // shared library. ALR's dlopen (file-backed PROT_EXEC under
                // untrusted_app) REJECTS a non-executable .so — Debian ships .so as
                // 0644 (no x), which extracts to 0600 and breaks dlopen of e.g. the
                // gdk-pixbuf svg loader (device-evidence: gtk3 SIGABRT) and the base
                // pixbuf loaders. Shared libs need x here even though stock Linux
                // dlopen does not require it.
                if (entry.mode and 0b001_001_001 != 0 || isSharedLibName(target.name)) {
                    target.setExecutable(true, true)
                }
            }
            else -> throw IllegalArgumentException("unsupported tar entry type: ${entry.name}")
        }
    }

    /**
     * Overlay-aware extraction: apply a stage tar ON TOP of an already-installed
     * base rootfs while refusing to downgrade base shared libraries.
     *
     * The base ships libraries under the §5-E flat-SONAME convention: a real file
     * named `libNAME.so.MAJOR` (e.g. the base `libharfbuzz.so.0` is the real
     * harfbuzz 8.3.0 binary). An overlay that repoints such a SONAME to a symlink,
     * or ships a versioned variant (`libNAME.so.MAJOR.MINOR...`) for it, would
     * silently downgrade the base lib — the harfbuzz 8.3.0 -> 6.0.0 regression that
     * broke the GTK stack. Such entries are skipped and the base library is kept.
     *
     * tools/overlay_guard.py enforces the same rule host-side as a build gate
     * before a stage tar is ever shipped to the device.
     */
    fun extractOverlayTar(archive: File, rootfsDir: File): OverlayExtractResult {
        rootfsDir.mkdirs()
        val rootCanonical = rootfsDir.canonicalPath
        val frozen = buildFrozenSonames(rootfsDir)
        var extracted = 0
        val skipped = ArrayList<String>()
        TarArchiveInputStream(archive.inputStream().buffered()).use { tar ->
            while (true) {
                val entry = tar.getNextEntry() ?: break
                validateTarEntry(entry)
                when (val verdict = overlayVerdict(entry, frozen)) {
                    is OverlayVerdict.Skip -> {
                        skipped.add("${entry.name}: ${verdict.reason}")
                        android.util.Log.w("alr-rootfs", "overlay-guard skip ${entry.name}: ${verdict.reason}")
                    }
                    is OverlayVerdict.AllowWithNote -> {
                        android.util.Log.w("alr-rootfs", "overlay-guard note ${entry.name}: ${verdict.note}")
                        extractEntry(tar, entry, rootfsDir, rootCanonical)
                        extracted++
                    }
                    OverlayVerdict.Allow -> {
                        extractEntry(tar, entry, rootfsDir, rootCanonical)
                        extracted++
                    }
                }
            }
        }
        android.util.Log.i("alr-rootfs", "overlay extracted=$extracted skipped=${skipped.size}")
        return OverlayExtractResult(extracted = extracted, skipped = skipped)
    }

    private sealed class OverlayVerdict {
        object Allow : OverlayVerdict()
        data class AllowWithNote(val note: String) : OverlayVerdict()
        data class Skip(val reason: String) : OverlayVerdict()
    }

    private data class SoLib(val stem: String, val soname: String, val version: List<Int>) {
        val isFlat: Boolean get() = version.size == 1
    }

    private fun parseSoLib(basename: String): SoLib? {
        val marker = ".so."
        val idx = basename.indexOf(marker)
        if (idx < 0) return null
        val stem = basename.substring(0, idx + ".so".length)
        val verText = basename.substring(idx + marker.length)
        if (verText.isEmpty()) return null
        val parts = verText.split(".")
        // All components must be plain decimal ints; toIntOrNull also guards the
        // (pathological) overflow case so a weird filename can never crash extraction.
        val version = parts.map { it.toIntOrNull() ?: return null }
        return SoLib(stem, "$stem.${version[0]}", version)
    }

    private fun versionLe(a: List<Int>, b: List<Int>): Boolean {
        val n = maxOf(a.size, b.size)
        for (i in 0 until n) {
            val ai = a.getOrElse(i) { 0 }
            val bi = b.getOrElse(i) { 0 }
            if (ai != bi) return ai < bi
        }
        return true
    }

    private fun versionStr(v: List<Int>): String = v.joinToString(".")

    /** A shared library by name: `libfoo.so`, `libfoo.so.1`, `libpixbufloader_svg.so`. */
    private fun isSharedLibName(name: String): Boolean {
        val base = name.substringAfterLast('/')
        return base.endsWith(".so") || base.contains(".so.")
    }

    private fun isSymlinkPath(file: File): Boolean = try {
        val st = android.system.Os.lstat(file.absolutePath)
        (st.st_mode and android.system.OsConstants.S_IFMT) == android.system.OsConstants.S_IFLNK
    } catch (e: android.system.ErrnoException) {
        false
    }

    /**
     * Scan the installed base rootfs for SONAMEs provided as real library files
     * (the flat-SONAME convention) and record the resolved version per directory-
     * scoped soname. Symlinks are never followed, so only real files freeze a
     * soname and merged-usr dir symlinks are not double-walked.
     */
    private fun buildFrozenSonames(rootfsDir: File): Map<String, List<Int>> {
        val frozen = HashMap<String, List<Int>>()
        val rootPrefix = rootfsDir.absolutePath
        val stack = ArrayDeque<File>()
        stack.addLast(rootfsDir)
        while (stack.isNotEmpty()) {
            val dir = stack.removeLast()
            val children = dir.listFiles() ?: continue
            for (child in children) {
                if (isSymlinkPath(child)) continue
                if (child.isDirectory) {
                    stack.addLast(child)
                    continue
                }
                if (!child.isFile) continue
                val lib = parseSoLib(child.name) ?: continue
                val rel = child.absolutePath.removePrefix(rootPrefix).trimStart('/')
                val parent = rel.substringBeforeLast('/', "")
                val key = "$parent|${lib.soname}"
                val cur = frozen[key]
                if (cur == null || versionLe(cur, lib.version)) {
                    frozen[key] = lib.version
                }
            }
        }
        return frozen
    }

    private fun overlayVerdict(entry: TarArchiveEntry, frozen: Map<String, List<Int>>): OverlayVerdict {
        val name = entry.name.removePrefix("./")
        val baseName = name.substringAfterLast('/')
        val lib = parseSoLib(baseName) ?: return OverlayVerdict.Allow
        val parent = name.substringBeforeLast('/', "")
        val baseVer = frozen["$parent|${lib.soname}"] ?: return OverlayVerdict.Allow
        return when {
            entry.isSymbolicLink -> {
                val targetLib = parseSoLib(entry.linkName.substringAfterLast('/'))
                when {
                    baseVer.size == 1 -> OverlayVerdict.Skip(
                        "symlink over frozen flat SONAME ${lib.soname} -> ${entry.linkName} (base library kept)",
                    )
                    targetLib != null && !versionLe(baseVer, targetLib.version) -> OverlayVerdict.Skip(
                        "symlink downgrade ${lib.soname} -> ${entry.linkName} (${versionStr(targetLib.version)} < base ${versionStr(baseVer)})",
                    )
                    else -> OverlayVerdict.Allow
                }
            }
            !lib.isFlat -> when {
                baseVer.size == 1 -> OverlayVerdict.Skip(
                    "versioned variant ${versionStr(lib.version)} over frozen flat SONAME ${lib.soname} (base library kept)",
                )
                !versionLe(baseVer, lib.version) -> OverlayVerdict.Skip(
                    "versioned downgrade ${lib.soname} ${versionStr(lib.version)} < base ${versionStr(baseVer)}",
                )
                else -> OverlayVerdict.Allow
            }
            else -> OverlayVerdict.AllowWithNote(
                "flat overwrite of base ${lib.soname}; version unverifiable from filename (base ${versionStr(baseVer)})",
            )
        }
    }

    fun writeInstallMarker(markerPath: File) {
        markerPath.parentFile?.mkdirs()
        markerPath.writeText("installed\n")
    }

    fun isExtracted(plan: RootfsInstallPlan): Boolean {
        return plan.markerPath.isFile && File(plan.rootfsDir, "etc/os-release").isFile
    }

    private fun validateTarEntry(entry: TarArchiveEntry) {
        val name = entry.name
        require(name.isNotBlank()) { "empty tar entry name" }
        require(!name.startsWith("/")) { "absolute tar entry path is not allowed: $name" }
        require(name.split('/').none { it == ".." }) { "parent traversal is not allowed: $name" }
        require(!entry.isCharacterDevice && !entry.isBlockDevice && !entry.isFIFO) {
            "device-like tar entry is not allowed: $name"
        }
    }
}
