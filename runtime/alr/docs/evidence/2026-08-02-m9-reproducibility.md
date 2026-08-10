# 2026-08-02 — 재현성 작업 (진행 중)

`alr install --with git,node,codex` 한 줄로 오늘의 상태를 재현하는 것이 목표. **아직 완료되지 않았다.**

## 완료

| 항목 | 내용 |
|---|---|
| `alr install --with` | `git` / `node` / `codex` 파싱 및 설치 파이프라인 |
| **preload 자동 설치** | `install_preload()` — alr 바이너리 옆 → `$PREFIX/share/alr/` 순으로 찾아 `<R>/usr/lib/alr/` 에 넣는다 |
| 상대 심링크 헬퍼 | `guest_symlink()` — 깊이를 계산한다 |
| apt 하위 디렉토리 | `var/cache/apt/archives{,/partial}`, `var/lib/apt/lists{,/partial}`, `var/log/apt`, `usr/local/bin`, `opt` |
| Codex 샌드박스 설정 | `<R>/root/.codex/config.toml` + 보안 고지 출력 |
| `renameat2` 래퍼 | 아래 참조 |

깨끗한 설치에서 **node v24.18.1 / npm 11.16.0 / codex 0.146.0 / `env node`** 가 동작한다. `git` 만 미완.

## 발견 — preload 미설치가 재현성 구멍의 본체였다 ⚠️

`alr install` 이 `libalr_preload.so` 를 rootfs 에 넣지 않고 있었다. 개발 중에는 `dev-push.sh preload` 로 수동 배포해 왔기 때문에 눈치채지 못했다.

**증상이 특히 헷갈린다**: `alr run /path/to/node` 는 alr 이 경로를 직접 해석하므로 **동작한다.** 게스트가 *스스로* 찾는 것만 조용히 Android 파일시스템으로 간다 — apt 의 `/etc/apt/apt.conf.d`, `env` 의 PATH 탐색. 깨진 rootfs 처럼 보인다.

이제 못 찾으면 **경고를 낸다**: "the guest will run WITHOUT path virtualization".

## 발견 — `renameat2` 누락 (`utimes` 와 같은 유형)

`ca-certificates` postinst 가 실패했다:

```
mv: cannot move '/etc/ssl/certs/ca-certificates.crt.new' to 'ca-certificates.crt': No such file or directory
```

coreutils `mv` 는 `renameat()` 이 아니라 **`renameat2()`** 를 쓴다. 스펙 §6.6 목록에는 있었으나 구현하지 않았고, 절대 경로 소스가 Android 루트로 갔다.

**한 개의 미구현 rename 변종이 dpkg 를 half-configured 상태로 남기고, 그것이 이후 모든 apt 트랜잭션을 막았다.** 그래서 증상이 staging 디렉토리 오류라는 무관해 보이는 형태로 나타났다. 수정 후 ca-certificates 가 `done.` 으로 완료된다.

이것으로 "잘못된 대리 측정" 목록에 네 번째가 추가된다 ([M6 증거](2026-08-02-m6-package-manager.md)):

| 확인하려던 것 | 잘못 쓴 도구 | 왜 틀렸나 |
|---|---|---|
| `rename` 커버리지 | `mv` 가 되는지 눈으로 | 되는 케이스는 `renameat`, 안 되는 케이스는 `renameat2` |

## 미완 — apt 스테이징 (수정본으로도 재현)

`renameat2` 수정 후 **완전히 새 rootfs 에 한 번에** 설치해도 동일하다:

```
alr install ubuntu-24.04 --with git,node,codex
  -> node v24.18.1 / npm 11.16.0 / codex 0.146.0  ✅
  -> git                                          ❌ git install failed
```

preload 는 정상 설치되고(`preload installed from ...`) 경로 가상화도 동작한다(node/npm/`env node` 모두 OK). **apt 의 dpkg 스테이징만 실패한다.**

### 원인 확정 — apt 가 절대 심링크로 스테이징한다

`Debug::pkgDPkgPM=1` 로 apt 가 dpkg 에 넘기는 실제 인자를 확인했다: **디렉토리 하나뿐**이다.

```
/tmp/apt-dpkg-install-pDVQtm
```

