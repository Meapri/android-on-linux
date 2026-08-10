# M12 — spawn 계열·리졸버·`/proc/stat`, 그리고 `npm ci` 실측

- **날짜**: 2026-08-03
- **기기**: MediaTek arm64, `uid=10297 Seccomp=2 u:r:untrusted_app_27:s0`
- **게스트**: Ubuntu 24.04.4 base arm64, glibc 2.39, 패치 없음
- **수용 테스트**: **PASS=73 FAIL=0 KNOWN_FAIL=2** (이전 60/0/1). 신규 13건 추가, 회귀 0
- **호환성 폭**: 96/96 설치, 95/96 실행 — **회귀 없음**

이 라운드는 정적 감사로 시작했다. 5개 갭이 반박 과정을 통과했고, 그중 **하나는 개발 환경으로서 치명적인 결함**이었는데 기존 테스트 전부를 통과하고 있었다.

## 0. 먼저 잰다 — 크기를 정하기 전에

계획의 첫 단계는 코드가 아니라 **측정 블록**이었다. 네 개의 미확인 질문을 한 번에 답하게 하고, 그 답이 나오기 전에는 아무것도 구현하지 않았다. 이 프로젝트에서 반복된 실수가 정확히 그 반대였기 때문이다 — 추정으로 크기를 정하고, 편한 CLI 도구로 검증하고, 나중에 프록시가 틀렸다는 걸 발견하는 것.

## 1. spawn 계열 — `make` 가 깨져 있었다

`docs/04-preload-spec.md` §9.4 는 exec 진입점 13개를 필수로 선언한다. 구현되어 있던 것은 7개였다. **`posix_spawn`, `posix_spawnp`, `system`, `popen`, `pclose`, `fexecve`, `execveat` 이 없었다.**

glibc 의 `posix_spawn` 은 내부 `__spawnix` 로 커널에 도달한다 — 우리 `execve` 를 절대 거치지 않는다. `__nss_files_fopen`(§6.17), `__gen_tempname`→`__open`(§6.14) 과 같은 계열이다.

### 측정 — 셸 프록시가 아니라 실제 진입점으로

이 프로젝트는 같은 함수군을 다른 glibc 경로로 부르는 도구로 검증했다가 세 번 틀렸다(`mktemp`↔`mkstemp`, `touch -d`↔`utimes`, `mv`↔`rename`). 그래서 게스트의 gcc 로 프로브를 컴파일해 **실제 심볼에 바인딩**했다(`tests/device/probe_spawn.c`):

```
posix_spawn   rc=0 ok
  child: CANNOT LINK EXECUTABLE "echo": ".../libc.so" has bad ELF magic: 2f2a2047
posix_spawnp  rc=0 ok
  child: CANNOT LINK EXECUTABLE "echo": ...
system        rc=256
popen         pclose=256 out=(empty)
```

`2f2a2047` 은 `/* G` 다 — 게스트의 `libc.so` 는 GNU ld 스크립트이고, 그것을 읽고 있는 것은 **Android 의 bionic 링커**다. 즉 자식이 로더 재디스패치를 거치지 않고 커널이 직접 exec 했다.

**요란한 실패이지 조용한 탈출이 아니라는 점은 다행이다** — 게스트 파일시스템 대신 Android 파일시스템에서 조용히 성공하는 시나리오가 최악인데, 그건 아니었다.

### 실사용 영향

| 경로 | 결과 |
|---|---|
| `python3 subprocess.run` | ✅ 동작 (fork+exec 폴백) |
| `perl system` | ✅ 동작 (공개 `execvp`) |
| `python3 os.system` | ❌ rc 256 |
| **`make`** | ❌ `make: echo: No such file or directory` / `Error 127` |

**GNU make 4.3+ 는 모든 레시피 줄을 `posix_spawn` 으로 띄운다.** 개발 환경을 표방하면서 `make` 가 안 되고 있었다.

> 왜 안 드러났나: [M11](2026-08-02-m11-breadth.md) 의 breadth 검사에서 `build-essential` 항목은 `sh -c 'gcc … && /tmp/bt'` 였다. **셸이 직접 부르는 경로**라 통과했다. 네 번째 잘못된 프록시다.

### 수정

`exec_dispatch()` 에서 `exec_build()` 를 분리해, exec 와 spawn 이 **동일한 해석 경로**를 공유하게 했다. shebang 체인은 재귀 대신 **반복**으로 푼다 — 결과 벡터가 호출을 넘어 살아남아야 하므로 각 단계의 인터프리터 문자열이 호출자 버퍼(`ibuf[level]`)에 있어야 한다. 모든 저장소는 호출자 스택이다(R1: malloc 금지 유지).

