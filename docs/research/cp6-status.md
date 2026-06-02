# CP-6 진행 SSOT — Chromium-class native 실행 + raw-svc storm 분기 (WS-5 / L5)

> **"CP-6(Chromium 류 글리브 게스트가 ALR 로 어디까지 가는가)의 현 위치와 다음 분기"** 를 한 곳에
> 모은 SSOT. chromium 은 **사용자(찬우) 보류** 상태이므로, 이 문서는 *프로브/계측으로 device-확정된
> 사실*과 *그에 따른 결정 분기*만 정직하게 기록한다(본질해법 **구현** 착수는 보류 해제 + device 게이트
> 통과 후). 설계 근거는 세 ADR 에 있고 이 문서는 그것을 한 흐름으로 잇는다:
>
> - `docs/design/adr-001-syscall-overhead-user-notif.md` — syscall 중재 오버헤드 + USER_NOTIF 평가(원형).
> - `docs/design/adr-002-chromium-cp6-roadmap.md` — raw-svc storm 돌파(측정-우선; svc-rewrite vs USER_NOTIF A/B).
> - `docs/design/adr-chromium-storm-deadlock.md` — chromium `--dump-dom` "데드락" 재진단: **오진(misdiagnosis)**,
>   measure-first(측정창 분리 실험으로 진단부터 닫음; supervisor 재-tweak 금지).
> - `docs/design/adr-003-multiprocess-exec-reentry.md` — 멀티프로세스 exec re-entry(v1 상속 가설→round-7 반증;
>   §8 v2 Option S→round-9 W^X DEAD; v3 in-process 재-맵→round-10 device-proven).
>
> 소유: WS-5(L5). HOST-ONLY — 새 device 측정 없음, 기존 `docs/evidence/` 인용만. 벤치 문서 아님
> (성능 숫자는 `docs/PERFORMANCE.md`/`docs/research/cp3-cpu-overhead-ratio.md`). 잠긴 *범용* 로더
> 기능 SSOT 는 `docs/research/loader-feature-gaps.md`(storm 벽은 거기서 *갭으로 격상하지 않음* —
> CP-6 보류분이므로 여기로 분리).
>
> baseline: 통합 트리 v143 (round-10 step2 `docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`;
> round-10 step1 `docs/evidence/2026-06-02-round10-step1-inproc-reexec-mechanism-proven.md`;
> round-9 `docs/evidence/2026-06-02-round9-optionS-dead-wx-execve.md`;
> round-7 drain `docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`;
> round-6 `docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`). 디바이스 `R5KL20B6S3X`
> (SM-X236N, mt6878, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).

---

## 0. 한 줄 현 위치

**chromium-headless-shell(Chromium 147)이 ALR 네이티브 로더로 device 에서 `--version` 실행(child
exit=0)됐고, 그 init 경로의 중재 오버헤드 verdict 는 `mediation-negligible`(traps=0, emul=1) 이다.**
즉 *init* 은 라운드트립 storm 이 아니다 — 무거운 `--dump-dom` *render* 경로만 남았고, 그건 지금까지
"멀티스레드-ptrace 데드락"으로 불렸으나 **PR #2(`docs/design/adr-chromium-storm-deadlock.md`)가 그
데드락 진단을 *오진(misdiagnosis)*으로 재진단**했다(아래 §5). 5축 자가-적대 재진단이 세 데드락 후보
(clone-못함/clone-trap-block/futex-deadlock)를 전부 코드근거로 기각하고, best-가설 = **deadlock 이
아니라 무거운 single-init 이 `alarm(25s)` 측정창을 첫 워커 clone 전에 만료시킨 것**으로 좁혔다 →
verdict = **measure-first**(supervisor 재-tweak 금지, read-only 측정창 분리 실험으로 진단부터 닫는다).
그 측정으로 render 경로의 N(절대 raw-svc 수)·trap 분포가 device 로 측정돼야 비로소 두 본질해법
**M-R5(svc-rewrite) vs M-R1(USER_NOTIF)** 의 A/B 가 결정 가능하다.

### 0-a. "`--version` 실행"에서 "browser 가 페이지를 렌더"까지 — CR-1..CR-5 사다리

