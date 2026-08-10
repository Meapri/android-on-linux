# 02 — 아키텍처

## 1. 한 장 요약

```text
┌─ Termux (bionic, targetSdk 28, untrusted_app_27) ─────────────────────────┐
│                                                                            │
│  $PREFIX/bin/alr            CLI: install / doctor / run / shell / bench     │
│         │                                                                  │
│         │ fork()                                                           │
│         ├──────────────────────────────┐                                   │
│         │                              │                                   │
│  [parent] alr-supervisor        [child] PTRACE_TRACEME                     │
│   시그널 전용 ptracer                   → execve(게스트 ld.so, ...)          │
│   · SIGSYS(SYS_SECCOMP) → 에뮬 → 억제    ↓ 진짜 커널 execve                  │
│   · EVENT_{FORK,CLONE,EXEC} → 자식 부착   ↓                                 │
│   · 그 외 시그널 → 통과                                                      │
│   · PTRACE_SYSCALL 절대 사용 안 함  ← PRoot와의 결정적 차이                  │
└────────────────────────────────────────┼──────────────────────────────────┘
                                         │
┌─ 게스트 프로세스 (Ubuntu 24.04 glibc 2.39) ┼─────────────────────────────────┐
│                                         ↓                                  │
│  <R>/lib/ld-linux-aarch64.so.1  --library-path … --inhibit-cache            │
│                                 --argv0 <게스트 argv0>                       │
│                                 --preload <R>/usr/lib/alr/libalr_preload.so │
│                                 <프로그램 호스트 절대경로> <args…>            │
│         │                                                                  │
│         ├── libalr_fakeroot.so   (LD_PRELOAD 1번째, 선택) uid0 + 메타DB      │
│         ├── libalr_preload.so    (LD_PRELOAD 2번째, 필수)                   │
│         │      · 경로 재작성 (guest → host, 문자열 프리픽스)                  │
│         │      · /proc/self/{exe,cmdline,mounts,status} 가상화              │
│         │      · exec* 재디스패치 (ld.so 형태로 재작성)                       │
│         │      · link/linkat → link2symlink 에뮬레이션                       │
│         │      · syscall() 인터포즈 (libuv raw syscall 대응)                 │
│         │      · /dev/full 에뮬, PTY ioctl 번역                              │
│         │      · mount/chroot → EPERM (SIGSYS 사망 방지)                     │
│         └── git / node / codex / bash / apt …                              │
└────────────────────────────────────────────────────────────────────────────┘
```

`<R>` = rootfs 호스트 절대경로, 예: `/data/data/com.termux/files/usr/var/lib/alr/distros/ubuntu-24.04`

## 2. 왜 이 모양인가 — 세 개의 강제 조건

이 아키텍처는 취향이 아니라 [01-platform-facts.md](01-platform-facts.md)의 세 가지 하드 사실에서 **연역**된 것이다.

| 사실 | 강제되는 설계 |
|---|---|
| §A4 `set_robust_list`가 차단되고 **ld.so가 생성자보다 먼저 호출**한다 | 순수 LD_PRELOAD 불가 → **슈퍼바이저 필수** ([ADR 0001](adr/0001-signal-only-ptrace-supervisor.md)) |
| §C1 커널이 `PT_INTERP`를 호스트 루트 기준으로 해석한다 | 스톡 바이너리 직접 execve 불가 → **명시적 ld.so 호출 필수** ([ADR 0002](adr/0002-explicit-ldso-invocation.md)) |
| §B4 user namespace / mount / chroot 전부 없음 | 커널 수준 rootfs 불가 → **LD_PRELOAD 경로 가상화가 유일 선택지** ([ADR 0003](adr/0003-ld-preload-path-virtualization.md)) |

## 3. 슈퍼바이저가 PRoot와 다른 지점 — 이 제품의 핵심

**이 절을 오해하면 프로젝트 전체가 무너진다.**