`posix_spawn` 은 **(path, argv) 를 로더화한 뒤 진짜 `posix_spawn` 에 넘긴다.** 직접 재구현하지 않은 이유는 `file_actions` 와 `attrp` 를 건드리지 않고 그대로 보존하기 위해서다. `system`/`popen` 은 그 위에 얹었다.

`system()` 은 POSIX 대로 SIGINT/SIGQUIT 를 무시하고 SIGCHLD 를 블록한다 — `make` 와 configure 스크립트가 그 동작에 의존한다.

### 수정 후

```
posix_spawn   rc=0 ok        → SPAWN_ABS
posix_spawnp  rc=0 ok        → SPAWNP
system        rc=0           → ID=ubuntu      ← 게스트 셸이 게스트 파일시스템을 봄
popen         pclose=0       → ID=ubuntu
make                         → MAKEOK
python3 os.system            → SYSOK, rc 0
make 로 실제 C 컴파일·실행    → exit=42
```

`system` 이 **`ID=ubuntu`** 를 반환하는 것이 핵심 판별이다. Android 의 mksh 가 폰 파일시스템에서 돌았다면 다른 값이 나온다.

## 2. 리졸버 — 브리지가 절반만 덮고 있었다

`getaddrinfo`/`freeaddrinfo` 만 브리지되어 있었다. `gethostby*` 계열 전체와 `getnameinfo` 는 게스트 glibc 리졸버로 갔는데, **그건 여기서 죽어 있다** — Private DNS/VPN 이 평문 53 을 막는 것이 브리지가 존재하는 이유 그 자체다.

```
python3 socket.getaddrinfo("ports.ubuntu.com", 80)   → ('91.189.92.19', 80)   ✅
python3 socket.gethostbyname_ex("ports.ubuntu.com")  → herror 2               ❌
```

> `socket.gethostbyname()` 은 유효한 프로브가 **아니다.** CPython 은 그것을 `setipaddr()` → `getaddrinfo` 로 보낸다 — 이미 동작하던 경로다. 레거시 심볼에 실제로 바인딩하는 것은 `gethostbyname_ex` / `gethostbyaddr` 다.

### 그리고 게스트 `/etc/hosts` 는 아무도 읽지 않았다

브리지는 **bionic** 에 물어보므로 Android 의 `/system/etc/hosts` 를 읽는다. rootfs 자신의 `/etc/hosts` 는 양쪽 경로 모두에서 소비자가 없었다. Android 가 절대 제공할 수 없는 주소로 확인:

```
$ echo '10.99.99.99 alrprobe.invalid' >> $R/etc/hosts
$ alr run getent hosts  alrprobe.invalid   → (빈 출력)
$ alr run getent ahosts alrprobe.invalid   → (빈 출력)
```

### 수정 — `hosts: files dns`

`files` 절반을 §6.17 과 같은 방식으로 직접 구현하고, `dns` 절반은 브리지에 맡긴다. 레거시 계열은 우리 `getaddrinfo` 위에 얹어, 두 백엔드를 자동으로 상속한다.

추가 심볼: `gethostbyname`, `gethostbyname2`, `gethostbyname_r`, `gethostbyname2_r`, `gethostbyaddr`, `gethostbyaddr_r`, `getnameinfo`.

역방향은 `/etc/hosts` 로만 답한다. 브리지에 역방향 경로가 없고, 게스트 리졸버로 폴백하면 **브리지가 우회하려는 바로 그 53 차단에서 20초 멈춘다** — 정직한 `HOST_NOT_FOUND` 가 낫다. `getnameinfo` 도 같은 이유로 `NI_NAMEREQD` 가 아니면 숫자 표기로 떨어진다.

```
getent hosts   alrprobe.invalid  → 10.99.99.99  alrprobe.invalid
getent ahosts  alrprobe.invalid  → 10.99.99.99  STREAM alrprobe.invalid
getent hosts   10.99.99.99       → alrprobe.invalid          (역방향)
python gethostbyname_ex(실호스트) → ('ports.ubuntu.com', …, ['91.189.92.21'])
perl gethostbyname(실호스트)      → ports.ubuntu.com
```

> `gethostbyaddr` 만 구현하고 `gethostbyaddr_r` 을 빼먹었더니 CPython 의 `socket.gethostbyaddr` 이 계속 herror 2 였다. CPython 은 `_r` 형에 바인딩한다. **다섯 번째 같은 실수** — 이번엔 즉시 잡았다.

## 3. `/proc/stat` — `os.cpus()` 가 0개