즉 dpkg 는 `--recursive` 로 그 디렉토리를 **스스로 스캔**한다. 그런데 dpkg 는 `00-perl-modules-5.38_..._all.deb` 라는 **이름을 알면서 `stat` 에 실패**한다:

```
dpkg: error: cannot stat pathname '/tmp/apt-dpkg-install-pDVQtm/00-perl-modules-...deb'
```

**readdir 에는 보이는데 stat 이 실패한다 = 깨진 심링크다.** apt 가 `.deb` 를 스테이징 디렉토리에 심링크하고, 그 타깃이 절대 게스트 경로(`/var/cache/apt/archives/...`)이므로 커널이 **호스트 루트** 기준으로 풀어 존재하지 않는 곳을 가리킨다.

`link->copy` 로그가 한 줄도 안 나온 것과 정확히 일치한다 — apt 는 `link()` 가 아니라 `symlink()` 를 쓴다.

### ✅ 정정 — 래퍼는 정상 바인딩된다. 버그는 `relativize()` 안에 있다

`LD_DEBUG=bindings` 가 결정적이다:

```
binding file ln [0] to .../libalr_preload.so [0]: normal symbol `symlinkat' [GLIBC_2.17]
binding file .../libalr_preload.so [0] to .../libc.so.6 [0]: normal symbol `symlinkat'
```

**`ln` 은 우리 래퍼에 바인딩되고, 우리 래퍼는 libc 로 체인한다.** 버전 없는 정의가 `@GLIBC_2.17` 참조를 정상적으로 가로챈다 — 앞 절의 "버전 불일치" 추정은 **틀렸다.**

`ln` 은 raw syscall 도 쓰지 않는다 (`objdump -d | grep -c svc` == 0).

**따라서 남은 원인은 `relativize()` 내부다.** 래퍼는 호출되는데 타깃이 절대 그대로 남는다:

```
ln -sf /etc/os-release /tmp/rp4  ->  rp4 -> /etc/os-release   (상대화 안 됨)
```

`lg()` 출력이 안 보인 것을 "미경유" 로 오독했다. 로그가 없는 것은 부재의 증거가 아니었다 — 이 세션에서 반복된 실수의 또 다른 형태다.

다음 세션은 `relativize()` **진입 직후**에 무조건 로그를 넣고(early-return 앞), 어느 조건에서 빠지는지 본다. 로직을 손으로 검토하면 `target="/etc/os-release"`, `linkpath="/tmp/rp4"` 에서 depth=1 → `../etc/os-release` 가 나와야 맞다. 실제로 그렇지 않으므로:
- `linkpath` 가 예상과 다른 값일 가능성 (ln 이 dirfd + 상대 경로를 쓰는데 `dfd != AT_FDCWD` 분기의 `/proc/self/fd` 되읽기가 실패)
- 또는 `lg()` 가 이 지점에서만 출력되지 않는 별도 이유

`symlinkat` 의 `dfd` 값과 `linkpath` 원본을 찍는 것이 첫 번째다.

### ✅✅ 최종 정정 — `relativize()` 는 **소스에 존재하지 않았다**

위 지시대로 진입 직후에 무조건 로그를 넣고 빌드했더니, 바이너리에 그 문자열이 **0개**였다. 로컬 빌드 해시도 변하지 않았다. 소스를 직접 확인한 결과:

```
$ grep -c 'relativize' src/preload/alr_preload.c
0
$ sed -n '620,623p' src/preload/alr_preload.c
int symlink(const char *target, const char *linkpath)
{ P(linkpath); NEED(symlink); return real_symlink(target, _p); }
int symlinkat(const char *target, int dfd, const char *linkpath)
{ P(linkpath); NEED(symlinkat); return real_symlinkat(target, dfd, _p); }
```

**원본 그대로였다.** `relativize()` 도, `dfd` 해석도, `lg()` 도 파일에 들어간 적이 없다. 앞 세션에서 python heredoc 치환을 썼는데 패턴이 매치되지 않아 조용히 no-op 했고, 나는 그것을 확인하지 않은 채 "구현했다" 로 기록한 뒤 **존재하지 않는 함수를 디버깅했다.**

