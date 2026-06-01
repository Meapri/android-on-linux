package dev.chanwoo.androlinux

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.inputmethod.InputMethodManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.net.InetAddress
import java.net.ServerSocket
import java.net.SocketTimeoutException
import kotlin.concurrent.thread

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        System.loadLibrary("alr_loader")

        val rootfsManifest = RootfsManifest(
            name = "debian-arm64",
            version = "bookworm-slim-2026-05-gui-gpu-v40",
            assets = listOf(
                RootfsAsset(
                    path = "rootfs.tar.zst",
                    sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
                    sizeBytes = 0,
                ),
            ),
        )
        val rootfsPlan = buildRootfsInstallPlan(rootfsManifest, filesDir)
        val rootfsStatus = RootfsInstaller(this).prepareBundledTinyRootfs()
        // Goal-2 Phase B: stage Chromium (bookworm chromium-headless-shell + deps) into
        // the rootfs as a one-time overlay, then boot it IN-PROCESS through the ALR
        // native loader — headless, single-process, no sandbox (sidesteps the 6 gaps).
        // The 517MB tar is adb-push'd to /data/local/tmp (app-readable). Heavy (extract +
        // Chromium boot) → off the UI thread. Verified via logcat tag alr_loader.
        // Chromium DEFERRED (user: 크로미움 보류). Critically, chromium-stage.tar also
        // ships libharfbuzz.so.0.60000.0 (6.0.0) which OVERWROTE the rootfs's matched
        // harfbuzz 8.3.0 → pango 1.52.1 lost `hb_ot_color_has_paint` → all GTK apps +
        // GIMP exit=127. Keeping this overlay off preserves the rootfs's 8.3.0 stack
        // (and avoids re-extracting the 517MB tar on every cold start).
        @Suppress("ConstantConditionIf")
        if (false) Thread {
            try {
                val crTar = java.io.File("/data/local/tmp/chromium-stage.tar")
                // Marker keyed on tar size so re-pushing a fixed/updated stage tar
                // (e.g. new dep symlinks) auto-triggers a fresh overlay extract.
                val crMarker = java.io.File(rootfsStatus.rootfsDir, ".chromium-staged-${crTar.length()}")
                if (crTar.isFile && !crMarker.isFile) {
                    android.util.Log.i("alr_loader", "chromium-stage: extracting overlay (${crTar.length()} bytes)")
                    // Guarded extract (WS-4 M1): extractOverlayTar skips entries that would
                    // downgrade a base lib — incl. the harfbuzz 8.3.0->6.0.0 symlink documented
                    // above (and latent liblcms2/libopenjp2). With the guard the overlay can no
                    // longer break the GTK stack; it stays gated off here only per the user hold.
                    val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(crTar, rootfsStatus.rootfsDir)
                    crMarker.writeText("staged\n")
                    android.util.Log.i("alr_loader", "chromium-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "chromium-stage: guard skipped downgrades:\n${ovr.skipped.joinToString("\n")}")
                }
                val crReport = nativeAlrNativeLoaderProbe(
                    packageName,
                    applicationInfo.nativeLibraryDir,
                    filesDir.absolutePath,
                    cacheDir.absolutePath,
                    rootfsManifest.name,
                    // --version: reliable (single/few threads). The heavy --dump-dom render
                    // path still hits a non-deterministic multithread ptrace deadlock (boots
                    // to a 1-thread t-stop); the group-stop hardening below didn't fully fix
                    // it — PTRACE_SEIZE + EVENT_STOP/LISTEN is the deeper fix (backlogged).
                    "/usr/lib/chromium/chromium-headless-shell\n--no-sandbox\n--version",
                )
                android.util.Log.i("alr_loader", "chromium-boot:\n$crReport")
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "chromium-boot EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
        // Goal-2 universality: stage `foot` (a Wayland-native terminal, ~1-2 threads —
        // it clears the multithread ptrace wall that blocks Chromium's render) as a
        // one-time ~1.5MB overlay. foot binds wl_compositor/wl_subcompositor/wl_shm/
        // xdg_wm_base (all advertised) and CPU-renders glyphs into wl_shm, exactly the
        // path the WaylandPresenter already uploads to the SurfaceView for GIMP. The
        // launch happens later on the compositor (in the GUI thread, before GIMP).
        Thread {
            try {
                val footTar = java.io.File("/data/local/tmp/foot-stage.tar")
                val footMarker = java.io.File(rootfsStatus.rootfsDir, ".foot-staged-${footTar.length()}")
                if (footTar.isFile && !footMarker.isFile) {
                    android.util.Log.i("alr_loader", "foot-stage: extracting overlay (${footTar.length()} bytes)")
                    // Guarded extract (WS-4 M1): keep base libs if this overlay ships an
                    // older variant of a base SONAME (foot's deps overlap the GTK stack).
                    val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(footTar, rootfsStatus.rootfsDir)
                    footMarker.writeText("staged\n")
                    android.util.Log.i("alr_loader", "foot-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "foot-stage: guard skipped downgrades:\n${ovr.skipped.joinToString("\n")}")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "foot-stage EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
        // Goal-2 universality: also stage gtk3-widget-factory / gtk3-demo (lightweight
        // GTK3 demo apps, pty-free) as a ~3.6MB overlay. Their entire DT_NEEDED closure
        // is the GTK3 set the rootfs already ships for GIMP — only the demo binaries are
        // new. A second, DIFFERENT lightweight app rendering on the same compositor
        // broadens the "universal native GUI" proof beyond foot (and is the pty-free
        // safety net should foot's terminal pty be blocked under untrusted_app).
        Thread {
            try {
                val gtkDemoTar = java.io.File("/data/local/tmp/gtk3demo-stage.tar")
                val gtkDemoMarker = java.io.File(rootfsStatus.rootfsDir, ".gtk3demo-staged-${gtkDemoTar.length()}")
                if (gtkDemoTar.isFile && !gtkDemoMarker.isFile) {
                    android.util.Log.i("alr_loader", "gtk3demo-stage: extracting overlay (${gtkDemoTar.length()} bytes)")
                    // Guarded extract (WS-4 M1): the gtk3demo closure overlaps the base GTK3
                    // stack — keep base libs if the overlay ships an older SONAME variant.
                    val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(gtkDemoTar, rootfsStatus.rootfsDir)
                    gtkDemoMarker.writeText("staged\n")
                    android.util.Log.i("alr_loader", "gtk3demo-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "gtk3demo-stage: guard skipped downgrades:\n${ovr.skipped.joinToString("\n")}")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "gtk3demo-stage EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
        // CP-1 GUI completeness overlay (optional). NOTE (WS-4 verified): the base
        // rootfs ALREADY ships the full xkb tree (rules/evdev, keycodes/evdev,
        // symbols/{pc,us,inet}, …) and libxkbcommon — verified by tools/xkb_probe —
        // and the keymap SIGSEGV was fixed by WS-1's XKB_CONFIG_ROOT (v127, device
        // gtk3-widget-factory render). babl/gegl + 37 gegl-0.4 ops are also already
        // in the base. So this slot is for any RESIDUAL completeness bits (e.g. a
        // C.UTF-8 locale-archive, extra gimp-3.0/gegl ops) staged as needed. Applied
        // through the guarded extractOverlayTar so it can never downgrade a base lib.
        Thread {
            try {
                val xgTar = java.io.File("/data/local/tmp/xkb-gegl-stage.tar")
                val xgMarker = java.io.File(rootfsStatus.rootfsDir, ".xkbgegl-staged-${xgTar.length()}")
                if (xgTar.isFile && !xgMarker.isFile) {
                    android.util.Log.i("alr_loader", "xkb-gegl-stage: extracting overlay (${xgTar.length()} bytes)")
                    val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(xgTar, rootfsStatus.rootfsDir)
                    xgMarker.writeText("staged\n")
                    android.util.Log.i("alr_loader", "xkb-gegl-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "xkb-gegl-stage: guard skipped downgrades:\n${ovr.skipped.joinToString("\n")}")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "xkb-gegl-stage EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
        // M2 toolkit overlays (WS-4): SDL2 + netsurf-gtk stage tars, built host-side
        // by tools/deb_closure (base-subtracted, §5-E flat-SONAME). Guarded extract,
        // gated on the tar being adb-push'd to /data/local/tmp. Launch-on-compositor
        // is wired separately. Device test = CP-5.
        Thread {
            try {
                for (name in listOf("sdl2", "netsurf", "microbench", "interpose", "dpkg-db", "x11", "apt-config")) {
                    val tar = java.io.File("/data/local/tmp/$name-stage.tar")
                    val marker = java.io.File(rootfsStatus.rootfsDir, ".$name-staged-${tar.length()}")
                    if (tar.isFile && !marker.isFile) {
                        android.util.Log.i("alr_loader", "$name-stage: extracting overlay (${tar.length()} bytes)")
                        val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(tar, rootfsStatus.rootfsDir)
                        marker.writeText("staged\n")
                        android.util.Log.i("alr_loader", "$name-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                        if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "$name-stage: guard skipped downgrades:\n${ovr.skipped.joinToString("\n")}")
                    }
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "toolkit-stage EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
        // CP-2 GPU: stage the guest GPU shim(libEGL.so.1/libGLESv2.so.2 → /usr/lib/androlinux)
        // + glmark2-es2-wayland. The shim emits GL into the GpuRingHook ring (WS-2 CP-0);
        // the loader attaches it for the glmark2 guest → host Mali executor replays = real
        // GPU accel. Guarded extract (WS-4) — shim is private-dir so it never downgrades base.
        Thread {
            try {
                val gpushimTar = java.io.File("/data/local/tmp/gpushim-stage.tar")
                val gpushimMarker = java.io.File(rootfsStatus.rootfsDir, ".gpushim-staged-${gpushimTar.length()}")
                if (gpushimTar.isFile && !gpushimMarker.isFile) {
                    val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(gpushimTar, rootfsStatus.rootfsDir)
                    gpushimMarker.writeText("staged\n")
                    android.util.Log.i("alr_loader", "gpushim-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                }
                val glmarkTar = java.io.File("/data/local/tmp/glmark2-stage.tar")
                val glmarkMarker = java.io.File(rootfsStatus.rootfsDir, ".glmark2-staged-${glmarkTar.length()}")
                if (glmarkTar.isFile && !glmarkMarker.isFile) {
                    val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(glmarkTar, rootfsStatus.rootfsDir)
                    glmarkMarker.writeText("staged\n")
                    android.util.Log.i("alr_loader", "glmark2-stage: overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "glmark2-stage: guard skipped:\n${ovr.skipped.joinToString("\n")}")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "gpu-stage EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
        // WS-4 §10(b): apt/dpkg/X11 FUNCTIONAL probes. The dpkg-db/x11/apt-config overlays
        // already stage above; until now their only proof on device was the exec-summary
        // TextView (never logcat). This actually RUNS dpkg-query/apt-get/Xwayland through the
        // ALR native loader and emits version/exit/summary to logcat tag alr_loader, so the
        // integration device drain can capture functional evidence (not just "staged").
        launchPackageManagerProbes(rootfsStatus.rootfsDir, rootfsManifest.name)
        val nativeCommandRunner = NativeCommandRunner(
            File(applicationInfo.nativeLibraryDir),
            File(cacheDir, "proot-tmp"),
        )
        val nativeCommandResult = nativeCommandRunner.runSmokeTest()
        val prootCandidateResult = nativeCommandRunner.runProotCandidateSmokeTest()
        val prootShortVersionResult = nativeCommandRunner.runProotShortVersionProbe()
        val prootHelpResult = nativeCommandRunner.runProotHelpProbe()
        val prootNoEnvResult = nativeCommandRunner.runProotNoEnvVersionProbe()
        val prootViaLinkerResult = nativeCommandRunner.runProotViaLinkerVersionProbe()
        val prootHelloResult = nativeCommandRunner.runProotRootfsProgram(rootfsStatus.rootfsDir, "/bin/hello")
        val prootScriptResult = nativeCommandRunner.runProotRootfsProgram(rootfsStatus.rootfsDir, "/bin/script-hello")
        val prootShellResult = nativeCommandRunner.runProotRootfsShell(
            rootfsStatus.rootfsDir,
            "echo shell-c ok; /bin/hello; /bin/cat /etc/os-release",
        )
        val prootGlibcResult = nativeCommandRunner.runProotRootfsProgram(rootfsStatus.rootfsDir, "/bin/glibc-hello")
        val prootDashResult = nativeCommandRunner.runProotRootfsDash(
            rootfsStatus.rootfsDir,
            "echo dash-c ok; /usr/bin/env | /bin/cat",
        )
        val prootIdResult = nativeCommandRunner.runProotRootfsIdAsRoot(rootfsStatus.rootfsDir)
        val prootDpkgVersionResult = nativeCommandRunner.runProotRootfsDpkgVersion(rootfsStatus.rootfsDir)
        val prootDpkgArchResult = nativeCommandRunner.runProotRootfsDpkgPrintArchitecture(rootfsStatus.rootfsDir)
        val prootDpkgQueryVersionResult = nativeCommandRunner.runProotRootfsDpkgQueryVersion(rootfsStatus.rootfsDir)
        val prootDpkgSplitVersionResult = nativeCommandRunner.runProotRootfsDpkgSplitVersion(rootfsStatus.rootfsDir)
        val prootAptVersionResult = nativeCommandRunner.runProotRootfsAptVersion(rootfsStatus.rootfsDir)
        val prootAptGetVersionResult = nativeCommandRunner.runProotRootfsAptGetVersion(rootfsStatus.rootfsDir)
        val prootAptCacheVersionResult = nativeCommandRunner.runProotRootfsAptCacheVersion(rootfsStatus.rootfsDir)
        val prootAptConfigVersionResult = nativeCommandRunner.runProotRootfsAptConfigVersion(rootfsStatus.rootfsDir)
        val prootDpkgInstallLocalResult = nativeCommandRunner.runProotRootfsDpkgInstallLocalSmoke(rootfsStatus.rootfsDir)
        val prootInstalledPackageSmokeResult = nativeCommandRunner.runProotRootfsInstalledPackageSmoke(rootfsStatus.rootfsDir)
        val prootGuestGpuClientResult = nativeCommandRunner.runProotRootfsGuestGpuClient(rootfsStatus.rootfsDir)
        val guestGpuCommands = parseGuestGpuCommands(prootGuestGpuClientResult.stdout)
        val guestGpuIpcBridgeResult = runGuestGpuIpcBridge(nativeCommandRunner, rootfsStatus.rootfsDir)
        val prootGuestGlesShimSmokeResult = nativeCommandRunner.runProotRootfsGuestGlesShimSmoke(rootfsStatus.rootfsDir)
        val guestGlesShimCommand = parseGuestGlesShimCommand(prootGuestGlesShimSmokeResult.stdout)
        val prootGuestWaylandGuiResult = nativeCommandRunner.runProotRootfsGuestGuiClient(rootfsStatus.rootfsDir, "WAYLAND")
        val prootGuestX11GuiResult = nativeCommandRunner.runProotRootfsGuestGuiClient(rootfsStatus.rootfsDir, "X11")
        val guestWaylandGuiBridgeResult = runGuestGuiBridge(nativeCommandRunner, rootfsStatus.rootfsDir, "WAYLAND")
        val guestX11GuiBridgeResult = runGuestGuiBridge(nativeCommandRunner, rootfsStatus.rootfsDir, "X11")
        val guestGuiSurfaceCommands = guestWaylandGuiBridgeResult.commands + guestX11GuiBridgeResult.commands
        val surfaceGpuCommands = when {
            guestGuiSurfaceCommands.isNotEmpty() -> guestGuiSurfaceCommands
            guestGpuIpcBridgeResult.commands.isNotEmpty() -> guestGpuIpcBridgeResult.commands
            guestGpuCommands.isNotEmpty() -> guestGpuCommands
            guestGlesShimCommand != null -> listOf(guestGlesShimCommand)
            else -> listOf(GuestGpuCommand(0.05f, 0.18f, 0.45f, "host-default"))
        }
        val prootHelloVerboseResult = if (prootHelloResult.exitCode == 0) {
            null
        } else {
            nativeCommandRunner.runProotRootfsProgramVerbose(rootfsStatus.rootfsDir, "/bin/hello")
        }
        val nativeProbe = nativeLibraryProbe(applicationInfo.nativeLibraryDir)
        val alrTrampolineContinueProbe = nativeAlrTrampolineContinueProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrProcfsVirtualizationProbe = nativeAlrProcfsVirtualizationProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrWxSafeExecProbe = nativeAlrWxSafeExecProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrPerfComparisonProbe = nativeAlrPerfComparisonProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrInterposeProcfsProbe = nativeAlrInterposeProcfsProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrMemfdExecProbe = nativeAlrMemfdExecProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrSyscallSandboxProbe = nativeAlrSyscallSandboxProbe()
        val alrSeccompPathTrapProbe = nativeAlrSeccompPathTrapProbe(
            filesDir.absolutePath,
            cacheDir.absolutePath,
        )
        val alrUnixSocketProbe = nativeAlrUnixSocketProbe(
            filesDir.absolutePath,
            cacheDir.absolutePath,
        )
        val alrUnixSocketViable =
            alrUnixSocketProbe.lineStartingWith("alr us WAYLAND_TRANSPORT_VIABLE=")
                .substringAfter("VIABLE=", "") == "yes"
        val alrExecmemProbe = nativeAlrExecmemProbe()
        val alrNativeLoaderProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        )
        val alrNativeLoaderGuestExecPassed =
            alrNativeLoaderProbe.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:") ==
                "ALR NATIVE LOADER GUEST EXEC: PASS"
        // Multi-process generalization: a static glibc binary that spawns a pthread
        // and fork()s a child, to exercise the multi-tracee SIGSYS supervisor.
        val alrNativeLoaderMtProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/mt-test",
        )
        val alrNativeLoaderMtPassed =
            alrNativeLoaderMtProbe.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:") ==
                "ALR NATIVE LOADER GUEST EXEC: PASS"
        // A static-glibc program that opens /etc/alr-probe.txt: with loader path
        // mediation the guest's openat("/etc/...") is rewritten into the rootfs, so
        // it reads the fixture "alr-rootfs-ok" instead of failing against Android's
        // /etc. This is the end-to-end proof that real file-using programs work.
        val alrNativeLoaderFileioProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/fileio-test",
        )
        val alrNativeLoaderFileioMediated =
            alrNativeLoaderFileioProbe.contains("alr-rootfs-ok")
        // A DYNAMICALLY-linked glibc program: the loader loads the guest's own
        // ld-linux-aarch64.so.1 and hands control to it; the interpreter links
        // libc.so.6 from the rootfs and runs the program. Success = stdout
        // "alr-dyn-ok" with LINK MODE DYNAMIC.
        val alrNativeLoaderDynProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/dynhello",
        )
        val alrNativeLoaderDynPassed =
            alrNativeLoaderDynProbe.contains("alr-dyn-ok")
        // REAL Debian programs (not our test binaries), run through the same
        // dynamic loader + path mediation: /usr/bin/env prints the environment
        // (needs only libc.so.6); /usr/bin/id prints uid/gid (links libselinux and
        // reads /etc/passwd,/etc/group via the rootfs — exercises path mediation).
        val alrLoaderRealEnvProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/usr/bin/env",
        )
        val alrLoaderRealEnvPassed =
            alrLoaderRealEnvProbe.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:") ==
                "ALR NATIVE LOADER GUEST EXEC: PASS"
        val alrLoaderRealIdProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/usr/bin/id",
        )
        val alrLoaderRealIdPassed = alrLoaderRealIdProbe.contains("uid=")
        // A real interactive shell running a command: /bin/dash -c 'echo alr-shell-ok'.
        // The program spec is newline-delimited argv (path, then each arg), so the
        // loader builds argc=3 and dash runs the -c command. echo is a dash builtin
        // (no child exec needed) — this proves argv passing + a real shell.
        val alrLoaderDashProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/dash\n-c\necho alr-shell-ok",
        )
        val alrLoaderDashPassed = alrLoaderDashProbe.contains("alr-shell-ok")
        // Write-path mediation: dash creates /tmp/alrwp (openat O_CREAT, trapped and
        // rewritten into the rootfs) then reads it back via a redirect — all dash
        // builtins (no child exec). Proves guest writes land in the rootfs.
        val alrLoaderWriteProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/dash\n-c\necho wp-ok > /tmp/alrwp; read x < /tmp/alrwp; echo got:\$x",
        )
        val alrLoaderWritePassed = alrLoaderWriteProbe.contains("got:wp-ok")
        // In-process image decode: a gdk-pixbuf client decodes a PNG from memory +
        // from a file, and lists known formats. This is the prerequisite for GIMP
        // image loading and GTK icons. No display needed (synchronous).
        val alrLoaderPngProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/alr-png-test",
        )
        val alrLoaderPngDecoded = alrLoaderPngProbe.contains("alr-png: png decode OK")
        // Phase 6 final target: GIMP. Rung 1 — headless `gimp-console-3.0 --version`
        // proves GIMP's binary + its ~112-library closure (libgimp*, gegl, babl,
        // codecs) loads and runs in-process via the ALR loader. No display needed.
        val alrLoaderGimpProbe = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/usr/bin/gimp-console-3.0\n--version",
        )
        val alrLoaderGimpVersion =
            alrLoaderGimpProbe.contains("GNU Image Manipulation Program") ||
                Regex("GIMP.*version 3").containsMatchIn(alrLoaderGimpProbe) ||
                alrLoaderGimpProbe.contains("version 3.0")
        // WS-1 / CP-3: run the SAME microbench binary the device-native baseline uses
        // (the static arm64 microbench, staged by WS-4 at /usr/bin/microbench) THROUGH
        // the ALR loader, once per workload. Each emits its own
        // "MICROBENCH mode=<m> ... ns_per_op=<x>" line on guest stdout, which
        // build_native_loader_probe lifts into logcat (tag alr_loader) as
        // "alr-microbench: ... MICROBENCH ...". The integration drain diffs those
        // ns_per_op against the native baseline for a true apples-to-apples CP-3
        // overhead % (replacing the ALR-getppid≈218ns proxy). The `compute` arm is the
        // syscall-light gated (<5%) case; `syscall` is the syscall-storm reported case.
        // argv is newline-delimited (path, then each arg), matching the dash probes.
        val alrLoaderMicrobenchCompute = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/usr/bin/microbench\ncompute",
        )
        val alrLoaderMicrobenchComputeRan =
            alrLoaderMicrobenchCompute.contains("MICROBENCH mode=compute")
        val alrLoaderMicrobenchSyscall = nativeAlrNativeLoaderProbe(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/usr/bin/microbench\nsyscall",
        )
        val alrLoaderMicrobenchSyscallRan =
            alrLoaderMicrobenchSyscall.contains("MICROBENCH mode=syscall")
        val alrNativeLoaderSelftest = nativeAlrNativeLoaderSelftest()
        val alrNativeLoaderSelftestPassed =
            alrNativeLoaderSelftest.lineStartingWith("ALR NATIVE LOADER SELFTEST EXEC:") ==
                "ALR NATIVE LOADER SELFTEST EXEC: PASS"
        val alrGpuBoundaryProbe = nativeAlrGpuBoundaryProbe()
        val gpuBoundaryShmemCmdsPerFrame =
            alrGpuBoundaryProbe.lineStartingWith("alr gpu boundary shmem-ring cmds per 60fps frame=")
                .substringAfter("frame=", "0").toLongOrNull() ?: 0L
        val gpuBoundaryInprocCmdsPerFrame =
            alrGpuBoundaryProbe.lineStartingWith("alr gpu boundary inproc cmds per 60fps frame=")
                .substringAfter("frame=", "0").toLongOrNull() ?: 0L
        // A draw-call-heavy 60fps frame is ~2000 calls; require the realistic
        // (shared-ring) path to clear that with headroom to call the GPU boundary
        // viable for low-overhead passthrough.
        val gpuPassthroughBoundaryViable = gpuBoundaryShmemCmdsPerFrame >= 20000L
        val alrGpuMarshallingProbe = nativeAlrGpuMarshallingProbe()
        val alrGpuMarshallingPassed =
            alrGpuMarshallingProbe.lineStartingWith("ALR GPU MARSHALLING HARDWARE RENDER:") ==
                "ALR GPU MARSHALLING HARDWARE RENDER: PASS"
        // AHardwareBuffer zero-copy display probe (GEGL-GPU design Option C / M1): the
        // public-API path that lets the compositor sample a guest frame as a GL texture
        // with no per-frame CPU->GPU copy (AHB -> EGLImage -> GL_TEXTURE_EXTERNAL_OES).
        val alrAhbZeroCopyProbe = nativeAlrAhbZeroCopyProbe()
        val alrAhbZeroCopyPassed =
            alrAhbZeroCopyProbe.lineStartingWith("ALR AHB ZEROCOPY HARDWARE SAMPLE:") ==
                "ALR AHB ZEROCOPY HARDWARE SAMPLE: PASS"
        // GPU-native app track (Phase 4): M1 decodes a real shader+VBO+texture+draw
        // op stream on Mali; M2 carries the same stream through the SPSC command
        // ring then decodes it. Proves the GLES marshalling layer that lets a guest
        // drive the real GPU (the path a future libGLESv2 shim feeds).
        val alrGpuDrawProbe = nativeAlrGpuDrawProbe()
        val alrGpuDrawPassed =
            alrGpuDrawProbe.lineStartingWith("ALR GPU DRAW HARDWARE RENDER:") ==
                "ALR GPU DRAW HARDWARE RENDER: PASS"
        val alrGpuRingProbe = nativeAlrGpuRingProbe()
        val alrGpuRingPassed =
            alrGpuRingProbe.lineStartingWith("ALR GPU RING HARDWARE RENDER:") ==
                "ALR GPU RING HARDWARE RENDER: PASS"
        // M4 host half: render the decoded guest draw into an AHardwareBuffer-backed
        // FBO, then sample that AHB zero-copy (external-OES) — the guest-draw -> AHB
        // -> presentable loop the spinning-cube display will use.
        val alrGpuFboProbe = nativeAlrGpuFboProbe()
        val alrGpuFboPassed =
            alrGpuFboProbe.lineStartingWith("ALR GPU AHB RENDER (guest draw landed in AHB, direct read):") ==
                "ALR GPU AHB RENDER (guest draw landed in AHB, direct read): PASS"
        // M4 LIVE INTEGRATION keystone: producer thread streams the guest op stream
        // through the SPSC ring; the host executor thread (own Mali GLES2 ctx + AHB-FBO)
        // drains+decodes+presents 8 frames, synced by req_seq/reply_seq. Proves the live
        // ring -> executor -> AHB loop on Mali (only the loader fork remains after this).
        val alrGpuLiveProbe = nativeAlrGpuLiveProbe()
        val alrGpuLivePassed =
            alrGpuLiveProbe.lineStartingWith("ALR GPU LIVE INTEGRATION:") ==
                "ALR GPU LIVE INTEGRATION: PASS"
        // GPU 가속 정량: 같은 op-스트림을 같은 Mali에서 DIRECT(ring 없음) vs ALR(ring+executor)로
        // 렌더해 FPS 비율 = native-Mali 대비 ALR GPU 마샬링 효율(§0(b)). logcat tag alr_loader.
        val alrGpuThroughputProbe = nativeAlrGpuThroughputProbe()
        val alrGpuThroughputPassed =
            alrGpuThroughputProbe.lineStartingWith("ALR GPU THROUGHPUT:") ==
                "ALR GPU THROUGHPUT: PASS"
        // Goal-2 (Chromium) prep: V8-style iterative W^X executable memory. PASS => V8
        // JIT runs without --jitless on this untrusted_app domain.
        val jitWxProbe = nativeJitWxProbe()
        val jitWxPassed =
            jitWxProbe.lineStartingWith("ALR JIT WX CYCLE:") == "ALR JIT WX CYCLE: PASS"
        val hostGpuProbe = nativeHostGpuProbe()
        val hostVulkanProbe = nativeHostVulkanProbe()
        val requestedPermissions = requestedPermissionNames()
        val internetPermissionDeclared = Manifest.permission.INTERNET in requestedPermissions
        val networkStatePermissionDeclared = Manifest.permission.ACCESS_NETWORK_STATE in requestedPermissions
        val broadStoragePermissionDeclared = "android.permission.MANAGE_EXTERNAL_STORAGE" in requestedPermissions
        val rootfsHelloFile = File(rootfsStatus.rootfsDir, "bin/hello")
        val rootfsShellFile = File(rootfsStatus.rootfsDir, "bin/sh")
        val rootfsCatFile = File(rootfsStatus.rootfsDir, "bin/cat")
        val rootfsScriptFile = File(rootfsStatus.rootfsDir, "bin/script-hello")
        val rootfsGlibcHelloFile = File(rootfsStatus.rootfsDir, "bin/glibc-hello")
        val rootfsGlibcLoaderFile = File(rootfsStatus.rootfsDir, "lib/ld-linux-aarch64.so.1")
        val rootfsLibcFile = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libc.so.6")
        val rootfsDashFile = File(rootfsStatus.rootfsDir, "bin/dash")
        val rootfsEnvFile = File(rootfsStatus.rootfsDir, "usr/bin/env")
        val rootfsPasswdFile = File(rootfsStatus.rootfsDir, "etc/passwd")
        val rootfsGroupFile = File(rootfsStatus.rootfsDir, "etc/group")
        val rootfsNsswitchFile = File(rootfsStatus.rootfsDir, "etc/nsswitch.conf")
        val rootfsIdFile = File(rootfsStatus.rootfsDir, "usr/bin/id")
        val rootfsLibselinuxFile = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libselinux.so.1")
        val rootfsLibpcre2File = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libpcre2-8.so.0")
        val rootfsLibnssFilesFile = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libnss_files.so.2")
        val rootfsDpkgFile = File(rootfsStatus.rootfsDir, "usr/bin/dpkg")
        val rootfsLibmdFile = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libmd.so.0")
        val rootfsDpkgConfigDir = File(rootfsStatus.rootfsDir, "etc/dpkg/dpkg.cfg.d")
        val rootfsDpkgConfigFile = File(rootfsStatus.rootfsDir, "etc/dpkg/dpkg.cfg")
        val rootfsNeedrestartConfigFile = File(rootfsStatus.rootfsDir, "etc/dpkg/dpkg.cfg.d/needrestart")
        val rootfsAndrolinuxDpkgConfigFile = File(rootfsStatus.rootfsDir, "etc/dpkg/dpkg.cfg.d/00-androlinux-minimal")
        val rootfsDpkgQueryFile = File(rootfsStatus.rootfsDir, "usr/bin/dpkg-query")
        val rootfsDpkgCpuTableFile = File(rootfsStatus.rootfsDir, "usr/share/dpkg/cputable")
        val rootfsDpkgTupleTableFile = File(rootfsStatus.rootfsDir, "usr/share/dpkg/tupletable")
        val rootfsAptFile = File(rootfsStatus.rootfsDir, "usr/bin/apt")
        val rootfsAptGetFile = File(rootfsStatus.rootfsDir, "usr/bin/apt-get")
        val rootfsAptCacheFile = File(rootfsStatus.rootfsDir, "usr/bin/apt-cache")
        val rootfsAptConfigFile = File(rootfsStatus.rootfsDir, "usr/bin/apt-config")
        val rootfsLibAptPkgFile = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libapt-pkg.so.6.0")
        val rootfsLibAptPrivateFile = File(rootfsStatus.rootfsDir, "lib/aarch64-linux-gnu/libapt-private.so.0.0")
        val rootfsAptHttpMethodFile = File(rootfsStatus.rootfsDir, "usr/lib/apt/methods/http")
        val rootfsAptListsPartialDir = File(rootfsStatus.rootfsDir, "var/lib/apt/lists/partial")
        val rootfsLocalDebFile = File(rootfsStatus.rootfsDir, "var/cache/apt/archives/alr-smoke_1.0_arm64.deb")
        val rootfsDpkgDebFile = File(rootfsStatus.rootfsDir, "usr/bin/dpkg-deb")
        val rootfsInstalledSmokeFile = File(rootfsStatus.rootfsDir, "usr/local/bin/alr-package-smoke")
        val rootfsDpkgStatusFile = File(rootfsStatus.rootfsDir, "var/lib/dpkg/status")
        val rootfsDevNullFile = File(rootfsStatus.rootfsDir, "dev/null")
        val rootfsDpkgTriggersFile = File(rootfsStatus.rootfsDir, "var/lib/dpkg/triggers/File")
        val rootfsDpkgTriggersUnincorpFile = File(rootfsStatus.rootfsDir, "var/lib/dpkg/triggers/Unincorp")
        val rootfsRmFile = File(rootfsStatus.rootfsDir, "usr/bin/rm")
        val rootfsTarFile = File(rootfsStatus.rootfsDir, "usr/bin/tar")
        val rootfsDiffFile = File(rootfsStatus.rootfsDir, "usr/bin/diff")
        val rootfsLdconfigFile = File(rootfsStatus.rootfsDir, "usr/sbin/ldconfig")
        val rootfsLdconfigRealFile = File(rootfsStatus.rootfsDir, "sbin/ldconfig.real")
        val rootfsStartStopDaemonFile = File(rootfsStatus.rootfsDir, "usr/sbin/start-stop-daemon")
        val rootfsDpkgSplitFile = File(rootfsStatus.rootfsDir, "usr/bin/dpkg-split")
        val rootfsGuestGpuClientFile = File(rootfsStatus.rootfsDir, "usr/bin/alr-gpu-client")
        val rootfsGuestGlesShimSmokeFile = File(rootfsStatus.rootfsDir, "usr/bin/alr-gles-shim-smoke")
        val rootfsGuestGlesShimLibraryFile = File(rootfsStatus.rootfsDir, "usr/lib/androlinux/libalr_gles_shim.so")
        val rootfsWaylandGuiClientFile = File(rootfsStatus.rootfsDir, "usr/bin/alr-wayland-gpu-client")
        val rootfsX11GuiClientFile = File(rootfsStatus.rootfsDir, "usr/bin/alr-x11-gpu-client")
        val rootfsExecutionPassed = prootHelloResult.exitCode == 0 &&
            prootHelloResult.stdout.contains("hello from static arm64 rootfs")
        val shellScriptExecutionPassed = prootScriptResult.exitCode == 0 &&
            prootScriptResult.stdout.contains("hello from shell script rootfs")
        val shellCommandExecutionPassed = prootShellResult.exitCode == 0 &&
            prootShellResult.stdout.contains("shell-c ok") &&
            prootShellResult.stdout.contains("NAME=\"AndroLinux Tiny Rootfs\"")
        val glibcDynamicExecutionPassed = prootGlibcResult.exitCode == 0 &&
            prootGlibcResult.stdout.contains("hello from dynamic glibc rootfs") &&
            prootGlibcResult.stdout.contains("glibc version=")
        val androidEnvLeakPrefixes = listOf(
            "ANDROID_ROOT=",
            "ANDROID_DATA=",
            "BOOTCLASSPATH=",
            "DEX2OATBOOTCLASSPATH=",
            "SYSTEMSERVERCLASSPATH=",
            "ANDROID_SOCKET_",
            "EXTERNAL_STORAGE=",
            "KNOX_STORAGE=",
        )
        val guestEnvLeakedAndroidVars = prootDashResult.stdout.lineSequence().any { line ->
            androidEnvLeakPrefixes.any { prefix -> line.startsWith(prefix) }
        }
        val distroUserlandExecutionPassed = prootDashResult.exitCode == 0 &&
            prootDashResult.stdout.contains("dash-c ok") &&
            prootDashResult.stdout.contains("ALR_ROOTFS=") &&
            prootDashResult.stdout.contains("PATH=") &&
            !guestEnvLeakedAndroidVars
        val identityNumericRoot = prootIdResult.exitCode == 0 &&
            prootIdResult.stdout.contains("uid=0") &&
            prootIdResult.stdout.contains("gid=0")
        val identityNamedRoot = prootIdResult.stdout.contains("uid=0(root)") &&
            prootIdResult.stdout.contains("gid=0(root)")
        val identityNssExecutionPassed = identityNumericRoot && identityNamedRoot
        val dpkgVersionExecutionPassed = prootDpkgVersionResult.exitCode == 0 &&
            prootDpkgVersionResult.stdout.contains("Debian 'dpkg' package management program")
        val dpkgArchExecutionPassed = prootDpkgArchResult.exitCode == 0 &&
            prootDpkgArchResult.stdout.trim() == "arm64"
        val dpkgQueryExecutionPassed = prootDpkgQueryVersionResult.exitCode == 0 &&
            prootDpkgQueryVersionResult.stdout.contains("Debian dpkg-query package management program")
        val dpkgSplitExecutionPassed = prootDpkgSplitVersionResult.exitCode == 0 &&
            prootDpkgSplitVersionResult.stdout.contains("Debian 'dpkg-split' package split/join tool")
        val aptVersionExecutionPassed = prootAptVersionResult.exitCode == 0 &&
            prootAptVersionResult.stdout.contains("apt ") &&
            prootAptVersionResult.stdout.contains("arm64")
        val aptGetVersionExecutionPassed = prootAptGetVersionResult.exitCode == 0 &&
            prootAptGetVersionResult.stdout.contains("apt ") &&
            prootAptGetVersionResult.stdout.contains("arm64")
        val aptCacheVersionExecutionPassed = prootAptCacheVersionResult.exitCode == 0 &&
            prootAptCacheVersionResult.stdout.contains("apt ") &&
            prootAptCacheVersionResult.stdout.contains("arm64")
        val aptConfigVersionExecutionPassed = prootAptConfigVersionResult.exitCode == 0 &&
            prootAptConfigVersionResult.stdout.contains("apt ") &&
            prootAptConfigVersionResult.stdout.contains("arm64")
        val dpkgLocalInstallExecutionPassed = prootDpkgInstallLocalResult.exitCode == 0 &&
            !prootDpkgInstallLocalResult.stderr.contains("dpkg: error") &&
            (prootDpkgInstallLocalResult.stdout.contains("Setting up alr-smoke") ||
                prootDpkgInstallLocalResult.stderr.contains("Setting up alr-smoke") ||
                prootDpkgInstallLocalResult.stdout.contains("alr-smoke (1.0)") ||
                prootDpkgInstallLocalResult.stderr.contains("alr-smoke (1.0)") ||
                prootDpkgInstallLocalResult.stdout.contains("Selecting previously unselected package alr-smoke") ||
                prootDpkgInstallLocalResult.stderr.contains("Selecting previously unselected package alr-smoke"))
        val installedPackageExecutionPassed = prootInstalledPackageSmokeResult.exitCode == 0 &&
            prootInstalledPackageSmokeResult.stdout.contains("alr local deb package smoke ok")
        val guestGpuBridgeCommandPassed = prootGuestGpuClientResult.exitCode == 0 &&
            prootGuestGpuClientResult.stdout.contains("alr guest gpu client ok") &&
            guestGpuCommands.isNotEmpty()
        val guestGpuIpcBridgePassed = guestGpuIpcBridgeResult.clientResult.exitCode == 0 &&
            guestGpuIpcBridgeResult.commands.size == guestGpuIpcBridgeResult.expectedFrames &&
            guestGpuIpcBridgeResult.expectedFrames > 0 &&
            guestGpuIpcBridgeResult.error == null
        val guestGlesShimSmokePassed = prootGuestGlesShimSmokeResult.exitCode == 0 &&
            prootGuestGlesShimSmokeResult.stdout.contains("alr guest gles shim smoke ok") &&
            prootGuestGlesShimSmokeResult.stdout.contains("ALR_GLES_SHIM_LOAD ok") &&
            guestGlesShimCommand != null
        val guestWaylandGuiBridgePassed = guestWaylandGuiBridgeResult.clientResult.exitCode == 0 &&
            guestWaylandGuiBridgeResult.commands.size == guestWaylandGuiBridgeResult.expectedFrames &&
            guestWaylandGuiBridgeResult.expectedFrames > 0 &&
            guestWaylandGuiBridgeResult.error == null
        val guestX11GuiBridgePassed = guestX11GuiBridgeResult.clientResult.exitCode == 0 &&
            guestX11GuiBridgeResult.commands.size == guestX11GuiBridgeResult.expectedFrames &&
            guestX11GuiBridgeResult.expectedFrames > 0 &&
            guestX11GuiBridgeResult.error == null
        val hostGpuHardwareCandidate = hostGpuProbe.lineStartingWith("host gpu hardware candidate=") == "host gpu hardware candidate=true"
        val hostVulkanHardwareCandidate = hostVulkanProbe.lineStartingWith("host vulkan hardware candidate=") == "host vulkan hardware candidate=true"
        val alrTrampolineContinuePassed =
            alrTrampolineContinueProbe.lineStartingWith("ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION:") ==
                "ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: PASS"
        val alrProcfsVirtualizationPassed =
            alrProcfsVirtualizationProbe.lineStartingWith("ALR PROCFS PLAN:") == "ALR PROCFS PLAN: PASS"
        val alrWxSafeExecPassed =
            alrWxSafeExecProbe.lineStartingWith("ALR WX-SAFE EXEC STRATEGY:") == "ALR WX-SAFE EXEC STRATEGY: PASS"
        val alrPerfHarnessRan =
            alrPerfComparisonProbe.lineStartingWith("ALR PERF HARNESS:") == "ALR PERF HARNESS: PASS"
        val alrInterposeProcfsPassed =
            alrInterposeProcfsProbe.lineStartingWith("ALR INTERPOSE SELF EXE VIRTUALIZED:") ==
                "ALR INTERPOSE SELF EXE VIRTUALIZED: PASS" &&
                alrInterposeProcfsProbe.lineStartingWith("ALR INTERPOSE MOUNTS VIRTUALIZED:") ==
                "ALR INTERPOSE MOUNTS VIRTUALIZED: PASS" &&
                alrInterposeProcfsProbe.lineStartingWith("ALR INTERPOSE MOUNTS NO HOST LEAK:") ==
                "ALR INTERPOSE MOUNTS NO HOST LEAK: PASS"
        val alrMemfdExecPassed =
            alrMemfdExecProbe.lineStartingWith("ALR MEMFD EXECVEAT W^X-SAFE EXECUTION:") ==
                "ALR MEMFD EXECVEAT W^X-SAFE EXECUTION: PASS"
        val alrExecmemPassed =
            alrExecmemProbe.lineStartingWith("ALR EXECMEM ANON RX EXECUTION:") ==
                "ALR EXECMEM ANON RX EXECUTION: PASS"
        val alrSeccompPathTrapViable =
            alrSeccompPathTrapProbe.lineStartingWith("alr sc PATH_MEDIATION_VIABLE=")
                .substringAfter("PATH_MEDIATION_VIABLE=", "") == "yes"

        val executionSummary = "build: 0.4.131-cp5-gpu-throughput-v131" +
            "\nexecution summary" +
            "\nROOTFS EXECUTION: ${if (rootfsExecutionPassed) "PASS" else "FAIL"}" +
            "\nSHELL SCRIPT EXECUTION: ${if (shellScriptExecutionPassed) "PASS" else "FAIL"}" +
            "\nSHELL -C EXECUTION: ${if (shellCommandExecutionPassed) "PASS" else "FAIL"}" +
            "\nGLIBC DYNAMIC EXECUTION: ${if (glibcDynamicExecutionPassed) "PASS" else "FAIL"}" +
            "\nDISTRO USERLAND EXECUTION: ${if (distroUserlandExecutionPassed) "PASS" else "FAIL"}" +
            "\nCLEAN GUEST ENVIRONMENT: ${if (!guestEnvLeakedAndroidVars) "PASS" else "FAIL"}" +
            "\nIDENTITY NSS EXECUTION: ${if (identityNssExecutionPassed) "PASS" else "FAIL"}" +
            "\nDPKG VERSION EXECUTION: ${if (dpkgVersionExecutionPassed) "PASS" else "FAIL"}" +
            "\nDPKG ARCH EXECUTION: ${if (dpkgArchExecutionPassed) "PASS" else "FAIL"}" +
            "\nDPKG QUERY EXECUTION: ${if (dpkgQueryExecutionPassed) "PASS" else "FAIL"}" +
            "\nDPKG SPLIT EXECUTION: ${if (dpkgSplitExecutionPassed) "PASS" else "FAIL"}" +
            "\nAPT VERSION EXECUTION: ${if (aptVersionExecutionPassed) "PASS" else "FAIL"}" +
            "\nAPT-GET VERSION EXECUTION: ${if (aptGetVersionExecutionPassed) "PASS" else "FAIL"}" +
            "\nAPT-CACHE VERSION EXECUTION: ${if (aptCacheVersionExecutionPassed) "PASS" else "FAIL"}" +
            "\nAPT-CONFIG VERSION EXECUTION: ${if (aptConfigVersionExecutionPassed) "PASS" else "FAIL"}" +
            "\nDPKG LOCAL INSTALL EXECUTION: ${if (dpkgLocalInstallExecutionPassed) "PASS" else "FAIL"}" +
            "\nINSTALLED PACKAGE EXECUTION: ${if (installedPackageExecutionPassed) "PASS" else "FAIL"}" +
            "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: ${if (alrTrampolineContinuePassed) "PASS" else "FAIL"}" +
            "\nALR PROCFS VIRTUALIZATION PLAN: ${if (alrProcfsVirtualizationPassed) "PASS" else "FAIL"}" +
            "\nALR WX-SAFE EXEC STRATEGY: ${if (alrWxSafeExecPassed) "PASS" else "FAIL"}" +
            "\nALR PERF HARNESS: ${if (alrPerfHarnessRan) "PASS" else "FAIL"}" +
            "\nALR PERF PROOT VS ALR DEVICE COMPARISON: ${alrPerfComparisonProbe.lineStartingWith("ALR PERF PROOT VS ALR DEVICE COMPARISON:").substringAfter("COMPARISON: ", "PENDING_DEVICE")}" +
            "\nALR PROCFS INTERPOSE MECHANISM: ${if (alrInterposeProcfsPassed) "PASS" else "FAIL"}" +
            "\nALR MEMFD W^X-SAFE NATIVE EXEC: ${if (alrMemfdExecPassed) "PASS" else "FAIL"}" +
            "\nALR EXECMEM ANON RX NATIVE EXEC: ${if (alrExecmemPassed) "PASS" else "FAIL"}" +
            "\nALR NATIVE LOADER MECHANISM (freestanding): ${if (alrNativeLoaderSelftestPassed) "PASS" else "FAIL"}" +
            "\nALR NATIVE LOADER GUEST EXEC (glibc static): ${if (alrNativeLoaderGuestExecPassed) "PASS" else "FAIL"}" +
            "\nALR NATIVE LOADER GUEST EXEC (glibc threads+fork): ${if (alrNativeLoaderMtPassed) "PASS" else "FAIL"}" +
            "\nALR SECCOMP PATH-MEDIATION (seccomp-trace rootfs path rewrite): ${if (alrSeccompPathTrapViable) "VIABLE" else "BLOCKED"}" +
            "\nALR WAYLAND SOCKET TRANSPORT (named AF_UNIX host↔guest): ${if (alrUnixSocketViable) "VIABLE" else "BLOCKED"}" +
            "\nALR LOADER PATH-MEDIATION REAL FILE READ (glibc opens /etc via rootfs): ${if (alrNativeLoaderFileioMediated) "PASS" else "FAIL"}" +
            "\nALR NATIVE LOADER GUEST EXEC (glibc DYNAMIC via in-process ld.so): ${if (alrNativeLoaderDynPassed) "PASS" else "FAIL"}" +
            "\nALR REAL DEBIAN PROGRAM (/usr/bin/env, dynamic coreutils): ${if (alrLoaderRealEnvPassed) "PASS" else "FAIL"}" +
            "\nALR REAL DEBIAN PROGRAM (/usr/bin/id, libselinux+rootfs /etc): ${if (alrLoaderRealIdPassed) "PASS" else "FAIL"}" +
            "\nALR REAL SHELL COMMAND (/bin/dash -c 'echo …', argv passing): ${if (alrLoaderDashPassed) "PASS" else "FAIL"}" +
            "\nALR WRITE-PATH MEDIATION (dash writes+reads /tmp in rootfs): ${if (alrLoaderWritePassed) "PASS" else "FAIL"}" +
            "\nALR IN-PROCESS IMAGE DECODE (gdk-pixbuf PNG, GIMP prerequisite): ${if (alrLoaderPngDecoded) "PASS" else "FAIL"}" +
            "\nALR GIMP 3.0 LOADS (gimp-console-3.0 --version, 112-lib closure in-process): ${if (alrLoaderGimpVersion) "PASS" else "FAIL"}" +
            "\nALR CP-3 MICROBENCH compute (same binary via loader, ns_per_op in logcat alr-microbench): ${if (alrLoaderMicrobenchComputeRan) "RAN" else "ABSENT"}" +
            "\nALR CP-3 MICROBENCH syscall (same binary via loader, ns_per_op in logcat alr-microbench): ${if (alrLoaderMicrobenchSyscallRan) "RAN" else "ABSENT"}" +
            "\nALR GPU PASSTHROUGH BOUNDARY: ${if (gpuPassthroughBoundaryViable) "VIABLE" else "MARGINAL"} (shmem-ring ${gpuBoundaryShmemCmdsPerFrame} cmds/60fps-frame, inproc ${gpuBoundaryInprocCmdsPerFrame})" +
            "\nALR GPU MARSHALLING HARDWARE RENDER: ${if (alrGpuMarshallingPassed) "PASS" else "FAIL"}" +
            "\nALR AHB ZEROCOPY HARDWARE SAMPLE: ${if (alrAhbZeroCopyPassed) "PASS" else "FAIL"}" +
            "\nALR GPU DRAW HARDWARE RENDER (shader+VBO+texture+draw on Mali): ${if (alrGpuDrawPassed) "PASS" else "FAIL"}" +
            "\nALR GPU RING HARDWARE RENDER (op stream via SPSC ring -> Mali): ${if (alrGpuRingPassed) "PASS" else "FAIL"}" +
            "\nALR GPU AHB RENDER (guest draw -> AHB render target, direct read): ${if (alrGpuFboPassed) "PASS" else "FAIL"}" +
            "\nALR GPU LIVE INTEGRATION (guest -> ring -> host executor -> AHB present, 8 frames on Mali): ${if (alrGpuLivePassed) "PASS" else "FAIL"}" +
            "\nALR GPU THROUGHPUT (same op-stream on Mali: ALR ring+executor vs direct, % of native): ${if (alrGpuThroughputPassed) "PASS" else "FAIL"}" +
            "\nALR JIT WX CYCLE (V8-style iterative RW<->RX exec memory; PASS => Chromium V8 needs no --jitless): ${if (jitWxPassed) "PASS" else "FAIL"}" +
            "\nHOST GPU EGL/GLES EXECUTION: ${if (hostGpuHardwareCandidate) "PASS" else "FAIL"}" +
            "\nANDROID HOST VULKAN PROBE EXECUTION: ${if (hostVulkanHardwareCandidate) "PASS" else "FAIL"}" +
            "\nANDROID HOST VULKAN SURFACE PROBE EXECUTION: PENDING_SURFACE_CALLBACK" +
            "\nANDROID HOST VULKAN SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" +
            "\nHOST GPU SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" +
            "\nGUEST GPU BRIDGE COMMAND EXECUTION: ${if (guestGpuBridgeCommandPassed) "PASS" else "FAIL"}" +
            "\nGUEST GPU IPC BRIDGE EXECUTION: ${if (guestGpuIpcBridgePassed) "PASS" else "FAIL"}" +
            "\nGUEST GLES SHIM SMOKE EXECUTION: ${if (guestGlesShimSmokePassed) "PASS" else "FAIL"}" +
            "\nGUEST GPU MULTI-FRAME SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" +
            "\nGUEST WAYLAND GUI GPU BRIDGE EXECUTION: ${if (guestWaylandGuiBridgePassed) "PASS" else "FAIL"}" +
            "\nGUEST X11 GUI GPU BRIDGE EXECUTION: ${if (guestX11GuiBridgePassed) "PASS" else "FAIL"}" +
            "\nGUEST GUI GPU SURFACE EXECUTION: PENDING_SURFACE_CALLBACK" +
            "\nANDROID PERMISSION MODEL: ${if (internetPermissionDeclared && networkStatePermissionDeclared && !broadStoragePermissionDeclared) "PASS" else "FAIL"}" +
            "\nidentity numeric root=$identityNumericRoot" +
            "\nidentity named root=$identityNamedRoot" +
            "\nidentity proot mode=raw -r" +
            "\nguest env leaked android vars=$guestEnvLeakedAndroidVars" +
            "\nrootfs verified=${rootfsStatus.verified} extracted=${rootfsStatus.extracted}" +
            "\nrootfs /bin/hello exists=${rootfsHelloFile.isFile} executable=${rootfsHelloFile.canExecute()} bytes=${rootfsHelloFile.length()}" +
            "\nrootfs /bin/sh exists=${rootfsShellFile.isFile} executable=${rootfsShellFile.canExecute()} bytes=${rootfsShellFile.length()}" +
            "\nrootfs /bin/cat exists=${rootfsCatFile.isFile} executable=${rootfsCatFile.canExecute()} bytes=${rootfsCatFile.length()}" +
            "\nrootfs /bin/script-hello exists=${rootfsScriptFile.isFile} executable=${rootfsScriptFile.canExecute()} bytes=${rootfsScriptFile.length()}" +
            "\nrootfs /bin/glibc-hello exists=${rootfsGlibcHelloFile.isFile} executable=${rootfsGlibcHelloFile.canExecute()} bytes=${rootfsGlibcHelloFile.length()}" +
            "\nrootfs glibc loader exists=${rootfsGlibcLoaderFile.isFile} executable=${rootfsGlibcLoaderFile.canExecute()} bytes=${rootfsGlibcLoaderFile.length()}" +
            "\nrootfs libc exists=${rootfsLibcFile.isFile} executable=${rootfsLibcFile.canExecute()} bytes=${rootfsLibcFile.length()}" +
            "\nrootfs /bin/dash exists=${rootfsDashFile.isFile} executable=${rootfsDashFile.canExecute()} bytes=${rootfsDashFile.length()}" +
            "\nrootfs /usr/bin/env exists=${rootfsEnvFile.isFile} executable=${rootfsEnvFile.canExecute()} bytes=${rootfsEnvFile.length()}" +
            "\nrootfs /etc/passwd exists=${rootfsPasswdFile.isFile} bytes=${rootfsPasswdFile.length()}" +
            "\nrootfs /etc/group exists=${rootfsGroupFile.isFile} bytes=${rootfsGroupFile.length()}" +
            "\nrootfs /etc/nsswitch.conf exists=${rootfsNsswitchFile.isFile} bytes=${rootfsNsswitchFile.length()}" +
            "\nrootfs /usr/bin/id exists=${rootfsIdFile.isFile} executable=${rootfsIdFile.canExecute()} bytes=${rootfsIdFile.length()}" +
            "\nrootfs libselinux exists=${rootfsLibselinuxFile.isFile} executable=${rootfsLibselinuxFile.canExecute()} bytes=${rootfsLibselinuxFile.length()}" +
            "\nrootfs libpcre2 exists=${rootfsLibpcre2File.isFile} executable=${rootfsLibpcre2File.canExecute()} bytes=${rootfsLibpcre2File.length()}" +
            "\nrootfs libnss_files exists=${rootfsLibnssFilesFile.isFile} executable=${rootfsLibnssFilesFile.canExecute()} bytes=${rootfsLibnssFilesFile.length()}" +
            "\nrootfs /usr/bin/dpkg exists=${rootfsDpkgFile.isFile} executable=${rootfsDpkgFile.canExecute()} bytes=${rootfsDpkgFile.length()}" +
            "\nrootfs libmd exists=${rootfsLibmdFile.isFile} executable=${rootfsLibmdFile.canExecute()} bytes=${rootfsLibmdFile.length()}" +
            "\nrootfs /etc/dpkg/dpkg.cfg exists=${rootfsDpkgConfigFile.isFile} bytes=${rootfsDpkgConfigFile.length()}" +
            "\nrootfs /etc/dpkg/dpkg.cfg.d exists=${rootfsDpkgConfigDir.isDirectory}" +
            "\nrootfs stale needrestart dpkg cfg exists=${rootfsNeedrestartConfigFile.isFile}" +
            "\nrootfs androlinux minimal dpkg cfg exists=${rootfsAndrolinuxDpkgConfigFile.isFile}" +
            "\nrootfs /usr/bin/dpkg-query exists=${rootfsDpkgQueryFile.isFile} executable=${rootfsDpkgQueryFile.canExecute()} bytes=${rootfsDpkgQueryFile.length()}" +
            "\nrootfs /usr/share/dpkg/cputable exists=${rootfsDpkgCpuTableFile.isFile} bytes=${rootfsDpkgCpuTableFile.length()}" +
            "\nrootfs /usr/share/dpkg/tupletable exists=${rootfsDpkgTupleTableFile.isFile} bytes=${rootfsDpkgTupleTableFile.length()}" +
            "\nrootfs /usr/bin/apt exists=${rootfsAptFile.isFile} executable=${rootfsAptFile.canExecute()} bytes=${rootfsAptFile.length()}" +
            "\nrootfs /usr/bin/apt-get exists=${rootfsAptGetFile.isFile} executable=${rootfsAptGetFile.canExecute()} bytes=${rootfsAptGetFile.length()}" +
            "\nrootfs /usr/bin/apt-cache exists=${rootfsAptCacheFile.isFile} executable=${rootfsAptCacheFile.canExecute()} bytes=${rootfsAptCacheFile.length()}" +
            "\nrootfs /usr/bin/apt-config exists=${rootfsAptConfigFile.isFile} executable=${rootfsAptConfigFile.canExecute()} bytes=${rootfsAptConfigFile.length()}" +
            "\nrootfs libapt-pkg exists=${rootfsLibAptPkgFile.isFile} executable=${rootfsLibAptPkgFile.canExecute()} bytes=${rootfsLibAptPkgFile.length()}" +
            "\nrootfs libapt-private exists=${rootfsLibAptPrivateFile.isFile} executable=${rootfsLibAptPrivateFile.canExecute()} bytes=${rootfsLibAptPrivateFile.length()}" +
            "\nrootfs apt http method exists=${rootfsAptHttpMethodFile.isFile} executable=${rootfsAptHttpMethodFile.canExecute()} bytes=${rootfsAptHttpMethodFile.length()}" +
            "\nrootfs apt lists partial exists=${rootfsAptListsPartialDir.isDirectory}" +
            "\nrootfs local deb exists=${rootfsLocalDebFile.isFile} bytes=${rootfsLocalDebFile.length()}" +
            "\nrootfs /usr/bin/dpkg-deb exists=${rootfsDpkgDebFile.isFile} executable=${rootfsDpkgDebFile.canExecute()} bytes=${rootfsDpkgDebFile.length()}" +
            "\nrootfs installed alr smoke exists=${rootfsInstalledSmokeFile.isFile} executable=${rootfsInstalledSmokeFile.canExecute()} bytes=${rootfsInstalledSmokeFile.length()}" +
            "\nrootfs dpkg status exists=${rootfsDpkgStatusFile.isFile} bytes=${rootfsDpkgStatusFile.length()}" +
            "\nrootfs /dev/null placeholder exists=${rootfsDevNullFile.isFile} bytes=${rootfsDevNullFile.length()}" +
            "\nrootfs dpkg triggers File exists=${rootfsDpkgTriggersFile.isFile} bytes=${rootfsDpkgTriggersFile.length()}" +
            "\nrootfs dpkg triggers Unincorp exists=${rootfsDpkgTriggersUnincorpFile.isFile} bytes=${rootfsDpkgTriggersUnincorpFile.length()}" +
            "\nrootfs helper rm exists=${rootfsRmFile.isFile} executable=${rootfsRmFile.canExecute()} bytes=${rootfsRmFile.length()}" +
            "\nrootfs helper tar exists=${rootfsTarFile.isFile} executable=${rootfsTarFile.canExecute()} bytes=${rootfsTarFile.length()}" +
            "\nrootfs helper diff exists=${rootfsDiffFile.isFile} executable=${rootfsDiffFile.canExecute()} bytes=${rootfsDiffFile.length()}" +
            "\nrootfs helper ldconfig exists=${rootfsLdconfigFile.isFile} executable=${rootfsLdconfigFile.canExecute()} bytes=${rootfsLdconfigFile.length()}" +
            "\nrootfs helper ldconfig.real exists=${rootfsLdconfigRealFile.isFile} executable=${rootfsLdconfigRealFile.canExecute()} bytes=${rootfsLdconfigRealFile.length()}" +
            "\nrootfs helper start-stop-daemon exists=${rootfsStartStopDaemonFile.isFile} executable=${rootfsStartStopDaemonFile.canExecute()} bytes=${rootfsStartStopDaemonFile.length()}" +
            "\nrootfs /usr/bin/dpkg-split exists=${rootfsDpkgSplitFile.isFile} executable=${rootfsDpkgSplitFile.canExecute()} bytes=${rootfsDpkgSplitFile.length()}" +
            "\nrootfs /usr/bin/alr-gpu-client exists=${rootfsGuestGpuClientFile.isFile} executable=${rootfsGuestGpuClientFile.canExecute()} bytes=${rootfsGuestGpuClientFile.length()}" +
            "\nrootfs /usr/bin/alr-gles-shim-smoke exists=${rootfsGuestGlesShimSmokeFile.isFile} executable=${rootfsGuestGlesShimSmokeFile.canExecute()} bytes=${rootfsGuestGlesShimSmokeFile.length()}" +
            "\nrootfs /usr/lib/androlinux/libalr_gles_shim.so exists=${rootfsGuestGlesShimLibraryFile.isFile} executable=${rootfsGuestGlesShimLibraryFile.canExecute()} bytes=${rootfsGuestGlesShimLibraryFile.length()}" +
            "\nrootfs /usr/bin/alr-wayland-gpu-client exists=${rootfsWaylandGuiClientFile.isFile} executable=${rootfsWaylandGuiClientFile.canExecute()} bytes=${rootfsWaylandGuiClientFile.length()}" +
            "\nrootfs /usr/bin/alr-x11-gpu-client exists=${rootfsX11GuiClientFile.isFile} executable=${rootfsX11GuiClientFile.canExecute()} bytes=${rootfsX11GuiClientFile.length()}" +
            "\nproot guest gpu client exit=${prootGuestGpuClientResult.exitCode}" +
            "\nproot guest gpu client stdout=${prootGuestGpuClientResult.stdout}" +
            "\nproot guest gpu client stderr=${prootGuestGpuClientResult.stderr}" +
            "\nguest gpu stdout commands parsed=${guestGpuCommands.size}" +
            "\nguest gpu ipc host=${guestGpuIpcBridgeResult.host}" +
            "\nguest gpu ipc port=${guestGpuIpcBridgeResult.port}" +
            "\nguest gpu ipc expected frames=${guestGpuIpcBridgeResult.expectedFrames}" +
            "\nguest gpu ipc received frames=${guestGpuIpcBridgeResult.commands.size}" +
            "\nguest gpu ipc dropped frames=${(guestGpuIpcBridgeResult.expectedFrames - guestGpuIpcBridgeResult.commands.size).coerceAtLeast(0)}" +
            "\nguest gpu ipc lossless=${guestGpuIpcBridgeResult.expectedFrames > 0 && guestGpuIpcBridgeResult.expectedFrames == guestGpuIpcBridgeResult.commands.size}" +
            "\nguest gpu ipc raw=${guestGpuIpcBridgeResult.rawLines.joinToString("|")}" +
            "\nguest gpu ipc error=${guestGpuIpcBridgeResult.error ?: "none"}" +
            "\nproot guest gpu ipc client exit=${guestGpuIpcBridgeResult.clientResult.exitCode}" +
            "\nproot guest gpu ipc client stdout=${guestGpuIpcBridgeResult.clientResult.stdout}" +
            "\nproot guest gpu ipc client stderr=${guestGpuIpcBridgeResult.clientResult.stderr}" +
            "\nproot guest gles shim smoke exit=${prootGuestGlesShimSmokeResult.exitCode}" +
            "\nproot guest gles shim smoke stdout=${prootGuestGlesShimSmokeResult.stdout}" +
            "\nproot guest gles shim smoke stderr=${prootGuestGlesShimSmokeResult.stderr}" +
            "\nguest gles shim command parsed=${guestGlesShimCommand != null}" +
            "\nproot guest wayland gui client exit=${prootGuestWaylandGuiResult.exitCode}" +
            "\nproot guest wayland gui client stdout=${prootGuestWaylandGuiResult.stdout}" +
            "\nproot guest x11 gui client exit=${prootGuestX11GuiResult.exitCode}" +
            "\nproot guest x11 gui client stdout=${prootGuestX11GuiResult.stdout}" +
            "\nguest wayland gui ipc expected frames=${guestWaylandGuiBridgeResult.expectedFrames}" +
            "\nguest wayland gui ipc received frames=${guestWaylandGuiBridgeResult.commands.size}" +
            "\nguest wayland gui ipc lossless=${guestWaylandGuiBridgeResult.expectedFrames > 0 && guestWaylandGuiBridgeResult.expectedFrames == guestWaylandGuiBridgeResult.commands.size}" +
            "\nguest wayland gui ipc seq gaps=${guiSeqGaps(guestWaylandGuiBridgeResult.commands, guestWaylandGuiBridgeResult.expectedFrames)}" +
            "\nguest wayland gui ipc duplicate seq=${guiDuplicateSeqCount(guestWaylandGuiBridgeResult.commands)}" +
            "\nguest wayland gui ipc out of order=${guiOutOfOrder(guestWaylandGuiBridgeResult.commands)}" +
            "\nguest wayland gui ipc raw=${guestWaylandGuiBridgeResult.rawLines.joinToString("|")}" +
            "\nguest wayland gui ipc error=${guestWaylandGuiBridgeResult.error ?: "none"}" +
            "\nguest x11 gui ipc expected frames=${guestX11GuiBridgeResult.expectedFrames}" +
            "\nguest x11 gui ipc received frames=${guestX11GuiBridgeResult.commands.size}" +
            "\nguest x11 gui ipc lossless=${guestX11GuiBridgeResult.expectedFrames > 0 && guestX11GuiBridgeResult.expectedFrames == guestX11GuiBridgeResult.commands.size}" +
            "\nguest x11 gui ipc seq gaps=${guiSeqGaps(guestX11GuiBridgeResult.commands, guestX11GuiBridgeResult.expectedFrames)}" +
            "\nguest x11 gui ipc duplicate seq=${guiDuplicateSeqCount(guestX11GuiBridgeResult.commands)}" +
            "\nguest x11 gui ipc out of order=${guiOutOfOrder(guestX11GuiBridgeResult.commands)}" +
            "\nguest x11 gui ipc raw=${guestX11GuiBridgeResult.rawLines.joinToString("|")}" +
            "\nguest x11 gui ipc error=${guestX11GuiBridgeResult.error ?: "none"}" +
            "\nsurface gpu command source frames=${surfaceGpuCommands.size}" +
            "\nproot dpkg-split --version exit=${prootDpkgSplitVersionResult.exitCode}" +
            "\nproot dpkg-split --version stdout=${prootDpkgSplitVersionResult.stdout}" +
            "\nproot dpkg-split --version stderr=${prootDpkgSplitVersionResult.stderr}" +
            "\nnative smoke exit=${nativeCommandResult.exitCode}" +
            "\nnative smoke stdout=${nativeCommandResult.stdout}" +
            "\nalr packaged trampoline continue passed=$alrTrampolineContinuePassed" +
            "\nalr packaged trampoline continue summary=${alrTrampolineContinueProbe.lineStartingWith("ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION:")}" +
            "\nalr procfs virtualization passed=$alrProcfsVirtualizationPassed" +
            "\nalr procfs self exe guest view=${alrProcfsVirtualizationProbe.lineStartingWith("alr procfs self exe guest view=").substringAfter("guest view=", "")}" +
            "\nalr procfs self exe host truth=${alrProcfsVirtualizationProbe.lineStartingWith("alr procfs self exe host truth=").substringAfter("host truth=", "")}" +
            "\nalr procfs mounts host leak tokens=${alrProcfsVirtualizationProbe.lineStartingWith("alr procfs mounts host leak tokens=").substringAfter("host leak tokens=", "")}" +
            "\nalr wx-safe exec passed=$alrWxSafeExecPassed" +
            "\nalr wx-safe primary method=${alrWxSafeExecProbe.lineStartingWith("alr wx primary method=").substringAfter("primary method=", "")}" +
            "\nalr wx-safe entrypoint packaged=${alrWxSafeExecProbe.lineStartingWith("ALR WX-SAFE ENTRYPOINT IS PACKAGED:")}" +
            "\nalr wx-safe direct rootfs exec rejected=${alrWxSafeExecProbe.lineStartingWith("ALR WX-SAFE DIRECT ROOTFS EXEC REJECTED:")}" +
            "\nalr perf harness ran=$alrPerfHarnessRan" +
            "\nalr perf alr xlate ns/op=${alrPerfComparisonProbe.lineStartingWith("alr perf alr xlate ns/op=").substringAfter("ns/op=", "")}" +
            "\nalr perf syscall getppid ns/op=${alrPerfComparisonProbe.lineStartingWith("alr perf syscall getppid ns/op=").substringAfter("ns/op=", "")}" +
            "\nalr perf xlate cost in syscall units=${alrPerfComparisonProbe.lineStartingWith("alr perf xlate cost in syscall units=").substringAfter("syscall units=", "")}" +
            "\nalr procfs interpose passed=$alrInterposeProcfsPassed" +
            "\nalr procfs interpose self exe target=${alrInterposeProcfsProbe.lineStartingWith("alr interpose self exe target=").substringAfter("target=", "")}" +
            "\nalr procfs interpose mounts no host leak=${alrInterposeProcfsProbe.lineStartingWith("ALR INTERPOSE MOUNTS NO HOST LEAK:")}" +
            "\nalr memfd w^x-safe native exec passed=$alrMemfdExecPassed" +
            "\nalr memfd create=${alrMemfdExecProbe.lineStartingWith("ALR MEMFD CREATE:")}" +
            "\nalr memfd flag=${alrMemfdExecProbe.lineStartingWith("alr memfd flag=").substringAfter("flag=", "")}" +
            "\nalr memfd child exit=${alrMemfdExecProbe.lineStartingWith("alr memfd child exit=").substringAfter("exit=", "")}" +
            "\nalr memfd child stderr=${alrMemfdExecProbe.lineStartingWith("alr memfd child stderr=").substringAfter("stderr=", "")}" +
            "\nalr syscall sandbox blocked count=${alrSyscallSandboxProbe.lineStartingWith("alr syscall blocked count=").substringAfter("blocked count=", "")}" +
            "\nalr syscall execveat=${alrSyscallSandboxProbe.lineStartingWith("alr syscall execveat=").substringAfter("execveat=", "")}" +
            "\nalr syscall symlink=${alrSyscallSandboxProbe.lineStartingWith("alr syscall symlink=").substringAfter("symlink=", "")}" +
            "\nalr syscall symlinkat=${alrSyscallSandboxProbe.lineStartingWith("alr syscall symlinkat=").substringAfter("symlinkat=", "")}" +
            "\nalr syscall mknodat=${alrSyscallSandboxProbe.lineStartingWith("alr syscall mknodat=").substringAfter("mknodat=", "")}" +
            "\nalr execmem anon rx native exec passed=$alrExecmemPassed" +
            "\nalr execmem mprotect rx=${alrExecmemProbe.lineStartingWith("alr execmem mprotect rx=").substringAfter("mprotect rx=", "")}" +
            "\nalr execmem child signal=${alrExecmemProbe.lineStartingWith("alr execmem child signal=").substringAfter("signal=", "")}" +
            "\nalr execmem diag=${alrExecmemProbe.lineStartingWith("alr execmem diag=").substringAfter("diag=", "")}" +
            "\nalr gpu boundary inproc ns/op=${alrGpuBoundaryProbe.lineStartingWith("alr gpu boundary inproc dispatch ns/op=").substringAfter("ns/op=", "")}" +
            "\nalr gpu boundary socket ns/op=${alrGpuBoundaryProbe.lineStartingWith("alr gpu boundary socket per-cmd ns/op=").substringAfter("ns/op=", "")}" +
            "\nalr gpu boundary shmem-ring ns/op=${alrGpuBoundaryProbe.lineStartingWith("alr gpu boundary shmem-ring ns/op=").substringAfter("ns/op=", "")}" +
            "\nalr gpu passthrough boundary viable=$gpuPassthroughBoundaryViable" +
            "\nalr native loader guest exec passed=$alrNativeLoaderGuestExecPassed" +
            "\nalr native loader map=${alrNativeLoaderProbe.lineStartingWith("ALR NATIVE LOADER MAP:")}" +
            "\nalr native loader reached=${alrNativeLoaderProbe.lineStartingWith("alr native loader reached=").substringAfter("reached=", "")}" +
            "\nalr native loader child=${alrNativeLoaderProbe.lineStartingWith("alr native loader child exit=").substringAfter("exit=", "")}" +
            "\nalr native loader diag=${alrNativeLoaderProbe.lineStartingWith("alr native loader diag=").substringAfter("diag=", "")}" +
            "\nproot --version exit=${prootCandidateResult.exitCode}" +
            "\nlinker64 proot --version exit=${prootViaLinkerResult.exitCode}" +
            "\nproot hello quiet exit=${prootHelloResult.exitCode}" +
            "\nproot hello quiet stdout=${prootHelloResult.stdout}" +
            "\nproot hello quiet stderr=${prootHelloResult.stderr}" +
            "\nproot script exit=${prootScriptResult.exitCode}" +
            "\nproot script stdout=${prootScriptResult.stdout}" +
            "\nproot script stderr=${prootScriptResult.stderr}" +
            "\nproot shell -c exit=${prootShellResult.exitCode}" +
            "\nproot shell -c stdout=${prootShellResult.stdout}" +
            "\nproot shell -c stderr=${prootShellResult.stderr}" +
            "\nproot glibc exit=${prootGlibcResult.exitCode}" +
            "\nproot glibc stdout=${prootGlibcResult.stdout}" +
            "\nproot glibc stderr=${prootGlibcResult.stderr}" +
            "\nproot dash exit=${prootDashResult.exitCode}" +
            "\nproot dash stdout=${prootDashResult.stdout}" +
            "\nproot dash stderr=${prootDashResult.stderr}" +
            "\nproot id exit=${prootIdResult.exitCode}" +
            "\nproot id stdout=${prootIdResult.stdout}" +
            "\nproot id stderr=${prootIdResult.stderr}" +
            "\nproot dpkg --version exit=${prootDpkgVersionResult.exitCode}" +
            "\nproot dpkg --version stdout=${prootDpkgVersionResult.stdout}" +
            "\nproot dpkg --version stderr=${prootDpkgVersionResult.stderr}" +
            "\nproot dpkg --print-architecture exit=${prootDpkgArchResult.exitCode}" +
            "\nproot dpkg --print-architecture stdout=${prootDpkgArchResult.stdout}" +
            "\nproot dpkg --print-architecture stderr=${prootDpkgArchResult.stderr}" +
            "\nproot dpkg-query --version exit=${prootDpkgQueryVersionResult.exitCode}" +
            "\nproot dpkg-query --version stdout=${prootDpkgQueryVersionResult.stdout}" +
            "\nproot dpkg-query --version stderr=${prootDpkgQueryVersionResult.stderr}" +
            "\nproot apt --version exit=${prootAptVersionResult.exitCode}" +
            "\nproot apt --version stdout=${prootAptVersionResult.stdout}" +
            "\nproot apt --version stderr=${prootAptVersionResult.stderr}" +
            "\nproot apt-get --version exit=${prootAptGetVersionResult.exitCode}" +
            "\nproot apt-get --version stdout=${prootAptGetVersionResult.stdout}" +
            "\nproot apt-get --version stderr=${prootAptGetVersionResult.stderr}" +
            "\nproot apt-cache --version exit=${prootAptCacheVersionResult.exitCode}" +
            "\nproot apt-cache --version stdout=${prootAptCacheVersionResult.stdout}" +
            "\nproot apt-cache --version stderr=${prootAptCacheVersionResult.stderr}" +
            "\nproot apt-config --version exit=${prootAptConfigVersionResult.exitCode}" +
            "\nproot apt-config --version stdout=${prootAptConfigVersionResult.stdout}" +
            "\nproot apt-config --version stderr=${prootAptConfigVersionResult.stderr}" +
            "\nproot dpkg -i local deb exit=${prootDpkgInstallLocalResult.exitCode}" +
            "\nproot dpkg -i local deb stdout=${prootDpkgInstallLocalResult.stdout}" +
            "\nproot dpkg -i local deb stderr=${prootDpkgInstallLocalResult.stderr}" +
            "\nproot installed package smoke exit=${prootInstalledPackageSmokeResult.exitCode}" +
            "\nproot installed package smoke stdout=${prootInstalledPackageSmokeResult.stdout}" +
            "\nproot installed package smoke stderr=${prootInstalledPackageSmokeResult.stderr}" +
            "\nprobe dlopen talloc=${nativeProbe.lineStartingWith("dlopen libtalloc.so")}" +
            "\nprobe dlopen proot=${nativeProbe.lineStartingWith("dlopen libalr_proot.so")}" +
            "\nproot loader=${prootCandidateResult.environment["PROOT_LOADER"]}" +
            "\nproot tmp=${prootCandidateResult.environment["PROOT_TMP_DIR"]}" +
            "\nproot verbose=${prootCandidateResult.environment["PROOT_VERBOSE"]}" +
            "\nhost gpu renderer=${hostGpuProbe.lineStartingWith("gl renderer=")}" +
            "\nhost gpu vendor=${hostGpuProbe.lineStartingWith("gl vendor=")}" +
            "\nhost gpu software renderer=${hostGpuProbe.lineStartingWith("host gpu software renderer=")}" +
            "\nhost gpu hardware candidate=${hostGpuProbe.lineStartingWith("host gpu hardware candidate=")}" +
            "\nhost vulkan device=${hostVulkanProbe.lineStartingWith("host vulkan device=")}" +
            "\nhost vulkan device type=${hostVulkanProbe.lineStartingWith("host vulkan device type=")}" +
            "\nhost vulkan api version=${hostVulkanProbe.lineStartingWith("host vulkan api version=")}" +
            "\nhost vulkan android surface extension=${hostVulkanProbe.lineStartingWith("host vulkan android surface extension=")}" +
            "\nhost vulkan software renderer=${hostVulkanProbe.lineStartingWith("host vulkan software renderer=")}" +
            "\nhost vulkan hardware candidate=${hostVulkanProbe.lineStartingWith("host vulkan hardware candidate=")}" +
            "\npermission INTERNET declared=$internetPermissionDeclared" +
            "\npermission ACCESS_NETWORK_STATE declared=$networkStatePermissionDeclared" +
            "\npermission broad storage declared=$broadStoragePermissionDeclared" +
            "\npermission runtime dangerous requested=false"

        val verboseReport = nativeRuntimeReport(
            packageName,
            applicationInfo.nativeLibraryDir,
            filesDir.absolutePath,
            cacheDir.absolutePath,
            rootfsManifest.name,
            "/bin/hello",
        ) + "\n\nrootfs install dir: ${rootfsPlan.rootfsDir.absolutePath}" +
            "\nrootfs marker: ${rootfsPlan.markerPath.absolutePath}" +
            "\nrootfs status: ${rootfsStatus.manifestName}/${rootfsStatus.assetPath} verified=${rootfsStatus.verified}" +
            " extracted=${rootfsStatus.extracted}" +
            "\nrootfs staged archive: ${rootfsStatus.stagedArchive.absolutePath}" +
            "\nrootfs extracted dir: ${rootfsStatus.rootfsDir.absolutePath}" +
            "\nrootfs marker=${rootfsStatus.markerPath.absolutePath}" +
            "\n\nnext executable backend: android-native-test-command" +
            "\npackaged command: ${applicationInfo.nativeLibraryDir}/libalr_test_command.so" +
            "\nnative command exit=${nativeCommandResult.exitCode}" +
            "\nnative command stdout=${nativeCommandResult.stdout}" +
            "\nnative command stderr=${nativeCommandResult.stderr}" +
            "\n\nnative library probe:" +
            "\n$nativeProbe" +
            "\n\nALR packaged trampoline continue probe:" +
            "\n$alrTrampolineContinueProbe" +
            "\n\nALR procfs virtualization plan probe:" +
            "\n$alrProcfsVirtualizationProbe" +
            "\n\nALR W^X-safe exec strategy probe:" +
            "\n$alrWxSafeExecProbe" +
            "\n\nALR PRoot-vs-ALR perf comparison probe:" +
            "\n$alrPerfComparisonProbe" +
            "\n\nALR procfs interpose mechanism probe:" +
            "\n$alrInterposeProcfsProbe" +
            "\n\nALR memfd W^X-safe native exec probe:" +
            "\n$alrMemfdExecProbe" +
            "\n\nALR syscall sandbox capability probe:" +
            "\n$alrSyscallSandboxProbe" +
            "\n\nALR seccomp path-mediation probe (rootfs path rewrite via seccomp-trace):" +
            "\n$alrSeccompPathTrapProbe" +
            "\n\nALR Wayland unix-socket transport probe (named AF_UNIX host↔forked guest):" +
            "\n$alrUnixSocketProbe" +
            "\n\nALR execmem anon RX native exec probe:" +
            "\n$alrExecmemProbe" +
            "\n\nALR native ELF loader self-test (freestanding mechanism proof):" +
            "\n$alrNativeLoaderSelftest" +
            "\nalr native loader mt passed=$alrNativeLoaderMtPassed" +
            "\nalr native loader mt threads=${alrNativeLoaderMtProbe.lineStartingWith("alr native loader guest threads spawned=").substringAfter("spawned=", "")}" +
            "\nalr native loader mt emulated=${alrNativeLoaderMtProbe.lineStartingWith("alr native loader seccomp-emulated syscalls=").substringAfter("syscalls=", "")}" +
            "\nalr native loader mt stdout=${alrNativeLoaderMtProbe.lineStartingWith("alr native loader guest stdout=").substringAfter("stdout=", "")}" +
            "\n\nALR native ELF loader multi-process (threads+fork) probe:" +
            "\n$alrNativeLoaderMtProbe" +
            "\nalr native loader fileio mediated=$alrNativeLoaderFileioMediated" +
            "\n\nALR native ELF loader path-mediation real-file-read probe (fileio-test):" +
            "\n$alrNativeLoaderFileioProbe" +
            "\nalr native loader dyn passed=$alrNativeLoaderDynPassed" +
            "\n\nALR native ELF loader DYNAMIC (in-process ld.so handoff) probe (dynhello):" +
            "\n$alrNativeLoaderDynProbe" +
            "\n\nALR real Debian program probe (/usr/bin/env):" +
            "\n$alrLoaderRealEnvProbe" +
            "\n\nALR real Debian program probe (/usr/bin/id):" +
            "\n$alrLoaderRealIdProbe" +
            "\n\nALR real shell command probe (/bin/dash -c 'echo alr-shell-ok'):" +
            "\n$alrLoaderDashProbe" +
            "\n\nALR write-path mediation probe (dash writes+reads /tmp in rootfs):" +
            "\n$alrLoaderWriteProbe" +
            "\n\nALR in-process image decode probe (gdk-pixbuf PNG):" +
            "\n$alrLoaderPngProbe" +
            "\n\nALR GIMP 3.0 load probe (gimp-console-3.0 --version):" +
            "\n$alrLoaderGimpProbe" +
            "\n\nALR native ELF loader (anon-mmap-loader) probe:" +
            "\n$alrNativeLoaderProbe" +
            "\n\nALR guest->host GPU boundary cost probe:" +
            "\n$alrGpuBoundaryProbe" +
            "\n\nALR GPU command-marshalling probe (guest GLES stream -> host GPU):" +
            "\n$alrGpuMarshallingProbe" +
            "\n\nALR AHardwareBuffer zero-copy display probe (AHB -> EGLImage -> external-OES):" +
            "\n$alrAhbZeroCopyProbe" +
            "\n\nALR GPU-native draw probe (shader+VBO+texture+draw decoded on Mali):" +
            "\n$alrGpuDrawProbe" +
            "\n\nALR GPU-native ring probe (op stream via SPSC command ring -> Mali):" +
            "\n$alrGpuRingProbe" +
            "\n\nALR GPU-native AHB render-target probe (guest draw -> AHB-FBO -> external-OES sample):" +
            "\n$alrGpuFboProbe" +
            "\n\nAndroid host GPU probe:" +
            "\n$hostGpuProbe" +
            "\n\nAndroid host Vulkan probe:" +
            "\n$hostVulkanProbe" +
            "\n\nproot backend candidate: packaged native executable" +
            "\nproot actual env:" +
            prootCandidateResult.environment.entries.joinToString(separator = "") { "\n  ${it.key}=${it.value}" } +
            "\nproot candidate exit=${prootCandidateResult.exitCode}" +
            "\nproot candidate stdout=${prootCandidateResult.stdout}" +
            "\nproot candidate stderr=${prootCandidateResult.stderr}" +
            "\n\ndiagnostic note: libproot-loader.so is a PRoot loader executable, not a dlopen-able shared library; libtalloc.so is a dependency, not a standalone command. Direct crash probes are skipped in the default success report." +
            resultBlock("proot --version", prootCandidateResult) +
            resultBlock("proot -V", prootShortVersionResult) +
            resultBlock("proot --help", prootHelpResult) +
            resultBlock("proot no-env --version", prootNoEnvResult) +
            resultBlock("linker64 proot --version", prootViaLinkerResult) +
            resultBlock("proot hello quiet", prootHelloResult) +
            resultBlock("proot script", prootScriptResult) +
            resultBlock("proot shell -c", prootShellResult) +
            resultBlock("proot glibc", prootGlibcResult) +
            resultBlock("proot dash", prootDashResult) +
            resultBlock("proot id", prootIdResult) +
            resultBlock("proot dpkg --version", prootDpkgVersionResult) +
            resultBlock("proot dpkg --print-architecture", prootDpkgArchResult) +
            resultBlock("proot dpkg-query --version", prootDpkgQueryVersionResult) +
            resultBlock("proot dpkg-split --version", prootDpkgSplitVersionResult) +
            resultBlock("proot apt --version", prootAptVersionResult) +
            resultBlock("proot apt-get --version", prootAptGetVersionResult) +
            resultBlock("proot apt-cache --version", prootAptCacheVersionResult) +
            resultBlock("proot apt-config --version", prootAptConfigVersionResult) +
            resultBlock("proot dpkg -i local deb", prootDpkgInstallLocalResult) +
            resultBlock("proot installed package smoke", prootInstalledPackageSmokeResult) +
            resultBlock("proot guest gpu client", prootGuestGpuClientResult) +
            resultBlock("proot guest gpu ipc client", guestGpuIpcBridgeResult.clientResult) +
            resultBlock("proot guest gles shim smoke", prootGuestGlesShimSmokeResult) +
            resultBlock("proot guest wayland gui client", prootGuestWaylandGuiResult) +
            resultBlock("proot guest x11 gui client", prootGuestX11GuiResult) +
            resultBlock("proot guest wayland gui ipc client", guestWaylandGuiBridgeResult.clientResult) +
            resultBlock("proot guest x11 gui ipc client", guestX11GuiBridgeResult.clientResult) +
            optionalResultBlock("proot hello verbose on failure", prootHelloVerboseResult)

        val report = executionSummary + "\n\n--- verbose report ---\n" + verboseReport

        val view = TextView(this).apply {
            text = report
            textSize = 14f
            setPadding(32, 32, 32, 32)
            setTextIsSelectable(true)
        }
        val surfaceView = SurfaceView(this).apply {
            // Focusable for BOTH touch and hardware/IME keys so key events dispatch
            // to our setOnKeyListener (and the soft keyboard can target this view).
            isFocusableInTouchMode = true
            isFocusable = true
            // Mouse wheel / trackpad scroll -> wl_pointer.axis (M4). Touch-drag scroll
            // is handled by GTK from the wl_touch stream; this adds discrete scroll for
            // a real mouse/trackpad attached to the device.
            setOnGenericMotionListener { _, ev ->
                if (ev.actionMasked == android.view.MotionEvent.ACTION_SCROLL) {
                    val vs = ev.getAxisValue(android.view.MotionEvent.AXIS_VSCROLL)
                    val hs = ev.getAxisValue(android.view.MotionEvent.AXIS_HSCROLL)
                    // Wayland axis: +y = down, +x = right; Android VSCROLL +1 = scroll up.
                    if (vs != 0f) nativeWaylandInjectScroll(ev.x, ev.y, (-vs * 10.0), 0)
                    if (hs != 0f) nativeWaylandInjectScroll(ev.x, ev.y, (hs * 10.0), 1)
                    true
                } else false
            }
            // Production input path: forward real touches on the SurfaceView to the
            // focused Wayland client (as wl_touch + wl_pointer) so GUI apps are
            // interactive once a real toolkit window is up.
            setOnTouchListener { v, ev ->
                val phase = when (ev.actionMasked) {
                    android.view.MotionEvent.ACTION_DOWN,
                    android.view.MotionEvent.ACTION_POINTER_DOWN -> 0
                    android.view.MotionEvent.ACTION_UP,
                    android.view.MotionEvent.ACTION_POINTER_UP,
                    android.view.MotionEvent.ACTION_CANCEL -> 2
                    else -> 1
                }
                nativeWaylandInjectTouch(ev.getPointerId(ev.actionIndex), ev.x, ev.y, phase)
                if (phase == 0) {
                    // Tapping the GUI (re)claims key focus so hardware keys reach the
                    // guest. Do NOT auto-raise the soft keyboard here — it would steal
                    // focus and cover the lower half of the screen on every tap (e.g.
                    // swallowing menu-item clicks). The IME is raised only on demand
                    // (long-press) via showSoftKeyboard().
                    v.requestFocus()
                    v.performClick()
                }
                true
            }
            // Long-press toggles the on-screen keyboard on demand (for guest text
            // entry), without it hijacking ordinary taps.
            setOnLongClickListener { v ->
                v.requestFocus()
                showSoftKeyboard(v)
                true
            }
            // Hardware + soft-keyboard keys: translate Android KeyEvent.keyCode to a
            // Linux evdev keycode and inject as a wl_keyboard key. ACTION_DOWN=press,
            // ACTION_UP=release. Returning true for mapped keys consumes them so
            // Android doesn't also act on them; unmapped keys (e.g. BACK/VOLUME)
            // fall through to the system.
            setOnKeyListener { _, keyCode, ev ->
                val evdev = androidKeyToEvdev(keyCode)
                if (evdev == 0) {
                    false
                } else {
                    when (ev.action) {
                        KeyEvent.ACTION_DOWN -> { nativeWaylandInjectKey(evdev, 1); true }
                        KeyEvent.ACTION_UP -> { nativeWaylandInjectKey(evdev, 0); true }
                        else -> true
                    }
                }
            }
            holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(holder: SurfaceHolder) {
                    val encodedFrames = encodeSurfaceFrames(surfaceGpuCommands)
                    val vulkanSurfaceReport = nativeProbeVulkanSurface(holder.surface)
                    val vulkanSurfaceRenderReport = nativeRenderVulkanSurfaceFrames(holder.surface, encodedFrames)
                    val surfaceReport = nativeRenderGpuSurfaceFrames(holder.surface, encodedFrames)
                    // STEP B-1: a spinning textured cube through the LIVE GPU pipeline
                    // (guest op stream -> SPSC ring -> host executor -> AHB) presented
                    // onto this SurfaceView zero-copy via external-OES. In-process (no
                    // fork/rootfs) — the visible payoff of the v118 live backbone. Run
                    // synchronously here, before the compositor, so the single Surface
                    // is used sequentially; 60 frames (~1s) stays well inside the ANR window.
                    val cubeReport = nativeAlrGpuScreenCube(holder.surface, 60)
                    val cubeOk = cubeReport.lineStartingWith("ALR GPU SCREEN CUBE:") ==
                        "ALR GPU SCREEN CUBE: PASS"

                    // The summary was built before this callback, so its surface gates
                    // read PENDING_SURFACE_CALLBACK. Resolve them from the real render
                    // results now: hardware (non-software) renderer + all frames drawn
                    // with zero drops.
                    val vkProbeOk = vulkanSurfaceReport.contains("vulkan create android surface=ok") &&
                        vulkanSurfaceReport.contains("software renderer=false")
                    val vkRenderOk = vulkanSurfaceRenderReport.intFieldAfter("vulkan surface frames rendered=") > 0 &&
                        vulkanSurfaceRenderReport.intFieldAfter("vulkan surface frames dropped=") == 0 &&
                        vulkanSurfaceRenderReport.contains("software renderer=false")
                    val gpuFrames = surfaceReport.intFieldAfter("surface frames rendered=")
                    val gpuDropped = surfaceReport.intFieldAfter("surface frames dropped=")
                    val gpuSoftwareFalse = surfaceReport.contains("surface gpu software renderer=false")
                    val gpuSurfaceOk = gpuFrames > 0 && gpuDropped == 0 && gpuSoftwareFalse
                    val guiSurfaceOk = surfaceReport.intFieldAfter("surface gui total frames rendered=") > 0 &&
                        gpuDropped == 0 && gpuSoftwareFalse
                    fun gate(ok: Boolean) = if (ok) "PASS" else "FAIL"

                    var resolved = view.text.toString()
                    resolved = resolved.replaceFirst(
                        "ANDROID HOST VULKAN SURFACE PROBE EXECUTION: PENDING_SURFACE_CALLBACK",
                        "ANDROID HOST VULKAN SURFACE PROBE EXECUTION: ${gate(vkProbeOk)} after Surface callback",
                    )
                    resolved = resolved.replaceFirst(
                        "ANDROID HOST VULKAN SURFACE EXECUTION: PENDING_SURFACE_CALLBACK",
                        "ANDROID HOST VULKAN SURFACE EXECUTION: ${gate(vkRenderOk)} after Surface callback",
                    )
                    resolved = resolved.replaceFirst(
                        "HOST GPU SURFACE EXECUTION: PENDING_SURFACE_CALLBACK",
                        "HOST GPU SURFACE EXECUTION: ${gate(gpuSurfaceOk)} after Surface callback",
                    )
                    resolved = resolved.replaceFirst(
                        "GUEST GPU MULTI-FRAME SURFACE EXECUTION: PENDING_SURFACE_CALLBACK",
                        "GUEST GPU MULTI-FRAME SURFACE EXECUTION: ${gate(gpuSurfaceOk)} after Surface callback",
                    )
                    resolved = resolved.replaceFirst(
                        "GUEST GUI GPU SURFACE EXECUTION: PENDING_SURFACE_CALLBACK",
                        "GUEST GUI GPU SURFACE EXECUTION: ${gate(guiSurfaceOk)} after Surface callback",
                    )
                    view.text = resolved

                    view.append("\n\n--- Android host Vulkan surface probe ---\n$vulkanSurfaceReport")
                    view.append("\n\n--- Android host Vulkan surface renderer ---\n$vulkanSurfaceRenderReport")
                    view.append("\n\n--- Linux guest Wayland/X11 GUI GPU surface renderer ---\n$surfaceReport")
                    view.append("\n\nALR GPU SCREEN CUBE (live pipeline -> AHB -> external-OES on SurfaceView): ${if (cubeOk) "PASS" else "FAIL"}\n$cubeReport")

                    // Phase 3: stand up the in-app Wayland compositor on this
                    // Surface, then run a stock wl_shm guest client through the
                    // loader. Its committed buffer is uploaded by the compositor's
                    // EGL presenter and drawn onto this SurfaceView.
                    val dm = resources.displayMetrics
                    // Device-exact resolution + refresh for the guest's wl_output, so GUI
                    // apps see the real panel (1200x1920 @ 90Hz here) — not the hardcoded
                    // 60Hz / SurfaceView-derived approximation. getRealSize is rotation-
                    // aware (matches the fullscreen surface); refreshRate is the active mode.
                    val disp = if (android.os.Build.VERSION.SDK_INT >= 30) display
                        else @Suppress("DEPRECATION") windowManager.defaultDisplay
                    val realSize = android.graphics.Point()
                    @Suppress("DEPRECATION") disp?.getRealSize(realSize)
                    val outW = if (realSize.x > 0) realSize.x else dm.widthPixels
                    val outH = if (realSize.y > 0) realSize.y else dm.heightPixels
                    val refreshMhz = Math.round((disp?.refreshRate ?: 60f) * 1000f)
                    android.util.Log.i("alr_loader", "display: ${outW}x${outH} @ ${refreshMhz}mHz density=${dm.densityDpi}")
                    val wlStart = nativeWaylandCompositorStart(
                        cacheDir.absolutePath, holder.surface, dm.densityDpi, dm.xdpi, dm.ydpi,
                        outW, outH, refreshMhz)
                    val wlClient = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsManifest.name,
                        "/bin/alr-wl-test",
                    )
                    val wlStatus = nativeWaylandCompositorStatus()
                    val wlClientBound = wlClient.contains("alr-wl: bound")
                    val wlFramePresented = wlStatus.intFieldAfter("alr wl frames=") > 0
                    val wlCompositorUp = wlStart.contains("started") || wlStatus.contains("running")
                    // Real 2D toolkit rung: pixman (Cairo/X.Org/Qt's rasterizer)
                    // draws a gradient + AA circles + vector "ALR" text into a
                    // wl_shm buffer — a genuine toolkit, not a hand-filled color.
                    // Runs after wl-test so its richer frame is what's on screen.
                    val wlPixmanClient = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsManifest.name,
                        "/bin/alr-pixman-test",
                    )
                    val wlStatus2 = nativeWaylandCompositorStatus()
                    val wlPixmanDrew = wlPixmanClient.contains("alr-toolkit: pixman drew frame")
                    val wlToolkitPresented = wlStatus2.intFieldAfter("alr wl frames=") > wlStatus.intFieldAfter("alr wl frames=")
                    view.append("\n\nALR WAYLAND COMPOSITOR UP (libwayland-server on SurfaceView): ${gate(wlCompositorUp)}")
                    view.append("\nALR WAYLAND FIRST WINDOW (guest wl_shm client → compositor → SurfaceView): ${gate(wlClientBound && wlFramePresented)}")
                    view.append("\nALR REAL 2D TOOLKIT WINDOW (pixman gradient+AA shapes+text → SurfaceView): ${gate(wlPixmanDrew && wlToolkitPresented)}")
                    view.append("\n\n--- ALR Wayland compositor (Phase 3) ---\n$wlStart\n$wlStatus2")
                    view.append("\n\n--- ALR guest wl_shm client ---\n$wlClient")
                    view.append("\n\n--- ALR guest pixman 2D toolkit client ---\n$wlPixmanClient")

                    // Input injection (Stage B): run the wl_seat echo client on a
                    // background thread (it dispatches for ~6s), and inject a
                    // synthetic touch/pointer/key burst while it's waiting. The
                    // client counts the events it receives → proves Android input
                    // reaches a Wayland client through the compositor.
                    Thread {
                        val inputClient = nativeAlrNativeLoaderProbe(
                            packageName,
                            applicationInfo.nativeLibraryDir,
                            filesDir.absolutePath,
                            cacheDir.absolutePath,
                            rootfsManifest.name,
                            "/bin/alr-input-test",
                        )
                        val received = Regex("received (\\d+) input events")
                            .find(inputClient)?.groupValues?.get(1)?.toIntOrNull() ?: 0
                        runOnUiThread {
                            view.append(
                                "\nALR WAYLAND INPUT INJECTION (Android→wl_seat→client): " +
                                    "${gate(received > 0)} (received=$received)",
                            )
                            view.append("\n\n--- ALR guest wl_seat input-echo client ---\n$inputClient")
                        }
                    }.start()
                    // Fire the synthetic bursts after the echo client has connected
                    // and mapped (so a focus surface exists), with a couple retries.
                    val injX = 120f
                    val injY = 120f
                    view.postDelayed({ nativeWaylandInjectSelfTest(injX, injY) }, 1800)
                    view.postDelayed({ nativeWaylandInjectSelfTest(injX + 40f, injY + 40f) }, 2600)
                    view.postDelayed({ nativeWaylandInjectSelfTest(injX + 80f, injY + 80f) }, 3400)

                    // Interactive loop (Stage B end-to-end): start the pixman
                    // interactive client after the echo client has finished (~7s),
                    // then inject moving points — the client redraws a circle that
                    // follows the cursor and commits a new frame per event, proving
                    // input → client logic → render → display on the SurfaceView.
                    view.postDelayed({
                        Thread {
                            val interClient = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                "/bin/alr-interactive-test",
                            )
                            val redraws = Regex("redraws=(\\d+)")
                                .find(interClient)?.groupValues?.get(1)?.toIntOrNull() ?: 0
                            val hits = Regex("hits=(\\d+)")
                                .find(interClient)?.groupValues?.get(1)?.toIntOrNull() ?: 0
                            runOnUiThread {
                                view.append(
                                    "\nALR WAYLAND INTERACTIVE TOOLKIT (input→redraw→display loop): " +
                                        "${gate(redraws > 1 && hits > 0)} (redraws=$redraws hits=$hits)",
                                )
                                view.append("\n\n--- ALR guest interactive pixman client ---\n$interClient")
                            }
                        }.start()
                        // Inject moving points so the followed circle visibly moves.
                        view.postDelayed({ nativeWaylandInjectTouch(0, 180f, 160f, 0) }, 1500)
                        view.postDelayed({ nativeWaylandInjectTouch(0, 360f, 240f, 1) }, 2200)
                        view.postDelayed({ nativeWaylandInjectTouch(0, 480f, 360f, 1) }, 2900)
                        view.postDelayed({ nativeWaylandInjectTouch(0, 480f, 360f, 2) }, 3300)
                    }, 7000)

                    // Phase 6 headline: a REAL GTK3 application window. The merged
                    // rootfs ships the full GTK3 library closure (~50 .so) + service
                    // files (gschemas/fontconfig/gdk-pixbuf/icon-theme). gtk_init +
                    // a 640x480 toplevel rendered with Cairo into wl_shm → composited
                    // onto the SurfaceView. Runs last (after the other clients), heavy.
                    view.postDelayed({
                        Thread {
                            val gtkClient = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                "/bin/alr-gtk3-test",
                            )
                            val gtkRealized = gtkClient.contains("alr-gtk3: realized window")
                            val gtkStatus = nativeWaylandCompositorStatus()
                            runOnUiThread {
                                view.append(
                                    "\nALR REAL GTK3 WINDOW (gtk_init→toplevel→cairo→SurfaceView): " +
                                        "${gate(gtkRealized)}",
                                )
                                view.append("\n\n--- ALR Wayland compositor (after GTK3) ---\n$gtkStatus")
                                view.append("\n\n--- ALR guest GTK3 client ---\n$gtkClient")
                            }

                            // Goal-2 universality HEADLINE: a LIGHTWEIGHT real Linux
                            // GUI app — `foot`, a Wayland-native terminal. Only ~1-2
                            // threads (vs Chromium's ~22), so it clears the multithread
                            // ptrace-overhead wall that blocks Chromium's render. First
                            // the cheap `--version` (proves ld.so resolves libfcft4 +
                            // libutf8proc + the rootfs libs and the binary links), then
                            // `foot -e /bin/dash` opens a REAL terminal running the
                            // rootfs shell: foot binds the compositor globals, CPU-renders
                            // glyphs into wl_shm → the WaylandPresenter uploads to the
                            // SurfaceView. Success = a frame committed (counter advances).
                            val footVersion = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                "/usr/bin/foot\n--version",
                            )
                            val footVersionOk = footVersion.contains("foot version")
                            val framesBeforeFoot = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
                            val footGuiClient = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                "/usr/bin/foot\n-e\n/bin/dash",
                            )
                            val footStatus = nativeWaylandCompositorStatus()
                            val framesAfterFoot = footStatus.intFieldAfter("alr wl frames=")
                            val footRendered = framesAfterFoot > framesBeforeFoot
                            android.util.Log.i("alr_loader", "foot-result: rendered=$footRendered frames=$framesBeforeFoot->$framesAfterFoot ver=$footVersionOk")
                            android.util.Log.i("alr_loader", "foot-version:\n$footVersion")
                            android.util.Log.i("alr_loader", "foot-terminal:\n$footGuiClient")
                            android.util.Log.i("alr_loader", "foot-status:\n$footStatus")
                            runOnUiThread {
                                view.append(
                                    "\nALR FOOT TERMINAL (lightweight Wayland app: foot -e dash → wl_shm → SurfaceView): " +
                                        "${gate(footRendered)} (frames $framesBeforeFoot→$framesAfterFoot, ver=${gate(footVersionOk)})",
                                )
                                view.append("\n\n--- ALR guest foot --version ---\n$footVersion")
                                view.append("\n\n--- ALR guest foot terminal (foot -e /bin/dash) ---\n$footGuiClient")
                            }

                            // Goal-2 universality 2nd lightweight app: gtk3-widget-factory,
                            // a real GTK3 showcase (every widget type). Different code path
                            // than foot, pty-free, Wayland-native via GDK. Renders its
                            // toplevel into wl_shm → SurfaceView, same as GIMP but far
                            // lighter — proving the universal path isn't foot-specific.
                            val framesBeforeGtkDemo = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
                            val gtkDemoClient = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                "/usr/bin/gtk3-widget-factory",
                            )
                            val gtkDemoStatus = nativeWaylandCompositorStatus()
                            val framesAfterGtkDemo = gtkDemoStatus.intFieldAfter("alr wl frames=")
                            val gtkDemoRendered = framesAfterGtkDemo > framesBeforeGtkDemo
                            android.util.Log.i("alr_loader", "gtkdemo-result: rendered=$gtkDemoRendered frames=$framesBeforeGtkDemo->$framesAfterGtkDemo")
                            android.util.Log.i("alr_loader", "gtkdemo-client:\n$gtkDemoClient")
                            android.util.Log.i("alr_loader", "gtkdemo-status:\n$gtkDemoStatus")
                            runOnUiThread {
                                view.append(
                                    "\nALR GTK3 WIDGET-FACTORY (lightweight GTK3 app → wl_shm → SurfaceView): " +
                                        "${gate(gtkDemoRendered)} (frames $framesBeforeGtkDemo→$framesAfterGtkDemo)",
                                )
                                view.append("\n\n--- ALR guest gtk3-widget-factory ---\n$gtkDemoClient")
                            }

                            // CP-2 GPU 풀가속: glmark2-es2-wayland를 ALR loader로 실행.
                            // 게스트 libGLESv2 shim이 GL을 GpuRingHook ring으로 emit →
                            // host GpuExecutorService가 Mali GLES2로 replay → glmark2 score =
                            // 실제 Mali GPU 가속. loader가 config.program=glmark2 감지 →
                            // alr_loader_attach_gpu_ring + LD_LIBRARY_PATH에 shim 우선.
                            val framesBeforeGlmark2 = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
                            val glmark2Client = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                // build (geometry) + texture (exercises the 8MiB ring: a 1024x1024
                                // RGBA = 4MiB single OP_TEX_IMAGE_2D the old 1MiB ring dropped).
                                // duration capped so both scenes finish inside the guest alarm(25s).
                                "/usr/bin/glmark2-es2-wayland\n--data-path\n/usr/share/glmark2\n--benchmark\nbuild:duration=5\n--benchmark\ntexture:duration=5",
                            )
                            val glmark2Status = nativeWaylandCompositorStatus()
                            val framesAfterGlmark2 = glmark2Status.intFieldAfter("alr wl frames=")
                            android.util.Log.i("alr_loader", "glmark2-result: frames $framesBeforeGlmark2->$framesAfterGlmark2")
                            android.util.Log.i("alr_loader", "glmark2-client:\n$glmark2Client")
                            runOnUiThread {
                                view.append(
                                    "\nALR GLMARK2 (GLES2 shim → ring → Mali executor): frames $framesBeforeGlmark2→$framesAfterGlmark2",
                                )
                                view.append("\n\n--- ALR guest glmark2-es2-wayland ---\n$glmark2Client")
                            }

                            // Phase 6 FINAL: run real GIMP 3.0 on the compositor.
                            // gimp-3.0 (ET_DYN PIE) runs the full GUI; it renders
                            // its main window into wl_shm → the SurfaceView. The
                            // loader watchdog stops the long-running GUI; success =
                            // GIMP committed at least one frame (the compositor's
                            // frame counter advances) before then.
                            val framesBeforeGimp = gtkStatus.intFieldAfter("alr wl frames=")
                            val gimpGuiClient = nativeAlrNativeLoaderProbe(
                                packageName,
                                applicationInfo.nativeLibraryDir,
                                filesDir.absolutePath,
                                cacheDir.absolutePath,
                                rootfsManifest.name,
                                "/usr/bin/gimp-3.0",
                            )
                            val gimpStatus = nativeWaylandCompositorStatus()
                            val framesAfterGimp = gimpStatus.intFieldAfter("alr wl frames=")
                            val gimpRendered = framesAfterGimp > framesBeforeGimp
                            runOnUiThread {
                                view.append(
                                    "\nALR GIMP 3.0 GUI (gimp-3.0 → main window → SurfaceView): " +
                                        "${gate(gimpRendered)} (frames $framesBeforeGimp→$framesAfterGimp)",
                                )
                                view.append("\n\n--- ALR guest GIMP 3.0 GUI ---\n$gimpGuiClient")
                            }
                        }.start()
                    }, 14000)
                }

                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

                override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
            })
        }
        setContentView(
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                // Fullscreen GUI: the Wayland/GIMP output fills the SurfaceView,
                // which fills the screen. A fullscreen client then maps 1:1 with
                // injected touch coordinates (no band letterboxing to skew them).
                addView(
                    surfaceView,
                    LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f),
                )
                // Keep the report attached (so view.postDelayed drives the GUI
                // launch sequence and view.append still works) but out of the way
                // as a 1px strip under the fullscreen surface.
                addView(
                    ScrollView(this@MainActivity).apply { addView(view) },
                    LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1),
                )
            },
        )
        applyImmersive()
        // Claim key focus up front so hardware keyboard input reaches the guest
        // even before the first touch. (post: run after layout so the view tree is
        // attached and requestFocus actually takes.)
        surfaceView.post { surfaceView.requestFocus() }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // Android re-shows the system bars after dialogs/notifications steal focus;
        // re-hide them whenever we regain focus so the Linux GUI stays edge-to-edge.
        if (hasFocus) applyImmersive()
    }

    // WS-4 §10(b): run apt/dpkg/X11 FUNCTIONALLY through the ALR native loader and emit
    // the result to logcat (tag alr_loader), so the integration device drain can capture
    // real functional evidence — not just the overlay "staged" markers. Each program is
    // run with the same newline-delimited-argv loader probe the foot/gtkdemo/glmark2
    // launches use. No display is needed: dpkg-query/apt-get/Xwayland all have a pure
    // --version path that exits immediately. Heavy + depends on the dpkg-db/x11/apt-config
    // overlays being staged (the toolkit-stage thread above), so this runs on its own
    // background thread and waits (bounded) for the staged binaries to appear.
    private fun launchPackageManagerProbes(rootfsDir: File, rootfsName: String) {
        Thread {
            try {
                // dpkg-query/apt-get ship in the base; Xwayland comes from the x11 overlay.
                // Wait (bounded) for the staged binaries so we don't probe before the
                // concurrent toolkit-stage thread has extracted them.
                val dpkgQueryBin = File(rootfsDir, "usr/bin/dpkg-query")
                val aptGetBin = File(rootfsDir, "usr/bin/apt-get")
                val xwaylandBin = File(rootfsDir, "usr/bin/Xwayland")
                var waited = 0
                while (waited < 20000 &&
                    !(dpkgQueryBin.isFile && aptGetBin.isFile && xwaylandBin.isFile)
                ) {
                    Thread.sleep(500)
                    waited += 500
                }
                fun probe(label: String, program: String, okMarker: String) {
                    val out = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        program,
                    )
                    val exec = out.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:")
                    val ok = out.contains(okMarker)
                    android.util.Log.i(
                        "alr_loader",
                        "pkgfunc-$label: ok=$ok exec=[$exec] marker=[$okMarker]",
                    )
                    android.util.Log.i("alr_loader", "pkgfunc-$label-out:\n$out")
                }
                // (b1) dpkg admin DB query: --version proves the binary runs through the
                // loader; --list (paged via the reconstructed status DB) proves the
                // dpkg-db overlay is a real admin DB dpkg can read.
                android.util.Log.i(
                    "alr_loader",
                    "pkgfunc: dpkg-query=${dpkgQueryBin.isFile} apt-get=${aptGetBin.isFile} " +
                        "Xwayland=${xwaylandBin.isFile} (waited ${waited}ms)",
                )
                probe("dpkg-query-version", "/usr/bin/dpkg-query\n--version", "Debian dpkg-query")
                probe("dpkg-query-list", "/usr/bin/dpkg-query\n-l\nlibc6", "libc6")
                // (b2) apt: --version proves libapt-pkg loads + apt runs through the loader.
                probe("apt-get-version", "/usr/bin/apt-get\n--version", "apt ")
                // (b3) X11: Xwayland -version prints the X server version and exits without a
                // display — the no-display functional check for the x11 overlay (rootful
                // launch-on-compositor is L2/L3 integration, not this WS).
                probe("xwayland-version", "/usr/bin/Xwayland\n-version", "Xwayland")
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "pkgfunc EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
    }

    // Hide Android's status + navigation bars (swipe to reveal) so a desktop-class
    // Linux app like GIMP owns the whole screen. MUST run after setContentView so
    // the DecorView/insetsController exists (calling it in onCreate before the
    // decor is built throws NPE). The device's real metrics drive the Wayland
    // output resolution/DPI separately, at compositor start.
    private fun applyImmersive() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.decorView.windowInsetsController?.let { c ->
                c.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                c.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                    View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
        }
    }

    // Pop up the soft keyboard targeting the GUI SurfaceView, so on-screen typing
    // also reaches the guest (the hardware/dispatch path works regardless). Best
    // effort: harmless if no IME is present.
    private fun showSoftKeyboard(target: View) {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(target, InputMethodManager.SHOW_IMPLICIT)
    }

    // Translate an Android KeyEvent.keyCode into a Linux evdev keycode
    // (<linux/input-event-codes.h>), which the compositor forwards verbatim as a
    // wl_keyboard key (the guest applies its own default keymap). Returns 0 for keys
    // we don't map, so the caller lets Android handle them (BACK, VOLUME, etc.).
    private fun androidKeyToEvdev(keyCode: Int): Int = when (keyCode) {
        // Letters A..Z -> KEY_A..KEY_Z (evdev rows, not alphabetical).
        KeyEvent.KEYCODE_A -> 30
        KeyEvent.KEYCODE_B -> 48
        KeyEvent.KEYCODE_C -> 46
        KeyEvent.KEYCODE_D -> 32
        KeyEvent.KEYCODE_E -> 18
        KeyEvent.KEYCODE_F -> 33
        KeyEvent.KEYCODE_G -> 34
        KeyEvent.KEYCODE_H -> 35
        KeyEvent.KEYCODE_I -> 23
        KeyEvent.KEYCODE_J -> 36
        KeyEvent.KEYCODE_K -> 37
        KeyEvent.KEYCODE_L -> 38
        KeyEvent.KEYCODE_M -> 50
        KeyEvent.KEYCODE_N -> 49
        KeyEvent.KEYCODE_O -> 24
        KeyEvent.KEYCODE_P -> 25
        KeyEvent.KEYCODE_Q -> 16
        KeyEvent.KEYCODE_R -> 19
        KeyEvent.KEYCODE_S -> 31
        KeyEvent.KEYCODE_T -> 20
        KeyEvent.KEYCODE_U -> 22
        KeyEvent.KEYCODE_V -> 47
        KeyEvent.KEYCODE_W -> 17
        KeyEvent.KEYCODE_X -> 45
        KeyEvent.KEYCODE_Y -> 21
        KeyEvent.KEYCODE_Z -> 44
        // Digits 0..9 -> KEY_0..KEY_9 (KEY_1=2..KEY_9=10, KEY_0=11).
        KeyEvent.KEYCODE_0 -> 11
        KeyEvent.KEYCODE_1 -> 2
        KeyEvent.KEYCODE_2 -> 3
        KeyEvent.KEYCODE_3 -> 4
        KeyEvent.KEYCODE_4 -> 5
        KeyEvent.KEYCODE_5 -> 6
        KeyEvent.KEYCODE_6 -> 7
        KeyEvent.KEYCODE_7 -> 8
        KeyEvent.KEYCODE_8 -> 9
        KeyEvent.KEYCODE_9 -> 10
        // Whitespace / editing / control.
        KeyEvent.KEYCODE_SPACE -> 57            // KEY_SPACE
        KeyEvent.KEYCODE_ENTER -> 28            // KEY_ENTER
        KeyEvent.KEYCODE_DEL -> 14              // KEY_BACKSPACE (Android DEL == backspace)
        KeyEvent.KEYCODE_FORWARD_DEL -> 111     // KEY_DELETE
        KeyEvent.KEYCODE_TAB -> 15              // KEY_TAB
        KeyEvent.KEYCODE_ESCAPE -> 1            // KEY_ESC
        KeyEvent.KEYCODE_CTRL_LEFT -> 29        // KEY_LEFTCTRL
        KeyEvent.KEYCODE_CTRL_RIGHT -> 97       // KEY_RIGHTCTRL
        KeyEvent.KEYCODE_SHIFT_LEFT -> 42       // KEY_LEFTSHIFT
        KeyEvent.KEYCODE_SHIFT_RIGHT -> 54      // KEY_RIGHTSHIFT
        KeyEvent.KEYCODE_ALT_LEFT -> 56         // KEY_LEFTALT
        KeyEvent.KEYCODE_ALT_RIGHT -> 100       // KEY_RIGHTALT
        // Punctuation commonly needed for typing/paths.
        KeyEvent.KEYCODE_MINUS -> 12            // KEY_MINUS
        KeyEvent.KEYCODE_EQUALS -> 13           // KEY_EQUAL
        KeyEvent.KEYCODE_LEFT_BRACKET -> 26     // KEY_LEFTBRACE
        KeyEvent.KEYCODE_RIGHT_BRACKET -> 27    // KEY_RIGHTBRACE
        KeyEvent.KEYCODE_BACKSLASH -> 43        // KEY_BACKSLASH
        KeyEvent.KEYCODE_SEMICOLON -> 39        // KEY_SEMICOLON
        KeyEvent.KEYCODE_APOSTROPHE -> 40       // KEY_APOSTROPHE
        KeyEvent.KEYCODE_GRAVE -> 41            // KEY_GRAVE
        KeyEvent.KEYCODE_COMMA -> 51            // KEY_COMMA
        KeyEvent.KEYCODE_PERIOD -> 52           // KEY_DOT
        KeyEvent.KEYCODE_SLASH -> 53            // KEY_SLASH
        // Arrows.
        KeyEvent.KEYCODE_DPAD_LEFT -> 105       // KEY_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> 106      // KEY_RIGHT
        KeyEvent.KEYCODE_DPAD_UP -> 103         // KEY_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> 108       // KEY_DOWN
        else -> 0
    }

    private data class GuestGpuCommand(
        val red: Float,
        val green: Float,
        val blue: Float,
        val tag: String,
        val protocol: String = "GPU",
        val seq: Int = 0,
    )

    private data class GuestGpuIpcBridgeResult(
        val host: String,
        val port: Int,
        val expectedFrames: Int,
        val commands: List<GuestGpuCommand>,
        val rawLines: List<String>,
        val error: String?,
        val clientResult: NativeCommandResult,
    )

    private fun runGuestGpuIpcBridge(
        nativeCommandRunner: NativeCommandRunner,
        rootfsDir: File,
    ): GuestGpuIpcBridgeResult {
        val host = "127.0.0.1"
        val server = ServerSocket(0, 1, InetAddress.getByName(host)).apply { soTimeout = 3000 }
        val port = server.localPort
        val rawLines = mutableListOf<String>()
        val errors = mutableListOf<String>()
        val acceptThread = thread(name = "alr-gpu-ipc-bridge", start = true) {
            try {
                server.use { srv ->
                    val socket = srv.accept()
                    socket.use { accepted ->
                        accepted.soTimeout = 3000
                        accepted.getInputStream().bufferedReader().useLines { lines ->
                            lines.forEach { rawLines += it }
                        }
                    }
                }
            } catch (error: SocketTimeoutException) {
                errors += "timeout waiting for guest gpu ipc client"
            } catch (error: Exception) {
                errors += error.javaClass.simpleName + ": " + (error.message ?: "unknown")
            }
        }
        val clientResult = nativeCommandRunner.runProotRootfsGuestGpuClientIpc(rootfsDir, port)
        acceptThread.join(3500)
        if (acceptThread.isAlive) {
            errors += "accept thread still alive after join"
            server.close()
        }
        val commands = parseGuestGpuCommands(rawLines.joinToString("\n"))
        val expectedFrames = rawLines.firstOrNull { it.startsWith("ALR_GPU_IPC_HELLO ") }
            ?.substringAfter("frames=", "0")
            ?.substringBefore(" ")
            ?.toIntOrNull()
            ?: commands.size
        return GuestGpuIpcBridgeResult(
            host = host,
            port = port,
            expectedFrames = expectedFrames,
            commands = commands,
            rawLines = rawLines.toList(),
            error = errors.firstOrNull(),
            clientResult = clientResult,
        )
    }


    private fun runGuestGuiBridge(
        nativeCommandRunner: NativeCommandRunner,
        rootfsDir: File,
        protocol: String,
    ): GuestGpuIpcBridgeResult {
        val host = "127.0.0.1"
        val server = ServerSocket(0, 1, InetAddress.getByName(host)).apply { soTimeout = 3000 }
        val port = server.localPort
        val rawLines = mutableListOf<String>()
        val errors = mutableListOf<String>()
        val acceptThread = thread(name = "alr-gui-ipc-bridge-$protocol", start = true) {
            try {
                server.use { srv ->
                    val socket = srv.accept()
                    socket.use { accepted ->
                        accepted.soTimeout = 750
                        val reader = accepted.getInputStream().bufferedReader()
                        var expectedFrames = 0
                        while (true) {
                            val line = try {
                                reader.readLine()
                            } catch (timeout: SocketTimeoutException) {
                                if (expectedFrames > 0 && rawLines.count { it.startsWith("ALR_GUI_FRAME ") } >= expectedFrames) {
                                    null
                                } else {
                                    throw timeout
                                }
                            } ?: break
                            rawLines += line
                            if (line.startsWith("ALR_GUI_IPC_HELLO ")) {
                                expectedFrames = line.substringAfter("frames=", "0")
                                    .substringBefore(" ")
                                    .toIntOrNull()
                                    ?: 0
                            }
                            if (expectedFrames > 0 && rawLines.count { it.startsWith("ALR_GUI_FRAME ") } >= expectedFrames) {
                                break
                            }
                        }
                        val receivedFrames = rawLines.count { it.startsWith("ALR_GUI_FRAME ") }
                        val lossless = expectedFrames > 0 && receivedFrames == expectedFrames
                        val ack = "ALR_GUI_IPC_ACK protocol=$protocol received=$receivedFrames expected=$expectedFrames lossless=$lossless\n"
                        accepted.getOutputStream().write(ack.toByteArray())
                        accepted.getOutputStream().flush()
                    }
                }
            } catch (error: SocketTimeoutException) {
                errors += "timeout waiting for guest gui ipc client $protocol"
            } catch (error: Exception) {
                errors += error.javaClass.simpleName + ": " + (error.message ?: "unknown")
            }
        }
        val clientResult = nativeCommandRunner.runProotRootfsGuestGuiClientIpc(rootfsDir, protocol, port)
        acceptThread.join(3500)
        if (acceptThread.isAlive) {
            errors += "gui accept thread still alive after join $protocol"
            server.close()
        }
        val commands = parseGuestGuiCommands(rawLines.joinToString("\n"), protocol)
        val expectedFrames = rawLines.firstOrNull { it.startsWith("ALR_GUI_IPC_HELLO ") }
            ?.substringAfter("frames=", "0")
            ?.substringBefore(" ")
            ?.toIntOrNull()
            ?: commands.size
        return GuestGpuIpcBridgeResult(
            host = host,
            port = port,
            expectedFrames = expectedFrames,
            commands = commands,
            rawLines = rawLines.toList(),
            error = errors.firstOrNull(),
            clientResult = clientResult,
        )
    }

    private fun parseGuestGpuCommands(text: String): List<GuestGpuCommand> =
        text.lineSequence()
            .filter { it.startsWith("ALR_GPU_CLEAR ") }
            .mapNotNull { parseGuestGpuClearLine(it) }
            .toList()

    private fun parseGuestGuiCommands(text: String, expectedProtocol: String): List<GuestGpuCommand> =
        text.lineSequence()
            .filter { it.startsWith("ALR_GUI_FRAME ") }
            .mapNotNull { parseGuestGuiFrameLine(it, expectedProtocol) }
            .toList()

    private fun parseGuestGuiFrameLine(line: String, expectedProtocol: String): GuestGpuCommand? {
        val parts = line.trim().split(Regex("\\s+"))
        if (parts.size < 7 || parts[0] != "ALR_GUI_FRAME") return null
        val protocol = parts[1]
        if (protocol != expectedProtocol) return null
        val seq = parts[2].substringAfter("seq=", "0").toIntOrNull() ?: return null
        val red = parts[3].toFloatOrNull()?.coerceIn(0f, 1f) ?: return null
        val green = parts[4].toFloatOrNull()?.coerceIn(0f, 1f) ?: return null
        val blue = parts[5].toFloatOrNull()?.coerceIn(0f, 1f) ?: return null
        return GuestGpuCommand(red, green, blue, parts[6], protocol, seq)
    }

    private fun parseGuestGlesShimCommand(stdout: String): GuestGpuCommand? =
        stdout.lineSequence()
            .firstOrNull { it.startsWith("ALR_GLES_SHIM_COMMAND ALR_GPU_CLEAR ") }
            ?.removePrefix("ALR_GLES_SHIM_COMMAND ")
            ?.let { parseGuestGpuClearLine(it) }

    private fun parseGuestGpuClearLine(line: String): GuestGpuCommand? {
        val parts = line.trim().split(Regex("\\s+"))
        if (parts.size < 5 || parts[0] != "ALR_GPU_CLEAR") return null
        val red = parts[1].toFloatOrNull()?.coerceIn(0f, 1f) ?: return null
        val green = parts[2].toFloatOrNull()?.coerceIn(0f, 1f) ?: return null
        val blue = parts[3].toFloatOrNull()?.coerceIn(0f, 1f) ?: return null
        return GuestGpuCommand(red, green, blue, parts[4])
    }

    private fun encodeSurfaceFrames(commands: List<GuestGpuCommand>): String =
        commands.joinToString(separator = "\n") { "${it.red} ${it.green} ${it.blue} ${it.protocol}-seq${it.seq}-${it.tag}" }

    private fun guiSeqGaps(commands: List<GuestGpuCommand>, expectedFrames: Int): Int {
        if (expectedFrames <= 0) return 0
        val seen = commands.map { it.seq }.toSet()
        return (1..expectedFrames).count { it !in seen }
    }

    private fun guiDuplicateSeqCount(commands: List<GuestGpuCommand>): Int =
        commands.groupingBy { it.seq }.eachCount().values.sumOf { (it - 1).coerceAtLeast(0) }

    private fun guiOutOfOrder(commands: List<GuestGpuCommand>): Boolean =
        commands.map { it.seq }.zipWithNext().any { (left, right) -> right < left }

    private fun resultBlock(label: String, result: NativeCommandResult): String =
        "\n\n$label command=${result.command.absolutePath}" +
            "\n$label exit=${result.exitCode}" +
            "\n$label stdout=${result.stdout}" +
            "\n$label stderr=${result.stderr}"

    private fun optionalResultBlock(label: String, result: NativeCommandResult?): String =
        result?.let { resultBlock(label, it) } ?: "\n\n$label skipped=quiet rootfs execution passed"

    private fun String.lineStartingWith(prefix: String): String =
        lineSequence().firstOrNull { it.startsWith(prefix) } ?: "missing"

    private fun String.intFieldAfter(key: String): Int =
        lineSequence().firstOrNull { it.contains(key) }
            ?.substringAfter(key)
            ?.takeWhile { it.isDigit() }
            ?.toIntOrNull() ?: -1

    private fun requestedPermissionNames(): Set<String> =
        packageManager.getPackageInfo(packageName, PackageManager.GET_PERMISSIONS)
            .requestedPermissions
            ?.toSet()
            .orEmpty()

    private external fun nativeRuntimeReport(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeLibraryProbe(nativeLibraryDir: String): String

    private external fun nativeAlrTrampolineContinueProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrProcfsVirtualizationProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrWxSafeExecProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrPerfComparisonProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrInterposeProcfsProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrMemfdExecProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrSyscallSandboxProbe(): String

    private external fun nativeAlrSeccompPathTrapProbe(
        appFilesDir: String,
        appCacheDir: String,
    ): String

    private external fun nativeAlrUnixSocketProbe(
        appFilesDir: String,
        appCacheDir: String,
    ): String

    private external fun nativeAlrExecmemProbe(): String

    private external fun nativeAlrGpuBoundaryProbe(): String

    private external fun nativeAlrNativeLoaderProbe(
        packageName: String,
        nativeLibraryDir: String,
        appFilesDir: String,
        appCacheDir: String,
        rootfsName: String,
        program: String,
    ): String

    private external fun nativeAlrNativeLoaderSelftest(): String

    private external fun nativeAlrGpuMarshallingProbe(): String

    private external fun nativeAlrAhbZeroCopyProbe(): String

    private external fun nativeAlrGpuDrawProbe(): String

    private external fun nativeAlrGpuRingProbe(): String

    private external fun nativeAlrGpuFboProbe(): String

    private external fun nativeAlrGpuLiveProbe(): String
    private external fun nativeAlrGpuThroughputProbe(): String

    private external fun nativeJitWxProbe(): String

    private external fun nativeAlrGpuScreenCube(surface: android.view.Surface, frames: Int): String

    private external fun nativeHostGpuProbe(): String

    private external fun nativeHostVulkanProbe(): String

    private external fun nativeProbeVulkanSurface(surface: android.view.Surface): String

    private external fun nativeWaylandCompositorStart(
        cacheDir: String,
        surface: android.view.Surface,
        densityDpi: Int,
        xdpi: Float,
        ydpi: Float,
        outWidthPx: Int,
        outHeightPx: Int,
        refreshMilliHz: Int,
    ): String

    private external fun nativeWaylandCompositorStatus(): String

    private external fun nativeWaylandCompositorStop(): String

    private external fun nativeWaylandInjectSelfTest(x: Float, y: Float): String

    private external fun nativeWaylandInjectTouch(id: Int, x: Float, y: Float, phase: Int)

    private external fun nativeWaylandInjectKey(evdevKey: Int, pressed: Int)
    private external fun nativeWaylandInjectScroll(x: Float, y: Float, value: Double, axis: Int)

    private external fun nativeRenderVulkanSurfaceFrames(
        surface: android.view.Surface,
        encodedFrames: String,
    ): String

    private external fun nativeRenderGpuSurfaceFrames(
        surface: android.view.Surface,
        encodedFrames: String,
    ): String
}
