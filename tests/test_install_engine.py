"""Host-verified tests for tools/install_engine.py.

순수 로직(가짜 executor/clock/캐시)으로: 직렬 큐의 우선순위/FIFO/취소, 지수
백오프 재시도(횟수/대기 계산/단계 재개), 의존성 충돌 감지(다른 버전/Conflicts/
Breaks, 충돌은 영구 실패), 오프라인 캐시 히트(다운로드 건너뜀), InstallJob
상태기계 전이(합법/불법 가드)를 검증한다.
"""

from __future__ import annotations

import pytest

from tools.apt_catalog import parse_package_index
from tools.install_plan import build_install_plan
from tools.install_engine import (
    Conflict,
    DebCache,
    InMemoryDebCache,
    InstallEngine,
    InstallEngineError,
    InstalledPackage,
    JobState,
    RetryPolicy,
    StageFailure,
    TERMINAL_STATES,
    WORK_STAGES,
    deb_cache_key,
    detect_conflicts,
    plan_cache_hit_ratio,
    plan_cache_keys,
)


# --------------------------------------------------------------------------- #
# Fixtures — small offline index → real InstallPlans
# --------------------------------------------------------------------------- #
INDEX_TEXT = """\
Package: app
Version: 1.0
Architecture: arm64
Section: utils
Depends: liba
Filename: pool/a/app_1.0_arm64.deb
Size: 1000
Installed-Size: 100
Description: the app

Package: liba
Version: 2.0
Architecture: arm64
Filename: pool/a/liba_2.0_arm64.deb
Size: 2000
Installed-Size: 200
Description: library a

Package: other
Version: 5.0
Architecture: arm64
Section: utils
Filename: pool/o/other_5.0_arm64.deb
Size: 300
Installed-Size: 30
Description: another app
"""


@pytest.fixture()
def index():
    return parse_package_index(INDEX_TEXT)


@pytest.fixture()
def plan_app(index):
    return build_install_plan(["app"], index, base=set())


@pytest.fixture()
def plan_other(index):
    return build_install_plan(["other"], index, base=set())


# --------------------------------------------------------------------------- #
# Fake injected dependencies
# --------------------------------------------------------------------------- #

class RecordingExecutor:
    """모든 단계를 성공시키고 호출을 기록하는 가짜 executor.

    download 는 캐시 안 된 .deb 만 '받았다'고 그 키를 돌려준다(엔진이 캐시에 추가).
    """

    def __init__(self):
        self.calls: list[str] = []
        self.downloaded: list[set[str]] = []

    def resolve(self, plan):
        self.calls.append("resolve")

    def download(self, plan, cached_keys):
        self.calls.append("download")
        all_keys = set(plan_cache_keys(plan))
        to_fetch = all_keys - cached_keys
        self.downloaded.append(to_fetch)
        return to_fetch

    def extract(self, plan):
        self.calls.append("extract")

    def register(self, plan):
        self.calls.append("register")


class FlakyExecutor(RecordingExecutor):
    """지정한 단계에서 처음 N번 실패하다가 성공하는 executor."""

    def __init__(self, fail_stage: str, fail_times: int, *, retryable: bool = True):
        super().__init__()
        self.fail_stage = fail_stage
        self.fail_times = fail_times
        self.retryable = retryable
        self.seen = 0

    def _maybe_fail(self, stage: str):
        if stage == self.fail_stage:
            self.seen += 1
            if self.seen <= self.fail_times:
                raise StageFailure(
                    f"boom at {stage} ({self.seen})", retryable=self.retryable
                )

    def resolve(self, plan):
        self._maybe_fail("resolve")
        super().resolve(plan)

    def download(self, plan, cached_keys):
        self._maybe_fail("download")
        return super().download(plan, cached_keys)

    def extract(self, plan):
        self._maybe_fail("extract")
        super().extract(plan)

    def register(self, plan):
        self._maybe_fail("register")
        super().register(plan)


class FakeClock:
    """주입 sleep — 실제로 자지 않고 누적 대기만 기록."""

    def __init__(self):
        self.slept: list[float] = []

    def sleep(self, seconds: float):
        self.slept.append(seconds)

    @property
    def total(self) -> float:
        return sum(self.slept)