그래서 "래퍼는 불리는데 relativize 가 동작하지 않는다" 는 관찰은 전부 정확했다 — 래퍼는 정말 불렸고, 상대화 코드가 없었으니 절대 타깃이 그대로 남았다. 잘못된 것은 관찰이 아니라 **코드가 배포되었다는 전제**였다.

**규칙 (이 세션에서 세 번째로 같은 계열의 실수):** 편집이 적용되었음을 *바이너리에서* 확인하기 전에는 그 위에서 디버깅하지 않는다. 구체적으로:
1. 치환 후 `grep -c` 로 소스 확인 — 매치 실패 시 에러를 내는 편집 도구를 쓴다.
2. 빌드 후 `strings <so> | grep -c '<새 로그 문자열>'` 로 바이너리 확인.
3. 배포 후 **원격** 바이너리에 같은 검사.

세 지점 모두에서 이번에 실제로 걸렸다 — 로컬 빌드에도, 배포본에도 문자열이 0개였다.

### (오독) `ln` 이 우리 심링크 래퍼를 전혀 거치지 않는다

`symlink()`/`symlinkat()` 에 `lg()` 를 넣고 관찰했다. **로깅 자체는 정상 동작한다** (생성자 라인이 나온다):

```
$ ALR_LOG=2 ALR_LOG_FD=2 ./alr run /bin/true
alr preload: root=/data/.../alr-v2/ubuntu-24.04 len=52       ← 로깅 OK

$ ALR_LOG=2 ALR_LOG_FD=2 ./alr run /bin/ln -sf /etc/os-release /tmp/rp3
alr preload: root=/data/.../alr-v2/ubuntu-24.04 len=52
(symlink 로그 없음)                                            ← 래퍼 미경유
```

apt 를 같은 조건으로 돌려도 심링크 로그가 없다.

**즉 coreutils `ln` 은 공개 `symlink`/`symlinkat` 심볼을 통하지 않는다.** 그런데도 심링크는 만들어지고(절대 타깃으로), 결과적으로 깨진다.

이것이 사실이라면 **인터포즈 커버리지에 우리가 계산하지 못한 구멍의 한 부류**가 있다는 뜻이고, 심링크 하나의 문제가 아니다. `mkstemp`(gnulib 이 공개 `open` 사용)과 반대 방향의 사례 — 여기서는 gnulib 이 공개 심볼을 **우회**하는 쪽이다.

#### 확인된 사실 — 심볼 버전 불일치가 유력하다

```
preload 가 export:   symlinkat        GLOBAL DEFAULT  (버전 없음)
ln 이 import:        symlinkat@GLIBC_2.17            (버전 있음)
```

우리 `.so` 는 `symlink`/`symlinkat` 을 **버전 없이** export 하는데, `ln` 은 **`@GLIBC_2.17` 버전 참조**로 요청한다.

일반적으로 버전 없는 정의는 버전 참조에 대해 와일드카드로 동작하므로 LD_PRELOAD 인터포즈가 성립한다 — 그리고 실제로 `open`/`stat`/`readlink`/`execve`/`mkstemp`/`utimes`/`renameat2` 는 모두 같은 조건인데 **정상 동작한다.** 따라서 버전만으로는 설명이 안 되고, **왜 이 심볼만 다른지**가 규명 대상이다.

가능성:
- `ln` 이 `symlinkat` 을 호출하지 않고 다른 경로를 쓴다 (gnulib 대체 구현, 또는 raw syscall)
- 우리 래퍼가 불리지만 `lg()` 이전에 무언가로 빠진다 (검증됨: `lg()` 는 함수 끝 직전에 있고 ctor 로그는 정상 출력되므로 `g_log` 는 2다)

**다음 세션의 첫 질문은 "왜 안 잡히는가" 다.** 확인 순서:

1. `nm -D <preload> | grep symlink` — 정말 GLOBAL DEFAULT 로 export 되는가
2. `LD_DEBUG=bindings ./alr run /bin/ln ... 2>&1 | grep symlink` — `ln` 의 `symlinkat` 이 어디에 바인딩되는가 (우리 .so 인가 libc 인가)
3. `ln` 이 `syscall(SYS_symlinkat, ...)` 을 직접 쓰는지 — 그렇다면 §10 의 `syscall()` 인터포즈 마스크에 nr 36 이 이미 있으므로 **거기서 잡혀야 한다.** 잡히지 않는다면 `syscall()` 래퍼 자체가 이 경로에서 미경유라는 더 큰 문제다

