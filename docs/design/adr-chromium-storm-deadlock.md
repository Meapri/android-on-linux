> 격리 브랜치 research/chromium-storm 연구 산출(보고/연구 담당). main·ws-N 미변경. 본문이 칭하는 'ADR-004' 번호는 product-ux 브랜치의 adr-004와 충돌하므로 통합 세션이 재번호. WS-1(runtime_report/interpose)·WS-5(tests/bench)·통합 세션 리뷰 대상.

# ADR-004 — Chromium `--dump-dom` 멀티스레드 "데드락" 재진단 + storm 돌파 로드맵 (오진 정정, measure-first)

- 상태: **Proposed (device-pending)** — 5축 연구(R1 재진단 / R2 우회측정 / R3 svc-rewrite / R4 USER_NOTIF / R5 적대재정의) + 각 축 자가-적대검증 종합. 코드 변경 0, device 게이트 미통과.
- 워크스트림: **WS-1**(`app/src/main/cpp/runtime_report.cpp`·`alr_interpose/libalr_interpose.c` — supervisor/seccomp/계측) 주관, **WS-5**(`tests/`·`bench/` — 파서·결정모델·회귀) 협의.
- 작성: 2026-06-02. 격리 브랜치 `research/chromium-storm`(base v141). `main`·`ws-N` 미변경.
- 선행/도전: `docs/design/adr-002-chromium-cp6-roadmap.md`(이 문서가 §1 분기 **앞에 새 1순위 진단을 끼움**), `docs/design/adr-003-multiprocess-exec-reentry.md`(직교 — exec 벽; 본 ADR은 thread 벽). SSOT: `docs/research/cp6-status.md`.
- HARD CONSTRAINTS(불변): 비root(untrusted_app, no CAP_SYS_ADMIN), public Android API only, SELinux 우회 금지, W^X-safe, in-process(PRoot fallback-only), **device evidence 없이 "풀림" 주장 금지**, version stamp 불변.

---

## 1. 한 줄 결론

**"멀티스레드-ptrace 데드락"은 best-가설로 **오진**이다 — 코드 5대 사실(아래 §2)이 (a)clone-못함·(b)clone-trap-block·(c)futex-deadlock 세 데드락 후보를 전부 기각하고, 가장 설명력 높은 단일 원인은 "deadlock이 아니라 too-short measurement window on a genuinely heavy single-thread bring-up" 이다 — 즉 dynamic 게스트의 `alarm(25s)`(`runtime_report.cpp` L1938)가 chromium의 ld.so 동적링크 + V8 부트스트랩 + single-init render가 _첫 워커를 clone하기도 전에_ 만료해 `Threads=1` 스냅샷만 잡힌 것이다.** 푸는 경로는 supervisor를 또 만지는 게 아니라(그 길은 v122–v124에서 이미 막다른 골목) **read-only 측정창 분리 실험 한 줄**: dynamic alarm을 25s→`ALR_GUEST_ALARM_S`로 파라미터화(예 180s)하고 supervisor에 동일 wall-deadline + `EVENT_CLONE` 시계열(tid·n·t_ms) + 멈춘 tid의 마지막 nr을 로깅하면, "게스트가 첫 clone에 도달하는가/몇 초에/멈춘다면 어느 nr에서"가 device 한 방에 답해져 (a)/(b)/(c)/(window-too-short) 4분류가 결정된다. **단 이 결론은 v121 evidence(state R, utime 증가=실진행) 기반이며, v124 SEIZE 전환이 _새_ group-stop 데드락을 유발했을 2차 가설은 device 전에 배제 불가** → verdict = **measure-first**(5축 만장일치).

---

## 2. 핵심 미스터리 해소: "SEIZE fix가 있는데 왜 여전히 Threads=1 고착인가"

task가 1차 과제로 지목한 모순("L2754–2828의 `THE MULTI-THREAD FIX`가 이미 group-stop 데드락을 해결했다는데 `--dump-dom`은 여전히 Threads=1")은 **모순이 아니라 무교차(non-intersection)** 로 해소된다. 5축이 코드로 합의:

> **SEIZE/EVENT_STOP fix는 정상이며 증상과 무관하다.** L2754의 `PTRACE_EVENT_STOP` 분기는 `known_tids.insert(w).second`(L2783)로 new-clone(→CONT) vs group-stop(`GETSIGINFO==EINVAL`→`LISTEN`, L2798)을 정확히 가른다. **그러나 이 분기는 _워커가 clone된 후에만_ 발화한다.** `Threads=1`이면 코드는 이 분기에 한 번도 진입하지 않는다. fix가 다룰 group-stop 상황에 게스트가 _애초에 도달하지 못한_ 것이므로, "fix가 작동함"과 "증상이 남음"은 같은 평면에서 충돌하지 않는다.

