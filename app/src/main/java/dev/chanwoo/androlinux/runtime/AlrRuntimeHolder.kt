/*
 * AlrRuntimeHolder — 프로세스 단일 AlrRuntime 인스턴스 홀더 (integration-guide §4-A).
 *
 * AlrRuntime 의 단일 포그라운드 불변식(INV-1~3)은 프로세스에 런타임 인스턴스가 *하나* 일
 * 때만 전역으로 성립한다. LauncherActivity 와 RunningSurfaceActivity.provideRuntime() 가
 * 같은 인스턴스를 봐야 하므로 여기서 lazy 싱글턴으로 공급한다.
 *
 * v1: FakeAlrRuntime(런타임 없이 전 화면 흐름 검증). 실 게스트 실행은 buildRuntime 한 곳을
 * NativeAlrRuntime(appContext) 로 교체하면 전 화면이 실 런타임으로 전환된다(§4-B/C).
 */
package dev.chanwoo.androlinux.runtime

import android.content.Context

object AlrRuntimeHolder {
    @Volatile
    private var instance: AlrRuntime? = null

    fun get(context: Context): AlrRuntime =
        instance ?: synchronized(this) {
            instance ?: buildRuntime(context.applicationContext).also { instance = it }
        }

    // Phase 1: REAL runtime — discovers installed Linux apps from the rootfs (.desktop
    // scan) and runs them via the proven compositor + native-loader wiring (AlrNative
    // facade). FakeAlrRuntime stays for Compose @Preview usage elsewhere; only the holder
    // changes (§4-B). The whole UI talks to the AlrRuntime interface, so this single seam
    // flips every screen onto the real runtime.
    private fun buildRuntime(appContext: Context): AlrRuntime =
        NativeAlrRuntime(appContext)
}
