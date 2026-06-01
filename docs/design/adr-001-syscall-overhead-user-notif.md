# ADR-001 — syscall 중재 오버헤드와 SECCOMP_RET_USER_NOTIF 평가 (CP-6 제로-오버헤드 수렴)

- 상태: **Proposed** (분석/결정 문서, 코드 변경 없음)
- 워크스트림: **WS-1** (L1 CPU 실행/중재) — `docs/research/orchestration-5session-plan.md` §3 WS-1 M2/M3, §6.5, CP-6
- 작성: 2026-06-02, base HEAD `310c759`
- 대상 목표: §0 "오버헤드 제로"의 **(a) CPU 측 ptrace/seccomp 중재 라운드트립**
- 관련: `docs/design/pcgate-seccomp.md`(PCGATE 설계), `docs/design/dynamic-loader-patch.md`(in-process 로더)

---

## 1. 컨텍스트 — 우리가 측정으로 부딪힌 벽

ALR은 glibc arm64 게스트를 **in-process로** 실행한다(`runtime_report.cpp:build_native_loader_probe`가
guest ld.so로 ELF를 맵+점프, `fork`+ptrace supervisor가 감독). syscall 중재는 3겹이다:

1. **LD_PRELOAD interposer**(`alr_interpose/libalr_interpose.c`) — path 계열 9개 syscall
   (openat/openat2/newfstatat/statx/faccessat/faccessat2/readlinkat/mkdirat/unlinkat)을
   문자열 rewrite 후 **단일 `svc #0` 트램폴린**(`alr_tramp_syscall`)으로 직접 발행 → **in-process** 처리.
2. **PCGATE**(PC-gated seccomp, `libalr_interpose.c` ctor + `alr_install_execve_trace_filter`) —
   트램폴린 PC 범위 `[alr_tramp_lo, alr_tramp_hi)`에서 나온 path syscall은 `SECCOMP_RET_ALLOW`(트랩 없음),
   그 밖의 path syscall은 `SECCOMP_RET_TRACE`로 supervisor에 백스톱.
3. **ptrace supervisor**(`build_native_loader_probe`의 waitpid 루프, `PTRACE_EVENT_SECCOMP` 처리, L2054~) —
   RET_TRACE된 syscall을 `/proc/<tid>/mem`으로 x1(pathname) rewrite, TOCTOU-safe. 멀티스레드는
   PTRACE_SEIZE + EVENT_STOP/LISTEN(v124).

### 측정으로 확인된 두 가지 사실 (근거)

**(A) 일반 앱: 라운드트립 = 0, 오버헤드 = 0.**
`docs/evidence/2026-06-01-ws1-m2-cpu-mediation-overhead.md` +
`docs/evidence/2026-06-01-ws5-cpu-overhead-quantified.md`:
모든 일반 게스트가 `traps=0 rewrites=0 pcgate=1 interpose=1`. 일반 CLI(`dynhello`/`env`/`id`/`dash`/`alr-png-test`)
wall-clock **~18-20ms = native 프로세스 수준**. 동일 바이너리(static musl) native(`adb shell`) vs ALR
apples-to-apples:

| mode | native ns/op | ALR ns/op | overhead |
|------|-------------|-----------|----------|
| **compute**(syscall 없음) | 4.06 | 4.06 | **+0.00%** |
| **syscall**(`getpid` loop) | 200.36 | 224.36 | **+11.98%** |

→ syscall-light(§0의 지배적 클래스)는 **이미 0%**.

**(B) syscall storm: ~12% 오버헤드는 BPF가 아니라 seccomp 디스패치 고정비용 — PCGATE 슬림으로 못 줄임.**
`docs/evidence/2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs.md`:
> compute 4.06(=native, 0%), syscall 223.37(슬림 BPF) vs 224.36(구) = **~1ns만 감소**.
> syscall ~12%(24ns)는 BPF instruction 평가가 아니라 **seccomp 디스패치 고정 비용** → BPF 슬림화로 못 줄임.
> **seccomp 켜는 한 syscall 0% 원천 불가.**

즉 24ns/syscall은 트램폴린 PC가 ALLOW로 분류되더라도 커널이 매 syscall마다 BPF 프로그램을 한 번
태우는 진입/분기/리턴 자체의 비용이다. 이건 **모든** in-process seccomp 모델(RET_TRACE, RET_ALLOW,
RET_USER_NOTIF 모두)에 공통으로 남는 바닥이다.

