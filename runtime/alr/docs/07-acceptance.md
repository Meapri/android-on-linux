# 07 — Acceptance 및 벤치마크

## 1. Acceptance 문자열 규약

상위 프로젝트에서 이식한다. 모든 테스트는 정확히 한 줄을 낸다:

```
<TEST NAME>: PASS
<TEST NAME>: FAIL
<TEST NAME>: SKIP
<TEST NAME>: KNOWN_FAIL:<reason>
<TEST NAME>: PENDING_DEVICE
```

**다섯 가지 상태만 존재한다.** "대체로 동작함"이나 "환경 문제"는 없다. 실패는 안정적 `reason=` 코드를 갖는다.

| 상태 | 의미 |
|---|---|
| `PASS` | 통과 |
| `FAIL` | 실패. 고쳐야 한다 |
| `SKIP` | **이 환경에 해당 없음** (판정 불필요, 영구적) |
| `KNOWN_FAIL:<reason>` | 알려진 미해결 실패. 안정적 reason 코드 보유 |
| `PENDING_DEVICE` | 참조 디바이스가 없어 아직 판정 불가. **PASS도 FAIL도 아니다** |

`PENDING_DEVICE`는 pass-rate 분모에서 제외한다. 다만 [08-milestones.md](08-milestones.md)의 읽는 법에 따라 `Exit` 항목에 `PENDING_DEVICE`가 하나라도 남아 있으면 **그 마일스톤은 미완이다** — 분모 제외가 마일스톤 통과를 뜻하지 않는다.

`grep`으로 집계 가능해야 하므로 문자열을 임의로 바꾸지 않는다. 문자열 변경은 이 문서를 함께 고치는 것으로만 허용된다.

## 2. 마일스톤별 acceptance

> ⚠️ **이 절은 2026-08-03 에 실측으로 정리했다.** §1 이 세 줄 위에서 "테스트가 없는 이름에는 상태 토큰을 붙이지 않는다" 고 적어 두었는데, **토큰 달린 64개 중 35개에 러너가 없었고 32개가 `PASS` 를 달고 있었다.**
>
> 지운 것이 아니라 셋으로 갈랐다:
> - **러너를 만들었다 (17개)** — 값싼 것은 문서를 고치는 대신 실제로 쟀다. 그 과정에서 중첩 shebang 이 조용히 빈 출력에 exit 0 을 내던 버그가 나왔다.
> - **집계 러너가 재는 것 (11개)** — `(집계)` 로 표시하고 어느 러너가 재는지 가리킨다. `PRELOAD PATH ABS` 는 측정된다 — 다른 이름 아래에서.
> - **잘라냈다 (5개)** — `PRELOAD LINK2SYMLINK *` 는 [ADR 0004](adr/0004-link2symlink.md) 가 **기각한** 그림자 스킴의 성질이다. 러너가 없고 앞으로도 없어야 한다.
> - **미측정으로 표기 (3개)** — 원하지만 못 잰 것. 이름과 사유는 남기되 **토큰은 뗀다.**
>
> 재발은 [`scripts/check-acceptance-names.sh`](../scripts/check-acceptance-names.sh) 가 막는다.

> **현재 온디바이스 스위트 (2026-08-03)**: **PASS=78 FAIL=0 KNOWN_FAIL=2**
> ([M15](evidence/2026-08-03-m15-cmdline-2604.md)). 두 `KNOWN_FAIL`은
> `ALR CODEX LINKAGE:static-unhooked`와 `PRELOAD DEV FULL ENOSPC:non-goal-devfull`이며,
> 둘 다 아래에 이유를 적어 두었다. 기기는 MediaTek MT8775 / Android 16 /
> 커널 `6.1.145-android14` / `untrusted_app_27` / `Seccomp=2`. **2026-08-03 부터 참조 기기 #2**(Snapdragon 8 Elite / 커널 `6.6.98-android15`)가 추가됐고, 수용·폭·차단집합이 모두 일치한다([M19](evidence/2026-08-03-m19-snapdragon.md)).
>
> ⚠️ **아래 블록은 목표 문자열이지 스위트의 출력이 아니다.** 실제로 emit 되는 이름의 집합은
> `tests/host/`와 `tests/device/acceptance.sh`이고, 여기 적힌 이름 중 다수는 그 어느 쪽도 내지
> 않는다. **테스트가 없는 이름에는 상태 토큰을 붙이지 않는다** — 없는 테스트를 세지 않은 채
> "PASS=73 FAIL=0"을 보고해 온 것이 [M13](evidence/2026-08-03-m13-symbol-gate.md)과
> [M12 §7](evidence/2026-08-03-m12-spawn-resolver.md)이 정정한 바로 그 오류다. 그 숫자는 존재하는
> 테스트에 대해서는 정확했고, 없는 테스트에 대해서는 아무 말도 하지 않았다.

### M1 — 호스트 스캐폴딩
```
ALR BUILD HOST:                    (집계) `make test` (호스트 빌드 + 코어 테스트)
ALR BUILD PRELOAD:                 (집계) `scripts/check-preload.sh` (게이트 10종)
ALR PRELOAD GLIBC FLOOR 2.17:     PASS
ALR PATH RULE HOST TESTS:         PASS   (tests/cases/paths.tsv N cases)
ALR CONFIG ROUNDTRIP:              미측정 — 설정 파일이 구현되지 않았다 — `alr config` 도 `config.toml` 리더도 없다
ALR ELF CLASSIFY:                 PASS
```

### M2 — 슈퍼바이저
```
SUPERVISOR TRACEME HANDSHAKE:     PASS
SUPERVISOR SIGSYS SET_ROBUST_LIST:PASS
SUPERVISOR SIGSYS ENOSYS DEFAULT: PASS
SUPERVISOR SIGSYS RESTART LOOP GUARD: PASS
SUPERVISOR SIGSYS PASSTHROUGH:    PASS
SUPERVISOR CHILD TRACKING:        PASS
SUPERVISOR NO SYSCALL STOPS:      PASS
SUPERVISOR SIGNAL FORWARD:        PASS
SUPERVISOR EXIT CODE:             PASS
```