def make_engine(executor, **kwargs) -> InstallEngine:
    clock = FakeClock()
    engine = InstallEngine(executor=executor, sleep=clock.sleep, **kwargs)
    engine._fake_clock = clock  # type: ignore[attr-defined]  # 테스트 편의
    return engine


# --------------------------------------------------------------------------- #
# 큐 — 직렬 / 우선순위 / FIFO / 취소
# --------------------------------------------------------------------------- #
def test_single_job_runs_all_stages_in_order(plan_app):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("j1", plan_app)
    job = eng.run_next()
    assert job.state == JobState.DONE
    assert exe.calls == ["resolve", "download", "extract", "register"]


def test_queue_priority_order(plan_app, plan_other):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("low", plan_app, priority=200)
    eng.enqueue("high", plan_other, priority=10)
    # higher priority (lower number) first
    first = eng.run_next()
    assert first.job_id == "high"
    second = eng.run_next()
    assert second.job_id == "low"


def test_queue_fifo_within_same_priority(plan_app, plan_other):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("a", plan_app, priority=100)
    eng.enqueue("b", plan_other, priority=100)
    order = [eng.run_next().job_id, eng.run_next().job_id]
    assert order == ["a", "b"]


def test_run_all_processes_everything(plan_app, plan_other):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("a", plan_app)
    eng.enqueue("b", plan_other)
    done = eng.run_all()
    assert {j.job_id for j in done} == {"a", "b"}
    assert all(j.state == JobState.DONE for j in done)


def test_cancel_queued_job(plan_app):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("j1", plan_app)
    assert eng.cancel("j1") is True
    assert eng.get("j1").state == JobState.CANCELLED
    # cancelled job is not picked up by the queue
    assert eng.run_next() is None
    assert exe.calls == []


def test_cancel_terminal_job_returns_false(plan_app):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("j1", plan_app)
    eng.run_next()
    assert eng.get("j1").state == JobState.DONE
    assert eng.cancel("j1") is False


def test_duplicate_job_id_rejected(plan_app):
    eng = make_engine(RecordingExecutor())
    eng.enqueue("j1", plan_app)
    with pytest.raises(InstallEngineError):
        eng.enqueue("j1", plan_app)


def test_unknown_job_lookup_raises():
    eng = make_engine(RecordingExecutor())
    with pytest.raises(InstallEngineError):
        eng.get("nope")


def test_run_next_empty_returns_none():
    eng = make_engine(RecordingExecutor())
    assert eng.run_next() is None


# --------------------------------------------------------------------------- #
# 재시도 — 지수 백오프
# --------------------------------------------------------------------------- #
def test_retry_policy_backoff_schedule():
    p = RetryPolicy(max_retries=3, base_delay=1.0, factor=2.0, max_delay=60.0)
    assert p.delay_for_retry(0) == 0.0
    assert p.delay_for_retry(1) == 1.0
    assert p.delay_for_retry(2) == 2.0
    assert p.delay_for_retry(3) == 4.0


def test_retry_policy_clamps_to_max_delay():
    p = RetryPolicy(max_retries=10, base_delay=10.0, factor=10.0, max_delay=30.0)
    assert p.delay_for_retry(5) == 30.0  # 10*10^4 clamped to 30


def test_retry_policy_should_retry_window():
    p = RetryPolicy(max_retries=2)
    assert p.should_retry(1) is True
    assert p.should_retry(2) is True
    assert p.should_retry(3) is False


def test_retry_policy_validation():
    with pytest.raises(InstallEngineError):
        RetryPolicy(max_retries=-1)
    with pytest.raises(InstallEngineError):
        RetryPolicy(factor=0.5)


def test_job_retries_then_succeeds(plan_app):
    # download fails twice then succeeds → job DONE after 2 retries
    exe = FlakyExecutor("download", fail_times=2)
    eng = make_engine(exe, retry_policy=RetryPolicy(max_retries=3, base_delay=1.0))
    eng.enqueue("j1", plan_app)
    eng.run_all()
    job = eng.get("j1")
    assert job.state == JobState.DONE
    assert job.attempts == 2  # two failures before success
    # backoff slept for retry 1 and retry 2: 1.0 + 2.0
    assert eng._fake_clock.total == pytest.approx(3.0)