|  | PRoot | alr 슈퍼바이저 |
|---|---|---|
| 트랩 대상 | **syscall** (필터 테이블 ~100개: 경로 관련 + 프로세스 라이프사이클) | **시그널 전달만** |
| 트랩 빈도 | `git status` 10k 파일 → **12,000~15,000회** | 프로세스당 **손에 꼽는 횟수** (부팅 시 `set_robust_list` 1회, `rseq` 0~1회, 이후 차단 syscall이 실제로 발생할 때만) |
| 사용하는 ptrace 요청 | `PTRACE_SYSCALL` (+ seccomp `RET_TRACE`) | `PTRACE_CONT` 만. **`PTRACE_SYSCALL`을 절대 쓰지 않는다** |
| 경로 재작성 위치 | 트레이서가 `/proc/<tid>/mem`으로 게스트 메모리 수정 | **게스트 프로세스 안에서** 문자열 프리픽스 (컨텍스트 스위치 0) |
| path syscall 비용 | 4회 컨텍스트 스위치 + 6~15회 ptrace/process_vm 연산 ≈ **5~20 µs** (모델) | **≤ 100 ns 목표 → 실측 61 ns**(재작성) / **3.9 ns**(상대경로 통과) |

> **정확한 표현**: "우리는 모든 syscall에서 ptrace를 없앤다"가 **아니다** (PRoot도 모든 syscall을 트랩하지 않는다 — §00-product.md §4). 정확한 표현은:
> **"PRoot는 path-bearing syscall마다 ptrace 왕복을 낸다. alr은 path syscall을 절대 트랩하지 않는다. alr의 ptrace는 Android가 죽이려 드는 syscall이 실제로 발생했을 때만, 프로세스당 몇 번 발동한다."**

> ✅ **`MEASURED` (2026-08-02) — 이 절은 더 이상 설계 논증이 아니다.** 같은 기기·같은 워크로드(10,000 파일 `git status`), 각 5회 중앙값: native 42 ms / **alr 49 ms** / proot-distro 1,704 ms → **proot-distro 대비 34.8×**. 프로세스 기동(`/bin/true`, 9회 중앙값)은 24 / 28 / 304 ms → **10.9×**.
> alr 쪽 `rw()` 카운터 실측은 경로 호출 **9,912회 중 재작성 26회(0.26%)**, 나머지 9,887회(99.7%)는 `p[0] != '/'` 한 줄로 통과했다. 경로 계층 총비용 **≈ 40 µs**, 즉 49 ms 의 **0.08%** 다(증거 파일의 0.07% 는 56 ms 를 분모로 한 값이다 — 분모를 바꾸면서 비율을 다시 계산하지 않으면 이렇게 어긋난다). 슈퍼바이저 카운터는 `path_traps=0 syscall_stops=0` — **PRoot와 갈리는 불변식이 실행 중에도 성립한다.** [M8 실측](evidence/2026-08-02-m7-m8-workloads-perf.md)
>
> ⚠️ 위 표의 **PRoot 트랩 빈도 12,000~15,000회는 여전히 모델값이다** (`UNVERIFIED`). 실측한 것은 *우리* 경로 계층의 호출 수(9,912회)이지 proot 의 ptrace 왕복 수가 아니다 — 그것을 세려면 proot 를 `-v` 로 돌리거나 외부에서 관측해야 하고, 하지 않았다. 다만 proot 의 초과 시간 1,655 ms 를 9,912 로 나누면 **호출당 ≈ 167 µs** 로 [§D1](01-platform-facts.md)의 5~20 µs 모델보다 크다. 세 실행의 git 빌드가 서로 다르고(2.55 / 2.43 / 2.53) MediaTek MT8775 1회 세션이라는 단서는 [M8 의 정직성 규칙](evidence/2026-08-02-m7-m8-workloads-perf.md)을 그대로 따른다 — 34.8× 를 무단서 헤드라인으로 쓰지 않는다.

## 4. 컴포넌트

### 4.1 `alr` (CLI, bionic, `$PREFIX/bin/alr`)

- 언어: C11 + C++17. NDK 29 / `--target=aarch64-linux-android24` 로 크로스 빌드.
- 책임: 서브커맨드 파싱, 설정 로드, rootfs 발견, 게스트 env 조립, `alr-supervisor` 진입, 종료코드 전파, 시그널 포워딩.
- **슈퍼바이저를 별도 바이너리로 분리하지 않는다.** `alr`이 fork 후 부모 역할을 그대로 맡는다. 프로세스 하나가 줄고 phantom process 예산(§B8)을 아낀다.

