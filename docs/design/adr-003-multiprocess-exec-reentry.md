> 생성 경위: R4(멀티프로세스 exec re-entry, Phase C) 심층연구 + 자가 적대검증 산출. ADR-002 §6이 명시 보류한 R4 후속분을 정식 ADR로 승격. 레포 읽기 전용(`/Users/naen/Documents/alr-research-cp6`), `main`·`ws-N` 미변경. 코드 사실은 `app/src/main/cpp/runtime_report.cpp`(supervisor/seccomp/exec trap) 직접 확인, 커널 사실은 WebSearch 교차검증. 리뷰 대상: WS-1(`runtime_report.cpp`/`libalr_interpose.c`), WS-5(`tests/`/`bench/`), 통합 세션.

# ADR-003 — Chromium 멀티프로세스 exec re-entry (Phase C): "loader 재진입" 기각, "환경+seccomp 보존 + SEIZE 상속" 채택

- 상태: **Proposed** — R4 심층연구 + 자가 적대검증. 코드 변경 없음, device 게이트 미통과.
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