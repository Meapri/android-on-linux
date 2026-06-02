> 생성 경위: 5축 병렬 연구 워크플로우(`chromium-cp6-breakthrough`, run `wf_dd661532`) 산출 —
> USER_NOTIF 구현설계(R1) / syscall-mix 측정(R2) / seccomp-off 봉인(R3) / 적대·svc-rewrite(R5)
> 심층연구 + 각 축 적대적 검증을 종합. R4(멀티프로세스 exec re-entry)는 검증 단계가 실패하여
> 본 ADR에 미반영(별도 후속 필요).
> 위치: 격리 브랜치 `research/chromium-cp6`(보고/연구 담당 작성). `main`·`ws-N` 미변경.
> 리뷰 대상: WS-1(`runtime_report.cpp`/`libalr_interpose.c`), WS-5(`tests/`/`bench/`), 통합 세션.

# ADR-002 — CP-6 Chromium raw-svc storm 돌파 로드맵 (측정-우선, svc-rewrite vs USER_NOTIF)

- 상태: **Proposed** — 5축 연구(R1/R2/R3/R5) + 적대적 검증 종합. 코드 변경 없음, device 게이트 미통과.
- 워크스트림: **WS-1**(L1 CPU 실행/중재, `runtime_report.cpp`·`libalr_interpose.c`) 주관, **WS-5**(tests/bench/docs/evidence) 협의.
- 작성: 2026-06-02. 선행: `docs/design/adr-001-syscall-overhead-user-notif.md`(이 문서가 갱신/도전).
- 대상: ADR-001 §4 표의 미해결 행 **"raw `svc` 비-path storm (Chromium)"**.
- HARD CONSTRAINTS(불변): 비root(untrusted_app, no CAP_SYS_ADMIN), public Android API only, SELinux 우회 금지, W^X-safe, in-process(PRoot fallback-only), device evidence 없이 완료 주장 금지, version stamp 불변.

---

## 1. 한 줄 결론

**CP-6는 단일 은탄환으로 안 뚫린다 — 측정으로 storm 분해를 먼저 하고(R2, 즉시·무위험), 그 결과에 따라 본질해법을 `svc` binary-rewrite(R5, 잠재 최강·고위험)와 USER_NOTIF(R1, ADR-001 후보1·중위험)의 device A/B로 결정한다.** 단 ADR-001 §1-B의 못은 두 본질해법에도 그대로 박혀 있다: **seccomp를 켜는 한 24ns/syscall 디스패치 바닥은 어떤 모델로도 안 사라진다** — R5 검증이 밝힌 결정적 사실로, ALR 트램폴린의 종착점도 `svc #0`이고 PCGATE BPF는 트램폴린 PC를 포함한 **모든** `svc`를 한 번 태워 RET_ALLOW를 돌려준다(`libalr_interpose.c` 주석 L63). 따라서 CP-6의 현실 목표는 ADR-001과 동일하게 **"syscall 0%"가 아니라 "라운드트립 제거로 storm을 usable화"** 이며, 그 usable 여부는 아직 미측정인 storm 절대 syscall 수(N)에 전적으로 걸려 있어 **device 측정 전에는 어느 경로도 "뚫린다"고 단정할 수 없다.**

---

## 2. 다음 착수할 device-ready 단계 (우선순위순)

### M-R2 (1순위, 즉시·무위험·device-필요) — storm 분해 계측

검증에서 **유일하게 holds=true·priority=now·constraint_violations 없음**. ADR-001의 미검증 핵심 (c)를 "흡수율 추정" 이전에 "라운드트립 발생원 규명"으로 재정의한다. 핵심 코드 사실(검증 확정): 현 PCGATE BPF(`libalr_interpose.c` L471-510)는 9개 path nr `{34,35,48,56,78,79,291,437,439}`만 RET_TRACE 후보로, **그 외 raw-svc 전부 RET_ALLOW** → "비-path raw-svc storm은 우리 stacked 필터로는 0 라운드트립"이 코드 사실. 따라서 stime~70×의 진짜 발생원은 (i) path syscall을 raw-svc로 발행해 트램폴린 밖 PC에서 RET_TRACE되는 경우 + (ii) zygote가 SIGSYS하는 KNOWN path nr(openat2=437/faccessat2=439, 주석 L87-89) + (iii) unknown SIGSYS-emulate 다.

