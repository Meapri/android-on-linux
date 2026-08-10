# ADR 0001 — 시그널 전용 ptrace 슈퍼바이저

## Status

Accepted (설계 단계, 2026-08-02). 디바이스 검증은 M2에서.

## Context

목표는 "PRoot보다 낮은 오버헤드"이므로 초기 설계 의도는 **ptrace를 완전히 배제**하는 것이었다. Termux는 targetSdk 28이라 앱 데이터 경로의 execve가 허용되므로 ([01-platform-facts.md §B1](../01-platform-facts.md)), 상위 프로젝트가 W^X 때문에 만든 in-process ELF 리맵과 ptrace path 중재를 전부 버릴 수 있다고 봤다.

그런데 검증에서 이것이 **불가능**함이 드러났다.

**결정적 사실** ([§A4](../01-platform-facts.md), `SOURCE`):

glibc 2.39의 `sysdeps/nptl/dl-tls_init_tp.c` `__tls_init_tp()`가 호출 순서상
1. `set_tid_address`(96) — 허용
2. **`set_robust_list`(99) — Android seccomp 차단 → SIGSYS → 즉사**
3. `rseq`(293) — 차단 (튜너블로 회피 가능)

를 실행하는데, `__tls_init_tp()`는 `_dl_init`보다 **먼저** 돌아간다. `DT_INIT_ARRAY`도 `DT_PREINIT_ARRAY`도 이보다 앞서지 않는다. 그리고 시그널 처리 방식(disposition)은 execve가 초기화한다.

즉 **프로세스 내부에서 SIGSYS 핸들러를 설치할 기회가 구조적으로 존재하지 않는다.** 순수 `LD_PRELOAD` 설계로는 스톡 glibc가 부팅조차 못 한다.

보조 근거: 스택 seccomp 필터는 가장 제한적인 액션이 이기고 `RET_TRAP < RET_ERRNO`이므로, 자체 필터로 Android의 TRAP을 완화하는 것도 불가능하다 ([§A3](../01-platform-facts.md)).

## Decision

**시그널 전달 정지만 처리하는 ptrace 슈퍼바이저를 상시 유지한다.**

```
1. PTRACE_TRACEME + go-pipe 핸드셰이크로 경합 없이 부착
2. PTRACE_O_TRACE{FORK,VFORK,CLONE,EXEC,EXIT} | EXITKILL 로 트리 전체 추적
3. waitpid 루프에서 SIGSYS(si_code == SYS_SECCOMP) 만 가로채 syscall 에뮬레이션
4. PTRACE_SYSCALL 을 절대 호출하지 않는다
5. 자체 seccomp 필터를 설치하지 않는다
```

## Consequences

### 긍정

- **스톡 Ubuntu rootfs가 glibc 패치 없이 부팅한다.** 이것이 grun 대비 유일한 차별점이므로 반드시 지켜야 하는 속성이다.
- **비용이 워크로드에 비례하지 않는다.** 슈퍼바이저는 시그널이 실제로 발생할 때만 깨어난다. 정상 프로세스는 부팅 시 `set_robust_list` 1회 + 소수의 ENOSYS 프로브뿐이다. `git status`의 12,000~15,000회 path syscall에 대해서는 **한 번도 깨지 않는다.**
- **부수적으로 여러 문제를 한꺼번에 해결한다**:
  - Node 20+/libuv 1.45+의 `io_uring_setup` SIGSYS 사망 ([§C8](../01-platform-facts.md))
  - apt의 `setgroups()` 사망 (cred 계열 전체)
  - 미래 Android 버전이 새로 차단하는 syscall (기본 `-ENOSYS` 정책이 자동 대응)
- `PTRACE_SEIZE` 대신 `TRACEME`를 쓰므로 `PTRACE_EVENT_STOP` 의미론과 `PTRACE_LISTEN` 데드락(상위 프로젝트의 chromium 22-스레드 문제)이 아예 발생하지 않는다.

### 부정

- **ptrace가 완전히 사라지지 않는다.** "no ptrace" 마케팅은 쓸 수 없다. 정확한 표현은 [02-architecture.md §3](../02-architecture.md)에 있다.
- 게스트 프로세스가 traced 상태다:
  - 사용자가 게스트 안에서 `gdb`/`strace`를 쓸 수 없다 (한 프로세스에 트레이서는 하나).
  - `/proc/self/status`의 `TracerPid`가 0이 아니라, 안티디버깅을 하는 프로그램이 다르게 동작할 수 있다.
  - Codex 자신의 seccomp 샌드박스가 ptrace를 거부해도 **우리가 Codex를 추적하는 것에는 영향이 없다** (거부는 tracee가 tracer가 되는 것을 막을 뿐).
