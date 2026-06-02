# CP-6 진행 SSOT — Chromium-class native 실행 + raw-svc storm 분기 (WS-5 / L5)

> **"CP-6(Chromium 류 글리브 게스트가 ALR 로 어디까지 가는가)의 현 위치와 다음 분기"** 를 한 곳에
> 모은 SSOT. chromium 은 **사용자(찬우) 보류** 상태이므로, 이 문서는 *프로브/계측으로 device-확정된
> 사실*과 *그에 따른 결정 분기*만 정직하게 기록한다(본질해법 **구현** 착수는 보류 해제 + device 게이트
> 통과 후). 설계 근거는 세 ADR 에 있고 이 문서는 그것을 한 흐름으로 잇는다:
>
> - `docs/design/adr-001-syscall-overhead-user-notif.md` — syscall 중재 오버헤드 + USER_NOTIF 평가(원형).
> - `docs/design/adr-002-chromium-cp6-roadmap.md` — raw-svc storm 돌파(측정-우선; svc-rewrite vs USER_NOTIF A/B).
> - `docs/design/adr-003-multiprocess-exec-reentry.md` — 멀티프로세스 exec re-entry(loader 재진입 기각, 상속 채택).
>
> 소유: WS-5(L5). HOST-ONLY — 새 device 측정 없음, 기존 `docs/evidence/` 인용만. 벤치 문서 아님
> (성능 숫자는 `docs/PERFORMANCE.md`/`docs/research/cp3-cpu-overhead-ratio.md`). 잠긴 *범용* 로더
> 기능 SSOT 는 `docs/research/loader-feature-gaps.md`(storm 벽은 거기서 *갭으로 격상하지 않음* —
> CP-6 보류분이므로 여기로 분리).
>
> baseline: 통합 트리 v137. 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2, Android 16,
> 1200×1920@90Hz, untrusted_app).

---

## 0. 한 줄 현 위치

**chromium-headless-shell(Chromium 147)이 ALR 네이티브 로더로 device 에서 `--version` 실행(child
exit=0)됐고, 그 init 경로의 중재 오버헤드 verdict 는 `mediation-negligible`(traps=0, emul=1) 이다.**
즉 *init* 은 라운드트립 storm 이 아니다 — 무거운 `--dump-dom` *render* storm 만 남았고, 그건
멀티스레드-ptrace 데드락에 막혀 아직 device-미측정이다. 데드락이 풀려 render storm 의 N(절대 raw-svc
수)·trap 분포가 device 로 측정돼야 비로소 두 본질해법 **M-R5(svc-rewrite) vs M-R1(USER_NOTIF)** 의
A/B 가 결정 가능하다. exec re-entry(멀티프로세스)는 storm 과 **독립된 별개 벽**으로, ADR-003 이
`--single-process --no-zygote` 기준선 + read-only 프로브로 분리했다.

---

## 1. device-확정 사실 (evidence 인용)

| 항목 | 결과 | evidence | 한 줄 |
|------|------|----------|-------|
| chromium `--version` in-process 실행 | **RUNS** (child exit=0, `Chromium 147.0.7727.137`) | `docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md` | 186MB static-PIE + ~200-file 클로저가 ALR 로더로 PARSE/MAP/EXEC PASS, 비root·public-API |
| M-R2 syscall-mix 계측(init) | **traps=0 emul=1** (nr 99 SIGSYS 1회) `stime_us=917552 utime_us=292542 nonvol_ctxt=379` | `docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md` | supervisor 가 두 기존 trap 사이트에서 nr 히스토그램 + getrusage 집계(신규 ptrace op 0) |
| M-R2 verdict (`bench/syscall_mix.py`) | **mediation-negligible** | 동 evidence | 라운드트립 0 → ALR 중재는 init 병목 아님; 917ms stime 은 chromium 자체 init(lib load·futex/epoll) |
| chromium `--dump-dom`/`--headless` render storm | **WALL** (멀티스레드-ptrace 데드락) | `docs/evidence/2026-06-01-device-SM-X236N-chromium-runs-inprocess.md`(v121), 동 M-R2 evidence | raw-`svc` storm + ptrace SEIZE/EVENT_STOP 튜닝 미완 → storm 분해(N·trap) 미측정 |

> **정직 주의.** `classify_storm` 의 퇴화 케이스: init 에서 `nonvol_ctxt/(traps+emul)=379/1=379` 가
> 처음엔 "roundtrip-dominated" 로 오분류됐다. 라운드트립 1건은 ctxt-switch ~2 개를 만드므로 ratio 가
> `ROUNDTRIP_CTXT_MAX(=4)` 를 넘으면 그 switch 는 게스트 자체 것(중재 아님)이다 — 천장 추가 +
> `mediation-negligible` verdict + 회귀 테스트(이 chromium 데이터)로 고정(`tests/test_syscall_mix.py`).

