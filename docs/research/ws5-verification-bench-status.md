# WS-5 — 검증 & 벤치 & CI & 문서 (세션 5) 상태/소유권 선언

> 이 문서는 5-세션 병렬 오케스트레이션(`docs/research/orchestration-5session-plan.md`)에서
> **세션 5 = WS-5 (L5 검증/벤치/CI/문서)** 담당이 자기 작업 경계를 다른 세션과 겹치지 않게
> 명시하기 위한 durable 선언이다. 다른 세션은 이 파일을 읽기만 한다.

작성 시점 baseline: HEAD `0df74dc` (`0.4.124-seize-mt-supervisor-v124`). 디바이스 `R5KL20B6S3X` (SM-X236N).

## 격리 (Isolation)

- worktree: `/Users/naen/Documents/alr-ws5`, branch **`ws-5`** (플랜 §6.1 명명 규약).
- 베이스: clean HEAD(`0df74dc`, v124). 통합 트리의 미커밋 v126 작업은 가져오지 않음 (격리 목적).
- 현재 다른 worktree: `main`(통합), `alr-ws2`(`ws-2`), `Android-on-Linux-ws3`(`ws-3`), `.claude/worktrees/ws-4`(`worktree-ws-4`). WS-1은 `stash@{0}`로 parked.

## 소유 (내가 수정하는 것 — §2 L5)

- `tests/**` — 단, **stamp 핀 테스트는 제외**(아래 NON-GOALS). 신규 테스트만 추가.
- `docs/evidence/**` — evidence 수집 + 표준 템플릿.
- `docs/**` — 검증/벤치/호환 관련 신규 문서 (WS-5 네임드 파일만; 타 세션 research 문서는 읽기만).
- **`bench/`** — 신규 bench harness 패키지(플랜이 "bench harness(신규 도구)"로 WS-5에 명시). `tools/**`는 **WS-4 소유**이므로 거기 두지 않고 새 `bench/`에 둔다.

## NON-GOALS (절대 안 건드림 — 다른 세션/통합 소유)

- app native/kt 소스: `runtime_report.cpp`, `MainActivity.kt`, `alr_wayland/**`, `alr_gpu/**`, `libalr_interpose.c`, `build.gradle.kts` (L1/L2/L3 + 통합).
- `tools/**`, `assets/rootfs/**`, `RootfsInstaller.kt` (WS-4).
- **version stamp 및 stamp-핀 테스트** (`test_android_visible_build_stamp.py` 외, 현재 v124→v126 bump 미커밋 상태) — §6.4: 통합 세션만 bump.
- `git add -A` 금지. 항상 명시 경로만 add.

## §5 인터페이스 계약 — WS-5는 소비자(측정 대상)

WS-5는 전 WS 산출물을 **측정**한다. 계약 자체는 제공하지 않지만, 측정을 위해 다음 마커를 소비한다(읽기 전용):

- L1: `alr native loader path-mediation traps=N rewrites=M`, `alr sc seccomp_trace_events=N`, `all: pcgate=1 interpose=1 traps=0 rewrites=0`, `ALR NATIVE LOADER GUEST EXEC: PASS`, `child exit=N signal=M`, `ALR PERF HARNESS: PASS`.
- L2: `alr gpu boundary inproc dispatch ns/op=N`, `socket per-cmd ns/op=N`, `shmem-ring ns/op=N`, glmark2 score(예정).
- L3: wl_output 해상도/주사율 보고 라인(예정).
- 공통: `build: <stamp>`.

이 마커 문자열이 바뀌면 `bench/report_parse.py`의 정규식만 갱신한다(app 소스는 안 건드림).

## 디바이스 사용 정책 (물리 디바이스 1대 공유)

