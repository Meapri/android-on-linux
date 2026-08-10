# 03 — 슈퍼바이저 정밀 스펙 (SIGSYS rescue)

구현 위치: `src/supervisor/`. `alr` 바이너리에 링크된다 (별도 프로세스 아님).

## 1. 존재 이유

Android zygote seccomp 필터가 `set_robust_list`(99)를 `SECCOMP_RET_TRAP`으로 막는데, glibc의 `__tls_init_tp()`가 **어떤 DSO 생성자보다도 먼저** 그것을 호출한다 ([01-platform-facts.md §A4](01-platform-facts.md)). 따라서 프로세스 내부에서 SIGSYS 핸들러를 설치할 기회가 존재하지 않고, 스톡 glibc는 부팅하다 죽는다.

슈퍼바이저는 **부모 프로세스에서** 그 SIGSYS를 가로채 syscall을 에뮬레이션한다.

## 2. 절대 규칙

```
1. PTRACE_SYSCALL 을 절대 호출하지 않는다.
2. 자체 seccomp 필터를 설치하지 않는다 (v1).
3. 게스트 메모리에서 경로를 읽거나 쓰지 않는다. 경로 중재는 preload의 일이다.
4. 시그널 전달 정지(signal-delivery-stop)와 ptrace 이벤트 정지만 처리한다.
```

1번을 어기면 PRoot가 된다. 3번을 어기면 `/proc/<tid>/mem` 왕복이 생겨 성능 주장이 무너진다.

## 3. 기동 시퀀스

```c
pid_t child = fork();
if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);              /* ← 필수. 아래 설명 참조 */
    /* fd/터미널/cwd 정리는 여기서 */
    execve(ldso_host_path, ldso_argv, guest_envp);
    _exit(127);
}

int st;
waitpid(child, &st, __WALL);     /* WIFSTOPPED && WSTOPSIG == SIGSTOP */
ptrace(PTRACE_SETOPTIONS, child, 0, (void *)(
      PTRACE_O_TRACEFORK  | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE
    | PTRACE_O_TRACEEXEC  | PTRACE_O_TRACEEXIT
    | PTRACE_O_EXITKILL));
ptrace(PTRACE_CONT, child, 0, 0);   /* sig=0 → SIGSTOP 억제 */
```

### 3.1 `raise(SIGSTOP)`이 반드시 있어야 하는 이유

> ⚠️ **`PTRACE_TRACEME`은 정지(stop)를 발생시키지 않는다.** 커널의 `ptrace_traceme()`는 `current->ptrace = PT_PTRACED`를 설정하고 실제 부모에 연결할 뿐, `ptrace_stop()`에 들어가지 않는다.

이것이 두 개의 하드 요구사항을 만든다:

1. **자식이 스스로 정지해야 한다.** `raise(SIGSTOP)`이 없으면 자식은 곧장 execve로 달려가고 부모는 `waitpid()`에서 영원히 블록한다.
2. **`PTRACE_SETOPTIONS`는 tracee가 ptrace-stop 상태일 때만 동작한다.** `ptrace_check_attach()` / `ptrace_freeze_traced()`를 거치므로 실행 중인 자식에게는 `ESRCH`를 반환한다.

`raise(SIGSTOP)`은 시그널 전달 정지를 만들어 두 요구를 동시에 만족시킨다. 부모가 `PTRACE_CONT(sig=0)`로 깨우기 전까지 자식은 execve에 도달하지 못하므로, **별도의 go-pipe 핸드셰이크는 필요 없다** (자식을 붙잡아 두는 것이 곧 정지 자체다).

**왜 `PTRACE_SEIZE`가 아닌가**: `SEIZE`는 부모가 pid를 알고 붙기까지의 창(window)에 자식이 이미 execve를 지나 `set_robust_list`에 도달해 죽을 수 있다. `TRACEME` + 자기 정지는 그 경합이 구조적으로 없다.

`PTRACE_O_EXITKILL`: `alr`이 죽으면 게스트 트리도 함께 죽는다. 고아 게스트가 phantom process 예산([§B8](01-platform-facts.md))을 먹는 것을 막는다.

## 4. 메인 루프

