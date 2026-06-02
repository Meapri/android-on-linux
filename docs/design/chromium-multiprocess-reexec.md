# Chromium multiprocess (CR-5) under ALR — `/proc/self/exe` → guest-binary re-map at the exec trap

> 생성 경위: CR-mp-design lane. HOST-ONLY 설계 문서(코드 변경 0, device 검증 0). 코드 사실은
> `app/src/main/cpp/runtime_report.cpp`(supervisor exec-trap)·`alr_inproc_reexec.{c,h}`(resident
> 재-맵 트램폴린)·`runtime_report.cpp` L1504(loader 의 `host_path` 산출) 직접 확인. 재진입
> 메커니즘 사실은 **ADR-003-v3**(`docs/design/adr-003-multiprocess-exec-reentry.md` §8 + round-10/11
> device evidence)에서 인용. chromium 멀티프로세스 사실은 `docs/research/chromium-native-plan.md`
> Phase C / Scout 2·4 에서 인용. **이 문서는 `docs/research/**`(run-plan lane)나 어떤 코드도
> 편집하지 않는다 — 설계만.**
>
> 상태: **Proposed (device-pending)**. 의존: **G1 재-맵 게스트 실행 정확성**(round-11 v144 에서
> static glibc SIGILL device-FIXED, 매퍼 정확; dynamic-PIE 경로는 wired but device-未검증) +
> **per-exec inproc scoping**(round-12 g1-seqint lane, device-pending). 이 문서는 그 토대 위에
> chromium 의 zygote/renderer/gpu 자식 spawn 을 **`/proc/self/exe` → 알려진 게스트 바이너리 치환**
> 으로 매개하는 한 조각을 추가한다.
>
> HARD CONSTRAINTS(불변): 비root(untrusted_app, no CAP_SYS_ADMIN), public Android API only, SELinux
> 우회 금지, W^X-safe(새 execmem 0), in-process(커널 execve 0 — W^X 가 `app_data_file:execute` 를
> 막으므로 ADR-003-v2 Option S 는 DEAD), version stamp 불변, 새 ptrace op 0.

---

## 0. 한 줄 결론

chromium 의 zygote 가 renderer/gpu/utility 자식을 띄울 때 호출하는 `execve("/proc/self/exe",
["/proc/self/exe","--type=renderer", …IPC fd 번호…], envp)` 는 **현재 in-process 재-맵에서 의도적으로
SKIP** 된다 — `/proc/self/exe` 가 ALR 의 *Android* 로더 바이너리(interp `/system/bin/linker64`)로
resolve 되어 트램폴린이 Debian rootfs 로 매핑할 수 없기 때문(`runtime_report.cpp` L2389-2395
`proc-self-exe` skip). **CR-5 의 핵심 설계는 이 SKIP 을 "치환"으로 바꾸는 것이다**: 게스트가
`/proc/self/exe` 를 exec 하려 할 때, supervisor 가 그것을 **로더가 in-process 로 매핑한 _실제 게스트
프로그램의 host 경로_**(= `config.rootfs_dir + guest_rel`, 로더 L1504 의 `host_path` = chromium glibc
바이너리)로 인지·치환한 뒤, 원래 argv/envp 그대로 ADR-003-v3 in-process 재-맵(`alr_inproc_reexec_
trampoline`)에 태운다. chromium 은 **dynamic-PIE** 라 트램폴린의 *dynamic* 경로(PT_INTERP→`<rootfs>
<interp>` ld.so 매핑→AT_BASE=ld.so base→ld.so entry 점프, `alr_inproc_reexec.c` L633-649·L697-713)가
적용되며, 이는 static glibc 재-맵의 SIGILL 게이트(round-11 FIXED)와 별개 경로다. fd 상속은 **자동으로
보존**된다 — in-process 재-맵은 *execve 를 하지 않으므로*(syscall 취소 + PC-redirect) 커널이 fd
테이블을 건드리지 않는다(close-on-exec 도 발동 안 함). 따라서 chromium 이 명령줄 fd 번호로 넘기는 mojo
채널/IPC fd 가 재-맵된 자식에서 그대로 유효하다.

이 설계는 **새 메커니즘이 아니라** 기존 ADR-003-v3 트램폴린의 한 입력 케이스(`/proc/self/exe` 타깃)를
추가하는 것이다 — 새 ptrace op 0, 새 권한 0, 커널 execve 0, 새 execmem 0.

---

## 1. 문제의 정확한 모양 (왜 `/proc/self/exe` 가 현재 SKIP 되는가)