디바이스 `R5KL20B6S3X`는 5세션 공유 하드웨어다. WS-5의 **host-side 툴링/테스트/문서**는 device 없이 빌드·검증한다.
실제 device 벤치 런(APK build+install+logcat)은 **다른 세션의 device-test와 충돌**하므로:
- WS-5는 자기 worktree에서 임의로 APK를 install하지 않는다(공유 디바이스 clobber 방지).
- device 벤치는 **통합 세션이 merge한 빌드**를 대상으로, device 점유를 조율한 뒤 실행한다.
- device 검증은 항상 `am force-stop` 후 cold start (메모리: device-test-force-stop-first).

## 산출물 상태

| 항목 | 마일스톤 | 상태 |
|------|----------|------|
| `bench/report_parse.py` | (공통) 디바이스 리포트 마커 파서 | host-done, 테스트됨 |
| `bench/cpu_overhead.py` | M1 native vs ALR 오버헤드 모델 | host-done, 테스트됨 (device 런 pending) |
| `bench/regression_gate.py` | M3 회귀 매트릭스 게이트 | host-done, 테스트됨 (device 런 pending) |
| `bench/gpu_bench.py` | M2 glmark2 ALR vs Mali-direct 비율 모델 (≥0.70, software=false) | host-done, 테스트됨 (WS-2 glmark2 이후 device 런) |
| `bench/evidence_index.py` | evidence 디렉토리 인덱서/검증 | host-done, 테스트됨 (38 doc 인덱스) |
| `bench/__main__.py` | `python -m bench gate/overhead` CLI | host-done, 테스트됨 + 모듈 실행 스모크 |
| `docs/evidence/_TEMPLATE.md` | M4 evidence 표준 템플릿 | done |
| `docs/evidence/CAPTURE-RUNBOOK.md` | 공유 디바이스 안전 캡처 런북 | done |
| `docs/research/alr-compat-matrix.md` | M4 앱×결과 호환 매트릭스 | seeded (기존 evidence 기반) |
| `.github/workflows/ws5-host-ci.yml` + `scripts/run-host-tests.sh` | M5 host CI 게이트 (device는 수동) | done |
| `bench/display_verify.py` | CP-1 device-exact 디스플레이 검증 (1200×1920@90Hz; refresh는 마커 생기기 전까지 unverified) | host-done, 테스트됨 |
| `bench/report_parse.py` 일반화 | 제네릭 `ALR X: status` 마커 맵 + `wl_output` 파싱 (future-proof) | host-done, 테스트됨 |
| `docs/research/ws5-premerge-gate.md` + `scripts/ws5-premerge-check.sh` | §5 게이트 기준 (통합 세션 pre-merge 게이트, CP별) | done |
| `bench/microbench/` (`microbench.c` + README) | M1 same-binary native-vs-ALR 측정 타깃 (compute/syscall 모드) | source-done (cross-compile/stage는 WS-4) |
| `bench/__main__.py` `verify` 서브커맨드 | 리포트 1개로 gate + CP-1 display + 마커 요약 통합 | host-done, 테스트됨 |
| CP-3/M1 **device 실측 (ALR 절대값)** | M1 | **PARTIAL** — WS-1 M2가 device 캡처(APK v127); WS-5가 정량화 (`docs/evidence/2026-06-01-ws5-cpu-overhead-quantified.md`): 일반 CLI exec_ms ~18-20ms, path-xlate 4334.7 ns/op, **traps=0 device-verified**. native/PRoot baseline는 **PENDING** → % 오버헤드 비율 미산출 |
| 나머지 **device 실측** | M2/M3 | pending (통합 빌드 + 디바이스 점유 조율) |

baseline: `ws-5`는 현재 main **v127** 기준 (clean; APK `0.4.127-cp1-gui-baseline-v127`). host 테스트: `cd /Users/naen/Documents/alr-ws5 && uvx pytest tests/ -q` (pytest 미설치 → `uvx`). 단일 진입점: `scripts/run-host-tests.sh`; pre-merge 게이트: `scripts/ws5-premerge-check.sh`. CLI: `python -m bench {gate,verify,overhead,gpu,index}`.
