# 06 — CLI 스펙

바이너리: `$PREFIX/bin/alr`. 언어: C11 + C++17, NDK 29 / `--target=aarch64-linux-android24` 크로스 빌드.

## 1. 서브커맨드

```
alr install <distro> [--with git,node,codex] [--force] [--url <tarball>]
alr remove  <distro> [--force]  rootfs 삭제 (확인 프롬프트)
alr list
alr update-components [<distro>]

alr run [옵션] <command> [args...]     단일 명령 실행
alr shell [옵션]                       게스트 로그인 셸 (기본 bash -l)
alr exec [옵션] -- <command> [args...] run과 동일하나 옵션 파싱 모호성 없음

alr doctor [probe-dir]                 디바이스 능력 진단 (플래그 없음 — 아래 ⚠️)
alr version
alr config [get <key> | set <key> <value>]
```

> **`alr bench` 는 없다 — 서브커맨드가 아니라 디바이스 하네스다.** 벤치마크는 [`tests/device/bench.sh`](../tests/device/bench.sh) 와 `scripts/dev-push.sh bench` / `bench-ab` 가 들고 있고, 그 결과를 [`bench/regression_gate.py`](../bench/regression_gate.py) 가 기기별 baseline 과 대조한다. 유효한 측정은 **Termux 앱에서 fork 된 프로세스**를 요구하는데(uid ≥ 10000, `Seccomp: 2`) — `alr bench` 를 서브커맨드로 두면 그 조건을 만족하지 않는 곳에서 부를 수 있고, 그렇게 나온 숫자는 존재하지 않는 기기를 잰 것이다. [07 §4](07-acceptance.md) 가 하네스 쪽 계약이다.

### 1.1 `run` / `shell` / `exec` 공통 옵션

| 옵션 | 기본 | 설명 |
|---|---|---|
| `-d, --distro <name>` | 설정의 `default_distro` | 사용할 rootfs |
| `-w, --workdir <guest path>` | `/root` 또는 매핑된 cwd | 게스트 cwd |
| `-e, --env KEY=VAL` | — | 게스트 env 추가 (반복 가능) |
| `--fakeroot` / `--no-fakeroot` | 설정값 | uid 0 스푸핑 |
| `--log <0\|1\|2>` | 0 | 진단 상세도 |
| `--no-supervisor` | off | **위험**. 슈퍼바이저 없이 실행 (디버깅 전용, 대개 부팅 실패) |

### 1.2 cwd 매핑

- **현재 디렉토리가 `<R>` 아래면**: 대응하는 게스트 경로로 매핑.
- **그 밖이면**: `/root`로 폴백하고 경고를 낸다.

### 1.3 v1에는 바인드 매핑이 없다

**v1에서 게스트에게 보이는 것은 `<R>` 아래뿐이다.** `--bind`도, `$HOME` 자동 바인드도 없다.

> **왜 뺐는가**: 바인드는 [04-preload-spec.md §5.1](04-preload-spec.md)의 `rw()`가 **단일 무조건 접두사 붙이기**라는 설계와 양립하지 않는다. 바인드를 지원하려면 (a) 게스트 env로 바인드 테이블을 전달하는 변수, (b) `rw()`의 핫패스에 테이블 조회(≤100 ns 예산 위협), (c) `guest_canon()`의 역매핑 — 없으면 `getcwd`/`realpath`/`/proc/self/cwd`가 생 호스트 경로를 반환하고 다음 절대 open이 그것을 다시 접두사 붙여 이중 경로가 된다 — 이 셋이 모두 필요하다. 어느 것도 M1~M8에 마일스톤·acceptance·테스트가 없다.
>
> **Termux 홈의 프로젝트에서 작업하려면** 그 디렉토리를 rootfs 안으로 옮기거나(`<R>/root/` 아래), rootfs 안에서 클론한다. 이것이 v1의 워크플로다.

**v1.1 후보**: 바인드를 다시 넣으려면 위 (a)(b)(c)를 하나의 설계로 묶어 ADR을 쓰고, `rw()` 예산을 재측정하고, 역매핑 테스트를 acceptance에 추가한 뒤에 한다. 부분 구현은 조용한 경로 손상을 만든다.

