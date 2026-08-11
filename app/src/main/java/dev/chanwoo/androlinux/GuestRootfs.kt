package dev.chanwoo.androlinux

import java.io.File

/**
 * Which tree is THE guest.
 *
 * Three places used to answer this independently -- the probe harness picked a
 * distro by name, the launcher runtime used whatever [RootfsInstaller] had just
 * extracted, and the settings screen printed a hardcoded string -- so the app
 * could run one rootfs, measure another, and describe a third. This is the one
 * answer.
 *
 * Ubuntu 26.04 is the target. 24.04 stays in the list because a device
 * provisioned before the switch still has one and there is no reason to strand
 * it, and the bundled tiny rootfs stays last because it is a PoC tree -- busybox
 * and a hand-built library closure, no dpkg database worth the name -- useful as
 * a fallback and not as a product.
 */
object GuestRootfs {

    /** Most capable first. Names are directory names under `filesDir/rootfs`. */
    val PREFERENCE = listOf("ubuntu-26.04", "ubuntu-24.04", "ubuntu")

    /** What a directory has to contain before we will call it a guest. */
    private fun isUsable(dir: File): Boolean =
        File(dir, "bin/bash").isFile || File(dir, "usr/bin/bash").isFile

    /**
     * The guest tree to use, or null when none of the preferred trees is
     * provisioned. Callers fall back to whatever they were already using --
     * this function deliberately does not invent one, because extracting a
     * rootfs is not something a getter should do.
     */
    fun pick(filesDir: File): Pair<File, String>? {
        val base = File(filesDir, "rootfs")
        for (name in PREFERENCE) {
            val dir = File(base, name)
            if (dir.isDirectory && isUsable(dir)) return dir to name
        }
        return null
    }

    /**
     * Provision the target release when no guest tree exists yet.
     *
     * A fresh device has none: the app bundles a PoC rootfs, and the Ubuntu
     * trees were, until now, put there out of band. That made "install an app"
     * work only on a machine somebody had already prepared by hand, which is
     * not a product. alr can fetch and hash-verify an ubuntu-base image itself,
     * so the app just has to ask.
     *
     * Blocking and slow (a base image plus its unpack); callers run it off the
     * main thread. Returns the picked tree, or null if provisioning failed --
     * the caller then falls back to whatever it had.
     */
    fun ensureProvisioned(
        alr: dev.chanwoo.androlinux.AlrRuntime,
        filesDir: File,
        onStatus: (String) -> Unit = {},
    ): Pair<File, String>? {
        pick(filesDir)?.let { return it }
        val target = PREFERENCE.first()
        onStatus("provisioning $target")
        val r = alr.installDistro(target)
        // The whole transcript, not a tail: provisioning is a long chain
        // (discover, verify, untar, hardlink repair, relativize, boot check)
        // and the line that explains a failure is rarely the last one.
        runCatching {
            File(filesDir, "install-$target.txt")
                .writeText("--- stdout ---\n${r.stdout}\n--- stderr ---\n${r.stderr}")
        }
        if (!r.ok) {
            onStatus(
                "provisioning $target failed (${r.exitCode}): " +
                    (r.stderr + r.stdout).lineSequence()
                        .filter { it.isNotBlank() }.toList().takeLast(24).joinToString("\n  "),
            )
            return null
        }
        return pick(filesDir)
    }

    /** For the settings screen: the release actually running, not a constant. */
    fun describe(filesDir: File): String {
        val picked = pick(filesDir) ?: return "bundled tiny rootfs · arm64"
        val pretty = runCatching {
            File(picked.first, "etc/os-release").readLines()
                .firstOrNull { it.startsWith("PRETTY_NAME=") }
                ?.substringAfter('=')?.trim('"')
        }.getOrNull() ?: picked.second
        val glibc = runCatching {
            File(picked.first, "usr/lib/aarch64-linux-gnu")
                .list { _, n -> n.startsWith("libc.so.6") }
                ?.firstOrNull()
        }.getOrNull()
        return listOfNotNull(pretty, glibc?.let { "glibc" }, "arm64").joinToString(" · ")
    }
}