### M3 — 첫 게스트 부팅
```
ALR LDSO INVOKE:                  PASS
ALR BOOT /bin/true:               PASS  exit=0
ALR BOOT /bin/echo:               PASS  stdout="alr"
ALR BOOT /bin/bash -c true:       PASS
ALR GUEST GLIBC VERSION:          2.39
ALR SUPERVISOR SIGSYS COUNT:      sigsys=22 / pids=21  (기록만. 프로세스당 ≈1)  MEASURED
```

**설치 경로** — [`tests/device/install_gate.sh`](../tests/device/install_gate.sh), acceptance 와 별개로 돈다(전체 추출 비용이 든다).
```
INSTALL EXIT:                     PASS
INSTALL REPORT NINE LINES:        PASS   ← docs/05 §4 의 9줄이 이름으로 전부 있는지
INSTALL REPORT LDSO OPTIONS:      PASS   argv0/preload/library-path/inhibit-cache
INSTALL REPORT BOOT ECHO:         PASS   stdout="alr"
INSTALL REPORT UNVERIFIED IS SKIP: PASS  ← 다이제스트 없는 다운로드는 PASS 가 아니다
INSTALL HARDLINK uncompress:      PASS
INSTALL HARDLINK perl5.38.2:      PASS
INSTALL PRELOAD PRESENT:          PASS
INSTALL LDSO PRESENT:             PASS
INSTALL BOOT:                     PASS
INSTALL VIRTUALIZED:              PASS   PRETTY_NAME="Ubuntu 24.04.4 LTS"
INSTALL REJECTS TRUNCATED:        PASS   rc=125
INSTALL REJECTS UNBOOTABLE:       PASS   rc=125, rootfs 를 지우지 않고 남김
INSTALL REJECTS ESCAPING SYMLINK: PASS   rc=125  ← docs/05 §2 traversal, ADR 0009
INSTALL KEEPS INTERNAL SYMLINK:   PASS   ← 양성 대조. rootfs 안 심링크는 20개다
INSTALL DASH D IS A FLAG:         PASS
INSTALL REFUSES UNKNOWN OPTION:   PASS
INSTALL WARNS ON MISSING PRELOAD: PASS
```
> **`INSTALL REJECTS UNBOOTABLE` 이 없으면 `INSTALL BOOT /bin/true: PASS` 는 한 번도 다른 말을 한 적이 없는 줄이다.** 잘린 tarball 은 `verify_rootfs` 에서 리포트 **이전에** 죽으므로 부팅 검사에 대해 아무것도 증명하지 못한다. 그래서 게이트는 `verify_rootfs` 가 찾는 네 파일만 정확히 담고 **아무것도 동작하지 않는** tarball 을 만든다 — `/bin/sh` 의 내용이 문자열 `not an elf` 다. 추출도 성공하고 파일 검사도 성공하니, 잡을 수 있는 것은 실제로 돌려 보는 것뿐이다.

> **M3이 이 프로젝트의 진짜 첫 증명이다.** 여기가 통과하면 `set_robust_list` 문제가 실제로 풀린 것이다.
>
> SIGSYS 수는 [M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)에서 실측됐다. 소프트 게이트
> `sigsys_per_process <= 8`(§3) 안쪽이고, 스위트의 `SUPERVISOR SIGSYS PER RUN`이 매 실행 이 값을
> 확인한다. Node 기동은 **실행당** 10건이었는데(같은 문서) 그건 프로세스당 값이 아니므로 게이트와
> 직접 비교하지 말 것 — 프로세스 수를 함께 적지 않으면 이 두 숫자는 섞인다.

### M4 — 경로 가상화
```
PRELOAD PATH ABS:                  (집계) `ALR PATH RULE HOST TESTS` (73 assertions)
PRELOAD PATH REL:                  (집계) `ALR PATH RULE HOST TESTS`
PRELOAD PATH SYSDIR:               (집계) `ALR PATH RULE HOST TESTS`
PRELOAD PATH IDEMPOTENT:           (집계) `ALR PATH RULE HOST TESTS`
PRELOAD DOTDOT CLAMP:              (집계) `ALR PATH RULE HOST TESTS` (`tests/cases/paths.tsv`)
PRELOAD NORMALIZE BEFORE SYSDIR:  PASS
PRELOAD PBUF PATH_MAX:             (집계) `ALR PATH RULE HOST TESTS`
PRELOAD PROC SELF EXE:            PASS
PRELOAD PROC SELF CMDLINE:         (집계) `PRELOAD PROC CMDLINE` + `PRELOAD CMDLINE NO HOST PATH`
PRELOAD DLOPEN ABS PATH:          PASS
PRELOAD DLOPEN ORIGIN TOKEN:       미측정 — `$ORIGIN` 토큰 확장 경로를 밟는 게스트 라이브러리를 아직 못 찾았다
PRELOAD MKSTEMP:                  PASS
PRELOAD DEV FULL ENOSPC:          KNOWN_FAIL:non-goal-devfull
PRELOAD CHK SYMBOLS PRESENT:      PASS
PRELOAD NO MALLOC IN REWRITE:     (자동 검사 없음 — 아래 주석)
PRELOAD RW ABS COST:              61.0 ns/op  (게이트: <= 100)  MEASURED
PRELOAD RW REL COST:               3.9 ns/op  (게이트: <= 20)   MEASURED
PRELOAD RW SYSDIR COST:           13.8 ns/op  (게이트: <= 40)   MEASURED
PRELOAD RW MICROBENCH:            PASS
```