### 데드락 3후보 기각 (코드 L번호 근거)

| 후보 | 기각 근거 (코드) | 정량/evidence |
|---|---|---|
| **(a) seccomp-floor-glacial** (단일 init이 라운드트립 storm으로 느림) | PCGATE BPF(`libalr_interpose.c` L471–510)는 9 path nr `{34,35,48,56,78,79,291,437,439}`만 RET_TRACE, clone/clone3/futex/mmap 등 **비-path 전부 RET_ALLOW** → raw-svc 비-path는 0 라운드트립 | `bench/storm_cost_model.py`: 50M syscall×24ns=1.2s. utime가 25s 내내 기어가는 stime~70× 지배를 24ns 바닥은 설명 못함. M-R2 device traps=0(init)이 확증 |
| **(b) clone-trap-block** (supervisor가 clone을 못 처리) | clone(220)/clone3(435)은 비-path → RET_ALLOW. 성공 시 `PTRACE_O_TRACECLONE`로 `PTRACE_EVENT_CLONE`(L2828 `++guest_threads`) 정지가 생기나 처리는 L2822–2832 generic resume(즉시 CONT)로 막힘 없음. clone _진입_은 EVENT_SECCOMP를 안 탐(9 path nr만) | "supervisor가 clone을 못 처리"가 아니라 "게스트가 아직 clone을 호출 안 함" — `guest_threads=1`의 정확한 의미 |
| **(c) futex-deadlock** (형제가 未생성 워커의 futex 대기) | futex(98)도 비-path RET_ALLOW → SIGSYS-emulate 안 됨(emul_hist=99:1 set_robust_list 1회뿐). single-init이 아직 futex로 워커를 기다리는 단계에 도달조차 안 했을 수 있음 | (c)는 가능하나 (window) 가설의 _하위 사례_; device 프로브로 last_nr=98 멈춤 여부로 직접 판별 |

### 남는 best-가설 = "window-too-short on heavy single-init"

`alarm` 비대칭이 결정적이다: 자식은 L1794에서 전 시그널 SIG_DFL 리셋 후 L1938 `alarm(dynamic?25:5)` → chromium=dynamic(PT_INTERP=게스트 ld.so)이므로 **25s**, `SIGALRM=SIG_DFL`=자식 종료. **부모(supervisor)에는 alarm/타임아웃이 전무**(L2138 `waitpid(-1,__WALL)` 무한). 25s에 자식이 SIGALRM로 죽으면 그 시점 스냅샷이 곧 "Threads=1". utime 1→5 기어감 = 단일 메인스레드가 동적링크/relocation/V8 snapshot deserialize/`about:blank` DOM 구성을 CPU로 _진짜 진행 중_(R state)이라는 직접 증거다. 증상 = single-init이 25s 안에 첫 워커 clone에 **도달하지 못한 것**(=measurement-window 부족 + 186MB·~200 .so 링크가 무겁다는 두 요인의 합), deadlock 아님.

---

## 3. 데드락 fix 우선순위 (R1 진단 기반)

**대전제: supervisor를 또 tweak하지 말 것.** v122(라운드트립 fast-path)/v123(group-stop signal suppress)/v124(SEIZE+EVENT_STOP/LISTEN)는 전부 GIMP/`--version` 무회귀지만 `--dump-dom` 못 뚫음. "같은 supervisor tweak 재시도"는 막다른 골목으로 **금지**. 대신 **측정으로 진단을 먼저 닫는다**.

### D1 (1순위, 즉시·read-only·measure-first의 핵심) — 측정창 분리 실험
- **가설 판별이 목적**: dynamic alarm 25s를 `ALR_GUEST_ALARM_S` env로 파라미터화(기본 25 유지, 실험 시 180/0)하고 supervisor에 동일 wall-deadline + 3계측 추가.
- **3계측(전부 supervisor-내부, hot-loop 오염 0, 신규 ptrace op 0)**:
  1. **clone 시계열**: 매 `PTRACE_EVENT_CLONE`(L2828)에서 `alr clone tid=<w> n=<guest_threads> t_ms=<steady_clock since fork>` — 첫 워커 clone 도달 여부·몇 ms에·몇 개까지 오르다 멈추나.
  2. **alarm 연장 1회 한정 + stuck dump**: alarm 만료 직전 `alr stuck n_threads=<n> last_nr_per_tid=<tid:nr...>`.
  3. **tid별 마지막 nr**: 각 tid의 마지막 EVENT_SECCOMP nr와 마지막 SIGSYS nr.
