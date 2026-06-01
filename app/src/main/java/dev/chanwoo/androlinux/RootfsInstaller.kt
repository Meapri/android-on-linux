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
                        if (entry.mode and 0b001_001_001 != 0) {
                            target.setExecutable(true, true)
                        }
                    }
                    else -> throw IllegalArgumentException("unsupported tar entry type: ${entry.name}")
                }
            }
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