### 4.2 슈퍼바이저 (`src/supervisor/`, `alr` 안에 링크)

[03-supervisor-spec.md](03-supervisor-spec.md) 참조. 요약: 시그널 전용 ptracer + SIGSYS 에뮬레이션 테이블.

### 4.3 `libalr_preload.so` (게스트 glibc ABI, `<R>/usr/lib/alr/`)

[04-preload-spec.md](04-preload-spec.md) 참조. `zig cc --target=aarch64-linux-gnu.2.17` 로 빌드해 **rootfs 안에 배치**한다 (APK가 아니라 rootfs에 사는 것이 상위 프로젝트와 동일).

### 4.4 `libalr_fakeroot.so` (게스트 glibc ABI, 선택적)

> ⚠️ **정정 (2026-08-03) — 이 별도 `.so` 는 만들지 않았고 만들지 않는다.**
> 아래에 기술된 심볼 분할 계약은 그대로 유효하지만, 구현 위치가 다르다:
> fakeroot 신원 사칭은 **`libalr_preload.so` 안에** `ALR_FAKEROOT=1` 뒤에서
> 구현되어 있다(`src/preload/alr_preload.c`, §6.10 심볼군). `.so` 를 하나 더
> 두면 exec 마다 DSO 매핑·재배치가 한 번 더 늘고 전역 심볼 스코프가 커지는데
> ([01-platform-facts.md §D2](01-platform-facts.md)), 그 비용을 정당화할 만큼
> 분리 이득이 없다. 이 문서의 나머지 `libalr_fakeroot.so` 언급과
> `scripts/build-fakeroot.sh`, `src/fakeroot/` 는 **모두 이 정정의 적용을 받는다**
> — 릴리스 레이아웃(`docs/05-provisioning-spec.md` §3.4)에서도 제거했다.

`apt`/`dpkg`용. `LD_PRELOAD` 체인에서 **preload보다 먼저** 온다.

**심볼 분할이 load-bearing 계약이다.** 세 부류가 있다.

**(1) preload 전용** — 경로 재작성 심볼 전부. fakeroot는 이 이름들을 정의하지 않는다.

**(2) fakeroot 전용** — 경로를 받지 않는 자격증명 심볼: `getuid`/`geteuid`/`getgid`/`getegid`/`getgroups`/`getres[ug]id`, `setuid`/`setgid`/`seteuid`/`setegid`(0 반환 성공), `fchown`/`fchmod`(fd 기반), `umask`.

**(3) 공유 — 체인 계약** ⚠️ 여기가 틀리기 쉽다.

경로를 받으면서 메타데이터도 다루는 심볼은 **양쪽이 모두 정의**한다:
`stat` 계열 전부, `chown`/`lchown`/`fchownat`, `chmod`/`fchmodat`, `mknod`/`mknodat`.

규칙:
- fakeroot가 `LD_PRELOAD`에서 먼저 오므로 **바인딩을 이긴다.**
- fakeroot는 **경로를 스스로 재작성하지 않는다.** `dlsym(RTLD_NEXT, "<name>")`으로 체인해 **preload의 래퍼(재작성 수행)에 도달**시킨다.
- preload의 래퍼가 재작성 후 실제 libc를 호출하고 결과를 돌려주면, fakeroot가 그 위에 DB의 uid/gid/mode/rdev를 오버레이한다.

> **이 계약을 어기고 fakeroot가 이 심볼들을 종단(terminal)으로 처리하면**, `--fakeroot`(기본 켜짐)에서 모든 `chown`/`chmod`/`mknod`가 **재작성되지 않은 게스트 경로로 커널에 도달해 `ENOENT`**가 된다 — 정확히 fakeroot가 존재하는 이유인 apt/dpkg 경로가 깨진다.
>
> 이것이 [04-preload-spec.md §6.7](04-preload-spec.md)이 `chmod`/`fchmodat`/`chown`/`lchown`/`fchownat`/`mknod`/`mknodat`를 `wrappers.def`에 포함하는 이유다. **중복 정의가 아니라 의도된 체인이다.** 또한 `--no-fakeroot`에서는 preload의 정의가 유일한 재작성 지점이 된다.
>
> M6에서 업스트림 `fakeroot` 패키지 채택을 검토할 때도 이 계약이 성립한다: 업스트림 `libfakeroot.c`는 `chmod`/`chown`/`lchown`/`mknod`를 감싸고 `dlsym(RTLD_NEXT, ...)`로 해석한 `next_*`에 **원본 경로**를 넘기므로, preload가 그 심볼들을 정의하고 있어야만 재작성이 일어난다.

