"""RunningSurfaceActivity 소스 계약 검증 (제품 UX 트랙 A — host C-track).

RunningSurfaceActivity.kt 는 Compose/lifecycle 의존이라 host 에서 빌드 불가하므로,
ADR-004 §4-D2(View 기반 실행화면)·§5-F(AppSession 상태기계) 계약을 *소스 어서션* 으로
검증한다(tests/test_alr_runtime_launch_sources.py 패턴). 빌드/계측 검증은 통합 세션이
build.gradle·AndroidManifest 결선 후 device 에서 수행한다(M-UX-runningsurface 게이트).

검증 항목(task A 요구사항 1~5):
  (1) SurfaceView + SurfaceHolder.Callback → surfaceCreated 에서 bindSurface,
      surfaceDestroyed 에서 unbindSurface.
  (2) 생애주기: onResume→requestForeground, onPause→requestBackground,
      onDestroy→stop(StopReason.USER).
  (3) state(StateFlow) 관찰(lifecycleScope) → STARTING/RENDERING/CRASHED/STOPPED 분기
      + CRASHED 재시작 버튼.
  (4) Intent extra APP_ID 로 appId 수신 + AlrRuntime.launch(LaunchRequest).
  (5) 단일 포그라운드는 AlrRuntime 이 보장(이 화면은 자기 세션만) — 주석 명문화.
  + 빌드 미통합 헤더 / Compose 비의존(View 기반) / 건드리지 않을 파일 명시.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ACTIVITY = (
    ROOT
    / "app/src/main/java/dev/chanwoo/androlinux/ui/RunningSurfaceActivity.kt"
)


def _src() -> str:
    return ACTIVITY.read_text(encoding="utf-8")


def test_file_exists_in_owned_ui_package():
    assert ACTIVITY.exists(), "RunningSurfaceActivity.kt 가 ui/ 에 없음"
    src = _src()
    assert "package dev.chanwoo.androlinux.ui" in src
    assert "class RunningSurfaceActivity" in src
    # androidx ComponentActivity 기반 — lifecycleScope/repeatOnLifecycle 은 LifecycleOwner
    # 를 요구하므로 plain Activity 로는 컴파일이 안 된다(통합 시 ComponentActivity 로 결선).
    # ComponentActivity 는 androidx.activity 소속이라 Compose 의존이 아니다(아래 test_view_based_not_compose).
    assert ": ComponentActivity()" in src


def test_view_based_not_compose():
    """§4-D2: 실행화면은 View 기반(SurfaceView). Compose 비의존이어야 한다."""
    src = _src()
    assert "import android.view.SurfaceView" in src
    assert "import android.view.SurfaceHolder" in src
    # Compose 를 끌어오면 안 된다(입력 인젝션 강결합 → 회귀 위험, §4-D2).
    assert "androidx.compose" not in src, "실행화면은 Compose 비의존이어야 함(§4-D2)"
    assert "setContentView" in src


def test_surface_callback_binds_and_unbinds():
    """(1) surfaceCreated→bindSurface, surfaceDestroyed→unbindSurface."""
    src = _src()
    assert "SurfaceHolder.Callback" in src
    assert "addCallback(surfaceCallback)" in src
    assert "override fun surfaceCreated" in src
    assert "override fun surfaceDestroyed" in src
    # surfaceCreated 본문에 bindSurface, surfaceDestroyed 본문에 unbindSurface.
    created = src.split("override fun surfaceCreated", 1)[1].split(
        "override fun surfaceChanged", 1
    )[0]
    assert "bindSurface(holder)" in created
    destroyed = src.split("override fun surfaceDestroyed", 1)[1]
    assert "unbindSurface()" in destroyed


def test_lifecycle_maps_to_session_contract():
    """(2) onResume→requestForeground, onPause→requestBackground, onDestroy→stop(USER)."""
    src = _src()

    resume = src.split("override fun onResume", 1)[1].split(
        "override fun onPause", 1
    )[0]
    assert "requestForeground()" in resume

    pause = src.split("override fun onPause", 1)[1].split(
        "override fun onDestroy", 1
    )[0]
    assert "requestBackground()" in pause
    # 종료 중이면 background 중복 호출 방지(stop 이 처리).
    assert "isFinishing" in pause

    destroy = src.split("override fun onDestroy", 1)[1]
    assert "stop(StopReason.USER)" in destroy


def test_state_flow_observed_with_lifecycle_scope():
    """(3) state(StateFlow) 를 lifecycleScope + repeatOnLifecycle 로 관찰."""
    src = _src()
    assert "lifecycleScope" in src
    assert "repeatOnLifecycle(Lifecycle.State.STARTED)" in src
    assert "s.state.collectLatest" in src or "state.collectLatest" in src


def test_state_branches_cover_required_states():
    """(3) STARTING/RENDERING/CRASHED/STOPPED 분기 — 로딩·표시·재시작·복귀."""
    src = _src()
    # 분기 자체.
    assert "SessionState.STARTING" in src
    assert "SessionState.RENDERING" in src
    assert "SessionState.CRASHED" in src
    assert "SessionState.STOPPED" in src
    # STOPPED → finish() 로 런처 복귀.
    stopped = src.split("SessionState.STOPPED", 1)[1].split("SessionState.CRASHED", 1)
    assert "finish()" in stopped[0]
    # CRASHED → 재시작 버튼 결선.
    assert "restartSession()" in src
    assert "재시작" in src
    # STARTING → 로딩 인디케이터 오버레이.
    assert "ProgressBar" in src
    assert "loadingOverlay" in src


def test_intent_extra_app_id_and_launch():
    """(4) Intent extra APP_ID 수신 + AlrRuntime.launch(LaunchRequest)."""
    src = _src()
    assert 'EXTRA_APP_ID = "dev.chanwoo.androlinux.extra.APP_ID"' in src
    assert "getStringExtra(EXTRA_APP_ID)" in src
    # entryPath/args/protocol 까지 program-spec 복원.
    assert "getStringExtra(EXTRA_ENTRY_PATH)" in src
    assert "getStringArrayExtra(EXTRA_ARGS)" in src
    # LaunchRequest 합성 + runtime.launch.
    assert "LaunchRequest(" in src
    assert "runtime.launch(request)" in src


def test_single_foreground_delegated_to_runtime():
    """(5) 단일 포그라운드(INV-1~3)는 AlrRuntime 이 강제 — 화면은 자기 세션만."""
    src = _src()
    # 불변식을 런타임이 강제한다는 명문화(INV-2 양도).
    assert "INV-2" in src
    assert "AlrRuntime" in src
    # 화면은 단일 세션 핸들만 들고 관리.
    assert "private var session: AppSession?" in src
    # 전역 멀티세션 라우팅을 이 화면이 하지 않음(자기 것만).
    assert "자기 세션만" in src or "자기 것만" in src


def test_build_not_integrated_header_and_untouched_files():
    """빌드 미통합 헤더 + 건드리지 않을 파일(build.gradle/MainActivity/Manifest) 명시."""
    src = _src()
    assert "빌드 미통합" in src
    # task A 요구: Compose/lifecycle 의존성은 통합 세션.
    assert "lifecycle" in src and "통합 세션" in src
    # 본 트랙이 건드리지 않는 파일 명시.
    assert "build.gradle" in src
    assert "MainActivity" in src
    assert "AndroidManifest" in src
    # 기존 MainActivity(probe 하니스)는 신규 분리 — 안 건드림.
    assert "신규" in src and "분리" in src


def test_runtime_injection_seam_uses_shared_holder():
    """런타임 주입 seam — 통합 후 프로세스 단일 인스턴스(AlrRuntimeHolder)를 공급한다.

    INV-1~3(단일 포그라운드)은 LauncherActivity 와 *같은 런타임 인스턴스* 일 때만 전역으로
    성립하므로, provideRuntime() 은 AlrRuntimeHolder.get(...) 로 공유 인스턴스를 돌려준다.
    v1 홀더는 FakeAlrRuntime; 실 런타임은 홀더의 buildRuntime 한 곳만 교체하면 된다.
    """
    src = _src()
    assert "provideRuntime()" in src
    assert "AlrRuntimeHolder.get(applicationContext)" in src


def test_does_not_touch_main_activity_source():
    """이 트랙은 MainActivity.kt 를 수정하지 않는다(신규 분리 Activity)."""
    main_activity = (
        ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
    )
    # MainActivity 가 존재하면 RunningSurfaceActivity 를 직접 import/참조하지 않아야 함
    # (소유권 분리 — 결선은 통합 세션). 존재하지 않으면 검증 생략.
    if main_activity.exists():
        ma = main_activity.read_text(encoding="utf-8")
        assert "RunningSurfaceActivity" not in ma, (
            "MainActivity 가 RunningSurfaceActivity 를 참조 — 결선은 통합 세션 소유"
        )


# --------------------------------------------------------------------------- #
# BUG-2: 리눅스 타이틀바 ↔ Android 상태바 겹침 — 세이프에어리어 인셋 결선
# --------------------------------------------------------------------------- #
def test_applies_safe_area_insets_after_set_content_view():
    """BUG-2: setContentView 직후 인셋을 적용해 컴포지터 콘텐츠를 세이프에어리어로 가둔다."""
    src = _src()
    assert "applySafeAreaInsets()" in src
    # 반드시 setContentView 뒤(데코뷰/insetsController 존재 후)에 호출.
    set_cv = src.index("setContentView(buildContentView())")
    apply = src.index("applySafeAreaInsets()")
    assert set_cv < apply, "applySafeAreaInsets() 는 setContentView 뒤에 와야 함"


def test_inset_listener_pads_root_by_system_bars_and_cutout():
    """루트에 OnApplyWindowInsetsListener 로 상태바+내비바+컷아웃 만큼 패딩."""
    src = _src()
    assert "setOnApplyWindowInsetsListener" in src
    # systemBars() | displayCutout() = 상태바+내비바+카메라 노치.
    assert "WindowInsets.Type.systemBars()" in src
    assert "WindowInsets.Type.displayCutout()" in src
    # 패딩으로 세이프에어리어 적용(SurfaceView 가 그만큼 줄어 → wl_output 재구성).
    assert "setPadding(" in src


def test_inset_reapplies_on_rotation_via_listener():
    """리스너(일회성 패딩 아님)라서 회전/멀티윈도우에서 인셋이 재적용된다."""
    src = _src()
    # requestApplyInsets 로 최초 1회 강제 + 리스너로 이후 변경 자동 반영.
    assert "requestApplyInsets()" in src
    # 루트 참조를 보관해 리스너를 건다(재적용 대상).
    assert "rootLayout" in src
    assert "private lateinit var rootLayout: FrameLayout" in src


def test_keeps_status_bar_visible_consistent_with_chromium_path():
    """기본 UX 는 바 표시 + 콘텐츠 인셋(크로미움 경로와 일관) — 몰입형 아님(기본값)."""
    src = _src()
    # 바를 SHOW(BEHAVIOR_DEFAULT) — 크로미움 showSystemBars 와 동일한 "바 보임, 겹침 없음".
    assert "WindowInsetsController.BEHAVIOR_DEFAULT" in src
    assert "c.show(" in src
    # 기본 플래그는 비몰입(false): 바 표시 + 인셋.
    assert "IMMERSIVE_FULLSCREEN = false" in src


def test_immersive_alternative_documented_behind_flag():
    """몰입형(바 숨김, 진짜 풀스크린) 대안을 플래그로 제공 — device 튜닝용."""
    src = _src()
    assert "applyImmersiveFullscreen()" in src
    # 몰입형 경로: 바 숨김 + 스와이프 트랜지언트.
    assert "BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE" in src
    # 플래그로 분기(둘 중 하나만 적용).
    assert "if (IMMERSIVE_FULLSCREEN)" in src


def test_inset_size_flows_to_compositor_via_surface_changed():
    """인셋된 SurfaceView 크기가 surfaceChanged→onSurfaceChanged 로 컴포지터에 흐른다."""
    src = _src()
    # 이미 결선된 경로(BUG-2 가 의존): surfaceChanged 에서 onSurfaceChanged(width,height).
    assert "override fun surfaceChanged" in src
    assert "onSurfaceChanged(holder, width, height)" in src
