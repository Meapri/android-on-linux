package dev.chanwoo.androlinux

import java.io.File

data class RootfsAsset(
    val path: String,
    val sha256: String,
    val sizeBytes: Long,
) {
    init {
        require(isSafeRelativePath(path)) { "asset path must be relative and safe" }
        require(Regex("^[a-fA-F0-9]{64}$").matches(sha256)) { "sha256 must be 64 hex characters" }
        require(sizeBytes >= 0) { "sizeBytes must be non-negative" }
    }
}

data class RootfsManifest(
    val name: String,
    val version: String,
    val assets: List<RootfsAsset>,
) {
    init {
        require(Regex("^[A-Za-z0-9._-]+$").matches(name)) { "name must be safe" }
        require(Regex("^[A-Za-z0-9._-]+$").matches(version)) { "version must be safe" }
        require(assets.isNotEmpty()) { "at least one asset is required" }
    }
}

data class RootfsInstallPlan(
    val rootfsDir: File,
    val markerPath: File,
    val assetDestinations: Map<String, File>,
)

fun buildRootfsInstallPlan(manifest: RootfsManifest, appFilesDir: File): RootfsInstallPlan {
    val rootfsBase = File(appFilesDir, "rootfs")
    val rootfsDir = File(rootfsBase, manifest.name)
    val downloadDir = File(File(File(rootfsBase, ".downloads"), manifest.name), manifest.version)
    val markerPath = File(rootfsDir, ".alr-installed-${manifest.version}")
    val destinations = manifest.assets.associate { asset ->
        asset.path to File(downloadDir, asset.path)
    }
    return RootfsInstallPlan(
        rootfsDir = rootfsDir,
        markerPath = markerPath,
        assetDestinations = destinations,
    )
}

private fun isSafeRelativePath(path: String): Boolean {
    if (path.isBlank() || path.startsWith("/")) return false
    return path.split('/').none { it == ".." || it.isBlank() }
}