DB: `$ALR_FAKEROOT_DB`가 지정한 파일의 `mmap(MAP_SHARED)`. `(st_dev, st_ino)` 키의 open-addressed 해시(FNV-1a), 기본 슬롯 262144, `futex` 락. 업스트림 libfakeroot의 faked 데몬 + SysV 메시지큐를 대체한다.

> ⚠️ **정정 (2026-08-03) — 이 메타데이터 DB는 만들지 않았다. 위 문단은 계획으로만 읽어야 한다.**
> 실제로 들어간 것은 **신원 사칭뿐**이다: `ALR_FAKEROOT=1`이면 `getuid`/`geteuid`/`getgid`/`getegid`/`getres[ug]id`/`getgroups`가 0(root)을 답하고, `chown`/`lchown`/`fchownat`/`fchown`은 아무것도 하지 않고 성공을 가장한다. **파일별 uid/gid/mode 장부는 없고 `stat`은 진짜 소유자를 보고한다** (`src/preload/alr_preload.c` 의 `fakeroot` 절 주석). dpkg 는 chown 뒤에 소유권을 되읽지 않으므로 언팩에는 이것으로 충분하다 — `requires superuser privilege` 블로커가 이렇게 풀렸고([M6 블로커 5](evidence/2026-08-02-m6-package-manager.md)), 그 위에서 apt/dpkg 로 git 이 실제로 설치된다([M10](evidence/2026-08-02-m10-apt-install-git.md)). **그러나 소유권을 감사하는 것에는 충분하지 않다.** 진짜 DB가 필요해지는 소비자가 나타날 때 위 설계를 꺼내 쓴다.

> ✅ **`MEASURED` (2026-08-03) — SysV IPC는 막혀 있다. 업스트림 `fakeroot` 채택안은 닫혔다.**
> `alr doctor` 의 syscall 스윕(468개 중 239개 차단)이 낸 차단 집합에 **aarch64 SysV IPC 블록 전체 186–197 이 통째로** 들어 있다 — `msgget`/`msgctl`/`msgrcv`/`msgsnd`(186–189), `semget`/`semctl`/`semtimedop`/`semop`(190–193), `shmget`/`shmctl`/`shmat`/`shmdt`(194–197). [스윕 증거](evidence/2026-08-02-device-bringup.md), 목록 원본은 [`src/supervisor/alr_sigsys_table.h`](../src/supervisor/alr_sigsys_table.h) 의 `blocked=` 블록.
> 게스트 안에서도 직접 확인했다: `shmget`/`semget`/`msgget` 세 개가 전부 **`ENOSYS`**(`Function not implemented`) 로 돌아왔고, 같은 실행의 `ALR_LOG=2` 로그에 `alr sigsys` **4건**이 찍혔다 — `set_robust_list` 1건 + IPC 3건. **`EPERM` 이 아니라 `ENOSYS` 라는 점이 진단이다**: 차단 → SIGSYS → 테이블에 없으므로 `ALR_SIGSYS_DEFAULT_RET`(`-ENOSYS`) 경로가 그대로 돈 것이다. [M16 §1](evidence/2026-08-03-m16-ipc-audit.md)
> 따라서 업스트림 `libfakeroot` 의 기본 전송(`faked` 데몬 + SysV 메시지큐)은 이 플랫폼에서 성립하지 않는다. **M6의 A/B는 결정되었다 — 자체 구현으로 간다.** (업스트림의 TCP 변종이 원리적으로 남지만 이 저장소에서 확인하지 않았다: `UNVERIFIED`. 추적하지 않는 이유는 두 가지다 — 데몬 프로세스가 하나 늘어 phantom process 예산([§B8](01-platform-facts.md))을 먹고, 지금 필요한 것이 위 문단의 신원 사칭뿐이라 `faked` 가 관리해 줄 메타데이터의 소비자가 없다.) [RISKS R9](RISKS.md) 도 이것으로 닫힌다.
> 게스트 전반에 대한 영향은 제한적이다 — 대상 워크로드 중 SysV IPC 를 쓰는 것이 없고, 쓰는 소프트웨어(일부 DBMS, X11 MIT-SHM)는 애초에 비목표다. `ENOSYS` 는 라이브러리가 폴백 경로를 타게 하는 표준 신호이기도 하다([M16 §1](evidence/2026-08-03-m16-ipc-audit.md)).