### 진짜 벽 (CP-6의 핵심)

위 ~12%는 `getpid` loop처럼 **interposer가 후킹 가능한 / 혹은 ALLOW로 끝나는** 가벼운 syscall이다.
**Chromium류 syscall-storm**은 다르다(`docs/evidence/2026-06-01-device-SM-X236N-chromium-runs-inprocess.md`):
`--version`은 `traps=0`로 즉시 끝나지만, `--dump-dom about:blank`(V8+렌더)는 431MB RSS, state R,
**stime이 utime의 ~70×** — 즉 거의 전부 커널/중재에 묶여 진행은 하되 심하게 느려진다.

원인 두 갈래:
- **raw `svc`**: Chromium/V8/sandbox는 libc wrapper를 거치지 않고 **직접 `svc`**를 발행한다.
  LD_PRELOAD interposer는 libc 심볼을 가로채는 것이라 **raw `svc`를 후킹할 수 없다**
  (`libalr_interpose.c` 주석 L70-72, plan §3 WS-1 M2). → 트램폴린 PC를 못 타고 비-트램폴린 PC에서
  RET_TRACE → **매 syscall마다 ptrace supervisor 라운드트립**.
- **라운드트립 비용**: RET_TRACE 한 번 = 게스트 스레드 정지 → 커널이 부모를 깨움 → 부모
  `PTRACE_GETREGSET` + (path면) `/proc/<tid>/mem` read/write + `PTRACE_CONT` → 컨텍스트 스위치 2회 +
  syscall 여러 개. 이게 **syscall당 마이크로초 단위**라, 초당 수백만 syscall을 던지는 Chromium에서는
  벽이 된다. plan §3 WS-1 리스크: "in-process ptrace 라운드트립은 아키텍처적 벽".

요약: **§0 (a)의 일반-앱 부분은 이미 달성(0%)**. 남은 것은 **syscall-storm/raw-svc 앱(Chromium류)에서
RET_TRACE 라운드트립을 제거**하는 것. 이것이 CP-6의 단독 미해결 과제다.

---

## 2. 결정해야 할 것

> Chromium류 raw-`svc` syscall-storm 게스트에서, **비root · public Android API · in-process 실행
> 모델**을 유지하면서 per-syscall 중재 라운드트립을 어떻게 줄일 것인가? 어떤 경로를 어떤 단계로
> 추진하고, 어떻게 측정할 것인가?

제약(HARD):
- **비root**: untrusted_app, CAP_SYS_ADMIN 없음, root 없음.
- **public API only**: 비공개/hidden API, SELinux 우회 금지.
- **in-process**: 게스트는 앱 프로세스 안에서 실행(별도 컨테이너/PRoot 에뮬 아님; PRoot는 fallback-only).
- **W^X-safe**, version-stamp 불변(이 문서는 stamp 무관).

---

## 3. 후보 모델 평가

### 후보 1 — SECCOMP_RET_USER_NOTIF (out-of-process supervisor가 notif fd로 syscall 처리)

**아이디어.** seccomp 필터의 action을 RET_TRACE 대신 `SECCOMP_RET_USER_NOTIF`로. 필터 설치 시
`SECCOMP_FILTER_FLAG_NEW_LISTENER` 플래그를 주면 **listener fd**가 반환된다. 트랩된 syscall은 그 fd에
notification으로 쌓이고, 감독자(supervisor)가 `ioctl(SECCOMP_IOCTL_NOTIF_RECV)`로 꺼내 처리한 뒤
`SECCOMP_IOCTL_NOTIF_SEND`로 결과(반환값/errno) 또는 **`SECCOMP_USER_NOTIF_FLAG_CONTINUE`**(커널이 그냥
원래대로 실행)로 응답한다.

**왜 라운드트립이 싼가.** RET_TRACE는 ptrace 정지/재개 + GETREGSET + 레지스터 set + CONT까지 **여러
syscall + 컨텍스트 스위치 2회 이상**이 든다. USER_NOTIF는 RECV/SEND **두 번의 ioctl**과 (보통)
**별도 스레드/프로세스에서 블로킹 대기 중인 감독자**로 끝나, 라운드트립 경로가 짧다. 또한 한 fd로 여러
스레드의 notif를 **배치 처리**할 수 있어 멀티스레드 storm에 유리하다. plan §3 WS-1 M3: "라운드트립 ~10×
절감 후보" — 이 후보가 그 후보다. (정확한 배수는 §5 측정으로 확정. 10×는 가설.)

