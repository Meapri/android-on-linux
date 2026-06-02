> 생성 경위: R4(멀티프로세스 exec re-entry, Phase C) 심층연구 + 자가 적대검증 산출. ADR-002 §6이 명시 보류한 R4 후속분을 정식 ADR로 승격. 레포 읽기 전용(`/Users/naen/Documents/alr-research-cp6`), `main`·`ws-N` 미변경. 코드 사실은 `app/src/main/cpp/runtime_report.cpp`(supervisor/seccomp/exec trap) 직접 확인, 커널 사실은 WebSearch 교차검증. 리뷰 대상: WS-1(`runtime_report.cpp`/`libalr_interpose.c`), WS-5(`tests/`/`bench/`), 통합 세션.

# ADR-003 — Chromium 멀티프로세스 exec re-entry (Phase C): "loader 재진입" 기각, "환경+seccomp 보존 + SEIZE 상속" 채택

> ⚠️ **이 v1 본문(§1–§7)의 핵심 주장은 device(v139 drain#17)가 반증했다 — `§8 ADR-003-v2`가 이를 대체(SUPERSEDED)한다.** v1은 "loader 재진입 불요: execve 트랩에서 path만 rootfs로 rewrite하면 seccomp+SEIZE 상속으로 새 이미지가 자동으로 매개된다"고 결론냈다. 그러나 device 측정에서 **모든 exec에 걸쳐 `exec_events=0`** — `PTRACE_EVENT_EXEC`가 단 한 번도 발생하지 않았다. 근본 원인: 커널이 glibc ELF를 execve하려면 그 `PT_INTERP`(게스트 `/lib/ld-linux-aarch64.so.1`)를 resolve해야 하는데 **Android 커널은 게스트 ld.so를 resolve할 수 없어 커널-execve 자체가 실패** → 새 이미지가 실행에 진입하지 못한다(seccomp/SEIZE 상속을 "물려줄 새 이미지"가 애초에 안 생긴다). 즉 v1이 명시 보류한 "loader 재-map" 이 진짜 필요분이었다. v1 §1–§7은 **자식 클래스 분류(A: zygote-fork=clone vs B: fresh-execve)** 라는 여전히 유효한 분석으로 보존하되, "(B)는 path-rewrite만으로 자동 매개" 부분은 §8로 대체한다.

- 상태: **SUPERSEDED-by-§8** (v1 본문) — R4 심층연구 + 자가 적대검증. 코드 변경 없음, device가 핵심 가정 반증.
- 워크스트림: **WS-1**(`runtime_report.cpp`·`libalr_interpose.c`) 주관, **WS-5**(tests/bench/docs/evidence) 협의.
- 작성: 2026-06-02. 선행: `docs/design/adr-002-chromium-cp6-roadmap.md` §6(이 문서가 보류분을 승계), `docs/design/adr-001-syscall-overhead-user-notif.md`.
- 대상: ALR loader는 single map+jump(`alr_enter_guest`, `runtime_report.cpp` ~L1147)라 게스트 fork→execve 자식은 커널이 새 ELF로 주소공간 교체 → loader 재호출 안 됨. chromium zygote/gpu/utility의 멀티프로세스 모델에서 이 자식들을 다시 중재 하에 두는 문제.
- HARD CONSTRAINTS(불변): 비root(untrusted_app, no CAP_SYS_ADMIN), public Android API only, SELinux 우회 금지, W^X-safe, in-process(PRoot fallback-only), device evidence 없이 완료 주장 금지, version stamp 불변.

---

## 1. 한 줄 결론

**exec re-entry는 "loader를 다시 map+jump"하는 문제가 아니다 — chromium의 두 자식 클래스를 분리하면 (A) zygote-fork 자식(renderer 대다수)은 execve를 아예 안 거치므로 이미 매개된 주소공간 + 상속된 seccomp 필터 + SEIZE-상속 trace로 _자동으로_ 중재 하에 있고, (B) browser가 fresh-execve하는 자식(zygote 자신·gpu·utility)만 진짜 exec 벽이며, 이건 (a) seccomp 필터가 execve로 보존되고(커널 확정) (b) supervisor가 `PTRACE_O_TRACEEXEC`로 자식을 자동 추적하므로(커널 확정) "loader 재진입" 없이 풀린다. 단 _전 설계가 단 하나의 미검증 가정에 매달려 있다_: execve의 envp는 커널이 자동 보존하지 않고 _호출자(=chromium의 process launcher)가 직접 구성_하므로, ALR이 push한 `LD_PRELOAD`/`ALR_ROOTFS`/`LD_LIBRARY_PATH`가 자식 envp로 전파되는지는 chromium 런처 코드와 device가 함께 답해야 한다. 전파되면 interposer가 자동 재주입되어 (B)도 완전 매개, 안 되면 (B)는 seccomp/ptrace 백스톱만 남아 path-mediation은 되나 interposer 가속/W^X .so 로딩이 죽는다.** 따라서 R4의 현실 목표는 "모든 chromium 프로세스 완전 in-process"가 아니라 **"`--single-process --no-zygote`로 exec 벽 자체를 0으로 만든 상태를 1차 usable 기준선으로 삼고, 멀티프로세스는 exec 자식 분류별 device 프로브로 어디까지 자동 상속되는지 측정"** 이다.

---

## 2. exec re-entry 설계 (자식 클래스 분리)

핵심 통찰: chromium은 자식을 한 가지 방법으로 안 띄운다. 커널/chromium 문서 확정 분류:

| 자식 클래스 | 띄우는 법 | execve 거치나 | ALR 중재 상태 |
|---|---|---|---|
| **renderer**(다수) | zygote가 **fork**(clone, no exec) | **아니오** | 주소공간이 zygote의 매개된 사본 → 이미 매개 + seccomp 상속 + SEIZE 상속 |
| **zygote 자신** | browser가 **fresh execve** | 예 | (B) exec 벽 — TRACEEXEC 필요 |
| **gpu process** | browser가 **fresh execve**(zygote 안 거침, 문서 확정) | 예 | (B) exec 벽 |
| **utility/network** | 대개 zygote-fork(2계층 zygote), 일부 unsandboxed zygote | 혼합 | fork분은 (A), exec분은 (B) |

### (A) zygote-fork 자식 — "공짜로 매개됨" (loader 재진입 불요)
- zygote는 `alr_enter_guest`로 이미 in-process 진입했고 그 시점에 (i) 게스트 glibc가 interposer와 함께 적재된 주소공간, (ii) `alr_install_*_trace_filter`로 스택된 seccomp 필터(`runtime_report.cpp` L1165/L1219), (iii) supervisor의 `PTRACE_SEIZE` + `PTRACE_O_TRACEFORK|VFORK|CLONE`(L1913) 하에 있다.
- renderer는 이 zygote를 **clone**한다. clone은 주소공간을 그대로 복제(또는 CoW 공유) → **interposer 코드·LD_LIBRARY_PATH·rootfs 매핑이 그대로 따라온다. execve가 없으니 envp 재구성 문제 자체가 발생하지 않는다.** 이게 R4 가설의 가장 강한 부분: renderer 대다수는 _아무 추가 작업 없이_ 이미 완전 매개.
- supervisor 측: `PTRACE_O_TRACECLONE`이 새 renderer를 `PTRACE_EVENT_STOP`으로 자동 attach(L2176~ 핸들러가 이미 처리: 신규 tid면 CONT, group-stop이면 LISTEN). seccomp 필터도 clone으로 상속 → renderer의 path syscall도 EVENT_SECCOMP로 트랩. **현 코드가 이미 이 경로를 device-증명**했다(메모리: Chromium --dump-dom 22 threads 다중-tracee 핸들링).

### (B) fresh-execve 자식 — TRACEEXEC로 재포착 (이것만 신규 작업)
exec 벽의 본질은 "loader 재호출이 안 됨"이지만, _loader가 한 일 중 execve를 넘어 보존되는 것과 안 되는 것을 분리_하면 재호출이 불필요함이 드러난다:

1. **seccomp 필터 — 보존됨(커널 확정).** "If execve is allowed, the existing filters will be preserved across a call to execve" (man7 seccomp.2). NO_NEW_PRIVS는 zygote가 이미 세팅. → 새 ELF(gpu process 바이너리)는 **자동으로 ALR의 path-trap/execve-trap 필터 하에서 시작**한다. loader가 매번 `alr_install_*_filter`를 다시 부를 필요가 없다.
2. **trace 상속 — 보존됨(커널 확정).** SEIZE된 tracee가 execve하면 tracer는 그대로 유지되고 `PTRACE_O_TRACEEXEC`로 `PTRACE_EVENT_EXEC` stop이 발생(execve 반환 _전_). supervisor 루프(L2244 `if (event != 0)`)가 이 이벤트를 잡아 그냥 CONT하면 새 이미지가 traced+filtered 상태로 실행. → path syscall은 여전히 EVENT_SECCOMP로 트랩되어 rootfs로 rewrite됨.
3. **그래서 "재진입"의 정체는**: 새 ELF를 ALR execmem에 map+jump하는 게 _아니라_, (1)(2)가 보장하는 "필터+trace가 살아있는 채로 커널이 정상 적재한 새 이미지"를 supervisor가 EVENT_EXEC에서 인지하고 path-mediation handler를 계속 돌리는 것뿐이다.

### supervisor의 신규 작업 3가지(전부 기존 trap 사이트 확장, 신규 ptrace op/권한 0)
- **(B-1) execve _인자_ path mediation — x0 rewrite(§3 별도).** 현 핸들러는 path를 **x1=regs[1]**에서 읽는다(L2073). execve의 path는 **x0=regs[0]**, x1은 argv(char**), x2는 envp. 코드가 이미 `is_exec`면 x1 rewrite를 _건너뛴다_(L2064-2068 주석 자인). 신규: `is_exec`일 때 **regs[0]을 읽어** rootfs 밖 절대경로(`/usr/lib/chromium/...`)를 rootfs host path로 rewrite. argv/envp는 절대 안 건드림.
- **(B-2) EVENT_EXEC 처리 명시화.** 현재 L2244의 generic `event != 0` CONT로 우발적으로 처리되나, `PTRACE_EVENT_EXEC`에서 `PTRACE_GETEVENTMSG`로 former tid를 읽어 known_tids/mem_fds 캐시를 _무효화_(exec 후 /proc/<tid>/mem 매핑이 완전히 바뀜 → 기존 stale-fd evict 로직 L2087이 이미 ESRCH/EIO로 잡지만, exec은 명시적 evict가 더 견고)해야 한다.
- **(B-3) interposer 자동 재주입은 envp 전파에 _의존_(§3·§4의 핵심 미검증).** seccomp/ptrace는 (B)에서 자동 살아남지만, interposer(`libalr_interpose.so`)는 _새 ELF의 ld.so가 LD_PRELOAD를 읽어야_ 재주입된다. LD_PRELOAD는 **커널이 보존하지 않고 chromium 런처가 envp에 다시 넣어줘야** 한다(§4-가정-1).

---

## 3. execve 인자 path mediation 설계 (x0, rootfs 밖 절대경로)

- **레지스터 정정**: aarch64에서 `execve(path, argv, envp)` = (x0, x1, x2). 현 supervisor의 "x1 = pathname" 가정(L1167 주석, 9개 *at-style용)은 execve엔 틀리다. → exec 트랩 분기에서만 `path_addr = regs[0]`.
- **읽기/쓰기 기법은 기존과 동일·증명됨**: 캐시된 `/proc/<tid>/mem` O_RDWR fd로 pread(x0 가리키는 path) → `translate_rootfs_path`(이미 pure·memoized, L2124) → 스택 scratch에 pwrite → `regs[0] = scratch` → SETREGSET. TOCTOU-safe(트레이시는 syscall-entry에서 frozen).
- **rootfs 밖 절대경로 처리**: chromium은 `/usr/lib/chromium/chrome` 또는 `/proc/self/exe`를 exec한다.
  - rootfs 매핑 절대경로(`/usr/lib/...`) → 기존 `translate_rootfs_path`로 `<rootfs>/usr/lib/...` rewrite. **단 그 host 경로의 ELF가 다시 glibc-arm64라야** 새 이미지도 동일 필터 하에 돈다(같은 rootfs이므로 성립).
  - `/proc/self/exe` exec(chromium이 자기 재실행 시) → `/proc/self/exe`는 rootfs 밖 가상 심볼릭이고 현 코드가 `/proc`를 일부러 rewrite 제외(L2107 `sysdir`). 이 경우 커널은 _현재 매핑된 실제 바이너리_를 exec → ALR execmem에 매핑된 게스트가 아니라 **bionic이 적재한 host 바이너리(APK 내 .so)** 를 가리킬 위험. **→ chromium이 `/proc/self/exe`를 쓰면 exec 대상이 ALR 게스트가 아니게 되어 설계가 깨진다. 이건 device로 확인할 미검증점(§4-가정-3).**
- **argv[0] 불변**: argv는 x1(char**)이고 절대 안 건드림. argv[0]이 가상 경로여도 path(x0)만 host로 바뀌면 커널은 host ELF를 적재, argv[0]은 게스트가 보는 이름 유지(정상).

---

## 4. 미검증 가정 (device 없이는 못 닫음)

- **[가정-1·전 설계의 급소] execve envp 전파.** 커널은 execve로 환경을 자동 보존하지 않고 _호출자가 envp를 구성_한다(execve 의미론 확정). chromium의 process launcher(`base::LaunchProcess`/`ChildProcessLauncher`)가 자식 envp를 만들 때 부모의 `LD_PRELOAD`/`LD_LIBRARY_PATH`/`ALR_ROOTFS`/`ALR_PCGATE`를 **passthrough 하는지 clear 하는지**가 (B)의 interposer 재주입 성패를 가른다. WebSearch 확정 사실: "zygote 때문에 browser의 환경변수가 renderer에 상속 안 되는 알려진 버그/보안 설계"가 있다(chromium-dev). → **chromium은 환경을 의도적으로 거를 가능성이 높다.** 전파 실패 시 (B)는 seccomp+ptrace 백스톱만 남는다(path-mediation은 살되 interposer 가속/W^X .so 로딩은 죽음).
- **[가정-2] AT_SECURE 미발동.** 게스트 exec이 어떤 경로로든 ld.so의 secure-execution mode(AT_SECURE=1)를 트리거하면 "슬래시 포함 LD_PRELOAD는 무시, LD_LIBRARY_PATH 무시"(ld.so.8 확정). ALR의 LD_PRELOAD는 절대 rootfs 경로(슬래시 포함, R3 규칙)라 AT_SECURE 하에선 **즉시 무력화**. untrusted_app 비-setuid exec이면 보통 AT_SECURE=0이나, Android 도메인 전이/seccomp 조합에서의 거동은 device-only.
- **[가정-3] `/proc/self/exe` exec 비사용.** chromium이 자식을 `/proc/self/exe`로 재실행하면 exec 대상이 ALR 게스트 매핑이 아니라 host 적재 이미지를 가리킬 수 있어(§3) 설계가 깨진다. 실제 chromium이 절대 바이너리 경로를 쓰는지 `/proc/self/exe`를 쓰는지 device strace로만 확정.
- **[가정-4] untrusted_app SELinux가 자식-exec 후에도 SEIZE-trace를 유지.** ALR supervisor가 _자기 fork 자식_을 ptrace하는 건 device-증명됨(GIMP/chromium --version). 그러나 그 자식이 _자기 execve_로 도메인이 미묘하게 바뀌거나 새 이미지가 `execmem` perms를 요구할 때 SELinux가 trace/exec를 막는지는 미확정. ptrace 권한은 attach 시 1회 체크되고 SEIZE-상속 tracee는 재-attach 안 하므로 _아마_ 통과하나, Android per-app seccomp + 자기-exec 바이너리의 도메인 전이 거동은 device-only.
- **[가정-5] 2계층 zygote / unsandboxed zygote.** chromium은 sandboxed zygote가 unsandboxed 자식을 못 띄우는 문제로 _두 번째 unsandboxed zygote_를 browser가 별도 exec한다(문서 확정). 이 두 zygote가 각각 (B) exec 벽이고, unsandboxed zygote가 ALR seccomp 필터를 어떻게 물려받는지(또는 거부하는지)는 미측정.

---

## 5. device 프로브 + DEVICE-REQ 마커

### M-R4-fork (1순위, 즉시 ROI — `--single-process --no-zygote` 기준선 확정)
- **무엇**: exec 벽을 _아예 만들지 않는_ 구성으로 chromium을 in-process 실행해, R4 문제 없이 도달 가능한 usable 상한을 못 박는다. exec 자식이 0이면 (A)/(B) 분류 자체가 불필요 → ADR-002의 syscall-storm(독립 벽)만 남는다.
- **DEVICE-REQ 마커**: `DEVICE-REQ: ALR-M-R4-singleproc — SM-X236N (am force-stop first), chromium-headless-shell --no-sandbox --single-process --no-zygote --headless --dump-dom about:blank ×1; expect child exit=0 AND logcat 'alr_loader'에 execve trap 0건(EVENT_EXEC 미발생); gate = exit=0 AND exec-자식 0. 외부 strace 금지(supervisor-내부 집계).`

### M-R4-execmap (2순위, exec 자식 분류 계측 — read-only)
- **무엇**: 멀티프로세스(zygote 살림)로 띄우되 supervisor에 **EVENT_EXEC/EVENT_CLONE 카운터 + 각 exec의 x0 path 1줄 로깅**만 추가(기존 trap 사이트 확장, 신규 ptrace op 0). renderer(clone)=N, gpu/zygote(exec)=M 비율과 _exec된 절대경로_(rootfs 안/밖, `/proc/self/exe` 여부 §4-가정-3)를 device 수치로 확정.
- **DEVICE-REQ 마커**: `DEVICE-REQ: ALR-M-R4-execmap — SM-X236N (am force-stop first), chromium-headless-shell --no-sandbox --headless --dump-dom about:blank ×1(zygote 살림); capture 'alr exec clone_events=<n> exec_events=<m>' + 각 exec의 'alr exec x0=<path>'; gate = 로깅 present AND 분류표 작성 가능(clone분=자동매개 / exec분=path 분포). child exit은 비-gate(멀티프로세스는 깨질 수 있음, 분류가 산출물).`

### M-R4-envprop (3순위·급소 검증 — interposer 재주입 가부)
- **무엇**: (B) exec 자식 안에서 `getenv("ALR_ROOTFS")`/`getenv("LD_PRELOAD")`가 살아있는지를 interposer ctor가 1줄 로깅. 살아있으면 §4-가정-1 성립(interposer 자동 재주입 OK), 죽었으면 chromium이 환경을 걸렀다는 device 증거 → (B)는 백스톱-only로 재정의.
- **DEVICE-REQ 마커**: `DEVICE-REQ: ALR-M-R4-envprop — SM-X236N, chromium 멀티프로세스 exec 자식의 interposer ctor에서 'ALR-ENVPROP ld_preload=<0|1> alr_rootfs=<0|1> at_secure=<0|1>' 로깅; gate = 로깅 present. ld_preload=0 ⇒ §4-가정-1 반증(interposer 재주입 불가→(B) seccomp/ptrace 백스톱-only). at_secure=1 ⇒ §4-가정-2 반증(LD_PRELOAD 무력).`

(주의: M-R4-execmap/envprop는 _계측·프로브_로 chromium 사용자 보류(메모리: chromium-native-goal)와 무관히 진행 가능. 실제 (B-1) x0-rewrite **구현 착수**는 M-R4-execmap이 "exec 자식이 rootfs 안 절대경로를 쓴다(=가정-3 통과)"를 보인 _후_로 게이트.)

---

## 6. host 프로토타입 가능범위 (device 불요, WS-5 협의)

- **(P1, M-R4-execmap용)** `bench/exec_map.py`(신규): `clone_events`/`exec_events`/`exec x0=<path>` 라인 파싱 → exec path를 `{rootfs-내 절대, rootfs-외 절대, /proc/self/exe, 상대}`로 분류 + clone:exec 비율을 markdown. `bench/report_parse.py`에 `_CLONE_EVENTS/_EXEC_EVENTS/_EXEC_X0` 정규식.
- **(P2, B-1용)** `tests/test_execve_pathrw.py`(신규): execve 트랩의 **x0(regs[0]) vs path 9-syscall의 x1(regs[1])** 분기 결정모델을 순수함수로 — `is_exec`면 regs[0] 읽고 argv(regs[1])/envp(regs[2]) 절대 불변, `/proc/self/exe`·`/proc/*`는 rewrite 제외, rootfs-내 idempotency guard(L2114) 적용 — 을 0-mismatch로. host에서 fixture 레지스터 배열로 검증(실 ptrace 불요).
- **(P3, 공통)** chromium 프로세스 그래프 모델(browser→exec[zygote,gpu,unsandboxed-zygote], zygote→clone[renderer×N])을 harness에 인코딩 → M-R4-execmap 실측이 들어오면 "자동매개(clone)분 / exec-벽(exec)분" 즉시 산정. **단 host(darwin)는 실커널 seccomp-across-execve/SEIZE-EVENT_EXEC 거동 불가** → 분류 로직 회귀만, 상속 효과는 device-only.
- **(P4, 가정-1용)** `tests/test_env_passthrough.py`: chromium `base::LaunchOptions`의 `clear_environment`/`environment` 머지 규칙을 _공개 소스 기준_ 모델링해 "ALR env가 자식에 살아남는 조건"을 문서화(실 chromium 빌드 불요, 코드 독해 기반 가설 → M-R4-envprop이 device로 확정/반증).

---

## 7. 정직 섹션

**독립 / 겹침 (ADR-002 syscall-storm 대비):**
- **독립**: R4(exec 벽)와 ADR-002(syscall-storm)는 _서로 다른 벽_이다. exec 벽은 "자식 프로세스를 매개 하에 두느냐"의 문제고, syscall-storm은 "매개 하에 둔 뒤 syscall당 24ns 바닥/라운드트립"의 문제다. R4가 100% 풀려도 storm은 그대로고, storm이 0이어도 멀티프로세스 exec 자식은 여전히 R4 처리가 필요. ADR-002 §6이 "독립적인 별개 벽"이라 한 것과 정합.
- **겹침(1점)**: 둘 다 **`--single-process --no-zygote`로 _동시에_ 우회**된다 — exec 자식이 0이면 R4 벽이 사라지고, 단일 프로세스면 storm도 한 주소공간에 집중(분산 안 됨). 그래서 M-R4-fork(§5)가 _두 ADR 공통의 1차 기준선_이다. **우선순위**: 사용자가 chromium 멀티프로세스 완전체를 요구하기 전까지는 `--single-process` 기준선 + R4 _프로브_(execmap/envprop, read-only)만 진행하고, (B-1) x0-rewrite 등 실 구현은 M-R4-execmap PASS(가정-3 통과) + 사용자 재개 신호 후로 게이트. ADR-002의 M-R2(storm 분해)와 R4의 M-R4-execmap은 _같은 chromium 1회 실행_에서 동시 계측 가능(둘 다 supervisor-내부 집계, hot-loop 오염 0) → 한 device 세션으로 두 ADR 입력을 동시 확보.

**device 측정 전엔 미확정인 것:**
1. **execve envp 전파(가정-1)** — 전 (B) 설계의 급소. chromium이 ALR env를 거르면 interposer 자동 재주입 불가.
2. **chromium이 `/proc/self/exe`를 exec하는가(가정-3)** — 그렇다면 exec 대상이 게스트 아닌 host 이미지가 되어 §3 설계가 깨진다.
3. **AT_SECURE 발동 여부(가정-2)** — 발동 시 절대경로 LD_PRELOAD 무력.
4. **자식-exec 후 SELinux가 SEIZE-trace/execmem 유지(가정-4·5)** — 2계층 zygote 포함.

**"영영 안 될" 리스크와 그때의 목표 재정의:**
- **자가 적대검증(핵심 주장 1개를 자기공격)**: 본 ADR의 §1 주장 — "loader 재진입 불요, seccomp+trace 상속으로 충분" — 을 공격한다. 비root/in-process/W^X은 **안 깬다**(새 syscall 0, 새 권한 0, 새 ptrace op 0, 새 execmem 매핑 0 — 전부 기존 trap 사이트 확장이고 커널이 정상 적재한 이미지에 필터/trace만 _상속_시킨다). 커널 사실 모순도 **없다**(seccomp-across-execve, SEIZE-across-fork/exec, x0=path 전부 man page 확정). **그러나 주장의 _실용적 충분성_은 가정-1에서 붕괴할 수 있다**: "환경+seccomp 보존이면 충분"의 _환경_ 절반은 커널이 아니라 chromium 런처가 통제한다 — chromium이 LD_PRELOAD를 거르면(WebSearch상 그럴 공산이 큼) seccomp/ptrace 절반은 살아 path-mediation은 되지만 **interposer 기반 가속과 file-backed PROT_EXEC .so 로딩이 (B) 자식에서 죽어**, gpu/zygote 자식은 "매개되긴 하나 느리고, dlopen 일부 실패" 상태가 된다. 즉 _설계는 제약을 안 깨고 커널 사실과도 안 맞지만, "완전 매개"는 envp 전파라는 chromium-쪽 변수에 인질로 잡혀 있다._
- **영영 안 될 시나리오**: (a) 가정-1 반증(chromium이 env clear) + (b) 가정-3 적중(`/proc/self/exe` exec로 exec 대상이 host 이미지) 가 동시에 참이면, (B) exec 자식은 _interposer 없이_ + _게스트 아닌 이미지_로 실행되어 in-process 매개가 원리적으로 불가. 이때 fork 자식(renderer)은 여전히 (A)로 완전 매개되나 gpu/zygote는 안 된다.
- **그때의 목표 재정의**: R4를 "모든 chromium 프로세스 완전 in-process"에서 **"`--single-process --no-zygote`로 단일 매개 프로세스 usable 달성(이게 1차 증명 상한) + 멀티프로세스는 fork-자식(renderer) 완전 매개 / fresh-exec 자식(gpu/zygote)은 best-effort(seccomp+ptrace path-mediation은 보장, interposer 가속은 envp 전파 성공 시에만) + 한계 문서화"** 로 재정의한다. 이는 ADR-002 §5의 "syscall-light는 0% 달성, storm은 best-effort+한계 문서화" 재정의 톤과 일관 — R4도 "fork-자식은 자동, exec-자식은 best-effort"로 _자식 클래스별_ 정직한 상한을 긋는다. **constraint_violations 없음은 확정**(비root/in-process/W^X/SELinux 어느 것도 안 깨며, 깨질 위험이 있는 가정은 전부 §4에 device-only로 격리). chromium 사용자 보류 상태이므로(메모리: chromium-native-goal) M-R4-fork/execmap/envprop(기준선·프로브)는 보류와 무관히 진행, (B-1) 실 구현 착수만 게이트.

---

관련 파일(절대경로):
- 본 ADR: `/Users/naen/Documents/alr-research-cp6/docs/design/adr-003-multiprocess-exec-reentry.md` (제안 위치, 격리 브랜치)
- 선행: `/Users/naen/Documents/alr-research-cp6/docs/design/adr-002-chromium-cp6-roadmap.md` §6, `/Users/naen/Documents/alr-research-cp6/docs/design/adr-001-syscall-overhead-user-notif.md`
- WS-1 코드: `/Users/naen/Documents/alr-research-cp6/app/src/main/cpp/runtime_report.cpp` (supervisor 루프 L2025~, EVENT_SECCOMP path-rewrite L2053~, `is_exec` x1-skip L2064-2068, execve-trace 필터 L1219, SEIZE opts L1913, env push L1477~), `/Users/naen/Documents/alr-research-cp6/app/src/main/cpp/alr_interpose/libalr_interpose.c`
- WS-5: `/Users/naen/Documents/alr-research-cp6/tests/test_pcgate_bpf_logic.py`, `/Users/naen/Documents/alr-research-cp6/bench/report_parse.py`

핵심 코드 사실(load-bearing): supervisor의 path-rewrite는 `path_addr = static_cast<uintptr_t>(regs[1])`(x1, `runtime_report.cpp` L2073)에서 path를 읽으며, 코드 자신이 L2064-2068 주석에서 "execve's x1 is argv (a char**), so blindly rewriting x1 would corrupt argv ... never treat an exec syscall's x1 as a path"라며 exec를 _건너뛴다_. execve의 path는 x0(regs[0])이므로 (B-1) 구현은 exec 분기에서 `regs[0]`을 읽도록 분기 추가가 필요하다.

Sources: [seccomp(2) — Linux manual page (filters preserved across execve, NO_NEW_PRIVS)](https://man7.org/linux/man-pages/man2/seccomp.2.html), [ptrace(2) — Linux manual page (PTRACE_O_TRACEEXEC/PTRACE_EVENT_EXEC, SEIZE inheritance to fork/vfork/clone children)](https://www.man7.org/linux/man-pages/man2/ptrace.2.html), [No New Privileges — Linux Kernel docs](https://docs.kernel.org/userspace-api/no_new_privs.html), [execve / LD_PRELOAD must be in envp explicitly](https://linuxvox.com/blog/want-the-excutable-run-by-execve-to-use-my-preloaded-library/), [ld.so(8) — secure-execution mode ignores LD_PRELOAD with slashes and LD_LIBRARY_PATH](https://www.man7.org/linux/man-pages/man8/ld.so.8.html), [Chromium docs — linux/zygote.md (renderers forked from zygote)](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/linux/zygote.md), [Chromium linux sandbox docs (GPU process exec'd, two-layer zygote)](https://chromium.googlesource.com/chromium/src/+/0e94f26e8/docs/linux_suid_sandbox.md), [chromium-dev — env vars not inherited by renderers via zygote](https://groups.google.com/a/chromium.org/g/chromium-dev/c/aW2GU3KKg6c), [Mojo Core overview — AF_UNIX/SCM_RIGHTS fd passing](https://chromium.googlesource.com/chromium/src/+/master/mojo/core/README.md), [seccomp_filter kernel docs — child processes constrained to same filters across fork/execve](https://docs.kernel.org/userspace-api/seccomp_filter.html)
---
---

# §8. ADR-003-v2 — re-map-on-exec: 정적 re-entry 스텁 (Option S 채택)

> 생성 경위: R8-A. v1(§1–§7)의 "(B) fresh-execve 자식은 path-rewrite + seccomp/SEIZE 상속으로 자동 매개" 주장을 device(v139 drain#17)가 반증한 데 대한 정정·재설계. HOST-ONLY 작업: 설계 + ISOLATED PoC 스텁(`app/src/main/cpp/alr_reentry/`)만, 동작 검증은 device(통합 세션)로 위임. 동작하는 loader(`runtime_report.cpp`/`alr_loader*.cpp`)는 미변경.

- 상태: **Proposed (device-pending)** — 설계 확정 + PoC 스텁 컴파일·정적-ELF 검증 완료. re-entry 실증은 통합 세션 drain.
- 워크스트림: **R8-A** (이 문서 + `app/src/main/cpp/alr_reentry/`); 통합 세션이 supervisor에 `execve→stub` rewrite 배선.

## 8.0 device 사실 (무엇이 바뀌었나)

v1 §2-(B)는 "fresh-execve 자식은 새 syscall 0 / 새 권한 0 / 새 ptrace op 0 — 커널이 정상 적재한 새 이미지에 필터+trace만 상속시키면 된다"고 했다. **device가 이를 반증:**

- B-1(execve x0→rootfs path rewrite)은 **device-proven**: `alr exec x0=/bin/dash reason=rewrite` — 경로 rewrite는 정확히 동작.
- 그러나 **모든 exec에 걸쳐 `exec_events=0`** — `PTRACE_EVENT_EXEC`가 단 한 번도 발생하지 않았다.
- 근본 원인: rewrite된 rootfs 바이너리(`<rootfs>/bin/dash`)는 **glibc 동적 ELF**라 `PT_INTERP = /lib/ld-linux-aarch64.so.1`을 가진다. 커널 `execve`는 binfmt_elf 단계에서 이 인터프리터를 **커널이 직접 open/map** 해야 하는데, 그 경로는 **게스트 게스트 절대경로**(rootfs 밖, Android 루트에 그런 파일 없음)이고, 설령 supervisor가 그 open을 트랩해 rewrite하려 해도 **binfmt_elf의 인터프리터 open은 seccomp/ptrace가 가로채는 syscall 경계 _안쪽_(커널 내부 path lookup)이라 우리 트랩 사이트로 안 온다.** → 커널-execve가 인터프리터 resolve 실패로 **에러 반환**, 새 이미지가 안 생기고, 따라서 상속시킬 대상도 없다.
- 결론: **"loader가 새 ELF를 in-process로 다시 map" 이 실제 필요분이다** — v1이 명시 보류했던 바로 그것. v1 §2-(A)(zygote-fork=clone 자식은 주소공간 복제로 자동 매개)는 **여전히 유효**(clone엔 execve 벽이 없다); 깨진 건 (B)뿐이다.

## 8.1 설계 공간 (3안)

목표: 게스트가 `execve(target, argv, envp)`를 호출할 때, **커널-execve를 성공시키되 그 결과가 ALR이 in-process로 다시 매개한 glibc 게스트**가 되도록.

### Option S — 정적 re-entry 스텁 (★ 채택)
- **무엇**: `PT_INTERP`도 glibc 의존도 없는, **커널이 직접 execve할 수 있는 작은 static-PIE/ET_EXEC aarch64 ELF** (`alr-reentry`). supervisor가 게스트 execve를 **`execve(<rootfs>/usr/lib/androlinux/alr-reentry, [<stub-path>, <target>, argv[1..]...], envp)`** 로 rewrite한다. 커널은 이 정적 스텁을 정상 적재(인터프리터 resolve 불필요) → execve 성공 → `PTRACE_EVENT_EXEC` 발생. 스텁은 ALR의 in-process 매핑 로직을 재사용해 **게스트 ld.so + target을 in-process로 map하고 올바른 초기 SP/auxv로 ld.so 진입점에 점프** = ALR loader를, 게스트-실행가능 부트스트랩으로 재패키징한 것.
- **왜 동작하나(핵심 통찰)**: 커널의 **진짜** execve(정적 스텁 대상)는 다음을 _상속 보존_한다 —
  1. **stacked seccomp 필터** (seccomp.2: "filters are preserved across execve") → 스텁과 그 뒤 게스트가 같은 path/execve 트랩 필터 하에서 시작.
  2. **PTRACE_SEIZE + PTRACE_O_TRACEEXEC** (ptrace.2: SEIZE는 execve를 가로질러 유지되고 `PTRACE_EVENT_EXEC` stop이 execve 반환 _전_ 발생) → supervisor가 새 이미지를 자동 재포착.
  3. **envp** — supervisor가 rewrite 시 envp를 그대로 전달(또는 B-3 augment)하므로 `LD_PRELOAD=<abs rootfs interpose .so>`·`ALR_ROOTFS`가 스텁→게스트로 전파.
- **비용/리스크**: 스텁 1개(수십 KB) 빌드·rootfs 스테이징. 매퍼 로직을 loader와 _당분간 중복_(통일 TODO). exec당 커널-execve 1회 + in-process map 1회(=원 launch와 동급) 오버헤드. **하지만 새 ptrace op 0, 새 권한 0, 새 SELinux 도메인 우회 0** — 정적 스텁의 execve는 untrusted_app가 이미 하는 평범한 동작.

### Option P — ptrace 주입식 remap (기각)
- **무엇**: supervisor가 execve 트랩에서 **커널 execve를 억제**(syscall nr을 무효값으로 SETREGSET하거나 `PTRACE_SYSCALL`로 -ENOSYS 반환시키고)하고, **ptrace로 tracee에 mmap/mprotect를 원격 구동**(레지스터에 syscall 인자 세팅 → single-step → 결과 회수 반복)해 주소공간을 처음부터 재구성, regset(PC=ld.so 진입, SP=새 스택)을 직접 써넣어 점프.
- **왜 훨씬 복잡/취약한가**:
  - tracee 안에서 syscall을 _원격 구동_하려면 매 mmap/mprotect/read마다 (레지스터 세팅 → 안전한 `svc` 명령으로 PC 점프 → trap 회수 → 원복)을 수십~수백 회. 게스트 ELF의 PT_LOAD가 N개면 그만큼 라운드트립.
  - **W^X 충돌**: PROT_EXEC를 가진 익명 매핑을 _tracee 컨텍스트에서_ 만들려면 그 안에 RW→memcpy→RX 시퀀스를 또 원격 구동해야 하고, memcpy 자체를 tracee 메모리에 pwrite로 채워야 한다(수 MB ld.so를 ptrace pwrite로 = 극악).
  - **에러 복구 표면**이 거대(중간 실패 시 tracee가 반쯤-매핑된 좀비). 커널-execve가 해주는 "원자적 주소공간 교체 + auxv 세팅"을 전부 손으로 재현.
  - **이득 0**: Option S가 정적 스텁 execve _한 번_으로 동일 결과를 커널 원자성으로 얻는다. P는 S가 못 하는 걸 하나도 못 하면서 훨씬 깨지기 쉽다. → **기각.**

### Option T — 게스트-메모리 트램폴린 (부분기각·S의 미래 최적화)
- **무엇**: 첫 launch 때 게스트 주소공간에 **미리 매핑해 둔 in-guest 트램폴린**(작은 PROT_EXEC 코드 + 매퍼)이 있고, execve를 그 트램폴린 진입으로 redirect(커널-execve 안 함, supervisor가 PC를 트램폴린으로 SETREGSET)해 _커널을 거치지 않고_ 같은 프로세스 안에서 새 ELF를 map+jump.
- **장점**: 커널-execve·새 프로세스 이미지 0 → 가장 빠를 수 있음.
- **치명적 결함**: execve의 POSIX 의미론(이미지 교체 = **기존 매핑/fd 정리, signal disposition 리셋, 새 stack, exec-only state**)을 손으로 재현해야 하는데, 특히 **execve는 호출 _프로세스_를 교체**하지 스레드만 바꾸는 게 아니다 — 멀티스레드 게스트(chromium·GIMP)에서 execve는 _호출 스레드 외 모든 스레드를 커널이 동기적으로 종료_시킨다. 트램폴린은 이 스레드 정리를 못 해 → 좀비 스레드가 새 이미지와 주소공간 공유 → 즉시 붕괴. 또 **seccomp 필터/SEIZE는 "이미 우리 하에 있으니" 보존되지만, exec-on-self의 fd close-on-exec·signal reset을 안 하면 새 프로그램이 더러운 상태로 시작.**
  - → **현 단계 기각.** 단 _스레드 없는 단일-스레드 자식_(dpkg→sh 같은 단순 helper)에 한해 S의 커널-execve 비용을 없애는 **미래 최적화 후보**로 §8.7에 보류. 1차는 S(정확성 우선).

**채택: Option S.** 이유 한 줄: **커널-execve가 공짜로 해주는 (원자적 이미지 교체 + 스레드 정리 + seccomp/SEIZE/envp 상속)을 그대로 쓰면서, 단지 execve 대상만 "커널이 적재 가능한 정적 부트스트랩"으로 바꿔 PT_INTERP 벽을 우회**한다. P/T는 그 커널 보장을 손으로 재현하려다 깨진다.

## 8.2 supervisor가 "이 exec은 stub 행" 임을 아는 법 + rewrite 형태

현 B-1 핸들러(`runtime_report.cpp` L2223~, `is_exec` 분기)는 이미 execve 트랩에서 x0(execve)/x1(execveat)의 **target path**를 읽고 `decide_exec_path_mediation`로 rootfs host path를 만든다. v2는 그 **rewrite 타깃을 "rootfs 안의 target 바이너리"가 아니라 "정적 스텁"으로 바꾸고, 원래 target을 argv로 밀어 넣는다:**

```
원래 게스트: execve("/bin/dash", ["/bin/dash","-c","..."], envp)
v1 (device-fail): execve("<rootfs>/bin/dash", [동일 argv], envp)   ← 커널이 PT_INTERP resolve 실패
v2 (Option S):    execve("<rootfs>/usr/lib/androlinux/alr-reentry",
                         ["<rootfs>/usr/lib/androlinux/alr-reentry",  // stub argv[0]
                          "/bin/dash",                                 // stub argv[1] = 원 target (게스트 절대경로!)
                          "-c","..."],                                 // 원 argv[1..]
                         envp)                                         // envp 그대로(B-3가 LD_PRELOAD/ALR_ROOTFS 보장)
```

- **target은 _게스트 절대경로_ 그대로** argv[1]에 넣는다(rootfs host path 아님). 스텁이 자기 안에서 다시 `<rootfs>+target`/`<rootfs>+PT_INTERP`로 합쳐 연다 — 이로써 rootfs 매핑 로직의 SSOT가 스텁(=loader와 동일 규칙) 한 곳. (대안: supervisor가 미리 host path로 변환해 argv[1]에 넣어도 됨 — §8.7 미정점.)
- **"stub 행 인지" 는 불필요**: supervisor는 _모든_ execve 트랩을 동일하게 stub rewrite한다. 그럼 스텁이 execve한 결과는? 스텁은 **execve를 다시 호출하지 않는다**(in-process map+jump). 따라서 _재귀 rewrite 위험이 없다_. 게스트가 _그 다음_ execve를 호출하면(dpkg→sh) 그때 다시 stub rewrite — 매 단계가 1 stub. **단** 스텁 자신이 만약 어떤 이유로 execve를 부르면(안 부르지만 방어적으로) 그 x0이 **이미 `<rootfs>/usr/lib/androlinux/alr-reentry`** 이므로 `already-host`/`is-stub` 가드로 무한루프 차단(§8.7-가드).
- **검출 보강(권장)**: supervisor가 stub의 절대 host path를 상수로 알고, exec x0이 그것과 정확히 일치하면 "이미 stub" → rewrite 스킵(idempotent). 이 한 줄 가드가 재귀의 유일한 방어선.

## 8.3 argv / envp 보존

- **argv**: supervisor가 **새 argv 배열을 tracee 메모리에 합성**해야 한다(스텁 path를 [0]에 끼우고 원 argv를 1칸 shift). 이건 B-1의 "x0만 rewrite, argv 불변"보다 한 단계 더 — **argv 자체를 재작성**. 기법: B-3가 이미 하는 "scratch window에 string blob + 새 포인터 배열을 pwrite하고 레지스터를 거기로" 와 동일 패턴을, 이번엔 **argv(x1/execve, x2/execveat)** 에 적용. 스텁 path 문자열 + 원 argv 포인터들 + 새 [0]만 추가하면 되므로 blob이 작다.
- **envp**: **그대로 통과**. 단 B-3(child envp re-injection, L2295~)가 **이미** `LD_PRELOAD=<abs rootfs interpose .so>`·`ALR_ROOTFS`를 보장하므로, 스텁은 envp에서 `ALR_ROOTFS`를 읽어 ld.so를 찾고, 게스트 glibc는 `LD_PRELOAD`로 interposer를 자동 재주입. **v1 §4-가정-1(chromium이 env clear)** 은 B-3가 매 exec마다 강제 주입으로 _이미_ 해소: envp는 supervisor가 통제(커널이 아니라).
- **AT_SECURE**: 스텁이 게스트 auxv에 `AT_SECURE=0`을 쓴다(§8.4). 정적 스텁의 execve는 setuid가 아니므로 커널이 스텁에게 준 auxv도 AT_SECURE=0 → ld.so가 슬래시 포함 LD_PRELOAD(R3 규칙)를 존중. (v1 §4-가정-2 해소.)

## 8.4 스텁이 합성해야 하는 auxv / SP

스텁은 **자기 자신이 받은 커널 auxv**(정적 ELF로서 커널이 정상 제공)에서 머신 사실을 읽어 **게스트용 auxv를 재합성**한다. PoC가 구현한 것(`alr_reentry.c`):

- **자기 auxv에서 읽음**: `AT_PAGESZ`(16K 디바이스 대응 — loader의 하드코딩 0xfff 대신 _런타임 페이지크기_), `AT_HWCAP/HWCAP2`(ifunc resolver용 — PoC는 0 전달, TODO 실값), `AT_RANDOM`(16B 복사), `AT_SYSINFO_EHDR`(vDSO), `AT_UID/EUID/GID/EGID/CLKTCK`.
- **게스트용으로 합성**(loader L1815~와 동형): `AT_PHDR/PHENT/PHNUM`=target의 phdr, `AT_BASE`=ld.so load base(동적)/0(정적), `AT_ENTRY`=target 진입, `AT_EXECFN`=게스트 argv0, `AT_FLAGS=0`, `AT_SECURE=0`, `AT_RANDOM`=새 16B, `AT_PAGESZ`=런타임값.
- **초기 SP**: SysV 레이아웃 `[argc][argv..][NULL][envp..][NULL][auxv..][AT_NULL]`, 16B 정렬. PoC는 fresh 512KiB 스택에 빌드.
- **점프**: 동적이면 **ld.so 진입(`interp.entry`)** 으로(ld.so가 target의 DT_NEEDED를 링크 후 AT_ENTRY로 점프), 정적이면 target 진입 직행. `mov sp,<start>; mov x0,#0; br <entry>`.
- **TLS 차이(중요)**: loader는 bionic TCB를 숨기려 `msr tpidr_el0, <clean tcb>` 를 했지만, **스텁은 안 한다** — 스텁은 (bionic 안이 아니라) **커널이 갓 적재한 깨끗한 프로세스**라 숨길 foreign TCB가 없고, glibc ld.so가 자기 TLS를 세팅한다. (PoC `enter_guest`가 tpidr 미설정.)
- **signal/rseq**: 정적 스텁은 libsigchain·bionic rseq를 _상속하지 않는다_(커널-execve가 signal disposition을 SIG_DFL로 리셋, rseq 해제). loader가 하던 `rt_sigaction` SIG_DFL 루프·rseq 우회가 **스텁에선 불필요** — 커널-execve가 공짜로 해준다(Option S의 또 다른 이득).

## 8.5 멀티프로세스 (dpkg→sh→dpkg-deb 각 단계 stub 재진입)

- dpkg가 maintainer script를 `execve("/bin/sh", ...)` → supervisor가 stub rewrite → 커널이 스텁 적재(EVENT_EXEC) → 스텁이 sh(glibc) in-process map+jump.
- sh가 `execve("/usr/bin/dpkg-deb", ...)` → 또 stub rewrite → 또 스텁 → dpkg-deb 게스트.
- **각 단계가 독립 커널-execve**라 seccomp 필터·SEIZE가 단계마다 상속(커널 확정). interposer는 envp의 LD_PRELOAD로 매 게스트에 재주입(B-3). → **체인 전체가 매개**.
- supervisor 변경 0(새 tracee 핸들링은 기존 `PTRACE_O_TRACEFORK|CLONE` + waitpid(-1,__WALL) 루프가 이미 처리). stub rewrite 한 가지만 추가.

## 8.6 W^X / seccomp 상호작용

- **스텁의 매핑은 W^X-safe**: PoC 매퍼가 loader와 동일하게 PT_LOAD를 **RW로 mmap → memcpy → 최종 prot(RX 등)로 mprotect** (동시 W+X 매핑 0). PROT_EXEC 매핑 후 `dc cvau/ic ivau` I-cache sync(`flush_icache`, libc 없는 freestanding이라 직접 발행).
- **untrusted_app file-backed PROT_EXEC**: device-proven(메모리: ALR dynamic loader proven, v79 — untrusted_app가 rootfs .so의 file-backed PROT_EXEC 허용). 스텁의 anon-RW→RX는 그보다 약한 요구(익명 execmem)로, 역시 device-proven 경로.
- **seccomp**: 스텁은 새 필터를 _설치하지 않는다_(커널이 상속시킨 filter 그대로). 스텁이 부르는 mmap/mprotect/openat/read는 filter상 ALLOW(또는 path syscall이면 supervisor가 rewrite — 스텁이 여는 `<rootfs>...` 경로는 `already-host`라 idempotent no-op). 스텁은 execve를 **안 부르므로** execve-trace 필터에 안 걸린다.
- **PCGATE**: 스텁 자체는 interposer를 안 쓴다(직접 svc). 게스트는 LD_PRELOAD로 interposer를 받아 평소 PCGATE 경로. 스텁의 소수 syscall은 핫루프 아님(매 exec 1회).

## 8.7 미검증/열린 리스크 (device·통합 세션이 닫을 것)

1. **[R-1·1순위] EVENT_EXEC가 실제로 뜨는가.** Option S 전체가 "정적 스텁이면 커널-execve 성공 → EVENT_EXEC"에 걸려 있다. PoC는 정적 ELF임을 _정적_ 증명(no PT_INTERP/ET_EXEC/AArch64)했으나, **untrusted_app + stacked seccomp 하에서 그 execve가 정말 성공해 EVENT_EXEC를 내는지는 device-only.** 만약 안 뜨면(SELinux가 execmem/exec 막거나, 정적 ELF조차 막히면) Option S도 실패 → 그땐 P/T 재검토. (반증 시 ADR 재작성.)
2. **[R-2] argv 재작성의 메모리 안전.** supervisor가 tracee scratch에 새 argv 포인터 배열+스텁 path를 pwrite할 때, scratch 위치(현 B-1은 `sp-2048`)가 원 argv/envp 문자열과 겹치지 않아야. 긴 argv(chromium)면 2048B 부족 가능 → 동적 크기 + 더 낮은 scratch. host 테스트로 레이아웃 회귀.
3. **[R-3] `/proc/self/exe` exec.** v1 §4-가정-3 그대로 잔존: chromium이 `/proc/self/exe`를 execve하면 그 가상 심볼릭은 _현재 매핑된 실제 바이너리_(스텁? 게스트? bionic?)를 가리켜 stub rewrite가 무의미. target=`/proc/self/exe`면 스텁은 "내가 직전에 실행한 게스트 target"을 알 방법이 필요(envp `ALR_REENTRY_SELF=<게스트경로>` 주입 등) — §8.7 미구현, device strace로 빈도 확정 후.
4. **[R-4] ifunc HWCAP.** PoC 매퍼는 IRELATIVE resolver에 HWCAP 0 전달. glibc ld.so/일부 .so의 ifunc(memcpy 등)이 HWCAP 기반 변형을 고르므로, **스텁이 자기 auxv의 AT_HWCAP/HWCAP2를 resolver에 전달**하도록 통합 전 보강 필요(loader는 `getauxval`로 실값 사용). 미보강 시 잘못된(느린/틀린) ifunc 변형 선택 위험.
5. **[R-5] target buf 크기.** PoC는 target/ld.so를 8MiB .bss 버퍼에 read. 큰 동적 실행파일(chromium 본체 수십 MB)은 초과 → 통합 시 **mmap-the-file**(MAP_PRIVATE 파일 매핑 후 그 위에서 PT_LOAD 파싱)로 교체. dpkg/sh/dash 같은 helper는 8MiB로 충분(PoC 범위).
6. **[R-6] 스텁 스테이징.** `<rootfs>/usr/lib/androlinux/alr-reentry`에 스텁을 넣는 것은 RootfsInstaller/overlay 작업(통합 세션). 스텁 SONAME/권한·실행비트. (R8-B 스테이징 오버레이와 합류 지점.)
7. **[보류·T 최적화] 단일스레드 helper의 커널-execve 생략.** dpkg→sh류는 스레드가 없으니 Option T 트램폴린으로 커널-execve를 없애 더 빠르게 가능 — 단 정확성(S) 확정 후. chromium/GIMP(멀티스레드)는 영구히 S(커널의 스레드 정리 필요).

## 8.8 host 프로토타입 범위 (이 PoC가 한 것 / 안 한 것)

- **한 것**: `app/src/main/cpp/alr_reentry/alr_reentry.c`(freestanding static aarch64, 자체 ELF 매퍼+auxv/SP 빌더, no libc) + `build-reentry.sh`. host 게이트로 **정적 aarch64 ELF(ET_EXEC, AArch64, no PT_INTERP, no DT_NEEDED)** 임을 증명 — 커널이 execve 가능한 형태 확정. APK 네이티브 빌드와 **완전 분리**(guest_shim과 동일하게 별도 빌드, CMake 미참조).
- **안 한 것(=통합·device)**: (a) supervisor의 execve→stub rewrite 배선(§8.2의 argv 재작성), (b) 스텁 rootfs 스테이징(§8.7-R-6), (c) 실제 re-entry 실증(EVENT_EXEC 발생 + dpkg unpack 성공) — **device-only**. PoC는 _실행 불가_(aarch64 디바이스 필요)이며, "건전한 설계 + 컴파일 클린·유효 정적-ELF" 가 산출물이지 "증명된 re-entry"가 아니다(정직).

## 8.9 통합 세션이 다음에 배선할 것 (정확한 핸드오프)

1. **스텁 빌드·스테이징**: `bash app/src/main/cpp/alr_reentry/build-reentry.sh` → `out/alr-reentry`를 rootfs `<rootfs>/usr/lib/androlinux/alr-reentry`(실행비트)로. (R8-B 오버레이와 합류.)
2. **supervisor rewrite 변경**(`runtime_report.cpp` `is_exec` 분기, L2223~): `decide_exec_path_mediation`이 현재 만드는 "target→rootfs host path" 를 **"x0/x1=스텁 host path, argv 재작성=[스텁path, 원target(게스트절대), 원argv[1..]]"** 로 교체. argv 재작성은 B-3의 envp blob 합성 패턴 재사용(scratch window). **idempotency 가드**: exec x0이 이미 스텁 host path면 스킵(§8.2).
3. **envp**: B-3 그대로(LD_PRELOAD/ALR_ROOTFS 보장) — 변경 불요.
4. **device-verify (DEVICE-REQ)**: `DEVICE-REQ: ALR-R8-reentry — SM-X236N (am force-stop first), guest exec 체인(예: dpkg-deb -x 또는 /bin/dash -c 'exec /bin/echo hi'); gate = (i) supervisor 로그에 exec_events>0 AND EVENT_EXEC 발생, (ii) 스텁 'ALR-REENTRY: jumping' 로그, (iii) 재진입 게스트가 rootfs를 보고 정상 종료(echo 출력/dpkg unpack). 반증=exec_events 여전히 0 ⇒ §8.7-R-1(정적 스텁 execve도 막힘) ⇒ Option P/T 재검토.`
5. **보강(통합 전 권장)**: §8.7 R-4(ifunc HWCAP 실값), R-5(mmap-the-file for 큰 ELF), R-2(동적 argv scratch 크기).

## 8.10 정직 섹션

- **constraint_violations 없음**: 새 syscall 0(스텁은 mmap/mprotect/openat/read/execve만, 전부 untrusted_app 평범 동작), 새 ptrace op 0, 새 권한 0, SELinux 우회 0(정적 ELF execve는 우회가 아니라 정상 경로), W^X-safe(RW→RX, 동시 W+X 0), version stamp 불변. 동작 loader 미변경(PoC는 격리 디렉터리).
- **자가 적대검증(핵심 주장 자기공격)**: 주장 — "정적 스텁이면 커널-execve 성공 → EVENT_EXEC → in-process 재매개". 공격점: (1) **정적 ELF조차 untrusted_app+seccomp에서 execve가 막힐 수 있다**(R-1) — 그러나 ALR은 이미 자기 fork 자식을 execve 없이 in-process 진입시키는 것과 별개로, _정적 바이너리 native exec_ 자체는 device-proven(메모리: glibc-static native exec SOLVED v73/v76)이라 정적 execve 경로는 살아있을 공산이 크다. (2) **chromium `/proc/self/exe`** (R-3) — 이건 S가 못 푸는 진짜 한계로 격리, dpkg/apt(절대경로 exec)는 S로 풀린다. (3) **스레드 정리** — Option T가 깨지는 지점인데 S는 커널-execve가 해주므로 안전. → 설계는 제약을 안 깨고 커널 사실(seccomp/SEIZE/auxv across execve)과 정합하며, 유일한 급소(R-1)는 device 1회 drain으로 확정/반증된다.
- **"영영 안 될" 시나리오**: R-1 반증(정적 스텁 execve조차 EVENT_EXEC 미발생)이면 Option S 폐기 → Option P(ptrace remap, 고비용)만 남고, 그조차 막히면 멀티프로세스 exec 자식은 in-process 매개 불가 → `--single-process`/단일프로세스 helper로 목표 재정의(v1 §7 톤 계승). 단 fork(clone) 자식은 v1 §2-(A)로 여전히 자동 매개.

## 8.11 관련 파일 (절대경로, 이 워크트리)

- PoC 스텁: `app/src/main/cpp/alr_reentry/alr_reentry.c`, `app/src/main/cpp/alr_reentry/build-reentry.sh`, 산출물 `app/src/main/cpp/alr_reentry/out/alr-reentry`.
- 동작 loader(미변경, 매퍼 SSOT 출처): `app/src/main/cpp/runtime_report.cpp` — `map_elf_image_into_execmem`(L1270~), 초기 SP/auxv 빌드(L1770~1908), `alr_enter_guest`(L1152~), execve trace 필터(L1224~), B-1 exec path mediation(L2212~), B-3 envp re-injection(L2295~).
- 분류 함수: `app/src/main/cpp/alr_runtime/alr_exec.hpp` `decide_exec_path_mediation`(통합 시 stub-target 형태로 확장).
- 선행 v1: 본 문서 §1–§7(SUPERSEDED-by-§8).

Sources: [seccomp(2) — filters preserved across execve, NO_NEW_PRIVS](https://man7.org/linux/man-pages/man2/seccomp.2.html), [ptrace(2) — PTRACE_O_TRACEEXEC/PTRACE_EVENT_EXEC, SEIZE across execve](https://www.man7.org/linux/man-pages/man2/ptrace.2.html), [execve(2) — kernel resolves PT_INTERP; thread teardown; argv/envp caller-built](https://man7.org/linux/man-pages/man2/execve.2.html), [ld.so(8) — AT_SECURE secure-execution ignores slash-LD_PRELOAD](https://www.man7.org/linux/man-pages/man8/ld.so.8.html), [ELF binfmt — static ET_EXEC needs no interpreter](https://man7.org/linux/man-pages/man5/elf.5.html)