## 5. exec 체인 — 정확한 형태

### 5.1 최초 진입 (`alr run git status`)

```
1. alr: 설정 로드, R = rootfs 경로 확정
2. alr: 게스트 argv 해석
      "git" → guest PATH 탐색 → 게스트 경로 "/usr/bin/git" → 호스트 경로 "<R>/usr/bin/git"
3. alr: 게스트 env 조립 (§6)
4. alr: pipe(go_pipe) 생성
5. alr: fork()
6. child : PTRACE_TRACEME
           go_pipe에서 1바이트 읽기 (부모가 옵션 설정할 때까지 블록)
           execve("<R>/lib/ld-linux-aarch64.so.1", [ldso, "--library-path", …,
                  "--inhibit-cache", "--argv0", "git",
                  "--preload", "<R>/usr/lib/alr/libalr_preload.so",
                  "<R>/usr/bin/git", "status"], guest_env)
7. parent: waitpid → 최초 stop
           PTRACE_SETOPTIONS(TRACEFORK|TRACEVFORK|TRACECLONE|TRACEEXEC|TRACEEXIT|EXITKILL)
           go_pipe에 1바이트 쓰기
           PTRACE_CONT
8. parent: 시그널 전용 루프 진입
```

> **`PTRACE_TRACEME` + go-pipe 핸드셰이크를 쓰는 이유**: `PTRACE_SEIZE`는 부모가 자식 pid를 안 뒤에 붙어야 해서, 그 사이 자식이 execve를 지나 `__tls_init_tp()`의 `set_robust_list`에 도달해 죽을 수 있다. `TRACEME`는 자식이 스스로 붙으므로 경합이 없고, go-pipe는 **부모가 `PTRACE_SETOPTIONS`를 끝낸 뒤에** 자식이 execve하도록 보장한다.

### 5.2 게스트 안에서의 이후 exec

게스트가 `execve("/usr/bin/gpg", argv, envp)`를 부르면 **preload의 `execve` 래퍼**가:

```
1. "/usr/bin/gpg" 를 게스트 경로로 인식
2. 호스트 경로 "<R>/usr/bin/gpg" 로 변환
3. 해당 파일을 열어 첫 256바이트 분류:
     "\x7fELF" + PT_INTERP 있음 → 동적 → ld.so 형태로 재작성
     "\x7fELF" + PT_INTERP 없음 → 정적 → 직접 execve (preload 없이 실행됨, 로깅)
     "#!"                        → shebang 파싱 → 인터프리터를 게스트에서 해석 후 1번부터 재귀
     그 외                        → ENOEXEC
4. envp에서 LD_PRELOAD/ALR_* 재주입 (idempotent)
5. real execve(<R>/lib/ld-linux-aarch64.so.1, [ldso, "--library-path", …,
               "--argv0", <원래 argv[0]>, "--preload", <preload 호스트경로>,
               "<R>/usr/bin/gpg", argv[1..]], 재작성된 envp)
```

이 재작성은 **`execve`/`execveat`/`execl`/`execlp`/`execle`/`execv`/`execvp`/`execvpe`/`fexecve`/`posix_spawn`/`posix_spawnp`/`system`/`popen` 전부**에서 일어나야 한다. 하나라도 빠지면 그 자식은 후킹되지 않은 채 `ENOENT`로 죽는다.

새 프로세스는 커널 execve로 태어나므로 **슈퍼바이저의 `PTRACE_EVENT_EXEC`가 자동으로 잡고**, seccomp 필터도 그대로 상속되며, SIGSYS 구제도 계속 유효하다.