**비root에서 가능한가? — 가능.**
- `SECCOMP_SET_MODE_FILTER`는 **`PR_SET_NO_NEW_PRIVS`가 켜져 있으면 CAP_SYS_ADMIN 없이** 호출 가능하다.
  zygote는 이미 NO_NEW_PRIVS를 켜고, ALR은 이미 그 위에 **스택 필터를 추가로 설치**한다
  (`libalr_interpose.c` ctor, `alr_install_*_filter`가 `prctl(PR_SET_NO_NEW_PRIVS,1)`로 재확인). 즉
  추가 seccomp 필터 설치 권한은 **이미 실증**되어 있다.
- `SECCOMP_FILTER_FLAG_NEW_LISTENER`는 동일한 `seccomp(SECCOMP_SET_MODE_FILTER, flags, &prog)` 경로의
  플래그일 뿐이고 추가 권한을 요구하지 않는다(NO_NEW_PRIVS 경로에서 listener fd 반환). 커널 ≥ 5.0
  (USER_NOTIF), CONTINUE/ADDFD는 ≥ 5.5/5.9. **디바이스는 Android 16 / 최신 커널**이라 UAPI는 갖춰져 있을
  것으로 보이나, **device 실측으로 listener fd 반환을 1차 게이트**해야 한다(아래 §5 M3-a).
- 단, **Android seccomp 정책과의 양립성**: untrusted_app 기반 zygote 필터가 우리의 추가 필터/플래그를
  막지 않는지, `seccomp(...)` 자체가 zygote 필터에서 SIGSYS로 죽지 않는지 device-검증 필요. (PCGATE가
  이미 ctor에서 stacked filter를 device-성공시켰으므로 `seccomp` 호출 자체는 통과로 추정되나, NEW_LISTENER
  플래그 경로는 별도 확인.)

**in-process 실행 모델과 어떻게 결합하나? — 핵심 난점.**
USER_NOTIF의 의미는 "**트랩한 스레드는 커널 안에서 블로킹**하고, **다른 어떤 컨텍스트**가 fd로 응답한다"이다.
- **응답자가 같은 프로세스의 다른 스레드여도 되는가?** 된다. listener fd만 가지고 있으면 동일 프로세스의
  별도 스레드가 RECV/SEND 할 수 있다. **그러나** Chromium 같은 storm 앱이 **모든** 스레드를 동시에
  트랩시키면, 응답 스레드가 게스트와 같은 프로세스에서 스케줄을 다투고, 게스트가 응답 스레드를 (시그널/
  우선순위로) 굶기면 데드락 위험이 있다. 또 게스트가 응답 스레드의 메모리/시그널을 건드릴 수 있어 격리가 약하다.
- **그래서 권고는 out-of-process 감독자.** ALR의 현재 supervisor는 이미 **`fork`로 분리된 부모
  프로세스**다(in-process로 "맵+점프"하는 것은 자식이고, 감독자는 부모). 따라서 **부모(감독자)에게 listener
  fd를 전달**하면 USER_NOTIF 모델은 현재 fork+supervisor 구조와 자연스럽게 맞는다. "in-process 실행"은
  유지된다 — 게스트 코드 실행은 여전히 자식 프로세스 안에서 in-process로 맵+점프; 바뀌는 것은 **중재 채널**
  뿐이다(ptrace → notif fd).

**FD 전달 / SCM_RIGHTS.**
- listener fd는 **필터를 설치하는 쪽(자식, ctor)에서 반환**된다. 그 fd를 **부모(감독자)로 보내야** 한다.
  - 옵션 (i) **socketpair + SCM_RIGHTS**: fork 전에 `socketpair(AF_UNIX)`를 만들어 두고, 자식 ctor가
    `seccomp(..., NEW_LISTENER)`로 받은 fd를 `sendmsg(SCM_RIGHTS)`로 부모에 전달. 부모는 `recvmsg`로 수신.
    (현재 코드에 go_pipe/SCM 류 동기화가 이미 있어 패턴이 익숙하다.)
  - 옵션 (ii) **`pidfd_getfd`**: 부모가 `pidfd_open(child)` 후 `pidfd_getfd(pidfd, listener_fd_num)`로
    자식의 fd를 복제. 단 자식이 그 fd 번호를 부모에 알려줘야 하고, `pidfd_getfd`는 `PTRACE_MODE_ATTACH`
    권한(같은 uid면 OK)이 필요. ptrace를 이미 쓰므로 권한은 충족.
  - 권고: **(i) SCM_RIGHTS**(가장 이식성 높고 권한 단순) 1차, (ii)는 fallback.

