"""설치 엔진 — install_plan 위의 큐/재시도/충돌/캐시/상태기계 (순수 로직).

C 트랙(설치 엔진). ``tools.install_plan`` 이 "무엇을 받아 어떻게 설치할지"의
*플랜*(closure → stage-tar 지시)을 만든다면, 이 모듈은 그 플랜들을 **실제로
처리하는 엔진의 순수 로직**을 담는다: 여러 설치 요청을 직렬 큐로 우선순위/취소
관리하고, 단계별 실패에 지수 백오프로 재시도하고, 이미 설치된 다른 버전이나
Conflicts/Breaks 와의 의존성 충돌을 감지하고, 다운로드 완료한 .deb 를 오프라인
캐시 키로 재사용하고, 한 설치 작업(InstallJob)의 상태기계 전이를 강제한다.

순수 로직 원칙 — 이 모듈은 **네트워크/디스크 IO 를 직접 하지 않는다**. 다운로드/
추출/등록은 주입 가능한 인터페이스(``InstallExecutor`` 프로토콜)로 들어오고,
시간(백오프 sleep)·다운로드 캐시 디렉토리·이미 설치된 패키지 DB 도 전부 주입
파라미터다. 그래서 host-side 에서 가짜 executor·가짜 clock 으로 전이/재시도/충돌/
캐시 히트를 결정적으로 검증할 수 있다(tests/test_install_engine.py).

**빌드 미통합: 이 모듈은 순수 파이썬 host 로직이며 Android/네이티브 의존이 없다.
Compose/lifecycle 의존성(있다면)은 통합 세션이 배선한다.** 디바이스 측 실제
다운로드/추출 구현(deb_closure.build_overlay / RootfsInstaller.extractOverlayTar)은
이 엔진의 ``InstallExecutor`` 를 구현하는 어댑터로 통합 세션에서 연결된다.

경계:
  * closure 해석·.deb 목록·크기·stage-tar 지시 = ``install_plan`` (재사용, 수정 X).
  * 카탈로그 엔트리/매니페스트 스키마 = ``apt_catalog`` / ``alr_manifest`` (import X
    직접; 이 모듈은 plan 레벨에서만 동작하고 매니페스트는 안 본다).
  * D1 단일 포그라운드(RENDERING≤1)는 *실행* 불변식이라 여기 무관 — 설치는 앱
    실행과 독립적인 백그라운드 작업이다(직렬 큐로 1개씩 처리).

host 검증: tests/test_install_engine.py (가짜 executor/clock, 오프라인).
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field, replace
from enum import Enum
from typing import Callable, Protocol

from tools.install_plan import InstallPlan


# --------------------------------------------------------------------------- #
# InstallJob 상태기계
# --------------------------------------------------------------------------- #
#
# QUEUED  →  RESOLVING  →  DOWNLOADING  →  EXTRACTING  →  REGISTERING  →  DONE
#   │            │             │              │              │
#   │            └──── 어느 작업 단계든 실패하면 ── FAILED ── (재시도 정책이
#   │                                                  남았으면) ── RETRYING ──┐
#   │                                                                          │
#   └── 취소되면 어디서든 ── CANCELLED                     RETRYING ── 큐 재진입 ┘
#
# RETRYING 은 "다음 시도까지 대기" 상태이고, 백오프가 지나면 다시 RESOLVING 부터
# (또는 마지막 성공 단계 다음부터 — 아래 resume_stage 참조) 재개한다.

class JobState(str, Enum):
    """InstallJob 의 상태. str 혼합 enum 이라 직렬화/비교가 쉽다."""

    QUEUED = "QUEUED"
    RESOLVING = "RESOLVING"
    DOWNLOADING = "DOWNLOADING"
    EXTRACTING = "EXTRACTING"
    REGISTERING = "REGISTERING"
    DONE = "DONE"
    FAILED = "FAILED"
    RETRYING = "RETRYING"
    CANCELLED = "CANCELLED"


# 작업이 거치는 활성 작업 단계의 순서(상태기계의 "전진" 경로). 재시도/재개는
# 이 순서 안에서만 점프한다.
WORK_STAGES: tuple[JobState, ...] = (
    JobState.RESOLVING,
    JobState.DOWNLOADING,
    JobState.EXTRACTING,
    JobState.REGISTERING,
)

# 합법적 전이 표 — 엔진이 set_state 로 강제. 표 밖 전이는 InstallEngineError.
_ALLOWED_TRANSITIONS: dict[JobState, frozenset[JobState]] = {
    JobState.QUEUED: frozenset({JobState.RESOLVING, JobState.CANCELLED}),
    JobState.RESOLVING: frozenset(
        {JobState.DOWNLOADING, JobState.FAILED, JobState.CANCELLED}
    ),
    JobState.DOWNLOADING: frozenset(
        {JobState.EXTRACTING, JobState.FAILED, JobState.CANCELLED}
    ),
    JobState.EXTRACTING: frozenset(
        {JobState.REGISTERING, JobState.FAILED, JobState.CANCELLED}
    ),
    JobState.REGISTERING: frozenset(
        {JobState.DONE, JobState.FAILED, JobState.CANCELLED}
    ),
    # FAILED → RETRYING(시도 남음) 또는 그대로 종착(시도 소진). 취소도 가능.
    JobState.FAILED: frozenset({JobState.RETRYING, JobState.CANCELLED}),
    # RETRYING → 백오프 후 작업 단계 재진입(보통 RESOLVING) 또는 취소.
    JobState.RETRYING: frozenset(
        {JobState.RESOLVING, JobState.DOWNLOADING, JobState.EXTRACTING,
         JobState.REGISTERING, JobState.CANCELLED}
    ),
    # 종착 상태들 — 전이 없음.
    JobState.DONE: frozenset(),
    JobState.CANCELLED: frozenset(),
}

# 더 진행할 수 없는 종착 상태.
TERMINAL_STATES: frozenset[JobState] = frozenset(
    {JobState.DONE, JobState.CANCELLED}
)


class InstallEngineError(RuntimeError):
    """엔진 불변식 위반(불법 상태 전이, 알 수 없는 job 등)."""


class StageFailure(Exception):
    """executor 가 한 작업 단계에서 실패를 알릴 때 던지는 예외.

    ``retryable=False`` 면 재시도 정책이 남아도 즉시 영구 FAILED 로 간다
    (예: 의존성 충돌 — 같은 입력으로 재시도해봐야 또 실패).
    """

    def __init__(self, message: str, *, retryable: bool = True) -> None:
        super().__init__(message)
        self.retryable = retryable


# --------------------------------------------------------------------------- #
# 재시도 정책 — 지수 백오프 (순수 모델, sleep 은 주입)
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class RetryPolicy:
    """지수 백오프 재시도 정책(순수 계산 — 실제 대기는 엔진의 sleep 콜백).

    attempt 0 이 첫 시도(백오프 없음). 그 다음 재시도의 대기 시간은
    ``base_delay * factor**(attempt-1)`` 을 ``max_delay`` 로 클램프한 값이다.
    ``max_retries`` 는 *추가* 재시도 횟수(첫 시도 제외) — 총 시도는 1+max_retries.
    """

    max_retries: int = 3
    base_delay: float = 1.0
    factor: float = 2.0
    max_delay: float = 60.0

    def __post_init__(self) -> None:
        if self.max_retries < 0:
            raise InstallEngineError("max_retries must be >= 0")
        if self.base_delay < 0 or self.max_delay < 0:
            raise InstallEngineError("delays must be >= 0")
        if self.factor < 1.0:
            raise InstallEngineError("backoff factor must be >= 1.0")

    def delay_for_retry(self, retry_number: int) -> float:
        """``retry_number`` 번째 재시도(1-기반) 전에 대기할 초.

        retry_number=1 → base_delay, =2 → base_delay*factor, ... (max_delay 클램프).
        retry_number<=0 은 0(첫 시도엔 대기 없음).
        """
        if retry_number <= 0:
            return 0.0
        delay = self.base_delay * (self.factor ** (retry_number - 1))
        return min(delay, self.max_delay)

    def should_retry(self, attempts_made: int) -> bool:
        """이미 ``attempts_made`` 번 시도했을 때 한 번 더 시도할지.

        attempts_made 는 *실패한 시도 수*. 1 + max_retries 번까지 허용하므로
        attempts_made <= max_retries 이면 재시도 여지가 있다.
        """
        return attempts_made <= self.max_retries


# --------------------------------------------------------------------------- #
# 의존성 충돌 감지 (이미 설치된 다른 버전 / Conflicts / Breaks)
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class InstalledPackage:
    """이미 rootfs 에 설치된 패키지(충돌 감지 입력).

    conflicts/breaks 는 *이 설치된 패키지가* 선언한 Conflicts:/Breaks: 의
    패키지명 집합(버전 제약은 v1 에선 이름 수준만 — 보수적으로 이름이 겹치면
    충돌로 본다).
    """

    name: str
    version: str
    conflicts: frozenset[str] = frozenset()
    breaks: frozenset[str] = frozenset()


@dataclass(frozen=True)
class Conflict:
    """감지된 충돌 한 건(UI 표시/차단용)."""

    package: str          # 새로 설치하려는 패키지
    reason: str           # "version-mismatch" | "conflicts" | "breaks"
    detail: str           # 사람이 읽는 설명

    def as_dict(self) -> dict:
        return {"package": self.package, "reason": self.reason, "detail": self.detail}


def detect_conflicts(
    plan: InstallPlan,
    installed: dict[str, InstalledPackage],
    *,
    declared_conflicts: dict[str, frozenset[str]] | None = None,
    declared_breaks: dict[str, frozenset[str]] | None = None,
) -> list[Conflict]:
    """플랜의 .deb 들 vs 이미 설치된 패키지 → 충돌 목록(없으면 빈 리스트).

    감지 규칙(보수적, v1):
      1) **버전 불일치** — 플랜이 받을 패키지가 이미 *다른 버전*으로 설치돼 있으면
         충돌(stage-tar 추출이 기존 파일을 덮어 ABI/마커가 어긋날 수 있음). 같은
         버전이면 충돌 아님(재설치 무해 — 캐시/idempotent).
      2) **새 패키지가 설치된 패키지를 Conflicts/Breaks** — plan 패키지명이
         ``declared_conflicts[plan_pkg]`` / ``declared_breaks[plan_pkg]`` 에
         들어있고 그 대상이 설치돼 있으면 충돌.
      3) **설치된 패키지가 새 패키지를 Conflicts/Breaks** — 설치된 패키지의
         ``conflicts``/``breaks`` 집합에 plan 패키지명이 들어있으면 충돌.

    declared_* 는 새로 설치할 패키지가 선언한 관계(인덱스 stanza 의 Conflicts:/
    Breaks: 를 호출자가 파싱해 넘김). 없으면 (1)+(3)만 본다.
    """
    declared_conflicts = declared_conflicts or {}
    declared_breaks = declared_breaks or {}
    conflicts: list[Conflict] = []

    for deb in plan.debs:
        inst = installed.get(deb.name)
        # (1) 버전 불일치
        if inst is not None and inst.version != deb.version:
            conflicts.append(
                Conflict(
                    package=deb.name,
                    reason="version-mismatch",
                    detail=(
                        f"{deb.name} {deb.version} 를 설치하려 하지만 이미 "
                        f"{inst.version} 가 설치돼 있습니다"
                    ),
                )
            )
        # (2) 새 패키지가 설치된 무언가를 Conflicts/Breaks
        for rel_map, reason in (
            (declared_conflicts, "conflicts"),
            (declared_breaks, "breaks"),
        ):
            for target in rel_map.get(deb.name, ()):  # type: ignore[union-attr]
                if target in installed:
                    conflicts.append(
                        Conflict(
                            package=deb.name,
                            reason=reason,
                            detail=f"{deb.name} 가 설치된 {target} 와(과) {reason}",
                        )
                    )
        # (3) 설치된 패키지가 새 패키지를 Conflicts/Breaks
        for inst_pkg in installed.values():
            if deb.name in inst_pkg.conflicts:
                conflicts.append(
                    Conflict(
                        package=deb.name,
                        reason="conflicts",
                        detail=f"설치된 {inst_pkg.name} 가 {deb.name} 와(과) 충돌",
                    )
                )
            if deb.name in inst_pkg.breaks:
                conflicts.append(
                    Conflict(
                        package=deb.name,
                        reason="breaks",
                        detail=f"설치된 {inst_pkg.name} 가 {deb.name} 를 깨뜨림",
                    )
                )
    return conflicts


# --------------------------------------------------------------------------- #
# 오프라인 캐시 — 다운로드 완료한 .deb 재사용 키
# --------------------------------------------------------------------------- #

def deb_cache_key(name: str, version: str, filename: str) -> str:
    """한 .deb 의 캐시 키 — 이름+버전+풀경로 basename 으로 결정적 식별.

    같은 (name, version) 라도 다른 pool 경로면 다른 산출일 수 있으나, mirror 의
    Filename 은 버전까지 인코딩하므로 (name, version, basename(filename)) 이면
    충분히 유일하다. 슬래시는 키에서 제거(파일명으로 안전).
    """
    base = filename.rsplit("/", 1)[-1] if filename else f"{name}_{version}.deb"
    return f"{name}@{version}#{base}"


class DebCache(Protocol):
    """다운로드 완료한 .deb 의 오프라인 캐시(주입). 순수 키-기반."""

    def has(self, key: str) -> bool: ...
    def add(self, key: str) -> None: ...


@dataclass
class InMemoryDebCache:
    """테스트/호스트용 메모리 캐시(키 집합). 디바이스는 디렉토리 어댑터로 대체."""

    keys: set[str] = field(default_factory=set)

    def has(self, key: str) -> bool:
        return key in self.keys

    def add(self, key: str) -> None:
        self.keys.add(key)


# --------------------------------------------------------------------------- #
# InstallExecutor — 주입되는 부수효과(네트워크/IO) 인터페이스
# --------------------------------------------------------------------------- #

class InstallExecutor(Protocol):
    """엔진이 각 작업 단계에서 호출하는 부수효과 인터페이스.

    각 메서드는 성공하면 반환, 실패하면 ``StageFailure`` 를 던진다. ``download``
    는 캐시 히트로 건너뛸 .deb 키 집합을 받고, *실제로 받은*(캐시 추가할) 키
    집합을 돌려준다. 디바이스 통합 세션이 deb_closure/RootfsInstaller 어댑터로
    구현한다.
    """

    def resolve(self, plan: InstallPlan) -> None: ...
    def download(self, plan: InstallPlan, cached_keys: set[str]) -> set[str]: ...
    def extract(self, plan: InstallPlan) -> None: ...
    def register(self, plan: InstallPlan) -> None: ...


# --------------------------------------------------------------------------- #
# InstallJob
# --------------------------------------------------------------------------- #

@dataclass
class InstallJob:
    """한 설치 요청의 상태 — 큐 항목이자 상태기계 인스턴스.

    낮은 ``priority`` 값이 먼저(0 이 최우선). 같은 우선순위면 ``seq``(등록 순서)로
    안정 정렬한다 — FIFO. ``attempts`` 는 *실패한* 시도 수(재시도 판단용).
    """

    job_id: str
    plan: InstallPlan
    priority: int = 100
    seq: int = 0
    state: JobState = JobState.QUEUED
    attempts: int = 0
    resume_stage: JobState = JobState.RESOLVING  # 재개 시 진입할 작업 단계
    last_error: str = ""
    conflicts: tuple[Conflict, ...] = ()
    history: list[JobState] = field(default_factory=list)

    @property
    def is_terminal(self) -> bool:
        return self.state in TERMINAL_STATES

    @property
    def is_active(self) -> bool:
        """큐가 아직 진행시켜야 하는 상태인가(종착도 아니고 영구실패도 아님)."""
        if self.state in TERMINAL_STATES:
            return False
        # FAILED 인데 더 재시도 안 할 거면 사실상 종착(엔진이 그렇게 마킹).
        return True

    def sort_key(self) -> tuple[int, int]:
        return (self.priority, self.seq)


# --------------------------------------------------------------------------- #
# InstallEngine — 직렬 큐 + 상태기계 구동
# --------------------------------------------------------------------------- #

@dataclass
class InstallEngine:
    """설치 작업의 직렬 큐 + 상태기계 엔진(순수 로직).

    한 번에 한 작업만 처리(직렬 — INV: 동시 추출이 rootfs 를 경쟁하지 않음). 큐에서
    우선순위→등록순으로 다음 작업을 꺼내 작업 단계를 순서대로 돌리고, 실패하면
    재시도 정책에 따라 백오프 후 재큐하거나 영구 FAILED 로 종착시킨다.

    부수효과(다운로드/추출/등록)는 ``executor`` 로, 대기는 ``sleep`` 콜백으로,
    시간은 ``clock`` 으로 주입 — 전부 테스트에서 가짜로 대체 가능.
    """

    executor: InstallExecutor
    retry_policy: RetryPolicy = field(default_factory=RetryPolicy)
    cache: DebCache = field(default_factory=InMemoryDebCache)
    sleep: Callable[[float], None] = time.sleep
    clock: Callable[[], float] = time.monotonic
    installed: dict[str, InstalledPackage] = field(default_factory=dict)
    declared_conflicts: dict[str, frozenset[str]] = field(default_factory=dict)
    declared_breaks: dict[str, frozenset[str]] = field(default_factory=dict)

    _queue: list[InstallJob] = field(default_factory=list)
    _jobs: dict[str, InstallJob] = field(default_factory=dict)
    _seq: int = 0
    # 백오프 누적 대기(테스트가 총 대기 시간을 확인할 수 있게).
    total_backoff: float = 0.0

    # ---- 큐 관리 --------------------------------------------------------- #

    def enqueue(
        self, job_id: str, plan: InstallPlan, *, priority: int = 100
    ) -> InstallJob:
        """새 설치 작업을 큐에 넣는다(같은 job_id 재등록은 에러)."""
        if job_id in self._jobs:
            raise InstallEngineError(f"job already exists: {job_id}")
        job = InstallJob(
            job_id=job_id, plan=plan, priority=priority, seq=self._seq
        )
        self._seq += 1
        self._jobs[job_id] = job
        self._queue.append(job)
        return job

    def get(self, job_id: str) -> InstallJob:
        try:
            return self._jobs[job_id]
        except KeyError:
            raise InstallEngineError(f"unknown job: {job_id}") from None

    @property
    def pending(self) -> list[InstallJob]:
        """아직 처리할(QUEUED/RETRYING) 작업을 처리 순서대로."""
        ready = [j for j in self._queue if j.state in (JobState.QUEUED, JobState.RETRYING)]
        ready.sort(key=lambda j: j.sort_key())
        return ready

    def cancel(self, job_id: str) -> bool:
        """작업 취소 — 아직 종착 전이면 CANCELLED 로. 이미 종착이면 False.

        직렬 엔진이라 "현재 실행 중" 작업은 단계 사이에서 취소를 확인한다(아래
        _run_job 의 취소 체크). 큐 대기 중 작업은 즉시 CANCELLED.
        """
        job = self.get(job_id)
        if job.state in TERMINAL_STATES:
            return False
        self._set_state(job, JobState.CANCELLED)
        return True

    def _next_job(self) -> InstallJob | None:
        ready = self.pending
        return ready[0] if ready else None

    # ---- 상태 전이 강제 --------------------------------------------------- #

    def _set_state(self, job: InstallJob, new: JobState) -> None:
        allowed = _ALLOWED_TRANSITIONS.get(job.state, frozenset())
        if new not in allowed:
            raise InstallEngineError(
                f"illegal transition {job.state.value} -> {new.value} (job {job.job_id})"
            )
        job.state = new
        job.history.append(new)

    # ---- 구동 ------------------------------------------------------------ #

    def run_next(self) -> InstallJob | None:
        """다음 대기 작업 하나를 종착(DONE/FAILED-소진/CANCELLED)까지 처리.

        처리할 작업이 없으면 None. 직렬 — 항상 한 작업만.
        """
        job = self._next_job()
        if job is None:
            return None
        self._run_job(job)
        return job

    def run_all(self, *, max_iterations: int = 1000) -> list[InstallJob]:
        """대기 작업이 없을 때까지 순서대로 처리하고, 처리한 작업들을 반환.

        max_iterations 는 무한루프 가드(취소되지 않는 영구 재시도 등 방지 —
        정책상 재시도는 유한하므로 정상 경로에선 안 닿는다).
        """
        processed: list[InstallJob] = []
        for _ in range(max_iterations):
            job = self.run_next()
            if job is None:
                break
            processed.append(job)
        return processed

    def _run_job(self, job: InstallJob) -> None:
        """한 작업을 상태기계로 끝까지 구동(재시도 포함)."""
        # RETRYING 으로 큐에 다시 들어온 경우 백오프를 먼저 소화.
        if job.state == JobState.RETRYING:
            delay = self.retry_policy.delay_for_retry(job.attempts)
            if delay > 0:
                self.sleep(delay)
                self.total_backoff += delay
            # RETRYING → 재개 작업 단계로 진입.
            self._set_state(job, job.resume_stage)
        elif job.state == JobState.QUEUED:
            self._set_state(job, JobState.RESOLVING)
        else:
            raise InstallEngineError(
                f"_run_job entered with non-startable state {job.state.value}"
            )

        # 작업 단계 순회.
        start_idx = WORK_STAGES.index(job.state)
        for stage in WORK_STAGES[start_idx:]:
            # 단계 진입 전 취소 체크(직렬 엔진의 협조적 취소 지점).
            if job.state == JobState.CANCELLED:
                return
            # job.state 가 이미 stage 면 그대로, 아니면 전이.
            if job.state != stage:
                self._set_state(job, stage)
            try:
                self._run_stage(job, stage)
            except StageFailure as exc:
                self._handle_failure(job, stage, exc)
                return
        # 모든 단계 통과 → DONE.
        self._set_state(job, JobState.DONE)

    def _run_stage(self, job: InstallJob, stage: JobState) -> None:
        """한 작업 단계의 부수효과를 executor 로 실행."""
        if stage == JobState.RESOLVING:
            # 충돌은 resolve 단계에서 감지 — 충돌 있으면 영구(재시도 무의미) 실패.
            conflicts = detect_conflicts(
                job.plan,
                self.installed,
                declared_conflicts=self.declared_conflicts,
                declared_breaks=self.declared_breaks,
            )
            if conflicts:
                job.conflicts = tuple(conflicts)
                raise StageFailure(
                    f"의존성 충돌 {len(conflicts)}건: "
                    + "; ".join(c.detail for c in conflicts),
                    retryable=False,
                )
            self.executor.resolve(job.plan)
        elif stage == JobState.DOWNLOADING:
            cached_keys = {
                deb_cache_key(d.name, d.version, d.filename)
                for d in job.plan.debs
                if self.cache.has(deb_cache_key(d.name, d.version, d.filename))
            }
            fetched = self.executor.download(job.plan, cached_keys)
            for key in fetched:
                self.cache.add(key)
        elif stage == JobState.EXTRACTING:
            self.executor.extract(job.plan)
        elif stage == JobState.REGISTERING:
            self.executor.register(job.plan)
            # 등록 성공 → installed DB 에 반영(이후 작업의 충돌 감지에 쓰임).
            for deb in job.plan.debs:
                self.installed[deb.name] = InstalledPackage(
                    name=deb.name, version=deb.version
                )
        else:
            raise InstallEngineError(f"not a work stage: {stage.value}")

    def _handle_failure(
        self, job: InstallJob, stage: JobState, exc: StageFailure
    ) -> None:
        """단계 실패 처리 — FAILED 로 가고, 재시도 여지 있으면 RETRYING+재큐."""
        job.attempts += 1
        job.last_error = str(exc)
        self._set_state(job, JobState.FAILED)
        if exc.retryable and self.retry_policy.should_retry(job.attempts):
            # 재시도: 실패한 단계부터 재개(앞 단계는 이미 성공 — 재실행 안 함).
            job.resume_stage = stage
            self._set_state(job, JobState.RETRYING)
            # 큐에 그대로 남아있으므로 pending 이 다시 집어 든다.
        # else: FAILED 그대로(영구). 큐에서 pending 에 안 잡혀 더 처리 안 됨.


# --------------------------------------------------------------------------- #
# 편의 — plan 의 캐시 키들(엔진 밖에서도 캐시 사전조회 가능)
# --------------------------------------------------------------------------- #

def plan_cache_keys(plan: InstallPlan) -> list[str]:
    """플랜의 모든 .deb 캐시 키(결정적 순서)."""
    return [deb_cache_key(d.name, d.version, d.filename) for d in plan.debs]


def plan_cache_hit_ratio(plan: InstallPlan, cache: DebCache) -> float:
    """플랜의 .deb 중 캐시에 이미 있는 비율(0.0~1.0). 빈 플랜은 1.0."""
    keys = plan_cache_keys(plan)
    if not keys:
        return 1.0
    hits = sum(1 for k in keys if cache.has(k))
    return hits / len(keys)