- **판정 분기**: clone에 **끝내 도달 못함**=(a/c 계열, 또는 single-init-too-slow); **도달함**=`window-too-short` 확정 → CP-6 storm 분기 자체가 절대시간 문제로 환원. 멈춘 tid의 last_nr=futex(98)면 (c), mmap/mprotect(222/226) 폭주면 (a-변형/SIGSYS-emul storm), clone 미발화면 single-init-slow.
- **파일**: `runtime_report.cpp` L1938 alarm + L2828 EVENT_CLONE + L2138 waitpid 루프(WS-1). 파서/결정모델 = 신규 `bench/deadlock_triage.py`(WS-5).

### D2 (2순위, read-only A/B) — clone-도달 분해 프로브
- chromium을 `--single-process --no-zygote` vs default로 띄워 Threads 증가 유무 대조. `--single-process`에서 증가가 _사라지면_ 멀티스레드 자체가 데드락이 아니라 모드 차이임을 보임. default에서 EVENT_CLONE이 0→N 오르다 특정 tid에서 멈추면 그 tid의 마지막 nr이 (c)의 직접 증거.

### D3 (3순위, read-only) — stime 발생원 nr 시계열 분해
- 자식이 SIGUSR1에 히스토그램 dump하도록 해 5s/15s/25s 3구간 스냅샷으로 stime이 path-trap(우리 중재)인지 mmap/futex(게스트 자체)인지 시계열 분리.

> **2차 가설을 죽이지 말 것(정직).** R1·R5 자가-적대검증이 공통 적시: v124 SEIZE 전환 _후_ 실제로 state `t` park가 관측됐을 가능성(task의 "state t에 park" 서술이 최신 device 관측일 수 있음)을 배제 못한다. 그렇다면 SEIZE-induced-new-deadlock(첫 clone 직후 new tid의 group-stop을 `known_tids` 분기가 race로 빗나가 LISTEN-park)이 살아난다. **D1이 이를 단번에 가른다**: clone에 도달하는데 그 직후 멈추면 SEIZE-race, clone에 끝내 도달 못하면 window-too-short.

---

## 4. ★ svc-rewrite가 "데드락 + storm 동시 해결"이라는 가설의 평가 (R3 × R1)

**판정: 거짓. svc-rewrite는 데드락과 무관하고(R1), 비-path 지배 render storm엔 +34ns로 오히려 손해 가능(R3) — 실효는 미측정 raw-svc-PATH trap 분율에만 걸려 measure-first가 강제된다.**

### 4-1. 데드락 축 (R3 × R1 교차검증으로 falsified)
clone(220)/clone3(435)/futex(98)는 모두 비-path nr → PCGATE BPF가 EVENT_SECCOMP trap을 **아예 안 만든다**. 워커 생성은 supervisor seccomp 라운드트립을 경유하지 않는다. `Threads=1` 증상은 L2754–2832의 ptrace group-stop/EVENT_STOP/EVENT_CLONE 처리에 있고(또는 §2의 window), 이는 RET_TRACE↔RET_ALLOW와 **직교**한다. **svc-rewrite는 RET_TRACE를 트램폴린 br로 치환하는 것일 뿐 — 데드락 평면을 건드리지 못한다.**

### 4-2. storm 축 (ROI 조건부·음수 가능)
- `bench/storm_cost_model.py` 50M storm: 비-path 지배(trap≈0)면 current 1.2s vs svc_rewrite 2.9s — svc_hook 34ns가 _이미 ALLOW된 svc에 순증_이라 **손해**.
- svc-rewrite 이득은 _비-트램폴린 PC의 raw-inline-svc PATH syscall_(openat/statx 등)에만 발생하는데, 인터포저가 이미 9 path를 트램폴린 경유시키므로(`libalr_interpose.c` L66–72) 그 잔여 모집단은 render 정상상태에서 작다(파일은 startup에 열림, per-frame 아님). 그 분율 = M-R2 `trace_hist[path-nr@raw-svc-PC]`로만 확정 → **데드락-차단으로 미측정**.
- **kill-shot 정정(확정 계승)**: R5의 "svc-rewrite는 seccomp 미경유 → 24ns 바닥 제거"는 **거짓**. 트램폴린 종착 svc도 PCGATE BPF를 태운다(주석 L63). 24ns 바닥은 svc-rewrite에도 잔존, 정성 우위만 유효.