### 5.3 shebang 처리

커널의 `binfmt_script`는 호스트 루트 기준으로 인터프리터를 찾으므로 쓸 수 없다. preload가 직접 파싱한다:
- 첫 줄 `#!` 다음, 최대 255바이트 (`fs/binfmt_script.c`와 동일 한도)
- 인터프리터와 **분리되지 않는 단일 인자** 하나 (Linux 의미론)
- 인터프리터 경로를 게스트 네임스페이스에서 해석
- 재귀 깊이 상한 4 (Linux `BINPRM_MAX_RECURSION` 대응)

## 6. 게스트 환경변수 계약

### 6.1 alr이 설정하는 것

```
ALR_ROOT=<R>                      # 게스트에게 보이는 rootfs 호스트 경로. preload의 재작성 기준
ALR_GUEST_EXE=<게스트 절대경로>     # /proc/self/exe 가상화의 답
ALR_GUEST_ARGV0=<게스트 argv[0]>
ALR_PRELOAD=<R>/usr/lib/alr/libalr_preload.so
ALR_FAKEROOT=0|1
ALR_LOG=0|1|2                     # 0=조용, 1=경고, 2=추적
ALR_LOG_FD=<fd>                   # 진단 전용 fd (stderr 오염 방지)
LD_PRELOAD=[<fakeroot>:]<preload> # 절대 호스트 경로. 게스트 경로 쓰면 로드 실패
GLIBC_TUNABLES=glibc.pthread.rseq=0
HOME=/root
TMPDIR=/tmp
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
TERM=<호스트에서 상속>
LANG=C.UTF-8
LC_ALL=C.UTF-8
```

### 6.2 반드시 제거하는 것

`LD_PRELOAD`(Termux의 bionic 값 — §B7), `LD_LIBRARY_PATH`, `ANDROID_ROOT`, `ANDROID_DATA`, `ANDROID_ART_ROOT`, `ANDROID_I18N_ROOT`, `ANDROID_TZDATA_ROOT`, `BOOTCLASSPATH`, `DEX2OATBOOTCLASSPATH`, `SYSTEMSERVERCLASSPATH`, `ANDROID_SOCKET_*`, `EXTERNAL_STORAGE`, `PREFIX`, `TERMUX_*`.

**제거는 항목 삭제이지 빈 문자열 대입이 아니다** (§B7 규칙 1).

### 6.3 통과시키는 것 (allowlist)

`TERM`, `COLUMNS`, `LINES`, `SSH_*`(사용자가 명시 요청 시), `http_proxy`/`https_proxy`/`no_proxy`, 사용자가 `alr run -e KEY=VAL`로 지정한 것.

### 6.4 `GLIBC_TUNABLES`의 역할

`glibc.pthread.rseq=0`은 **`rseq` syscall 자체를 없앤다** (§A4). 이건 부분 완화다 — `set_robust_list`에는 대응 튜너블이 없어서 슈퍼바이저가 여전히 필요하다. 그래도 프로세스당 SIGSYS 트랩을 하나 줄이므로 유지한다.

## 7. 상위 프로젝트에서 무엇을 가져오는가

`android-on-linux` 저장소 기준. Codex는 해당 파일을 **참조**하되 라이선스/클린룸 규칙([00-product.md](00-product.md) §6.5)을 따른다.

### 7.1 그대로 이식 (알고리즘 수준)

