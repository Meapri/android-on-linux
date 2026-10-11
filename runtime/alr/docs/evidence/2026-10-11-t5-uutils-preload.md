# T5 — uutils coreutils가 preload 레인에서 동작한다, 상대 링크 경로의 심링크 수정

- **날짜**: 2026-10-11
- **엔진 라벨**: `engine=alr`
- **기기**: Samsung SM-S937N, Snapdragon 8 Elite SM8750, Android 17 (SDK 37), 커널 6.6.127
- **컨텍스트**: `uid=10057  u:r:untrusted_app_27:s0  Seccomp=2` (Termux sshd를 거친 셸)
- **게스트**: Ubuntu 26.04.1 LTS (glibc 2.43), coreutils는 Ubuntu 기본값인 uutils(GNU 재지정 없음)
- **목적**: [ADR 0005](../../../../docs/adr/0005-termux-first-goals.md) T5의 첫 기준, "26.04의 uutils coreutils가 GNU 재지정 없이 동작한다"

## 결론

- uutils가 자기 이름을 잃은 원인은 커널 auxv의 `AT_EXECFN`이었다. preload가 시작할 때 그 문자열을 게스트 경로로 덮어쓰니, uutils가 GNU 재지정 없이 동작했다.
- 그 결과 수용 테스트는 `PASS=171 FAIL=0 KNOWN_FAIL=1 SKIP=0`([원본](2026-10-11-t5-acceptance-uutils.txt)), 호환성 폭은 `96/96`([원본](2026-10-11-t5-breadth-uutils.txt))이다.
- `alr install`과 `alr adopt`는 더 이상 coreutils를 GNU로 바꾸지 않는다. `alr use-gnu-coreutils`는 원하는 사람이 쓰는 선택 명령으로 남는다.
- 같은 회차에 찾은 버그도 고쳤다. 상대 링크 경로로 만든 절대 심링크(`ln -s /etc/hostname l`)가 Android 쪽을 가리키던 문제다.

## 1. 원인: `AT_EXECFN`, 그리고 왜 예전 조사에서 빠졌나

[M15 §3](2026-08-03-m15-cmdline-2604.md)은 `argv[0]`, `/proc/self/cmdline`, `/proc/self/exe`, `getauxval(AT_EXECFN)`을 모두 배제하고 "raw `svc` 때문에 고칠 수 없다"고 결론 냈다. 그런데 tawcroot 트레이스([T5 스파이크 §5](2026-10-11-t5-coexistence-spike.md))에서 uutils가 이름을 정하기 직전에 부르는 것이 보였다. 바로 raw `prctl(PR_GET_AUXV)`(`0x41555856`)이다. 이 호출은 glibc의 `getauxval`을 거치지 않으므로, 예전 조사의 preload 트레이스에는 잡히지 않았다.

그 auxv의 `AT_EXECFN`은 커널이 실행한 파일, 즉 ld.so의 경로다. glibc ld.so는 명시적으로 실행되면 **자기 사본**의 `AT_EXECFN`을 `argv[0]`으로 바꾼다. 그래서 `getauxval()`은 프로그램 경로를 돌려준다. 하지만 커널이 보관한 원본(`PR_GET_AUXV`와 `/proc/self/auxv`가 돌려주는 값)은 여전히 스택 위의 ld.so 경로 문자열을 가리킨다.

```
getauxval(AT_EXECFN)  -> /data/.../ubuntu-26.04/usr/bin/python3   (ld.so가 바꾼 사본)
PR_GET_AUXV AT_EXECFN -> /data/.../ubuntu-26.04/lib/ld-linux-aarch64.so.1
```

uutils의 inline `svc` 74개는 `ioctl`, `fstat`, `fcntl`, `read`, `lseek` 같은 fd 계열이라 경로를 싣지 않는다. 경로 syscall은 libc를 거치므로 preload가 이미 처리하고 있었다. 그래서 막혀 있던 것은 이름 하나뿐이었다.

## 2. 수정

`src/preload/alr_preload.c`의 `alr_init`: `PR_GET_AUXV`로 커널 auxv를 읽는다. 그 `AT_EXECFN` 문자열이 `ALR_LDSO`와 같고 게스트 경로(`ALR_GUEST_EXE`)가 그 길이 안에 들어가면, 문자열을 그 자리에서 덮어쓴다. 스택은 쓰기 가능하고, 두 경로(`PR_GET_AUXV`, `/proc/self/auxv`)가 모두 이 문자열을 가리키므로 한 번의 쓰기로 충분하다. 추가 syscall은 프로세스당 `prctl` 하나다.

