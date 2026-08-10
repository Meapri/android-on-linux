# M10 — `alr install --with git` 완주

- **날짜**: 2026-08-02
- **기기**: MediaTek arm64, `uid=10297 Seccomp=2 u:r:untrusted_app_27:s0` (`ACCEPTANCE CONTEXT: PASS`)
- **게스트**: Ubuntu 24.04.4 base arm64, glibc 2.39, 패치 없음
- **결과**: **MEASURED — 아무것도 없는 상태에서 `alr install --url … --with git` 2분 27초, `git version 2.43.0` 동작**

`alr install --with git` 은 이 프로젝트의 마지막 남은 블로커였다. 원인은 하나가 아니라 **네 개가 직렬로 쌓여 있었고**, 앞의 것을 고쳐야 뒤의 것이 드러났다. 네 개 모두 같은 계열이다: **glibc 내부 호출은 `LD_PRELOAD` 로 가로챌 수 없고, Android 의 `/etc` 는 읽기 전용 `/system/etc` 심링크다.**

## 1. 절대 심링크 타깃 — apt 스테이징 실패

**증상**

```
E: Sub-process /usr/bin/dpkg returned an error code
dpkg-deb: error: cannot stat pathname '/tmp/apt-dpkg-install-XXXXXX/00-perl-modules-….deb'
```

**원인.** 심링크의 *내용*은 커널이 **실제 호스트 루트** 기준으로 푼다. 이 해석은 커널 안에서 일어나므로 어떤 libc 훅보다 아래에 있어 우리가 개입할 수 없다. 따라서 rootfs 안에 저장된 `/var/cache/apt/archives/…` 같은 절대 타깃은 Android 의 `/var` 를 가리키고, 존재하지 않는다. readdir 에는 보이는데 stat 이 실패하는 것이 정확히 이 모양이다.

**수정.** `symlink()`/`symlinkat()` 에서 절대 타깃을 등가의 **상대 경로**로 변환한다 (`src/preload/alr_preload.c` `relativize()`). 상대 타깃은 커널이 링크 자신의 디렉토리 기준으로 풀고, 그 디렉토리는 이미 rootfs 안이다.

- `/etc/os-release` at `/tmp/x` → `../etc/os-release`
- `/etc/os-release` at `/usr/local/bin/x` → `../../../etc/os-release`
- **`/proc`·`/sys`·`/dev` 타깃은 절대 유지** — 이들은 `rw()` 가 재작성하지 않으므로 상대화하면 오히려 깨진다.
- `symlinkat(dfd, 상대경로)` 는 `/proc/self/fd/N` 을 되읽어 게스트 경로를 복원한 뒤 깊이를 계산한다. 복원에 실패하면 **타깃을 건드리지 않는다** — 깊이를 틀리게 잡는 것이 절대 링크보다 나쁘다.

**검증**: 스테이징 에러 소멸. 회귀 테스트 4개 (`PRELOAD SYMLINK RELATIVIZED / DEREFERENCES / DEPTH / SYSDIR ABS`).

> ⚠️ 이 가설은 앞 세션 내내 **미확인** 상태였고, 문서에도 그렇게 적혀 있었다. 이번에 실제로 구현하고 나서야 확정되었다. 구현 코드가 소스에 들어가지 않은 채 "구현했다" 로 기록되어 있던 것이 원인이다 — [m9 문서의 최종 정정](2026-08-02-m9-reproducibility.md) 참조.

## 2. `NETLINK_AUDIT` — `groupadd` 즉시 중단

**증상**

```
Cannot open audit interface - aborting.
fatal: `/sbin/groupadd -g 100 _ssh' returned error code 1. Exiting.
dpkg: error processing package openssh-client (--configure): … exit status 82
```

**원인.** shadow 의 `audit_help_open()` 은 `audit_open()` 이 `EINVAL`/`EPROTONOSUPPORT`/`EAFNOSUPPORT` 로 실패할 때만 "커널에 audit 이 없다" 로 보고 계속 진행한다. Android 는 SELinux 로 `NETLINK_AUDIT` 을 `EACCES`/`EPERM` 로 거부하므로 하드 에러로 읽혀 즉시 종료한다. **에러 문자열 자체가 원인을 확정한다** — 저 메시지는 그 분기에서만 나온다.

**수정.** `socket(AF_NETLINK, *, NETLINK_AUDIT)` 을 `EPROTONOSUPPORT` 로 응답. 스크립트가 이미 가지고 있는 no-audit 경로를 타게 한다.

## 3. `lckpwdf()` — `cannot lock /etc/group`

**증상**

```
groupadd: cannot lock /etc/group; try again later.   (종료 코드 10 = E_GRP_UPDATE)
```

**결정적 관찰.** groupadd 실행 후 `/etc` 에 **파일이 하나도 생기지 않았다** — `group.<pid>` 도, `group.lock` 도. 즉 잠금 파일을 만들기 *전에* 실패한다. 이것이 하드링크/`nlink` 이론(당시 유력했던 가설)을 기각했다.

```
$ LD_DEBUG=bindings groupadd …
binding file /sbin/groupadd to …/libc.so.6: normal symbol `lckpwdf' [GLIBC_2.17]
$ ls -ld /etc                 # 호스트
lrw-r--r-- 1 root root 11 /etc -> /system/etc
$ touch /etc/.alr-probe
touch: cannot touch '/etc/.alr-probe': Read-only file system
```