- **무엇**: supervisor의 기존 두 trap 사이트(`runtime_report.cpp` PTRACE_EVENT_SECCOMP 핸들러 ~L563/L2057, SIGSYS emulate ~L2282)는 이미 `regs[8]`=nr을 GETREGSET으로 읽는다 → `std::unordered_map<int,uint64_t>` 2개(`trace_hist`/`emul_hist`)로 nr별 집계만 추가(**신규 ptrace op 0개, hot-loop storm 오염 0, 신규 권한 0**). 루프 종료 시 시작/종료 `/proc/<pid>/stat`(stime/utime field 14/15) + Σ tid `/proc/<tid>/status`(nonvoluntary_ctxt_switches) 1회씩 읽어 출력.
- **파일(소유권)**: `runtime_report.cpp`(WS-1) 계측 + 출력 라인. 파서/모델 = `bench/report_parse.py`·신규 `bench/syscall_mix.py`(WS-5). 출력 포맷은 WS-1→WS-5 핸드오프 협의.
- **선행 수정(필수)**: emulate backstop 상한이 `emulated_syscalls > 8192 → SIGKILL`(L2313-2316), 저장은 `emulated_list[64]`(L1946)에서 멈춤 → storm에서 분포가 잘린다. 1회 측정 한정 상한 상향(또는 시간상한)으로 전체 분포 확보.
- **device 게이트 PASS 조건**: `chromium-headless-shell --no-sandbox --headless --dump-dom about:blank` 1회에서 logcat `alr_loader`에 `alr sc trace_hist nr:count...` + `alr sc emul_hist nr:count...` + `alr sc stime_us=.. utime_us=.. nonvol_ctxt=.. traps=.. emul=..`가 캡처되고, child exit=0 무회귀. **판정 분기**: `nonvol_ctxt ≈ 2×(traps+emul)`이면 라운드트립이 stime 지배 → 본질해법(M-R5/M-R1) ROI 큼; 어긋나면 stime은 chromium 자체 ALLOW-syscall 24ns×N 무게 → **USER_NOTIF/svc-rewrite로도 못 줄임이 device로 폭로**되어 목표 재정의로 직행.
- **DEVICE-REQ 마커**: `DEVICE-REQ: ALR-M-R2-syscall-mix — SM-X236N (am force-stop first), chromium-headless-shell --no-sandbox --dump-dom about:blank ×1; capture 'alr sc trace_hist' + 'alr sc emul_hist' + 'alr sc stime_us/utime_us/nonvol_ctxt/traps/emul'; gate = lines present AND child exit=0 AND emul backstop raised this run. 외부 strace 금지(supervisor-내부 집계만).`
- **위험**: nonvol_ctxt 신호분리 불완전(chromium 자체 futex/epoll_wait 블로킹도 ctxt switch 생성 — 검증·R2 스스로 인정). 그래도 trace_hist/emul_hist의 path vs 비-path nr 비율은 직접 답을 준다.

### M-R5/M-R1 A/B (2순위, device-필요) — 본질해법 device 결정: `svc`-rewrite vs USER_NOTIF

M-R2가 "라운드트립 지배"를 확증하면, **두 본질해법을 같은 raw-svc storm에서 device A/B로 비교해 채택을 결정**한다. 검증상 둘 다 **holds=uncertain, constraint_violations 없음** — 메커니즘은 건전하나 효과가 미실측 N에 걸림.

**(2a) M-R5-svcscan — `svc` binary-rewrite 가용성/ROI 프로브 (read-only 먼저).** R5는 ADR-001 후보2의 좁은 변형(ASC-Hook식 ARM64 `svc` in-process rewriting)을 본질해법으로 격상. 요구하는 W^X 게이트는 v120 device-PASS(`2026-06-01-...v120-jit-wx-cycle.md`: rwx_mmap_ok=true, wx_granularity_ok=true)로 **정확히 열려 있음** → 비root·in-process·W^X 무위반.