### 1.1 chromium 의 멀티프로세스 spawn 모델
`chromium-native-plan.md` Scout 2 확정: chromium 자식 = **같은 바이너리를 `fork()`+`execve()`** 하되
`--type=renderer`/`--type=gpu-process`/`--type=utility` 로 역할 분기. zygote 는 fork(clone, no exec)
로 renderer 다수를 띄우지만 **zygote 자신·gpu process 는 browser 가 fresh execve** 한다(ADR-003-v1
§2 표). 그 execve 의 타깃은 **`/proc/self/exe`** 다 — chromium 은 자기 바이너리 경로를 하드코딩하지 않고
"지금 실행 중인 나 자신"을 re-exec 한다(`base::GetProcessExecutablePath()` 가 `/proc/self/exe` 를
선호; argv[0] 도 보통 `/proc/self/exe`).

### 1.2 ALR 에서 `/proc/self/exe` 의 의미가 뒤집힌다
ALR 은 게스트(chromium glibc 바이너리)를 **in-process 로** 매핑한다 — 별도 프로세스로 execve 하지
않는다. 따라서 커널 관점의 "현재 프로세스 이미지"는 여전히 **Android 앱의 bionic 바이너리**(interp
`/system/bin/linker64`)다. 즉 게스트가 `/proc/self/exe` 를 읽으면 **chromium glibc 바이너리가 아니라
ALR 로더(Android .so 를 적재한 앱 프로세스)** 로 resolve 된다. 이것이 `loader-feature-gaps.md` G1 의
"게스트가 `/proc/self/exe`(interp `/system/bin/linker64`)를 exec — chromium zygote 가 *Android* 앱
바이너리를 re-exec → Debian rootfs 로 매개 불가" 항목이다.

### 1.3 현재 코드가 이걸 SKIP 하는 이유 (그리고 그게 옳았던 이유)
`runtime_report.cpp` L2378-2423(G1 seqint inproc-redirect scoping):
```
// (a) /proc/self/exe or any /proc/ path resolves to the loader's own ANDROID
//     bionic binary, not a rootfs glibc ELF — the trampoline cannot map it.
if (std::strcmp(gp, "/proc/self/exe") == 0 || std::strncmp(gp, "/proc/", 6) == 0) {
    inproc_skip_reason = "proc-self-exe";
}
```
재-맵 트램폴린(`alr_inproc_reexec_worker`, `alr_inproc_reexec.c` L599-606)은 **타깃 host 경로를
열어 그 ELF 를 읽는다**. `/proc/self/exe` 를 그 host 경로로 넘기면 트램폴린이 **Android bionic
바이너리**(또는 그 심볼릭)를 열어 — Debian glibc ELF 가 아니므로 — 게스트로 매개할 수 없다(설령 ELF
파싱은 되어도 그건 chromium 이 아니다). device drain 이 보였듯 over-broad redirect 는 직렬 supervision
을 wedge 시킨다(onCreate 프로브가 chromium zygote 의 `/proc/self/exe` exec 에서 ~9s 정지, L2380-2383).
**그래서 현재 SKIP 은 정확한 보수적 처리다 — chromium 의 `/proc/self/exe` 를 무엇으로 치환할지 모르는
상태에서는.** CR-5 가 푸는 것은 정확히 "무엇으로 치환할지"다.

---

## 2. 설계 — `/proc/self/exe` → 알려진 게스트 바이너리 치환 (그 다음 정상 재-맵)

### 2.1 supervisor 가 "실제 게스트 바이너리 host 경로"를 아는 법 (loader → supervisor 스레딩)

로더는 launch 시점에 게스트 프로그램의 host 경로를 **이미 계산한다**:
`runtime_report.cpp` L1503-1504
```cpp
const std::string guest_rel = guest_argv[0];          // 게스트 절대경로, 예 "/opt/chromium/chrome"
const std::string host_path = config.rootfs_dir + guest_rel;  // <rootfs>/opt/chromium/chrome
```
이 `host_path` 가 곧 **chromium glibc 바이너리의 host 경로**다(로더가 in-process 로 매핑한 그 ELF).
현재 이 값은 launch 경로의 로컬 변수로, supervisor 루프로 전달되지 않는다. **CR-5 의 (1)번 작업:**

- **supervisor state 에 `std::string guest_self_exe_host` 를 추가**하고, launch 시점의 `host_path`
  (= 게스트 프로그램의 rootfs host 경로)를 거기에 채워 supervisor 루프(`runtime_report.cpp` L2025~,
  exec-trap L2378~)로 스레드한다. supervisor 가 게스트 절대경로(`guest_rel`)와 host 경로(`host_path`)
  를 모두 알면 `/proc/self/exe` exec 트랩에서 둘 다 복원할 수 있다.
- **선택지 A (권장)**: supervisor 함수(현재 `supervise_*` 류)에 `const std::string& self_exe_host`
  인자 1개 추가. 단순·명시적. (실제 인자명/시그니처는 통합 세션이 코드에서 확정 — 이 문서는 *무엇을*
  스레드해야 하는지만 규정.)
- **선택지 B**: `config` 에 `guest_program_host` 필드 추가(이미 `config.rootfs_dir`·`config.program`
  보유). config 가 이미 exec-trap 까지 in-scope 이므로(L2428 `config.rootfs_dir` 사용) 가장 적은 배선.