```
$ node -e 'console.log(require("os").cpus().length)'   → 0
$ head -1 /proc/stat            → Permission denied
$ nproc                         → 8
$ getconf _NPROCESSORS_ONLN     → 8
$ node -e '…availableParallelism()' → 8
$ head -2 /proc/cpuinfo         → processor : 0        ← 읽힘
```

Android 가 앱 uid 에 `/proc/stat` 을 `EACCES` 로 막는다. libuv 의 `uv_cpu_info()` 는 거기서 `cpuN` 줄을 찾으므로 빈 배열을 돌려준다. `os.cpus().length` 로 워커 풀 크기를 정하는 코드는 0을 받는다.

**CPU 개수는 진짜로 알 수 있다**(`sched_getaffinity` 가 동작해 `nproc`=8). 그래서 `/proc/mounts` 와 같은 합성 메커니즘으로 `/proc/stat` 을 만든다. 개수는 실제 값이고 **시간 필드는 전부 0** 이다.

> ⚠️ 델타로 CPU **사용률**을 계산하는 도구는 항상 0% 를 본다. 이건 측정값이 아니라 문서화된 한계다([RISKS](../RISKS.md)). `/proc/cpuinfo` 는 읽히므로 건드리지 않는다 — 진짜 SoC 데이터를 조작으로 대체하면 `lscpu` 가 퇴보한다.

또한 합성은 **진짜 `/proc/stat` 이 정말 못 읽힐 때만** 발동한다. 허용하는 기기에서는 진짜 숫자가 무조건 낫다.

```
node os.cpus().length  → 8
/proc/cpuinfo          → CPU part : 0xd05      ← 진짜 값 유지
```

## 4. `npm ci` — G5 의 마지막 PENDING_DEVICE

**동일 조건 A/B.** node 바이너리와 npm 을 **복사해서** 양쪽을 같게 만들었다(v22.20.0 / 10.9.3), 락파일·npm 캐시도 동일. [M8](2026-08-02-m7-m8-workloads-perf.md) 의 git 비교를 약하게 만들었던 "빌드 상이" 문제를 제거한 것이다.

| | proot-distro | alr | 배수 |
|---|---|---|---|
| `npm ci` (105 패키지) | 6.87 / 6.24 / 6.26 s | 2.00 / 2.00 / 1.99 s | **3.12×** |

양쪽 모두 105개 패키지 설치, `tsc --version` 동작 확인. alr 쪽은 `.bin` 심링크(`../typescript/bin/tsc`)와 실제 TypeScript 컴파일·실행까지 검증했다.

`docs/00-product.md` §4 의 방어 가능 범위가 **1.5–3×** 였으니 추정이 정확했다 — `git status` 의 34.8× 처럼 빗나가지 않았다.

**남은 caveat**: proot 게스트는 Ubuntu 26.04, alr 게스트는 24.04 다. node/npm/락파일/캐시는 동일하지만 베이스 배포판이 다르다. 단일 MediaTek 기기, 1회 세션.

## 5. `alr install` 이 `--url` 없이 동작한다

`docs/05-provisioning-spec.md` §1.1 의 SHA256SUMS 탐색을 구현했다. `ubuntu-base-24.04-base-arm64.tar.gz` 는 404 이고 `latest` 심링크가 없으므로, SHA256SUMS 를 읽어 **가장 높은 포인트 릴리스**를 고르고 그 해시로 검증한다.

```
$ alr install
alr: resolving the current ubuntu-base point release
alr: …/ubuntu-base-24.04.4-base-arm64.tar.gz
alr: sha256 ok
```

캐시된 tarball 도 매번 재검증한다. 불일치하면 한 번 다시 받고, 그래도 안 맞으면 지우고 실패한다 — 오래된 포인트 릴리스가 캐시에 남아 있는 것이 흔한 경우이기 때문이다. `--url` 로 직접 지정하면 대조할 해시가 없으므로 검증하지 않는다.

## 6. 절차 — 이번에도 검증 장치가 두 번 틀렸다

1. **대조 실험이 무효였다.** `ALR_NO_NSS` 빌드를 배포했다고 믿었는데 `dev-push.sh preload` 가 내부에서 재빌드하며 플래그를 전달받지 않았다. **배포본 심볼을 세어 보고** 알았다 — 그리고 대조군이 실제로 대조가 되는지(`getent` 가 빈 출력을 내는지) 확인하고서야 결론을 냈다.
2. **`dpkg-divert … | head -1` 이 SIGPIPE 로 명령을 죽였다.** "Adding 'local diversion…'" 메시지는 출력되었지만 데이터베이스에는 기록되지 않았고, 다음 `libc-bin` 업그레이드가 no-op 셰임을 되돌렸다. **또 메시지를 보고 상태를 확인하지 않은 것이다.** (`alr.c` 의 실제 코드 경로는 파이프가 없어 무관하다.) 이제 수용 테스트가 파일이 아니라 **dpkg 데이터베이스**를 검사한다.

