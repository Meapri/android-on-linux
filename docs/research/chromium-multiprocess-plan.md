# Chromium 멀티프로세스(CR-5) 게이트 맵 — `--single-process` 를 뗐을 때 무엇이 필요한가

> 생성 경위: 메인(통합) 세션, chromium 멀티프로세스 *시험 설계*. **HOST-ONLY**(코드 변경 0,
> device 측정 0; chromium 은 사용자 보류). 이 문서는 "chromium CR-1(`--single-process --no-zygote`)
> 이 in-process 렌더 device-달성(v159)" + "v2 exec child re-map device-검증(dpkg→dpkg-deb/tar/rm/sh
> 5 child re-map, interposer/fakeroot 상속, x21 envp fix; commit `7f45def`/`13f928e`)" 위에서,
> **`--single-process` 를 떼면 chromium 의 멀티프로세스 모델(zygote fork→renderer, browser→gpu
> fresh-execve)이 그 child-re-map 위에서 가능한가**를 한 곳에 정리한 게이트 맵이다.
>
> 코드 사실은 `app/src/main/cpp/runtime_report.cpp`(supervisor exec-trap L2435~, in-process 재-맵
> redirect L2587~, B-3 envp re-injection + x21 mirror L2947~/L3060-3073, EVENT_EXEC L3098~) ·
> `app/src/main/cpp/alr_runtime/alr_exec.cpp`(`decide_exec_path_mediation` L259~,
> `decide_exec_envp_injection` L340~) 직접 확인. 재진입 메커니즘 사실은 **ADR-003-v3**
> (`docs/design/adr-003-multiprocess-exec-reentry.md` §8) + **`/proc/self/exe` 치환 설계**
> (`docs/design/chromium-multiprocess-reexec.md`)에서 인용 — **이 문서는 그 둘과 중복 설계를 다시
> 쓰지 않고**, "MP 게이트 맵 + device 플래그/DEVICE-REQ + 본체 제안 diff" 의 SSOT 역할만 한다.
>
> 상태: **Proposed (device-pending)**. chromium 은 사용자 보류 — 실제 드레인 착수는 보류 해제 +
> 선행 게이트(G0/G1) device-PASS 후. HARD CONSTRAINTS(불변): 비root(untrusted_app, no
> CAP_SYS_ADMIN), public Android API only, SELinux 우회 금지, W^X-safe(새 execmem 0 — 커널 execve 0),
> in-process, 새 ptrace op 0, version stamp 불변, device evidence 없이 완료/RUNS 주장 금지.

---

## 0. 한 줄 결론

**`--single-process` 를 떼면 자식이 두 클래스로 갈린다 — (A) zygote 가 `fork()` 하는 renderer 다수는
execve 를 안 거치므로 fork 자식의 주소공간 복제 + 상속 seccomp/SEIZE 로 _이미 매개_(추가 작업 0,
ADR-003-v1 §2-A; v2 dpkg→sh fork 체인으로 device-방증), (B) browser 가 `fork()+execve("/proc/self/exe",
…)` 하는 zygote 자신·gpu·utility 만 진짜 exec 벽이다.** (B) 는 v2 에서 device-검증된 **exec child
re-map**(in-process 재-맵, 커널 execve 0)과 **동일 메커니즘** 위에 있으나, v2 가 다룬 dpkg 체인은 자식이
*다른 rootfs 절대경로 바이너리*(`/usr/bin/dpkg-deb` 등)를 exec 한 반면 — chromium 자식은 **자기 자신을
`/proc/self/exe` 로 re-exec** 한다. 그 `/proc/self/exe` 는 ALR 케이스에선 *Android* 로더 바이너리(interp
`/system/bin/linker64`)로 resolve 되어 트램폴린이 rootfs 로 매핑 불가 → **현재 의도적으로 SKIP**
(`runtime_report.cpp` L2622-2624 `proc-self-exe`). 따라서 chromium MP 의 **유일한 신규 작업은
"`/proc/self/exe` → 알려진 게스트 바이너리(host_path) 치환"** 한 조각이고(설계 = `chromium-multiprocess-
reexec.md`), 그 외 (argv/envp/fd 보존, B-3 envp 강제주입, EVENT 처리)는 v2 코드가 **이미** 처리한다.

**v2 가 증명한 것과 chromium MP 가 추가로 요구하는 것의 정확한 경계:**

| 축 | v2 (dpkg 체인) device-검증 | chromium MP 가 추가로 요구 |
|---|---|---|
| fork 자식 (no exec) | dpkg→sh fork, fd/주소공간 상속 | **동일** — renderer = zygote-fork, 추가 0 |
| fresh-execve 자식 | `/usr/bin/dpkg-deb` 등 **rootfs 절대경로** → re-map | `/proc/self/exe` → **치환 필요**(절대경로 아님) |
| argv 재작성 | in-proc 재-맵은 argv 불변(x20 원본) | **동일** — 치환은 x19(타깃)만, argv 불변 |
| envp 강제주입 | B-3 + x21 mirror(`7f45def`) | **동일** — chromium 이 LD_PRELOAD 걸러도 B-3 가 강제 |
| fd 상속 | execve 없음 → fd 테이블 불변 | **동일이 _더_ 유리** — mojo 는 fd 보존을 *원함* |
| **신규** | — | **`/proc/self/exe` 치환** + supervision throughput + per-renderer GPU ring |

즉 **chromium MP 는 v2 의 exec-re-map 트랙에 "한 입력 케이스(`/proc/self/exe`)"를 더하는 것**이지 새
메커니즘이 아니다 — 새 ptrace op 0, 새 권한 0, 커널 execve 0, 새 execmem 0.

---

## 1. MP 게이트 맵 (`--single-process` 제거 시 어디서 무엇이 깨지나)

