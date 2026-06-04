/*
 * LauncherActivity — Compose 진입점 (integration-guide §2-A).
 *
 * MAIN/LAUNCHER intent 를 받아 AlrApp(NavHost: launcher/catalog/appDetail/settings)을
 * setContent 로 띄운다. 현 MainActivity 는 plain Activity(probe 하니스)라 setContent 를
 * 못 쓰므로 이 ComponentActivity 를 런처 진입점으로 둔다(MainActivity 는 LAUNCHER 강등).
 *
 * 런타임: AlrRuntimeHolder(프로세스 단일 인스턴스)에서 가져온다 — RunningSurfaceActivity 와
 * 같은 인스턴스를 공유해야 INV-1~3(단일 포그라운드)이 전역으로 성립한다(§4-A). v1 은
 * FakeAlrRuntime, §4-B/C 에서 실 런타임으로 교체.
 *
 * onLaunchApp: AlrApp 의 실행 위임 콜백 — LaunchRequest 를 RunningSurfaceActivity 의
 * Intent extra 로 매핑해 실행화면을 띄운다.
 */
package dev.chanwoo.androlinux.ui

import android.content.Intent
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.lifecycle.lifecycleScope
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.AlrRuntimeHolder
import dev.chanwoo.androlinux.runtime.BundledCatalog
import dev.chanwoo.androlinux.runtime.InstallProgress
import dev.chanwoo.androlinux.runtime.LaunchRequest
import java.io.File
import kotlinx.coroutines.launch

class LauncherActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val runtime: AlrRuntime = AlrRuntimeHolder.get(applicationContext)
        maybeRunInstallTestHook(runtime)
        setContent {
            AlrApp(
                runtime = runtime,
                onLaunchApp = { req -> startRunningSurface(req) },
            )
        }
    }

    /**
     * DEBUG-only device test hook for the in-app install loop. When
     * `/data/local/tmp/.alr-install-test` is present (content = pkg, default "galculator"),
     * drive `runtime.install(pkg)` EXACTLY as the catalog's Install button does
     * (CatalogViewModel.requestInstall → runtime.install(appId).collect) — same Flow, same
     * AptInstaller pipeline, same refreshInstalledApps() that publishes the new tile to the
     * launcher grid. Logs each InstallProgress to logcat (tag "alr_install_test") so the
     * device run can prove install→rescan→tile WITHOUT GUI-automating Compose taps. Removes
     * the marker after firing so a relaunch does not re-install. No-op when the marker is
     * absent, so normal launches are unaffected.
     */
    private fun maybeRunInstallTestHook(runtime: AlrRuntime) {
        val marker = File("/data/local/tmp/.alr-install-test")
        if (!marker.isFile) return
        val appId = runCatching { marker.readText().trim() }.getOrDefault("").ifEmpty { "galculator" }
        runCatching { marker.delete() }
        Log.i("alr_install_test", "hook armed → runtime.install(\"$appId\")")
        lifecycleScope.launch {
            runtime.install(appId).collect { p ->
                when (p) {
                    is InstallProgress.Running ->
                        Log.i("alr_install_test", "progress $appId ${p.percent}% ${p.stage.name} (${p.stage.label})")
                    is InstallProgress.Done ->
                        Log.i("alr_install_test", "DONE $appId — installedApps now lists: " +
                            runtime.installedApps.value.joinToString { it.appId })
                    is InstallProgress.Failed ->
                        Log.w("alr_install_test", "FAILED $appId: ${p.message}")
                }
            }
        }
    }

    /**
     * AlrApp.onLaunchApp 위임 — LaunchRequest → 실행화면 Intent.
     *
     * 보통 앱은 generic RunningSurfaceActivity 로 라우팅한다(종전 동작). 단, 카탈로그가 이 appId
     * 에 *전용 launchActivity* 를 지정한 경우(chromium → ".ui.ChromiumStandalone")는 그 Activity
     * 로 명시 Intent 를 보낸다 — chromium 은 LEAN runChromiumStandalone 경로(GPU/probe/GIMP/
     * toolkit 스킵)로 띄워야 browser+renderer in-proc re-map 메모리 헤드룸을 확보하기 때문이다
     * (generic 경로면 OOM). alias 는 같은 앱(exported=false)이라 패키지명+클래스명으로 시작
     * 가능. launchActivity 가 없으면(=대다수 앱) 종전대로 RunningSurfaceActivity 로 보낸다.
     */
    private fun startRunningSurface(req: LaunchRequest) {
        val launchActivity = BundledCatalog.launchActivityFor(req.appId)
        if (launchActivity != null) {
            Log.i("alr_launch", "routing ${req.appId} via dedicated activity $launchActivity (LEAN path)")
            val cls = if (launchActivity.startsWith(".")) packageName + launchActivity else launchActivity
            val i = Intent().apply {
                setClassName(this@LauncherActivity, cls)
                // The dedicated alias (ChromiumStandalone) drives its own lean entrypoint; the
                // LaunchRequest payload is not needed there, but pass the appId for parity/logs.
                putExtra(RunningSurfaceActivity.EXTRA_APP_ID, req.appId)
            }
            runCatching { startActivity(i) }
                .onFailure { Log.e("alr_launch", "dedicated-activity launch failed for ${req.appId}: ${Log.getStackTraceString(it)}") }
            return
        }
        val i = Intent(this, RunningSurfaceActivity::class.java).apply {
            putExtra(RunningSurfaceActivity.EXTRA_APP_ID, req.appId)
            putExtra(RunningSurfaceActivity.EXTRA_ENTRY_PATH, req.entryPath)
            putExtra(RunningSurfaceActivity.EXTRA_ARGS, req.args.toTypedArray())
            putExtra(RunningSurfaceActivity.EXTRA_PROTOCOL, req.protocol.name)
        }
        startActivity(i)
    }
}