def test_job_fails_permanently_after_retries_exhausted(plan_app):
    exe = FlakyExecutor("download", fail_times=99)  # always fails
    eng = make_engine(exe, retry_policy=RetryPolicy(max_retries=2, base_delay=1.0))
    eng.enqueue("j1", plan_app)
    eng.run_all()
    job = eng.get("j1")
    assert job.state == JobState.FAILED
    # 1 initial + 2 retries = 3 attempts
    assert job.attempts == 3
    assert "boom" in job.last_error
    # no further processing
    assert eng.run_next() is None


def test_retry_resumes_from_failed_stage_not_from_start(plan_app):
    # fail at EXTRACT once → resolve/download must NOT run a second time
    exe = FlakyExecutor("extract", fail_times=1)
    eng = make_engine(exe, retry_policy=RetryPolicy(max_retries=2, base_delay=0.0))
    eng.enqueue("j1", plan_app)
    eng.run_all()
    assert eng.get("j1").state == JobState.DONE
    # resolve once, download once, extract twice (1 fail + 1 ok), register once
    assert exe.calls.count("resolve") == 1
    assert exe.calls.count("download") == 1
    assert exe.calls.count("extract") == 1  # only the successful one is recorded
    assert exe.calls.count("register") == 1


def test_non_retryable_failure_is_permanent(plan_app):
    exe = FlakyExecutor("download", fail_times=1, retryable=False)
    eng = make_engine(exe, retry_policy=RetryPolicy(max_retries=5))
    eng.enqueue("j1", plan_app)
    eng.run_all()
    job = eng.get("j1")
    assert job.state == JobState.FAILED
    assert job.attempts == 1  # no retry attempted


# --------------------------------------------------------------------------- #
# 의존성 충돌 감지
# --------------------------------------------------------------------------- #
def test_detect_conflict_version_mismatch(plan_app):
    installed = {"liba": InstalledPackage(name="liba", version="1.0")}  # plan wants 2.0
    conflicts = detect_conflicts(plan_app, installed)
    assert any(c.reason == "version-mismatch" and c.package == "liba" for c in conflicts)


def test_no_conflict_same_version(plan_app):
    installed = {
        "app": InstalledPackage(name="app", version="1.0"),
        "liba": InstalledPackage(name="liba", version="2.0"),
    }
    assert detect_conflicts(plan_app, installed) == []


def test_detect_conflict_installed_declares_conflicts(plan_app):
    installed = {
        "rival": InstalledPackage(
            name="rival", version="1.0", conflicts=frozenset({"app"})
        )
    }
    conflicts = detect_conflicts(plan_app, installed)
    assert any(c.reason == "conflicts" and c.package == "app" for c in conflicts)


def test_detect_conflict_installed_declares_breaks(plan_app):
    installed = {
        "rival": InstalledPackage(
            name="rival", version="1.0", breaks=frozenset({"liba"})
        )
    }
    conflicts = detect_conflicts(plan_app, installed)
    assert any(c.reason == "breaks" and c.package == "liba" for c in conflicts)


def test_detect_conflict_new_pkg_declares_conflicts(plan_app):
    installed = {"victim": InstalledPackage(name="victim", version="1.0")}
    declared = {"app": frozenset({"victim"})}
    conflicts = detect_conflicts(plan_app, installed, declared_conflicts=declared)
    assert any(c.reason == "conflicts" for c in conflicts)


def test_engine_conflict_is_permanent_failure(plan_app):
    exe = RecordingExecutor()
    eng = make_engine(exe, retry_policy=RetryPolicy(max_retries=5))
    eng.installed["liba"] = InstalledPackage(name="liba", version="1.0")  # mismatch
    eng.enqueue("j1", plan_app)
    eng.run_all()
    job = eng.get("j1")
    assert job.state == JobState.FAILED
    assert job.attempts == 1  # conflict is non-retryable
    assert job.conflicts  # recorded for UI
    # executor never got past resolve
    assert exe.calls == []


# --------------------------------------------------------------------------- #
# 오프라인 캐시
# --------------------------------------------------------------------------- #
def test_deb_cache_key_is_deterministic():
    k1 = deb_cache_key("app", "1.0", "pool/a/app_1.0_arm64.deb")
    k2 = deb_cache_key("app", "1.0", "pool/a/app_1.0_arm64.deb")
    assert k1 == k2
    assert "/" not in k1