---

## 2. 결정 흐름 (M-R2 verdict → 다음 분기)

```
chromium --version  ──[device PASS, exit=0]──>  M-R2 계측(init)
                                                     │
                                          verdict = mediation-negligible
                                          (traps=0  ⇒ init 은 storm 아님)
                                                     │
            ┌────────────────────────────────────────┴───────────────────────────────┐
            │ (storm 벽은 init 이 아니라 RENDER 경로에만 있다)                          │
            ▼                                                                          ▼
   chromium --dump-dom  ──[WALL: 멀티스레드-ptrace 데드락]──> render storm N·trap 미측정   (deadlock fix 필요)
            │                                                                          │
            │  ── deadlock fix (PTRACE_SEIZE + EVENT_STOP/LISTEN 튜닝) ──>  M-R2 storm 분해 ──┐
            │                                                                                 │
            ▼                                                                                 ▼
   분기 판정(ADR-002 §2/§4):  trace_hist 의 비-path nr 빈도 + nonvol_ctxt÷(traps+emul) 역산비
            │
   ┌────────┴─────────────────────────────────────────────┐
   │ 라운드트립 지배(ratio≈2)                               │ syscall-weight 지배(ratio≈0)
   ▼                                                        ▼
 본질해법 A/B (device):                              목표 재정의(ADR-002 §5):
   M-R5-svcscan  vs  M-R1-unotif                     storm 은 24ns×N 바닥이 하한 ⇒ best-effort
   (둘 다 read-only 프로브 먼저)                       + 한계 문서화(syscall-light 는 이미 0% PASS)
```

### 분기 A — 본질해법 A/B (라운드트립 지배일 때만 ROI)
- **M-R5-svcscan** (ADR-002 §2 2a): 게스트 text 의 `mov x8,#nr; svc #0` 사이트를 **스캔만**(rewrite 안
  함) → `svc_sites=<n> rewritable=<n>/<n>` 로깅. W^X 게이트 v120 device-PASS(`docs/evidence/2026-06-01-device-SM-X236N-v120-jit-wx-cycle.md`)
  로 비root·in-process·W^X 무위반. **read-only 프로브**라 chromium 보류와 무관히 평가 가능. host 코어 =
  `tests/svc_match.py` + `tests/test_svc_rewrite_match.py`(ARM64 svc/MOVZ/MOVK/BR 비트 인코딩 단위테스트).
- **M-R1-unotif** (ADR-002 §2 2b): `seccomp(SET_MODE_FILTER, NEW_LISTENER)` 별도 필터 + SCM_RIGHTS 로
  listener fd 부모 전달 → `ALR USER_NOTIF PROBE: PASS` 가용성 프로브. TSYNC+NEW_LISTENER EINVAL 충돌은
  `TSYNC_ESRCH` 로 회피(별도 필터). host 결정모델 = `tests/test_user_notif_bpf_logic.py`(action 우선순위
  signed-min: notif/그외→ALLOW 가 PCGATE path→RET_TRACE 를 못 이김 = 두 채널 공존).
- **kill-shot 정정(확정)**: R5 의 "svc-rewrite 는 seccomp 미경유 → 24ns 바닥 제거"는 **거짓** —
  트램폴린 종착 `svc` 도 PCGATE BPF 를 태운다. 24ns 바닥은 svc-rewrite 에도 잔존(정성 우위만 유효).
  storm cost 정량모델 = `bench/storm_cost_model.py`(M-R2 의 실측 N 이 들어오면 즉시 usable 판정).

### 분기 B — 목표 재정의 (syscall-weight 지배일 때)
ADR-002 §5: **"syscall 0%/제로 오버헤드"는 chromium storm 에 원리적으로 불가**(seccomp 켜는 한 24ns
바닥). syscall-light/path-heavy 게스트는 **이미 0% PASS**(CP-3 apples-to-apples: compute +0.00%,
`docs/evidence/2026-06-01-cp3-apples-to-apples-gtk3-svg-perm.md`), storm 게스트는 best-effort 가속 +
한계 문서화로 재정의한다.

---

## 3. exec re-entry (멀티프로세스 — storm 과 독립된 별개 벽)