**원인.** shadow 의 `commonio_lock()` 은 자체 잠금 파일보다 먼저 glibc `lckpwdf()` 를 부른다. glibc 는 그 안에서 리터럴 `/etc/.pwd.lock` 을 **내부** `__open64_nocancel` 로 연다 — 공개 심볼이 아니라 우리가 가로챌 수 없고, 따라서 Android 의 읽기 전용 `/system/etc` 로 가서 실패한다.

**수정.** `lckpwdf`/`ulckpwdf` **자체가 공개 심볼**이므로 우리가 가져온다. `rw()` 를 거쳐 rootfs 안의 `/etc/.pwd.lock` 을 열고 `fcntl(F_SETLK)` 로 잠근다. glibc 는 `alarm(15)`+`SIGALRM` 으로 대기를 제한하지만, preload 가 게스트 몰래 시그널 핸들러를 설치해서는 안 되므로 **같은 상한을 재시도로** 구현했다.

## 4. NSS `files` 백엔드 — 사용자/그룹 이름 해석 부재

**증상**

```
chgrp: invalid group: '_ssh'      # /etc/group 에는 _ssh:x:100: 이 분명히 있음
```

**결정적 관찰**

```
$ head -1 $ROOTFS/etc/group        →  root:x:0:
$ alr run getent group root        →  (빈 출력)
$ alr run getent passwd root       →  (빈 출력)
$ ls -la /system/etc/group         →  89 바이트 (Android AID 테이블)
```

**원인.** glibc 2.34+ 는 `files` NSS 백엔드를 libc 안에 내장하고 있고, 그것이 리터럴 `/etc/passwd`·`/etc/group` 을 `__nss_files_fopen` → 내부 `fopen` 별칭으로 연다. 역시 가로챌 수 없다. 결과적으로 **게스트 안의 모든 이름 조회가 폰의 89바이트 AID 테이블에서 답을 받고 있었다.**

이것은 `openssh-client` 하나의 문제가 아니라 `chown user:group`, `getent`, `id`, 그리고 시스템 계정을 만드는 모든 postinst 에 영향을 주는 **핵심 호환성 공백**이었다. 그동안 드러나지 않은 이유는 fakeroot 아래에서 uid 0 을 사칭하므로 대부분의 도구가 이름 조회 없이 통과했기 때문이다.

**수정.** 공개 API 는 가로챌 수 있으므로 `files` 백엔드를 직접 구현했다 (`alr_preload.c`, `pwd.h`/`grp.h` 절):

| 구분 | 구현 |
|---|---|
| 조회 | `getpwnam` `getpwuid` `getgrnam` `getgrgid` |
| 재진입 | `getpwnam_r` `getpwuid_r` `getgrnam_r` `getgrgid_r` |
| 열거 | `setpwent`/`getpwent`/`endpwent`, `setgrent`/`getgrent`/`endgrent` |
| 보조 | `getgrouplist` |

`files` 는 여기서 동작할 수 있는 **유일한** 소스다 — `nss_systemd`·`nss_ldap` 은 게스트가 띄우지 않는 데몬을 요구한다.

**알려진 한계 (의도된 것)**
- 그룹당 멤버 **64명** 상한. 초과 시 잘라내고 `ALR_LOG` 에 기록한다 — 조용한 절단이 아니다.
- `initgroups()` 는 구현하지 않았다. 보조 그룹 설정은 `setgroups` 가 seccomp 로 막혀 있어 애초에 불가능하다. 필요해지면 fakeroot 의미론에 맞춰 별도로 판단한다.
- `ls -l` 은 호스트 uid(예: `10297`)로 소유된 파일에 대해 숫자를 그대로 보여준다. rootfs 의 `/etc/passwd` 에 그 uid 가 없으니 **정확한 동작**이다.

## 결과

```
$ time alr install --url …ubuntu-base-24.04.4-base-arm64.tar.gz --with git
Setting up git (1:2.43.0-1ubuntu7.3) ...
alr: installed …/alr-v3/ubuntu-24.04
real  2m27.091s

$ alr run /usr/bin/git --version
git version 2.43.0
```

`openssh-client` 도 `ii` (정상 설치) 상태로 완료된다 — 이전에는 `iF` 로 남았다.

## 수용 테스트

```
PASS=52  FAIL=0  KNOWN_FAIL=3  SKIP=0
ALR DEVICE ACCEPTANCE: PASS
```

신규 회귀 테스트 9개 전부 PASS, 기존 43개 회귀 없음. `KNOWN_FAIL` 3개는 이전부터 문서화된 미구현 항목(`/proc/mounts` 호스트 정보 노출, `/proc/self/status` 로더 이름 노출, `/dev/full` 미에뮬레이션)으로 변동 없다.

## 절차 교훈

**한 배포 경로만 고치면 다른 경로가 조용히 옛 바이너리를 쓴다.** 신규 테스트 9개가 처음에 전부 FAIL 났는데, 원인은 코드가 아니라 `scripts/dev-push.sh` 의 `accept` 모드가 `ALR_DISTROS_DIR` 를 무시하고 **기본 rootfs** 를 쓴다는 점이었다. 수동 검증은 `alr-v2` 에서, 수용 테스트는 `alr-distros` 에서 돌고 있었다. 실패를 코드 탓으로 돌리기 전에 **어느 바이너리가 실제로 실행되는지** 먼저 확인해야 한다 — 이 세션에서 같은 계열의 실수가 반복된 지점이다.