## 2. 설정 파일

`$PREFIX/etc/alr/config.toml` (전역) → `$HOME/.alr/config.toml` (사용자) 순으로 병합.

**우선순위**: 플래그 > 환경변수 > 사용자 설정 > 전역 설정 > 내장 기본값.

```toml
default_distro = "ubuntu-24.04"

[runtime]
fakeroot = true          # apt/dpkg에 필요
log      = 0

[paths]
root = "$PREFIX/var/lib/alr"   # 선행 $PREFIX만 전개된다

[network]
mirror = ""              # 비우면 cdimage.ubuntu.com 사용 (권장)
```

| 키 | 대응 환경변수 | 내장 기본값 |
|---|---|---|
| `default_distro` | `ALR_DISTRO` | `ubuntu-24.04` |
| `runtime.fakeroot` | `ALR_FAKEROOT` | `false` |
| `runtime.log` | `ALR_LOG` | `0` |
| `paths.root` | `ALR_ROOT_DIR` | `$PREFIX/var/lib/alr/distros` |
| `network.mirror` | — | `https://cdimage.ubuntu.com/ubuntu-base/releases` |

```
$ alr config                       # 전 설정, 유효값, 그리고 출처
$ alr config get runtime.fakeroot  # 유효값 하나만 (스크립트용)
$ alr config set runtime.fakeroot true
```

**출처 열이 이 명령의 값어치다.** *"fakeroot 는 true"* 는 행동으로 옮길 수 없고, *"fakeroot 는 true, 환경변수에서"* 는 네 곳 중 어디를 고쳐야 하는지 알려 준다 — 설정 덤프를 읽는 사람이 실제로 묻는 질문이 그것이다.

