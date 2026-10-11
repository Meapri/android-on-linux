# T5 스파이크 — tawcroot 레인과 alr 슈퍼바이저의 공존

- **날짜**: 2026-10-11
- **엔진 라벨**: `engine=alr` (preload 레인), `engine=tawcroot` (vendored tawcroot, 프로토타입. 제품에 링크하지 않았다)
- **기기**: Samsung SM-S937N, Snapdragon 8 Elite SM8750, Android 17 (SDK 37), 커널 6.6.127
- **컨텍스트**: `uid=10057  u:r:untrusted_app_27:s0  Seccomp=2` (Termux sshd를 거친 셸)
- **게스트**: Ubuntu 26.04.1 LTS (glibc 2.43)
- **목적**: [ADR 0005](https://github.com/Meapri/android-on-linux/blob/main/docs/adr/0005-termux-first-goals.md) T5의 첫 질문, "alr의 ptrace 슈퍼바이저와 tawcroot의 `SIGSYS` 핸들러가 한 프로세스에서 공존할 수 있는가"

## 결론

- **공존은 가능하다.** 두 필터의 트랩은 `si_errno`(`SECCOMP_RET_DATA`)로 구별되고, 그 구별을 슈퍼바이저에 넣으니 tawcroot가 슈퍼바이저 아래에서 동작했다.
- **하지만 tawcroot 레인을 ptrace 아래에서 돌리면 안 된다.** 추적 중인 프로세스는 넘겨주는 트랩마다 트레이서를 왕복하므로, 트랩 하나가 0.5 µs에서 약 60 µs로 늘었다. 실제 워크로드는 40배 느려졌다.
- 따라서 레인 설계는 "정적 바이너리로 exec하는 순간 그 프로세스를 슈퍼바이저에서 떼어 낸다"여야 한다. zygote 트랩은 tawcroot가 스스로 처리한다(아래 §5).

## 1. 두 필터의 트랩은 구별된다

`RET_TRAP`이 보내는 `SIGSYS`의 `si_errno`에는 그 결과를 낸 필터의 `SECCOMP_RET_DATA`가 들어간다. 기기에서 직접 쟀다.

```
zygote only (vhangup)              nr=58   trapped=yes si_errno=0x0
own filter only (getppid)          nr=173  trapped=yes si_errno=0x7477
both filters (vhangup)             nr=58   trapped=yes si_errno=0x7477
```

- zygote 필터의 데이터는 0이다.
- 둘 다 막는 syscall에서는 나중에 설치한 필터(게스트 쪽)의 데이터가 보고된다. 커널 문서의 "같은 우선순위면 가장 최근 필터의 데이터"와 일치한다.

## 2. 수정 전: 슈퍼바이저가 tawcroot의 트랩을 삼켰다

수정 전 슈퍼바이저는 `si_code == SYS_SECCOMP`인 `SIGSYS`를 모두 zygote 차단으로 보고 표의 값을 돌려줬다. tawcroot도 데이터 0인 `RET_TRAP`을 썼다.

```
alr supervisor: pids=2 sigsys=23 emulated=23 passthrough_signals=1 path_traps=0 syscall_stops=0
     11 alr sigsys: nr=56 name=? ret=-38      # openat
      9 alr sigsys: nr=79 name=? ret=-38      # newfstatat
bin/true: error while loading shared libraries: libc.so.6: cannot open shared object file: Error 38
```

## 3. 수정

- **슈퍼바이저**(`src/supervisor/alr_supervisor.c`): `si_errno != 0`인 `SIGSYS`는 게스트가 쌓은 필터의 것으로 보고 그대로 전달한다. zygote 트랩(데이터 0)은 이전처럼 구제한다. 재현 테스트 `SUPERVISOR GUEST FILTER TRAP PASSTHROUGH`(`tests/device/test_supervisor.c` case 11)는 수정 전 `rc=3`(게스트 트랩이 삼켜짐)으로 FAIL, 수정 후 PASS다. 슈퍼바이저 스위트 13/13 PASS.
- **tawcroot**(`third_party/tawcroot`, 로컬 수정): 필터의 `RET_TRAP`에 데이터 `0x7477`을 싣는다.

수정 후 같은 실행:

```
inner_rc=0
alr supervisor: pids=2 sigsys=64 emulated=3 passthrough_signals=62 path_traps=0 syscall_stops=0
     61 alr sigsys: data=0x7477 from a guest filter, passthrough
      3 alr sigsys: nr=99 name=set_robust_list ret=0
etc
```

tawcroot가 다시 실행한 syscall이 zygote에 막히면, 그 트랩은 데이터 0으로 슈퍼바이저에 와서 구제된다(`set_robust_list`).

수정 후 수용 테스트는 `PASS=169 FAIL=0 KNOWN_FAIL=1 SKIP=0`이다.

## 4. 비용: ptrace 아래의 트랩은 100배 이상 비싸다

`bench/microbench/trap_cost.c`(필터 데이터를 `0x7477`로 바꿈)를 Termux에서 직접, 그리고 `alr run` 아래에서 실행했다.

| 실행 | signal round trip | D6 round-trip cost |
|---|---:|---:|
| Termux 직접, CPU 무고정 | +383 ns | +518 ns |
| Termux 직접, CPU 7 | +444 ns | +572 ns |
| `alr run` 아래, CPU 무고정 | +60,305 ns | +58,659 ns |
| `alr run` 아래, CPU 7 | +65,940 ns | +65,761 ns |

추적 중인 프로세스에 오는 신호는 슈퍼바이저가 넘겨주기로 결정하더라도 매번 signal-delivery-stop을 거친다. 한 번의 트랩에 트레이서와의 컨텍스트 전환이 두 번 붙는다.

워크로드(`find /usr/share -type f | wc -l`, 61,865개 파일, 3회):

| 경로 | ms | 네이티브 대비 |
|---|---:|---:|
| Termux의 GNU find 4.11.0, 같은 트리 | 151~177 | 1.0 |
| `engine=alr` (preload) | 151~167 | 약 1.0 |
| `engine=tawcroot` 단독 | 349~368 | 약 2.2 |
| `engine=tawcroot`, alr 슈퍼바이저 아래 | 6,023~6,320 | 약 40 |

- 동적 바이너리는 지금처럼 preload 레인에 두어야 한다. tawcroot 레인은 경로가 많은 워크로드에서 T2 예산(1.2배)을 넘는다.
- tawcroot 레인은 preload가 닿지 않는 바이너리(정적, raw-syscall)에만 쓴다.
- 이 표는 `find` 한 가지 워크로드만 반영한다. T2의 공식 워크로드(`git status`, `npm ci` 등)로는 재지 않았다.

## 5. tawcroot 단독 실행 결과와 필요한 것

alr 없이 Termux에서 `tawcroot -r ROOTFS -b proc:/proc -b dev:/dev -b sys:/sys -- CMD`로 실행했다.

| 대상 | 결과 |
|---|---|
| `/bin/bash -c 'id -u; pwd; ls /etc \| wc -l'` | `0`, `/root`, `129` |
| `readlink /proc/self/exe` | 게스트 경로(`/usr/bin/gnureadlink`) |
| `apt-get --version` | `apt 3.2.0 (arm64)` |
| codex 0.162.1 (정적 musl) `--version`, `doctor` | 동작, git과 rg 발견 |
| uutils(`/usr/lib/cargo/bin/coreutils/*`, GNU 재지정 없음) | 아래 수정 후 `ls`, `cat`, `stat`, `readlink`, `id`, `sha256sum`, `wc` 동작 |

필요했던 것:

- **빌드**: Termux clang 21에서 `-mno-outline-atomics`를 더하면 aarch64로 빌드된다. 빠뜨리면 `__aarch64_cas4_acq` 등이 정의되지 않아 링크에 실패한다. musl이 아니라 bionic 헤더로 빌드하면 된다.
- **바인드**: `/proc`, `/dev`, `/sys`는 `-b`로 직접 줘야 한다. 주지 않으면 `/proc/self/cmdline`이 없다.
- **`PR_GET_AUXV`(로컬 수정)**: uutils는 rustix로 `prctl(PR_GET_AUXV)`를 직접 불러 `AT_EXECFN`에서 자기 이름을 정한다. tawcroot가 이 호출을 커널로 넘기면 tawcroot 자신의 auxv가 돌아와서, uutils가 자기 이름을 `tawcroot`나 `4`(`--exec-child`의 fd 번호)로 본다. 로더가 만든 게스트 auxv를 돌려주도록 고쳤다. **alr에서 uutils가 `unknown program 'ld-linux-aarch64.so'`로 실패하던 것도 같은 원인이다**(커널이 실행한 것은 ld.so다). preload는 raw `prctl`을 가로챌 수 없으므로, alr이 26.04에서 GNU coreutils로 재지정하는 이유가 이것이다.
- **인자 경로**: alr의 execve 래퍼는 정적 자식의 절대 경로 인자를 호스트 경로로 바꾼다(`exec_build`의 `ALR_EXE_ELF_STATIC` 분기). tawcroot에는 게스트 경로가 그대로 가야 하므로, 이 스파이크에서는 상대 경로를 써서 우회했다. 레인을 붙일 자리가 이 분기다.

## 6. 설계에 주는 결론

1. **레인 진입 시 추적을 끊는다.** 정적 또는 raw-syscall 바이너리를 tawcroot로 실행할 때, 그 프로세스는 슈퍼바이저에서 떼어 내야 한다. 예를 들면 `PTRACE_EVENT_EXEC`에서 새 이미지가 레인 바이너리이면 `PTRACE_DETACH`한다. zygote 트랩은 tawcroot의 디스패치가 처리한다. 그 반환값이 alr의 에뮬레이션 표(`set_robust_list` → 0 등)와 같은지는 확인하지 않았다.
2. **슈퍼바이저의 데이터 기반 구별은 그대로 둔다.** 추적을 끊기 전의 짧은 구간과 게스트 자신의 `RET_TRAP` 샌드박스에 필요하다.
3. **레인의 자식이 문제다.** seccomp 필터는 fork와 exec를 거쳐 상속되고 지울 수 없다. codex가 띄운 bash와 git도 tawcroot 레인(약 2.2배)에 남는다. 이를 피하려면 레인 밖의 브로커가 자식을 대신 띄워야 한다. 예를 들면 hostbin 래퍼가 필터가 없는 프로세스에 요청하는 방식이다. 아직 시도하지 않았다.
4. **환경을 정리해야 한다.** 레인으로 넘어갈 때 alr의 `LD_PRELOAD`, `LD_LIBRARY_PATH`, `ALR_*`를 지우지 않으면, 게스트 ld.so가 호스트 경로의 preload를 찾다가 경고를 낸다. Termux의 `HOME`이 넘어가면 게스트가 rootfs 안에 Android 경로 디렉터리를 만든다(이 스파이크에서 실제로 생겼고, 지웠다).

## 정직한 한계

- zygote 필터의 데이터가 0이라는 사실은 Snapdragon, Android 17 한 대에서만 쟀다. 필터는 AOSP bionic이 만들므로 다른 기기도 같을 가능성이 높다. 다만 bionic 소스로 확인하지 않았고, MediaTek 기기에서도 재지 않았다.
- tawcroot 레인의 경로 변환 비용은 `find` 한 가지로만 쟀다.
- tawcroot의 upstream 테스트(`tests/`)는 vendoring하지 않았으므로, 로컬 수정은 기기 실행으로만 확인했다.