```
chromium (NO --single-process, --no-zygote, --no-sandbox)
        │
        ├── browser process (= ALR 가 in-process 로 launch 한 그 프로세스)
        │     ALR 매개: ✅ launch-time map+jump (CR-1 에서 device-RUNS)
        │
        ├── fork() ───► renderer ×N        [클래스 A: no execve]
        │     ALR 매개: ✅ 자동 (fork = 주소공간 복제 + seccomp/SEIZE 상속; v2 dpkg-fork 로 방증)
        │     게이트: 없음 (이미 매개) — 단 supervision throughput(§3 리스크-1)
        │
        └── fork()+execve("/proc/self/exe", ["--type=zygote|gpu-process|utility", …fd]) 
              [클래스 B: fresh-execve, 자기 재실행]
              ALR 매개: ⚠️  exec-trap 에 도달하나 현재 `proc-self-exe` 로 SKIP (L2622-2624)
              ┌─────────────────────────────────────────────────────────────────┐
              │ GATE-1 (★ 신규, 유일): /proc/self/exe → host_path 치환            │
              │   현재: SKIP → 커널이 execve 진행 → Android linker64 가 로드(붕괴) │
              │   필요: 치환 후 ADR-003-v3 in-proc 재-맵(커널 execve 0)에 태움    │
              ├─────────────────────────────────────────────────────────────────┤
              │ GATE-2: dynamic-PIE 재-맵 정확성 (chromium 은 ~200 .so dynamic)   │
              │   = G1 의존 (static SIGILL 은 r11 FIXED; dynamic ld.so 재-맵은     │
              │     wired-but-device-未검증) — 선행 G0(작은 dynamic helper)       │
              ├─────────────────────────────────────────────────────────────────┤
              │ GATE-3: Mojo IPC (AF_UNIX + SCM_RIGHTS + memfd) 자식에서 동작     │
              │   fd *번호* 보존은 GATE-1 의 부수효과(execve 없음 → fd 불변)      │
              │   SCM_RIGHTS/memfd 의 supervisor 상호작용은 §2 에서 분리          │
              └─────────────────────────────────────────────────────────────────┘
```

**핵심**: GATE-1 이 chromium MP 의 **유일한 신규 코드**다. GATE-2 는 G1(별도 lane, in-flight)의
의존이고, GATE-3 는 대부분 GATE-1 의 부수효과(execve 부재 → fd 보존)다. v2 가 다룬 dpkg 체인은
GATE-1 을 **건드리지 않는다**(dpkg 자식은 `/proc/self/exe` 가 아니라 절대경로 exec → 기존 rewrite
경로로 처리됨) — 그래서 chromium MP 가 v2 위에서 *추가로* 요구하는 것이 정확히 GATE-1 한 조각이다.

### 1.1 GATE-1 — `/proc/self/exe` 치환 (이미 설계 완료, 코드 1조각)

설계 SSOT 는 **`docs/design/chromium-multiprocess-reexec.md`** (§2.1 loader→supervisor 스레딩, §2.2
exec-trap 3-경우 치환, §2.3 치환 후 기존 재-맵 경로). 그 설계가 요구하는 코드 변경의 핵심:

> **`runtime_report.cpp` L2622-2624 의 `proc-self-exe` SKIP 을, `host_path`(launch 시점에 계산된
> 게스트 프로그램의 rootfs host 경로)가 있으면 "치환 후 재-맵" 으로 바꾼다.**

**★ 통합 세션을 위한 새 사실(이 문서가 코드를 읽어 확정):** `chromium-multiprocess-reexec.md` §2.1 은
`host_path` 를 supervisor 로 "스레드"하는 선택지 A/B 를 논했지만 — **그 스레딩은 불필요하다.** launch +
supervisor 루프가 **같은 함수(`build_native_loader_probe`, L1478~) 안에** 있고, `host_path` 는 L1507 에
선언되어 fork(L1742) 후 부모(=supervisor) 분기의 exec-trap(L2622)까지 **이미 in-scope** 다. 즉
`guest_self_exe_host` 라는 새 변수/인자/필드를 추가할 필요 없이, exec-trap 에서 **`host_path` 를 그대로
참조**하면 된다. (chromium 자식은 전부 같은 chrome 바이너리를 re-exec 하므로 단일 스칼라로 충분 —
`chromium-multiprocess-reexec.md` §2.1 의 "멀티-바이너리 추적 자료구조 불필요" 와 정합.) → §6 제안 diff
참조.

### 1.2 GATE-2 — dynamic-PIE 재-맵 (G1 의존, 이 문서의 범위 밖이나 명시)

chromium glibc 바이너리는 `~200` .so 를 dlopen 하는 **dynamic-PIE(ET_DYN + PT_INTERP=
`/lib/ld-linux-aarch64.so.1`)**. GATE-1 치환으로 host 경로가 정해지면 트램폴린의 *dynamic* 경로
(`alr_inproc_reexec.c` L633-649: `<rootfs><interp>` ld.so 매핑 → AT_BASE=ld.so base → ld.so entry
점프)가 적용된다. **정직**: round-11 device 는 *static* `/bin/sh` 로 매퍼 정확성을 입증했고 dynamic
경로는 "wired-but-device-未검증". 따라서 GATE-1 을 코딩해도 GATE-2(dynamic ld.so 재-맵이 ~200 .so
클로저를 성공 실행)는 **선행 G0** — 작은 dynamic helper(rootfs `/bin/echo` 같은 dynamic ELF)가
in-process 재-맵으로 깨끗이 종료하는 것 — 가 chromium *전에* device-확정돼야 한다. (G1 SSOT =
`docs/research/loader-feature-gaps.md`; g1-seqint per-exec scoping = `docs/research/r12-remaining-
gaps-status.md`.)

### 1.3 GATE-3 — Mojo IPC (대부분 GATE-1 의 부수효과; §2 에서 분리)

`--single-process` 가 우회하던 Mojo(`chromium-native-plan.md` Scout 2·4-⑤: AF_UNIX + SCM_RIGHTS
fd-passing + memfd 공유메모리)가 MP 에서 surface. 아래 §2 가 supervisor 가 무엇을 처리해야/안 해도
되는지 분리한다.

---

## 2. Mojo IPC(AF_UNIX + memfd + SCM_RIGHTS) supervisor 처리

chromium 자식은 mojo 채널/IPC fd 를 **상속된 fd + 명령줄 fd 번호**로 받는다(`--mojo-platform-channel-
handle=<fd>`, `--shared-files=`, `--field-trial-handle=`). MP 에서 이 fd 계약이 살아야 한다.

### 2.1 fd 번호 보존 — supervisor 가 **아무것도 안 해도** 보존됨 (GATE-1 의 부수효과)

**핵심 커널 사실**: in-process 재-맵은 **execve 를 하지 않는다**(syscall 취소 `NT_ARM_SYSTEM_CALL=-1`
L2691-2698 + PC-redirect, 프로세스/스레드 이미지는 fork 자식 그대로). 따라서:

- **fd 테이블이 전혀 안 바뀐다** — chromium 이 fork 직후 setup 한 inherited fd(mojo 채널 소켓,
  shared-memory memfd, field-trial handle)가 fd *번호 그대로* 살아있다.
- **close-on-exec 도 발동 안 한다**(execve 가 없으니) — 명령줄에 박힌 fd 번호(argv 불변, GATE-1 은
  x19 타깃만 바꾸고 argv/envp 는 원본 x20/x21) 와 실제 fd 테이블이 **일치한 채 유지**된다.
- 이것이 "in-process 재-맵이 fd 상속을 *자연히* 보존한다"의 정확한 의미: **execve 의 fd-재구성 자체가
  안 일어나므로 보존할 것도 없다.** (ADR-003-v2 §8.4 는 커널-execve 의 fd 정리를 *이점*으로 들었으나,
  mojo 는 fd 를 *유지*해야 하므로 — execve 의 fd 정리가 오히려 방해 — **v3 의 "execve 안 함" 이
  chromium 에 _더_ 맞다**.)
- **이 부분은 v2 가 이미 device-방증**: dpkg→dpkg-deb/tar 체인이 재-맵 후 파이프/fd 로 데이터를
  주고받으며 unpack 진행(commit `7f45def` "exec child data extract re-map (configured)")했다 — fd 가
  재-맵을 가로질러 살아있다는 직접 증거.

→ **supervisor 처리 = 0** (fd 번호 보존은 메커니즘의 부수효과). 이것이 GATE-3 의 가장 강한 부분이며,
chromium MP 가 fd 측면에서 v2 dpkg 체인보다 *어렵지 않은* 이유다.

### 2.2 SCM_RIGHTS fd-passing — supervisor 가 **방해하지 않아야** 함 (UNTESTED, device-only)

mojo 는 런타임에 `sendmsg`/`recvmsg` 의 ancillary data(`SCM_RIGHTS`)로 fd 를 *프로세스 간 전달*한다
(fork 시점 상속이 아니라 동작 중 전달). `chromium-native-plan.md` Scout 4-⑤: **"SCM_RIGHTS fd-passing
UNTESTED (supervisor 가 ancillary data 를 오독할 수 있음)".**

- **supervisor 가 sendmsg/recvmsg 를 트랩하나?** — 아니다. PCGATE BPF 는 **9 path nr 만 RET_TRACE**
  (`cp6-status.md` §5-(a): clone/clone3/futex/mmap/sendmsg/recvmsg 전부 RET_ALLOW). sendmsg/recvmsg
  는 path syscall 이 아니므로 **EVENT_SECCOMP 트랩 자체가 안 걸린다** → supervisor 가 ancillary data 를
  건드릴 기회가 없다. "오독" 리스크는 *supervisor 가 그 syscall 을 트랩해 인자를 rewrite 할 때만* 발생
  하는데, 트랩을 안 하므로 SCM_RIGHTS 는 **커널이 직접 처리**(supervisor 우회).
- **단 미확정**: SCM_RIGHTS 로 전달되는 fd 가 가리키는 객체(AF_UNIX 소켓/memfd)가 재-맵된 자식의
  주소공간에서 유효한가는 device-only(§5-가정). fd 번호 보존(§2.1)은 *상속* fd 에 대한 것이고,
  *동작 중 전달* fd 의 정합성은 mojo 프로토콜 레벨 — host(darwin)로 모델 불가.

→ **supervisor 처리 = 0(트랩 안 함이 정답)**. DEVICE-REQ 로 격리(§4 ALR-CR5-mojo).

### 2.3 memfd 공유메모리 — `--disable-dev-shm-usage` + memfd-exec 회피

- chromium 은 `/dev/shm` 대신 memfd 로 공유메모리를 만든다(`--disable-dev-shm-usage` 가 `/dev/shm`
  부재를 우회). **memfd *생성*(`memfd_create`)은 path syscall 아님 → 트랩 안 함 → supervisor 처리 0.**
- **단 memfd-*execveat* 은 BLOCKED**(`chromium-native-plan.md` Scout 4-③: EACCES, SELinux). chromium
  은 공유메모리 memfd 를 *실행*하지 않으므로(데이터용) 무관 — **단 V8 이 memfd-exec 로 JIT 를 하지
  않는다는 확인 필요**. v120 JIT-WX 프로브가 "anon mmap RW→RX + 직접 RWX mmap 동작, `--jitless` 불요"
  를 device-PASS(`chromium-native-plan.md` JIT 섹션)했으므로 V8 은 anon execmem 경로(memfd 아님)를
  쓴다 — memfd-exec 차단과 충돌 안 함.
- → **supervisor 처리 = 0**. memfd 데이터는 커널 처리, JIT 는 v120 검증된 anon execmem.

### 2.4 요약 — Mojo 에 대한 supervisor 신규 작업

| Mojo 요소 | supervisor 가 트랩? | 신규 작업 | 근거 |
|---|---|---|---|
| 상속 fd (mojo 채널/handle, 명령줄 번호) | 아니오 | **0** (execve 부재 → fd 불변, §2.1) | v2 device-방증 |
| SCM_RIGHTS fd-passing (sendmsg/recvmsg) | 아니오 (비-path nr) | **0** (트랩 안 함 = 오독 없음, §2.2) | PCGATE 9-path-only |
| memfd 공유메모리 | 아니오 (비-path nr) | **0** (커널 처리, §2.3) | Scout 4 |
| memfd-execveat (만약 V8 이 쓰면) | — | 회피 (anon execmem, v120) | JIT-WX 프로브 PASS |

**결론: Mojo 는 supervisor 신규 코드 0.** chromium MP 가 mojo 측면에서 요구하는 것은 *플래그*
(`--no-sandbox`, `--disable-dev-shm-usage`)와 *메커니즘 부수효과*(execve 부재 → fd 보존)뿐이며, 실제
mojo 프로토콜 동작은 device-only 검증(§4 ALR-CR5-mojo). 이는 GATE-1 한 조각이 chromium MP 의 유일한
신규 코드라는 §1 결론을 강화한다.

---

## 3. `/proc/self/exe` exec — v158 처리와의 관계 (혼동 방지)

**중요한 구분**: 코드에는 `/proc/self/exe` 가 **두 곳**에 나오는데 의미가 정반대다.

### 3.1 launch-time `ALR_GUEST_EXE` (v158, 이미 처리됨) — 게스트가 *읽는* `/proc/self/exe`