위의 `--version` device-PASS 는 *바이너리가 뜨고 풀 closure 를 링크하고 깨끗이 종료* 레벨일 뿐,
**브라우저로서 페이지를 렌더하는 것**은 아니다. 그 사다리의 실행-경로 SSOT 가 새 문서
**`docs/research/chromium-run-plan.md`**(CR-1..CR-5; GPU flag 사다리는 자매
**`docs/research/chromium-gpu-path.md`**)다:

- **CR-1 (near-term)** — `chromium-headless-shell --single-process --no-zygote --no-sandbox
  --disable-gpu --dump-dom <data:/file: 페이지>` = single-proc headless 가 *실제로* 페이지를
  파싱·레이아웃·렌더하고 DOM 을 dump(net/GPU/멀티proc 0). drain gate = logcat `child exit=0` +
  `guest stdout=` 에 직렬화된 DOM. **net/GPU/멀티proc 벽을 모두 우회하므로 유일한 near-term 칸.**
- **CR-2** — += 진짜 `https://` URL(net overlay: CA bundle/DNS). 여전히 `--single-process`.
- **CR-3** — += GPU(`--use-gl=angle --use-angle=swiftshader` → … → 우리 GLES shim → Mali; 사다리/
  staged-lib = `chromium-gpu-path.md`). 여전히 `--single-process`(in-proc GPU 스레드).
- **CR-4** — `--ozone-platform=wayland` = GUI 창을 우리 in-app Wayland compositor 에(필수 wl =
  `wl_compositor`/`wl_shm`/`xdg_wm_base`). 여전히 `--single-process`.
- **CR-5 (장기, G1-gated)** — `--single-process` 떼기 = 멀티프로세스 zygote. **이건 §3 의 G1
  (in-process 재-맵 exec re-entry)에 게이트** — G1 은 메커니즘+map/jump device-proven(ADR-003-v3,
  `0x400640`)이나 재-맵 게스트 SIGILL + `/proc/self/exe` pass-through 가 남아 **RUNS 아님(in-flight)**.

**왜 near-term 이 CR-1 뿐인가 — measure-first.** CR-1 의 `--dump-dom` 은 과거 "멀티스레드-ptrace
데드락"으로 stall 했으나, **PR #2(`docs/design/adr-chromium-storm-deadlock.md`)가 그 데드락을
오진(misdiagnosis)으로 재진단** — best-가설은 데드락이 아니라 **무거운 single-init 이 `alarm` 측정창을
첫 워커 clone 전에 만료**(§5; chromium 은 이제 120s 를 받는다). 따라서 CR-1 의 1순위는 supervisor
re-tweak 가 아니라 **측정창 분리**(`ALR_GUEST_ALARM_S`)로 render 가 끝까지 진행하는지 device 1회로
가르는 것. 그리고 **`--single-process`(+`--no-zygote`)가 멀티프로세스 zygote 벽(=CR-5/G1)을 우회**
하므로 CR-1..CR-4 는 G1 없이 진행 가능하고, G1 은 CR-5 에서만 게이트된다(그래서 CR-5 만 장기).

exec re-entry(멀티프로세스)는 storm/데드락과 **독립된 별개 벽**(thread 벽이 아니라 exec 벽)이고, 이
벽은 round-9→round-10 에서 **device-정복**됐다 — **in-process 재-맵(커널 execve 전무)이 map+jump 까지
device-proven**(ADR-003-v3). round-6/7 의 B-1(execve x0 path-rewrite) + B-3(child envp) 는
device-fires 하나 **모든 execve 에서 `exec_events=0`**(커널이 glibc-aarch64 ELF 의 `PT_INTERP`=게스트
ld.so 를 resolve 못 함 ⇒ 커널-execve 미완)으로 **불충분**임이 드러났고, round-9 v140 이
ADR-003-v2 **Option S(커널-execve stub)를 W^X 로 DEAD** 입증(`app_data_file:execute` neverallow)한 뒤,
round-10 이 **ADR-003-v3(in-process 재-맵, NO execve)로 메커니즘(v141) + map+jump(v143)을 device-proven**
했다(§3). 따라서 chromium 의 zygote/gpu fresh-execve 자식(=멀티프로세스의 핵심)은 이제
**in-process-remap 트랙 위**에 있다 — 단 재-맵된 static 게스트가 startup 에서 **SIGILL** 하고
chromium 은 추가로 `/proc/self/exe`(Android linker64) re-exec 라는 별도 pass-through 가 필요해, 아직
device-RUNS 승급은 안 된다.

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
*at-style x1 만 읽고 exec 는 `is_exec` 로 건너뜀, `app/src/main/cpp/runtime_report.cpp`).