### 4-3. 실제 rewrite의 진짜 블로커 (R3 확정)
단일 4B 정렬 svc→br store는 ARMv8(B2.2.1) 원자적·stop-the-world 불필요(IC IVAU+DSB ISH+ISB만). 진짜 블로커는 **branch-target reachability**: `tests/svc_match.py` `find_svc_sites`의 `preceded_by_x8_load` 규칙은 necessary-but-insufficient — 분기로 진입한 svc(V8 JIT/PLT 간접분기)는 x8 미정의로 br하면 garbage. 정적 스캔은 간접 분기타깃 열거 불가 → 풀 디스어셈+CFG 필요.

**결론: svc-rewrite는 "데드락 + storm 동시 해결" kill-shot이 아니다.** 데드락은 §3(D1)으로 선결하고, svc-rewrite ROI는 그 후 M-R2 path-raw-svc 분율이 유의미할 때만 2차로 측정. **순서를 "storm 가속"에서 분리해 "데드락 선결 → ROI 측정"으로 재배치.**

---

## 5. 다음 device 단계 (1~3개) + DEVICE-REQ 마커 + 파일 소유

### 단계 1 (1순위·필수 — 데드락 vs throttle 격리) — WS-1: `runtime_report.cpp`, WS-5: `bench/deadlock_triage.py`
> `DEVICE-REQ: ALR-R1-deadlock-rediagnose — SM-X236N (am force-stop first), chromium-headless-shell --no-sandbox --headless --dump-dom about:blank ×1, read-only only. (1) 매 PTRACE_EVENT_CLONE에서 'alr clone tid=<w> n=<guest_threads> t_ms=<steady_clock since fork>'. (2) dynamic alarm 25s→ALR_GUEST_ALARM_S(180) 일시 + supervisor 동일 wall-deadline + 'alr stuck n_threads=<n> last_nr_per_tid=<tid:nr...>'. (3) tid별 마지막 EVENT_SECCOMP nr + 마지막 SIGSYS nr. GATE = 세 라인 present AND child 무크래시(exit OR SIGALRM at 180s). 외부 strace 금지(supervisor-내부 집계만). 판정: clone 도달=window-too-short 확정; 미도달=single-init-slow; 멈춘 last_nr=98⇒(c)futex, 222/226⇒(a-변형/SIGSYS-emul storm).`

### 단계 2 (2순위·read-only A/B — 모드 대조) — WS-1: `runtime_report.cpp`
> `DEVICE-REQ: ALR-R1-singleproc-ab — SM-X236N (am force-stop first), chromium-headless-shell --no-sandbox --headless --dump-dom about:blank ×1 두 번: (A) --single-process --no-zygote, (B) default. GATE = 두 실행 모두 'alr clone' 시계열 라인 + Threads 스냅샷 캡처. 판정: (A)에서 Threads 증가 소멸=멀티스레드 자체가 데드락 아님(모드 차이); (B)에서 EVENT_CLONE 0→N 후 특정 tid 멈춤=그 tid last_nr이 (c) 직접 증거. child exit은 비-gate.`

### 단계 3 (3순위·조건부 — D1이 window-too-short를 보인 후에만 storm 분해) — WS-1: `runtime_report.cpp`, `libalr_interpose.c`; WS-5: `bench/syscall_mix.py`
> `DEVICE-REQ: ALR-M-R2-storm-decompose — SM-X236N (am force-stop first), ALR_GUEST_ALARM_S 충분 상향 후 --dump-dom render가 첫 워커 clone에 도달한 실행에서 'alr sc trace_hist nr:count' + 'alr sc emul_hist nr:count' + 'alr sc stime_us/utime_us/nonvol_ctxt/traps/emul' 캡처. GATE = 라인 present AND emul backstop 이미 1<<20(L2889). 판정: trace_hist의 path-nr@raw-svc-PC 분율=svc-rewrite 실효 상한; nonvol_ctxt÷(traps+emul)으로 라운드트립 vs syscall-weight 분기(ADR-002 §2).`