> **`/dev/full`은 구현하지 않고 비목표로 재분류했다** ([M12 §9](evidence/2026-08-03-m12-spawn-resolver.md)).
> 서빙하려면 프로세스에서 가장 뜨거운 syscall인 `write()`를 인터포즈해야 하는데, 대상 워크로드 중
> 이 디바이스 노드를 쓰는 것이 없고, preload 자신이 `write()`를 호출하므로 정의하는 순간 자기
> 호출을 가로챈다. 결정적으로 실패 표면이 열려 있다 — `puts`/`putchar`/`fwrite_unlocked`/`dprintf`
> 중 **빠뜨린 심볼은 전부 조용히 성공한 쓰기**가 된다. 열거 가능하고 요란하게 실패하는
> `mkstemp`(9개)·NSS(15개) 계열과 다르다. 같은 라운드에서 **프로브 자체도 고쳤다**: 기존
> `: < /dev/full`은 O_RDONLY 열기라 `/dev/zero`로 리다이렉트만 해도 통과했다. 지금은 실제 쓰기가
> `ENOSPC`를 내는지 보고, 안 나므로 정직하게 `KNOWN_FAIL`로 센다.
>
> **`PRELOAD NO MALLOC IN REWRITE`를 emit 하는 테스트는 없다.** R1(재작성 경로 malloc 금지)은
> 현재 코드 규약과 리뷰로만 지켜지며, `src/preload/alr_preload.c`는 `getaddrinfo` 계열을 명시적
> 예외로 문서화한다(반환 리스트를 호출자가 `freeaddrinfo`로 푸는 계약이라 malloc 을 피할 수 없다).
> §3이 이것을 하드 불변식으로 선언하지만 **감시하는 러너가 없다** — §3의 주석을 볼 것.

### M5 — exec 연속성
```
PRELOAD EXEC DYNAMIC:             PASS
PRELOAD EXEC SHEBANG:             PASS
PRELOAD EXEC SHEBANG RECURSION:   PASS
PRELOAD EXEC STATIC:               (집계) `CLI STATIC BINARY WARNS` + `ALR CODEX LINKAGE`
PRELOAD EXEC ENVP IDEMPOTENT:     PASS
PRELOAD EXEC ALL 13 VARIANTS:      (집계) `ALR EXEC RULE TESTS` (44 assertions)
PRELOAD SYSCALL REWRITE:          PASS
PRELOAD SYMLINKAT ASYMMETRY:      PASS
ALR BASH INTERACTIVE:             PASS
ALR PIPELINE:                     PASS   (echo | grep | wc)
```

> **`PRELOAD EXEC STATIC`의 `KNOWN_FAIL`은 영구적이다 — 비목표다.**
> [ADR 0006](adr/0006-raw-syscall-binaries.md)이 근거를 **정정하면서** 그렇게 결정했다. 이전에는
> "zygote 필터가 자리를 차지해 seccomp user notification 이 불가능하다"고 적어 왔는데 틀렸다:
> 스택된 필터에서 커널은 수치가 낮은 액션을 택하므로 `RET_USER_NOTIF`(0x7fc00000)가 zygote 의
> `RET_ALLOW`(0x7fff0000)를 이기고, 설치가 `EPERM`이던 것은 정책이 아니라 `no_new_privs`가 0이라서였다
> (앱 프로세스는 켜지 않는다). 즉 **가로챌 수는 있다.** 막는 것은 비용이다 —
> `bench/microbench/notif_cost.c` 실측으로 알림 왕복 **154 µs/호출**(베이스라인 438 ns의 352배)이고,
> 필터 평가 자체는 공짜다. 경로 syscall 3만 번짜리 워크로드면 가로채기만으로 4.6초라 PRoot 보다
> 느려진다. arm64 `PR_SET_SYSCALL_USER_DISPATCH`는 이 커널에 없다(두 형태 모두 `EINVAL`).
> **SUD 를 지원하는 커널이 흔해지면 이 결정을 뒤집어야 한다.**

### M6 — 패키지 매니저
```
ALR DPKG VERSION:                 PASS
ALR DPKG ARCH:                    PASS   arm64
ALR APT VERSION:                  PASS
ALR APT UPDATE:                   PASS
ALR APT INSTALL git:              PASS
ALR DPKG LOCAL INSTALL:           PASS
ALR FAKEROOT IDENTITY:            PASS   uid=0 gid=0
```

> `apt update` → `apt install git` 완주는 [M10](evidence/2026-08-02-m10-apt-install-git.md)에서
> 실측됐다 — 아무것도 없는 상태에서 2분 27초, `git version 2.43.0` 동작. 막고 있던 네 개의 결함은
> 전부 같은 계열이었다: **glibc 내부 호출은 `LD_PRELOAD`로 가로챌 수 없고, Android 의 `/etc`는
> 읽기 전용 `/system/etc` 심링크다.**