- **무엇(프로브)**: 게스트 로드 후 `/proc/self/maps`+PT_LOAD 워크로 text의 `mov x8,#nr; svc #0`(0xd4000001) 사이트를 **스캔만**(rewrite 안 함) → 총 site 수 + x8-mov 선행으로 안전치환 가능 비율 로깅. ROI 확정 후에만 실제 rewrite(`svc`→기존 `alr_tramp_syscall` 트램폴린으로 `br`) 단계.
- **파일(소유권)**: 스캐너+L1/L2 트램폴린 = `libalr_interpose.c`(WS-1), 로드 후 1회 스캔 훅 + dlopen 재스캔 = `runtime_report.cpp`(WS-1). host 인코더/디코더 단위테스트 = `tests/`(WS-5).
- **device 게이트 PASS**: `svc_sites=<n> rewritable=<n>/<n>` 로깅 + child exit=0 무회귀. 후속 rewrite 단계 게이트 = 같은 storm에서 traps 감소 + exit=0.
- **DEVICE-REQ 마커**: `DEVICE-REQ: ALR-M-R5-svcscan — SM-X236N, chromium-headless-shell in-process 직후 svc-scan(read-only); expect 'ALR-SVCSCAN svc_sites=<n> rewritable=<n>/<n>'; gate = 로깅 present AND child exit=0 무회귀.`
- **위험(검증 확정)**: ① **kill-shot 산술 오류** — R5의 "svc-rewrite는 seccomp 미경유 → 24ns 바닥 제거"는 **거짓**. 트램폴린 종착 `svc`도 PCGATE BPF를 태운다(주석 L63). 50M storm은 24ns 바닥만 1.2s, +hook 34ns면 ~2.9s(R5 주장 1.7s가 아님, ~70% 과소). 단 2.9s도 usable이라 **정성 결론(svc-rewrite ≫ USER_NOTIF)은 뒤집히지 않음**. ② 늦은 dlopen .so의 svc 미포착 + hook가 svc-캡슐화 함수 호출 시 무한루프(ASC-Hook 논문 자인). ③ V8 JIT 런타임 생성 svc는 정적 스캔 회피(chromium은 거의 무해하나 V8 소스 미확정). ④ 186MB+수백 .so 전 hot site 안정 치환 미검증.

**(2b) M-R1-unotif — USER_NOTIF 가용성 프로브 (ADR-001 M3-a 구현 강하).** R1은 ADR-001 후보1을 구현 설계로 한 단계 내림. 결정적 발견(커널 사실 일치): **TSYNC+NEW_LISTENER는 EINVAL 충돌**(v5.7 commit 51891498 TSYNC_ESRCH로 해소) → 현 PCGATE가 TSYNC를 쓰므로(`libalr_interpose.c` L518) notif 필터는 **별도 설치** 필요. 기존 인프라 재사용 확정: `socketpair`(L899/943)·go_pipe SCM 핸드셰이크(L1618-1627)·PTRACE_SEIZE·EVENT_SECCOMP·GETREGSET 모두 코드에 존재 → listener fd를 부모로 넘길 배관이 이미 있음.

- **무엇(프로브)**: fork 직전 `socketpair(AF_UNIX,SOCK_SEQPACKET)` → 자식 ctor에서 `seccomp(SET_MODE_FILTER, NEW_LISTENER, &allow_prog)`(먼저 flag 0, 실패 시 `TSYNC_ESRCH|NEW_LISTENER`) → fd>=0 → `sendmsg(SCM_RIGHTS)` 부모 전달 → 부모 recvmsg → 게스트가 화이트리스트 nr 1개(getpid) 1회 발행 → 부모 notif 스레드 `ioctl(NOTIF_RECV)` 1건 → `resp.flags=SECCOMP_USER_NOTIF_FLAG_CONTINUE`로 `ioctl(NOTIF_SEND)` → 1왕복.
- **스레드 역할(R1 설계)**: 부모 (A) 기존 waitpid SEIZE 루프 = execve 트랩·path RET_TRACE 백스톱·fault 캡처(ptrace만 가능한 TOCTOU-safe 메모리 rewrite 유지) + (B) 신규 notif 스레드 = 단일 listener fd RECV 루프(N 게스트스레드 notif 직렬 수신, 대부분 CONTINUE로 짧게 유지, 워커풀로 확장). 두 채널 독립 — path 9개는 **절대 USER_NOTIF로 안 옮김**(CONTINUE TOCTOU).
- **파일(소유권)**: `runtime_report.cpp`(WS-1) socketpair+notif 스레드, `libalr_interpose.c`(WS-1) 별도 NEW_LISTENER 필터 + 비-path hot nr 화이트리스트 BPF. host BPF 시뮬 = `tests/test_pcgate_bpf_logic.py`(WS-5).
- **device 게이트 PASS**: `ALR USER_NOTIF PROBE: PASS listener_fd=<n> scm_recv=ok recv1=ok send_continue=ok` + child exit=0. **실패 시 후보1 폐기 → M-R5 단독 진행.**
- **DEVICE-REQ 마커**: `DEVICE-REQ: ALR-M-R1-user-notif-availability — SM-X236N (am force-stop first), nativeAlrUserNotifProbe; expect 'ALR USER_NOTIF PROBE: PASS'; FAIL stage=new_listener ⇒ 후보1 폐기→svc-rewrite 단독.`
- **위험(검증 확정)**: ① **NEW_LISTENER가 untrusted_app SELinux/커널을 통과하는지 공개 근거 없음**(WebSearch 확인: Android O seccomp은 syscall-number allowlist + 앱 추가필터 허용이나 NEW_LISTENER 플래그 구별 문서 없음) — device-only. ② 단일 fd 직렬 RECV가 N스레드 storm에서 새 병목 위험. ③ supervisor death → 게스트 syscall ENOSYS로 풀림(ptrace death와 다른 실패모드) → 부모 생존 강보장 필요. ④ 라운드트립 실측 절감 배수(ADR "~10×") 미확정 — WebSearch는 "ptrace보다 효율적"이라는 정성 합의만, 마이크로초급 배수 공개 근거 없음.

