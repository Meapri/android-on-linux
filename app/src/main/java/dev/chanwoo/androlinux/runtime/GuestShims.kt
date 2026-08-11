package dev.chanwoo.androlinux.runtime

import android.util.Log
import java.io.File

/**
 * Guest-side shims for things the distro assumes and Android will not give it.
 *
 * These are rootfs edits, not runtime edits, and they are deliberately staged
 * into a directory of our own that we put FIRST on the guest's PATH -- nothing
 * the distro shipped is overwritten, and removing the directory removes the
 * shim.
 */
object GuestShims {

    private const val TAG = "alr_runtime"

    /** Guest-form path; PATH-prepended for GUI launches. */
    const val SHIM_DIR = "/usr/lib/alr/shim"

    /**
     * bwrap, without the sandbox.
     *
     * WHY THIS EXISTS. Ubuntu 26.04's libgdk_pixbuf links libglycin, so EVERY
     * image gdk-pixbuf decodes -- including the icons GTK loads from its own
     * resources before it draws anything -- is handed to a glycin loader that
     * glycin runs under bubblewrap. On Android that fails at the first step:
     *
     *     bwrap: Can't read /proc/sys/kernel/overflowuid: Permission denied
     *
     * SELinux does not let untrusted_app read that sysctl, and a user namespace
     * is not available to an app either. glycin has no fallback -- with bwrap
     * removed it reports "Could not spawn the following command" and fails the
     * same way -- so GTK aborts:
     *
     *     Gtk:ERROR:gtkiconhelper.c:495: Failed to load image-missing.svg
     *     Bail out!
     *
     * which is SIGABRT before a window ever appears. That is what "sakura and
     * htop just close" was.
     *
     * WHAT THIS GIVES UP, plainly: glycin sandboxes image loaders because
     * decoding untrusted image data is a classic memory-safety target, and this
     * shim runs the loader with the app's own privileges instead of a
     * stripped-down subset. The app is still inside Android's sandbox --
     * untrusted_app_27, app-private storage, no root, the zygote seccomp filter
     * -- so the blast radius is this app's data, not the device. It is a real
     * reduction against upstream's intent all the same, and the only
     * alternative on this platform is that no GTK app runs at all.
     *
     * The option table is bubblewrap's, and unknown options are a hard error
     * rather than a guess: mis-parsing the arity would silently shift which
     * argument we treat as the command, and exec the wrong thing.
     */
    private val BWRAP_SHIM = """
        #!/bin/bash
        # alr: bwrap without the sandbox. See GuestShims.kt for why, and for what
        # this gives up. Options are consumed per bubblewrap's own arity; the
        # first token that is not an option (or its value) is the command.
        set -u
        declare -A ARITY0 ARITY1 ARITY2
        for o in --unshare-all --unshare-user --unshare-user-try --unshare-ipc \
                 --unshare-pid --unshare-net --unshare-uts --unshare-cgroup \
                 --unshare-cgroup-try --share-net --clearenv --new-session \
                 --die-with-parent --as-pid-1 --disable-userns \
                 --assert-userns-disabled --level-prefix --help --version; do
            ARITY0[${'$'}o]=1
        done
        for o in --args --argv0 --fd --userns --userns2 --pidns --uid --gid \
                 --hostname --chdir --unsetenv --lock-file --sync-fd --remount-ro \
                 --exec-label --file-label --proc --dev --tmpfs --mqueue --dir \
                 --seccomp --add-seccomp-fd --block-fd --userns-block-fd \
                 --info-fd --json-status-fd --cap-add --cap-drop --perms --size \
                 --chmod --overlay-src; do
            ARITY1[${'$'}o]=1
        done
        for o in --bind --bind-try --dev-bind --dev-bind-try --ro-bind \
                 --ro-bind-try --file --bind-data --ro-bind-data --symlink \
                 --setenv; do
            ARITY2[${'$'}o]=1
        done

        chdir=""
        declare -a setenv_k setenv_v
        while [ ${'$'}# -gt 0 ]; do
            a=${'$'}1
            case ${'$'}a in
              --*)
                if [ -n "${'$'}{ARITY0[${'$'}a]:-}" ]; then shift; continue; fi
                if [ -n "${'$'}{ARITY1[${'$'}a]:-}" ]; then
                    # --chdir is the one 1-arg option whose effect we DO keep:
                    # the loader resolves relative paths against it.
                    [ "${'$'}a" = --chdir ] && chdir=${'$'}2
                    shift 2; continue
                fi
                if [ -n "${'$'}{ARITY2[${'$'}a]:-}" ]; then
                    # --setenv is kept too; the mount options are exactly what a
                    # shim cannot honour and exactly what we are dropping.
                    if [ "${'$'}a" = --setenv ]; then
                        setenv_k+=("${'$'}2"); setenv_v+=("${'$'}3")
                    fi
                    shift 3; continue
                fi
                echo "alr bwrap shim: unknown option ${'$'}a" >&2
                exit 125
                ;;
              *) break ;;
            esac
        done
        [ ${'$'}# -gt 0 ] || { echo "alr bwrap shim: no command" >&2; exit 125; }
        # NOT --clearenv: the loader is a dynamic guest binary and needs the
        # environment alr launched the session with (ALR_ROOT, the loader path,
        # the preload) or it cannot start at all. glycin's own --setenv values
        # are applied on top, which is the part that carries meaning.
        i=0
        while [ ${'$'}i -lt ${'$'}{#setenv_k[@]} ]; do
            export "${'$'}{setenv_k[${'$'}i]}=${'$'}{setenv_v[${'$'}i]}"
            i=${'$'}((i + 1))
        done
        [ -n "${'$'}chdir" ] && cd "${'$'}chdir" 2>/dev/null
        exec "${'$'}@"
    """.trimIndent() + "\n"

    /**
     * Stage the shims into [rootfsDir]. Idempotent: rewritten only when the
     * content differs, so an app update refreshes it and a normal launch does
     * not touch the filesystem. Returns true when the shim dir is usable.
     */
    fun install(rootfsDir: File): Boolean = runCatching {
        val dir = File(rootfsDir, SHIM_DIR.removePrefix("/"))
        dir.mkdirs()
        val bwrap = File(dir, "bwrap")
        val want = BWRAP_SHIM.toByteArray()
        if (!bwrap.isFile || !bwrap.readBytes().contentEquals(want)) {
            bwrap.writeBytes(want)
            Log.i(TAG, "staged bwrap shim at ${bwrap.absolutePath}")
        }
        bwrap.setExecutable(true, false)
        bwrap.canExecute()
    }.getOrElse {
        Log.w(TAG, "bwrap shim staging failed: ${it.message}")
        false
    }
}