> ⚠️ **3번이 특히 중요하다.** `syscall()` 인터포즈는 Node 의 libuv 대응의 근거이기도 하다. 그것이 `ln` 에서 동작하지 않는다면 **libuv 대응도 실제로는 다른 이유로 통과한 것**일 수 있고, `ALR NODE FS STAT: PASS` 의 해석을 다시 봐야 한다.

### (이전 시도) 상대화가 발동하지 않는다

`relativize()` 를 `symlink()`/`symlinkat()` 에 구현했고(dirfd 를 `/proc/self/fd/N` 으로 되읽어 상대 linkpath 도 처리), 배포본 해시가 로컬 빌드와 **일치함을 확인**했다. 그런데도:

```
ln -sf /etc/os-release /tmp/relprobe
  readlink -> /etc/os-release   (상대화 안 됨)
  test -e   -> BROKEN
```

**즉 코드가 배포되었는데 상대화 분기를 타지 않는다.** 다음 세션의 첫 작업은 원인 규명이며, 가장 빠른 방법은 `relativize()` 진입부에 `lg()` 한 줄을 넣어 어느 early-return 으로 빠지는지 보는 것이다:

```c
lg("alr relativize: target=%s link=%s\n", target ? target : "(null)",
   linkpath ? linkpath : "(null)");
```

확인할 것: (a) 함수가 불리기는 하는가, (b) `linkpath` 가 절대인가 상대인가, (c) `dfd` 값이 무엇인가.

### ⚠️ 가설 자체를 재검증할 것

위 실패와 **apt 실패는 별개일 수 있다.** "apt 가 symlink 로 스테이징한다"는 결론은 다음 정황 증거에서 나온 추론이다:
- dpkg 가 파일 이름은 알면서 `stat` 에 실패 (readdir 에는 보이는데 stat 불가)
- `link->copy` 로그가 한 줄도 없음

**직접 확인하지 못했다.** 스테이징 디렉토리를 실패 순간에 포착하려 두 번 시도했으나 apt 가 즉시 정리해 실패했다. 다음 세션에서는 `strace` 대신 **preload 에 `symlink`/`symlinkat` 호출 로그를 넣고 apt 를 돌려** 실제로 심링크를 만드는지부터 확정할 것. 가설이 틀렸다면 위 수정 방향 전체가 무의미하다.

### 수정 방향 (가설이 맞을 경우) — 절대 심링크 타깃을 상대로 변환

우리 `symlink`/`symlinkat` 래퍼는 **의도적으로 target 을 재작성하지 않는다.** 심링크 내용은 게스트 네임스페이스 문자열이어야 하고, 호스트 접두사를 붙이면 rootfs 에 호스트 경로가 박히기 때문이다. 그 규칙 자체는 옳다.

그러나 **절대 게스트 타깃은 커널이 호스트 기준으로 풀기 때문에 애초에 동작할 수 없다.** 해법은 접두사를 붙이는 것이 아니라 **상대화**다:

```
symlink("/var/cache/apt/archives/foo.deb", "/tmp/apt-dpkg-install-X/00-foo.deb")
  -> symlink("../../var/cache/apt/archives/foo.deb", "<R>/tmp/apt-dpkg-install-X/00-foo.deb")
```

상대 심링크는 커널 해석과 게스트 관점이 **둘 다 옳다** — 같은 게스트 위치를 가리키고, rootfs 를 옮겨도 유지된다. M7 에서 node 심링크를 손으로 상대화해 고친 것과 같은 해법을 일반화한 것이다.

**규칙 변경이므로 ADR 이 필요하다.** 부작용: `readlink()` 가 원래의 절대 형태가 아니라 상대 형태를 돌려준다. 대부분 무해하지만 심링크 문자열을 비교하는 프로그램에는 보인다.