```c
for (;;) {
    int st;
    pid_t t = waitpid(-1, &st, __WALL | __WNOTHREAD);
    if (t < 0) { if (errno == EINTR) continue; if (errno == ECHILD) break; fatal(); }

    if (WIFEXITED(st) || WIFSIGNALED(st)) { reap(t); if (t == leader) leader_status = st;
                                            if (no_tracees_left()) break; continue; }
    if (!WIFSTOPPED(st)) continue;

    int sig   = WSTOPSIG(st);
    int event = (st >> 16) & 0xff;

    if (event != 0) { handle_event(t, event); continue; }        // §4.2

    /* 새 tracee의 최초 정지: 자식 쪽 정지가 부모의 EVENT_FORK/CLONE보다 먼저 도착한 경우.
       이 정지는 커널이 주입한 startup SIGSTOP 이다. */
    if (!known(t)) { known_add(t); set_running(t); ptrace(PTRACE_CONT,t,0,0); continue; }

    /* EVENT_FORK/CLONE 이 먼저 도착해 known 이지만 아직 NEW:
       startup SIGSTOP 을 삼킨다. 재주입하면 게스트 스레드 그룹 전체가 group-stop 된다. */
    if (state(t) == NEW && sig == SIGSTOP) { set_running(t); ptrace(PTRACE_CONT,t,0,0); continue; }

    if (sig == SIGSYS) { if (handle_sigsys(t)) { ptrace(PTRACE_CONT,t,0,0); continue; } }

    if (is_group_stop(t, sig)) { handle_group_stop(t, sig); continue; }   // §4.4

    ptrace(PTRACE_CONT, t, 0, sig);                              // 통과: 시그널 그대로 전달
}
```

**`__WNOTHREAD` 필수**: 한 프로세스 안에서 여러 슈퍼바이저 인스턴스가 살 수 있다(`alr` 안에서 중첩 실행). 그냥 `waitpid(-1)`은 다른 인스턴스의 stop을 훔쳐 `ESRCH` 크로스토크를 일으킨다 — 상위 프로젝트가 디바이스에서 증명한 버그다.

> ⚠️ **새 tracee의 최초 정지는 `SIGTRAP`이 아니라 `SIGSTOP`이다.** 커널 `ptrace_init_task()`는 비-SEIZE 트레이서에 대해 `sigaddset(&child->pending.signal, SIGSTOP)`만 한다 (`PTRACE_EVENT_STOP`은 `PT_SEIZED`에만 온다). 따라서 모든 새 자식·스레드의 첫 정지는 `WSTOPSIG == SIGSTOP`, `(st>>16) == 0`인 평범한 시그널 전달 정지다.
>
> 이것을 `SIGTRAP`으로 검사하면 조건이 **절대 매치되지 않고**, `SIGSTOP`이 그대로 재주입되어 `do_signal_stop()`이 **게스트 스레드 그룹 전체를 group-stop** 시킨다. 즉 게스트 안의 모든 `pthread_create()`/`fork()`가 게스트를 통째로 멈춘다.

### 4.0 tid 상태 레지스트리

경합 때문에 두 경로로 tid를 알게 되므로 상태가 필요하다.

| 상태 | 진입 | 의미 |
|---|---|---|
| `NEW` | `handle_event`의 `PTRACE_GETEVENTMSG` (부모 쪽 이벤트가 먼저 도착) | startup `SIGSTOP`을 아직 소비하지 않음 |
| `RUNNING` | startup `SIGSTOP`을 소비했거나, 자식 쪽 정지를 먼저 봄 | 정상 |

`known_add(tid)` → `NEW`. `set_running(tid)` → `RUNNING`.

### 4.1 SIGSYS 처리

```c
static bool handle_sigsys(pid_t t)
{
    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, t, 0, &si) < 0)   return false;
    if (si.si_code != SYS_SECCOMP)                  return false;   // 게스트 자신의 SIGSYS는 통과
    if (si.si_arch != AUDIT_ARCH_AARCH64)           return false;

    uint64_t regs[34];                       // x0..x30, sp=31, pc=32, pstate=33
    struct iovec iov = { regs, sizeof regs };
    if (ptrace(PTRACE_GETREGSET, t, NT_PRSTATUS, &iov) < 0) return false;

    long nr  = (long)si.si_syscall;          // regs[8]도 같은 값이나 siginfo가 정본
    long ret = alr_emulate(nr, regs);        // §5 테이블

    regs[0]  = (uint64_t)ret;                // ⚠️ 필수 — 진입 시 regs[0]에는 arg0이 들어 있다
    regs[32] = (uint64_t)(uintptr_t)si.si_call_addr;   // ⚠️ 방어적 PC 고정

    if (ptrace(PTRACE_SETREGSET, t, NT_PRSTATUS, &iov) < 0) return false;

    stats.sigsys_emulated++;
    if (stats.sigsys_emulated > ALR_SIGSYS_RUNAWAY_CAP) fatal_runaway(t, nr);
    return true;                             // 호출자가 sig=0으로 CONT → 시그널 억제
}
```

