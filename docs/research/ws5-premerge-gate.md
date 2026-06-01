# WS-5 — 프리머지 게이트 기준 (Pre-merge Gate Criteria) §5

> 이 문서는 **통합(INTEGRATION) 세션**이 워크스트림 브랜치(`ws-1`..`ws-5`)를 `main`으로 merge
> 하거나 체크포인트(CP-0..CP-6)를 끊기 **전에** 반드시 만족해야 하는 게이트를 정의한다.
> 워크스트림 세션은 자기 worktree에서 host 게이트만 셀프체크한다; device 게이트와
> version-stamp bump는 **통합 세션 전용**이다.
>
> 단일 진입점(워크스트림 셀프체크): `scripts/ws5-premerge-check.sh`.
> bench CLI: `python -m bench {gate,overhead,gpu,index}`.

---

## Host gates (자동화 — 매 merge마다)

머지 후보 트리에서 device 없이 전부 검증 가능. 통과 못 하면 merge 금지.

- **전체 테스트 green:** `cd <worktree> && PATH="$HOME/.local/bin:$PATH" uvx pytest tests/ -q` 가
  **ALL passed** (host에 pytest 미설치 → `uvx` 사용). 1개라도 fail/error면 게이트 FAIL.
- **version-stamp drift 없음:** stamp 핀 사이트(메모리: version-stamp-pin-sites)가 **서로 정합**해야 하고,
  bump은 **통합 세션만** 한다. 워크스트림 브랜치가 stamp/stamp-핀 테스트를 건드렸으면 reject.
  (핀 사이트는 한 번에 같이 움직인다 — 부분 bump = drift = FAIL.)
- **explicit-paths-only staging:** 머지/스테이징은 항상 **명시 경로**로만. `git add -A` 금지
  (다른 세션 미커밋 작업 보호; 플랜 §6.6).

워크스트림 셀프체크: `scripts/ws5-premerge-check.sh` → `WS-5 HOST GATE: PASS`.

---

## Device gates (수동 — device-gated 체크포인트마다)

물리 디바이스 1대 공유 → device 점유를 조율한 뒤, **통합 세션이 merge한 빌드**를 대상으로 실행.

- **force-stop + cold start:** 캡처 전 항상 `am force-stop <pkg>` 후 cold launch
  (warm resume는 onCreate 스킵 → overlay/probe 미실행; 메모리: device-test-force-stop-first).
- **리포트 캡처:** MainActivity summary / logcat 라인을 그대로 저장 (paraphrase 금지).
- **no-regression 게이트:** `python -m bench gate <report>` → **PASS**.
  무회귀 불변식 `all: pcgate=1 interpose=1 traps=0 rewrites=0` + GIMP/CLI 프로브 expected-exit 정합.
  하나라도 깨지면 FAIL (mediation_ok=False 또는 exit 회귀).
- **evidence 파일링:** `docs/evidence/_TEMPLATE.md`를 복사해
  `docs/evidence/YYYY-MM-DD-device-<MODEL>-<slug>.md`로 채워 커밋(REAL 라인만, honest-scope 포함).
- **CP-1 (디스플레이):** display device-exact — `bench.display_verify`로 wl_output 보고값이
  **1200×1920, 90Hz**와 정확히 일치하는지 검증 (해상도·주사율 모두 exact).
- **CP-2 (GPU 네이티브):** `python -m bench gpu --alr-score <S> --mali-score <M> --alr-renderer <R>` →
  ALR/Mali **ratio ≥ 0.70** AND **software=false** (GL_RENDERER가 소프트웨어 렌더러가 아님).
- **CP-3 (CPU 오버헤드):** `python -m bench overhead --native-ns <N> --alr-ns <A>` →
  syscall-light 워크로드 오버헤드 **< 5%**. syscall-storm(chromium류)은 측정·보고만 하고 게이트하지 않음.

---

## Per-checkpoint gate table (CP-0..CP-6)

| CP | 의미 | Host gate | Device gate | Owning WS |
|----|------|-----------|-------------|-----------|
| CP-0 | 인터페이스 합의 (§5 A–E 헤더) | `uvx pytest tests/ -q` green; stamp drift 없음 | — (device 불필요) | WS-1 (제공), 전 WS |
| CP-1 | GUI 기반 + 디스플레이 | host green | force-stop+cold; `bench gate` PASS; `bench.display_verify` → 1200×1920·90Hz exact; evidence | WS-3 (+WS-4) |
| CP-2 | GPU 네이티브 1차 (glmark2) | host green | `bench gate` PASS; `bench gpu` ratio ≥ 0.70 & software=false; evidence | WS-2 (+WS-1 ring, WS-3 present) |
| CP-3 | CPU 오버헤드 정량 | host green | `bench gate` PASS; `bench overhead` syscall-light < 5%; evidence | WS-1 (+WS-5) |
| CP-4 | zero-copy present (dmabuf) | host green | `bench gate` PASS; dmabuf 경로 + CPU readback 0; evidence | WS-3 (+WS-2) |
| CP-5 | 범용성 / toolkit | host green | `bench gate` PASS; toolkit 매트릭스 + apt 앱 실행; compat-matrix 갱신; evidence | WS-4 |
| CP-6 | 제로 오버헤드 수렴 | host green | `bench gate` PASS; chromium-class usable 또는 USER_NOTIF ADR; evidence | WS-1 |

모든 device-gated CP는 **host green + force-stop+cold + `bench gate` PASS + evidence 파일링**을
공통 전제로 깐다. 위 표의 device gate 칼럼은 그 위에 얹는 CP별 추가 조건이다.

---

## Merge discipline

- **worktree-per-session:** 세션당 git worktree(브랜치 `ws-1`..`ws-5`); 자기 레이어 파일만 수정,
  레이어 경계는 §5 인터페이스 계약으로만 (플랜 §6.1–6.2).
- **integration merges:** 워크스트림은 직접 `main`에 push하지 않는다. **통합 세션이** 인터페이스 계약
  기준 검수 → host 게이트 → (해당 시) device 게이트 → merge (플랜 §6.5).
- **stamp bump integration-only:** version-stamp는 **통합 세션만** bump하고, 핀 사이트를 한 번에
  같이 움직인다. 워크스트림 브랜치의 stamp 변경은 머지에서 떨군다 (플랜 §6.4).
- **explicit paths only:** 스테이징은 항상 명시 경로. `git add -A` 금지 (§6.6).