### M-R3-seal (3순위, **검증에서 holds=uncertain·priority=later로 강등** — 보조책)

R3(쓰기/설정 절대경로 rootfs 봉인 + 9 path nr를 BPF에서 RET_ALLOW)는 **path storm만 제거하고 raw-svc 비-path storm 벽은 못 깬다**(R3 자인, ADR-001 §4 정합). 검증이 3개 결함 적시: ① V1 "9 path nr 무조건 ALLOW"는 현 PCGATE와 상당 중복이며 supervisor의 raw-svc-path idempotency rewrite backstop을 제거 → 봉인 완벽할 때만 correct. ② zygote가 437/439를 SIGSYS하면 later RET_ALLOW로 구제 불가(stacked = most-restrictive 우선) → path-ALLOW 일부 무효. ③ 봉인 raw-svc path는 trap0이 아니라 rewrite0(trap+GETREGSET+pread 라운드트립은 발생). **→ M-R2/M-R5 PASS 후 잔여 path init 가속 보조로만, 단독 우선순위 낮음.** 파일: `runtime_report.cpp` guest_env(WS-1), `tests/test_pcgate_bpf_logic.py`(WS-5).

---

## 3. host로 지금 만들 수 있는 프로토타입/단위테스트 (device 불요, WS-5 협의)

- **(P1, M-R2용)** `bench/syscall_mix.py`(신규): `trace_hist`/`emul_hist` 'nr:count' 문자열 파싱 → path-nr 집합 `{34,35,48,56,78,79,291,437,439}`로 분할 → path_trap_ratio / 비-path(=흡수가능) 비율 + `nonvol_ctxt÷(traps+emul)` 역산비를 markdown으로. `bench/report_parse.py`에 `_TRACE_HIST/_EMUL_HIST/_STIME` 정규식 추가.
- **(P2, M-R1용)** `tests/test_pcgate_bpf_logic.py` 확장: 별도 USER_NOTIF-action 필터 결정모델 — path9→RET_TRACE, 화이트리스트 비-path nr→RET_USER_NOTIF, 그외→ALLOW, foreign arch→ALLOW, **action 우선순위 signed-min**(RET_USER_NOTIF>RET_TRACE>RET_ALLOW이므로 notif의 그외→ALLOW가 PCGATE의 path→RET_TRACE를 못 이김 = 두 채널 공존 성립)을 0-mismatch로. + `TSYNC` vs `TSYNC_ESRCH|NEW_LISTENER` 반환(fd vs EINVAL) 분기 검증.
- **(P3, M-R5용)** `tests/` 신규: ELF PT_LOAD 워크 + `mov x8,#imm; svc #0` 패턴 매처 + `br` 치환 바이트 인코딩(0xd4000001 식별, 4바이트 정렬 검증)을 순수함수로 떼어 .so 바이트 픽스처 단위테스트. ASC-Hook 3단 트램폴린(3×mov+br 48비트 적재, x8 재assign, x30 stack save) host 어셈→디스어셈 round-trip.
- **(P4, 공통)** storm cost 정량모델(getpid loop·ASC-Hook 34ns·ALR ~100µs 라운드트립)을 bench harness에 편입 → M-R2의 실측 N이 들어오면 즉시 usable 판정. **단 host(darwin)는 arm64 self-rewrite/실커널 NEW_LISTENER 불가** → 메커니즘 회귀만, 효과는 device-only.