`src/cli/alr.c`: install과 adopt가 uutils 트리를 GNU로 재지정하던 동작과 그 경고를 없앴다.

## 3. 검증

uutils applet 114개를 각각 `--version`으로 실행했다. 113개가 자기 이름을 인식했다. `relpath`는 이미지에 심링크는 있지만 uutils 바이너리에 그 applet이 없다. 이 결과는 Ubuntu 이미지 자체의 상태이며, alr과는 관계없다.

같은 명령 30개를 GNU(`gnu*`)와 uutils로 나란히 실행해 출력을 비교했다(`touch`, `mkdir -p`, `cp`/`mv`/`rm`, `chmod`/`stat`, `du`, `realpath`, `df`, `sort`/`uniq`, `sha256sum`, `install`, `split`, `chown`, `timeout`, `tty`, `shred` 등). 차이는 두 가지였다.

| 항목 | GNU | uutils | 판단 |
|---|---|---|---|
| `ls -l` 권한 열 끝 | `.` (SELinux 컨텍스트) | `+` (ACL로 표시) | uutils가 SELinux xattr를 ACL로 표시한다. 실제 Ubuntu에서는 확인하지 못했다 |
| `stty -a` 속도 | `38400` | `speed 15 baud`, `stty speed`는 "invalid argument" | `speed` 피연산자는 uutils에 없다. 15는 c_cflag의 B38400 코드값이다. preload의 `tcgetattr`는 glibc 2.43과 같은 의미를 따른다(`tcgetattr`은 GLIBC_2.17 한 버전뿐이고, 옛 ABI 변환은 `cf*speed@GLIBC_2.17`이 한다) |

수용 테스트에서 고친 검사 두 개:

- `DOCTOR P11 DYNAMIC CLEAN`의 대상을 `/bin/true`에서 `/bin/bash`로 바꿨다. 26.04에서 `/bin/true`는 svc를 가진 uutils라서, "libc만 쓰는 동적 바이너리"라는 검사의 전제가 깨졌다.
- `ALR PTY SPEED`는 GNU `stty`(`gnustty`)를 쓴다. 이 검사는 preload의 glibc `speed_t` 변환을 확인하는 것이고, uutils `stty`에는 `speed`가 없다.

새로 설치하는 경로도 별도 루트에서 확인했다. `alr install`은 `INSTALL BOOT`(출력 없는 `/bin/true`)까지 PASS였고, `/usr/bin/ls`는 `../lib/cargo/bin/coreutils/ls`로 남았다.

## 4. 같은 회차에 고친 것: 상대 링크 경로의 절대 심링크

`ln -s /etc/hostname l`은 `symlinkat("/etc/hostname", AT_FDCWD, "l")`을 부른다. preload는 절대 타깃을 상대 타깃으로 바꾸는데(`relativize`), 링크 경로가 절대일 때만 그렇게 했다. 그래서 이 호출은 `/etc/hostname`을 그대로 저장했고, 커널은 Android의 `/etc/hostname`을 따라갔다.

```
수정 전:  cd /tmp && ln -sf /etc/os-release alrslr && head -1 alrslr
          head: cannot open 'alrslr' for reading: No such file or directory
수정 후:  PRETTY_NAME="Ubuntu 26.04.1 LTS"
```

링크의 부모 디렉터리를 `O_PATH`로 열고 `/proc/self/fd`로 읽어, 커널이 해석한 게스트 경로로 깊이를 계산하도록 고쳤다(`link_guest_path`). 상대 경로, dirfd, `..`가 모두 같은 방법으로 처리된다. 수용 테스트에 `PRELOAD SYMLINK RELATIVE LINKPATH`와 `PRELOAD SYMLINK DOTDOT LINKPATH`를 추가했다.

## 남은 것

- **Go 바이너리**: `go build`는 여전히 `mkdir /tmp/go-build…: permission denied`로 실패한다. Go 런타임은 경로 syscall까지 raw로 낸다. preload로는 닿지 않으므로 tawcroot 레인이 필요하다.
- 기존에 GNU로 재지정된 rootfs는 그대로 동작한다. Ubuntu 기본 상태로 되돌리는 명령은 만들지 않았다.
