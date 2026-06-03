/*
 * RunningSurfaceActivity — 리눅스 게스트 앱의 *실행 화면*(ADR-004 §3 RunningSurface,
 * §4-D2 D2). Launcher/Catalog/AppDetail/Settings 4화면은 Compose 지만, 이 실행화면만
 * **View 기반 Activity** 다 — SurfaceView + Wayland 컴포지터가 입력 인젝션
 * (nativeWaylandInject*)을 View 콜백에 강결합하고 있어 Compose 로 옮기면 입력 경로를
 * 재작성해야 하기 때문(§4-D2 근거). 그래서 이 화면은 순수 Activity + SurfaceView 다.
 *
 * 역할: 현 MainActivity 가 두 역할(probe 하니스 + 컴포지터 호스트)을 한 클래스에 엉켜
 * 들고 있던 것 중 *컴포지터 호스트* 절반을 떼어낸 신규 분리 Activity. probe 하니스
 * (MainActivity)는 본 트랙이 *건드리지 않는다* — 이 Activity 는 신규 파일로 분리된다
 * (ADR-004 §4-D2 / §8-개선-3). 하드코딩 GIMP/foot 경로 대신 Intent extra(APP_ID +
 * program-spec)로 받은 LaunchRequest 를 AlrRuntime.launch 로 실행한다.
 *
 * 생애주기 ↔ §5-F AppSession 상태기계:
 *   onCreate     : Intent extra → LaunchRequest → AlrRuntime.launch(req) → AppSession.
 *                  SurfaceView 를 contentView 로, SurfaceHolder.Callback 등록.
 *   surfaceCreated   → session.bindSurface(holder)   (컴포지터 본드 + 첫 present)
 *   surfaceDestroyed → session.unbindSurface()       (present 중단)
 *   onResume     → session.requestForeground()  (RENDERING 진입 — INV-2 양도는 런타임이)
 *   onPause      → session.requestBackground()   (BACKGROUND 전이 — D5 정책 device 후)
 *   onDestroy/뒤로가기 → session.stop(StopReason.USER)  (STOPPING→STOPPED)
 *   state(StateFlow) 관찰(lifecycleScope) →
 *     STARTING  : 로딩 인디케이터 오버레이.
 *     RENDERING : SurfaceView 표시(오버레이 숨김).
 *     CRASHED   : 에러 + [재시작] 버튼(같은 LaunchRequest 로 재launch).
 *     STOPPED   : finish() → 런처 복귀.
 *
 * 단일 포그라운드(INV-1~3)는 *AlrRuntime 이 강제* 한다(launch/requestForeground 가
 * 기존 RENDERING 세션을 먼저 BACKGROUND/STOPPING 으로 원자적 양도). 이 화면은
 * 자기 세션만 관리하고, 전역 불변식은 런타임을 신뢰한다(§5-F INV-2).
 *
 * 빌드 미통합: Compose/lifecycle 의존성은 통합 세션이 build.gradle 에 추가한다 — 이
 * 파일은 Compose 비의존(순수 View)이나 androidx.lifecycle(lifecycleScope/repeatOnLifecycle)
 * 과 kotlinx-coroutines(StateFlow collect)에 의존한다. AlrRuntime 주입(FakeAlrRuntime
 * vs 실 런타임)·AndroidManifest 의 <activity> 등록은 *통합 세션* 이 수행한다 — 본 트랙은
 * build.gradle·MainActivity.kt·AndroidManifest.xml 을 건드리지 않는다(통합 세션 D2).
 *
 * 소유: 제품 UX 트랙(ui/ 신규). 기반 계약은 runtime/AlrRuntime.kt §5-F(SSOT).
 */
package dev.chanwoo.androlinux.ui

import android.os.Bundle
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.AlrRuntimeHolder
import dev.chanwoo.androlinux.runtime.AppSession
import dev.chanwoo.androlinux.runtime.LaunchRequest
import dev.chanwoo.androlinux.runtime.NativeAppSession
import dev.chanwoo.androlinux.runtime.SessionState
import dev.chanwoo.androlinux.runtime.StopReason
import dev.chanwoo.androlinux.runtime.SurfaceProtocol
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