### M7 — 타깃 워크로드
```
ALR GIT VERSION:                  PASS
ALR GIT CLONE LOCAL:              PASS   ← link2symlink 회귀 테스트
ALR GIT CLONE HTTPS:              PASS   ← NSS + resolver + git-remote-https 서브프로세스
ALR GIT STATUS 10K:               PASS   elapsed_ms=49   MEASURED (native 42 / proot 1,704)
ALR GIT HOOKS:                    PASS   ← shebang exec
ALR NODE VERSION:                 PASS
ALR NODE EXECPATH:                PASS   ← process.execPath가 게스트 경로여야 함
ALR NODE FS STAT:                 PASS   ← libuv raw syscall 회귀 테스트
ALR NODE IO_URING SURVIVE:        PASS   (Node 22)  ← SIGSYS 구제 회귀 테스트
ALR NPM CI:                        미측정 — 손으로만 쟀다([M12 §4](evidence/2026-08-03-m12-spawn-resolver.md)) — 하네스에 없다
ALR CODEX VERSION:                PASS   ← "바이너리가 뜬다"는 뜻뿐이다. 아래 주석
ALR CODEX LINKAGE:                PASS   ← 정적임을 확인하는 관찰 라인 (후킹은 여전히 불가)
CLI HOSTBIN PRESENT:              PASS   29 wrappers
CLI HOSTBIN RUNS GUEST TOOL:      PASS   ← 래퍼가 게스트 git 2.43.0 을 부른다
CLI HOSTBIN SELF CONTAINED:       PASS   ← env -i 로도 동작 (자식은 아무것도 물려받지 않는다)
ALR CODEX FINDS GUEST GIT:        PASS   git version 2.43.0
ALR CODEX FINDS GUEST RG:         PASS   ripgrep 14.1.0
ALR CODEX CONFIG IN ROOTFS:       PASS   config loaded
ALR PTY TMUX:                     PASS
PRELOAD UNIX SOCKET PATH:         PASS   ← bind/connect 재작성 + 추상 소켓 통과 (대조)
PRELOAD PATH COVERAGE:            PASS   ← xattr/inotify/pathconf/getsockname/nftw/setmntent/glob
RESOLV SERVICE PORT:              PASS   ← numeric=8080 named=80 none=0
CLI LIST HONOURS CONFIG ROOT:     PASS
CLI VERSION HONOURS -d:           PASS
ALR DPKG DEB BUILD:               PASS   ← setup 의 상태를 더 이상 버리지 않는다
ROOTFS DPKG LISTS PACKAGES:       PASS
RESOLV HOSTS FILES:               PASS
RESOLV AHOSTS FILES:              PASS
RESOLV REVERSE FILES:             PASS
RESOLV LEGACY DNS:                PASS
RESOLV BRIDGE ABSENT:             PASS   ← 브리지 없는 폴백. 이게 abort 하고 있었다
ALR DIG VERSION:                  PASS
ALR DIG VERSION NO BRIDGE:        PASS
DOCTOR P11 DYNAMIC CLEAN:         PASS   ← /bin/true: 0 svc
DOCTOR P11 STATIC FLAGGED:        PASS   ← ld.so: PT_INTERP 없음
DOCTOR P11 COUNTS NONZERO:        PASS   ← 인자만 받고 PASS 찍는 스텁을 거른다
DOCTOR P11 REJECTS NON ELF:       PASS
DOCTOR P11 SWEEPS DIR:            PASS   ← 디렉토리를 주면 스윕한다 (docs/06 §3.1)
DOCTOR P11 SKIPS LIBS:            PASS   ← libc 로 가득한 디렉토리에서 0건
DOCTOR P12 PHANTOM:               PASS   40/40 생존
CLI CONFIG GET DEFAULT:           PASS
CLI CONFIG SET REPORTS NEW:       PASS   ← 쓴 뒤의 값을 보고하는가 (cfg() 메모이즈)
CLI CONFIG AFFECTS RUNTIME:       PASS   ← 설정이 `alr run id -u` 를 0 으로 바꾸는가
CLI CONFIG FLAG OUTRANKS:         PASS   ← --no-fakeroot 가 설정을 이기는가
CLI CONFIG UNKNOWN KEY:           PASS
CLI CONFIG BAD VALUE:             PASS
CLI GUEST USERDB:                 PASS   ← whoami → alr
CLI GUEST USERDB LS:              PASS   ← ls -ld /root → "alr alr" (이전: "10297 10297")
```

> **리졸버 검사가 왜 일곱 개인가.** 앞의 세 개는 전부 `/etc/hosts` 에서 답을 받아 **브리지에 닿지 않는다.** 그래서 브리지가 두 가지 방식으로 완전히 깨져 있는 동안 셋 다 초록이었다([RISKS R15](RISKS.md)). 잡은 것은 이 스위트가 아니라 `dig -v` 를 돌리는 폭 검사였고, 증상은 이름 해석 실패가 아니라 `free(): invalid pointer` 라는 **abort** 였다. `RESOLV BRIDGE ABSENT` 는 브리지가 없을 때의 폴백을 명시적으로 밟는다 — 데몬이 안 뜬 기기에서 지원되는 경로이고, 거기서 게스트의 모든 이름 조회가 죽고 있었다.

> **`PRELOAD PATH COVERAGE` 가 재는 것.** 일곱 개 중 여섯은 양성이고 하나는 **음성 대조**다.
> `glob-guest` 는 `glob()` 이 **게스트 경로**를 돌려주는지 본다 — 결과를 되돌리지 않는 "꼼꼼하게 다
> 감싸자" 식 변경이 모든 호출자를 깨뜨리는데, 그걸 잡는 검사다.
> 모든 검사는 **Android 에 없는 경로**를 쓴다. 첫 판본은 `/etc` 를 감시하고 `/etc` 를 `pathconf` 했는데
> Android 에 `/etc` 가 있어서(`/system/etc` 심링크) **틀린 디렉토리를 보면서 둘 다 ok 를 보고했다.**
> 이틀 사이 두 번째 유령 통과다 — 첫 번째는 `connect()` 에서 멈춰 `accept(2)` 가 막힌 것을 놓쳤다.