def test_cache_hit_skips_download(plan_app):
    exe = RecordingExecutor()
    cache = InMemoryDebCache()
    # pre-seed cache with ALL plan keys
    for k in plan_cache_keys(plan_app):
        cache.add(k)
    eng = make_engine(exe, cache=cache)
    eng.enqueue("j1", plan_app)
    eng.run_next()
    # download was called but fetched nothing (all cached)
    assert exe.downloaded == [set()]


def test_cache_partial_hit_downloads_rest(plan_app):
    exe = RecordingExecutor()
    cache = InMemoryDebCache()
    keys = plan_cache_keys(plan_app)
    cache.add(keys[0])  # only first cached
    eng = make_engine(exe, cache=cache)
    eng.enqueue("j1", plan_app)
    eng.run_next()
    fetched = exe.downloaded[0]
    assert keys[0] not in fetched
    assert keys[1] in fetched


def test_download_populates_cache(plan_app):
    exe = RecordingExecutor()
    cache = InMemoryDebCache()
    eng = make_engine(exe, cache=cache)
    eng.enqueue("j1", plan_app)
    eng.run_next()
    # after a successful run, all plan keys are cached
    assert plan_cache_hit_ratio(plan_app, cache) == 1.0


def test_plan_cache_hit_ratio_empty_plan(index):
    empty = build_install_plan(["liba"], index, base={"liba"})  # already base
    assert empty.debs == ()
    assert plan_cache_hit_ratio(empty, InMemoryDebCache()) == 1.0


def test_second_job_reuses_cache_from_first(index):
    # two jobs sharing 'liba': the second's liba download is a cache hit
    exe = RecordingExecutor()
    cache = InMemoryDebCache()
    eng = make_engine(exe, cache=cache)
    p1 = build_install_plan(["app"], index, base=set())   # app + liba
    p2 = build_install_plan(["liba"], index, base=set())  # liba only
    eng.enqueue("first", p1)
    eng.enqueue("second", p2)
    eng.run_all()
    # second download fetched nothing new (liba already cached by first)
    assert exe.downloaded[-1] == set()


# --------------------------------------------------------------------------- #
# 상태기계 전이
# --------------------------------------------------------------------------- #
def test_happy_path_history(plan_app):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    job = eng.enqueue("j1", plan_app)
    eng.run_next()
    assert job.history == [
        JobState.RESOLVING,
        JobState.DOWNLOADING,
        JobState.EXTRACTING,
        JobState.REGISTERING,
        JobState.DONE,
    ]


def test_failure_history_includes_failed_and_retrying(plan_app):
    exe = FlakyExecutor("download", fail_times=1)
    eng = make_engine(exe, retry_policy=RetryPolicy(max_retries=2, base_delay=0.0))
    job = eng.enqueue("j1", plan_app)
    eng.run_all()
    assert JobState.FAILED in job.history
    assert JobState.RETRYING in job.history
    assert job.history[-1] == JobState.DONE


def test_illegal_transition_is_guarded(plan_app):
    eng = make_engine(RecordingExecutor())
    job = eng.enqueue("j1", plan_app)
    # QUEUED -> DONE is not a legal transition
    with pytest.raises(InstallEngineError):
        eng._set_state(job, JobState.DONE)


def test_terminal_states_have_no_outgoing(plan_app):
    eng = make_engine(RecordingExecutor())
    job = eng.enqueue("j1", plan_app)
    eng.run_next()
    assert job.state in TERMINAL_STATES
    with pytest.raises(InstallEngineError):
        eng._set_state(job, JobState.RESOLVING)


def test_work_stages_constant_order():
    assert WORK_STAGES == (
        JobState.RESOLVING,
        JobState.DOWNLOADING,
        JobState.EXTRACTING,
        JobState.REGISTERING,
    )


def test_registering_updates_installed_db(plan_app):
    exe = RecordingExecutor()
    eng = make_engine(exe)
    eng.enqueue("j1", plan_app)
    eng.run_next()
    # after DONE, the engine's installed DB reflects the new packages
    assert "app" in eng.installed
    assert eng.installed["app"].version == "1.0"
    assert eng.installed["liba"].version == "2.0"


def test_conflict_dataclass_as_dict():
    c = Conflict(package="app", reason="version-mismatch", detail="x")
    d = c.as_dict()
    assert d == {"package": "app", "reason": "version-mismatch", "detail": "x"}