`runtime_report.cpp` L1595-1600:
```cpp
// it for readlink("/proc/self/exe") — the real /proc/self/exe of this in-process
// path mediation maps the asset open. Only used when guest_rel is absolute.
guest_env.push_back("ALR_GUEST_EXE=" + guest_rel);
```
이건 게스트(또는 ld.so/interposer)가 `readlink("/proc/self/exe")` 로 *자기 경로를 질의*할 때, Android
로더 경로 대신 **게스트 절대경로**(`guest_rel`, 예 `/opt/chromium/chrome`)를 돌려주도록 `ALR_GUEST_EXE`
env 로 알려주는 것 — **readlink/open 매개**다. 이건 CR-1(single-process)에서 이미 device-동작하며,
"게스트가 `/proc/self/exe` 를 *읽는* 것" 을 푼다.

### 3.2 exec-trap `proc-self-exe` SKIP (GATE-1 의 대상) — 게스트가 *exec 하는* `/proc/self/exe`

`runtime_report.cpp` L2622-2624 (exec-trap 안):
```cpp
if (std::strcmp(gp, "/proc/self/exe") == 0 ||
    std::strncmp(gp, "/proc/", 6) == 0) {
    inproc_skip_reason = "proc-self-exe";
}
```
이건 게스트가 `execve("/proc/self/exe", …)` 로 **자기를 re-exec** 할 때, 트램폴린이 매핑할 host 경로를
모르므로(`/proc/self/exe` → Android linker64) **재-맵을 SKIP** 하는 것. **이게 GATE-1 이 바꾸는 지점**
이다 — §3.1 의 readlink 매개(ALR_GUEST_EXE)와 **별개**.

### 3.3 둘의 합류 — GATE-1 은 §3.2 를 §3.1 과 같은 답으로 푼다

GATE-1 의 치환 타깃 `host_path`(= `config.rootfs_dir + guest_rel`, L1507)는 **§3.1 의 `guest_rel` 과
같은 게스트를 가리킨다**. 즉:
- §3.1: 게스트가 `/proc/self/exe` 를 *읽으면* → `ALR_GUEST_EXE`=`guest_rel`(게스트 절대경로) 반환.
- §3.2/GATE-1: 게스트가 `/proc/self/exe` 를 *exec 하면* → `host_path`=`rootfs_dir+guest_rel`(host 경로)
  로 치환 후 재-맵.

**두 경로가 같은 launch 게스트로 수렴** — chromium 자식은 전부 같은 chrome 바이너리를 re-exec 하므로
일관(`chromium-multiprocess-reexec.md` §5.3-(a) "치환이 틀린 바이너리를 가리킬 수 없다"). v158 이 *읽기*
를 풀었으니 GATE-1 은 *exec* 를 같은 사실 위에서 푸는 것이다.

### 3.4 `/proc/<pid>/exe` 변종 주의

현 L2623 `std::strncmp(gp, "/proc/", 6)` 는 *모든* `/proc/*` 를 `proc-self-exe` 로 분류한다. GATE-1 은
그 중 **exec 타깃이 될 수 있는 것**(`/proc/self/exe` 또는 `/proc/<digits>/exe`)만 치환 대상으로
좁혀야 하고, 그 외 `/proc/*`(예 `/proc/self/maps`, `/proc/cpuinfo`)는 exec 타깃이 아니므로 계속 SKIP.
chromium 이 실제로 어떤 형태(`/proc/self/exe` vs `/proc/<pid>/exe` vs 절대 chrome 경로)를 쓰는지는
device strace(supervisor-내부 x0 로깅, 이미 `first_exec_x0` L2583-2585 로 1줄 잡힘)로 확정(§5-가정-1).

---

## 4. device 시험 플래그셋 (`--no-zygote` 단계) + DEVICE-REQ

### 4.1 플래그 사다리 — `--single-process` 제거를 두 단계로

CR-1..CR-4 는 `--single-process` 를 유지하므로 MP 벽을 안 건드린다. CR-5 는 그것을 떼는데, **한 번에
다 떼지 말고** exec 벽을 단계적으로 좁힌다:

```
[기준선 = CR-1, device-RUNS v159]
  chromium-headless-shell --single-process --no-zygote --no-sandbox \
    --disable-gpu --disable-dev-shm-usage --user-data-dir=<rootfs-writable> \
    --dump-dom data:text/html,<html><body><h1>alr</h1></body></html>
  → exec 자식 0 (MP 벽 없음). 이미 device-PASS.

[CR-5-step-A = exec 벽을 _만들되 최소_]  ← GATE-1 의 첫 device 노출
  chromium-headless-shell --no-sandbox --disable-gpu --disable-dev-shm-usage \
    --no-zygote                         (각 자식이 fresh-execve(/proc/self/exe); COW zygote 우회) \
    --renderer-process-limit=1          (자식 _수_ 제한 → supervision throughput 벽 분리, §리스크-1) \
    --user-data-dir=<rootfs-writable> \
    --dump-dom data:text/html,<h1>alr</h1>
  → NO --single-process. 자식 = browser + (zygote 없이 직접) renderer×1 + gpu(가능).
    기대: 각 fresh-execve 자식이 GATE-1 치환 → ALR-INPROC 재-맵 진입.

[CR-5-step-B = 자식 수 풀기]
  CR-5-step-A 에서 --renderer-process-limit 제거 → renderer 다수 + supervision throughput 실측.

[CR-5-full = CR-3/CR-4 합성]
  += --use-gl=angle … (GPU, per-renderer ring) + --ozone-platform=wayland (display).
```

