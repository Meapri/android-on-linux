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
| `bench/display_verify.py` | CP-1 device-exact 디스플레이 검증 (1200×1920@90Hz) | **device-VERIFIED** — 통합 빌드의 `display: 1920x1200 @ 90000mHz density=213` 마커로 해상도+90Hz refresh 둘 다 검증 (`docs/evidence/2026-06-01-ws5-cp1-display-verified.md`); 더 이상 host-only/unverified 아님 |
| `bench/report_parse.py` 일반화 | 제네릭 `ALR X: status` 마커 맵 + `wl_output` 파싱 (future-proof) | host-done, 테스트됨 |
| `docs/research/ws5-premerge-gate.md` + `scripts/ws5-premerge-check.sh` | §5 게이트 기준 (통합 세션 pre-merge 게이트, CP별) | done |
| `bench/microbench/` (`microbench.c` + README) | M1 same-binary native-vs-ALR 측정 타깃 (compute/syscall 모드) | source-done (cross-compile/stage는 WS-4) |
| `bench/__main__.py` `verify` 서브커맨드 | 리포트 1개로 gate + CP-1 display + 마커 요약 통합 | host-done, 테스트됨 |
| CP-3/M1 **device 실측 (ALR 절대값)** | M1 | **PARTIAL** — WS-1 M2가 device 캡처(APK v127); WS-5가 정량화 (`docs/evidence/2026-06-01-ws5-cpu-overhead-quantified.md`): 일반 CLI exec_ms ~18-20ms, path-xlate 4334.7 ns/op, **traps=0 device-verified**. native/PRoot baseline는 **DEVICE-REQ (outstanding, merge 커밋에 filed)** → % 오버헤드 비율 미산출 |
| CP-1 **display** device 실측 | (L3) | **DONE (device-VERIFIED)** — 통합(merge) 빌드 리포트의 `display: 1920x1200 @ 90000mHz` 마커로 WS-5가 해상도+90Hz를 검증. host-only 모델이 아니라 실제 device 마커 소비로 닫힘 |
| CP-4 **dmabuf/AHB present** device 실측 | M2 (L3) | **DONE (device-VERIFIED)** — WS-5가 drain#5 소비(`bench`/present_verify): `ALR AHB ZEROCOPY IMPORT: PASS` + `gtkdemo-result: rendered=true frames=12→13`(ws-3 WaylandPresenter, 단일 게이트 후 회귀 0). AHB→external-OES zero-copy present는 device-verified. evidence: `docs/evidence/2026-06-01-drain5-cp4-dmabuf-present-single-gate.md`. 남은 nuance: guest-side dmabuf 프로토콜 광고는 WS-3 M2 잔여 |
| CP-2 **infra** device 실측 | M2 | **DONE (device-VERIFIED, infra only)** — loader ring attach + EGL library dlopen(libpthread 수정 후) + Mali self-test `software renderer=false` 전부 device-verified. evidence: `docs/evidence/2026-06-01-cp2-glmark2-egl-dlopen-resolved.md`. **Score는 아래 PENDING** |
| CP-3 native/PRoot **baseline** | M1/M3 | **DEVICE-REQ (outstanding)** — ALR 절대값은 device-verified(traps=0, exec_ms ~18-20ms)이나 native/PRoot baseline 미측정 → % 오버헤드 비율 미산출. 현재 유일하게 남은 DEVICE-REQ. merge 커밋에 DEVICE-REQ로 filed; 통합 세션 device 리스 대기 |
| CP-2 **glmark2 Score** device 실측 | M2 | **PENDING (WS-2)** — CP-2 infra는 device-verified(위)이나 Score는 `eglChooseConfig() didn't return any configs`로 막힘 = WS-2 shim eglChooseConfig (0 configs). WS-2 M3가 EGL config/surface/GLES-via-ring 구현 후 통합 빌드에서 device 런 |
| 나머지 **device 실측** | M2/M3 | pending (통합 빌드 + 디바이스 점유 조율) |

## 운영 모드 (플랜 §9 / §10 적용 중)

플랜(`docs/research/orchestration-5session-plan.md`) **§9 Device Lease Protocol**과 **§10 reassignment**이
이제 발효 중이다. WS-5의 역할은 **merge된 통합 빌드 evidence를 소비/검증**하는 것이며, **자체 APK install은 하지 않는다**
(공유 디바이스 `R5KL20B6S3X` clobber 방지). device가 필요한 측정(CP-2 glmark2, CP-3 native/PRoot baseline)은
**DEVICE-REQ**로 merge 커밋에 filed되어 있고, §9 리스 프로토콜에 따라 통합 세션이 device를 점유 조율한 뒤 캡처한 리포트를
WS-5가 파서/게이트로 닫는다. CP-1 display는 이 경로로 이미 **device-VERIFIED**로 닫혔다.

baseline: `ws-5`는 현재 origin/main **`c009063`** (통합 트리 v127; APK `0.4.127-cp1-gui-baseline-v127`) 기준 (clean). host 테스트: `cd /Users/naen/Documents/alr-ws5 && uvx pytest tests/ -q` (pytest 미설치 → `uvx`) — 현재 host 테스트 ~**386+** 수집. 단일 진입점: `scripts/run-host-tests.sh`; pre-merge 게이트: `scripts/ws5-premerge-check.sh`. CLI: `python -m bench {gate,verify,overhead,gpu,index}`.