두 번 모두 같은 규칙으로 잡혔다: **실패를 코드 탓으로 돌리기 전에 어느 바이너리·어느 상태가 실제인지 확인한다.**

## 7. 정직성 정정 — `KNOWN_FAIL=1` 이 뜻하던 것

[M11 §6](2026-08-02-m11-breadth.md) 의 "남은 KNOWN_FAIL: `/dev/full` 하나" 는 **제품이 아니라 그때 존재하던 테스트에 대한 진술**이었다. 그 시점에 spawn 계열과 리졸버 갭이 코드에 살아 있었지만, 검사하는 테스트가 없어 PASS 로도 FAIL 로도 세어지지 않았다. m11 에 정정을 달았다.

앞으로 수치를 낼 때는 **커버리지의 한계를 같이 적는다.**

## 8. `codex` 는 정적 링크다 — `PASS` 가 뜻하던 것보다 훨씬 약하다

통합 검증 중 codex 가 매번 이 경고를 내는 것을 확인했다:

```
WARNING: proceeding, even though we could not create PATH aliases:
         Read-only file system (os error 30)
codex-cli 0.146.0
```

"Read-only file system" 은 경로가 Android 의 읽기 전용 루트로 샜을 때의 전형적 신호다. 확인:

```
$ llvm-readelf -l …/usr/local/bin/codex
Elf file type is EXEC (Executable file)
There are 6 program headers …
  LOAD  …                       ← INTERP 프로그램 헤더 없음
$ llvm-readelf -d …/usr/local/bin/codex | grep NEEDED
  (없음)
$ ALR_LOG=2 alr run …/codex --version | grep -c 'alr preload:'
0                               ← 대조: git 은 1
```

**codex 는 정적 링크 바이너리다**(ET_EXEC, 269 MB, `NEEDED` 없음). `LD_PRELOAD` 는 원리적으로 닿지 않으므로 **경로 가상화가 전혀 적용되지 않는다** — codex 의 모든 경로 연산은 rootfs 가 아니라 Android 파일시스템으로 간다.

따라서 `ALR CODEX VERSION: PASS` 는 **바이너리가 실행된다**는 뜻이지 **게스트 안에서 동작한다**는 뜻이 아니다. 이건 [RISKS](../RISKS.md) 의 "정적 링크 게스트 바이너리" 한계에 정확히 해당하는데, 제품 문서의 G5 는 그 구분 없이 "codex 실사용 가능" 으로 적고 있었다. 정정했다.

`node`/`npm` 에는 해당하지 않는다 — 둘 다 동적 링크라 정상적으로 가상화된다. 수용 테스트에 `ALR CODEX LINKAGE` 를 추가해, 이 상태를 추적하고 향후 동적 빌드로 바뀌면 알아채도록 했다.

> 이 결함을 잡은 방식도 기록해 둘 만하다. 처음에는 `strings | grep -m2` 와 `readelf | grep -c` 로 판단했는데 **둘 다 도구가 실패해도 "없음" 으로 보이는 형태**였다. 도구의 실제 출력과 종료코드를 보고서야 확정했다. `grep -c` 는 증거가 아니다.

## 9. `/dev/full` — 구현하지 않고 비목표로 재분류

정적 감사의 권고를 받아들였다. 이유:

- 서빙하려면 프로세스에서 가장 뜨거운 syscall 인 `write()` 를 인터포즈해야 하는데, 대상 워크로드 중 이 디바이스 노드를 쓰는 것이 없다.
- preload 자신이 내부적으로 `write()` 를 호출하고 `-fvisibility=default` 로 빌드되므로, `write` 를 정의하면 **자기 호출을 스스로 가로챈다** — 빌드가 이미 `__*_chk` 에 대해 방어하고 있는 바로 그 자기 재귀 계열이다.
- 실패 표면이 열려 있다(`puts`, `putchar`, `fwrite_unlocked`, `dprintf`, …). **빠뜨린 심볼은 전부 조용히 성공한 쓰기가 된다.** 열거 가능하고 요란하게 실패하는 `mkstemp`(9개)·NSS(15개) 계열과 다르다.

수용 테스트도 함께 고쳤다: 기존 프로브 `: < /dev/full` 은 **O_RDONLY 열기**라, `/dev/zero` 로 리다이렉트만 해도 통과한다 — §12 가 금지하는 지름길이 그대로 통과하는 검사였다. 이제 실제 쓰기가 `ENOSPC` 를 내는지 본다.