**USER_NOTIF로 "못 하는 것" (한계 — 반드시 인지).**
- **메모리 접근의 TOCTOU**: notif 안에서 syscall 인자(레지스터)는 보이지만, **포인터가 가리키는 게스트
  메모리(예: pathname 문자열)는 notif가 직접 안 준다**. 감독자가 `/proc/<pid>/mem`(또는 pidfd+process_vm_*)
  로 읽어야 한다. 게다가 **표준 USER_NOTIF는 path-rewrite류에 TOCTOU 위험**이 있다: 감독자가 읽은 뒤
  CONTINUE로 커널에 맡기면, 그 사이 게스트가 다른 스레드로 인자를 바꿔치기할 수 있다("CONTINUE는 보안
  필터로 쓰지 말라"는 커널 경고). **결론: path-mediation을 USER_NOTIF+CONTINUE로 옮기는 것은 위험.**
  - 그래서 **권고 분업**(아래 §4): path 9개는 **계속 in-process interposer**가 처리(이미 0 라운드트립);
    USER_NOTIF는 **path가 아닌, raw-svc로 들어오는 비-path syscall**(또는 emulate-해서-결과만 돌려주면 되는
    syscall)에 한정. 비-path syscall은 보통 **인자 자체가 값**이거나 결과만 채우면 되므로 메모리 TOCTOU가
    없거나, ADDFD/`process_vm_writev`로 결과를 쓰면 된다.
- **`SECCOMP_USER_NOTIF_FLAG_CONTINUE`의 보안 한계**: 위 TOCTOU 때문에 confinement 목적엔 부적합. 우리
  목적은 보안이 아니라 **성능(라운드트립 제거)**이므로, "그냥 통과시킬 syscall"엔 CONTINUE가 정확히 맞다 —
  단 그 분류가 path-rewrite를 포함하면 안 된다.
- **fd 주입**: 감독자가 게스트에 fd를 넣어줘야 하는 syscall은 `SECCOMP_IOCTL_NOTIF_ADDFD`로 가능(커널 ≥5.9).
- **블로킹 의미**: 트랩 스레드는 응답까지 **커널에서 블로킹**. 감독자가 죽거나 fd가 닫히면 트랩 syscall은
  **ENOSYS로 풀린다**(커널이 listener 사망 시 처리). 이건 RET_TRACE 백스톱(tracer 사망=게스트 죽음)과 다른
  실패 모드라, 감독자 생존을 강하게 보장해야 한다.

**장점.** 라운드트립 경로 단축(~ioctl 2회) + 멀티스레드 배치(한 fd) + 현재 fork+supervisor 구조와 호환 +
비root 가능(NO_NEW_PRIVS).
**단점.** path-rewrite엔 TOCTOU로 부적합(분업 필요) → raw `svc` storm을 **얼마나** 흡수하는지는 결국
"몇 %의 syscall이 path가 아니면서 USER_NOTIF로 처리 가능한가"에 달림. seccomp **디스패치 고정비용 24ns/
syscall은 그대로 남는다**(§1-B). 즉 USER_NOTIF는 **라운드트립을 줄이지, seccomp 진입비용 0을 만들지 않는다.**

---

### 후보 2 — 부분 정적 패치 / `svc` site의 binary rewriting

**아이디어.** 게스트 바이너리/라이브러리의 `svc #0` 명령어 사이트를 (로드 타임에) 스캔해서 우리 트램폴린
호출로 **rewrite**(또는 점프 패치). 그러면 raw `svc`도 트램폴린 PC를 타게 되어 RET_ALLOW로 in-process 처리.

**장점.** raw `svc`까지 interposer로 흡수 가능(LD_PRELOAD의 근본 한계를 우회). 라운드트립 0.
**단점/제약.**
- **W^X**: 게스트 코드 페이지를 PROT_WRITE로 만들어 패치 → 다시 PROT_EXEC. ALR은 JIT W^X 사이클이
  device-검증됨(`v120-jit-wx-cycle`)이라 가능은 하나, **로드된 모든 .so의 모든 `svc`를 스캔/패치**하는 건
  무겁고, dlopen으로 나중에 로드되는 라이브러리까지 추적해야 한다.
- **PIC/오프셋**: `svc` 앞 명령으로 x8(nr) 세팅을 안정적으로 식별해야 하고, 명령 정렬(arm64 4바이트 고정이라
  유리)·분기 타겟·예외 핸들러를 깨면 안 됨.
- **V8/JIT 코드**: Chromium V8은 **런타임에 코드를 생성**하고 그 안에서 syscall을 거의 안 하지만, sandbox/
  PartitionAlloc 등은 직접 syscall을 한다. **새로 생성되는 코드의 `svc`는 정적 패치로 못 잡는다** → 결국
  seccomp 백스톱이 필요.
- **안정성/이식성 리스크 큼**: 임의 바이너리에 대한 명령어 rewriting은 깨지기 쉽고 디버깅이 어렵다.

**판정.** 강력하지만 고위험. **전면 정적 패치는 비권고**. 단, **좁은 변형**(아래 §4 보조)으로 가치 있음:
"가장 빈번한 hot `svc` 사이트(libc 내부의 vsyscall-bypass, V8 sandbox의 알려진 site) 몇 개만 핀포인트 패치"는
ROI가 있을 수 있다 — 측정으로 hot site를 먼저 찾은 뒤.

---

### 후보 3 — vDSO / seccomp data 기반 우회

**아이디어 (3a) vDSO**: `clock_gettime`/`gettimeofday`/`getcpu`/`time` 등은 커널이 vDSO로 노출해
**syscall 자체가 안 일어난다**(유저공간에서 읽음). 게스트가 vDSO를 제대로 쓰면 이 부류는 애초에 트랩도
seccomp 진입도 없다 → **24ns 고정비용도 0**.
**아이디어 (3b) seccomp data 기반 분기 최적화**: 현재 PCGATE BPF는 이미 nr/IP를 정밀 분류한다
(`libalr_interpose.c` L463-505, host에서 `tests/test_pcgate_bpf_logic.py`로 검증). 추가로 "확정적으로
무해해서 트랩 자체가 불필요한 syscall"을 BPF 단계에서 더 많이 RET_ALLOW로 빼면 RET_TRACE 빈도가 준다.
하지만 §1-B에서 보았듯 **ALLOW로 분류해도 seccomp 진입 24ns는 남는다** → 라운드트립은 줄지만 진입비용은 못 줄임.

**장점.** vDSO 경로(3a)는 **유일하게 seccomp 진입비용까지 0**으로 만드는 길(해당 syscall 부류 한정).
저위험.
**단점.** 적용 범위가 **vDSO가 커버하는 소수 syscall(주로 시계류)**로 한정. Chromium storm의 본체
(futex/epoll/read/write/mmap/ioctl)는 vDSO에 없다 → storm 자체는 못 줄임.
**판정.** **저비용 보조로 채택**(§4): 게스트가 vDSO를 확실히 쓰도록 보장(ld.so가 AT_SYSINFO_EHDR를 받게
+ glibc clock_gettime이 vDSO 경로를 타는지 device 확인). storm 본질 해법은 아님.

---

### 후보 4 — 현행 PCGATE 확장 (in-guest interposer로 더 많은 syscall in-process 처리)

**아이디어.** interposer가 후킹하는 libc 심볼을 path 9개에서 확장 — 예: `realpath`/`opendir`/`scandir`처럼
지금 libc 내부 경로로 RET_TRACE 백스톱을 타는 호출, 그리고 자주 쓰이는 비-path syscall wrapper를 트램폴린
경유로 묶어 RET_ALLOW화. plan §3 WS-1 M2가 정확히 이것.
**장점.** 기존 메커니즘 확장이라 **저위험·점진적**. 라운드트립 0(트램폴린 경유분). 이미 검증된 PC-gate +
host BPF 로직 테스트(`tests/test_pcgate_bpf_logic.py`) 인프라 재사용.
**단점/한계.** **libc wrapper를 거치는 호출만** 잡는다. Chromium/V8/sandbox의 **raw `svc`는 여전히 못 잡음**
(후보 1/2의 영역). 또 seccomp 진입 24ns 고정비용은 남음. 즉 **일반 GUI 앱(GTK/Qt) 폭 확대엔 효과적이나
Chromium storm 벽 자체는 못 깸**.
**판정.** **즉시 착수 가능한 저위험 기반작업으로 채택**(M2). raw-svc storm은 별도(M3=후보1).

---

## 4. 결정 (권고)

**원칙: §0 (a)는 클래스별로 다른 해법을 쓴다 — 하나의 은탄환은 없다.**

| syscall 클래스 | 권고 처리 | 라운드트립 | seccomp 진입 24ns |
|---------------|-----------|-----------|------------------|
| compute(syscall 없음) | 그대로 | 0 (이미) | 없음 (이미 0%) |
| path 9개 (libc 경유) | **interposer 트램폴린**(현행) | 0 (이미) | 남음 |
| vDSO 가능(clock류) | **vDSO 보장**(후보 3a) | 없음 | **0** |
| path가 아닌 libc-wrapped | **PCGATE 확장**(후보 4, M2) | 0 | 남음 |
| **raw `svc` 비-path storm (Chromium)** | **USER_NOTIF out-of-process 감독자**(후보 1, M3) | **~크게 절감** | 남음 |
| raw `svc` path storm | interposer 못 잡음 → RET_TRACE 백스톱 유지(TOCTOU 안전) | 라운드트립 있음 | 남음 |

**채택 경로(우선순위):**
1. **후보 4 (PCGATE 확장)** — 저위험·즉시. 일반 앱 폭 확대. raw-svc는 못 잡음.
2. **후보 3a (vDSO 보장)** — 저비용 보조. 시계류 진입비용 0.
3. **후보 1 (USER_NOTIF)** — **Chromium storm의 본질적 해법 후보.** 단 **path-rewrite는 옮기지 않는다**
   (TOCTOU). 비-path / 결과-only syscall에 한정. out-of-process 감독자 = 현 fork supervisor 확장.
4. **후보 2 (정적 patch)** — **전면 비권고.** 측정으로 hot `svc` site가 소수로 확인되면 그때만 좁게.

**근거 요약:**
- §0 (a)의 **지배적 클래스(syscall-light)는 이미 0%** — 추가 작업 불요. ADR의 무게중심은 storm 한정.
- USER_NOTIF는 비root 가능(NO_NEW_PRIVS, 이미 stacked filter 실증)하고 현 fork supervisor와 구조적으로 맞다.
- 단, **seccomp 디스패치 24ns/syscall 바닥은 어떤 모델로도 안 사라진다**(§1-B). 따라서 "syscall 0%"는
  **seccomp가 켜진 한 원리적으로 불가**하고, 현실 목표는 "**라운드트립 제거로 storm을 usable화**"(§0 목표가
  Chromium엔 "<5%"가 아니라 "usable"인 것과 일치)이다.
- **Chromium은 사용자가 "보류"한 상태**(메모리: chromium-native-goal, plan §1 L4). 이 ADR은 **기술 경로만
  제시**하고 구현 착수는 사용자 재개 신호 후. M2/M3-a(평가/프로브)는 보류와 무관하게 진행 가능.

---

## 5. 측정 기준 (마일스톤 게이트 — 측정 없이 완료 주장 금지)

기준선과 단위는 §1의 기존 evidence를 그대로 사용한다:
- raw syscall 단위: `getpid`/`getppid` ~200-224 ns/op (device).
- seccomp 진입 고정비용: ~24 ns/syscall (PCGATE on, ALLOW 분류). **하한**으로 명시.
- 일반 CLI wall-clock: ~18-20ms (native 수준).
- 도구: `python -m bench overhead --native-ns ... --alr-ns ... --storm`(WS-5 harness),
  `run_perf_comparison`(`runtime_report.cpp`, logcat `alr perf ...`), per-guest `traps/rewrites/exec_ms`.

**신규 마이크로벤치(syscall-storm, 평가용 — WS-5와 포맷 협의):**
- **storm 마이크로벤치**: tight loop로 (a) `getpid`(libc-wrapped, 후킹 가능), (b) **raw `svc` getpid**
  (inline asm, interposer 우회 — Chromium의 대리), (c) path syscall, (d) clock_gettime(vDSO 대상) 각각 N회.
  각 모드 native(adb shell) vs ALR wall-clock/ns-per-op, 그리고 `traps` 카운터. 이게 **(b)에서의 라운드트립
  비용을 직접 노출**한다.
- **A/B 모드**: 현행(RET_TRACE 백스톱) vs USER_NOTIF 프로토타입 — 동일 (b) raw-svc storm에서 ns/op 비교.
  목표 게이트: **USER_NOTIF가 RET_TRACE 대비 라운드트립 ≥ 한 자릿수 배수 절감**(plan §3 M3 "~10×" 가설을
  실측으로 확정/반증). 절감이 가설보다 작으면 후보 1의 우선순위를 재평가.

**마일스톤(측정 가능):**
- **M2 (후보 4·즉시, 보류 무관)**: PCGATE를 비-path libc-wrapped syscall로 확장. 게이트: 대표 GUI 게스트
  (gtk3-widget-factory/foot/gimp)의 `traps` 카운트 감소(회귀 0, host pytest green). raw-svc는 불변 예상.
- **M3-a (USER_NOTIF 가용성 프로브 — 코드 최소, device-필요)**: 자식 ctor에서
  `seccomp(SET_MODE_FILTER, NEW_LISTENER, &allow_prog)` 시도 → listener fd 반환 여부 + SCM_RIGHTS로
  부모 수신 여부 + 1건 RECV/SEND(CONTINUE) 왕복. 게이트: device에서 **listener fd 획득 + 1 왕복 PASS**.
  실패 시(커널/Android 정책) 후보 1 폐기, 후보 2 hot-site 재평가.
- **M3-b (USER_NOTIF 프로토타입 — 비-path storm)**: 부모 감독자가 notif fd로 (b)-class syscall을
  CONTINUE/emulate. 게이트: storm 마이크로벤치 (b)에서 RET_TRACE 대비 ns/op 절감 배수 측정 + 회귀 0.
- **M3-c (vDSO 보장, 후보 3a)**: 게스트 clock_gettime이 vDSO 경로(트랩 0)인지 device 확인. 게이트:
  clock_gettime storm에서 `traps=0` + native와 동률.
- **M4 (Chromium, 사용자 재개 시)**: `--headless --dump-dom about:blank`가 **usable 시간 내 완료**
  (stime/utime 비율 개선을 RSS/state R 진행과 함께 evidence화). plan CP-6 = "chromium-class usable 또는
  USER_NOTIF ADR" — 이 문서가 후자, M4가 전자.

device 검증은 plan §9 단일 게이트(통합 세션)로만. 이 ADR 자체는 host 빌드 불요(문서).

---

## 6. 합의/미합의 + 후속

- **합의(이 ADR의 결정):** 클래스별 분업(§4). USER_NOTIF는 **비-path raw-svc storm 한정**, **path는 절대
  USER_NOTIF로 안 옮김**(TOCTOU). 전면 정적 패치 비권고. vDSO·PCGATE확장은 저위험 보조.
- **미합의/측정 대기:** (i) device에서 NEW_LISTENER가 untrusted_app 정책을 통과하는가(M3-a), (ii) 라운드트립
  실측 절감 배수(M3-b)가 "~10×" 가설에 부합하는가, (iii) Chromium storm 중 USER_NOTIF로 흡수 가능한
  syscall 비율.
- **후속(이 ADR을 구현하려면 WS-1이 할 다음):**
  1. **M2 PCGATE 확장**부터 — `libalr_interpose.c`에 비-path libc wrapper 추가 + 트램폴린 경유. host BPF
     로직 테스트(`tests/test_pcgate_bpf_logic.py`) 확장은 WS-5와 협의(tests는 WS-5 소유).
  2. **storm 마이크로벤치** 설계를 WS-5에 핸드오프(raw-svc inline asm 모드 포함; tests/bench는 WS-5 소유).
  3. **M3-a 가용성 프로브**를 `runtime_report.cpp`의 자식 ctor 근처에 최소 구현(NEW_LISTENER 시도 + SCM_RIGHTS
     전달 + 1 왕복) → `DEVICE-REQ`로 통합 세션 큐에 1차 게이트.
  4. 결과에 따라 M3-b 프로토타입(부모 감독자 RECV/SEND 루프; 현 waitpid 루프와 별 채널로 공존) 또는 후보 폐기.
- **소유권 주의:** 구현 시 `runtime_report.cpp`(WS-1)·`libalr_interpose.c`(WS-1)는 WS-1 소유라 가능하나,
  `tests/`·bench·`docs/evidence/`는 **WS-5 소유**이므로 인터페이스/포맷 협의 후 핸드오프(plan §2/§6).