- 슈퍼바이저 프로세스가 하나 더 필요하다 → phantom process 예산([§B8](../01-platform-facts.md))을 1 소모한다. 그래서 별도 바이너리로 분리하지 않고 `alr` 자신이 부모 역할을 맡는다.

### 위험과 완화

| 위험 | 완화 |
|---|---|
| SIGSYS 에뮬레이션의 aarch64 함정 3개 (`regs[0]` 미기입, `si_call_addr` PC 고정 누락, `-ERESTARTNOINTR` 무한루프) | [03-supervisor-spec.md §4.1](../03-supervisor-spec.md)에 정확한 코드와 근거. 각각 회귀 테스트 존재 |
| 에뮬레이션 테이블에 항목 누락 | `alr doctor` P2가 syscall 0..460을 실제로 쓸어 디바이스의 진짜 차단 집합을 덤프. 미등록 항목은 기본 `-ENOSYS` + **경고** |
| `PTRACE_SYSCALL` 도입 유혹 | regression gate의 하드 불변식 `syscall_stops == 0` |

## Alternatives considered

### (A) userland-exec 부트스트랩 — 유일하게 ptrace를 완전히 없애는 방법

`alr`이 SIGSYS 핸들러를 먼저 설치한 bionic 정적 스텁을 exec하고, 그 스텁이 ld.so + 게스트 바이너리를 손으로 매핑해 점프한다. execve가 없으므로 핸들러가 `__tls_init_tp()`까지 살아남는다.

**기각 이유**: 진짜 ELF 로더를 써야 하고, 커널 execve가 공짜로 주는 것들을 전부 직접 에뮬레이션해야 한다 — fresh mm, 커널이 무장한 brk, fd 테이블 `FD_CLOEXEC` 정리, 올바른 `/proc/self/exe`, `binfmt_script`, non-PIE `ET_EXEC`의 고정 주소 충돌. 상위 프로젝트의 `alr_inproc_reexec.c`(~1,200줄)가 정확히 그 고통의 기록이고, 그 파일의 주석은 각 항목이 어떻게 디바이스에서 터졌는지를 남겨 놓았다.

**v1에서 하지 않는다.** M8에서 슈퍼바이저 오버헤드가 실측으로 문제가 될 때만 재검토한다. 그럴 가능성은 낮다 — 프로세스당 몇 번뿐이다.

### (B) glibc 패치 (termux-pacman 방식)

`clone3.S` 삭제, `set_robust_list`/`rseq` 무력화 등.

**기각 이유**: "스톡 Ubuntu 아카이브"라는 **유일한 차별점을 버린다**. 그러면 grun과 같은 제품이 되는데 grun이 더 성숙하다. 게다가 `libc6`가 apt로 업그레이드될 때마다 깨진다.

### (C) 부팅 창에서만 ptrace하고 detach

부팅 시 `set_robust_list`/`rseq`만 구제하고, preload 생성자가 in-process SIGSYS 핸들러를 설치한 뒤 detach.

**기각 이유**: 움직이는 부품이 늘어나는데 얻는 게 없다. 상시 유지의 정상 상태 비용이 이미 0이고(시그널이 없으면 깨지 않는다), detach하면 io_uring 같은 **런타임 중반**의 SIGSYS를 in-process 핸들러가 처리해야 해서 결국 두 개의 에뮬레이션 경로를 유지하게 된다.

향후 최적화 후보로만 기록한다.

### (D) `SECCOMP_RET_USER_NOTIF`

**기각 이유**: 완화가 아니라 추가만 가능하므로 Android의 TRAP을 되돌릴 수 없다 ([§A3](../01-platform-facts.md)). 애초에 적용 대상이 아니다.

## Related

- [ADR 0002](0002-explicit-ldso-invocation.md) — 명시적 ld.so 호출
- [ADR 0003](0003-ld-preload-path-virtualization.md) — 경로 가상화
- 상위 프로젝트 ADR 0003 (CPU syscall mediation) — 같은 문제를 APK 제약 아래에서 푼 기록