---

## 4. ADR-001 대비 추가/수정/도전한 결정 (미검증 a/b/c 진전)

- **[추가·우선순위 역전]** ADR-001은 후보1(USER_NOTIF)을 "본질해법 후보"로 단독 지목했으나, ADR-002는 **측정(M-R2)을 본질해법보다 앞 1순위로** 세운다 — 코드 사실(현 BPF가 비-path raw-svc를 이미 ALLOW)이 ADR-001 §1-B의 "raw-svc 비-path가 매 syscall 라운드트립"이라는 암묵 전제와 모순되기 때문. **stime~70×의 발생원이 라운드트립인지 chromium 자체 24ns×N 무게인지부터 device로 분해**해야 USER_NOTIF/svc-rewrite의 ROI를 안다.
- **[도전·격상]** ADR-001이 "전면 비권고"한 후보2(정적 patch)를, R5는 **좁은 `svc`-rewrite로 본질해법 후보 격상**(W^X 게이트 v120 PASS). ADR-002는 이를 **M-R1과 동순위 A/B 경쟁자로 채택**. 단 R5의 "seccomp 미경유" kill-shot 산술은 **기각**(트램폴린 종착 svc도 BPF 태움, 주석 L63) → 24ns 바닥은 svc-rewrite에도 잔존, 정성 우위만 유효.
- **[보강·강등]** R3(봉인+path-ALLOW)는 ADR-001 §4 "raw-svc path storm = RET_TRACE 백스톱" 행의 부분 최적화일 뿐 → **보조책으로 명시 강등**.
- **미검증 (a) NEW_LISTENER 가용성**: 진전 = device 프로브 설계 확정(M-R1, socketpair+SCM+1왕복) + 커널 사실 확정(TSYNC 충돌→TSYNC_ESRCH 별도 필터 필요). **여전히 device-only** — WebSearch로 untrusted_app SELinux 명시 허용 근거 못 찾음(Android O는 syscall-number allowlist 기반, 앱 추가필터 허용 사례는 있으나 NEW_LISTENER 플래그 구별 무문서).
- **미검증 (b) 절감 배수**: 진전 = "~10×"는 가설 재확인, WebSearch로 "ptrace보다 효율적"이란 정성 합의만 확보(마이크로초 배수 공개 근거 없음). M-R1 A/B device 실측 전 미확정. R5 정량모델은 USER_NOTIF 낙관 100s/비관 250s(cross-process ioctl 2회+/proc/mem N회 잔존) vs svc-rewrite ~2.9s를 시사하나 **둘 다 미실측 N 의존**.
- **미검증 (c) 흡수 가능 비율**: 진전 = **M-R2가 이걸 device 수치로 직접 답한다**(trace_hist의 비-path nr 빈도 = USER_NOTIF로 흡수할 trap이 애초에 존재하는지). 현 BPF상 비-path가 ALLOW라 비-path trap이 거의 0이면 **M-R1의 ROI가 device 이전에 재평가**된다.

---

## 5. 정직 섹션

**device 측정 전엔 미확정인 것:**
1. **Chromium storm 절대 raw-svc 수(N=5M~50M?)** — 모든 정량모델의 핵심 입력. 공개 strace 출처 없음(R5 자인). M-R2 미실행이면 어느 경로도 usable 단정 불가.
2. **stime~70×의 지배 요인** — 라운드트립 vs chromium 자체 24ns×N. M-R2의 ctxt_switch 역산이 분기하나 신호분리 불완전.
3. **NEW_LISTENER가 untrusted_app SELinux/커널을 통과하는가** (a) — M-R1 device 게이트로만. FAIL이면 후보1 전면 폐기.
4. **USER_NOTIF 실측 절감 배수** (b) — 단일 fd 직렬 RECV가 N스레드 storm에서 새 병목이 될 위험.
5. **`svc`-rewrite가 186MB+수백 .so 전 hot site를 안정 치환** + V8 JIT svc 회피분 — M-R5 device 게이트로만.