**round-6 v138 device 진전: B-1 execve x0 path-rewrite 가 device-fires.**
`docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`:
```
alr exec x0=/bin/sh reason=rewrite     traps=1 rewrites=1 exec_events=0 clone_events=7
```
게스트 `execve(/bin/sh)` 가 EVENT_SECCOMP 에서 trap 되고 program path(x0)가 rootfs 로 재작성됨
(argv/envp 불변), 게스트 fork(7 clones). no-exec 게스트는 `traps=0 rewrites=0`(무회귀). 즉
ADR-003 B-1 경로는 하드웨어에서 작동한다. 단 `apt-install: unpacked=false`.

**round-7 v139 device 결정: B-3 도 평가되나 `exec_events=0` 가 진짜 벽을 드러냄(execve 미완).**
`docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`:
```
alr exec x0=/bin/dash reason=rewrite envp_reason=already
alr exec envp_injected=0 ld_preload_set=0      (모든 exec)
apt-install: unpacked=false configured=false exec=[ALR NATIVE LOADER GUEST EXEC: FAIL]
```
**B-3**(child envp 재주입) 결정함수가 모든 execve/execveat trap 에서 평가되며 `/bin/dash` 에 대해
`envp_reason=already` 를 냈다 — exec 된 child 가 로더-설정 `LD_PRELOAD`/`ALR_ROOTFS` 를 *상속*하므로
재주입 불필요(`envp_injected=0` 은 올바른 no-op). **그러나 결정적 관측: 관측된 모든 exec 에서
`exec_events=0`** — `PTRACE_EVENT_EXEC`(새 program image 가 로더 아래에서 실제 실행 진입)가 한 번도
발화하지 않는다. 근인은 커널이 glibc-aarch64 ELF 의 execve 를 **완료하지 못함**(그 `PT_INTERP`=게스트
ld.so `/lib/ld-linux-aarch64.so.1` 를 Android 커널이 resolve 불가 → execve 실패, 새 이미지 없음).
따라서 ADR-003 의 전제("(B) 는 'loader 재진입'이 아니라 seccomp-across-execve + `PTRACE_O_TRACEEXEC`
자동 재포착으로 풀린다")는 **하드웨어로 disproven** 이며, B-1 + B-3 는 **necessary-but-NOT-sufficient**.
진짜 벽 = **loader 의 on-exec in-process 새-ELF 재-맵(re-entry stub / loader-as-bootstrap)** — 설계는
**ADR-003-v2**(docs/design/, 병행 세션 소유; 이 SSOT 는 경로만 참조)가 담당. apt 는 추가로 `apt`
top-level 이 `GUEST EXEC FAIL`(`traps=0`)로 execve trap 전에 실패 — apt 가 minimally-staged
(`apt-config-stage.tar` 10KiB; 전체 closure 부재)라 full apt 스테이징 오버레이가 병행 진행된다.

**★ round-9→round-10 device: in-process 재-맵 벽 정복(커널 execve 0).** round-7 이 지목한 "on-exec
새-ELF 재-맵" 벽이 **device-proven 으로 정복**됐다 — 단 ADR-003-v2 의 *방식*은 죽고 ADR-003-v3 가 정답.
- **round-9 v140 — Option S DEAD(W^X).** `docs/evidence/2026-06-02-round9-optionS-dead-wx-execve.md`:
  ADR-003-v2 의 Option S(커널이 적재 가능한 정적 re-entry stub 을 execve)를 시험했으나 splice 정상
  발화(`spliced=1`)에도 **모든 execve 에서 `exec_events=0`**, stub 미실행(2초마다 재푸시해 stub 존재
  확인 후에도 동일). 근인 = **W^X**: rootfs 파일이 `app_data_file` 라벨이고 targetSdk 35 untrusted_app
  정책의 `neverallow untrusted_app … app_data_file:file execute` 가 app-storage 파일의 *모든* execve 를
  막는다. **커널-execve 로 re-entry 하는 길은 비-root untrusted_app 에서 구조적으로 죽었다.**
- **round-10 step1 v141 — 메커니즘 device-proven.**
  `docs/evidence/2026-06-02-round10-step1-inproc-reexec-mechanism-proven.md`:
  `ALR-REEXEC: inproc trampoline reached (no execve)` / `inproc_redirected=1` / child exit=123. execve
  seccomp-trap 에서 `NT_ARM_SYSTEM_CALL=-1`(커널이 execve skip) + PC 를 fork-상속 loader `.text` 의
  resident 트램폴린으로 redirect → **커널 execve 0** 으로 resident 코드 진입.
- **round-10 step2 v143 — 진짜 map+jump device-proven.**
  `docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`:
  `ALR-INPROC: mapped, jumping entry=0x400640` — 트램폴린이 target ELF 를 `mmap(PROT_EXEC)`(W^X-허용)
  로 in-process map → 새 SysV stack → **entry 점프(커널 execve 0)**, static+dynamic 경로 wired.

즉 chromium 멀티프로세스의 핵심인 (B) fresh-execve 자식은 이제 **in-process-remap 트랙**(ADR-003-v3)
위에 있다. 남은 것 = (i) 재-맵된 static glibc 게스트가 자기 startup 에서 **SIGILL**(실행-정확성 버그),
(ii) chromium 의 `**/proc/self/exe**`(interp `/system/bin/linker64`) re-exec 는 *Android* 앱 바이너리라
Debian rootfs 로 매개 불가 → **별도 pass-through** 필요(rootfs-바이너리 재-맵과 구분),
(iii) apt 는 비-root `dpkg: requires superuser`(fakeroot/root-emulation)로 exec-re-entry 와 **독립**.

| 자식 클래스 | 띄우는 법 | ALR 중재 | 상태 |
|---|---|---|---|
| renderer(다수) | zygote가 **fork**(no exec) | 이미 매개(주소공간 복제 + 상속 seccomp/SEIZE) | 자동(ADR-003 §2-A) |
| zygote/gpu | browser가 **fresh execve** | (B) exec 벽 — B-1 x0-rewrite + B-3 envp **만으론 불충분**(`exec_events=0`); on-exec 재-맵 필요 | **재-맵 메커니즘+map/jump device-proven(r10, ADR-003-v3)**; 단 재-맵 게스트 SIGILL + `/proc/self/exe` pass-through 잔여 ⇒ 미승급 |

**급소(재구도, round-7).** round-6 의 가설은 "남은 급소 = B-3 child envp 재주입"이었으나 round-7 가
이를 **반증**했다: 관측된 execs 는 LD_PRELOAD 를 *상속*하므로 envp 주입이 불필요(`envp_reason=already`)
한데도 `apt-install: unpacked=false` 가 변하지 않는다. 진짜 급소는 **execve-completion / 새-이미지
재진입** — exec 된 rootfs 바이너리가 ALR 중재 아래에서 *실제로 실행*되어야 하고, 그건 envp 가 아니라
**loader 가 exec 시 새 ELF 를 직접 in-process 재-맵**해야 풀린다(`/proc/self/exe` 가정-3,
AT_SECURE 가정-2 도 이 재-맵 위에서만 의미). chromium 멀티프로세스(zygote/gpu)는 이 재-맵에 게이트된다.

**device 프로브(read-only, 보류 무관)**: M-R4-fork(`--single-process --no-zygote` 기준선) /
M-R4-execmap(clone:exec 분류 + x0 path 분포) / M-R4-envprop(`ALR-ENVPROP ld_preload=<0|1>` 로깅).
(B-1) x0-rewrite **실구현** 착수는 M-R4-execmap 이 "exec 자식이 rootfs-내 절대경로 사용(`/proc/self/exe`
아님)"을 device 로 보인 후 + 사용자 재개 신호로 게이트(ADR-003 §5).