- **왜 단순한가**: chromium 은 *자기 자신을* re-exec 하므로(argv[0]=`/proc/self/exe`, 또는 절대
  바이너리 경로), 치환 타깃은 **항상 최초 launch 한 게스트 프로그램**과 동일하다. 멀티-바이너리 추적
  자료구조가 불필요 — 단일 `guest_self_exe_host` 스칼라로 충분(chromium 자식은 전부 같은 chrome
  바이너리). (dpkg→sh→dpkg-deb 같은 *다른* 바이너리 체인은 `/proc/self/exe` 가 아니라 절대경로 exec
  이므로 §2.2-경우2 의 기존 rootfs-rewrite 경로로 처리되고 이 스칼라를 안 쓴다.)

### 2.2 exec-trap 의 치환 로직 (3 경우)

exec-trap(L2387-2423)의 분기를 다음으로 확장한다(설계 — 코드 아님):

**경우 1 — `gp == "/proc/self/exe"` (chromium 자기 재실행):**
현재: `inproc_skip_reason = "proc-self-exe"` → SKIP.
CR-5: `guest_self_exe_host` 가 비어있지 않으면(=launch 게스트를 안다면) **SKIP 대신**
`host = guest_self_exe_host`(chromium glibc 바이너리의 rootfs host 경로)로 치환하고 §2.3 의 정상
inproc 재-맵 경로(L2424-2478)에 태운다. `guest_self_exe_host` 가 비어있으면(=launch 게스트 미상,
예외적) 기존 SKIP 유지(안전 fallback).
- **주의 — `/proc/<pid>/exe` 변종**: chromium 은 보통 `/proc/self/exe` 를 쓰지만, 일부 경로는
  `/proc/<pid>/exe`(자기 pid)도 쓸 수 있다. 현재 L2393 의 `std::strncmp(gp, "/proc/", 6)` 가
  *모든* `/proc/*` 를 `proc-self-exe` 로 분류하므로, 그 중 `exe` 로 끝나는 것만 치환 대상으로
  좁혀야 한다(`/proc/self/exe` 또는 `/proc/<digits>/exe`). 그 외 `/proc/*`(예 `/proc/self/maps`)는
  exec 타깃이 아니므로 계속 SKIP. — **device 로 chromium 이 실제 어떤 형태를 쓰는지 확정 필요(§5-가정-1).**

**경우 2 — `gp` 가 rootfs-내 절대경로 (chromium 이 자기 절대 바이너리 경로를 exec, 또는 dpkg→sh):**
현재 동작 그대로. `med.should_rewrite`(reason "rewrite") 또는 `med.reason=="already-host"` 면
`host = med.should_rewrite ? med.host_path : gp` 로 정상 재-맵(L2424-2427). chromium 이
`/proc/self/exe` 대신 절대 chrome 경로를 쓰면 이 경우로 자동 처리되어 §2.1 의 스칼라조차 불필요.

**경우 3 — 그 외 (non-rootsfs/relative/sysdir/stub):**
현재 그대로 SKIP(`non-rootfs`/`stub`). 변경 없음.

### 2.3 치환 후엔 기존 재-맵 경로를 그대로 탄다 (새 메커니즘 0)

치환으로 `host` 가 chromium glibc 바이너리의 rootfs host 경로로 정해지면, **이후는 L2424-2478 의
기존 inproc 재-맵 코드가 변경 없이 처리**한다:
1. host 경로 문자열 + rootfs 문자열을 tracee scratch(`sp-2048`)에 pwrite(L2436-2445).
2. `regs[19]=host`, `regs[20]=argv`, `regs[21]=envp`, `regs[22]=rootfs`, `regs[32](pc)=
   &alr_inproc_reexec_trampoline`(L2446-2456) — `alr_inproc_reexec.h` 의 entry ABI.
3. `NT_ARM_SYSTEM_CALL=-1` 로 execve syscall 취소(L2461-2468) — **커널 execve 0**(W^X 우회).
4. tracee 가 resident 트램폴린에 진입 → `alr_inproc_reexec_worker`(L579~)가 host 경로를 열어
   chromium ELF 를 in-process 매핑 → entry 점프.

**chromium 은 dynamic-PIE 이므로 트램폴린의 _dynamic_ 경로가 적용된다**(아래 §3) — round-11 에서
device-FIXED 된 건 *static* glibc startup SIGILL 이고, dynamic 경로(ld.so 위임)는 별개로 wired
되어 있다(`alr_inproc_reexec.c` L633-649).

---

## 3. chromium 은 dynamic-PIE — *dynamic* 재-맵 경로가 적용된다 (G1 의존 명시)

