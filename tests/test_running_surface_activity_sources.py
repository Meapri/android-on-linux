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
    # View 기반 Activity — Compose Activity 아님.
    assert ": Activity()" in src


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


def test_runtime_injection_seam_defaults_to_fake():
    """런타임 주입 seam — v1 기본 FakeAlrRuntime, 통합 세션이 실 런타임 교체."""
    src = _src()
    assert "provideRuntime()" in src
    assert "FakeAlrRuntime()" in src
    # 통합 세션이 override/교체할 수 있게 open.
    assert "protected open fun provideRuntime()" in src


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