/**
 * 게스트 앱 실행 Activity — 자기 [AppSession] 하나의 생애주기만 관리한다.
 *
 * Intent extra 계약(통합 세션이 Launcher→여기로 띄울 때 채움):
 *   [EXTRA_APP_ID]     (필수) 매니페스트 안정 appId(= 세션 키).
 *   [EXTRA_ENTRY_PATH] (필수) rootfs 절대 실행 경로 또는 .desktop 경로.
 *   [EXTRA_ARGS]       (선택) argv[1..] 문자열 배열.
 *   [EXTRA_PROTOCOL]   (선택) "WAYLAND"|"X11" — 미지정 시 WAYLAND.
 *
 * AlrRuntime 주입: v1 은 [FakeAlrRuntime] 로 부트(런타임 없이 흐름 검증). 통합 세션이
 * Application 수준 싱글턴(또는 DI)으로 실 런타임을 주입하도록 [provideRuntime] 를
 * override/교체한다. 본 트랙은 device 런타임을 몰라도 흐름이 동작한다.
 */
class RunningSurfaceActivity : ComponentActivity() {

    private lateinit var surfaceView: SurfaceView
    private lateinit var loadingOverlay: View
    private lateinit var loadingLabel: TextView
    private lateinit var crashOverlay: View
    private lateinit var crashLabel: TextView

    private lateinit var runtime: AlrRuntime
    private lateinit var request: LaunchRequest

    /** 현재 세션 — onCreate 에서 launch, [restartSession] 에서 재launch 시 교체. */
    private var session: AppSession? = null

    /** surfaceCreated 가 이미 왔는지(세션 교체 후 재바인드 판단). */
    private var lastHolder: SurfaceHolder? = null

    // ----------------------------------------------------------------------- //
    // 생애주기
    // ----------------------------------------------------------------------- //

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val parsed = parseRequest()
        if (parsed == null) {
            // APP_ID/entryPath 미수신 — 실행할 게 없음. 런처로 즉시 복귀.
            finish()
            return
        }
        request = parsed
        runtime = provideRuntime()

        setContentView(buildContentView())
        surfaceView.holder.addCallback(surfaceCallback)