**host 프로토타입(WS-5)**: `tests/exec_map_model.py`(clone:exec 분류 파서; `tests/test_exec_map.py`) + `tests/test_execve_pathrw.py`
(execve **x0** vs *at-style **x1** 분기 결정모델 — `is_exec`면 regs[0] 읽고 argv/envp 불변, `/proc/*`
rewrite 제외, rootfs-내 idempotency guard). darwin 호스트는 실커널 seccomp-across-execve/SEIZE-EVENT_EXEC
거동 불가 → 분류/분기 로직 회귀만, 상속 효과는 device-only.

---

## 5. chromium `--dump-dom` "멀티스레드 데드락" 재진단 — 오진(misdiagnosis), measure-first

§3 의 exec 벽(thread 벽이 아니라 exec 벽)과 **직교**하는, render 경로의 "멀티스레드-ptrace 데드락"
진단이 PR #2 에서 **재진단**됐다(`docs/design/adr-chromium-storm-deadlock.md`; 격리 브랜치
research/chromium-storm, base v141, main·ws-N 미변경). 본문이 칭하는 'ADR-004' 번호는 product-ux
브랜치 충돌로 통합 세션이 파일명(`adr-chromium-storm-deadlock`)으로 재번호.

**한 줄 결론: "멀티스레드-ptrace 데드락"은 best-가설로 *오진(misdiagnosis)*이다.** 5축 자가-적대
재진단(R1 재진단/R2 우회측정/R3 svc-rewrite/R4 USER_NOTIF/R5 적대재정의)이 코드 5대 사실로 세 데드락
후보를 **전부 기각**했다:
- **(a) seccomp-floor-glacial**(단일 init 이 라운드트립 storm 으로 느림) — PCGATE BPF 는 9 path nr
  만 RET_TRACE, clone/clone3/futex/mmap 등 비-path 전부 RET_ALLOW → 0 라운드트립. M-R2 device traps=0
  (init)이 확증.