ADR-003: chromium 의 두 자식 클래스를 분리하면 — **(A) zygote-fork 자식(renderer 다수)은 execve 를 안
거치므로 이미 매개된 주소공간 + 상속 seccomp + SEIZE-trace 로 _자동_ 매개**, **(B) fresh-execve 자식
(zygote/gpu)만 진짜 벽**이고 "loader 재진입"이 아니라 seccomp-across-execve(커널 확정) +
`PTRACE_O_TRACEEXEC` 자동 재포착으로 푼다. 신규 작업 = (B-1) execve **x0** path mediation(현 코드는
*at-style x1 만 읽고 exec 는 `is_exec` 로 건너뜀, `app/src/main/cpp/runtime_report.cpp` L2089-2092).

| 자식 클래스 | 띄우는 법 | ALR 중재 | 상태 |
|---|---|---|---|
| renderer(다수) | zygote가 **fork**(no exec) | 이미 매개(주소공간 복제 + 상속 seccomp/SEIZE) | 자동(ADR-003 §2-A) |
| zygote/gpu | browser가 **fresh execve** | (B) exec 벽 — TRACEEXEC + x0-rewrite | IN-PROGRESS(host 프로토타입) |

**급소(미검증).** execve **envp 전파**(ADR-003 §4 가정-1) — chromium 런처가 `LD_PRELOAD`/`ALR_ROOTFS`
를 자식 envp 로 넘기는지에 interposer 재주입이 인질로 잡힌다(WebSearch 상 chromium 이 거를 공산).
+ `/proc/self/exe` exec(가정-3) + AT_SECURE(가정-2) — 전부 device-only.

**device 프로브(read-only, 보류 무관)**: M-R4-fork(`--single-process --no-zygote` 기준선) /
M-R4-execmap(clone:exec 분류 + x0 path 분포) / M-R4-envprop(`ALR-ENVPROP ld_preload=<0|1>` 로깅).
(B-1) x0-rewrite **실구현** 착수는 M-R4-execmap 이 "exec 자식이 rootfs-내 절대경로 사용(`/proc/self/exe`
아님)"을 device 로 보인 후 + 사용자 재개 신호로 게이트(ADR-003 §5).

**host 프로토타입(WS-5)**: `tests/exec_map_model.py`(clone:exec 분류 파서; `tests/test_exec_map.py`) + `tests/test_execve_pathrw.py`
(execve **x0** vs *at-style **x1** 분기 결정모델 — `is_exec`면 regs[0] 읽고 argv/envp 불변, `/proc/*`
rewrite 제외, rootfs-내 idempotency guard). darwin 호스트는 실커널 seccomp-across-execve/SEIZE-EVENT_EXEC
거동 불가 → 분류/분기 로직 회귀만, 상속 효과는 device-only.

---

## 4. ADR 상호참조 한눈에

| ADR | 무엇 | 이 문서와의 관계 |
|-----|------|-----------------|
| **ADR-001** | syscall 중재 오버헤드 측정 + USER_NOTIF 후보(원형) | §2 의 "24ns 바닥은 seccomp 켜는 한 안 사라짐"·USER_NOTIF 후보의 출발점 |
| **ADR-002** | raw-svc storm 돌파(측정-우선; M-R2/M-R5/M-R1/M-R3) | §1 M-R2 verdict·§2 분기(svc-rewrite vs USER_NOTIF A/B)의 근거 |
| **ADR-003** | 멀티프로세스 exec re-entry(상속 채택, loader 재진입 기각) | §3 의 자식 클래스 분리·envp 전파 급소·M-R4 프로브 |

**정직 섹션(미확정 — device 측정 전).** ① render storm 절대 N(=5M~50M?) 미측정 — 모든 정량모델의
핵심 입력. ② stime 지배 요인(라운드트립 vs chromium 자체 24ns×N) — M-R2 storm 분해가 답하나 데드락
선결. ③ NEW_LISTENER 가 untrusted_app SELinux 통과하는지(M-R1 device-only). ④ svc-rewrite 가
186MB+수백 .so 안정 치환(M-R5 device-only). ⑤ execve envp 전파(ADR-003 가정-1, exec re-entry 급소).

*갱신 규칙:* device evidence 추가 시에만 RUNS/PASS 승급(evidence 파일명 명기). host-only 진전
(프로토타입/계측 설계)만으로는 storm/exec 벽을 "풀림"으로 올리지 않는다. CP-6 는 사용자 보류이므로
본질해법/x0-rewrite **구현** 착수는 보류 해제 + 해당 device 게이트 PASS 후. 잠긴 *범용* 로더 기능은
`docs/research/loader-feature-gaps.md`, GUI 렌더 셋은 `docs/research/gui-universality-status.md`.