> ⚠️ **스키마가 이 문서의 이전 판보다 작다. 두 키를 실측으로 떨어냈다.**
>
> **`[env] passthrough`** — 이미 전부에 대해 참이다. `alr` 은 호스트 environ 을 blocklist 만 빼고 통째로 게스트에 복사한다(`src/cli/alr.c`). 그래서 "넘길 이름 목록" 은 넘기는 것을 늘리지 못하고 **줄이는 것밖에** 못 하는데, 그건 이 키의 뜻이 아니다. **어떤 결과도 바꿀 수 없는 키는 없는 것보다 나쁘다** — 조작 수단처럼 읽히기 때문이다.
>
> **`[runtime] supervisor`** — 이 문서가 스스로 붙여 둔 주석이 *"false 로 두지 말 것 — 부팅이 안 된다"* 였다. 기본값이 아닌 유일한 값이 이후 모든 실행을 깨뜨리는 **영속** 설정은 지뢰다. 한 번짜리 `--no-supervisor` 가 이미 있고 `reason=no-supervisor-requested` 로 자기를 알린다.
>
> **`[binds]`** 는 v1 에 없다 — [§1.3](#13-바인드-마운트는-v1에-없다) 참조.

**모르는 키는 무시하지 않고 보고한다.** 오타 난 설정이 받아들여진 것처럼 보이는 것이 설정 파일의 대표적 실패 방식이다.

**`alr config set` 은 사용자 파일만 고친다.** 전역 파일은 공유될 수 있고 이 명령이 다시 쓸 것이 아니다. 그리고 사용자 파일을 **새로 읽어서** 고친다 — `cfg()` 가 들고 있는 것은 두 파일이 **병합된** 결과라, 그걸 되쓰면 전역 설정 전부를 사용자 파일로 조용히 복사해 얼려 버린다.

**주석은 보존되지 않는다.** 스키마가 닫혀 있고, 다섯 개 키를 위해 주석을 왕복시키려면 문서 모델 전체를 들고 있어야 한다. 쓴 파일의 첫 줄이 그렇게 적혀 있다.

## 3. `alr doctor`

**진단 도구다.** 기기가 이상하게 굴 때, 그리고 **지원 대상(Android 16) 밖에서 돌릴 때** 실행한다 — 리포트와 붙여 넣을 수 있는 에뮬레이션 표를 출력한다.

> ⚠️ 원래 이 자리에는 "결과를 `state/<distro>/doctor.json`에 캐시하고 런타임이 읽는다" 가 있었다. **구현된 적이 없고**(그 파일을 읽거나 쓰는 코드가 0건), [ADR 0007](adr/0007-android-16-only.md) 로 릴리스별 분기가 사라지면서 필요도 없어졌다.

[01-platform-facts.md §G](01-platform-facts.md)의 **P0~P10** 을 실행한다(P2a 포함).

> ⚠️ **구현과 스펙이 다른 부분 — 실측 2026-08-03.** `alr doctor` 는 **플래그를 받지 않는다.** 이 문서가 적어 둔 `--json`·`--full` 은 구현된 적이 없고, `argv[1]` 은 **프로브 디렉토리**로 해석된다. 그래서 문서대로 `alr doctor --json` 을 치면 멀쩡한 기기에서 `probe dir --json` 으로 프로브가 실패해 **`VERDICT: NOT READY` 와 "design dead"** 라는 가짜 진단이 나왔다. 지금은 알 수 없는 옵션을 `reason=doctor-unknown-option` 으로 거절한다(exit 2). **`--json` 출력은 여전히 없다.**
>
> **P11·P12 는 2026-08-04 에 구현됐다** — [§G](01-platform-facts.md) 표와 아래 §3.1 샘플 출력이 실측이다. 이 문장은 그 이전 상태를 적어 둔 것이었다.

### 3.1 출력 형식

```
alr doctor — device capability report

  host
    android_sdk        35
    selinux            Enforcing                    [P1] PASS
    seccomp_mode       2                            [P1] PASS
    termux_build       f-droid (targetSdk 28)       [P3] PASS
    abi                arm64-v8a
    page_size          4096

  execution
    exec app_data_file                              [P3] PASS
    anon mmap RW→RX                                 [P4] PASS
    file-backed PROT_EXEC mmap (rootfs)             [P5] PASS
    getrandom                                       [P10] PASS
    memfd_create                                    [P10] PASS

  filesystem
    link(2) same-dir                                [P6] FAIL EACCES  → link2symlink 활성화
    /dev/full                                       [P9] FAIL EACCES  → 에뮬레이션 활성화
    /dev/{null,zero,urandom,tty,ptmx}               PASS
    posix_openpt/grantpt/unlockpt                   [P8] PASS

  namespaces
    unshare(CLONE_NEWUSER)                          [P7] EINVAL (예상됨)

  seccomp blocked syscalls                          [P2]
    99   set_robust_list      → emulate 0
    100  get_robust_list      → emulate -ENOSYS
    293  rseq                 → emulate -ENOSYS
    437  openat2              → emulate -ENOSYS
    439  faccessat2           → emulate -ENOSYS
    ... (총 N개, 에뮬레이션 테이블에 없던 것 M개는 ⚠️ 로 표시)

  raw-syscall sweep                                 [P11]
    (아래는 지어낸 예시가 아니라 `MEASURED` 2026-08-04, SM-X236N)
      .../usr/bin/cargo                              39 svc
      .../usr/bin/fzf                                38 svc
      .../usr/bin/rg                                  4 svc
      .../usr/lib/go-1.22/bin/go            STATIC,  38 svc
      .../usr/lib/go-1.22/pkg/tool/linux_arm64/asm  STATIC, 36 svc
      ... and 20 more (showing 20)
    745 executables scanned, 40 issue their own syscalls or are static -> WARN

  process budget                                    [P12]
    live descendants 40 of 40 / phantom limit 32 -> PASS

  VERDICT: READY   (link2symlink=on, devfull_emu=on)
```

### 3.2 실패 등급

| 등급 | 의미 | 예 |
|---|---|---|
| `FATAL` | 제품이 동작 불가 | P3 exec 거부, P5 file-backed exec 거부, P10 getrandom 차단 |
| `MITIGATED` | 기능이 켜져서 해결됨 | P6 link → link2symlink, P9 /dev/full → 에뮬 |
| `EXPECTED` | 정상 | P7 EINVAL |
| `WARN` | 사용자가 알아야 함 | **P0 미지원 Android 릴리스**, P11 Go 바이너리, P12 프로세스 예산 |
| `INVALID` | 측정 결과를 신뢰할 수 없음 | P1 permissive 디바이스 |

**`INVALID`면 벤치마크 결과에 무효 표시가 붙는다** ([00-product.md §6.6](00-product.md)). 실제로 그 판정을 내리는 것은 `alr bench`(존재하지 않는다 — §1 참조)가 아니라 [`tests/device/bench.sh`](../tests/device/bench.sh) 자신의 유효성 게이트다: uid ≥ 10000 이고 `Seccomp: 2` 가 아니면 **숫자를 내지 않고 거부한다.** 무효 표시를 붙이는 것보다 낫다 — 표시가 붙은 숫자는 결국 인용된다.

### 3.3 P2 (syscall 스윕) 구현 주의

`PTRACE_SECCOMP_GET_FILTER`는 `CAP_SYS_ADMIN`이 필요해 쓸 수 없다. 대신:
- 자식 프로세스를 fork하고 SIGSYS 핸들러를 설치
- syscall 0..460을 **무해한 더미 인자로** 호출 (부작용 있는 것은 스킵 목록으로 제외: `exit`, `execve`, `kill`, `reboot`, `unlink` 등)
- SIGSYS가 오면 차단, `ENOSYS`면 미구현, 그 외면 허용으로 분류
- 각 syscall마다 자식을 새로 fork하는 것이 안전하다 (하나가 프로세스를 망가뜨려도 격리)

**지원 대상인 Android 16 기기에서 돌린다** — 새 벤더·새 커널을 만날 때마다다([ADR 0007](adr/0007-android-16-only.md)). allowlist 는 릴리스마다 늘었지만(365 → 392줄) 다른 릴리스는 범위 밖이므로 이 스윕의 대상이 아니다.

## 4. `alr bench`

[07-acceptance.md](07-acceptance.md) 참조.

## 5. 출력 규약

- **stdout은 게스트 것이다.** alr의 진단은 전부 stderr 또는 `ALR_LOG_FD`로.
- 종료 코드는 게스트의 것을 그대로 전파 (`WIFSIGNALED` → `128 + sig`).
- alr 자신의 실패는 `125`를 쓴다 (`env`/`nice`의 관례와 일치, 게스트 코드와 충돌 최소화).
- ~~`--json`이 있으면 stdout에 JSON 한 덩어리, 사람용 출력 없음.~~ **미구현** — 어느 서브커맨드에도 `--json` 이 없다.

## 6. 시그널 처리

`alr`이 받는 시그널을 게스트 프로세스 그룹으로 포워딩한다:

| 시그널 | 동작 |
|---|---|
| `SIGINT`, `SIGTERM`, `SIGQUIT`, `SIGHUP` | `kill(-leader_pgid, sig)` |
| `SIGWINCH` | 포워딩 (터미널 크기 변경) |
| `SIGTSTP`, `SIGCONT` | 포워딩 (잡 컨트롤) |
| `SIGCHLD` | 슈퍼바이저 루프가 처리 |

**터미널 소유권**: `alr shell`은 게스트를 포그라운드 프로세스 그룹으로 만들어야 한다 (`tcsetpgrp`). Android에 제약은 없다 ([§B8](01-platform-facts.md)).

## 7. 에러 메시지 품질

사용자가 볼 실패는 **원인과 다음 행동**을 담는다. 예:

```
alr: 게스트 실행 실패
  reason=ldso-missing
  <R>/lib/ld-linux-aarch64.so.1 가 없습니다.
  rootfs가 손상되었을 수 있습니다. `alr install ubuntu-24.04 --force` 로 재설치하세요.
```

```
alr: Play Store 버전 Termux는 지원하지 않습니다
  reason=unsupported-android-policy
  이 Termux는 targetSdk >= 29 라 앱 데이터 경로의 실행이 SELinux에 의해 거부됩니다.
  F-Droid 또는 GitHub 릴리스 버전을 설치하세요: https://github.com/termux/termux-app/releases
```

```
alr: 게스트 프로세스가 상태 없이 사라졌습니다
  reason=android-phantom-process-kill
  Android가 앱당 백그라운드 프로세스를 32개로 제한합니다 (현재 자손 31개).
  Termux 알림에서 wake lock 을 켜거나 동시 프로세스를 줄이세요.
```