**세 가지 함정 (전부 [§A5](01-platform-facts.md)에서 검증됨):**

1. **`regs[0]`을 반드시 쓴다.** 커널이 `syscall_rollback()`을 먼저 실행해 arm64에서 `regs->regs[0] = regs->orig_x0`이 되므로, 진입 시 `regs[0]`은 **첫 번째 인자**다. 안 쓰면 인자값이 반환값이 된다.
2. **`regs[32]`(pc)를 `si_call_addr`로 덮어쓴다.** `arch/arm64/kernel/signal.c`가 시그널 전달 전에 `regs->regs[0]`으로 syscall-restart 판정을 하는데, rollback 후 그 값은 arg0이다. arg0이 `-512`/`-513`/`-514`/`-516`이면 커널이 pc를 svc로 되돌린다. `-ERESTARTNOINTR(-513)`은 revert 분기가 없어 **무한 재트랩**한다. 저장 1회로 이 케이스가 무해해진다.
3. **`si_code == SYS_SECCOMP`을 검증한다.** 게스트가 자기 목적으로 발생시킨 SIGSYS(예: 게스트 자신의 seccomp 샌드박스)를 삼키면 안 된다.

`ALR_SIGSYS_RUNAWAY_CAP`: 기본 `1 << 20`. 넘으면 폭주로 판단하고 진단과 함께 종료한다. 정상 프로세스는 한 자리~두 자리 수를 낸다.

### 4.2 이벤트 처리

| 이벤트 | 동작 |
|---|---|
| `PTRACE_EVENT_FORK` / `VFORK` / `CLONE` | `PTRACE_GETEVENTMSG`로 새 pid 획득 → `known_add` → `PTRACE_CONT`. 새 tracee는 옵션을 상속하므로 재설정 불필요 |
| `PTRACE_EVENT_EXEC` | `known` 유지. `GETEVENTMSG`가 알려주는 이전 tid를 정리. `PTRACE_CONT` |
| `PTRACE_EVENT_EXIT` | `PTRACE_CONT`로 실제 종료 진행 |
| 미지 tid의 첫 stop | 새 클론의 최초 정지 (`SIGSTOP`). `known_add` + `set_running` 후 `PTRACE_CONT(sig=0)` |

### 4.3 시그널 통과 규칙

SIGSYS(SYS_SECCOMP) **외의 모든 시그널은 원래 번호 그대로 `PTRACE_CONT`의 3번째 인자로 재주입**한다. 삼키거나 바꾸지 않는다.

`alr`이 자기 스트림에서 받는 `SIGINT`/`SIGTERM`/`SIGQUIT`/`SIGHUP`/`SIGWINCH`는 **게스트 프로세스 그룹으로 포워딩**한다 (`kill(-leader_pgid, sig)`). `SIGTSTP`/`SIGCONT`도 잡 컨트롤을 위해 포워딩한다 — 단 §4.4의 group-stop 처리와 함께여야 동작한다.

### 4.4 group-stop 처리 — 생략하면 Ctrl-Z가 게스트를 영구 정지시킨다

> ⚠️ **group-stop은 SEIZE 전용 개념이 아니다.** `SEIZE`를 쓰지 않으면 group-stop이 **사라지는 게 아니라 식별 불가능해진다.** 커널 `do_signal_stop()`은 traced task 전부에 `JOBCTL_TRAP_STOP`을 설정하고, `do_jobctl_trap()`의 비-SEIZE 분기가 `ptrace_stop(signr, CLD_STOPPED, ...)`을 호출한다. 트레이서에게는 `WIFSTOPPED` + `WSTOPSIG == 정지 시그널` + `(st>>16) == 0`으로 보고되어 **§4의 루프가 시그널 전달 정지와 구별할 수 없다.**

식별 방법:
```c
static bool is_group_stop(pid_t t, int sig)
{
    if (sig != SIGSTOP && sig != SIGTSTP && sig != SIGTTIN && sig != SIGTTOU) return false;
    siginfo_t si;
    return ptrace(PTRACE_GETSIGINFO, t, 0, &si) < 0 && errno == EINVAL;   /* EINVAL = group-stop */
}
```

**규칙: group-stop에서 정지 시그널을 재주입하지 않는다.** 커널이 폐기하며, 재주입은 게스트의 정지를 풀어 버린다.

Ctrl-Z가 재주입 없이 깨지는 방식: 정지 #1은 `SIGTSTP` 전달 정지 → 루프가 `SIGTSTP` 주입 → tracee가 group-stop 진입 → 같은 시그널로 정지 #2 보고 → 루프가 다시 주입 → 무한 반복 또는 영구 정지.

