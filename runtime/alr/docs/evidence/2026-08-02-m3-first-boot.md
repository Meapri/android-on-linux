# 2026-08-02 — M3: 스톡 Ubuntu 24.04 첫 부팅

**패치되지 않은 Ubuntu 24.04 arm64 rootfs가 Termux 안에서 부팅한다.**
기기: SM-X236N / Android 16 / `untrusted_app_27` / `Seccomp=2`.

## 결과

```
ALR BOOT /bin/true:              exit=0
ALR BOOT /bin/echo:              "alr"
ALR BOOT bash -c true:           exit=0
ALR GUEST GLIBC VERSION:         2.39      (Ubuntu GLIBC 2.39-0ubuntu8.7)
ALR HARDLINK MEMBER RUNS:        perl5.38.2 -> "42"
ALR SUPERVISOR:  pids=1 sigsys=1 emulated=1 path_traps=0 syscall_stops=0
```

**게스트 glibc는 재빌드도 패치도 하지 않았다.** cdimage의 `ubuntu-base-24.04.4-base-arm64.tar.gz`를 그대로 풀었다. 이것이 [00-product.md §4](../00-product.md)의 유일한 차별점(grun 대비 "스톡 아카이브 호환성")이 성립한다는 첫 증거다.

`sigsys=1`: 프로세스당 SIGSYS 한 번(`set_robust_list`)만 발생하고 슈퍼바이저가 구제한다. `path_traps=0 syscall_stops=0` — PRoot와 갈리는 불변식이 유지된다.

## 설계 전제의 실증 (반증 테스트)

두 ADR의 근거를 **같은 명령을 조건만 바꿔 실행**해 확인했다. 문헌 추론이 아니라 이 기기의 동작이다.

### ADR 0001 — 슈퍼바이저가 load-bearing인가

```
$ env -i <R>/lib/ld-linux-aarch64.so.1 --library-path ... --argv0 true <R>/usr/bin/true
Unknown signal 31
exit=159        # 128 + 31 = SIGSYS
```

슈퍼바이저를 거치지 않으면 **정확히 같은 명령이 SIGSYS로 죽는다.** glibc의 `__tls_init_tp()`가 `set_robust_list`(99)를 호출하는데 Android가 `SECCOMP_RET_TRAP`으로 막고, 그 시점은 어떤 DSO 생성자보다도 이르다. → **순수 `LD_PRELOAD` 설계로는 부팅 불가**가 실증되었다. ([ADR 0001](../adr/0001-signal-only-ptrace-supervisor.md))

### ADR 0002 — 명시적 ld.so 호출이 필요한가

```
$ env -i <R>/usr/bin/true
No such file or directory
exit=127
```

파일은 **존재하는데도** ENOENT다. 커널이 `PT_INTERP`(`/lib/ld-linux-aarch64.so.1`)를 rootfs가 아니라 **실제 호스트 루트** 기준으로 해석하기 때문이다. → **스톡 바이너리를 그냥 execve할 수 없다**가 실증되었다. ([ADR 0002](../adr/0002-explicit-ldso-invocation.md))

### §B7 — Termux env 오염

첫 반증 시도는 `env -i` 없이 돌려서 다른 실패가 났다:

```
true: error while loading shared libraries: libc.so: cannot open shared object file
```

`libc.so.6`이 아니라 **`libc.so`**를 찾는다 — ssh 세션의 Termux `LD_LIBRARY_PATH`가 새어 들어가 게스트 ld.so가 bionic 라이브러리 이름을 집은 것이다. `alr`의 `build_env()`가 `LD_PRELOAD`/`LD_LIBRARY_PATH`/`GLIBC_TUNABLES`를 **상속하지 않고 교체**하는 이유가 이것이다. ([§B7](../01-platform-facts.md))

## 발견 — ubuntu-base에 하드링크 멤버가 있다 (문서 정정)

[05-provisioning-spec.md §2](../05-provisioning-spec.md)는 "하드링크가 복사로 저하될 수 있다"고만 적었으나, 실제로는 **tar가 실패하고 파일이 아예 생기지 않는다**:

```
tar: usr/bin/perl5.38.2: Cannot hard link to 'usr/bin/perl': Permission denied
tar: usr/bin/uncompress: Cannot hard link to 'usr/bin/gunzip': Permission denied
tar: Exiting with failure status
```

`link(2)`가 앱 사설 저장소에서 `EACCES`라는 doctor P6 측정과 정확히 일치한다. 방치하면 **조용히 깨진 rootfs**가 된다 — `perl5.38.2`와 `uncompress`가 없는데 tar 경고를 흘려보내면 나중에야 발견한다.

`fix_hardlinks()`를 추가했다: `tar -tvzf`로 하드링크 멤버를 열거하고, `link()`를 먼저 시도한 뒤 실패하면 복사한다.

```
alr: hardlink members: 0 linked, 2 copied, 0 failed
$ alr run /usr/bin/perl5.38.2 -e 'print 42'   ->  42
```

> 복사는 등가가 아니다: `st_nlink`가 1로 남고 수정이 전파되지 않는다. 갓 추출한 읽기 위주 rootfs에는 충분하지만, **게스트가 만드는 하드링크**의 일반 해법은 preload의 link2symlink 계층이다([ADR 0004](../adr/0004-link2symlink.md)). doctor P6이 `EACCES`를 보고했으므로 M6에서 반드시 켠다.

## 알려진 한계 (M4 이전이라 정상)

**경로 가상화가 아직 없다.** preload가 M4이므로 게스트는 rootfs를 `/`로 보지 못한다. `alr run /bin/bash -c 'ldd --version'`이 Android의 toybox `ldd`를 실행한다 — 게스트 bash의 `PATH` 탐색이 **호스트 파일시스템**을 향하기 때문이다.

`alr`이 스스로 해석한 최초 프로그램만 rootfs를 가리킨다. M4에서 `libalr_preload.so`가 붙으면 해소된다.

## 재현

```bash
ALR_SSH_KEY=<key> ./scripts/dev-push.sh alr
ssh -p 8022 <termux> 'cd ~/alr && ALR_ROOT_DIR=$HOME/alr-distros \
  ./alr install ubuntu-24.04 --url https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.4-base-arm64.tar.gz'
ssh -p 8022 <termux> 'cd ~/alr && ALR_ROOT_DIR=$HOME/alr-distros ALR_LOG=1 ./alr run /bin/true'
```