- **`--no-zygote` 의 역할**: 각 자식이 zygote-COW-fork 대신 **fresh-execve(`/proc/self/exe`)** 로 뜬다
  (`chromium-native-plan.md` Scout 2: "`--no-zygote` makes each a fresh exec — easier to mediate than
  a zygote COW fork"). 이게 GATE-1(치환)을 **모든 자식에 균일하게** 태우는 가장 단순한 형태 → 1차
  CR-5 는 `--no-zygote` 로 zygote-COW 의 미묘함(2계층 zygote, unsandboxed zygote)을 우회.
- **`--no-zygote` 라도 fork 자식은 여전히 존재?** — `--no-zygote` 는 *zygote* 를 없애지만 browser 가
  자식을 띄우는 방식은 여전히 `fork()+execve(/proc/self/exe)` 다(zygote 의 COW-fork 만 사라짐). 즉
  CR-5-step-A 의 모든 자식이 클래스 B(fresh-execve) → GATE-1 한 경로로 수렴. (클래스 A=zygote-fork 는
  `--no-zygote` 를 *떼야* 나타나며, 그건 CR-5 이후 단계.)
- **`--renderer-process-limit=1`**: 자식 *수* 를 제한해 supervision throughput 벽(§리스크-1)을 GATE-1
  정확성과 *분리* 측정. (자식 *내* syscall 율 = ADR-002 storm 과 독립한 별개 벽 — 자식 *수*.)
- **`--no-sandbox` 필수(옵션 아님)**: chromium 자체 seccomp 가 ALR 의 path/execve 트랩 syscall 을
  DENY 할 수 있다(most-restrictive-wins, Scout 2·4-④). 전 구간 필수.
- **`--disable-gpu` (1차)**: GPU ring SPSC 단일-consumer 충돌(Scout 4-⑥)을 분리 — software 렌더로 MP
  exec 벽만 본다. GPU(CR-3 결합)는 GATE-1 + dynamic 재-맵이 device-PASS 된 후.

### 4.2 DEVICE-REQ 마커

> **선행 게이트(이게 PASS 돼야 CR-5 착수 의미 있음):**
> ```
> DEVICE-REQ: ALR-CR5-G0-dynhelper — SM-X236N (am force-stop first); ALR_REEXEC_INPROC=1;
>  rootfs 의 작은 _dynamic_ helper(예 /bin/echo, ET_DYN+PT_INTERP) 를 게스트가 execve;
>  gate = `ALR-INPROC: worker target=<rootfs>/bin/echo` + `interp=<rootfs>/lib/ld-linux-aarch64.so.1`
>   + `mapped, jumping` + child exit=0 (dynamic ld.so 재-맵이 DT_NEEDED 클로저 실행). 
>  반증 = SIGILL/crash ⇒ GATE-2(dynamic 재-맵) 미해결, CR-5 보류. (G1 lane 소관.)
> ```
> ```
> DEVICE-REQ: ALR-CR5-G1-seqint — g1-seqint per-exec scoping 이 device-안정(전역 inproc ON 으로
>  onCreate 프로브 시퀀스 완주, wedge 없음). (r12 g1-seqint lane 소관 — CR-5 는 소비자.)
> ```

> **CR-5 본체(선행 게이트 통과 + 사용자 보류 해제 후):**
> ```
> DEVICE-REQ: ALR-CR5-stepA — SM-X236N (am force-stop first); ALR_REEXEC_INPROC=1;
>  chromium-headless-shell --no-sandbox --no-zygote --disable-gpu --disable-dev-shm-usage
>   --renderer-process-limit=1 --user-data-dir=<rootfs-writable>
>   --dump-dom data:text/html,<html><body><h1>alr</h1></body></html>  (NO --single-process) ×1;
>  expect (supervisor 내부 집계, 외부 strace 금지):
>   (i) `alr exec x0=/proc/self/exe`(또는 /proc/<pid>/exe) — chromium 이 자기 재실행을 쓰는지 확정,
>   (ii) GATE-1 치환 후 `ALR-INPROC: worker target=<rootfs>/.../chrome`
>        + `interp=<rootfs>/lib/ld-linux-aarch64.so.1` + `mapped, jumping`,
>   (iii) `inproc_redirected>0` AND fresh-execve 자식이 ALR 매개 아래 RUNS,
>   (iv) `child exit=0` + guest stdout DOM 에 `<h1>alr</h1>` 담은 `</html>`;
>  gate = (ii)+(iii) — fresh-execve 자식이 /proc/self/exe→host_path 치환으로 재-맵 진입 AND DOM 산출.
>  반증: 치환 후에도 `exec_events` 가 커널-execve 로 진행(Android linker64 로드) ⇒ GATE-1 치환 미배선,
>        또는 치환 후 dynamic ld.so 가 클로저 실행 실패 ⇒ GATE-2(G0 로 회귀).
> ```
> ```
> DEVICE-REQ: ALR-CR5-mojo — (ALR-CR5-stepA 의 _부수_ 관측) fresh-execve 자식이 재-맵 진입한 뒤
>  mojo 채널이 동작하는가: gate = 자식이 browser 와 IPC 핸드셰이크 완료(렌더 결과가 browser 로
>  돌아와 DOM 직렬화). 반증 = 자식은 재-맵됐으나(ii PASS) IPC 가 끊겨 DOM 미산출 ⇒ §5-가정(SCM_RIGHTS
>  /memfd 정합성) device-반증, mojo 레벨 디버그 필요(supervisor 코드 아님).
> ```
> ```
> DEVICE-REQ: ALR-CR5-stepB — stepA PASS 후 --renderer-process-limit 제거 → renderer 다수;
>  gate = 다수 fresh-execve 자식이 전부 재-맵 진입 AND supervision 이 wedge 안 됨(직렬 ptrace
>  throughput). 반증 = N 자식에서 supervision wedge ⇒ §리스크-1(throughput 벽) — per-exec
>  non-blocking 처리(g1-seqint) 필요. 이건 GATE-1 과 _독립한_ 별개 벽(자식 수).
> ```

---

## 5. 정직 섹션 — 미검증 가정 / 리스크 (device 없이 못 닫음)

### 5.1 미검증 가정

- **[가정-1] chromium 이 정확히 `/proc/self/exe` 를 exec 하는가(아니면 절대 chrome 경로/`/proc/<pid>
  /exe`).** 절대경로면 `decide_exec_path_mediation` 의 기존 "rewrite" 경로로 *자동* 처리 → GATE-1
  코드조차 불필요. `/proc/self/exe` 면 GATE-1 치환 필요. `first_exec_x0`(L2583) 가 device 로 1줄에
  답한다. **이 가정이 GATE-1 의 필요성 자체를 가른다** — 그래서 ALR-CR5-stepA 의 (i) 가 1순위 관측.
- **[가정-2] 재-맵된 chromium 이 argv[0]=`/proc/self/exe`(또는 host 경로)를 다시 stat/open 하지
  않는가.** in-proc 재-맵은 argv[0] 을 *보존*(원본 `/proc/self/exe`)하는데, chromium 내부가 그것으로
  리소스(pak/icu)를 찾으면 §3.2 의 Android-바이너리 문제 재발. chromium 은 보통 `--user-data-dir`
  /resources-dir 로 찾으나, §3.1 의 `ALR_GUEST_EXE` readlink 매개가 이걸 *완화*한다(자기 경로 질의가
  게스트 절대경로를 받음). device-only.
- **[가정-3] dynamic-PIE 재-맵이 ~200 .so 클로저를 성공 실행(GATE-2).** static SIGILL FIXED(r11) ≠
  dynamic ld.so re-map proven. 선행 G0(ALR-CR5-G0-dynhelper). CR-5 는 이 가정에 **강하게 의존**.
- **[가정-4] 직렬 ptrace supervision 이 chromium 자식 *폭증* 을 감당(리스크-1).** ALR-CR5-stepB 가
  `--renderer-process-limit` 제거로 측정.
- **[가정-5] mojo 채널 소켓/SCM_RIGHTS fd/memfd 가 재-맵된 자식에서 동작.** fd *번호* 는 보존되나
  (§2.1), SCM_RIGHTS fd-passing 은 Scout 4-⑤ "UNTESTED", mojo 핵심 채널은 우회 불가. ALR-CR5-mojo
  로 격리. host(darwin)는 mojo 프로토콜/실커널 socket 거동 불가 → device-only.

### 5.2 정직한 리스크 (분량 기준)

- **[리스크-1·1순위] chromium 의 빠른 다중 spawn vs 직렬 ptrace supervision throughput.** zygote/browser
  가 renderer 를 빠르게 pre-fork 하고 gpu/utility 도 동시에 뜬다. ALR supervisor 는 단일 루프
  (`waitpid(-1, __WALL)` L2381)로 모든 tracee 를 *직렬* 처리하며, 각 exec trap 은 pread/pwrite
  /SETREGSET 라운드트립을 동반. 자식이 빠르게 여럿 뜨면 supervision 이 병목/wedge(round-11 onCreate
  wedge 의 MP 버전; CR-1 의 600s 드레인이 "단일 프로세스도 ~2min 침묵" 을 본 것과 같은 계열).
  **완화(설계 메모, 구현 아님)**: per-exec non-blocking 처리(g1-seqint lane), 또는 1차 CR-5 는
  `--renderer-process-limit=1` 로 자식 수 제한해 throughput 벽을 GATE-1 정확성과 분리 측정. — 이건
  ADR-002 의 syscall-storm 과 *독립*(자식 *수* vs 자식 *내* syscall 율).
- **[리스크-2] `--no-sandbox` 필수(§4.1).** chromium 자체 seccomp 가 ALR 의 path/execve 트랩을 DENY
  할 수 있다(most-restrictive-wins). 전 구간 필수 — 옵션 아님. (in-proc 재-맵 자식도 ALR stacked
  seccomp 를 상속하므로 chromium 이 자기 필터를 추가 설치하면 충돌 → `--no-sandbox` 로 차단.)
- **[리스크-3] per-renderer GPU ring(Scout 4-⑥).** 현 SPSC 단일-consumer ring 은 multi-renderer 가
  interleave. 1차 CR-5 는 `--disable-gpu`(software)로 분리, GPU(CR-3 결합)는 후속 — N-ring/tagging
  필요. (`docs/research/chromium-gpu-path.md` 소관.)
- **[리스크-4] supervision wedge 의 2차 가설(SEIZE-induced).** CR-1 의 "render 데드락" 재진단
  (`adr-chromium-storm-deadlock.md`)이 best-가설 = window-too-short 로 좁혔으나 SEIZE-전환이 *새*
  group-stop 데드락을 유발했을 가능성은 device 전 배제 불가. MP 는 자식 수가 더 많아 이 표면이 커짐 →
  ALR-CR5-stepB 가 N-자식에서 재확인.

### 5.3 자가 적대검증 (핵심 주장 자기공격)

주장: "chromium MP 의 유일한 신규 코드는 GATE-1(`/proc/self/exe`→host_path 치환) 한 조각이고, 나머지
(argv/envp/fd 보존, Mojo, EVENT 처리)는 v2 코드가 이미 처리한다." 공격점:

- **(a) Mojo 가 정말 supervisor 신규 코드 0 인가?** — sendmsg/recvmsg/memfd_create 가 비-path nr 이라
  PCGATE 가 트랩 안 함(`cp6-status.md` §5-(a))은 코드 사실. fd 번호 보존은 execve 부재의 직접 귀결
  (커널 사실). **그러나 mojo 프로토콜 *동작*(SCM_RIGHTS fd 가 가리키는 객체 정합성)은 다른 층** —
  §2.2/가정-5 로 격리, 정직. "supervisor 코드 0" 은 맞으나 "mojo 가 device 에서 동작" 은 미확정. **부분
  성립.**
- **(b) GATE-1 이 정말 한 조각인가, 아니면 argv 재작성도 필요한가?** — in-proc 재-맵(v3)은 argv 를
  x20 으로 원본 전달(L2677-2679)하므로 Option S 의 argv 재작성(L2730~)이 *불필요*. GATE-1 은 x19(타깃
  host)만 바꾼다. 코드 확인: 치환은 `inproc_skip_reason` 분기(L2617-2653) 한 곳을 바꾸면 L2654 이후
  기존 재-맵 경로가 그대로 탄다. **성립.**
- **(c) `host_path` 스레딩이 정말 불필요한가?** — `build_native_loader_probe`(L1478) 한 함수 안에
  launch(host_path L1507) + fork(L1742) + supervisor 루프(L2381) + exec-trap(L2622)이 전부 있어
  `host_path` 가 exec-trap 까지 in-scope. **코드 확인 성립** — `chromium-multiprocess-reexec.md` §2.1
  의 선택지 A/B(인자/필드 추가)보다 *더 단순*(변수 그대로 참조).
- **(d) 제약 위반?** — 새 syscall 0, 새 ptrace op 0(기존 SETREGSET/pread/pwrite), 새 권한 0, 커널
  execve 0(W^X 우회), 새 execmem 0(트램폴린 mmap PROT_EXEC 는 round-10 device-proven), version stamp
  불변, SELinux 우회 0. **위반 없음.**
- **결론**: 설계는 제약을 안 깨고 커널 사실(execve 부재→fd 보존, AT_SECURE=0→LD_PRELOAD 존중)과
  정합. 유일한 *실용적* 급소는 **(가정-3) dynamic ld.so 재-맵 정확성**(G0 선행)과 **(리스크-1)
  supervision throughput**(stepB) — 둘 다 chromium 전에 작은 helper + 자식-수-제한으로 분리 확정 가능.
  "GATE-1 한 조각" 주장은 *코드 변경 분량* 으로는 성립하나, *device-동작* 은 G0/가정-5 에 인질.

### 5.4 "영영 안 될" 시나리오와 목표 재정의

- 가정-3 반증(dynamic 재-맵이 chromium 클로저 실패) + 리스크-1 적중(supervision wedge)이 동시 참이면
  CR-5(MP 렌더)는 현 아키텍처로 불가. 그때의 재정의: **CR-1~CR-4 의 `--single-process --no-zygote`
  단일-매개 경로를 usable 상한**(이미 v159 device-RUNS)으로 두고, CR-5 는 "renderer-수-제한 +
  software-only + dynamic-helper-proven 후 단계적 확대" 로 best-effort 재정의 — ADR-003-v1 §7 / v2
  §8.10 / `chromium-multiprocess-reexec.md` §5.4 의 "fork-자식 자동 / fresh-exec 자식 best-effort +
  한계 문서화" 톤 계승. fork(clone) 자식(renderer)은 v1 §2-A 로 *여전히* 자동 매개(GATE-1 무관).

---

## 6. 제안 diff (본체 미수정 — 통합/WS-1 이 배선할 정확한 변경)

> 이 문서는 코드를 편집하지 않는다. 아래는 GATE-1(유일한 신규 코드)의 *제안* 형태다 — 실제 배선/
> 변수명/시그니처는 통합 세션이 코드에서 확정. 설계 SSOT 는 `chromium-multiprocess-reexec.md` §2.

### 6.1 GATE-1 — exec-trap 의 `/proc/self/exe` SKIP → 치환 (한 곳)

`runtime_report.cpp` L2617-2653 의 `inproc_skip_reason` 분기에서, **`proc-self-exe` 케이스를 "치환"
으로 분기**한다. `host_path`(L1507, supervisor scope 에 이미 in-scope)를 그대로 쓴다 — 새 변수 불요.

```cpp
// 현재 (L2619-2625):
if (inproc_reexec_on) {
    // (a) /proc/self/exe ... the trampoline cannot map it.
    if (std::strcmp(gp, "/proc/self/exe") == 0 ||
        std::strncmp(gp, "/proc/", 6) == 0) {
        inproc_skip_reason = "proc-self-exe";
    }
    ...

// 제안 (GATE-1):
//   - exec 타깃이 될 수 있는 /proc/self/exe 또는 /proc/<digits>/exe 만 "치환 후보" 로 좁히고,
//     launch 게스트의 host_path 를 알면(=비어있지 않으면) SKIP 대신 host_path 로 치환한다.
//   - 그 외 /proc/* (예 /proc/self/maps) 는 exec 타깃이 아니므로 계속 proc-self-exe SKIP.
//   - host_path 가 비면(예외적; launch 게스트 미상) 안전 fallback 으로 기존 SKIP 유지.
if (inproc_reexec_on) {
    // exec 타깃 형태 판별: "/proc/self/exe" 또는 "/proc/<digits>/exe" 만 self-exe.
    bool is_self_exe = (std::strcmp(gp, "/proc/self/exe") == 0);
    if (!is_self_exe && std::strncmp(gp, "/proc/", 6) == 0) {
        const char* p = gp + 6;
        const char* slash = std::strchr(p, '/');
        // /proc/<digits>/exe (자기 pid 변종)
        if (slash != nullptr && std::strcmp(slash, "/exe") == 0) {
            bool all_digit = (slash > p);
            for (const char* q = p; q < slash; ++q)
                if (*q < '0' || *q > '9') { all_digit = false; break; }
            is_self_exe = all_digit;
        }
    }
    if (is_self_exe) {
        // host_path = config.rootfs_dir + guest_rel (launch 게스트의 rootfs host 경로,
        // 이 함수 L1507 에서 이미 계산되어 supervisor 루프에 in-scope). chromium 자식은
        // 전부 같은 chrome 바이너리를 re-exec 하므로 단일 스칼라로 충분.
        if (!host_path.empty()) {
            // 치환: SKIP 대신, 아래 in-proc 재-맵 경로(L2654~)가 쓸 host 타깃을 host_path 로.
            // med.host_path 를 직접 못 바꾸므로(med 는 gp 기준), 재-맵 host 선택을 보강:
            //   L2655-2657 의 `host = med.should_rewrite ? med.host_path : gp` 를
            //   self-exe 일 때 host_path 로 오버라이드(아래 6.2 와 한 쌍).
            // inproc_skip_reason 은 nullptr 로 두어(=SKIP 안 함) 재-맵 경로 진입.
            if (first_exec_x0.empty()) { first_exec_x0 = gp; first_exec_reason = "self-exe-subst"; }
        } else {
            inproc_skip_reason = "proc-self-exe";   // 안전 fallback
        }
    } else if (std::strncmp(gp, "/proc/", 6) == 0) {
        inproc_skip_reason = "proc-self-exe";       // 비-exe /proc/* 는 계속 SKIP
    }
    // (c) alr-reentry stub idempotency: 기존 그대로 (L2627-2634)
    ...
```

### 6.2 GATE-1 짝 — 재-맵 host 타깃 선택에 self-exe 오버라이드

L2654-2657 의 host 선택을, self-exe 치환이면 `host_path` 를 쓰도록:

```cpp
// 현재 (L2654-2657):
if (inproc_reexec_on && inproc_skip_reason == nullptr) {
    const std::string host =
        med.should_rewrite ? med.host_path : std::string(gp);

// 제안: self-exe 면 launch 게스트의 host_path 로(gp=/proc/self/exe 는 rootfs 밖이라
//       med.should_rewrite=false → 원래 gp 를 쓰면 트램폴린이 /proc/self/exe 를 열어 실패).
if (inproc_reexec_on && inproc_skip_reason == nullptr) {
    const bool self_exe_subst = (first_exec_reason == "self-exe-subst") /* 또는 위 분기의 로컬 플래그 */;
    const std::string host =
        self_exe_subst ? host_path
                       : (med.should_rewrite ? med.host_path : std::string(gp));
```

(주의: `first_exec_reason` 는 *첫* exec 만 기록하므로 실제 배선은 **로컬 bool `self_exe_this_trap`**
을 6.1 분기에서 세워 6.2 로 넘기는 게 정확 — 위는 *의도* 표현. 통합 세션이 로컬 플래그로 확정.)

### 6.3 변경 없음(이미 처리됨 — v2/v158)

- **argv/envp**: in-proc 재-맵은 argv(x20)/envp(x21) 원본 전달(L2677-2682) — Option S 의 argv
  재작성(L2730~) *불필요*. GATE-1 은 x19 타깃만 바꿈. **변경 0.**
- **B-3 envp 강제주입 + x21 mirror**: L2947~ / L3060-3073(`7f45def`) 이 매 exec trap 에서
  `LD_PRELOAD`/`ALR_ROOTFS` 강제 + 재-맵 시 x21 mirror. chromium 이 env 걸러도 강제됨. **변경 0.**
- **EVENT_EXEC / fd evict**: L3098~ 가 처리(in-proc 재-맵은 커널 execve 0 이라 EVENT_EXEC 자체가 안
  뜨지만, 혹시 SKIP fallback 으로 커널 execve 가 진행되면 fd 캐시 무효화). **변경 0.**
- **`/proc/self/exe` readlink 매개**: L1595-1600 `ALR_GUEST_EXE`(v158) 가 *읽기* 처리. **변경 0.**

### 6.4 host 회귀 테스트 (제안, WS-5 협의 — device 불요)

`tests/test_execve_pathrw.py`(기존, execve x0 vs at-style x1 결정모델)를 확장해 **GATE-1 치환 결정
로직**을 순수함수로 추가:
- 입력 `(gp, host_path, med.reason)` → 출력 `{재-맵 host, skip_reason}`.
- 케이스: `gp="/proc/self/exe"` + `host_path` 비어있음 → `skip=proc-self-exe`(fallback);
  `gp="/proc/self/exe"` + `host_path="<rootfs>/opt/chromium/chrome"` → `host=host_path, skip=none`;
  `gp="/proc/1234/exe"` (digits) → 치환; `gp="/proc/self/maps"` → `skip=proc-self-exe`(비-exe);
  `gp="/usr/bin/dpkg-deb"` (rootfs 절대) → `host=med.host_path, skip=none`(기존 경로, self-exe 아님).
- darwin host 는 실커널 ptrace/seccomp 불가 → **결정 로직 회귀만**(치환 *효과* 는 device-only).

---

## 7. 교차참조

| 문서 | 무엇 | 이 문서와의 관계 |
|---|---|---|
| `docs/design/chromium-multiprocess-reexec.md` | `/proc/self/exe`→guest-binary 치환 **설계 SSOT** | GATE-1 의 상세 설계(§2 치환 로직, §3 dynamic, §4 fd) — 이 문서는 그 위에 MP 게이트 맵+device 플래그+제안 diff |
| `docs/design/adr-003-multiprocess-exec-reentry.md` | exec re-entry ADR(v1 상속 기각→v3 in-proc 재-맵) | GATE-1 이 타는 재-맵 메커니즘(§8 ADR-003-v3); v2 Option S 는 DEAD |
| `docs/research/chromium-run-plan.md` | CR-1..CR-5 실행 사다리 SSOT | CR-5(§4.1 플래그)가 이 문서의 MP 게이트와 동일; 이 문서는 CR-5 를 게이트별로 분해 |
| `docs/research/cp6-status.md` | render storm/exec 벽 진단 SSOT | §3-(B) 자식 클래스 분리·in-proc-remap 트랙; PCGATE 9-path-only(§2.2 mojo 근거) |
| `docs/research/loader-feature-gaps.md` | G1(exec re-entry) 잠긴-기능 SSOT | GATE-2(dynamic 재-맵)의 게이트 — G0 선행 |
| `docs/research/r12-remaining-gaps-status.md` | R12 6레인(g1-seqint = per-exec scoping) | 리스크-1(supervision throughput) 완화 lane — CR-5 는 소비자 |
| `docs/research/chromium-native-plan.md` | Goal-2 Phase A–F + 4-scout | Phase C=MP=CR-5; Scout 2(fork+exec/seccomp)·4(IPC/ring 6갭) |
| `docs/research/chromium-gpu-path.md` | CR-3 GPU flag 사다리 | 리스크-3(per-renderer ring) 소관 |

---

## 8. 한 페이지 요약 (통합 세션용)

1. **`--single-process` 제거 = 자식 2클래스**: (A) zygote-fork renderer = 자동 매개(추가 0, v2 dpkg-
   fork 방증), (B) fresh-execve(`/proc/self/exe`) zygote/gpu/utility = exec 벽.
2. **(B) 의 유일한 신규 코드 = GATE-1**: exec-trap 의 `/proc/self/exe` SKIP(L2622-2624)을, supervisor
   에 *이미 in-scope* 인 `host_path`(L1507) 로 **치환**(§6.1/6.2). 새 변수/스레딩/argv-재작성 불요 —
   `chromium-multiprocess-reexec.md` §2.1 의 선택지보다 단순(코드를 읽어 확정).
3. **Mojo = supervisor 신규 코드 0**(§2): fd 번호 보존은 execve 부재의 부수효과(v2 방증), SCM_RIGHTS
   /memfd 는 비-path nr 이라 트랩 안 함(PCGATE 9-path-only) = supervisor 우회. mojo *프로토콜 동작* 만
   device-only(가정-5).
4. **`/proc/self/exe` 는 두 곳**(§3): v158 `ALR_GUEST_EXE`(읽기, 이미 처리) vs exec-trap SKIP(exec,
   GATE-1 대상) — 별개지만 같은 launch 게스트로 수렴.
5. **device 사다리**(§4): CR-5-stepA(`--no-zygote --renderer-process-limit=1`, exec 벽 최소 노출) →
   stepB(자식 수 풀기, throughput) → full(GPU/display). 선행 = G0(작은 dynamic helper 재-맵) + G1-
   seqint(per-exec scoping). 전 구간 `--no-sandbox`(필수)·`--disable-gpu`(1차).
6. **정직**(§5): GATE-1 은 *코드 분량* 한 조각이나 *device-동작* 은 가정-3(dynamic ld.so 재-맵)·리스크-1
   (supervision throughput)에 인질 — 둘 다 chromium 전에 작은 helper+자식수제한으로 분리 확정. chromium
   사용자 보류이므로 실제 드레인은 보류 해제 + G0/G1 device-PASS 후. host-only 진전만으로 RUNS 승급 금지.