resume은 `alr` 자신의 `SIGCONT` 포워딩만으로 부족하다:

- **리더 레벨 (바깥 Termux 셸의 Ctrl-Z)**: 리더가 group-stop되면 (1) `tcsetpgrp`로 터미널을 `alr`의 원래 포그라운드 pgrp에 돌려주고, (2) `SIGTSTP`를 `SIG_DFL`로 바꾼 뒤 `alr` 자신에게 `raise(SIGTSTP)` — 그래야 바깥 셸이 잡이 진짜 멈춘 것으로 본다.
- **`alr`의 `SIGCONT` 수신 시**: (1) `tcsetpgrp`로 터미널을 게스트 pgrp에 돌려주고, (2) 정지된 tracee 각각에 `PTRACE_CONT(t, 0, 0)`, (3) `kill(-leader_pgid, SIGCONT)` — 게스트 쪽 `SIGCONT` 핸들러와 셸 부기(bookkeeping)가 돌아야 한다.

## 5. 에뮬레이션 테이블

`src/supervisor/alr_sigsys_table.h`. **단일 테이블**로 유지해 `alr doctor`의 프로브 결과와 대조 가능하게 한다.

| nr | 이름 | 반환 | 근거 |
|---|---|---|---|
| 99 | `set_robust_list` | `0` (성공) | glibc은 실패를 우아하게 넘기지만 성공이 더 안전. **이것이 존재 이유 1번** |
| 100 | `get_robust_list` | `-ENOSYS` | |
| 293 | `rseq` | `-ENOSYS` | 튜너블로 대개 호출되지 않음. 방어용 |
| 425 / 426 / 427 | `io_uring_setup` / `_enter` / `_register` | `-ENOSYS` | **Node 20+ / libuv 1.45+ 생존에 필수** ([§C8](01-platform-facts.md)) |
| 436 | `close_range` | `-ENOSYS` | glibc 폴백이 `/proc/self/fd`를 순회 |
| 437 | `openat2` | `-ENOSYS` | |
| 439 | `faccessat2` | `-ENOSYS` | |
| 441 | `epoll_pwait2` | `-ENOSYS` | |
| 449 | `futex_waitv` | `-ENOSYS` | |
| 452 | `fchmodat2` | `-ENOSYS` | |
| 143,144,145,146,147,149,151,152,159 | `setregid`,`setgid`,`setreuid`,`setuid`,`setresuid`,`setresgid`,`setfsuid`,`setfsgid`,`setgroups` | `0` (성공) | **필수.** 자식이 `setgid(getgid())`로 권한을 낮추려다 execve 전에 죽는다. apt의 `_apt` 샌드박스와 gpgv 손자 프로세스가 여기서 사라진다 |
| 40 | `mount` | `-EPERM` | 게스트 툴이 "Bad system call"로 죽는 대신 정상 실패 |
| 39 | `umount2` | `-EPERM` | |
| 51 | `chroot` | `-EPERM` | |
| 161 | `sethostname` | `-EPERM` | |
| 162 | `setdomainname` | `-EPERM` | ⚠️ 162다. 171은 `adjtimex`이며 상위 프로젝트 표의 전사 오류를 그대로 옮기지 말 것 |
| 171 | `adjtimex` | `-EPERM` | `SECCOMP_BLOCKLIST_APP.TXT`에 명시. glibc에 ENOSYS 폴백이 없어 `-ENOSYS` 기본값이 부적절 |
| 170 | `settimeofday` | `-EPERM` | 동일 |
| 112 | `clock_settime` | `-EPERM` | 동일 |
| 266 | `clock_adjtime` | `-EPERM` | 동일 |
| — | **그 외 전부** | `-ENOSYS` | glibc의 폴백 로직이 `ENOSYS`를 키로 쓴다 |

> **`-ENOSYS`가 기본값인 이유**: glibc의 "새 syscall 먼저 시도, ENOSYS면 옛것" 패턴 전부가 이 값으로 키잉된다 ([§C5](01-platform-facts.md)). `EPERM`이나 `EINVAL`을 기본으로 쓰면 폴백이 발동하지 않고 프로그램이 진짜로 실패한다.

> **`getrandom`(278)과 `memfd_create`(279)는 테이블에 없다.** 폴백이 없어서 에뮬레이션할 값이 없다. `alr doctor` P10이 이들의 가용성을 확인하고, 차단된 디바이스라면 **크게 실패**해야 한다 — 조용히 넘기면 Node가 이해 불가능하게 죽는다.