| 원본 | → alr | 비고 |
|---|---|---|
| `alr_runtime/alr_path.cpp` | `src/common/alr_path_rule.h` | 정규화 + 변환. `..`가 루트에서 클램프되어 이스케이프가 구조적으로 불가. **Linux 헤더 없는 TU로 재구성**해 macOS에서 테스트 가능하게 |
| `alr_runtime/alr_config.cpp` | `src/common/alr_config.*` | `alr-config-v1` 탭 구분 + `%XX` 이스케이프 + FNV-1a 체크섬. exec 경계로 상태 넘기는 데 그대로 유용 |
| `alr_runtime/alr_exec.cpp`의 순수 결정 커널 | `src/common/alr_exec_rule.*` | `decide_exec_path_mediation`, `decide_exec_envp_injection`, `path_under`, `colon_list_contains`, `split_env_entry` — FS 접근 0, 그대로 |
| `alr_runtime/alr_elf.cpp` | `src/common/alr_elf.*` | ELF64 aarch64 헤더 리더 + `PT_INTERP` 추출. exec 분류에 필요 |
| `alr_interpose/libalr_interpose.c`의 `rw()` / `guest_canon()` | `src/preload/alr_rewrite.c` | 재작성 핵심. **스택 버퍼, malloc 금지** 규칙 유지 |
| `alr_interpose/libalr_interpose.c`의 심볼 표 (~120개) | `src/preload/wrappers.def` | §F3대로 `__*_chk` 5개 추가 |
| `alr_fakeroot/libalr_fakeroot.c` + `alr_fakeroot_db.h` | `src/fakeroot/` | mmap DB 설계 그대로 |
| `runtime_report.cpp`의 SIGSYS 에뮬레이션 테이블 | `src/supervisor/alr_sigsys.c` | 값 매핑(99→0, cred 계열→0, 기본 -ENOSYS)이 이미 옳다 |
| `bench/` 전체 | `bench/` | 마커 계약, 오버헤드 산술, regression gate, syscall-mix 분해 — APK 비의존 |
| `tools/safe_tar.py`, `stage_tar_spec.py` | `src/cli` 또는 `scripts/` | 안전 추출 규칙 |

### 7.2 재작업 필요

| 원본 | 문제 | alr에서 |
|---|---|---|
| `alr_runtime/alr_wx.cpp` | Android W^X 결정 매트릭스 전체 | **삭제.** Termux에는 W^X 문제가 없다 |
| `alr_inproc_reexec.c` | execve 대체 in-proc ELF 리맵 (1200줄) | **삭제.** 진짜 커널 execve를 쓴다. (단 §7.4의 미래 옵션 A 후보로 문서에만 남긴다) |
| `runtime_report.cpp`의 path-trace 슈퍼바이저 | `PTRACE_SYSCALL`/seccomp `RET_TRACE`로 path 재작성 | **삭제.** preload가 in-process로 한다 |
| `alr_interpose/alr_pts.c` | socketpair PTY 에뮬레이션 | **삭제.** Termux는 진짜 devpts가 있다 (§B5). 대신 **ioctl 번역 계층**으로 대체 |
| `runtime_report.cpp` JNI 진입 + 리포트 문자열 (~8 kLOC) | APK 전용 | **삭제.** 진짜 `main()`으로 |
| `kMaxArgv=64` / `kMaxEnv=64` | 조용한 절단 — 이미 디바이스 버그 유발 | **동적 할당.** 상한 없음 |
| `alarm()` + 문자열 휴리스틱 워치독 | CLI에 부적합 | **삭제.** 게스트가 끝날 때까지 실행 |
| `libalr_interpose.c`의 chromium 게이트 | GUI 전용 | 삭제 |

### 7.3 무관 (삭제)

`alr_wayland/`, `alr_gpu/`, `alr_audio/`, `alr_usb/`, `alr_doh/`, `alr_jit/`(→ `alr doctor` 서브커맨드로 축소), `alr_reentry/`, `alr_runtime_hook.cpp`, `alr_runtime_interposer.cpp`, `alr_runtime_launcher.cpp`, `alr_runtime_trampoline.cpp`, `proot_candidate.cpp`, `test_command.cpp`, Kotlin/Compose 전체, GUI env 블록.

### 7.4 미래 옵션으로만 기록

**옵션 A — userland-exec 부트스트랩**: `alr`이 SIGSYS 핸들러를 먼저 설치한 bionic 정적 스텁을 exec하고, 그 스텁이 ld.so + 게스트 바이너리를 손으로 매핑해 점프한다. execve가 없으므로 핸들러가 `__tls_init_tp()`까지 살아남는다. **ptrace를 완전히 제거하는 유일한 방법.** 대가: 진짜 ELF 로더를 써야 하고, 커널 execve가 주는 것들(fresh mm, 커널 armed brk, fd 테이블 정리, 올바른 `/proc/self/exe`, `binfmt_script`)을 전부 직접 에뮬레이션해야 한다 — 상위 프로젝트가 `alr_inproc_reexec.c`에서 겪은 고통이 정확히 그것이다.
→ **v1에서 하지 않는다.** M9 이후 슈퍼바이저 오버헤드가 실측으로 문제가 될 때만 재검토한다.