- **(b) clone-trap-block**(supervisor 가 clone 못 처리) — clone/clone3 은 비-path RET_ALLOW,
  EVENT_CLONE 처리는 즉시 CONT. "supervisor 가 clone 못 처리"가 아니라 "게스트가 아직 clone 안 함".
- **(c) futex-deadlock** — futex 도 비-path RET_ALLOW(emul 안 됨); single-init 이 futex 로 워커를
  기다리는 단계에 도달조차 안 했을 수 있음.

best-가설 = **deadlock 이 아니라 무거운 single-thread bring-up 이 측정창을 만료**: dynamic 게스트의
`alarm(25s)`(`runtime_report.cpp` L1938)가 chromium 의 ld.so 동적링크 + V8 부트스트랩 + single-init
render 가 *첫 워커를 clone 하기도 전에* 만료해 `Threads=1` 스냅샷만 잡힌 것(utime 1→5 기어감 =
단일 메인스레드가 R state 로 *진짜 진행 중*이라는 직접 증거). **미스터리 해소**: L2754 의 SEIZE/
EVENT_STOP fix 는 정상이나 *워커가 clone 된 후에만* 발화 — `Threads=1` 이면 그 분기에 진입조차 안 하니
"fix 작동함"과 "증상 남음"은 무교차(non-intersection).

**verdict = measure-first(5축 만장일치).** supervisor 를 또 tweak 하는 길(v122–v124)은 막다른 골목으로
**금지**. 대신 **read-only 측정창 분리 실험**: dynamic alarm 을 `ALR_GUEST_ALARM_S` 로 파라미터화
(예 180s) + supervisor 에 `EVENT_CLONE` 시계열(tid·n·t_ms) + 멈춘 tid 의 마지막 nr 로깅 → "게스트가
첫 clone 에 도달하는가/몇 초에/멈추면 어느 nr 에서"가 device 한 방에 답해져 (a)/(b)/(c)/window-too-short
4분류 결정. **단 2차 가설(v124 SEIZE 전환이 *새* group-stop 데드락을 유발했을 가능성)은 device 전에
배제 불가** → 정직하게 measure-first. host 코어 = `bench/storm_microbench_model.py` +
`tests/test_storm_microbench_model.py`(N-thread raw-svc storm cost 모델). 이 재진단은 §0·§2 의 storm
A/B(M-R5 vs M-R1) *앞에* 끼는 1순위 진단이다 — render storm 의 N 이 측정되려면 먼저 "render 가 첫
워커 clone 에 도달"해야 하므로.

---

## 4. ADR 상호참조 한눈에