구현 위치: `src/preload/alr_preload.c` 의 `symlink()` / `symlinkat()`. `target[0] == '/'` 이고 sysdir 이 아닐 때만 상대화하고, 깊이는 `guest_symlink()`(`src/cli/alr.c`)와 같은 방식으로 `linkpath` 의 디렉토리 깊이에서 계산한다.

### (이전) 유력 가설 — apt 가 절대 심링크를 만든다

실패 파일명이 `00-perl-modules-5.38_..._all.deb` 로, apt 가 순서 접두사를 붙여 스테이징 디렉토리에 놓는 이름이다. 그런데:

- `ALR_LOG=2` 에 `link->copy` / `linkat->copy` 가 **한 줄도 안 나온다** → apt 는 `link()` 를 쓰지 않는다
- `mkdtemp` 과 수동 `cp` 는 양쪽 rootfs 에서 정상
- 실패 후 스테이징 디렉토리에 잔해가 없다(apt 가 정리)

**apt 가 `symlink()` 로 스테이징한다면** 타깃이 절대 게스트 경로(`/var/cache/apt/archives/foo.deb`)가 되고, 커널은 심링크 타깃을 **호스트 루트** 기준으로 해석하므로 깨진 링크가 된다 → dpkg 의 `stat` 이 ENOENT. 이것은 [M7 에서 겪은 node 심링크 버그](2026-08-02-m7-m8-workloads-perf.md)와 **정확히 같은 부류**다.

> 우리 `symlink`/`symlinkat` 래퍼는 **의도적으로 target 을 재작성하지 않는다** — 심링크 내용은 게스트 네임스페이스 문자열이어야 하기 때문이다. 그 규칙 자체는 옳지만, 그 결과 rootfs 안의 절대 심링크는 게스트가 *따라갈 때* 커널이 호스트 기준으로 푼다. **이 구조적 한계를 어떻게 다룰지가 미해결 설계 문제다.**

### 다음 세션 첫 명령

```bash
# 1) apt 가 정말 symlink 를 쓰는지 확인 (가장 빠른 판별)
ALR_ROOT_DIR=$HOME/alr-v2 ALR_FAKEROOT=1 ./alr run /bin/bash -c \
  'apt-get install -y --no-install-recommends git & sleep 4; ls -la /tmp/apt-dpkg-install-*/ | head -5'
#    -> 'l' 로 시작하는 항목이 보이면 가설 확정

# 2) 확정 시 대안
#    a. APT::Keep-Downloaded-Packages / Dir::Cache::archives 를 /tmp 안으로 옮겨 상대화
#    b. symlink 래퍼가 "rootfs 내부를 가리키는 절대 target" 을 상대 경로로 변환 (규칙 변경 — ADR 필요)
```

## 이전 진단 (수정 전 rootfs 관측)

```
dpkg: error: cannot stat pathname '/tmp/apt-dpkg-install-XXXXXX/00-perl-modules-...deb'
```

**기존 rootfs(`alr-distros`)에서는 `apt-get install less` 가 완전히 성공한다.** 깨끗한 rootfs 에서만 실패하므로 코드가 아니라 **rootfs 상태 차이**다.

관측된 차이:

| | `alr-distros` (수동 보정됨) | `alr-fresh` (`alr install`) |
|---|---|---|
| rootfs root | 755 | **700** |
| `/usr` | **700** | 755 |
| `/tmp` | **1777** | 755 |
| `ln` 폴백 | 동작 (24개) | 동작 (29개) |

`ln` 은 양쪽 다 되므로 link 폴백 문제가 아니다. 다음에 볼 것:

1. **rootfs root 가 700** — `alr install` 의 `rename()` 이 `part` 의 모드를 그대로 가져온다. 게스트가 `/` 를 traverse 할 때 영향이 있는지.
2. apt 가 스테이징에 `link` 가 아니라 다른 수단(`copy_file`, `rename`)을 쓰는지 — `ALR_LOG=2` 의 `linkat->copy` 줄이 나오는지로 판별된다.
3. 첫 트랜잭션이 실패한 rootfs 는 dpkg 상태가 깨져 이후가 전부 연쇄 실패한다. **깨끗한 rootfs 에서 한 번에 성공하는지**로만 판정해야 한다.