(주의: 단계 3의 본질해법 A/B(M-R5 svc-rewrite vs M-R1 USER_NOTIF) **구현 착수**는 D1이 "데드락 아님=storm이 실재"를 보인 후 + chromium 보류 해제 신호로 게이트. R4 결론: USER_NOTIF는 path를 TOCTOU로 못 옮기므로 멀티-tracee waitpid 루프·EVENT 처리가 그대로 남아 **데드락을 직접 회피 못 함**, 간접 완화는 흡수율 의존이며 현 BPF상 비-path가 이미 ALLOW라 흡수할 trap이 storm에서도 적을 공산.)

---

## 6. host 프로토타입 (device 불요, WS-5; darwin은 분류/분기 로직 회귀만 — 실커널 seccomp/SEIZE/arm64 self-rewrite는 device-only)

- **(P1, D1용) `bench/deadlock_triage.py`(신규)**: `alr clone tid n t_ms` + `alr stuck n_threads last_nr_per_tid` 라인 파싱 → `{clone_reached: bool, first_clone_ms, max_threads, stuck_tid_last_nr}` 정규화 + **(a)single-init-slow / (b)clone-trap / (c)futex-wait / (d)SIGSYS-emul-storm을 nr 패턴으로 분류하는 순수함수** + pytest. 기존 `tests/test_syscall_mix.py`·`tests/test_storm_cost_model.py` 패턴 재사용.
- **(P2, storm budget용) `bench/storm_cost_model.py` 확장**: 이미 있는 `estimate_storm_seconds`(L222)로 'window 25s에 N=얼마까지 소화'를 역산하는 헬퍼 + alarm 연장 필요량 host 추정. 'init traps≈0이면 세 전략(current/user_notif/svc_rewrite) 모두 24ns×N로 수렴'을 산술로 보여 **본질해법 A/B는 init/window 벽에 ROI 0**을 host-증명.
- **(P3, SEIZE-race 반증용) SEIZE 분기 결정모델 단위테스트(신규)**: `known_tids.insert(w).second`(L2783) 분기 + `GETSIGINFO` EINVAL/ESRCH/성공 3-케이스를 순수 상태기계로 떼어 'new-tid initial-stop을 LISTEN-park하는 race가 가능한가'를 0-mismatch로 검증(§3 2차 가설을 host에서 반증/입증).
- **(P4, svc-rewrite 한계용) `tests/svc_match.py` 확장**: 멀티-site 일괄 br 치환 `apply_svc_rewrites(code, sites)->patched_bytes` + idempotency(이미 br인 site 재치환 금지) + **branch-target 안전성 모델**(B/BL/CBZ/TBZ 디코드해 svc word가 분기 타깃인지 보수적 검출, 간접 br=unknown 표식 → `rewritable`을 `preceded_by_x8_load AND not_branch_target`로 강화) + 적대 픽스처(분기로 진입하는 svc) 회귀. 이미 61 host test PASS(`test_svc_rewrite_match.py`).
- **(P5, R4 정리용) `tests/test_user_notif_bpf_logic.py` 확장**: 'ptrace로 남는 nr 집합 = path9 ∪ {clone,fork,vfork,execve,execveat}(EVENT 생성 nr)'을 enumerate하고 'USER_NOTIF 설치 전후 이 집합 불변'을 0-mismatch로 assert(="USER_NOTIF가 waitpid 데드락 표면을 못 줄임"의 host-검증).

---

## 7. 정직 섹션

**device 측정 전엔 미확정인 것:**
1. **`Threads=1`이 정말 window-too-short인가 vs SEIZE-induced-new-deadlock인가** (§3 1차/2차 가설). D1 alarm-연장 실험이 (clone-도달=window / clone-직후-멈춤=SEIZE-race / 미도달=single-init-slow)로 단번에 가른다. **D1 없이는 미확정 = measure-first가 정직.**
2. **chromium `--version`은 같은 동적링크를 0초대에 끝내므로(traps=0, exit=0), `--dump-dom`의 추가분(V8+render)이 25s를 정말 넘는지는 가정** — device 미측정. 'init이 무겁다'는 정성 주장.
3. **stime~70×의 지배 요인** — ALR madvise/mprotect 중재 vs chromium 자체(mmap/futex/JIT W^X 사이클). M-R2 storm 분해가 답하나 D1 선결.
4. **utime 1→5의 단위** — 초가 아니라 jiffies(50ms)면 25s 중 24.9s를 D(uninterruptible sleep=futex_wait)로 보냈을 수 있고, 그건 '진행중 슬로다운'이 아니라 '한 futex에 영구 블록'(=부정한 데드락의 변종). D1의 last_nr_per_tid가 이를 직접 가른다.
5. **render storm 절대 N(=5M~50M?)** — 모든 정량모델의 핵심 입력. 데드락/window 선결 후 M-R2로만.