### 3.1 왜 dynamic 경로인가
chromium glibc 바이너리는 `~200` .so 클로저(`runtime_report` 메모리: 186MB+ chrome + nss/icu/
freetype/harfbuzz/fontconfig/expat/xcb/libgbm)를 dlopen 하는 **dynamic-PIE(ET_DYN + PT_INTERP=
`/lib/ld-linux-aarch64.so.1`)** 다. 따라서 트램폴린은:
- `alr_inproc_reexec.c` L615-617 에서 **PT_INTERP 를 찾고**(`interp_str != 0` → `is_static=0`),
- L633-649 에서 **`<rootfs><interp>`**(=`<rootfs>/lib/ld-linux-aarch64.so.1`)를 읽어 guest ld.so 를
  `map_elf_image` 로 매핑,
- L697-713 에서 **AT_BASE=interp.base / AT_PHDR=프로그램 phdr / AT_ENTRY=프로그램 entry** 인 auxv 를
  합성한 SysV 스택을 만들고,
- **ld.so 의 entry 로 점프**(`enter_guest`, L512-520) — 그러면 ld.so 가 chromium 의 DT_NEEDED
  ~200 .so 를 정상 링크 후 AT_ENTRY 로 점프.

이는 ALR 의 launch-time 동적 로딩(`loader-feature-gaps` G1-②: "dynamic linking at scale — WORKS,
GIMP ~290 .so device-proven v79")과 **동일한 ld.so 위임 모델**이다 — 즉 재-맵 dynamic 경로가 동작하면
chromium 자식의 dlopen 클로저도 launch 와 동일하게 풀린다.

### 3.2 G1 의존 — 정직하게
- **device-proven**: in-process 재-맵 메커니즘(cancel execve + PC-redirect → 트램폴린 → map+jump,
  round-10 v141/v143) + static glibc startup 정확성(round-11 v144, SIGILL FIXED — 단일-span 매핑 +
  실 AT_HWCAP).
- **wired but device-未검증**: **dynamic-PIE 재-맵 경로**(L633-649 ld.so 매핑 분기). round-11 evidence
  는 static `/bin/sh` 로 매퍼 정확성을 입증했고 dynamic 경로도 "static+dynamic 경로 둘 다 wired"라 기록
  됐으나(`loader-feature-gaps` round-10 step2), **dynamic 재-맵이 ld.so 를 띄워 DT_NEEDED 클로저까지
  성공 실행하는 것은 device 로 미검증.** CR-5 는 이 dynamic 경로의 정확성에 **강하게 의존**한다 — static
  SIGILL 이 풀렸다고 dynamic ld.so re-map 이 풀린 게 아니다. (선행 device-req: 작은 dynamic helper
  — 예 rootfs `/bin/echo`(dynamic) — 가 재-맵으로 깨끗이 실행되는 것을 chromium 전에 확정.)
- **per-exec scoping 의존**: `ALR_REEXEC_INPROC` 가 기본 OFF 이고, 전역 ON 은 onCreate 직렬 프로브를
  wedge 시킨다(round-11). CR-5 는 g1-seqint lane 의 **per-exec scoping + non-blocking supervision**
  (rootfs-내 자식만 재-맵, 직렬 supervision wedge 방지)이 device 로 안정화된 *후*에야 chromium 멀티
  프로세스를 켤 수 있다. — CR-5 는 그 lane 의 *소비자*이지 그 자체가 아니다.

---

## 4. argv/envp/fd 보존

### 4.1 argv 보존 — `/proc/self/exe` 치환에서 argv 는 *건드리지 않는다*

핵심: ADR-003-v2 Option S(stub splice)는 argv 를 `[stub, target, orig argv1..]` 로 **재작성**해야
했다(L2480-2529, 스텁이 argv[1] 에서 타깃을 읽으니까). **하지만 in-process 재-맵(v3)은 argv 를
재작성하지 않는다** — 타깃 host 경로는 레지스터 `x19` 로 직접 전달되고(L2446), argv 는 `x20` 으로 게스트
원본 그대로 넘어간다(L2447-2449, `is_at ? regs[2] : regs[1]`). 트램폴린의 워커는 `x20`(argv)을 그대로
새 SysV 스택에 복사하며 **argv[0] 을 보존**한다(`alr_inproc_reexec.c` L675-683, 주석 "argv[0] stays
as-is").
- 결과: 재-맵된 chromium 자식은 `argv = ["/proc/self/exe", "--type=renderer", "--field-trial-handle=…",
  "--shared-files=…", …]` 를 *원본 그대로* 본다. chromium 은 argv[0] 을 거의 안 쓰고 `--type=` 와 fd
  번호 플래그들만 파싱하므로(§4.3), argv[0] 이 `/proc/self/exe` 인 채여도 무해. (만약 어떤 chromium
  코드가 argv[0]=`/proc/self/exe` 를 다시 stat/open 하면 §1.2 의 Android-바이너리 문제가 재발할 수
  있음 — §5-가정-2 로 격리, device 로 확인.)
- **`/proc/self/exe` 치환은 _타깃 경로(x19)만_ 게스트-절대→host 로 바꾸고 argv/envp 는 불변** — Option
  S 의 argv 재작성보다 *단순*하다. 이것이 v3 가 v2 보다 chromium 에 유리한 또 다른 이유.

### 4.2 envp 보존 — supervisor 통제 + B-3 강제 주입

envp 는 `x21` 로 게스트 원본 그대로 전달(L2450-2452). chromium 이 자식 envp 를 어떻게 구성하든
(ADR-003-v1 §4-가정-1: chromium 이 LD_PRELOAD 를 거를 공산), **B-3(child envp 재주입,
`runtime_report.cpp` L2295~ / `alr_exec.hpp` `decide_exec_envp_*`)이 매 exec trap 에서
`LD_PRELOAD=<abs rootfs interpose .so>`·`ALR_ROOTFS` 를 강제 보장**한다 — envp 는 커널이 아니라
supervisor 가 통제하므로 v1 §4-가정-1 은 이미 해소(ADR-003-v2 §8.3). 단:
- in-process 재-맵은 ld.so 가 LD_PRELOAD 를 읽어 interposer 를 자동 재주입하는 경로다. **AT_SECURE=0**
  을 워커가 auxv 에 명시하므로(L710) 슬래시-포함 LD_PRELOAD(R3 규칙)가 무력화되지 않는다(v1 §4-가정-2
  해소).
- 재-맵 dynamic 경로의 envp 는 워커가 `x21` 을 그대로 새 스택에 복사한다(L685-693). B-3 가 trap 시점에
  이미 envp 를 augment 했다면 그 augment 가 그대로 자식 ld.so 에 전달된다.

### 4.3 fd 상속 — **in-process 재-맵이 자동 보존** (CR-5 의 가장 강한 부분)

chromium 자식은 mojo 채널과 IPC fd 를 **상속된 fd + 명령줄 fd 번호**로 받는다
(`chromium-native-plan` Scout 2: "Mojo IPC = AF_UNIX + SCM_RIGHTS fd passing + memfd shared memory";
chromium 은 `--shared-files=`/`--field-trial-handle=`/mojo `--mojo-platform-channel-handle=<fd>` 등
**명령줄에 fd *번호*를 박아** 자식이 그 번호로 inherited fd 를 집는다).

**execve 라면** close-on-exec 가 아닌 fd 만 살아남고 fd 테이블이 커널에 의해 재구성된다. **그러나
in-process 재-맵은 execve 를 하지 않는다** — syscall 취소(`NT_ARM_SYSTEM_CALL=-1`) + PC-redirect 일
뿐, 프로세스(스레드) 이미지는 fork 자식 그대로다. 따라서:
- **fd 테이블이 전혀 안 바뀐다.** chromium 이 fork 직후 자식에 setup 한 inherited fd(mojo 채널 소켓,
  shared-memory memfd, field-trial handle)가 fd *번호 그대로* 살아있다 — close-on-exec 도 발동 안
  한다(execve 가 없으니).
- 명령줄에 박힌 fd 번호(argv 불변, §4.1)와 실제 fd 테이블이 **일치한 채로 유지**된다 — chromium 의
  fd-번호↔inherited-fd 계약이 깨지지 않는다.
- 이것이 "in-process 재-맵이 fd 상속을 *자연히* 보존한다"의 정확한 의미: **execve 의 fd-재구성 자체가
  일어나지 않으므로 보존할 것도 없다.** (ADR-003-v2 §8.4 가 "커널-execve 가 공짜로 해주는 것" 중 fd
  정리를 이점으로 들었지만, chromium 의 mojo 는 *fd 를 유지*해야 하므로 — execve 의 fd 정리가 오히려
  방해 — v3 의 "execve 안 함"이 chromium 에 *더* 맞다.)

**단 미묘한 점**: in-process 재-맵은 zygote fork 자식의 *주소공간*을 그대로 두고 그 위에 새 ELF 를
매핑한다. fork 자식은 이미 zygote 의 fd 를 상속받았고(clone), 재-맵은 그 fd 를 그대로 둔다. 즉 fork
(clone, fd 상속) → 재-맵(execve 없음, fd 보존)의 2단으로 fd 가 전 구간 보존된다. (renderer 가 zygote-
fork 라 execve 를 아예 안 거치면 — ADR-003-v1 §2-(A) — 애초에 이 트랩에 안 오고 이미 완전 매개; 이
fd 논의는 *fresh-execve* 하는 gpu/zygote 자식에 해당.)

---

## 5. 정직 섹션 — 미검증 가정 / 리스크 (device 없이 못 닫음)

### 5.1 미검증 가정

- **[가정-1] chromium 이 정확히 `/proc/self/exe` 를 exec 하는가(아니면 절대 chrome 경로인가).**
  절대경로면 §2.2-경우2 의 기존 rootfs-rewrite 로 자동 처리되어 §2.1 스칼라조차 불필요. `/proc/self/exe`
  면 §2.2-경우1 치환이 필요. `/proc/<pid>/exe` 변종 여부도 device strace(supervisor-내부 x0 로깅,
  ADR-003-v1 §5 M-R4-execmap)로 확정. — **이 가정이 설계 분기를 가른다.**
- **[가정-2] 재-맵된 chromium 이 argv[0]=`/proc/self/exe`(또는 host 경로)를 다시 열지 않는가.**
  §4.1 — chromium 내부가 argv[0] 을 stat/open 해 리소스(pak/icu)를 찾으면 §1.2 의 Android-바이너리
  문제 재발. chromium 은 보통 `--user-data-dir`/`--resources-dir` 로 리소스를 찾지 자기 바이너리
  경로에 의존하지 않으나, `/proc/self/exe` 기반 리소스 탐색이 남아있으면 깨질 수 있음. device-only.
- **[가정-3] dynamic-PIE 재-맵이 ld.so 클로저를 성공 실행하는가(§3.2).** static SIGILL FIXED ≠
  dynamic ld.so re-map proven. 선행: 작은 dynamic helper(rootfs `/bin/echo`)가 재-맵으로 깨끗이
  종료하는 것을 chromium 전에 device 확정.
- **[가정-4] 직렬 ptrace supervision 이 chromium 의 자식 *폭증*을 감당하는가(§6 리스크 1).**
- **[가정-5] mojo 채널 소켓/memfd 가 재-맵된 자식에서 동작하는가.** fd 번호는 보존되나(§4.3),
  SCM_RIGHTS fd-passing 은 `chromium-native-plan` Scout 4-⑤ 에서 "UNTESTED(supervisor 가 ancillary
  data 를 오독할 수 있음)". memfd shared-memory 는 `--disable-dev-shm-usage` 로 우회 가능하나 mojo
  핵심 채널은 우회 불가. device-only.

### 5.2 정직한 리스크 (분량 기준)

- **[리스크-1·1순위] chromium 의 빠른 다중 프로세스 spawn vs 직렬 ptrace supervision throughput.**
  zygote 는 페이지 로드 시 renderer 를 빠르게 pre-fork 하고 gpu/utility 도 동시에 뜬다. ALR supervisor
  는 단일 루프로 모든 tracee 를 직렬 처리(`waitpid(-1, __WALL)`)하며, 각 exec trap 은 pread/pwrite
  /SETREGSET 라운드트립을 동반한다. 자식이 빠르게 여럿 뜨면 supervision 이 병목/wedge 될 수 있다
  (round-11 이 본 onCreate wedge 의 멀티프로세스 버전). **완화 방향(설계 메모, 구현 아님)**: per-exec
  non-blocking 처리(g1-seqint lane), 또는 1차 CR-5 는 `--renderer-process-limit=1` 등으로 자식 수를
  제한해 throughput 벽을 분리 측정. — 이건 ADR-002 의 syscall-storm 과 *독립*한 별개 벽(자식 *수* vs
  자식 *내* syscall 율).
- **[리스크-2] 샌드박스는 `--no-sandbox` 유지 필수.** `chromium-native-plan` Scout 2·4: chromium 자체
  seccomp 샌드박스 필터는 most-restrictive-wins 로 스택되어 **ALR 이 트랩하려는 path/execve syscall 을
  DENY** 할 수 있다. CR-5 전 구간 `--no-sandbox` 필수(옵션 아님). (in-process 재-맵 자식도 ALR 의
  stacked seccomp 를 상속하므로, chromium 이 자기 필터를 추가로 설치하면 충돌 — `--no-sandbox` 로 차단.)
- **[리스크-3] 재-맵된 자식 간 shared-memory/mojo.** §4.3 은 fd *번호* 보존을 보장하나, 여러 재-맵된
  renderer 가 같은 memfd/소켓을 공유할 때의 정합성(특히 GPU ring 은 `chromium-native-plan` Scout 4-⑥
  에서 "single-child only — 한 SPSC ring, 한 consumer; multi-renderer 는 interleave")은 미검증. 1차
  CR-5 는 `--disable-gpu`(software)로 GPU ring 충돌을 분리, GPU(CR-3 결합)는 후속.
- **[리스크-4] `/proc/self/exe` 치환의 단일-스칼라 가정(§2.1).** chromium 은 자기 자신만 re-exec 하므로
  단일 `guest_self_exe_host` 로 충분하나, 만약 chromium 이 보조 바이너리(예 `chrome_crashpad_handler`)
  를 *별도* `/proc/self/exe` 가 아닌 형태로 exec 하면 그건 §2.2-경우2(절대경로)로 처리됨 — 단일 스칼라가
  잘못된 타깃을 주는 일은 없음(스칼라는 `/proc/self/exe` 케이스에만 쓰임). 안전.

### 5.3 자가 적대검증 (핵심 주장 자기공격)

주장: "`/proc/self/exe` → `guest_self_exe_host` 치환 + 기존 dynamic 재-맵 + argv/envp/fd 자동 보존이면
chromium 멀티프로세스가 매개된다." 공격점:
- **(a) `/proc/self/exe` 치환이 틀린 바이너리를 가리킬 수 있나?** — chromium 자식은 *같은 chrome
  바이너리*를 re-exec 하므로(Scout 2 확정) `guest_self_exe_host` = launch chrome 과 항상 동일. 틀릴
  여지 없음(다른 바이너리는 절대경로 exec → 경우2). **공격 실패.**
- **(b) fd 보존 주장이 틀렸나?** — execve 가 없으면 커널이 fd 테이블을 안 건드린다는 건 커널 사실
  (in-process 재-맵은 PC redirect + mmap 일 뿐). close-on-exec 미발동도 execve 부재의 직접 귀결.
  **공격 실패** (단 fd *번호 보존*과 *mojo 프로토콜 동작*은 다른 층 — 후자는 가정-5 로 격리, 정직).
- **(c) 제약 위반?** — 새 syscall 0, 새 ptrace op 0(기존 SETREGSET/pread/pwrite 만), 새 권한 0,
  커널 execve 0(W^X 우회), 새 execmem 0(트램폴린의 mmap PROT_EXEC 는 round-10 device-proven, W^X 허용),
  version stamp 불변, SELinux 우회 0. **위반 없음.** 깨질 위험이 있는 건 전부 §5.1 device-only 가정으로
  격리.
- **결론**: 설계는 제약을 안 깨고 커널 사실(execve 부재→fd 보존, AT_SECURE=0→LD_PRELOAD 존중)과
  정합한다. 유일한 *실용적* 급소는 **(가정-3) dynamic ld.so 재-맵 정확성**과 **(리스크-1) supervision
  throughput** — 둘 다 chromium 이전에 작은 dynamic helper + 자식-수-제한 프로브로 분리 확정 가능.

### 5.4 "영영 안 될" 시나리오와 목표 재정의

- 가정-3 반증(dynamic ld.so 재-맵이 chromium 클로저에서 실패) + 리스크-1 적중(supervision 이 자식
  폭증에 wedge)이 동시에 참이면, CR-5(멀티프로세스 render)는 현 아키텍처로 불가. 그때의 재정의:
  **CR-1~CR-4 의 `--single-process --no-zygote --no-sandbox` 단일-매개 경로를 usable 상한**으로 두고
  (`chromium-native-plan` Phase B 가 모든 6 gap 을 sidestep), CR-5 는 "renderer-수-제한 + software-only
  + dynamic-helper-proven 후 단계적 확대"로 best-effort 재정의. — ADR-003-v1 §7 / v2 §8.10 의 "fork-자식
  자동 / fresh-exec 자식 best-effort + 한계 문서화" 톤 계승.

---

## 6. device-req (future) + host 검증 가능 범위

### 6.1 deviceReqGate (이 문서의 미래 게이트)
```
DEVICE-REQ: ALR-CR5-multiproc — SM-X236N (am force-stop first); 선행 게이트 통과 필수:
 (g0) 작은 dynamic helper(rootfs /bin/echo)가 in-process 재-맵으로 깨끗이 종료(가정-3),
 (g1) g1-seqint per-exec scoping 이 device-안정(전역 inproc ON 으로 onCreate 완주);
 그 후: chromium-headless-shell --no-sandbox --no-zygote --disable-gpu --disable-dev-shm-usage
 --headless (NO --single-process) about:blank ×1; expect: zygote/renderer fresh-execve 가
 /proc/self/exe→guest_self_exe_host 치환으로 재-맵 진입(`ALR-INPROC: worker target=<rootfs>/.../chrome`
 + `interp=<rootfs>/lib/ld-linux-aarch64.so.1` + `mapped, jumping`), inproc_redirected>0,
 멀티프로세스로 페이지 렌더; gate = renderer 자식이 재-맵 진입(worker 로그) AND 페이지 DOM/픽셀 산출.
 반증: /proc/self/exe 치환 후에도 dynamic ld.so 가 클로저 실행 실패 ⇒ 가정-3(dynamic 재-맵)으로 회귀.
```
(주의: chromium 은 사용자 보류 상태 — `loader-feature-gaps`/`cp6-status`. 이 device-req 는 *future*
이며, 실제 드레인은 보류 해제 후. 단 §6.2 host 모델·`/proc/self/exe` x0 로깅 프로브는 보류와 무관.)

### 6.2 host 에서 지금 검증 가능한 것 (이 문서가 *주장하지 않는* 것과 구분)
- **모델 가능(host)**: `/proc/self/exe`→`guest_self_exe_host` 치환 *결정 로직*을 순수함수로 — 입력
  `(gp, guest_self_exe_host, med)` → 출력 `{치환 host, skip-reason}` 분기표. (`tests/test_execve_
  pathrw.py` 류 결정모델 확장 — *통합 세션 소유*, 이 lane 은 코드 미편집.)
- **모델 불가(device-only)**: 실커널 cancel-execve+PC-redirect, dynamic ld.so 재-맵 실행, fd 테이블
  보존, mojo 채널 동작, supervision throughput — host(darwin)는 ptrace/seccomp/Android-커널 거동
  불가. 전부 §5 device-req 로 격리.

---

## 7. 통합 세션 핸드오프 (정확한, 코드 변경 목록 — 이 lane 은 설계만)

이 문서는 코드를 편집하지 않는다. 통합/WS-1 세션이 배선할 정확한 변경:
1. **loader→supervisor 스레딩**: launch 의 `host_path`(`runtime_report.cpp` L1504, = 게스트 프로그램
   rootfs host 경로)를 supervisor exec-trap in-scope 로 전달(§2.1, 선택지 A/B). 단일 스칼라
   `guest_self_exe_host`.
2. **exec-trap 치환 분기**(`runtime_report.cpp` L2389-2395): `/proc/self/exe`(및 `/proc/<pid>/exe`)
   케이스를 현재의 `proc-self-exe` SKIP 에서 — `guest_self_exe_host` 가 있으면 — `host =
   guest_self_exe_host` 치환으로 바꿔 L2424-2478 정상 재-맵 경로에 진입(§2.2-경우1). `guest_self_exe_
   host` 부재 시 기존 SKIP fallback. `/proc/self/maps` 등 비-exe `/proc/*` 는 계속 SKIP.
3. **argv/envp 변경 0**(§4.1·4.2): in-process 재-맵은 argv/envp 를 x20/x21 로 원본 전달하므로 Option
   S 의 argv 재작성(L2480-2529)은 *불필요* — `/proc/self/exe` 치환은 x19(타깃 host)만 바꾼다.
4. **dynamic 경로 device-검증**(§3.2): chromium 전에 작은 dynamic helper 로 ld.so 재-맵 정확성 확정.
5. **device-verify**: §6.1 DEVICE-REQ (보류 해제 후).

---

관련 파일(절대경로, 이 워크트리):
- 본 설계: `docs/design/chromium-multiprocess-reexec.md` (이 문서, NEW).
- 선행 ADR: `docs/design/adr-003-multiprocess-exec-reentry.md` (§8 ADR-003-v2 Option S DEAD →
  본 문서는 그 후속 ADR-003-v3 in-process 재-맵의 chromium-멀티프로세스 적용).
- G1 SSOT: `docs/research/loader-feature-gaps.md` (G1 exec-re-entry; round-9/10/11 device 갱신).
- chromium 계획: `docs/research/chromium-native-plan.md` (Phase C 멀티프로세스, Scout 2·4 fork+exec
  /seccomp/IPC/JIT).
- supervisor exec-trap(읽기 전용 참조, 미편집): `app/src/main/cpp/runtime_report.cpp` — loader
  `host_path` 산출 L1504, inproc-redirect scoping + `/proc/self/exe` skip L2378-2423, 정상 재-맵
  L2424-2478, Option S splice(불필요분) L2480-2529, B-3 envp 재주입 L2295~.
- resident 재-맵 트램폴린(읽기 전용 참조, 미편집): `app/src/main/cpp/alr_inproc_reexec.h` (entry ABI
  x19-x22), `app/src/main/cpp/alr_inproc_reexec.c` (worker L579~, PT_INTERP/dynamic 분기 L615-649,
  auxv/SP 빌드 L697-728, AT_SECURE=0 L710, argv[0] 보존 L675-683).

Sources(커널/chromium 사실, 본문 인용): [execve(2) — kernel rebuilds fd table per close-on-exec;
argv/envp caller-built](https://man7.org/linux/man-pages/man2/execve.2.html), [ptrace(2) —
PTRACE_SETREGSET/NT_ARM_SYSTEM_CALL syscall cancel, SEIZE across fork/clone](https://www.man7.org/linux/man-pages/man2/ptrace.2.html),
[ld.so(8) — AT_SECURE=0 honors slash-LD_PRELOAD](https://www.man7.org/linux/man-pages/man8/ld.so.8.html),
[Chromium docs — linux/zygote.md (renderers forked; zygote/gpu fresh-exec)](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/linux/zygote.md),
[Mojo Core — AF_UNIX + SCM_RIGHTS fd passing, command-line fd handles](https://chromium.googlesource.com/chromium/src/+/master/mojo/core/README.md),
[seccomp(2) — filters preserved across execve, most-restrictive-wins stacking](https://man7.org/linux/man-pages/man2/seccomp.2.html)