        // 세션 시작 — 즉시 핸들 반환, 상태는 StateFlow 로(§5-F).
        startSession()
    }

    override fun onResume() {
        super.onResume()
        // 포그라운드 획득 — RENDERING 진입 요청. INV-2(기존 RENDERING 양도)는 런타임이.
        session?.requestForeground()
    }

    override fun onPause() {
        super.onPause()
        // 포그라운드 양도 — BACKGROUND 전이(suspend or 보류; D5 정책 device 후 확정).
        // 종료(isFinishing) 중이면 stop 이 onDestroy 에서 처리하므로 중복 background 생략.
        if (!isFinishing) {
            session?.requestBackground()
        }
    }

    override fun onDestroy() {
        // 뒤로가기/종료 → 정상 종료 요청(STOPPING→STOPPED). 사용자 사유.
        session?.stop(StopReason.USER)
        // 표면 콜백 해제(누수 방지).
        if (this::surfaceView.isInitialized) {
            surfaceView.holder.removeCallback(surfaceCallback)
        }
        super.onDestroy()
    }

    // ----------------------------------------------------------------------- //
    // 세션 시작 / 재시작 + 상태 관찰
    // ----------------------------------------------------------------------- //

    /** [request] 로 세션을 새로 시작하고 상태 관찰을 건다(onCreate, 재시작 공용). */
    private fun startSession() {
        val s = runtime.launch(request)
        session = s
        // 표면이 이미 살아 있으면(세션 교체) 즉시 재바인드.
        lastHolder?.let { s.bindSurface(it) }
        observeState(s)
    }

    /**
     * CRASHED 후 [재시작] — 기존(크래시) 세션은 이미 종단 상태이므로 그대로 두고
     * 같은 LaunchRequest 로 새 세션을 launch. 새 세션이 surface 를 재바인드한다.
     */
    private fun restartSession() {
        crashOverlay.visibility = View.GONE
        loadingOverlay.visibility = View.VISIBLE
        startSession()
    }

    /**
     * 세션 [s] 의 state(StateFlow)를 lifecycle-aware 하게 관찰해 오버레이/표시를 분기.
     * repeatOnLifecycle(STARTED): 화면이 보이는 동안만 수집(백그라운드에서 누수 없음).
     * collectLatest: 빠른 전이 시 직전 분기 처리를 취소하고 최신 상태만 반영.
     */
    private fun observeState(s: AppSession) {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                s.state.collectLatest { state ->
                    // 관찰 대상이 이미 교체된 세션이면(재시작) 무시.
                    if (session !== s) return@collectLatest
                    renderState(state)
                }
            }
        }
    }

    /** §5-F 상태 → 화면 분기(STARTING/RENDERING/CRASHED/STOPPED + BACKGROUND/STOPPING). */
    private fun renderState(state: SessionState) {
        when (state) {
            SessionState.STARTING -> {
                loadingLabel.text = LABEL_STARTING
                loadingOverlay.visibility = View.VISIBLE
                crashOverlay.visibility = View.GONE
            }
            SessionState.RENDERING -> {
                // 첫 프레임 present — SurfaceView 표시, 오버레이 숨김.
                loadingOverlay.visibility = View.GONE
                crashOverlay.visibility = View.GONE
            }
            SessionState.BACKGROUND -> {
                // 포그라운드 양도됨(다른 앱이 올라옴/홈). 표면은 유지하되 표시 갱신 안 함.
                // v1 은 별도 UI 없음 — onResume 이 다시 requestForeground 로 복귀시킨다.
            }
            SessionState.STOPPING -> {
                loadingLabel.text = LABEL_STOPPING
                loadingOverlay.visibility = View.VISIBLE
                crashOverlay.visibility = View.GONE
            }
            SessionState.STOPPED -> {
                // 게스트 정상 종료(exit=0) → 런처 복귀.
                if (!isFinishing) finish()
            }
            SessionState.CRASHED -> {
                // 비정상 종료(SIGSEGV 등) → 에러 + [재시작].
                loadingOverlay.visibility = View.GONE
                crashLabel.text = LABEL_CRASHED
                crashOverlay.visibility = View.VISIBLE
            }
        }
    }

    // ----------------------------------------------------------------------- //
    // SurfaceHolder.Callback — 컴포지터 본드/언본드 (§5-F bindSurface/unbindSurface)
    // ----------------------------------------------------------------------- //

    private val surfaceCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            lastHolder = holder
            // 표면 생성 → 세션에 holder 양도(nativeWaylandCompositorStart + present).
            session?.bindSurface(holder)
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            lastHolder = holder
            // Real runtime: reconfigure the live compositor output to the new content-area
            // size (rotation / multi-window) so the guest re-lays-out instead of going
            // black. FakeAlrRuntime has no such method → no-op via the safe cast.
            (session as? NativeAppSession)?.onSurfaceChanged(holder, width, height)
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            // 표면 파괴 → present 중단(언본드). 세션 자체는 살아 있을 수 있음(BACKGROUND).
            session?.unbindSurface()
            lastHolder = null
        }
    }

    // ----------------------------------------------------------------------- //
    // Intent extra → LaunchRequest
    // ----------------------------------------------------------------------- //

    /** Intent extra 에서 [LaunchRequest] 를 복원. appId/entryPath 가 없으면 null. */
    private fun parseRequest(): LaunchRequest? {
        val appId = intent?.getStringExtra(EXTRA_APP_ID)?.takeIf { it.isNotBlank() } ?: return null
        val entryPath = intent?.getStringExtra(EXTRA_ENTRY_PATH)?.takeIf { it.isNotBlank() }
            ?: return null
        val args = intent?.getStringArrayExtra(EXTRA_ARGS)?.toList().orEmpty()
        val protocol = intent?.getStringExtra(EXTRA_PROTOCOL)
            ?.let { runCatching { SurfaceProtocol.valueOf(it) }.getOrNull() }
            ?: SurfaceProtocol.WAYLAND
        return LaunchRequest(
            appId = appId,
            entryPath = entryPath,
            args = args,
            protocol = protocol,
        )
    }

    // ----------------------------------------------------------------------- //
    // 런타임 주입 (통합 세션이 실 런타임으로 교체)
    // ----------------------------------------------------------------------- //

    /**
     * AlrRuntime 공급 — 프로세스 단일 인스턴스([AlrRuntimeHolder])를 반환한다. LauncherActivity
     * 와 *같은 인스턴스* 를 봐야 INV-1~3(단일 포그라운드)이 전역으로 성립하므로 홀더로 가져온다.
     * v1 홀더는 [FakeAlrRuntime]; §4-B/C 에서 홀더의 buildRuntime 한 곳만 실 런타임으로 바꾸면
     * 이 화면도 자동 전환된다(런타임 없이 흐름 동작).
     */
    private fun provideRuntime(): AlrRuntime = AlrRuntimeHolder.get(applicationContext)

    // ----------------------------------------------------------------------- //
    // 뷰 구성 (순수 View — Compose 아님)
    // ----------------------------------------------------------------------- //

    private fun buildContentView(): View {
        val root = FrameLayout(this).apply {
            layoutParams = ViewGroup.LayoutParams(MATCH, MATCH)
        }

        surfaceView = SurfaceView(this).apply {
            layoutParams = FrameLayout.LayoutParams(MATCH, MATCH)
            // Route touch into the focused Wayland client (scroll/click/draw). Real runtime
            // only; FakeAlrRuntime sessions ignore it via the safe cast. Mirrors
            // runChromiumStandalone's SurfaceView touch forwarding.
            setOnTouchListener { _, ev ->
                (session as? NativeAppSession)?.injectTouch(ev)
                true
            }
        }
        root.addView(surfaceView)

        // 로딩 오버레이(STARTING/STOPPING) — 인디케이터 + 라벨.
        loadingOverlay = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(MATCH, MATCH)
            visibility = View.VISIBLE
        }
        val loadingBox = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER)
        }
        loadingBox.addView(ProgressBar(this).apply {
            layoutParams = FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER)
        })
        loadingLabel = TextView(this).apply {
            text = LABEL_STARTING
            layoutParams = FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER).apply {
                topMargin = LABEL_TOP_MARGIN_PX
            }
        }
        loadingBox.addView(loadingLabel)
        (loadingOverlay as FrameLayout).addView(loadingBox)
        root.addView(loadingOverlay)

        // 크래시 오버레이(CRASHED) — 에러 라벨 + [재시작].
        crashOverlay = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(MATCH, MATCH)
            visibility = View.GONE
        }
        val crashBox = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER)
        }
        crashLabel = TextView(this).apply {
            text = LABEL_CRASHED
            layoutParams = FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER)
        }
        crashBox.addView(crashLabel)
        val restartButton = Button(this).apply {
            text = LABEL_RESTART
            layoutParams = FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER).apply {
                topMargin = LABEL_TOP_MARGIN_PX
            }
            setOnClickListener { restartSession() }
        }
        crashBox.addView(restartButton)
        (crashOverlay as FrameLayout).addView(crashBox)
        root.addView(crashOverlay)

        return root
    }

    companion object {
        const val EXTRA_APP_ID = "dev.chanwoo.androlinux.extra.APP_ID"
        const val EXTRA_ENTRY_PATH = "dev.chanwoo.androlinux.extra.ENTRY_PATH"
        const val EXTRA_ARGS = "dev.chanwoo.androlinux.extra.ARGS"
        const val EXTRA_PROTOCOL = "dev.chanwoo.androlinux.extra.PROTOCOL"

        private const val LABEL_STARTING = "앱을 시작하는 중…"
        private const val LABEL_STOPPING = "종료하는 중…"
        private const val LABEL_CRASHED = "앱이 종료되었습니다"
        private const val LABEL_RESTART = "재시작"

        private const val LABEL_TOP_MARGIN_PX = 96
        private const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
        private const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT
    }
}
