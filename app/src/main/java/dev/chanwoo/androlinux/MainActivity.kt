package dev.chanwoo.androlinux

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.os.Handler
import androidx.annotation.Keep
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
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
    // --- Android soft-keyboard IME <-> guest zwp_text_input_v3 state ---------
    // Driven by the compositor's onGuestImeState upcall (a guest text field
    // enabled/disabled text input). imeWanted gates onCheckIsTextEditor so the IME
    // only treats the SurfaceView as an editor when a guest field has focus;
    // imeInputType is recomputed from the guest content_purpose/hint. The
    // SurfaceView the IME binds to is stashed here so onGuestImeState (on the UI
    // thread, posted from the compositor thread) can restartInput / show / hide it.
    @Volatile private var imeWanted = false
    @Volatile private var imeInputType = InputType.TYPE_CLASS_TEXT
    private var imeSurfaceView: SurfaceView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        System.loadLibrary("alr_loader")

        // Standalone Chromium app entry (launched via the .ui.ChromiumStandalone
        // activity-alias "ALR Chromium", or the adb marker /data/local/tmp/.alr-cronly):
        // a LEAN chromium-only path that SKIPS the entire heavy MainActivity onCreate
        // (GPU live executor + ~40 native probes + 17 toolkit overlays + GIMP). The full
        // app baseline (~400MB resident before chromium even starts) was OOM-ing the full
        // GUI chromium browser (browser + renderer, each a ~800MB in-process re-map) on
        // this 5.5GB device. Stripping the baseline to ~just the compositor frees the
        // headroom chromium needs. Reuses the SAME native loader + Wayland compositor +
        // rootfs as MainActivity (zero native change).
        if (componentName.className.endsWith("ChromiumStandalone") ||
            File("/data/local/tmp/.alr-cronly").isFile
        ) {
            runChromiumStandalone()
            return
        }

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
        // CP-6 un-deferred (user direction 2026-06-02): chromium-headless-shell --version
        // loads the full chromium closure in-process so the M-R2 (ADR-002) supervisor
        // syscall-mix instrumentation captures its 'alr sc trace_hist/emul_hist/stime'
        // distribution. Gated on chromium-stage.tar being adb-push'd to /data/local/tmp
        // (the 541MB tar is not auto-present). --dump-dom (the heavier render storm) still
        // hits the multithread ptrace deadlock (backlogged) — --version is the working path.
        if (java.io.File("/data/local/tmp/chromium-stage.tar").isFile) Thread {
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
                // CR-1 (chromium-run-plan): the engine renders a page, SINGLE-PROCESS,
                // --dump-dom of a self-contained data: URL (no net/GPU/multiprocess).
                // DEVICE FINDING (v148/v149 drains, evidence 2026-06-02-cr1-chromium-
                // supervisor-deadlock): even single-process + --user-data-dir + up to a
                // 600s window, chromium --dump-dom NEVER completes — the loader goes
                // silent and the supervisor's waitpid never reaps the guest (the
                // multithread ptrace supervision DEADLOCKS on chromium's ~20-thread
                // syscall storm). So it is NOT the alarm window (PR #2 hypothesis); it is
                // the supervision wall. Because this probe HOLDS the loader's guest-launch
                // lock until the alarm (blocking every later GPU/GUI probe), it is GATED
                // behind a flag file so normal cold starts are unaffected — it runs only
                // for a dedicated investigation (adb create /data/local/tmp/.alr-cr1).
                if (java.io.File("/data/local/tmp/.alr-cr1").isFile) {
                    val crCr1 = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsManifest.name,
                        "/usr/lib/chromium/chromium-headless-shell\n--no-sandbox\n--single-process" +
                            "\n--no-zygote\n--disable-gpu\n--disable-dev-shm-usage" +
                            "\n--user-data-dir=/tmp/cr1-profile\n--no-first-run" +
                            "\n--no-default-browser-check\n--disable-crash-reporter" +
                            // v159 post-ICU wedge diagnosis: chromium verbose init log to
                            // stderr (merged into the captured guest output) — the LAST line
                            // before the stall pinpoints which init stage blocks.
                            "\n--enable-logging=stderr\n--v=1\n--dump-dom" +
                            "\ndata:text/html,<h1>ALR-CR1-OK</h1><div style=color:red>render</div>",
                    )
                    val cr1Rendered = crCr1.contains("ALR-CR1-OK")
                    android.util.Log.i(
                        "alr_loader",
                        "chromium-CR1 (single-process headless render): " +
                            "${if (cr1Rendered) "PASS rendered-DOM-has-marker" else "FAIL/incomplete"}\n$crCr1",
                    )
                }
                // CR-2 (chromium-run-plan): NETWORK — fetch a REAL https URL in-process.
                // Sockets are un-mediated (seccomp traces only path+execve), so this tests
                // DNS (resolv.conf) + TCP + TLS (CA bundle), all from chromium-net-stage.tar
                // (extracted via the toolkit loop). Gated behind /data/local/tmp/.alr-cr2.
                // --dump-dom of a real page → success = the page's real DOM (e.g. the
                // example.com <title>), proving the network stack works in-process.
                if (java.io.File("/data/local/tmp/.alr-cr2").isFile) {
                    val crCr2 = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsManifest.name,
                        "/usr/lib/chromium/chromium-headless-shell\n--no-sandbox\n--single-process" +
                            "\n--no-zygote\n--disable-gpu\n--disable-dev-shm-usage" +
                            "\n--user-data-dir=/tmp/cr2-profile\n--no-first-run" +
                            "\n--no-default-browser-check\n--disable-crash-reporter" +
                            "\n--enable-logging=stderr\n--v=1\n--dump-dom" +
                            // IP-literal (no DNS) isolates connect+TLS from the DNS layer.
                            "\nhttps://1.1.1.1/",
                    )
                    val cr2Fetched = crCr2.contains("one.one.one.one") ||
                        crCr2.contains("Cloudflare") || crCr2.contains("1.1.1.1<")
                    // Dump the FULL probe report (incl. chromium's complete stderr) to a file
                    // so the connect/TLS brk CHECK — which lands in the MIDDLE of the output
                    // that logcat truncates (head/tail only) — is recoverable via run-as.
                    try {
                        java.io.File(filesDir, "cr2-report.txt").writeText(crCr2)
                    } catch (_: Throwable) {}
                    android.util.Log.i(
                        "alr_loader",
                        "chromium-CR2 (network https fetch+render): " +
                            "${if (cr2Fetched) "PASS fetched-real-page" else "FAIL/incomplete"} (full report -> filesDir/cr2-report.txt)",
                    )
                }
                // CR-5 stepA (chromium-multiprocess-plan §8): the REAL multiprocess
                // render — drop --single-process so chromium spawns the renderer as a
                // FRESH child that re-execs "/proc/self/exe". GATE-1 (runtime_report.cpp
                // STEP-2 self-exe SUBSTITUTE) re-points that exec to the rootfs chrome
                // (host_path) and in-process re-maps it (no kernel execve; W^X-safe),
                // inheriting the seccomp+SEIZE trace. --no-zygote keeps the tree shallow
                // (zygote pre-fork is a separate exec storm); --renderer-process-limit=1
                // bounds it to one renderer. Requires ALR_REEXEC_INPROC=1 (the in-process
                // re-map gate) — set ONLY around this probe so every other path is
                // unchanged (strict no-regression). Gated behind /data/local/tmp/.alr-crmp
                // so normal cold starts never pay this (it holds the guest-launch lock).
                if (java.io.File("/data/local/tmp/.alr-crmp").isFile) {
                    android.system.Os.setenv("ALR_REEXEC_INPROC", "1", true)
                    try {
                        val crMp = nativeAlrNativeLoaderProbe(
                            packageName,
                            applicationInfo.nativeLibraryDir,
                            filesDir.absolutePath,
                            cacheDir.absolutePath,
                            rootfsManifest.name,
                            "/usr/lib/chromium/chromium-headless-shell\n--no-zygote" +
                                "\n--renderer-process-limit=1\n--no-sandbox\n--disable-gpu" +
                                "\n--disable-dev-shm-usage\n--user-data-dir=/tmp/crmp" +
                                "\n--no-first-run\n--no-default-browser-check" +
                                "\n--disable-crash-reporter\n--enable-logging=stderr\n--v=1" +
                                "\n--dump-dom" +
                                "\ndata:text/html,<h1>ALR-CRMP-OK</h1>",
                        )
                        val crMpRendered = crMp.contains("ALR-CRMP-OK")
                        try {
                            java.io.File(filesDir, "crmp-report.txt").writeText(crMp)
                        } catch (_: Throwable) {}
                        android.util.Log.i(
                            "alr_loader",
                            "chromium-CRMP (multiprocess --no-zygote renderer render): " +
                                "${if (crMpRendered) "PASS rendered-DOM-has-marker" else "FAIL/incomplete"} " +
                                "(full report -> filesDir/crmp-report.txt)\n$crMp",
                        )
                    } finally {
                        android.system.Os.unsetenv("ALR_REEXEC_INPROC")
                    }
                }
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
                for (name in listOf("sdl2", "netsurf", "qt6", "xwayland", "babl-gegl", "microbench", "interpose", "dpkg-db", "x11", "apt-config", "chromium-net", "nss", "chromium-gui", "pulse")) {
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
        // v2 apt-pipeline (research/apt-launch SSOT §5 R-V2-LAUNCH): MARKER-GATED
        // `dpkg -i hello.deb` under the fakeroot+interpose chain. Distinct from the
        // launchPackageManagerProbes apt-install probe above (that one runs the base
        // alr-smoke .deb WITHOUT fakeroot — it dies on `requires superuser`/EPERM for a
        // non-root uid). This block extracts the fakeroot + apt-dpkg stage overlays, flips
        // the ALR_FAKEROOT host-env hook the loader reads, and runs the real `hello` .deb.
        // Gated on /data/local/tmp/.alr-aptdrain so normal cold starts never run it (same
        // convention as .alr-cr1/.alr-cr2). See docs/design/v2-apt-pipeline-ssot.md §5/§7.
        launchAptDrainProbe(rootfsStatus.rootfsDir, rootfsManifest.name)
        // WS-4 §10(b) toolkit launch FUNCTIONAL probes: run a lightweight smoke of the
        // SDL2 / Qt6 / netsurf binaries (from the sdl2/qt6/netsurf overlays staged above)
        // through the ALR native loader and Log.i(tag=alr_loader, "toolkit-<name>: ...") the
        // result, so the integration device drain captures GUEST EXEC PASS + a version
        // marker — without needing a display (the GUI bodies are launched on the compositor
        // by the integration drain). Same pkgfunc pattern as launchPackageManagerProbes.
        launchToolkitProbes(rootfsStatus.rootfsDir, rootfsManifest.name)
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
        // §VK-M2: guest Vulkan enumerate/props marshalled through the ring to the REAL
        // Mali libvulkan and back (logcat tag alr_loader "vk-marshal:").
        val alrVkMarshalProbe = nativeAlrGpuVkMarshalProbe()
        val alrVkMarshalPassed =
            alrVkMarshalProbe.lineStartingWith("ALR VK ENUM MARSHAL:") ==
                "ALR VK ENUM MARSHAL: PASS"
        // §VK-M2 body: guest Vulkan device/queue/cmdbuf/clear-submit -> real Mali
        // libvulkan -> AHB clear render -> readback. logcat tag alr_loader "vk-render:".
        val alrVkRenderProbe = nativeAlrGpuVkRenderProbe()
        val alrVkRenderPassed =
            alrVkRenderProbe.lineStartingWith("ALR VK RENDER MARSHAL:") ==
                "ALR VK RENDER MARSHAL: PASS"
        // R12-G3: VK render breadth — a real graphics-pipeline vkCmdDraw (not just a
        // clear) marshalled to Mali + AHB readback. logcat tag "vk-draw:".
        val alrVkDrawProbe = nativeAlrGpuVkDrawProbe()
        val alrVkDrawPassed =
            alrVkDrawProbe.lineStartingWith("ALR VK DRAW MARSHAL:") ==
                "ALR VK DRAW MARSHAL: PASS"
        // VK-M3 (guest Vulkan ICD): host-side self-test of the ICD servicer + dual-ring
        // transport — a host producer speaks the same wire the guest libvulkan.so.1 ICD
        // emits, the servicer replays it on real Mali + writes the reply ring, asserting
        // "Mali-G615" surfaced. logcat tag "vk-icd-service:". (Guest-ICD end-to-end is
        // the alr-vk-enum device test, run via the loader with ALR_VK_ICD=1.)
        val alrVkIcdServiceProbe = nativeAlrGpuVkIcdServiceProbe()
        val alrVkIcdServicePassed =
            alrVkIcdServiceProbe.lineStartingWith("ALR VK ICD SERVICE:") ==
                "ALR VK ICD SERVICE: PASS"
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

        val executionSummary = "build: 0.4.163-sd-v163" +
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
            "\nALR VK ENUM MARSHAL (guest Vulkan enumerate/props -> ring -> real Mali libvulkan): ${if (alrVkMarshalPassed) "PASS" else "FAIL"}" +
            "\nALR VK RENDER MARSHAL (guest Vulkan device+queue+cmdbuf+clear -> real Mali -> AHB readback): ${if (alrVkRenderPassed) "PASS" else "FAIL"}" +
            "\nALR VK DRAW MARSHAL (guest Vulkan graphics-pipeline vkCmdDraw -> real Mali -> AHB readback): ${if (alrVkDrawPassed) "PASS" else "FAIL"}" +
            "\nALR VK ICD SERVICE (guest libvulkan.so.1 ICD path: servicer drains ring -> real Mali enum -> reply ring): ${if (alrVkIcdServicePassed) "PASS" else "FAIL"}" +
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
        val surfaceView = object : SurfaceView(this) {
            // Make the SurfaceView a soft-keyboard editor so an IME can deliver text
            // (commitText/setComposingText) — a bare SurfaceView returns no
            // InputConnection and silently drops all soft-IME text. onCheckIsTextEditor
            // is gated on imeWanted so the keyboard only engages when a guest text
            // field has focus (set by the compositor's onGuestImeState upcall).
            override fun onCheckIsTextEditor(): Boolean = imeWanted
            override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
                outAttrs.inputType = imeInputType
                outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
                    EditorInfo.IME_FLAG_NO_EXTRACT_UI or
                    EditorInfo.IME_ACTION_NONE
                // fullEditor=false: there is no local text buffer; AlrInputConnection
                // relays every edit straight to the guest via the native IME bridge.
                return AlrInputConnection(this, false)
            }
        }.apply {
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
            // focused Wayland client as multi-contact wl_touch (NOT a synthetic mouse).
            // Each MotionEvent forwards ALL of its contacts, then one frame closes the
            // atomic set; single-finger pointer emulation for pointer-only clients is
            // decided compositor-side. This is the "touch feels like a mouse" fix:
            //  - MOVE batches every current pointer (index 0..pointerCount-1), so a
            //    two-finger drag moves BOTH fingers (the old code only moved index 0);
            //  - DOWN/UP/POINTER_DOWN/POINTER_UP use actionIndex for BOTH the id AND the
            //    coordinate (the old code sent index-0's x/y with the wrong finger's id);
            //  - ACTION_CANCEL maps to a real wl_touch.cancel, not a lift.
            setOnTouchListener { v, ev ->
                when (ev.actionMasked) {
                    android.view.MotionEvent.ACTION_CANCEL -> {
                        nativeWaylandInjectTouchCancel()
                    }
                    android.view.MotionEvent.ACTION_MOVE -> {
                        // One MOVE callback batches movement for ALL current contacts.
                        for (i in 0 until ev.pointerCount) {
                            nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 1)
                        }
                        nativeWaylandInjectTouchFrame()
                    }
                    android.view.MotionEvent.ACTION_DOWN,
                    android.view.MotionEvent.ACTION_POINTER_DOWN -> {
                        val i = ev.actionIndex
                        nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 0)
                        nativeWaylandInjectTouchFrame()
                        if (ev.actionMasked == android.view.MotionEvent.ACTION_DOWN) {
                            // The FIRST contact (re)claims key focus so hardware keys
                            // reach the guest. Do NOT auto-raise the soft keyboard here —
                            // it would steal focus and cover the lower half of the screen
                            // on every tap (e.g. swallowing menu-item clicks). The IME is
                            // raised only on demand (long-press) via showSoftKeyboard().
                            v.requestFocus()
                            v.performClick()
                        }
                    }
                    android.view.MotionEvent.ACTION_UP,
                    android.view.MotionEvent.ACTION_POINTER_UP -> {
                        val i = ev.actionIndex
                        nativeWaylandInjectTouch(ev.getPointerId(i), ev.getX(i), ev.getY(i), 2)
                        nativeWaylandInjectTouchFrame()
                    }
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
                    // Wire the guest-IME-state upcall so a guest text field raises the
                    // Android soft keyboard. The editor SurfaceView is stashed in
                    // imeSurfaceView right after construction (below); onGuestImeState reads
                    // it on the UI thread (posted from the compositor thread).
                    nativeWaylandImeRegisterStateCallback()
                    // Audio sink (design §9B): start the in-app PulseAudio-native server on
                    // the SAME XDG_RUNTIME_DIR the compositor + guest env use
                    // (cacheDir/alr-xdg → socket at .../pulse/native). Output-only AAudio
                    // needs no runtime permission on API 26+. Started here, before the
                    // chromium-test early return below, so audio is up for chromium too.
                    try {
                        val rate = (getSystemService(AUDIO_SERVICE) as android.media.AudioManager)
                            .getProperty(android.media.AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)
                            ?.toIntOrNull() ?: 48000
                        val xdgRuntimeDir = cacheDir.absolutePath + "/alr-xdg"
                        val audioStart = nativeAudioSinkStart(xdgRuntimeDir, rate)
                        android.util.Log.i("alr_loader", "audio sink: $audioStart")
                    } catch (e: Throwable) {
                        android.util.Log.e("alr_loader",
                            "audio sink start EXC: ${android.util.Log.getStackTraceString(e)}")
                    }
                    // Clipboard bridge: start mirroring the Android primary clip into the
                    // guest once the compositor (and its data_device) is up. The native
                    // sink (installed in nativeWaylandCompositorStart) handles guest->Android.
                    if (!clipListenerRegistered) {
                        try {
                            clipboard.addPrimaryClipChangedListener(clipListener)
                            clipListenerRegistered = true
                        } catch (t: Throwable) {
                            android.util.Log.w("alr_clipboard",
                                "addPrimaryClipChangedListener failed: ${t.message}")
                        }
                    }
                    // chromium-test fast path: when a CR-test flag is set, the chromium probe
                    // thread (started in onCreate) needs the loader's guest-launch lock
                    // immediately. This GUI guest battery (wl/pixman/gtk/foot/GIMP — GIMP alone
                    // holds the lock up to its 1800s alarm) otherwise makes chromium wait ~250s,
                    // so CR-1/CR-2 drains keep missing the window. Skip it here: the compositor
                    // is already up (above) and headless chromium (--disable-gpu) needs none of
                    // these. Normal cold starts (no flag) are byte-identical.
                    if (java.io.File("/data/local/tmp/.alr-cr1").isFile ||
                        java.io.File("/data/local/tmp/.alr-cr2").isFile) {
                        android.util.Log.i("alr_loader",
                            "chromium-test mode: skipping GUI guest battery so chromium gets the loader lock")
                        return
                    }
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

                            // Goal-2 universality capstone: a real WEB BROWSER — netsurf-gtk
                            // (GTK3). drain#11 proved the ALR loader runs it to GTK init; here we
                            // launch it DISPLAY-BACKED on the compositor (same path as
                            // gtk3-widget-factory/GIMP: GDK → wl_shm → SurfaceView) so its window
                            // actually renders. about:welcome is a built-in page (no network).
                            // Guarded by binary presence (netsurf-stage auto-stage may still be
                            // extracting on its own thread).
                            if (java.io.File(rootfsStatus.rootfsDir, "usr/bin/netsurf-gtk").isFile) {
                                val framesBeforeNetsurf = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
                                val netsurfClient = nativeAlrNativeLoaderProbe(
                                    packageName,
                                    applicationInfo.nativeLibraryDir,
                                    filesDir.absolutePath,
                                    cacheDir.absolutePath,
                                    rootfsManifest.name,
                                    "/usr/bin/netsurf-gtk\nabout:welcome",
                                )
                                val netsurfStatus = nativeWaylandCompositorStatus()
                                val framesAfterNetsurf = netsurfStatus.intFieldAfter("alr wl frames=")
                                val netsurfRendered = framesAfterNetsurf > framesBeforeNetsurf
                                android.util.Log.i("alr_loader", "netsurf-result: rendered=$netsurfRendered frames=$framesBeforeNetsurf->$framesAfterNetsurf")
                                android.util.Log.i("alr_loader", "netsurf-client:\n$netsurfClient")
                                runOnUiThread {
                                    view.append(
                                        "\nALR NETSURF BROWSER (GTK3 web browser → wl_shm → SurfaceView): " +
                                            "${gate(netsurfRendered)} (frames $framesBeforeNetsurf→$framesAfterNetsurf)",
                                    )
                                    view.append("\n\n--- ALR guest netsurf-gtk about:welcome ---\n$netsurfClient")
                                }
                            } else {
                                android.util.Log.i("alr_loader", "netsurf-result: skipped (usr/bin/netsurf-gtk not staged yet)")
                            }

                            // Toolkit matrix capstone (WS-4 M2): launch a Qt6 + an SDL2 GUI
                            // demo DISPLAY-BACKED on the compositor — the SAME path as
                            // netsurf/gtk3-widget-factory/GIMP (toolkit → wl_shm → SurfaceView)
                            // so the window actually renders. We pick the FIRST candidate binary
                            // that exists (overlays agent stages a GUI demo; exact path is
                            // reconciled by the integration session) and gate on the compositor
                            // frame counter advancing (rendered = frames grew). Presence-guarded:
                            // if no GUI demo is staged we log "<name>: skipped(not staged)".
                            //
                            // (q) Qt6: a windowed widget demo. qtpaths6/qtdiag6 are CLI (no
                            // window) so we DON'T use them here — only a real GUI example.
                            // QT_QPA_PLATFORM=wayland is injected by the loader's env for the
                            // qt6 program family so the demo binds the ALR compositor.
                            val qt6GuiCandidates = listOf(
                                "usr/lib/qt6/examples/widgets/widgets/analogclock/analogclock",
                                "usr/lib/qt6/examples/widgets/widgets/wiggly/wiggly",
                                "usr/lib/aarch64-linux-gnu/qt6/examples/widgets/widgets/analogclock/analogclock",
                                "usr/lib/qt6/examples/gui/analogclock/analogclock",
                                "usr/bin/qml6",
                                "usr/lib/qt6/bin/qml",
                            )
                            val qt6GuiRel = qt6GuiCandidates.firstOrNull {
                                java.io.File(rootfsStatus.rootfsDir, it).isFile
                            }
                            if (qt6GuiRel != null) {
                                val framesBeforeQt6 = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
                                val qt6Client = nativeAlrNativeLoaderProbe(
                                    packageName,
                                    applicationInfo.nativeLibraryDir,
                                    filesDir.absolutePath,
                                    cacheDir.absolutePath,
                                    rootfsManifest.name,
                                    "/$qt6GuiRel",
                                )
                                val qt6Status = nativeWaylandCompositorStatus()
                                val framesAfterQt6 = qt6Status.intFieldAfter("alr wl frames=")
                                val qt6Rendered = framesAfterQt6 > framesBeforeQt6
                                android.util.Log.i("alr_loader", "qt6gui-result: rendered=$qt6Rendered frames=$framesBeforeQt6->$framesAfterQt6 bin=/$qt6GuiRel")
                                android.util.Log.i("alr_loader", "qt6gui-client:\n$qt6Client")
                                runOnUiThread {
                                    view.append(
                                        "\nALR QT6 GUI (Qt6 widget demo → wl_shm → SurfaceView): " +
                                            "${gate(qt6Rendered)} (frames $framesBeforeQt6→$framesAfterQt6)",
                                    )
                                    view.append("\n\n--- ALR guest qt6 GUI demo ($qt6GuiRel) ---\n$qt6Client")
                                }
                            } else {
                                android.util.Log.i("alr_loader", "qt6gui-result: skipped(not staged) (none of ${qt6GuiCandidates.joinToString(",")})")
                            }

                            // (s) SDL2: a window-opening SDL2 demo (testdraw2/testsprite2 from
                            // libsdl2-tests render a window via SDL_CreateWindow → wl_shm). The
                            // installed-tests binaries take an immediate-exit duration so the
                            // loader watchdog isn't strictly needed, but it backstops anyway.
                            val sdl2GuiCandidates = listOf(
                                "usr/libexec/installed-tests/SDL2/testdraw2",
                                "usr/libexec/installed-tests/SDL2/testsprite2",
                                "usr/libexec/installed-tests/SDL2/testgeometry",
                                "usr/libexec/installed-tests/SDL2/testwm2",
                            )
                            val sdl2GuiRel = sdl2GuiCandidates.firstOrNull {
                                java.io.File(rootfsStatus.rootfsDir, it).isFile
                            }
                            if (sdl2GuiRel != null) {
                                val framesBeforeSdl2 = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
                                val sdl2Client = nativeAlrNativeLoaderProbe(
                                    packageName,
                                    applicationInfo.nativeLibraryDir,
                                    filesDir.absolutePath,
                                    cacheDir.absolutePath,
                                    rootfsManifest.name,
                                    "/$sdl2GuiRel",
                                )
                                val sdl2Status = nativeWaylandCompositorStatus()
                                val framesAfterSdl2 = sdl2Status.intFieldAfter("alr wl frames=")
                                val sdl2Rendered = framesAfterSdl2 > framesBeforeSdl2
                                android.util.Log.i("alr_loader", "sdl2gui-result: rendered=$sdl2Rendered frames=$framesBeforeSdl2->$framesAfterSdl2 bin=/$sdl2GuiRel")
                                android.util.Log.i("alr_loader", "sdl2gui-client:\n$sdl2Client")
                                runOnUiThread {
                                    view.append(
                                        "\nALR SDL2 GUI (SDL2 window demo → wl_shm → SurfaceView): " +
                                            "${gate(sdl2Rendered)} (frames $framesBeforeSdl2→$framesAfterSdl2)",
                                    )
                                    view.append("\n\n--- ALR guest sdl2 GUI demo ($sdl2GuiRel) ---\n$sdl2Client")
                                }
                            } else {
                                android.util.Log.i("alr_loader", "sdl2gui-result: skipped(not staged) (none of ${sdl2GuiCandidates.joinToString(",")})")
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
                            // CR-4 demo (gated /data/local/tmp/.alr-crwin): launch the full GUI
                            // Chromium browser (ozone-wayland) on the compositor INSTEAD of GIMP
                            // — the SAME persistent-client path that renders GIMP's window into
                            // wl_shm → SurfaceView. Strict no-regression: a normal cold start
                            // still launches GIMP. Binary + 0-missing-.so closure ride
                            // chromium-gui-stage.tar (/usr/lib/chromium/chromium, ozone-wayland
                            // statically linked, T1). Offline demo.html (no network).
                            val crWinMarker = java.io.File("/data/local/tmp/.alr-crwin")
                            val crWin = crWinMarker.isFile
                            val gimpGuiClient = if (crWin) {
                                val rootfsDirF = java.io.File(java.io.File(filesDir, "rootfs"), rootfsManifest.name)
                                // wait (bounded) for the 337MB chromium-gui overlay to extract
                                val chromiumBin = java.io.File(rootfsDirF, "usr/lib/chromium/chromium")
                                var waited = 0
                                while (waited < 120000 && !chromiumBin.isFile) { Thread.sleep(1000); waited += 1000 }
                                try {
                                    val demoSrc = java.io.File("/data/local/tmp/alr-demo.html")
                                    val demoDst = java.io.File(rootfsDirF, "root/demo.html")
                                    demoDst.parentFile?.mkdirs()
                                    if (demoSrc.isFile) demoSrc.copyTo(demoDst, overwrite = true)
                                    else demoDst.writeText("<!doctype html><meta charset=utf-8><body style='margin:0;background:#10101e;color:#fff;font-family:sans-serif;text-align:center'><h1 style='padding-top:30vh'>ALR - Chromium on Android</h1><p>ALR-CR4-OK</p></body>")
                                } catch (e: Throwable) {
                                    android.util.Log.e("alr_loader", "crwin demo.html EXC: ${android.util.Log.getStackTraceString(e)}")
                                }
                                android.util.Log.i("alr_loader", "crwin: chromium bin=${chromiumBin.isFile} (waited ${waited}ms); launching ozone-wayland window")
                                android.system.Os.setenv("ALR_REEXEC_INPROC", "1", true)
                                nativeAlrNativeLoaderProbe(
                                    packageName,
                                    applicationInfo.nativeLibraryDir,
                                    filesDir.absolutePath,
                                    cacheDir.absolutePath,
                                    rootfsManifest.name,
                                    "/usr/lib/chromium/chromium\n--ozone-platform=wayland" +
                                        "\n--no-sandbox\n--no-zygote\n--renderer-process-limit=1\n--disable-gpu" +
                                        "\n--disable-dev-shm-usage\n--user-data-dir=/tmp/cr4-profile" +
                                        "\n--no-first-run\n--no-default-browser-check" +
                                        "\n--disable-crash-reporter\n--start-maximized" +
                                        "\n--window-size=1200,1920\n--enable-logging=stderr\n--v=1" +
                                        "\nfile:///root/demo.html",
                                )
                            } else {
                                nativeAlrNativeLoaderProbe(
                                    packageName,
                                    applicationInfo.nativeLibraryDir,
                                    filesDir.absolutePath,
                                    cacheDir.absolutePath,
                                    rootfsManifest.name,
                                    "/usr/bin/gimp-3.0",
                                )
                            }
                            val gimpStatus = nativeWaylandCompositorStatus()
                            val framesAfterGimp = gimpStatus.intFieldAfter("alr wl frames=")
                            val gimpRendered = framesAfterGimp > framesBeforeGimp
                            val crWinLabel = if (crWin) "CR-4 Chromium window (ozone-wayland)" else "GIMP 3.0 GUI"
                            runOnUiThread {
                                view.append(
                                    "\nALR $crWinLabel (→ main window → SurfaceView): " +
                                        "${gate(gimpRendered)} (frames $framesBeforeGimp→$framesAfterGimp)",
                                )
                                view.append("\n\n--- ALR guest $crWinLabel ---\n$gimpGuiClient")
                            }
                        }.start()
                    }, 14000)
                }

                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

                override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
            })
        }
        // Stash the editor SurfaceView for the guest-IME-state upcall (onGuestImeState
        // raises/hides the soft keyboard against it on the UI thread).
        imeSurfaceView = surfaceView
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

    override fun onDestroy() {
        // Clipboard bridge: stop mirroring the Android clip. The native sink + the
        // MainActivity global ref are released when the compositor is stopped
        // (nativeWaylandCompositorStop); here we just drop the listener so a
        // destroyed Activity isn't held by the system clipboard service.
        if (clipListenerRegistered) {
            try { clipboard.removePrimaryClipChangedListener(clipListener) } catch (_: Throwable) {}
            clipListenerRegistered = false
        }
        // Tear down the audio sink (joins the epoll thread, closes AAudio + the
        // socket). Best-effort: a stop on a never-started sink is a no-op.
        try {
            val audioStop = nativeAudioSinkStop()
            android.util.Log.i("alr_loader", "audio sink: $audioStop")
        } catch (e: Throwable) {
            android.util.Log.e("alr_loader",
                "audio sink stop EXC: ${android.util.Log.getStackTraceString(e)}")
        }
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus) return
        // Standalone Chromium app keeps the Android bars visible (normal-app frame);
        // every other path re-hides them for an edge-to-edge Linux GUI.
        if (componentName.className.endsWith("ChromiumStandalone") ||
            java.io.File("/data/local/tmp/.alr-cronly").isFile
        ) {
            showSystemBars()
        } else {
            applyImmersive()
        }
    }

    // Lean standalone Chromium browser window (see the onCreate gate). Stands up ONLY
    // the in-app Wayland compositor on a fullscreen SurfaceView and launches the full
    // GUI chromium browser (ozone-wayland) — no GPU probe battery, no toolkit zoo, no
    // GIMP — so the process stays small enough for chromium's browser+renderer to fit.
    // Same native path that renders GIMP's window to wl_shm -> SurfaceView; offline page.
    private fun runChromiumStandalone() {
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
        val rootfsStatus = RootfsInstaller(this).prepareBundledTinyRootfs()
        val rootfsDir = rootfsStatus.rootfsDir
        // Extract only the overlays chromium needs (its libs + the LD_PRELOAD path
        // interposer + TLS/NSS + net + C.UTF-8 locale). Gated on each tar being
        // adb-push'd to /data/local/tmp; the base rootfs already ships the GTK/X/font
        // stack. No GPU shim / toolkit / GIMP overlays here (lean).
        Thread {
            // CR-3 (chromium-gpu-child-plan): also stage gpushim — our Mali GLES/EGL
            // marshalling shim (/usr/lib/androlinux/libEGL.so.1 + libGLESv2.so.2 +
            // unversioned symlinks). chromium runs --use-gl=angle --use-angle=gles-egl,
            // and ANGLE dlopen()s these as the "system" EGL/GLES; with the GPU ring
            // attached (loader, gated on ALR_GPU_ACCEL=1) they drive the host Mali
            // executor (the same shim that scored glmark2-es2 1074 on-device).
            // vk-icd: the guest Vulkan ICD overlay (/usr/lib/androlinux/libvulkan.so.1 +
            // unversioned symlink + alr_icd.json manifest). A guest launched with
            // ALR_VK_ICD=1 binds it as libvulkan and marshals to real Mali over the VK
            // ring (alr_gpu/guest_icd/, host servicer alr_gpu_vk_host_service.hpp). Built
            // by tools/build_vk_icd_overlay.py -> /data/local/tmp/vk-icd-stage.tar.
            for (name in listOf("interpose", "nss", "chromium-net", "xkb-gegl", "gpushim", "vk-icd", "chromium-gui")) {
                try {
                    val tar = File("/data/local/tmp/$name-stage.tar")
                    val marker = File(rootfsDir, ".$name-staged-${tar.length()}")
                    if (tar.isFile && !marker.isFile) {
                        android.util.Log.i("alr_loader", "cronly $name-stage: extracting (${tar.length()} bytes)")
                        val ovr = RootfsInstaller(this).extractOverlayTar(tar, rootfsDir)
                        marker.writeText("staged\n")
                        android.util.Log.i("alr_loader", "cronly $name-stage: done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    }
                } catch (e: Throwable) {
                    android.util.Log.e("alr_loader", "cronly $name-stage EXC: ${android.util.Log.getStackTraceString(e)}")
                }
            }
        }.start()

        // Shared between surfaceCreated (compositor start + chromium launch) and
        // surfaceChanged (dynamic resize): the last content-area surface size the
        // SurfaceView reported, so the chromium launch argv (window-size) and every
        // later resize use the SAME content-area pixels (NOT the full panel), which
        // is what keeps chromium inside the system-bar insets.
        val crSurfaceW = java.util.concurrent.atomic.AtomicInteger(0)
        val crSurfaceH = java.util.concurrent.atomic.AtomicInteger(0)
        val crLaunched = java.util.concurrent.atomic.AtomicBoolean(false)
        val crRefreshMhz = java.util.concurrent.atomic.AtomicInteger(60000)

        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Thread {
                    try {
                        val dm = resources.displayMetrics
                        val disp = if (Build.VERSION.SDK_INT >= 30) display
                            else @Suppress("DEPRECATION") windowManager.defaultDisplay
                        // CONTENT-AREA size, not the full panel: the SurfaceView is laid
                        // out inside the system-bar insets (see the inset listener below),
                        // so its surfaceFrame is the area between the status bar and the
                        // nav bar. The compositor output = this size, so chromium lays its
                        // omnibox/tab strip out for the visible region and never paints
                        // under the bars. Fall back to the panel size only if the frame is
                        // not ready yet (0).
                        val frame = holder.surfaceFrame
                        val outW = if (frame.width() > 0) frame.width() else dm.widthPixels
                        val outH = if (frame.height() > 0) frame.height() else dm.heightPixels
                        val refreshMhz = Math.round((disp?.refreshRate ?: 60f) * 1000f)
                        crSurfaceW.set(outW); crSurfaceH.set(outH); crRefreshMhz.set(refreshMhz)
                        val wlStart = nativeWaylandCompositorStart(
                            cacheDir.absolutePath, holder.surface, dm.densityDpi, dm.xdpi, dm.ydpi,
                            outW, outH, refreshMhz,
                        )
                        android.util.Log.i("alr_loader", "cronly: compositor ${wlStart.lineSequence().firstOrNull()} content=${outW}x${outH}")
                        // Launch chromium exactly ONCE. If the surface is destroyed+recreated
                        // (some rotation/multi-window transitions), surfaceCreated re-fires:
                        // re-bind the compositor (above) but do NOT spawn a second chromium —
                        // the running browser just gets reconfigured via surfaceChanged.
                        if (!crLaunched.compareAndSet(false, true)) {
                            android.util.Log.i("alr_loader", "cronly: compositor re-bound (surface recreate); chromium already running")
                            return@Thread
                        }
                        // CR-4 race fix: wait for the chromium-gui overlay to FINISH
                        // extracting, not just for the chromium binary to appear. The
                        // device drain showed chromium launching at "bin=true" (the binary
                        // is early in the tar) while libopenh264.so.7 (under
                        // usr/lib/aarch64-linux-gnu, late in the tar) was still extracting —
                        // chromium's ld.so then died with "error while loading shared
                        // libraries: libopenh264.so.7: cannot open shared object file" ~50ms
                        // before that .so landed. Gate on the extraction-complete marker the
                        // staging thread writes LAST (".chromium-gui-staged-<size>") AND on a
                        // late-tar dependency file, so launch only proceeds with the overlay
                        // fully in place.
                        // CR-4 full chromium: gate on the FULL browser binary
                        // (/usr/lib/chromium/chromium, NOT chromium-shell) + the
                        // chromium-gui overlay extraction-complete marker + a late-tar
                        // dependency (libopenh264) so launch only proceeds with the whole
                        // ~337MB overlay in place (avoids the libopenh264 launch race).
                        val chromiumBin = File(rootfsDir, "usr/lib/chromium/chromium")
                        val cgTar = File("/data/local/tmp/chromium-gui-stage.tar")
                        val cgMarker = File(rootfsDir, ".chromium-gui-staged-${cgTar.length()}")
                        val openh264 = File(rootfsDir, "usr/lib/aarch64-linux-gnu/libopenh264.so.7")
                        var waited = 0
                        while (waited < 180000 &&
                            !(chromiumBin.isFile && cgMarker.isFile && openh264.isFile)
                        ) { Thread.sleep(500); waited += 500 }
                        try {
                            val demoSrc = File("/data/local/tmp/alr-demo.html")
                            val demoDst = File(rootfsDir, "root/demo.html")
                            demoDst.parentFile?.mkdirs()
                            if (demoSrc.isFile) demoSrc.copyTo(demoDst, overwrite = true)
                            else demoDst.writeText("<!doctype html><meta charset=utf-8><body style='margin:0;background:#10101e;color:#fff;font-family:sans-serif;text-align:center'><h1 style='padding-top:30vh'>ALR - Chromium on Android</h1><p>ALR-CR4-OK</p></body>")
                        } catch (_: Throwable) {}
                        android.util.Log.i("alr_loader", "cronly: chromium bin=${chromiumBin.isFile} (waited ${waited}ms); launching ozone-wayland window")
                        android.system.Os.setenv("ALR_REEXEC_INPROC", "1", true)
                        // CR-3 Mali GPU accel opt-in (chromium-gpu-child-plan §3.A-1).
                        // This is the SOLE non-glmark2 trigger the loader checks
                        // (runtime_report.cpp gpu_accel_requested): it makes the loader
                        // GPU HARDWARE accel reverted to software (see --disable-gpu below):
                        // do NOT set ALR_GPU_ACCEL, so the loader skips the Mali ring attach
                        // and this is exactly the stable software-raster path that renders.
                        // (The ANGLE-EGL-device + ring-attach scaffolding stays in the tree
                        // for the dedicated GPU effort; it is simply not triggered here.)
                        // CR-3 device diagnostics (temporary): trace the shim's EGL call
                        // sequence (eglGetDisplay/eglGetPlatformDisplay/eglInitialize/
                        // eglChooseConfig/eglCreateContext) to guest stderr → logcat
                        // (alr_cr_out), so ANGLE's exact init path + failure point are
                        // visible. Tag [alr-egl]. Safe to drop once GL_RENDERER=Mali holds.
                        android.system.Os.setenv("ALR_SHIM_DIAG", "1", true)
                        // CR-4 live diagnostics: stream chromium stderr (--v=1) + the
                        // in-process re-map trampoline diag to logcat (tags alr_cr_out /
                        // alr_cr_diag) AS THEY ARRIVE, so a wedged GUI chromium's init
                        // trace is visible without waiting for the (never-arriving) probe
                        // report. Only set on this lean cronly path.
                        android.system.Os.setenv("ALR_TEE_GUEST_STDOUT", "1", true)
                        // Standalone app: pin chromium up forever — no SIGALRM lifetime cap
                        // (was 180s) and no stall-watchdog ceiling/no-progress kill (was
                        // 200s/40s), which is what turned the window black "after a while".
                        android.system.Os.setenv("ALR_PERSIST_GUEST", "1", true)
                        val out = nativeAlrNativeLoaderProbe(
                            packageName,
                            applicationInfo.nativeLibraryDir,
                            filesDir.absolutePath,
                            cacheDir.absolutePath,
                            rootfsManifest.name,
                            // CR-4 re-map-storm fix (flag layer) — DEVICE-DIAGNOSED.
                            // Live tee (alr_cr_out) showed the runaway is NOT a self-exe
                            // GPU storm: with --no-zygote, the launch chromium repeatedly
                            // fork+execs TWO absolute-path binaries — chrome_crashpad_handler
                            // and /usr/lib/chromium/chromium (argc=10, chromium's internal
                            // child argv) — each of which the supervisor in-process re-maps
                            // (~258 MiB / chromium image). The trigger is a CRASH-RESTART
                            // loop: chromium logs "No usable sandbox!" then IMMEDIATE_CRASHes
                            // (SIGTRAP brk), spawning chrome_crashpad_handler, and retries —
                            // 4× crashpad + 8× chromium = the 542->1052 MiB climb, twice,
                            // until the watchdog SIGKILLs. --no-sandbox is on the LAUNCH argv
                            // but the child bootstrap still trips the sandbox path.
                            // FIX: --single-process collapses renderer + GPU + utility into
                            // the ONE browser process — no fork+exec of chromium children at
                            // all, so the in-process re-map count for chromium drops to ZERO
                            // (only the launch image, mapped once by the normal loader path,
                            // not the trampoline). That removes the storm at its source
                            // instead of throttling it. --in-process-gpu/--disable-gpu-
                            // compositing keep GPU work CPU-side (we are --disable-gpu/wl_shm).
                            // Crashpad is suppressed via --disable-crash-reporter/breakpad.
                            // SANDBOX (device-diagnosed crash cause): the live drain showed
                            // chromium logs "No usable sandbox!" then IMMEDIATE_CRASHes right
                            // after GetCollectStatsConsent, and the only -ENOSYS'd syscall is
                            // nr=99 set_robust_list. chromium installs its OWN seccomp-bpf
                            // filter and probes the namespace/setuid sandbox — both fight our
                            // ptrace+seccomp supervision. --no-sandbox alone does NOT stop the
                            // seccomp-bpf filter install. So disable EVERY sandbox layer:
                            //   --disable-seccomp-filter-sandbox : chromium must NOT install a
                            //       nested seccomp-bpf filter (it collides with our SEIZE+
                            //       seccomp trace and is the prime crash suspect).
                            //   --disable-setuid-sandbox / --disable-namespace-sandbox /
                            //   --disable-gpu-sandbox : no SUID helper, no userns clone, no GPU
                            //       sandbox — none can work inside an untrusted_app domain.
                            // FULL chromium browser (/usr/lib/chromium/chromium — the real
                            // tabbed UI with omnibox, NOT content_shell). Multiprocess by
                            // nature, but --single-process collapses renderer+GPU+utility into
                            // the ONE browser process so the loader maps ONE address space (no
                            // per-child ~258MiB re-map storm). The two child forks full chromium
                            // still does even single-process — chrome_crashpad_handler and any
                            // residual --type= child — are handled at the LOADER (this session):
                            //   * crashpad: runtime_report.cpp neuters its exec to exit0
                            //     (--disable-crash-reporter/breakpad already set), breaking the
                            //     device-observed crashpad↔chromium refork RSS storm.
                            //   * sandbox: a chromium child's argv DROPS these --*-sandbox flags
                            //     (chromium rebuilds child argv from its own cmdline), so the
                            //     child logged "No usable sandbox!" and brk()-crashed. The loader
                            //     now re-injects the missing sandbox-disable flags into the
                            //     re-mapped child's argv (decide_chromium_child_argv). Keep them
                            //     on the LAUNCH argv too so the browser process itself is covered.
                            "/usr/lib/chromium/chromium\n--ozone-platform=wayland" +
                                "\n--no-sandbox\n--disable-seccomp-filter-sandbox" +
                                "\n--disable-setuid-sandbox\n--disable-namespace-sandbox" +
                                "\n--disable-gpu-sandbox" +
                                // GPU HARDWARE accel (Mali via the alr_gpu shim: --use-gl=angle
                                // → libEGL/libGLESv2 shim → ring → Mali executor) was wired +
                                // attempted, but chromium's GPU process FAILS to create its
                                // shared/virtualized GL context ("Failed to create shared
                                // context for virtualization" / "SharedImageStub: unable to
                                // create context") — the glmark2-era shim does not yet implement
                                // chromium's EGL context-sharing model, and the failure BLACKS
                                // the page. Reverted to stable software raster so the standalone
                                // browser keeps rendering; Mali accel is a dedicated shim effort
                                // (the ANGLE EGL-device exts + ring-attach scaffolding stay in).
                                "\n--single-process\n--no-zygote\n--disable-gpu" +
                                "\n--in-process-gpu\n--disable-gpu-compositing" +
                                "\n--disable-dev-shm-usage\n--user-data-dir=/tmp/cr4-profile" +
                                "\n--no-first-run\n--no-default-browser-check" +
                                "\n--disable-crash-reporter\n--disable-breakpad" +
                                // demo.html is file:// (offline). full chromium still probes
                                // DoH (https://dns.google) in the background → NSS init →
                                // libsqlite3.so.0 (flat-path gap) → FATAL nss_error=-5925,
                                // AFTER the window already painted. Kill ALL background net so
                                // the offline window persists (no DNS/DoH/component/pings).
                                "\n--disable-background-networking" +
                                "\n--disable-features=DnsOverHttps,AsyncDns" +
                                "\n--no-pings\n--disable-component-update" +
                                // Match the ACTUAL compositor surface (device commits a
                                // 1920x1200 landscape toplevel). The earlier 1200x1920
                                // (portrait) mismatch made chromium lay its tab strip +
                                // omnibox out for a 1200-wide window while the surface was
                                // 1920 wide, pushing the top chrome partly off the visible
                                // area. Sizing to 1920x1200 lets the full browser UI fit.
                                // Size chromium to the CONTENT-AREA surface (between the
                                // status bar + nav bar), not the full panel — so the omnibox/
                                // tab strip + page fit the visible region and nothing paints
                                // under the Android bars. surfaceChanged re-sizes on rotation.
                                "\n--start-maximized\n--window-size=${crSurfaceW.get()},${crSurfaceH.get()}" +
                                "\n--ozone-override-screen-size=${crSurfaceW.get()},${crSurfaceH.get()}" +
                                "\n--enable-logging=stderr\n--v=1" +
                                // RENDER FIX (device-diagnosed): a file:///root/demo.html
                                // load painted, but full chromium's FileURLLoader served it
                                // as text/plain (the rootfs lacks /etc/mime.types and the
                                // platform xdgmime query under ALR path mediation does not
                                // resolve .html→text/html), so the page showed as SOURCE.
                                // A data:text/html URL carries the MIME inline — no file
                                // MIME lookup, no path mediation — so chromium renders the
                                // actual page. The HTML is the same offline branded demo,
                                // URL-encoded (spaces→%20, #→%23) into the single argv token.
                                "\n" + chromiumDataUrl(),
                        )
                        android.util.Log.i("alr_loader", "cronly chromium exited:\n$out")
                    } catch (e: Throwable) {
                        android.util.Log.e("alr_loader", "cronly EXC: ${android.util.Log.getStackTraceString(e)}")
                    }
                }.start()
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                // Rotation / multi-window / split-screen / any resolution change resizes
                // the SurfaceView surface. Reconfigure the live compositor output (wl_output
                // + re-send xdg_toplevel.configure to chromium) to the NEW content-area size
                // so chromium re-lays-out for the new geometry instead of going black. The
                // native side coalesces a rotation-animation burst and rebinds the
                // ANativeWindow. Skip no-op repeats (first surfaceChanged often equals the
                // surfaceCreated size).
                if (width <= 0 || height <= 0) return
                if (width == crSurfaceW.get() && height == crSurfaceH.get()) return
                crSurfaceW.set(width); crSurfaceH.set(height)
                val dm = resources.displayMetrics
                val disp = if (Build.VERSION.SDK_INT >= 30) display
                    else @Suppress("DEPRECATION") windowManager.defaultDisplay
                val refreshMhz = Math.round((disp?.refreshRate ?: 60f) * 1000f)
                crRefreshMhz.set(refreshMhz)
                val r = nativeWaylandCompositorResize(
                    holder.surface, width, height, dm.densityDpi, dm.xdpi, dm.ydpi, refreshMhz,
                )
                android.util.Log.i("alr_loader", "cronly: surfaceChanged ${width}x$height -> $r")
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
        })
        // Fullscreen surface; route touch into the focused Wayland client for scroll/click.
        surfaceView.setOnTouchListener { _, ev ->
            val phase = when (ev.actionMasked) {
                android.view.MotionEvent.ACTION_DOWN, android.view.MotionEvent.ACTION_POINTER_DOWN -> 0
                android.view.MotionEvent.ACTION_MOVE -> 1
                else -> 2
            }
            nativeWaylandInjectTouch(ev.getPointerId(ev.actionIndex), ev.x, ev.y, phase)
            true
        }
        // Strictly inset the SurfaceView INSIDE the system bars: fitsSystemWindows on the
        // container consumes the status-bar + nav-bar insets as padding, so the SurfaceView
        // (hence its surface = the compositor output) is exactly the content area between
        // the bars. chromium then NEVER paints under the Android bars (normal-app frame);
        // its surfaceFrame/surfaceChanged report the inset size, which drives the compositor
        // output + chromium --window-size. (Bar height changes on rotation re-fire
        // surfaceChanged → nativeWaylandCompositorResize.)
        setContentView(
            android.widget.FrameLayout(this).apply {
                fitsSystemWindows = true
                addView(
                    surfaceView,
                    android.widget.FrameLayout.LayoutParams(
                        android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                        android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                    ),
                )
            },
        )
        // Show the Android status bar + nav bar (soft keys) — normal-app frame, NOT immersive.
        showSystemBars()
        surfaceView.post { surfaceView.requestFocus() }
    }

    // The offline CR-4 demo page as a data:text/html URL. Returned to the cronly launch
    // argv as the single navigation token. We embed the MIME inline (data:text/html) so
    // full chromium renders the page instead of MIME-sniffing a file:// load to
    // text/plain (the rootfs has no /etc/mime.types and the platform xdgmime query does
    // not resolve under ALR path mediation, device-confirmed showing the HTML as source).
    // ZERO network: every byte is inline (no <link>/<script src>/<img>/web-font). A tiny
    // inline-script clock makes the window visibly LIVE (proves a real renderer, not a
    // static blit). Encoded minimally — only the bytes that break a URL token (space,
    // '#', '%') — which keeps the data URL valid as one whitespace-free argv element.
    private fun chromiumDataUrl(): String {
        val html = buildString {
            append("<!DOCTYPE html><html lang=en><head><meta charset=utf-8>")
            append("<meta name=viewport content='width=device-width,initial-scale=1'>")
            append("<title>ALR Chromium on Android</title>")
            append("<style>")
            append("html,body{margin:0;height:100%;font-family:system-ui,sans-serif}")
            append("body{background:radial-gradient(1200px 800px at 25% -10%,#1c2a63,#0b1021 60%);")
            append("color:#e9edff;display:flex;flex-direction:column;justify-content:center;padding:8vw}")
            append("h1{font-size:7vw;margin:0 0 2vh;font-weight:800;letter-spacing:-.5px}")
            append("p{font-size:3.2vw;margin:.4vh 0;color:#9fb0e0}")
            append(".badge{display:inline-block;margin-top:3vh;padding:1.4vh 3vw;border-radius:999px;")
            append("background:#8ad6ff;color:#06122c;font-weight:700;font-size:3vw}")
            append(".dot{display:inline-block;width:2.4vw;height:2.4vw;border-radius:50%;background:#7fe07f;")
            append("margin-right:1.4vw;vertical-align:middle;animation:pulse 1.2s infinite}")
            append("@keyframes pulse{0%,100%{opacity:1}50%{opacity:.25}}")
            append("</style></head><body>")
            append("<h1>ALR &mdash; Chromium on Android</h1>")
            append("<p>Full chromium browser, rendered through the ALR Wayland compositor.</p>")
            append("<p><span class=dot></span>Live render &bull; <span id=clk>--:--:--</span></p>")
            append("<div class=badge>ALR-CR4-OK</div>")
            append("<script>function t(){var d=new Date();")
            append("document.getElementById('clk').textContent=d.toLocaleTimeString();}")
            append("t();setInterval(t,1000);</script>")
            append("</body></html>")
        }
        // Minimal URL-encoding: only characters that would terminate/confuse the single
        // argv token or the data-URL grammar. The newline-delimited argv splits on '\n'
        // (none here); spaces would split shell-style consumers, '#' starts a URL fragment,
        // '%' is the escape introducer. Everything else (incl. < > " ' ; ,) is data-URL safe.
        val enc = html
            .replace("%", "%25")
            .replace(" ", "%20")
            .replace("#", "%23")
        return "data:text/html,$enc"
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
                // (b4) apt/dpkg INSTALL: actually run `dpkg -i <local .deb>` through the ALR
                // loader so dpkg UNPACKS a package into the admin DB (the overlays agent ships
                // a small alr-smoke .deb under var/cache/apt/archives). HONEST: the maintainer
                // scripts (preinst/postinst) are fork+exec'd by dpkg — that's an exec re-entry
                // the loader doesn't yet support, so `Setting up` (configure) may fail while the
                // UNPACK ("Unpacking alr-smoke") succeeds. We log whatever happens; we do NOT
                // try to make the maintainer-script exec work (separate, larger feature). The
                // `Unpacking`/`Selecting` marker = unpack-stage success, regardless of configure.
                val localDeb = File(rootfsDir, "var/cache/apt/archives/alr-smoke_1.0_arm64.deb")
                if (localDeb.isFile) {
                    val out = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        "/usr/bin/dpkg\n-i\n/var/cache/apt/archives/alr-smoke_1.0_arm64.deb",
                    )
                    val exec = out.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:")
                    val unpacked = out.contains("Unpacking alr-smoke") ||
                        out.contains("Selecting previously unselected package alr-smoke") ||
                        out.contains("Preparing to unpack")
                    val configured = out.contains("Setting up alr-smoke")
                    android.util.Log.i(
                        "alr_loader",
                        "apt-install: unpacked=$unpacked configured=$configured exec=[$exec] " +
                            "(maintainer-script exec-re-entry may fail — logged honestly)",
                    )
                    android.util.Log.i("alr_loader", "apt-install-out:\n$out")
                } else {
                    android.util.Log.i("alr_loader", "apt-install: skipped(not staged) (var/cache/apt/archives/alr-smoke_1.0_arm64.deb absent)")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "pkgfunc EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
    }

    // v2 apt-pipeline drain (research/apt-launch SSOT §5 R-V2-LAUNCH / §7 DEVICE-REQ
    // ALR-V2-apt-unpack). MARKER-GATED `dpkg -i hello.deb` under the fakeroot+interpose
    // chain so a NON-ROOT (uid 10xxx) guest can clear dpkg's `requires superuser` gate +
    // chown/chmod EPERM and actually UNPACK a real Debian/Ubuntu .deb in-process.
    //
    // Mirrors the chromium-stage probe (onCreate L59+) pattern exactly: size-keyed extract
    // markers (.{name}-staged-<size>) via the WS-4 M1 guarded extractOverlayTar (can never
    // downgrade a base lib), a bounded wait for the staged binaries, and a flag-file gate so
    // normal cold starts are a strict no-op (only an explicit integration/device drain that
    // `adb shell touch /data/local/tmp/.alr-aptdrain` arms it — same convention as .alr-cr1).
    //
    // T1/T2 SPLIT (IMPORTANT — see SSOT §5 + runtime_report.cpp L1617-1636): the native JNI
    // entry `nativeAlrNativeLoaderProbe(package,libDir,filesDir,cacheDir,rootfsName,program)`
    // is a FIXED 6-arg signature — it takes NO extra-env parameter. The loader decides the
    // guest LD_PRELOAD itself and currently HARDCODES the single
    // `<rootfs>/usr/lib/androlinux/libalr_interpose.so` (runtime_report.cpp L1633). It reads
    // its A/B gates only from the HOST (app) process env via ::getenv (ALR_DISABLE_INTERPOSE,
    // ALR_PCGATE). So MainActivity's ONLY clean, signature-stable hook is to set an
    // `ALR_FAKEROOT=1` host-env var before the probe; the loader (T2, runtime_report.cpp —
    // WS-1 territory) must then chain fakeroot FIRST:
    //   LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_fakeroot.so
    //             :<rootfs>/usr/lib/androlinux/libalr_interpose.so
    //   + FAKEROOTUID=0 FAKEROOTGID=0  (ALR_ROOTFS is already injected by the loader).
    // Until that loader diff lands (SSOT §5 proposed diff A), the chain is NOT yet wired:
    // this block extracts the overlays + arms the marker + sets ALR_FAKEROOT + runs the dpkg
    // argv, and the unpack will only flip to true once T2 honors ALR_FAKEROOT. The dpkg argv
    // here is verbatim `tools/build_fakeroot_overlay.py --device-cmd`
    // (`dpkg --force-not-root --force-bad-path -i <deb>`).
    // One apt-install drain target: the dpkg `-i` package, its .deb (in the apt
    // cache), the extra stage tar that ships it, and the per-package unpack/configure
    // markers dpkg prints (these embed the package NAME, so they must be pkg-driven —
    // a hardcoded "hello" would never match galculator's output). SSOT for the drain's
    // package so generalizing from hello→galculator is one descriptor, not scattered edits.
    private data class AptDrainTarget(
        val pkg: String,
        val debCachePath: String,   // rootfs-rel, under var/cache/apt/archives/
        val stageTar: String?,      // extra /data/local/tmp/<name>-stage.tar to extract (null = none)
        val unpackedMarkers: List<String>,
        val configuredMarker: String,
    )

    private fun aptDrainTargetFor(pkg: String): AptDrainTarget = when (pkg) {
        // v2 breadth: a real GTK3 GUI app installed via dpkg, then launched on the
        // compositor. Its deb + unpacked closure ride galculator-stage.tar.
        "galculator" -> AptDrainTarget(
            pkg = "galculator",
            debCachePath = "var/cache/apt/archives/galculator_2.1.4-1.2build2_arm64.deb",
            stageTar = "galculator",
            unpackedMarkers = listOf(
                "Unpacking galculator",
                "Preparing to unpack",
                "Selecting previously unselected package galculator",
            ),
            configuredMarker = "Setting up galculator",
        )
        // v2 breadth (generalization): arbitrary catalog CLI/X11 apps installed via dpkg.
        // Each rides its own <pkg>-stage.tar (built by tools/build_install_stage.py) which
        // carries BOTH the leaf .deb at var/cache/apt/archives/<pkg>_<ver>_<arch>.deb (the
        // dpkg -i TARGET) AND the base-subtracted unpacked closure (so the binary + its
        // non-base libs are present for a launch attempt). Same descriptor shape as
        // galculator — only the package name / deb filename / launch class differ.
        "htop" -> AptDrainTarget(
            pkg = "htop",
            debCachePath = "var/cache/apt/archives/htop_3.3.0-4build1_arm64.deb",
            stageTar = "htop",
            unpackedMarkers = listOf(
                "Unpacking htop",
                "Preparing to unpack",
                "Selecting previously unselected package htop",
            ),
            configuredMarker = "Setting up htop",
        )
        "nano" -> AptDrainTarget(
            pkg = "nano",
            debCachePath = "var/cache/apt/archives/nano_7.2-2build1_arm64.deb",
            stageTar = "nano",
            unpackedMarkers = listOf(
                "Unpacking nano",
                "Preparing to unpack",
                "Selecting previously unselected package nano",
            ),
            configuredMarker = "Setting up nano",
        )
        "xterm" -> AptDrainTarget(
            pkg = "xterm",
            debCachePath = "var/cache/apt/archives/xterm_390-1ubuntu3_arm64.deb",
            stageTar = "xterm",
            unpackedMarkers = listOf(
                "Unpacking xterm",
                "Preparing to unpack",
                "Selecting previously unselected package xterm",
            ),
            configuredMarker = "Setting up xterm",
        )
        // default: the original v2 hello.deb proof (its deb rides apt-dpkg-stage.tar,
        // so no extra stage tar is needed).
        else -> AptDrainTarget(
            pkg = "hello",
            debCachePath = "var/cache/apt/archives/hello_2.10-3build1_arm64.deb",
            stageTar = null,
            unpackedMarkers = listOf(
                "Unpacking hello",
                "Preparing to unpack",
                "Selecting previously unselected package hello",
            ),
            configuredMarker = "Setting up hello",
        )
    }

    // CR-2 online apt: run `apt-get update` in-guest against the mirror pinned in /etc/hosts
    // (apt-mirror overlay) on a DNS-blocked device. Reuses the fakeroot + interpose chain
    // (apt writes /var/lib/apt/lists as uid=0; forks /usr/lib/apt/methods/http for the fetch).
    // HTTP mirror (apt integrity = GPG, no TLS) sidesteps the TLS/NSS connect-brk; the
    // /etc/hosts pin sidesteps the blocked DNS; host-curl already proved the pinned IP serves
    // the apt index. Armed by `echo update > /data/local/tmp/.alr-aptdrain`.
    private fun runAptUpdateProbe(rootfsDir: File, rootfsName: String) {
        Thread {
            try {
                android.util.Log.i("alr_loader", "aptupdate: armed — staging fakeroot/apt-dpkg/dpkg-db/apt-mirror")
                for (name in listOf("fakeroot", "apt-dpkg", "dpkg-db", "apt-mirror")) {
                    val tar = java.io.File("/data/local/tmp/$name-stage.tar")
                    val m = java.io.File(rootfsDir, ".aptdrain-$name-staged-${tar.length()}")
                    if (tar.isFile && !m.isFile) {
                        val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(tar, rootfsDir)
                        m.writeText("staged\n")
                        android.util.Log.i("alr_loader", "aptupdate: $name-stage done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                    } else if (!tar.isFile) {
                        android.util.Log.i("alr_loader", "aptupdate: $name-stage.tar absent (push it to /data/local/tmp)")
                    }
                }
                val fakerootSo = java.io.File(rootfsDir, "usr/lib/androlinux/libalr_fakeroot.so")
                val aptGetBin = java.io.File(rootfsDir, "usr/bin/apt-get")
                val hostsFile = java.io.File(rootfsDir, "etc/hosts")
                val srcFile = java.io.File(rootfsDir, "etc/apt/sources.list.d/alr-ports.sources")
                // v2 interpose-stage race (device-root-caused, same as launchAptDrainProbe):
                // the concurrent onCreate overlay thread (re)writes libalr_interpose.so IN
                // PLACE; if apt-get's ld.so mmaps it mid-rewrite, the LD_PRELOAD ctor never
                // runs → ZERO path mediation (traps=0) → apt reads the literal Android paths
                // and dies "E: Error reading the CPU table" (exit 100). WAIT for the interpose
                // .so to settle (the .interpose-staged-<len> marker is written only AFTER the
                // extract fully completes) before launching apt-get.
                val interposeSo = java.io.File(rootfsDir, "usr/lib/androlinux/libalr_interpose.so")
                val interposeStageTar = java.io.File("/data/local/tmp/interpose-stage.tar")
                val interposeStaging = {
                    interposeStageTar.isFile &&
                        (rootfsDir.listFiles { f -> f.name.startsWith(".interpose-staged-") }?.isEmpty() ?: true)
                }
                var w = 0
                while (w < 40000 &&
                    !(fakerootSo.isFile && aptGetBin.isFile && srcFile.isFile &&
                        interposeSo.isFile && !interposeStaging())
                ) { Thread.sleep(500); w += 500 }
                android.util.Log.i(
                    "alr_loader",
                    "aptupdate: fakeroot.so=${fakerootSo.isFile} apt-get=${aptGetBin.isFile} hosts=${hostsFile.isFile} sources=${srcFile.isFile} (waited ${w}ms)",
                )
                android.system.Os.setenv("ALR_FAKEROOT", "1", true)
                android.system.Os.setenv("ALR_REEXEC_INPROC", "1", true)
                android.system.Os.setenv("ALR_INTERPOSE_DIAG", "1", true)
                android.system.Os.setenv("ALR_TEE_GUEST_STDOUT", "1", true)
                try {
                    val out = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        "/usr/bin/apt-get\n-o\nAcquire::ForceIPv4=true\nupdate",
                    )
                    val online = out.contains("InRelease") || out.contains("Get:") ||
                        out.contains("Packages") || out.contains("Reading package lists")
                    android.util.Log.i("alr_loader", "aptupdate: apt-get update online=$online")
                    android.util.Log.i("alr_loader", "aptupdate-out:\n$out")
                } finally {
                    android.system.Os.unsetenv("ALR_FAKEROOT")
                    android.system.Os.unsetenv("ALR_REEXEC_INPROC")
                    android.system.Os.unsetenv("ALR_INTERPOSE_DIAG")
                    android.system.Os.unsetenv("ALR_TEE_GUEST_STDOUT")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "aptupdate EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
    }

    private fun launchAptDrainProbe(rootfsDir: File, rootfsName: String) {
        val marker = java.io.File("/data/local/tmp/.alr-aptdrain")
        if (!marker.isFile) return
        // The marker CONTENT names the package to install (default: hello). e.g.
        //   adb shell 'echo galculator > /data/local/tmp/.alr-aptdrain'
        // arms the galculator drain; an empty marker keeps the original hello proof.
        // CR-2: marker content "update" instead runs `apt-get update` against the mirror
        // pinned in /etc/hosts (apt-mirror overlay) — proves ONLINE apt on a DNS-blocked device.
        val markerContent = runCatching { marker.readText().trim() }.getOrDefault("")
        if (markerContent == "update") {
            runAptUpdateProbe(rootfsDir, rootfsName)
            return
        }
        val target = aptDrainTargetFor(markerContent.ifEmpty { "hello" })
        Thread {
            try {
                android.util.Log.i("alr_loader", "aptdrain: marker present — v2 apt-pipeline drain armed (pkg=${target.pkg})")
                // (L1 — SSOT §5) Extract the fakeroot + apt-dpkg stage overlays through the
                // SAME guarded extractOverlayTar the chromium/foot/toolkit blocks use, keyed on
                // tar size so re-pushing a rebuilt stage auto-re-extracts. fakeroot-stage.tar
                // ships usr/lib/androlinux/libalr_fakeroot.so (0755); apt-dpkg-stage.tar ships
                // usr/bin/{dpkg,apt,tar,…} + var/lib/dpkg admindir + var/cache/apt/archives/hello_*.deb.
                // ORDER MATTERS (v2 admindir fix): apt-dpkg-stage ships the admindir SCAFFOLD
                // (the empty var/lib/dpkg/{updates,triggers,alternatives}/ DIRECTORY entries +
                // zero-byte status), while dpkg-db-stage ships the POPULATED status (72KB) +
                // info/*.list but carries NO directory entries. v163 died at
                // `var/lib/dpkg/updates/tmp.i: No such file or directory` because the scaffold's
                // updates/ dir was missing (dpkg-db alone never creates it). So extract apt-dpkg
                // FIRST (creates updates/ etc.), then dpkg-db LAST so its real status overwrites
                // the scaffold's empty one while the scaffold's updates/ dir survives. All three
                // (fakeroot, apt-dpkg, dpkg-db) MUST land before the dpkg run below.
                // Use aptdrain-scoped markers (NOT the shared .{name}-staged-<size> the onCreate
                // toolkit loop uses for dpkg-db) so this sequential, ordered extract is never
                // pre-empted/skipped by that concurrent thread — guaranteeing apt-dpkg's scaffold
                // lands before dpkg-db's populated status every armed cold start.
                // The base three (fakeroot/apt-dpkg/dpkg-db) PLUS the target's own stage tar
                // (galculator-stage.tar ships its .deb at var/cache/apt/archives/ + the unpacked
                // closure so the compositor can launch /usr/bin/galculator). hello needs no extra
                // tar — its .deb already rides apt-dpkg-stage.tar — so target.stageTar is null there.
                val stageNames = listOf("fakeroot", "apt-dpkg", "dpkg-db") + listOfNotNull(target.stageTar)
                for (name in stageNames) {
                    val tar = java.io.File("/data/local/tmp/$name-stage.tar")
                    val marker = java.io.File(rootfsDir, ".aptdrain-$name-staged-${tar.length()}")
                    if (tar.isFile && !marker.isFile) {
                        android.util.Log.i("alr_loader", "aptdrain: $name-stage extracting overlay (${tar.length()} bytes)")
                        val ovr = RootfsInstaller(this@MainActivity).extractOverlayTar(tar, rootfsDir)
                        marker.writeText("staged\n")
                        android.util.Log.i("alr_loader", "aptdrain: $name-stage overlay done (extracted=${ovr.extracted} skipped=${ovr.skipped.size})")
                        if (ovr.skipped.isNotEmpty()) android.util.Log.w("alr_loader", "aptdrain: $name-stage guard skipped downgrades:\n${ovr.skipped.joinToString("\n")}")
                    } else if (!tar.isFile) {
                        android.util.Log.i("alr_loader", "aptdrain: $name-stage.tar absent (push it to /data/local/tmp to arm the drain)")
                    }
                }
                // The fakeroot .so + dpkg + the hello .deb must all be present before we probe;
                // the overlays land via the loop above (or a prior cold start's markers).
                val fakerootSo = java.io.File(rootfsDir, "usr/lib/androlinux/libalr_fakeroot.so")
                val dpkgBin = java.io.File(rootfsDir, "usr/bin/dpkg")
                // The target .deb in the apt cache: hello rides apt-dpkg-stage's --fetch-test-deb
                // asset; galculator rides galculator-stage.tar (var/cache/apt/archives/galculator_*.deb).
                val targetDeb = java.io.File(rootfsDir, target.debCachePath)
                // v2 ROOT CAUSE FIX (device-confirmed, /tmp/aptdrain9.log): the
                // libalr_interpose.so under usr/lib/androlinux/ is (re)written IN PLACE by
                // the SEPARATE onCreate overlay-staging Thread (the "interpose-stage"
                // extractOverlayTar). That thread ran CONCURRENTLY with this dpkg probe —
                // its extract window (interpose-stage: extracting → overlay done) bracketed
                // the dpkg launch. When the guest ld.so mmap'd libalr_interpose.so mid-
                // rewrite (truncate+write), the LD_PRELOAD of the interposer FAILED to load
                // (non-fatally: glibc skips a broken preload and proceeds), so the
                // interposer's init_array/ctor never ran → NO path mediation → dpkg's
                // relative opens hit literal Android paths → var/lib/dpkg/updates/tmp.i
                // ENOENT → unpacked=false. fakeroot.so was staged earlier and settled, so
                // ITS ctor ran (ALR-FRDIAG), which is why the two preloads diverged.
                // FIX: also WAIT for the interpose-stage marker (written only AFTER the
                // extract fully completes) so the .so is stable before any guest LD_PRELOADs
                // it. The marker is name-versioned by tar size (.interpose-staged-<len>); we
                // accept any .interpose-staged-* so a re-pushed tar of a different size still
                // gates correctly. Absent tar (interpose shipped only in the base rootfs) →
                // no marker is ever written, so don't block on it in that case.
                val interposeStageTar = java.io.File("/data/local/tmp/interpose-stage.tar")
                val interposeStaging = {
                    interposeStageTar.isFile &&
                        (rootfsDir.listFiles { f ->
                            f.name.startsWith(".interpose-staged-")
                        }?.isEmpty() ?: true)
                }
                var waited = 0
                while (waited < 20000 &&
                    !(fakerootSo.isFile && dpkgBin.isFile && targetDeb.isFile && !interposeStaging())) {
                    Thread.sleep(500)
                    waited += 500
                }
                val interposeStaged = !interposeStaging()
                android.util.Log.i(
                    "alr_loader",
                    "aptdrain: fakeroot.so=${fakerootSo.isFile} dpkg=${dpkgBin.isFile} " +
                        "${target.pkg}.deb=${targetDeb.isFile} interpose-staged=$interposeStaged (waited ${waited}ms)",
                )
                if (!(fakerootSo.isFile && dpkgBin.isFile && targetDeb.isFile)) {
                    android.util.Log.w("alr_loader", "aptdrain: prerequisites missing — skipping dpkg -i (push fakeroot-stage.tar + apt-dpkg-stage.tar" + (target.stageTar?.let { " + $it-stage.tar" } ?: "") + ")")
                    return@Thread
                }
                // (L2 — SSOT §5) Flip the host-env hook the loader reads. Setting ALR_FAKEROOT=1
                // on THIS app process env is the signature-stable trigger for the loader (T2,
                // runtime_report.cpp) to chain libalr_fakeroot.so FIRST ahead of the interpose
                // .so and push FAKEROOTUID/GID=0. We restore it immediately after the probe so
                // every OTHER probe path stays on the plain interpose-only chain (no regression).
                android.system.Os.setenv("ALR_FAKEROOT", "1", true)
                // (v2) Enable static re-map for dpkg's fork+exec children (zstd/sh/tar). dpkg
                // shells out to the extract helper + maintainer scripts; without INPROC those
                // statically-linked children don't get the in-process re-map and the unpack
                // stage stalls. Scoped to the drain (unset in finally) so no other path changes.
                android.system.Os.setenv("ALR_REEXEC_INPROC", "1", true)
                // (v2 diag) Arm the interposer's chdir/relative-create trace so the
                // device drain reveals exactly which syscall dpkg uses to create
                // var/lib/dpkg/updates/tmp.i and whether cwd lands inside the rootfs.
                // Scoped to the drain (unset in finally); pure diagnostic, no behavior change.
                android.system.Os.setenv("ALR_INTERPOSE_DIAG", "1", true)
                try {
                    // (L3 effected) dpkg -i <target>.deb. argv is verbatim the builder's
                    // --device-cmd: --force-not-root (bypass the superuser gate; fakeroot makes
                    // getuid()==0 but this is belt-and-suspenders) + --force-bad-path. The deb is
                    // referenced by its rootfs-relative path (the loader/interpose map it under
                    // the rootfs). The package is the marker-driven target (hello | galculator | …).
                    val out = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        "/usr/bin/dpkg\n--force-not-root\n--force-bad-path\n-i\n" +
                            "/" + target.debCachePath,
                    )
                    val exec = out.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:")
                    // §7 DEVICE-REQ markers: unpack-stage success (the extract child re-map must
                    // pass for the data.tar to actually unpack — G1 gated per SSOT §6). The markers
                    // embed the package name, so they come from the target descriptor.
                    val unpacked = target.unpackedMarkers.any { out.contains(it) }
                    // Configure stage = maintainer-script /bin/sh fork+exec (G1 too).
                    val configured = out.contains(target.configuredMarker)
                    android.util.Log.i(
                        "alr_loader",
                        "aptdrain: pkg=${target.pkg} unpacked=$unpacked configured=$configured exec=[$exec] " +
                            "ALR_FAKEROOT=1 (fakeroot-chain wiring = T2/runtime_report.cpp; " +
                            "extract+maintainer-script need G1 exec-re-entry per SSOT §6)",
                    )
                    android.util.Log.i("alr_loader", "aptdrain-out:\n$out")
                    // §7 stretch: post-install status query (proves the admin DB recorded the pkg).
                    val statusOut = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        "/usr/bin/dpkg\n--status\n" + target.pkg,
                    )
                    val installed = statusOut.contains("Status: install ok installed")
                    android.util.Log.i("alr_loader", "aptdrain: installed=$installed (dpkg --status ${target.pkg})")

                    // STEP 3 (v2 breadth): for the galculator GUI target, launch the binary on
                    // the ALR Wayland compositor (same render path as foot/netsurf/gtk3-widget-
                    // factory: loader → GDK → wl_shm → SurfaceView; rendered = the compositor
                    // frame counter advanced). Guarded to galculator so the hello (CLI) drain is
                    // unchanged. We launch regardless of the dpkg `installed`/`configured` verdict:
                    // /usr/bin/galculator is present from galculator-stage.tar's pre-unpacked
                    // closure (and the dpkg unpack lays it too), so the render proof stands on the
                    // binary's presence, not on the device install having fully succeeded.
                    if (target.pkg == "galculator") {
                        launchGalculatorOnCompositor(rootfsDir, rootfsName)
                    }
                } finally {
                    // Restore: every other probe stays on interpose-only (strict no-regression).
                    android.system.Os.unsetenv("ALR_FAKEROOT")
                    android.system.Os.unsetenv("ALR_REEXEC_INPROC")
                    android.system.Os.unsetenv("ALR_INTERPOSE_DIAG")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "aptdrain EXC: ${android.util.Log.getStackTraceString(e)}")
            }
        }.start()
    }

    // STEP 3 (v2 breadth): launch the apt-INSTALLED galculator on the ALR Wayland
    // compositor — the headline "real GUI app, installed via dpkg, then RUN". Same
    // render path as foot/netsurf/gtk3-widget-factory: the loader runs the guest GTK3
    // binary, GDK binds the in-app compositor, draws into wl_shm, the WaylandPresenter
    // uploads to the SurfaceView. rendered = the compositor frame counter advanced.
    //
    // This runs on the aptdrain thread, which fires EARLY in onCreate — long before the
    // SurfaceView's surfaceCreated stands up the compositor (nativeWaylandCompositorStart).
    // So we first WAIT (bounded) for the compositor to report "running" AND the galculator
    // binary to exist (the dpkg unpack lays /usr/bin/galculator, or galculator-stage.tar's
    // pre-unpacked closure already shipped it). Only then do we sample frames / launch.
    private fun launchGalculatorOnCompositor(rootfsDir: File, rootfsName: String) {
        try {
            val bin = java.io.File(rootfsDir, "usr/bin/galculator")
            // Bounded wait: compositor running + entrypoint present. 60s covers the
            // SurfaceView coming up + the GUI battery ahead of us on the GUI thread.
            var waited = 0
            while (waited < 60000 &&
                !(bin.isFile && nativeWaylandCompositorStatus().contains("STATUS: running"))
            ) {
                Thread.sleep(1000)
                waited += 1000
            }
            if (!bin.isFile) {
                android.util.Log.w("alr_loader", "galculator-launch: /usr/bin/galculator absent — skipping (push galculator-stage.tar)")
                return
            }
            if (!nativeWaylandCompositorStatus().contains("STATUS: running")) {
                android.util.Log.w("alr_loader", "galculator-launch: compositor not running after ${waited}ms — skipping render")
                return
            }
            val framesBefore = nativeWaylandCompositorStatus().intFieldAfter("alr wl frames=")
            // galculator is a GTK3 client; the loader injects GDK_BACKEND=wayland +
            // WAYLAND_DISPLAY for the gtk program family (same as gtk3-widget-factory),
            // so it binds the ALR compositor with no extra args. It opens its main window
            // immediately (no document/file needed), so the watchdog isn't required.
            val client = nativeAlrNativeLoaderProbe(
                packageName,
                applicationInfo.nativeLibraryDir,
                filesDir.absolutePath,
                cacheDir.absolutePath,
                rootfsName,
                "/usr/bin/galculator",
            )
            val status = nativeWaylandCompositorStatus()
            val framesAfter = status.intFieldAfter("alr wl frames=")
            val rendered = framesAfter > framesBefore
            val exec = client.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:")
            android.util.Log.i(
                "alr_loader",
                "galculator-launch: rendered=$rendered frames=$framesBefore->$framesAfter exec=[$exec] (waited ${waited}ms)",
            )
            android.util.Log.i("alr_loader", "galculator-client:\n$client")
            android.util.Log.i("alr_loader", "galculator-status:\n$status")
        } catch (e: Throwable) {
            android.util.Log.e("alr_loader", "galculator-launch EXC: ${android.util.Log.getStackTraceString(e)}")
        }
    }

    // WS-4 §10(b): toolkit launch FUNCTIONAL probes. The sdl2/qt6/netsurf overlays stage
    // above; this RUNS a lightweight, display-free smoke of each toolkit's binary through
    // the ALR native loader (the SAME newline-delimited-argv probe foot/gtkdemo/glmark2 +
    // launchPackageManagerProbes use) and emits the result to logcat tag alr_loader with a
    // stable "toolkit-<name>" marker the integration device drain greps. The GUI bodies
    // (real windows on the compositor) are launched by the integration drain — here we only
    // prove the binary loads its closure + runs (GUEST EXEC PASS) and prints its version.
    // Robust: each toolkit lists candidate binary paths; the first that exists is probed,
    // else we log "toolkit-<name>: missing" (the SDL2 runtime-lib overlay may ship no CLI
    // binary, and the minimal Qt6 closure may omit qtdiag — never a hard failure here).
    private fun launchToolkitProbes(rootfsDir: File, rootfsName: String) {
        Thread {
            try {
                // Wait (bounded) for the concurrent toolkit-stage thread to extract the
                // overlays before probing, so we don't race the extract.
                val netsurfBin = File(rootfsDir, "usr/bin/netsurf-gtk3")
                val sdl2Marker = File(rootfsDir, ".sdl2-staged-${File("/data/local/tmp/sdl2-stage.tar").length()}")
                val qt6Marker = File(rootfsDir, ".qt6-staged-${File("/data/local/tmp/qt6-stage.tar").length()}")
                var waited = 0
                while (waited < 20000 &&
                    !(netsurfBin.isFile || sdl2Marker.isFile || qt6Marker.isFile)
                ) {
                    Thread.sleep(500)
                    waited += 500
                }
                // Run the first existing candidate binary through the loader (display-free
                // --version / -v / immediate-exit path) and log toolkit-<name>.
                fun probe(name: String, candidates: List<String>, args: String, okMarker: String) {
                    val bin = candidates.firstOrNull { File(rootfsDir, it.removePrefix("/")).isFile }
                    if (bin == null) {
                        android.util.Log.i(
                            "alr_loader",
                            "toolkit-$name: missing (none of ${candidates.joinToString(",")} present)",
                        )
                        return
                    }
                    val program = if (args.isEmpty()) bin else "$bin\n$args"
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
                        "toolkit-$name: bin=[$bin] ok=$ok exec=[$exec] marker=[$okMarker]",
                    )
                    android.util.Log.i("alr_loader", "toolkit-$name-out:\n$out")
                }
                // (t1) netsurf-gtk3: the GTK3 browser. -v prints the version banner and the
                // help path exits without opening a window — proves its closure (libcurl/
                // libssh + base GTK3) links and runs through the loader.
                probe(
                    "netsurf",
                    listOf("/usr/bin/netsurf-gtk3", "/usr/bin/netsurf-gtk", "/usr/bin/netsurf"),
                    "-v",
                    "NetSurf",
                )
                // (t2) Qt6: the minimal qt6-wayland closure keeps the qtwayland plugins but
                // may omit CLI tools; probe whichever diagnostic/utility binary survives. A
                // QT_QPA_PLATFORM=minimal-style --version path exits without a display.
                probe(
                    "qt6",
                    // WS-4 build_toolkit_overlays ships qtpaths6 (real file; /usr/bin/qtpaths6 is a
                    // `..`-escaping symlink dropped by §5-E safe-symlink, so use the lib path).
                    listOf("/usr/lib/qt6/bin/qtpaths6", "/usr/bin/qtdiag6", "/usr/lib/qt6/bin/qtdiag", "/usr/bin/qmake6", "/usr/lib/qt6/bin/qmake"),
                    "--version",
                    "Qt",
                )
                // (t3) SDL2: WS-4 build_toolkit_overlays adds libsdl2-tests, whose `testver`
                // links only libSDL2 + libc (no display) and prints the SDL version — a real
                // launchable SDL2 binary. Fall back to sdl2-config if a -dev overlay is present.
                probe(
                    "sdl2",
                    listOf("/usr/libexec/installed-tests/SDL2/testver", "/usr/bin/sdl2-config"),
                    "--version",
                    "SDL",
                )
                // (t4) GIMP babl/gegl FILTER: the babl-gegl overlay ships the 30 babl + 37 gegl
                // op modules as 0o755 (so file-backed PROT_EXEC dlopen succeeds). This probe
                // actually LOADS babl+gegl and APPLIES a gegl op in the guest — no display.
                // Prefer the `/usr/bin/gegl` CLI (runs a real op graph: invert a tiny buffer);
                // else fall back to `gimp-console-3.0` batch Script-Fu which pulls in the full
                // babl/gegl stack and runs a built-in op. Either path forces babl+gegl dlopen.
                val geglCli = File(rootfsDir, "usr/bin/gegl")
                val gimpConsole = File(rootfsDir, "usr/bin/gimp-console-3.0")
                if (geglCli.isFile) {
                    // `gegl --help` enumerates registered ops (forces gegl module registry load,
                    // which dlopen's the gegl-0.4/*.so ops + the babl conversions they pull in).
                    val out = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        "/usr/bin/gegl\n--help",
                    )
                    val exec = out.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:")
                    val ok = out.contains("gegl") || out.contains("Usage") || out.contains("operation")
                    android.util.Log.i("alr_loader", "gimp-filter: via=gegl-cli ok=$ok exec=[$exec]")
                    android.util.Log.i("alr_loader", "gimp-filter-out:\n$out")
                } else if (gimpConsole.isFile) {
                    // gimp-console batch: gimp-version touches the plug-in/babl/gegl init path
                    // without opening a display; a real filter (plug-in-gauss) would also load
                    // the gegl op behind it. We keep it to gimp-version (HONEST: full filter
                    // batch needs a writable image + may exec a plug-in process — separate work).
                    val out = nativeAlrNativeLoaderProbe(
                        packageName,
                        applicationInfo.nativeLibraryDir,
                        filesDir.absolutePath,
                        cacheDir.absolutePath,
                        rootfsName,
                        "/usr/bin/gimp-console-3.0\n-i\n--batch-interpreter=plug-in-script-fu-eval\n-b\n(gimp-version)\n-b\n(gimp-quit 0)",
                    )
                    val exec = out.lineStartingWith("ALR NATIVE LOADER GUEST EXEC:")
                    val ok = out.contains("3.") || out.contains("GIMP") || out.contains("batch command executed successfully")
                    android.util.Log.i("alr_loader", "gimp-filter: via=gimp-console-batch ok=$ok exec=[$exec]")
                    android.util.Log.i("alr_loader", "gimp-filter-out:\n$out")
                } else {
                    android.util.Log.i("alr_loader", "gimp-filter: skipped(not staged) (no /usr/bin/gegl nor /usr/bin/gimp-console-3.0)")
                }
            } catch (e: Throwable) {
                android.util.Log.e("alr_loader", "toolkit EXC: ${android.util.Log.getStackTraceString(e)}")
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

    // Inverse of applyImmersive() for the standalone Chromium app: SHOW the Android
    // status bar + nav bar (soft keys) and inset the content below/above them, so the
    // browser reads like a normal Android app instead of an edge-to-edge fullscreen GUI.
    private fun showSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(true)
            window.decorView.windowInsetsController?.let { c ->
                c.show(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                c.systemBarsBehavior = WindowInsetsController.BEHAVIOR_DEFAULT
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
        }
    }

    // Pop up the soft keyboard targeting the GUI SurfaceView, so on-screen typing
    // also reaches the guest (the hardware/dispatch path works regardless). Best
    // effort: harmless if no IME is present.
    private fun showSoftKeyboard(target: View) {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        imm?.showSoftInput(target, InputMethodManager.SHOW_IMPLICIT)
    }

    // InputConnection that relays soft-keyboard edits to the focused guest via the
    // native IME bridge. fullEditor=false: there is NO local text buffer — every
    // method forwards straight to the compositor (zwp_text_input_v3.commit_string /
    // preedit_string / delete_surrounding_text). commit/preedit text is sent as a
    // UTF-8 ByteArray to avoid JNI's CESU-8 emoji corruption (see the native side).
    private inner class AlrInputConnection(
        targetView: View,
        fullEditor: Boolean,
    ) : BaseInputConnection(targetView, fullEditor) {
        override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
            val s = text?.toString() ?: ""
            nativeWaylandImeCommitText(s.toByteArray(Charsets.UTF_8))
            return true
        }

        override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
            val s = text?.toString() ?: ""
            val bytes = s.toByteArray(Charsets.UTF_8)
            // Composing cursor at the end of the run (caret line, not a selection).
            nativeWaylandImePreedit(bytes, bytes.size)
            return true
        }

        override fun finishComposingText(): Boolean {
            nativeWaylandImePreedit(ByteArray(0), 0)  // clear the composing run
            return true
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            // No local buffer to measure code points against, so char counts are
            // forwarded as UTF-8 byte counts. Exact for ASCII/Latin-1; soft keyboards
            // overwhelmingly delete via sendKeyEvent(KEYCODE_DEL) for non-full editors,
            // which takes the hardware-key path below instead (documented v1 limit).
            nativeWaylandImeDeleteSurrounding(beforeLength, afterLength)
            return true
        }

        override fun sendKeyEvent(event: KeyEvent?): Boolean {
            // Hardware-style keys the soft keyboard sends (Enter/Del/arrows) go through
            // the SAME evdev key path as a physical keyboard, so the guest receives real
            // wl_keyboard keys (KEY_ENTER/KEY_BACKSPACE), not a text edit. Printable text
            // arrives via commitText (commit_string), never here — the two never overlap.
            if (event == null) return super.sendKeyEvent(event)
            val evdev = androidKeyToEvdev(event.keyCode)
            if (evdev == 0) return super.sendKeyEvent(event)
            when (event.action) {
                KeyEvent.ACTION_DOWN -> nativeWaylandInjectKey(evdev, 1)
                KeyEvent.ACTION_UP -> nativeWaylandInjectKey(evdev, 0)
            }
            return true
        }

        override fun performEditorAction(actionCode: Int): Boolean {
            // Enter/Go/Search/Done -> inject KEY_ENTER (evdev 28) down+up to the guest.
            nativeWaylandInjectKey(28, 1)
            nativeWaylandInjectKey(28, 0)
            return true
        }
    }

    // Guest text-input enable/disable upcall from the compositor (JNI, on the
    // compositor thread). Hops to the UI thread, then raises or hides the soft
    // keyboard and re-reads the editor inputType. @Suppress: invoked by name from
    // native code (runtime_report.cpp ime_state_trampoline), not from Kotlin.
    @Suppress("unused")
    fun onGuestImeState(
        enabled: Boolean,
        purpose: Int,
        hint: Int,
        curX: Int,
        curY: Int,
        curW: Int,
        curH: Int,
    ) {
        runOnUiThread {
            val sv = imeSurfaceView ?: return@runOnUiThread
            imeWanted = enabled
            imeInputType = imeInputTypeFor(purpose, hint)
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
                ?: return@runOnUiThread
            if (enabled) {
                sv.requestFocus()
                imm.restartInput(sv)  // re-call onCreateInputConnection so inputType applies
                imm.showSoftInput(sv, InputMethodManager.SHOW_IMPLICIT)
            } else {
                imm.hideSoftInputFromWindow(sv.windowToken, 0)
            }
        }
    }

    // zwp_text_input_v3 content_purpose/hint -> Android EditorInfo.inputType. Enum
    // values mirror text-input-unstable-v3.xml verbatim (see the design §3.3).
    private fun imeInputTypeFor(purpose: Int, hint: Int): Int {
        var t = when (purpose) {
            2, 9 -> InputType.TYPE_CLASS_NUMBER                              // digits, pin
            3 -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED or
                InputType.TYPE_NUMBER_FLAG_DECIMAL                            // number
            4 -> InputType.TYPE_CLASS_PHONE                                  // phone
            5 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI            // url
            6 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS  // email
            8 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD       // password
            10, 11, 12 -> InputType.TYPE_CLASS_DATETIME                       // date/time/datetime
            else -> InputType.TYPE_CLASS_TEXT                                // normal/alpha/name/...
        }
        if (hint and 0x200 != 0) t = t or InputType.TYPE_TEXT_FLAG_MULTI_LINE      // multiline
        if (hint and 0x80 != 0) t = t or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS   // sensitive_data
        if (hint and 0x4 != 0) t = t or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES     // auto_capitalization
        return t
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
    private external fun nativeAlrGpuVkMarshalProbe(): String
    private external fun nativeAlrGpuVkRenderProbe(): String
    private external fun nativeAlrGpuVkDrawProbe(): String
    private external fun nativeAlrGpuVkIcdServiceProbe(): String

    private external fun nativeJitWxProbe(): String

    private external fun nativeAlrGpuScreenCube(surface: android.view.Surface, frames: Int): String

    private external fun nativeHostGpuProbe(): String

    private external fun nativeHostVulkanProbe(): String

    private external fun nativeProbeVulkanSurface(surface: android.view.Surface): String

    // ----- Clipboard bridge (Android <-> Linux-guest selection) -----
    // See docs/design/android-clipboard-bridge.md. ClipboardManager is a
    // UI-thread-affine system service; all get/set runs on the main thread.
    private val clipboard by lazy { getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager }

    // Loop-guard (§7): when WE write the guest's copy into the Android clipboard,
    // the resulting OnPrimaryClipChangedListener is the echo of the guest's own
    // selection — early-return so we don't push it back into the guest.
    @Volatile private var suppressClipEcho = false
    private var clipListenerRegistered = false

    // Android primary clip changed -> push the host selection to the guest.
    private val clipListener = ClipboardManager.OnPrimaryClipChangedListener {
        if (suppressClipEcho) return@OnPrimaryClipChangedListener
        try {
            val clip = clipboard.primaryClip
            if (clip == null || clip.itemCount == 0) {
                nativeWaylandClipboardSetAndroid(emptyArray(), null, null, null)
                return@OnPrimaryClipChangedListener
            }
            val desc = clip.description
            val item = clip.getItemAt(0)
            val text = item.coerceToText(this).toString()
            val html = if (desc != null && desc.hasMimeType(ClipDescription.MIMETYPE_TEXT_HTML))
                item.htmlText else null
            // image/png is phase 2 (content-URI re-encode); text/html + text first.
            val mimes = buildList {
                if (text.isNotEmpty()) {
                    add("text/plain;charset=utf-8"); add("text/plain"); add("UTF8_STRING")
                }
                if (!html.isNullOrEmpty()) add("text/html")
            }.toTypedArray()
            nativeWaylandClipboardSetAndroid(
                mimes, text.ifEmpty { null }, html, null)
        } catch (t: Throwable) {
            // primaryClip reads can throw if the app momentarily lacks focus (API 29+);
            // a clipboard hiccup must never crash the compositor host.
            android.util.Log.w("alr_clipboard", "clip read failed: ${t.message}")
        }
    }

    // native -> Kotlin: the GUEST advertised a new selection (comma-joined mimes).
    // We pull eagerly native-side, so nothing to do here but log (kept for the sink).
    @Keep
    fun onGuestClipboardOffer(mimes: String) {
        android.util.Log.i("alr_clipboard", "guest offer: $mimes")
    }

    // native -> Kotlin: the guest bytes for a text `mime` (UTF-8) are ready ->
    // write them to the Android clipboard (suppressing the echo, §7).
    @Keep
    fun onGuestClipboardText(mime: String, utf8: String) {
        runOnUiThread {
            suppressClipEcho = true
            try {
                clipboard.setPrimaryClip(ClipData.newPlainText("ALR", utf8))
            } catch (t: Throwable) {
                android.util.Log.w("alr_clipboard", "setPrimaryClip failed: ${t.message}")
            }
            // Release the guard after the listener has had a chance to fire+early-return.
            Handler(mainLooper).post { suppressClipEcho = false }
        }
    }

    // native -> Kotlin: the guest bytes for image/png are ready (phase 2). Write an
    // image clip so an Android app can paste it.
    @Keep
    fun onGuestClipboardImage(pngBytes: ByteArray) {
        runOnUiThread {
            android.util.Log.i("alr_clipboard", "guest image: ${pngBytes.size} bytes (phase 2)")
        }
    }

    private external fun nativeWaylandClipboardSetAndroid(
        mimes: Array<String>,
        utf8Text: String?,
        htmlText: String?,
        pngBytes: ByteArray?,
    )

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

    // Dynamic surface resize / rotation / multi-window: hands the compositor the NEW
    // surface (EGL rebind) + the content-area pixel size so wl_output and every mapped
    // toplevel reconfigure. Called from SurfaceHolder.surfaceChanged.
    private external fun nativeWaylandCompositorResize(
        surface: android.view.Surface,
        widthPx: Int,
        heightPx: Int,
        densityDpi: Int,
        xdpi: Float,
        ydpi: Float,
        refreshMilliHz: Int,
    ): String

    // ALR audio sink (in-app PulseAudio-native server -> AAudio; design §5f Option A).
    // Kotlin only starts/stops the sink + reads status — no PCM crosses JNI.
    private external fun nativeAudioSinkStart(xdgRuntimeDir: String, deviceRate: Int): String

    private external fun nativeAudioSinkStatus(): String

    private external fun nativeAudioSinkStop(): String

    private external fun nativeWaylandInjectSelfTest(x: Float, y: Float): String

    private external fun nativeWaylandInjectTouch(id: Int, x: Float, y: Float, phase: Int)
    // Close the atomic set of touch changes for one MotionEvent (wl_touch.frame).
    private external fun nativeWaylandInjectTouchFrame()
    // Drive wl_touch.cancel from ACTION_CANCEL (gesture stolen by the system).
    private external fun nativeWaylandInjectTouchCancel()

    private external fun nativeWaylandInjectKey(evdevKey: Int, pressed: Int)
    private external fun nativeWaylandInjectScroll(x: Float, y: Float, value: Double, axis: Int)

    // --- Android soft-keyboard IME <-> guest zwp_text_input_v3 bridge --------
    // commit/preedit text are passed as UTF-8 ByteArray (NOT String): JNI's
    // GetStringUTFChars yields modified UTF-8 (CESU-8), corrupting emoji/astral
    // code points; text.toByteArray(UTF_8) keeps them intact.
    private external fun nativeWaylandImeCommitText(utf8: ByteArray)
    private external fun nativeWaylandImePreedit(utf8: ByteArray, cursorByte: Int)
    private external fun nativeWaylandImeDeleteSurrounding(beforeBytes: Int, afterBytes: Int)
    private external fun nativeWaylandImeFocusHasTextInput(): Boolean
    private external fun nativeWaylandImeRegisterStateCallback()

    private external fun nativeRenderVulkanSurfaceFrames(
        surface: android.view.Surface,
        encodedFrames: String,
    ): String

    private external fun nativeRenderGpuSurfaceFrames(
        surface: android.view.Surface,
        encodedFrames: String,
    ): String

    // USB host bridge diagnostics (docs/design/android-usb-host.md §5): reports
    // whether the ALR USB bridge AF_UNIX socket (<cacheDir>/alr-usb/usbd.sock,
    // also exported to the guest as ALR_USB_SOCK) is present + connectable. Pure
    // observability; the bridge itself is the Kotlin dev.chanwoo.androlinux.usb.
    // UsbHostBridge started where the guest launch env is assembled.
    private external fun nativeUsbBridgeStatus(appCacheDir: String): String
}