**"영영 안 될" 리스크와 그때의 목표 재정의:**
- ADR-001 §1-B의 못 — **seccomp 켜는 한 24ns/syscall은 어떤 모델로도 안 사라진다** — 은 R5 검증으로 svc-rewrite에도 박혀 있음이 확정됐다. 즉 **"syscall 0%/제로 오버헤드"는 chromium storm에 대해 원리적으로 불가**. 이건 리스크가 아니라 확정 사실이다.
- **영영 안 될 시나리오**: M-R2가 (a) 라운드트립이 stime의 소수이고 stime 대부분이 chromium 자체 ALLOW-syscall 24ns×수천만이며, (b) 비-path 흡수 가능 비율이 낮고, (c) N이 너무 커서(예 >50M) 24ns 바닥만으로도 분 단위인 경우 — **USER_NOTIF도 svc-rewrite도 storm을 usable 시간으로 못 끌어내린다.** 이때 두 본질해법은 라운드트립만 줄일 뿐 24ns×N 바닥을 못 넘는다.
- **그때의 목표 재정의**: ADR-001 §4와 plan CP-6 정의("chromium-class usable **또는** USER_NOTIF ADR")에 따라 **CP-6를 "절대시간 usable"에서 "syscall-light/path-heavy 게스트는 0% 달성(이미 PASS), syscall-storm 게스트는 best-effort 가속 + 한계 문서화"로 재정의**한다. 이미 device-PASS인 `chromium-headless-shell --no-sandbox --version`(child exit=0, traps=0)을 "in-process 실행 가능 증명"의 상한으로 인정하고, full DOM 렌더 storm은 "기술적 가능하나 native 대비 큰 슬로다운, seccomp 24ns 바닥이 하한"으로 정직 기록한다. Chromium은 사용자가 보류한 상태이므로(메모리: chromium-native-goal), M-R2/M-R1-a/M-R5-svcscan(평가·프로브)는 보류와 무관하게 진행하되, 실제 본질해법 구현 착수는 사용자 재개 신호 + M-R2 PASS 후로 게이트한다.

**검증에서 낮춘 우선순위 명시**: R3는 holds=uncertain + 현 PCGATE 중복 + 437/439 zygote SIGSYS 미구제로 3순위 보조 강등. R5는 holds=uncertain + kill-shot 산술 오류(정정 후 정성 결론 유지)로 M-R1과 동순위 A/B. R1은 holds=uncertain + (a) device-only로 M-R5와 A/B. **constraint_violations는 4축 모두 없음** — 어느 경로도 비root/in-process/W^X/SELinux 제약을 깨지 않는다(이것만은 확정).

---

## 6. 후속 (R4 보류분)

R4(멀티프로세스 zygote/renderer exec re-entry, Phase C)는 연구는 완료됐으나 적대적 검증 단계가 StructuredOutput 누락으로 실패하여 본 ADR에 정식 반영하지 않았다. exec re-entry는 syscall-storm(이 ADR의 대상)과 **독립적인 별개 벽**이며 `--single-process`로 당분간 우회 중이다. 별도 ADR-003 또는 본 문서 개정으로 다룬다.

---

관련 파일(절대경로):
- 본 ADR: `docs/design/adr-002-chromium-cp6-roadmap.md` (격리 브랜치 `research/chromium-cp6`)
- 선행: `docs/design/adr-001-syscall-overhead-user-notif.md`
- WS-1 코드: `app/src/main/cpp/runtime_report.cpp`, `app/src/main/cpp/alr_interpose/libalr_interpose.c`
- WS-5: `tests/test_pcgate_bpf_logic.py`, `bench/report_parse.py`

Sources: [Android Developers Blog: Seccomp filter in Android O](https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html), [Write SELinux policy | AOSP](https://source.android.com/docs/security/features/selinux/device-policy), [seccomp(2) — Linux manual page](https://man7.org/linux/man-pages/man2/seccomp.2.html), [Seccomp BPF — Linux Kernel docs](https://docs.kernel.org/userspace-api/seccomp_filter.html), [Using seccomp_unotify as an alternative to ptrace](https://medium.com/@mindo.robert1/using-seccomp-user-notifications-seccomp-unotify-as-an-alternative-to-ptrace-for-syscall-1c806a3e2960)