**"영영 안 될" 리스크와 그때의 목표 재정의:**
- ADR-001 §1-B의 못 — **seccomp를 켜는 한 24ns/syscall 디스패치 바닥은 어떤 모델로도 안 사라진다** — 은 확정 사실(트램폴린 종착 svc도 BPF 태움). svc-rewrite/USER_NOTIF 둘 다 라운드트립만 줄일 뿐 24ns×N 바닥은 못 넘는다.
- **영영 안 될 시나리오**: D1이 (a) 무제한 alarm에도 `Threads==1`·utime 정체·State=D 지속 = 진짜 single-thread 블록(EVENT_STOP 분류 버그 or chromium self-futex)을 보이고, (b) 그게 SEIZE-race도 아니면 — multi-thread chromium render는 in-process ALR로 막힘. 또는 D1이 window-too-short를 확증해도 N이 너무 커서(>50M) 24ns 바닥만으로 분 단위면 절대시간 usable 불가.
- **그때의 목표 재정의(ADR-002 §5·ADR-003 §7 톤 계승)**: CP-6를 "chromium 멀티스레드 render storm usable"에서 **"syscall-light/path-heavy 게스트는 0% 달성(이미 PASS, CP-3 apples-to-apples), single-init 게스트(`--version`)는 device-PASS 상한으로 인정, full-DOM multi-thread render는 `--single-process --no-zygote --renderer-process-limit=1` + render 예산 상향으로 best-effort + 한계 문서화(deadlock-아님이면 절대시간, deadlock이면 thread-bring-up 벽)"** 로 재정의.

**constraint_violations 없음(확정)**: 본 ADR의 모든 제안(D1~D3 계측, alarm 파라미터화, host 결정모델, svc-rewrite 한계 모델)은 read-only·비root·public API·W^X-safe·in-process를 유지. 깨질 위험이 있는 것은 전부 device-only로 격리. **나(연구자)는 device 없음 → 산출은 여기까지(코드분석+가설+실험설계+host 검증 모델); device 확정은 통합 세션 DEVICE-REQ.**

---

관련 파일(절대경로):
- 본 ADR 제안 위치: `/Users/naen/Documents/alr-chromium-storm/docs/design/adr-004-chromium-storm-deadlock.md`(미작성 — 이 산출물이 본문; 통합 세션이 커밋)
- 선행: `/Users/naen/Documents/alr-chromium-storm/docs/design/adr-002-chromium-cp6-roadmap.md`, `/Users/naen/Documents/alr-chromium-storm/docs/design/adr-003-multiprocess-exec-reentry.md`, `/Users/naen/Documents/alr-chromium-storm/docs/research/cp6-status.md`
- WS-1 코드: `/Users/naen/Documents/alr-chromium-storm/app/src/main/cpp/runtime_report.cpp`(alarm L1938, waitpid 루프 L2138, EVENT_SECCOMP+trace_hist L2166/L2183, EVENT_STOP "THE MULTI-THREAD FIX" L2754–2820, EVENT_CLONE `++guest_threads` L2828, SIGSYS emul L2856/backstop L2889), `/Users/naen/Documents/alr-chromium-storm/app/src/main/cpp/alr_interpose/libalr_interpose.c`(PCGATE BPF L471–510, 트램폴린 path L66–72)
- WS-5: `/Users/naen/Documents/alr-chromium-storm/bench/storm_cost_model.py`(`estimate_storm_seconds` L222, `StormMeasurement` L258, `evaluate` L289), `/Users/naen/Documents/alr-chromium-storm/bench/syscall_mix.py`(`classify_storm` L191), `/Users/naen/Documents/alr-chromium-storm/tests/svc_match.py`(`find_svc_sites` L244, `make_svc_to_br_patch` L210), 신규 `bench/deadlock_triage.py`

Sources(코드 사실은 위 파일 직접 확인): [ptrace(2) — PTRACE_SEIZE/EVENT_STOP/PTRACE_LISTEN, group-stop semantics](https://man7.org/linux/man-pages/man2/ptrace.2.html), [seccomp(2) — filters across clone/execve, RET_TRACE/RET_ALLOW](https://man7.org/linux/man-pages/man2/seccomp.2.html), [seccomp_filter — Linux Kernel docs](https://docs.kernel.org/userspace-api/seccomp_filter.html)