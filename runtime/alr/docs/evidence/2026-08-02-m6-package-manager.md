# 2026-08-02 — M6: 패키지 매니저

**스톡 Ubuntu 아카이브에서 git을 설치하고 실행한다.** 기기: SM-X236N / Android 16 / `untrusted_app_27` / `Seccomp=2`.

## 결과

```
apt-get update                35.2 MB fetched, 패키지 목록 15개
apt-get install git           23 newly installed
git --version                 git version 2.43.0
git init / add / commit       PASS
git status                    PASS
git clone --local             PASS
perl -e 'use Cwd'             PASS
온디바이스 acceptance          PASS=36  FAIL=0  KNOWN_FAIL=3
```

## 순서대로 뚫은 블로커

각각은 앞의 것을 고쳐야 비로소 보였다. 진단 순서 자체가 기록할 가치가 있다.

| # | 증상 | 원인 | 수정 |
|---|---|---|---|
| 1 | `Temporary failure resolving` | 기기의 Private DNS(DoT) + VPN이 평문 53 차단. glibc 리졸버는 netd를 모른다 | `alr` 안의 bionic 리졸버 스레드 + `getaddrinfo` 인터포즈 |
| 2 | `mkstemp (2: No such file or directory)` | glibc `mkstemp`은 내부 별칭 `__open` 사용 | `mkstemp`/`mkstemps`/`mkostemp`/`mkostemps`/`mkdtemp` + `*64` |
| 3 | `Couldn't determine free space ... statvfs` | `statvfs` 미구현 | `statvfs`/`statvfs64`/`statfs`/`statfs64` |
| 4 | perl `Cwd.so: cannot open shared object` | 절대경로 `dlopen` 미재작성 | `dlopen` (`$ORIGIN` 토큰은 보존) |
| 5 | `requires superuser privilege` | fakeroot 부재 | preload 내장 신원 스푸핑 |
| 6 | `'sh' not found in PATH` | **중복 env 키** — `getenv`는 첫 번째를 반환 | 상속 `PATH`/`HOME`/`TMPDIR`/`LANG` 제거 |
| 7 | `error setting timestamps ... No such file` | dpkg는 `utimensat`이 아니라 **`utimes`** 사용 | `utimes`/`lutimes`/`utime`/`futimesat` |
| 8 | `ldconfig.real: Can't stat /lib/...` | ldconfig은 **정적 링크**(PT_INTERP 없음) → 후킹 불가. Android `/etc`는 읽기 전용 심링크 | `/sbin/ldconfig`을 no-op으로 교체 |
| 9 | `cannot stat /tmp/apt-dpkg-install-*/....deb` | coreutils `ln`은 `linkat`을 **실제 dirfd**와 함께 사용, 폴백이 `AT_FDCWD`만 처리 | `openat` 기반 복사로 dirfd 존중 |
| 10 | `fatal: hardlink different from source` | git이 링크 후 **inode 동일성 검증**. 복사본은 새 inode | link-identity 테이블 + `stat`/`lstat`/`fstatat`(**및 `*64`**) 패치 |

### 8번이 정당한 대체인 이유

`ldconfig`의 유일한 산출물은 `/etc/ld.so.cache`인데, 이 런타임은 로더를 항상 `--library-path` + `--inhibit-cache`로 호출한다([ADR 0002](../adr/0002-explicit-ldso-invocation.md)). **소비자가 없는 작업을 제거하는 것**이지 오류를 숨기는 것이 아니다.

### 10번이 전체 shadow 방식보다 나은 이유

[ADR 0004](../adr/0004-link2symlink.md)의 shadow-file 방식은 `lstat`/`readlink`/`readdir`/`scandir`/`unlink`/`rename`을 일관되게 인터포즈해 `st_nlink`를 유지해야 한다. 그런데 **호출자가 실제로 검사하는 것은 inode 동일성 하나**다. 복사 폴백이 만든 목적지가 원본의 `st_dev`/`st_ino`를 보고하도록 테이블을 두면 그 속성만 정확히 만족시킨다.

한계(명시): 프로세스 단위이고 512개로 제한된다. git은 링크와 검증을 한 프로세스에서 하므로 문제되지 않지만, **프로세스를 넘는 검사는 진실을 본다.**

## 반복된 방법론 실수 — 잘못된 대리 측정

같은 실수를 **세 번** 했다. 전부 "다른 프로그램으로 대신 쟀다"는 형태다.

| 검증하려던 것 | 잘못 쓴 도구 | 왜 틀렸나 |
|---|---|---|
| `mkstemp(3)` 커버리지 | `mktemp` 명령 | coreutils는 gnulib 구현 → 공개 `open` 사용 |
| `utimes` 커버리지 | `touch -d` | coreutils는 `utimensat` 사용 |
| 게스트 `PATH` | `bash -c 'echo $PATH'` | bash는 자체 테이블, 나중 할당이 이김. `getenv`는 첫 번째 |
| dpkg 언팩 | `dpkg-deb -x` | dpkg `--unpack`은 자체 tar 리더로 다른 코드 경로 |

**규칙**: 인터포즈 커버리지는 **실제 소비자와 같은 libc 진입점**으로 확인한다. 편한 CLI 도구로 대신 재지 않는다.

## 남은 KNOWN_FAIL

| 항목 | 영향 |
|---|---|
| `/proc/mounts` 가상화 | 게스트 `mount`가 호스트 마운트 테이블 노출 (정보 누출) |
| `/proc/self/status` `Name:` | 로더 이름이 보임 |
| `/dev/full` | 미에뮬레이션 |
| `gethostbyname` 계열 | `getent hosts`/`dig`/`nslookup` 은 아직 실패 (`getaddrinfo` 는 해결) |
| `posix_spawn`/`system`/`popen` | 이들을 쓰는 자식이 후킹 안 됨 |

## 재현

```bash
ALR_SSH_KEY=<key> ./scripts/dev-push.sh preload
ALR_SSH_KEY=<key> ./scripts/dev-push.sh alr
ALR_SSH_KEY=<key> ./scripts/dev-push.sh accept
```