### 5.1 테이블 확장 규칙

`alr doctor` P2(syscall 0..460 스윕)가 테이블에 없는 차단 syscall을 발견하면:
- (원래 `state/<name>/doctor.json` 에 기록한다고 적혀 있었으나 구현되지 않았다 — [ADR 0007 §2](adr/0007-android-16-only.md))
- 기본 `-ENOSYS` 정책으로 자동 처리
- **경고 로그** 출력 — 조용히 넘기면 새 Android 버전의 회귀를 놓친다

## 6. 통계와 진단

슈퍼바이저는 다음을 집계하고 `ALR_LOG>=1`에서 종료 시 출력한다.

```
alr supervisor: pids=<n> sigsys=<n> emulated=<n> passthrough_signals=<n>
                path_traps=0 syscall_stops=0 elapsed_ms=<n>
```

**`path_traps=0`과 `syscall_stops=0`은 불변식이다.** 0이 아니면 누군가 `PTRACE_SYSCALL`을 도입한 것이고, regression gate가 실패해야 한다 ([07-acceptance.md](07-acceptance.md)).

> ⚠️ **2026-08-03 까지 이 줄의 네 필드가 실제와 달랐다.**
> - `passthrough_signals` 와 `elapsed_ms` 는 **구조체에 없었다** — 스펙만 있고 출력되지 않았다. 지금은 둘 다 있다.
> - `path_traps` 와 `syscall_stops` 는 있었지만 **증가시키는 코드가 없었다.** 구조적으로 0이었고, 게이트가 그것을 측정값으로 읽었다. 즉 `PTRACE_SYSCALL` 을 잡으려고 존재하는 불변식이 그것을 볼 수 없었다.
>
> 지금은 넷 다 실측이며, 발화도 확인했다 — 통과 지점의 `PTRACE_CONT` 를 `PTRACE_SYSCALL` 로 바꾸면 `syscall_stops=190 path_traps=16` 이 나오고 슈퍼바이저 자체시험이 12/0 에서 7/5 가 된다. 소스 수준 가드는 [`scripts/check-invariants.sh`](../scripts/check-invariants.sh).

`ALR_LOG=2`에서는 SIGSYS 발생마다 한 줄:
```
alr sigsys: tid=<t> nr=<n> name=<name> ret=<r> pc=<hex>
```

## 7. 프로세스 수명

- `alr`은 **리더 프로세스가 끝날 때까지** 기다린다. `alarm()`도 워치독도 없다 — CLI는 사용자가 끝낼 때까지 돌아야 한다.
- 종료 코드: `WIFEXITED` → `WEXITSTATUS`; `WIFSIGNALED` → `128 + WTERMSIG`.
- 리더가 죽었는데 자손이 남아 있으면(데몬화) `PTRACE_O_EXITKILL`이 정리한다. 이 동작을 문서화한다 — `alr run` 안에서 백그라운드 데몬을 띄우는 용법은 지원하지 않는다.
- **phantom process 감시**: 살아 있는 tracee 수를 추적해 24를 넘으면 경고, 32에서 자손이 상태 없이 사라지면 `reason=android-phantom-process-kill`로 분류한다 ([§B8](01-platform-facts.md)).

## 8. 테스트 (M2 acceptance)

| 테스트 | 검증 대상 |
|---|---|
| `SUPERVISOR TRACEME HANDSHAKE` | go-pipe 순서가 지켜져 자식이 옵션 설정 전에 execve하지 않음 |
| `SUPERVISOR SIGSYS SET_ROBUST_LIST` | 직접 `syscall(99,...)`을 부르는 테스트 프로그램이 살아남고 0을 받음 |
| `SUPERVISOR SIGSYS ENOSYS DEFAULT` | 미지 차단 syscall이 `-ENOSYS`를 받음 |
| `SUPERVISOR SIGSYS RESTART LOOP GUARD` | arg0이 `-513`인 차단 syscall이 무한루프하지 않음 ← 함정 2 회귀 테스트 |
| `SUPERVISOR SIGSYS PASSTHROUGH` | `si_code != SYS_SECCOMP`인 SIGSYS는 게스트에 전달됨 |
| `SUPERVISOR CHILD TRACKING` | fork/clone/exec 자손 전부가 추적되고 SIGSYS 구제를 받음 |
| `SUPERVISOR NO SYSCALL STOPS` | 통계의 `syscall_stops == 0` |
| `SUPERVISOR SIGNAL FORWARD` | 게스트가 SIGINT를 정상 수신 |
| `SUPERVISOR EXIT CODE` | 게스트 종료 코드가 그대로 전파 |