| ADR | 무엇 | 이 문서와의 관계 |
|-----|------|-----------------|
| **ADR-001** | syscall 중재 오버헤드 측정 + USER_NOTIF 후보(원형) | §2 의 "24ns 바닥은 seccomp 켜는 한 안 사라짐"·USER_NOTIF 후보의 출발점 |
| **ADR-002** | raw-svc storm 돌파(측정-우선; M-R2/M-R5/M-R1/M-R3) | §1 M-R2 verdict·§2 분기(svc-rewrite vs USER_NOTIF A/B)의 근거 |
| **adr-chromium-storm-deadlock** | chromium `--dump-dom` "멀티스레드 데드락" 재진단 = **오진(misdiagnosis)**; measure-first(측정창 분리 실험) | §0·§5 — ADR-002 §1 분기 *앞에* 새 1순위 진단을 끼움; 데드락 3후보(clone-못함/clone-trap/futex) 전부 기각, best-가설 = window-too-short on heavy single-init; supervisor 재-tweak 금지 |
| **ADR-003** | 멀티프로세스 exec re-entry(v1: 상속 채택, loader 재진입 기각) | §3 의 자식 클래스 분리·envp 전파 급소·M-R4 프로브 — **단 round-7 가 "상속만으로 충분" 전제를 반증**(`exec_events=0`) ⇒ 재-맵으로 후속(아래) |
| **ADR-003-v2** | Option S — 커널이 적재 가능한 정적 re-entry stub 을 execve(loader-as-bootstrap) | §3 의 round-7 진짜 벽 후보였으나 **round-9 v140 이 W^X 로 DEAD 입증**(`app_data_file:execute` neverallow; `exec_events=0`, stub 미실행) |
| **ADR-003-v3** | in-process 재-맵 — 커널 execve 0, execve 취소 + PC-redirect → resident 트램폴린 map+jump | §3 의 정복 경로: **round-10 step1(메커니즘 v141) + step2(map+jump v143) device-proven**; 잔여 = 재-맵 게스트 SIGILL + `/proc/self/exe` + 비-root dpkg |

**정직 섹션(미확정 — device 측정 전).** ① render storm 절대 N(=5M~50M?) 미측정 — 모든 정량모델의
핵심 입력. ② stime 지배 요인(라운드트립 vs chromium 자체 24ns×N) — M-R2 storm 분해가 답하나 데드락
선결. ③ NEW_LISTENER 가 untrusted_app SELinux 통과하는지(M-R1 device-only). ④ svc-rewrite 가
186MB+수백 .so 안정 치환(M-R5 device-only). ⑤ **execve-completion 재진입**(round-7 v139: B-1
x0-rewrite + B-3 envp 모두 device-fires 하나 `exec_events=0` ⇒ 커널-execve 미완. round-9 v140: ADR-003
-v2 Option S 커널-execve stub 은 **W^X DEAD**. round-10 v141/v143: **ADR-003-v3 in-process 재-맵이
메커니즘 + map/jump device-proven**(커널 execve 0). **잔여(device-pending)**: 재-맵된 static glibc
게스트가 startup 에서 **SIGILL**(실행-정확성), chromium 의 `/proc/self/exe` re-exec pass-through, 비-root
`dpkg: requires superuser`(fakeroot), full apt 스테이징 오버레이). ⑥ **render storm "데드락"이 정말
window-too-short 인가 vs SEIZE-induced-new-deadlock 인가**(§5) — `ALR_GUEST_ALARM_S` 측정창 분리 실험이
device 1회로 가른다(measure-first; 2차 가설 device 전 배제 불가).

*갱신 규칙:* device evidence 추가 시에만 RUNS/PASS 승급(evidence 파일명 명기). host-only 진전
(프로토타입/계측 설계)만으로는 storm/exec 벽을 "풀림"으로 올리지 않는다. CP-6 는 사용자 보류이므로
본질해법/x0-rewrite **구현** 착수는 보류 해제 + 해당 device 게이트 PASS 후. 잠긴 *범용* 로더 기능은
`docs/research/loader-feature-gaps.md`, GUI 렌더 셋은 `docs/research/gui-universality-status.md`,
**"`--version` → browser 렌더" 실행 사다리(CR-1..CR-5)는 `docs/research/chromium-run-plan.md`**
(GPU flag 사다리 = `docs/research/chromium-gpu-path.md`).