> **`ALR CODEX VERSION: PASS` 가 뜻하는 것이 2026-08-04 에 바뀌었다.** codex 0.146.0 은 여전히
> **정적 링크 musl 바이너리**이고(`ET_EXEC`, `INTERP` 없음, 269 MB), `LD_PRELOAD` 는 여전히 **로드조차
> 되지 않는다.** 후킹은 원리적으로 불가능하고 앞으로도 그렇다 — `ALR CODEX LINKAGE` 가 그 사실을 계속
> 감시한다.
>
> **바뀐 것은 후킹이 필요 없게 만들었다는 것이다.** 정적 바이너리의 경로는 실제 루트에서 풀리므로,
> 게스트 형식이 아니라 **호스트 형식 환경**을 주면 재작성 없이 같은 파일에 도달한다. 여기에 호스트 측
> 래퍼(`<R>/usr/lib/alr/hostbin/`)를 PATH 에 놓아 게스트 도구를 부를 수 있게 하고, 중첩 `alr run` 이
> 바깥 supervisor 를 물려받게 했다. 자세한 것은 [ADR 0008 보론](adr/0008-static-guest-binaries-non-goal.md).
>
> 그래서 이 `PASS` 는 이제 **"실행된다"** 를 넘어 **"게스트의 도구로 일한다"** 를 뜻한다 — 위의 세
> `ALR CODEX FINDS/CONFIG` 줄이 그것을 각각 검사한다. `RISKS R7` 의 질문("codex 의 raw-syscall
> 백엔드가 인터포저를 무력화하는가")의 답은 **여전히 YES** 다. 달라진 것은 그게 치명적이지 않다는 것이다.

> **`ALR CODEX SANDBOX DISABLED`에는 상태 토큰을 붙이지 않는다.** [RISKS R8](RISKS.md)의 절반은
> 끝났다 — `alr`의 `with_codex()`는 `$R/root/.codex/config.toml`에
> `sandbox_mode = "danger-full-access"`를 쓰고 `alr: NOTE codex sandbox disabled; alr is not a
> security boundary`를 출력한다(`src/cli/alr.c`). 그러나 **철자가 아직 한 줄로 정리되지 않았다**:
> [M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)은 CLI 플래그가 `--sandbox`이고 설정 키로
> `sandbox_permissions`가 존재한다고 기록했는데, 우리가 쓰는 것은 `sandbox_mode`다. 게다가 codex 가
> 정적 링크라 `$HOME`이 Android 쪽으로 새므로 — codex 자신의 경고
> `could not create PATH aliases: Read-only file system (os error 30)`가 그 신호다 — **rootfs 안에
> 쓴 그 파일을 codex 가 읽기는 하는지조차 확인되지 않았다.** 이걸 판정하려면 게스트 안에서 codex 의
> 유효 설정을 되읽는 검사가 필요하다. 검사가 없는 이름에 `PASS`를 적지 않는다.
>
> **`ALR PTY TMUX`는 `PASS` 다 — 그런데 PTY 문제가 아니었다.**
> 이 이름은 *"지금까지의 디바이스 세션이 전부 비대화형이었고 `/dev/tty` 가 `ENXIO` 라 대화형 경로를
> 밟은 적이 없다"* 는 사유로 `PENDING_DEVICE` 였다. 러너를 쓰려고 붙어 보니 **사람이 앉은 터미널이
> 필요 없었다.** tmux 서버는 소켓 버그 **둘**로 죽고 있었다:
> `bind()`/`connect()` 가 인터포즈되지 않아 `sun_path` 가 재작성되지 않았고([§6.18](04-preload-spec.md)),
> `accept(2)` 는 zygote 필터에 막혀 `-ENOSYS` 로 에뮬레이션되고 있었다([§A6](01-platform-facts.md)).
> **연결을 받는 모든 게스트 프로그램이 깨져 있었다.** 둘 다 고친 뒤 `a1: 1 windows (created …)`.
>
> 서버와 조회를 **한 번의 `alr run` 안에** 넣는다 — `PTRACE_O_EXITKILL` 때문에 데몬화하는
> 프로세스가 자기를 띄운 호출보다 오래 살 수 없다([ADR 0001](adr/0001-signal-only-ptrace-supervisor.md)).
> 설계대로다. 대화형에서는 서버가 자기를 소유한 `alr shell` 만큼 산다.
>
> 이전에 실측된 것은 그대로 유효하다: `tmux -V` → `tmux 3.4`([M11 §4](evidence/2026-08-02-m11-breadth.md)),
> `/dev/ptmx` 페어의 ioctl 집합([M14 §1](evidence/2026-08-03-m14-ioctl-php.md)) — `TCGETS` `TCSETS`
> `TIOCGWINSZ` `TIOCSWINSZ` `FIONREAD` `TIOCOUTQ` 는 그냥 허용, `TCGETS2` `TIOCGSID` `TIOCGETD`
> `TIOCEXCL` 은 번역으로 통과, `TIOCSTI` 만 의도적으로 `EACCES`.
> **여전히 미측정**: 사람이 앉은 대화형 세션에서의 창 분할·리사이즈. 자동화가 판정할 수 있는
> 부분은 판정했고, 남은 것에는 상태 토큰을 붙이지 않는다.

### M8 — 성능
```
ALR BENCH GIT STATUS vs PROOT:    34.8x  MEASURED  native 42 / alr 49 / proot 1,704 ms
ALR BENCH NPM CI vs PROOT:        3.12x  MEASURED  alr 2.00 / proot 6.24~6.87 s (105 패키지)
ALR BENCH PROC STARTUP vs PROOT:  10.9x  MEASURED  /bin/true: native 24 / alr 28 / proot 304 ms
ALR BENCH NODE COLD vs PROOT:     6.60x  MEASURED  alr 55 / proot 363 ms (동일 node 바이너리)
ALR BENCH EXEC THROUGHPUT:        351 exec/s  MEASURED  alr 351 / proot 135 exec/s
ALR MEDIATION INVARIANT:          PASS   path_traps=0 syscall_stops=0  MEASURED
```

> **PRoot A/B 는 실측됐다** ([M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md),
> [M12 §4](evidence/2026-08-03-m12-spawn-resolver.md)). §4.4가 "아무도 재지 못했다"고 적어 둔
> 바로 그 숫자다. 초기에 `proot`가 자체 로더 초기화에서 실패한 것은 **우리 rootfs 와의 조합** 탓이었고,
> proot-distro 를 자체 rootfs 로 돌리면 정상 동작한다.
>
> **`npm ci`는 동일 조건 A/B 다** — node 바이너리·npm·락파일·npm 캐시를 복사해 양쪽을 같게 만들었다.
> `git status`의 34.8×를 약하게 만드는 "빌드가 다르다"는 반론이 여기에는 없다.
>
> **caveat (숫자를 인용할 때 같이 인용할 것)**: 단일 MediaTek MT8775 기기, 1회 세션, thermal 미고정.
> `git status` 3자 비교는 git 빌드가 서로 다르다(2.55 / 2.43 / 2.53). `npm ci`의 proot 게스트는
> Ubuntu 26.04, alr 게스트는 24.04다. 권장 표현은 [M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)의
> 마지막 절에 그대로 적혀 있다.
>
> **`ALR MEDIATION INVARIANT`는 실측으로도 성립한다.** 위 `git status` 실행의 슈퍼바이저 통계가
> `pids=21 sigsys=22 emulated=22 path_traps=0 syscall_stops=0`이었다. **PRoot 와 갈리는 선은
> 실측에서 지켜졌다** — 격차의 본체가 바로 이것이다. 1,704 − 49 = 1,655 ms 를 경로 호출 9,912회로
> 나누면 호출당 ≈167 µs 이고, alr 쪽 경로 계층 전체는 ≈40 µs 다.
>
> **이 두 줄은 더 이상 `PENDING_DEVICE`가 아니다.** 하네스가 없어서 미측정이었고
> ([`tests/device/bench.sh`](../tests/device/bench.sh) 가 그때는 없었다), 이제 있다. `dev-push.sh
> bench-ab` 가 돌리고 [`bench/regression_gate.py`](../bench/regression_gate.py) 가 기기별 baseline
> 과 대조한다. 방법은 그때 적어 둔 그대로다 — **같은 node 바이너리를 양쪽 게스트에 복사**하고
> `node -e 0`을 워밍업 뒤 5회 측정, 중앙값. 그렇게 하지 않으면 배포판 차이를 재게 된다.
> `MEASURED` 2026-08-04, SM-X236N: `NODE COLD 7.20x` (alr 46 / proot 331 ms),
> `EXEC THROUGHPUT 312 exec/s` (proot 118).
>   다만 [RISKS R5](RISKS.md)의 **판정 기준은 이미 충족됐다** — "`npm ci` 비율이 1.5배 미만이면
>   히어로 벤치에서 내린다"였고 실측은 3.12×다. 즉 exec 오버헤드가 히어로를 잡아먹지 않았다.

## 3. Regression gate

`bench/regression_gate.py`. CI와 온디바이스 스위트가 모두 실행한다.

> **`bench/regression_gate.py` 는 이제 존재한다** ([M17](evidence/2026-08-03-m17-bench-ab.md)).
> 설계상 **아무것도 재지 않는다** — 런타임 불변식은 실제 앱 프로세스(`uid>=10000 ∧ Seccomp==2`)에서만
> 나올 수 있고 호스티드 러너는 그것이 아니다. 그래서 기기 하네스가 사실을 내고 게이트는 그것을 **소비**한다:
>
> ```
> ./scripts/dev-push.sh accept   | tee accept.txt
> ./scripts/dev-push.sh bench-ab | tee bench.txt
> ./scripts/dev-push.sh bench    | tee rw.txt
> bench/regression_gate.py --from accept.txt bench.txt rw.txt
> bench/regression_gate.py --build-only        # CI, 기기 출력 없이 아티팩트 불변식만
> ```
>
> **없는 사실은 PASS 가 아니다** — 인자 없이 부르면 거부하고, 기대한 값이 출력에 없으면 `ABSENT` 로
> 세어 FAIL 시킨다. 빈 파일에 대해 green 을 내는 게이트는 게이트가 아니다. 실제로 이 규칙이 첫
> 실행에서 `rw_*_ns` 패턴 불일치를 잡았다.
>
> **`preload.malloc_calls == 0` 은 여전히 아무것도 검사하지 않는다.** 게이트가 그것을 `UNENFORCED` 로
> **매 실행 출력한다** — 문서 각주에만 있으면 잊히기 때문이다. R1 은 현재 코드 규약과 리뷰로만 지켜지며
> `getaddrinfo` 계열은 문서화된 예외다.
>
> 소프트 게이트는 `bench/baseline.json` 을 쓴다. `--update` 로만 갱신되고 암묵적으로 쓰이지 않는다 —
> 스스로 골대를 옮기는 게이트는 게이트가 아니다. 실패한 실행에서는 `--update` 를 **거부한다**.
>
> ⚠️ **`git_status_10k_ms` 와 `npm_ci_ms` 는 아무것도 측정하지 않는다** — 실측 2026-08-03. 소프트 게이트
> 목록에 있었지만 스크레이퍼에 패턴이 없었고, SOFT 루프는 하드와 달리 없는 키를 **조용히 건너뛴다.**
> 즉 이 문서가 "`npm ci` 가 10% 넘게 느려지면 경고한다" 고 말하는 동안 그 숫자를 만드는 곳이 없었다.
> `malloc_calls` 와 같은 처방을 쓴다 — 게이트가 매 실행 `UNMEASURED` 로 출력해 공백이 소스가 아니라
> **게이트 자신의 출력에** 보이게 한다. 둘 다 손으로 잰 적은 있다([M19 §6.1](evidence/2026-08-03-m19-snapdragon.md),
> [M12 §4](evidence/2026-08-03-m12-spawn-resolver.md)) — 하네스에 넣은 적이 없을 뿐이다.

**하드 불변식 — 어기면 즉시 실패:**
```
supervisor.syscall_stops  == 0
supervisor.path_traps     == 0
preload.rw_total_us       <= 1500      # git status 10k 재작성 총비용
preload.glibc_verneed_max == "2.17"
preload.malloc_calls      == 0         # UNENFORCED — 아래
```

**per-op 는 기기별 회귀 검사 + 절대 천장으로 옮겼다** ([M19 §7](evidence/2026-08-03-m19-snapdragon.md)):

```
preload.rw_{abs,rel,sysdir,under}_ns  <= 그 기기 기준선 × 2.5   (하드)
preload.rw_{abs,rel,sysdir,under}_ns  <= 500/40/150/500 ns      (절대 천장, 건너뛸 수 없음)
```

> ⚠️ **`rw_abs_ns <= 100` 은 더 이상 하드 불변식이 아니다.** 기기 간 이식되지 않기 때문이다 — 동일한 코드가 참조 #1 에서 58.9 ns, 참조 #2 에서 93~129 ns 다. 100 ns 선은 지원 기기 한쪽에서 통과하고 한쪽에서 실패한다. 목표를 옮긴 것이 아니라, **§13 이 원래 적어 둔 총비용 예산으로 게이트를 옮기고** per-op 는 더 촘촘한 기기별 검사로 대체한 것이다.
>
> `preload.malloc_calls` 는 **아무것도 측정하지 않는다.** 게이트가 매 실행 `UNENFORCED` 로 출력해 이 공백이 각주가 아니라 게이트 자신의 출력에 보이게 한다.

`syscall_stops != 0`은 누군가 `PTRACE_SYSCALL`을 도입했다는 뜻이고, **그 순간 이 제품은 PRoot다.** 성능 주장 전체가 무효화되므로 가장 중요한 게이트다.

**소프트 게이트 (경고 후 기록):**
```
git_status_10k_ms   <= 이전 최고 * 1.10
npm_ci_ms           <= 이전 최고 * 1.10
sigsys_per_process  <= 8
```

## 4. 벤치마크 하네스

`alr bench`. 상위 프로젝트의 `bench/`를 이식한다 (APK 비의존이라 거의 그대로 온다).

### 4.1 필수 워크로드

| ID | 워크로드 | 비교 대상 |
|---|---|---|
| `hello` | `/bin/true` × 100 | native Termux / proot-distro / alr |
| `sh_true` | `/bin/sh -c true` × 100 | 동일 |
| `stat_storm` | 1,000회 `stat` | 동일 |
| `open_storm` | 1,000회 `open/read/close` | 동일 |
| `spawn` | 자식 100개 spawn | 동일 |
| **`git_status_10k`** | 10k 파일 저장소의 `git status` | 동일 ← **히어로** |
| **`npm_ci`** | 중간 크기 프로젝트의 `npm ci` | 동일 ← **히어로** |
| `node_cold` | `node -e 0` | 동일 (디엠퍼사이즈) |
| `exec_throughput` | execve/s | 동일 ← [§D2](01-platform-facts.md), 오버헤드가 드러나는 곳 |
| `dpkg_arch` | `dpkg --print-architecture` | 동일 |

### 4.2 리포트 스키마

```
backend        native | proot-distro | alr
device         <model>
soc            <chipset>
android_sdk    <n>
kernel         <version>
selinux        Enforcing | Permissive      ← Permissive면 결과 INVALID
seccomp_mode   2 | 0                       ← 2가 아니면 결과 INVALID
distro         ubuntu-24.04
workload       <id>
iterations     <n>
elapsed_ms     <n>
stddev_ms      <n>
relative_to_proot  <ratio> | PENDING_DEVICE
evidence       MEASURED | MODELED
result         PASS | FAIL | KNOWN_FAIL:<reason>
```

> 위는 **템플릿**이다 — `relative_to_proot` 줄의 `PENDING_DEVICE`는 어휘로 남겨 둔다. 다만 2026-08-03
> 현재 `git_status_10k`(34.8×), `npm_ci`(3.12×), `/bin/true` 기동(10.9×, §2 M8의 `PROC STARTUP` —
> `hello` 워크로드와 반복 횟수가 다르다) 세 항목은 실제 값을 갖는다. **새 리포트가 이 셋을 다시
> `PENDING_DEVICE`로 내면 그건 회귀다.**

### 4.3 측정 규율

1. **`MEASURED`와 `MODELED`를 절대 섞지 말 것.** A/B 실측 전에는 `relative_to_proot=PENDING_DEVICE`.
2. **PRoot 베이스라인을 하나로 고정한다.** `PROOT_NO_SECCOMP=1`(모든 syscall 트랩)은 필드에서 흔한 워크어라운드지만 기본 설정과 **완전히 다른 베이스라인**이다. 한 차트에 섞지 말 것. 기본은 seccomp 켜진 proot-distro 기본값.
3. **워밍업 후 측정.** 콜드 페이지 캐시가 결과를 지배한다. 3회 워밍업 + 5회 측정, 중앙값 보고.
4. **동일 디바이스, 동일 세션, 동일 온도.** thermal throttling이 2배를 만든다.
5. **`getenforce`와 `Seccomp:` 검증 없이는 결과를 발표하지 않는다.** permissive 디바이스는 zygote 필터가 아예 없어 모든 문제가 사라져 보인다.

### 4.4 A/B는 측정됐다 — 남은 미측정 항목

상위 프로젝트는 **PRoot vs ALR A/B를 한 번도 측정하지 못했다** (SELinux가 APK의 rootfs 실행을 막아서).
**Termux가 그것을 처음으로 가능하게 했고, 2026-08-02/03에 실측됐다.** 이것이 M8의 핵심 산출물이며,
이 프로젝트가 상위 프로젝트에 되돌려주는 증거다.

| 항목 | 결과 | 출처 |
|---|---|---|
| `git status` 10k — proot vs native | 1,704 ms vs 42 ms = **40.6×** (정성적 보고만 있던 격차) | [M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md) |
| `git status` 10k — proot vs alr | **34.8×** | 동일 |
| 프로세스 기동 — proot vs alr | **10.9×** (304 vs 28 ms) | 동일 |
| `npm ci` — proot vs alr | **3.12×** (6.24~6.87 vs 1.99~2.00 s, 동일 바이너리·락파일·캐시) | [M12 §4](evidence/2026-08-03-m12-spawn-resolver.md) |
| 경로 계층의 실제 비용 | `git status` 1회당 **≈40 µs** (호출 9,912회 중 재작성 26회) | [M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md) |

> 마지막 줄이 이 표에서 가장 중요하다. **모델은 재작성 13,500회 × 61 ns = 0.82 ms 였고, 실측은 그보다
> 20배 쌌다.** `p[0] != '/'`를 함수 첫 줄에 둔 판단이 호출의 99.7%를 3.9 ns에 처리한다. 즉 **alr 의
> 오버헤드는 경로 계층이 아니다** — 56 ms 중 0.04 ms(0.07%)다. 나머지는 프로세스 기동(+8 ms)과
> 서로 다른 git 빌드·libc 다.

아직 아무도 재지 않은 것 (§4.3-1에 따라 `PENDING_DEVICE`로 둔다. 다만 **막는 것은 기기가 아니라
하네스다** — 기기는 있고 `alr bench`가 없다):
- **V8 JIT + W^X mmap churn 의 proot 오버헤드 분리.** 판정 방법: `node -e` 로 JIT 압력이 큰/없는
  두 워크로드를 같은 node 바이너리로 양쪽에서 돌려 차이를 뺀다. 막는 것: `alr bench` 부재.
- **Node cold start 의 proot 비용** (§2 M8 `ALR BENCH NODE COLD vs PROOT`).
- ~~**스냅드래곤 재측정.**~~ **완료** — [M19 §6](evidence/2026-08-03-m19-snapdragon.md). 다만 배수는 기기마다 달라 인용 시 기기를 함께 적는다. SoC 가 바뀌면 ptrace 왕복
  비용이 바뀔 수 있다 — 이 기기의 호출당 ≈167 µs 는 [§D1](01-platform-facts.md)의 5~20 µs 모델보다
  크고, 그 자체로 별도 확인 가치가 있다.

## 5. 호환성 폭 (compatibility breadth)

[00-product.md §4](00-product.md)의 포지셔닝이 성립하려면 **속도가 아니라 호환성**을 숫자로 방어해야 한다.

`alr bench --breadth`:
```
상위 M개 Ubuntu noble 패키지 중:
  설치 성공        N / M
  실행 성공        N / M
  KNOWN_FAIL 분류  N / M   (reason별 집계)
```

M = 100으로 시작 (curated 목록: 빌드 툴체인, 언어 런타임, CLI 유틸). 이 숫자가 grun 대비 유일한 차별점이므로 **공개 발표의 헤드라인 지표**가 된다.

**실측 (2026-08-03, Ubuntu 24.04)**: `tests/device/breadth.sh`, 큐레이션 96개.

```
ALR BREADTH: install=96/96 run=96/96
```

[M11](evidence/2026-08-02-m11-breadth.md)에서 96/96 설치 · 95/96 실행,
[M14](evidence/2026-08-03-m14-ioctl-php.md)에서 96/96 실행으로 올랐고
[M15](evidence/2026-08-03-m15-cmdline-2604.md)에서 회귀 없이 유지됐다. M 은 100이 아니라 96이다 —
목록이 96개다.

> ⚠️ **마지막 1건(`php-cli`)이 통과한 이유를 모른다.** preload 심볼 수가 152 근처의 임계값을 넘으면
> 통과하고 아래면 `*** buffer overflow detected ***`로 죽는다. php 가 `--version`에서 쓸 수도 없는
> 심볼(`scandir`) 하나만 빼도 재현되므로 **특정 심볼 가설은 반박됐다**. 남은 해석(미확인)은 Ubuntu 24.04
> 의 `_FORTIFY_SOURCE=3` 검사가 런타임 할당 크기에 의존하므로 preload 크기가 힙 레이아웃을 흔든다는
> 것이다. **이것을 "고쳤다"로 읽지 말 것** — 심볼을 덜어내는 변경이 php 를 다시 깨뜨릴 수 있고,
> 그때 원인은 그 변경과 무관할 것이다 ([M14 §2](evidence/2026-08-03-m14-ioctl-php.md)).
>
> **이 숫자는 Ubuntu 24.04 에 대한 것이다.** 26.04 는 설치·부팅·apt 까지 되지만 **uutils(Rust)
> coreutils 계열이 전부 깨진다** — 인라인 `svc` 74개짜리 raw-syscall 바이너리라 인터포저가 닿지 않고,
> [ADR 0006](adr/0006-raw-syscall-binaries.md)이 이를 **비목표**로 확정했다
> ([M15 §2](evidence/2026-08-03-m15-cmdline-2604.md)). 26.04 로 폭을 재면 다른 지표다.

## 6. 숨은 비용 — 발표 전 반드시 측정

[§B1/§B3](01-platform-facts.md): `untrusted_app_27` 도메인은 `execute`와 `execute_no_trans` 양쪽에 `auditallow`가 걸려 있다. **게스트의 모든 execve와 모든 `.so` 매핑이 logd에 감사 레코드를 남긴다.** Node 프로세스 하나가 시작 시 `.so` ~40개를 매핑하면 레코드 ~40개다.

exec 집약 워크로드(`git rebase`, npm postinstall)에서 `logcat -b events` 볼륨을 측정한다. **이것이 PRoot 대비 이 설계의 지배적 숨은 비용일 수 있다.** 오버헤드 주장을 발표하기 전에 수치를 확보한다.

**상태: `PENDING_DEVICE`. 기기는 있는데 관측 지점이 없다** ([RISKS R6](RISKS.md)).

2026-08-03에 확인한 것: **Termux 앱 프로세스 안에서는 이 측정이 불가능하다.**

```
logcat -b events -d    → 0줄, 에러 메시지 없음
logcat -b main   -d    → 정상 출력
```

`main` 버퍼는 읽히는데 `events` 버퍼만 빈다. **조용히 비는 것이 이 실패의 고약한 점이다** — 거부라고
말해 주지 않으므로 "레코드가 없다"로 오독하기 딱 좋다.

> ⚠️ **0줄을 "오버헤드 없음"으로 읽지 말 것.** 그건 측정이 아니라 권한 실패다. 감사 레코드는 우리가
> 못 읽을 뿐 여전히 남는다. 이 문단은 다음 사람이 그 실수를 하지 않도록 있다.

**판정 방법**: 외부 관측자가 필요하다 — adb 로 호스트에서 `logcat -b events` 를 열어 두고, 워크로드는
Termux 안에서 돌린다. exec 집약(`git rebase`, npm postinstall)을 native / alr / proot-distro 세 갈래로
돌려 레코드 수와 실행 시간을 함께 기록한다. **막는 것**: adb 를 붙일 수 있는 호스트(현재 세션은 기기
안에서만 돈다). 이 수치가 나오기 전까지 §2 M8 의 배수는 **로그 비용을 포함한 실측**임을 밝히고
인용한다 — 34.8×·10.9×·3.12× 는 이미 감사 레코드를 치르고 나온 숫자다.