## 8. 디렉토리 레이아웃

### 8.1 소스 트리

```text
alr-termux/
  docs/                     설계 문서 (이 디렉토리)
  src/
    cli/                    alr 서브커맨드, 설정, rootfs 발견
    supervisor/             시그널 전용 ptracer + SIGSYS 에뮬레이션
    preload/                게스트 glibc LD_PRELOAD 인터포저
    fakeroot/               게스트 glibc fakeroot shim
    common/                 양쪽이 공유하는 순수 로직 (경로 규칙, config, ELF, exec 규칙)
  scripts/
    build-host.sh           NDK로 alr 빌드
    build-preload.sh        zig cc로 libalr_preload.so 빌드
    build-fakeroot.sh       zig cc로 libalr_fakeroot.so 빌드
    test-native-core.sh     macOS/Linux에서 common/ 유닛 테스트
    dev-push.sh             ssh 경유 온디바이스 배포 + 실행
  tests/
    host/                   common/ 로직 테스트 (dev 머신에서 실행)
    device/                 온디바이스 acceptance 스위트
    cases/paths.tsv         공유 경로 변환 케이스 테이블 (C/C++/Python이 모두 소비)
  bench/                    벤치 하네스 (Python, 상위 프로젝트에서 이식)
```

### 8.2 온디바이스 레이아웃

```text
$PREFIX/bin/alr                                       CLI 바이너리
$PREFIX/etc/alr/config.toml                           전역 설정
$PREFIX/var/lib/alr/
    distros/<name>/                                   rootfs 루트 = <R>
        usr/lib/alr/libalr_preload.so                 게스트 측 .so
        usr/lib/alr/libalr_fakeroot.so
        usr/lib/alr/manifest.json                     {zig_version,target,sha256…}
    cache/downloads/                                  tarball 캐시
    state/<name>/fakeroot.db                          fakeroot 메타DB
    (state/<name>/doctor.json 은 계획만 있었고 구현되지 않았다 — ADR 0007 §2)
$HOME/.alr/                                           사용자별 오버라이드
```

## 9. 컴포넌트 경계 — 어긴 것 하나가 며칠을 태운다

| 규칙 | 이유 |
|---|---|
| **경로 재작성 규칙은 코드 상 정확히 한 곳**(`src/common/alr_path_rule.h`)에 있고, preload와 CLI가 그것을 include한다 | 두 개의 재작성기가 어긋나면 증상이 "가끔 파일을 못 찾음"으로 나타난다. 상위 프로젝트가 헤더 주석으로 명시한 요구사항이다 |
| **preload는 malloc/free를 호출하지 않는다.** 스택 버퍼만 | 게스트 allocator 초기화 전에 래퍼가 호출될 수 있고, 게스트가 자기 malloc을 후킹했을 수도 있다 |
| **preload는 `stat` 계열을 *호출*하지 않는다. *정의*만 한다** | §F2. `.2.17` 타깃이 이것을 링크 시점에 강제한다 |
| **preload는 `realpath`를 호출하지 않는다** | glibc 내부의 lstat 순회가 자기 래퍼로 재귀한다. 상위 프로젝트는 thread-local `g_rw_suppress` 플래그로 해결했다 |
| **슈퍼바이저는 `PTRACE_SYSCALL`을 절대 쓰지 않는다** | 쓰는 순간 PRoot가 된다. 성능 주장 전체가 무효화된다 |
| **슈퍼바이저는 자체 seccomp 필터를 설치하지 않는다** (v1) | §A3에 따라 완화는 불가능하고, 추가는 syscall당 ~24 ns를 더한다 |
| **`LD_PRELOAD`는 항상 절대 호스트 경로** | 게스트 경로를 쓰면 ld.so가 로드 시점에 조용히 실패한다 (아직 preload가 없어서 재작성이 안 된다) |
| **런타임 어디서도 `mount()`/`chroot()`를 호출하지 않는다** | §B4. 데드코드도 금지 |
