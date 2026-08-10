# 05 — 프로비저닝 스펙

`alr install <distro>` 가 하는 일. 모든 사실은 [01-platform-facts.md §E](01-platform-facts.md)에서 검증되었다.

## 1. 다운로드

### 1.1 Ubuntu (cdimage 경로 — 기본)

**`ubuntu-base-24.04-base-arm64.tar.gz`는 존재하지 않는다 (404).** 파일명은 포인트 릴리스로 붙고 `latest` 심링크가 없다.

```
1. GET https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/SHA256SUMS
2. 정규식 ^([0-9a-f]{64})\s+\*?(ubuntu-base-24\.04\.(\d+)-base-arm64\.tar\.gz)$
   → 세 번째 그룹이 가장 큰 줄 선택   (파일명 앞의 '*' 제거 주의)
3. 같은 디렉토리에서 그 파일명을 GET
4. 1의 해시로 검증
```

폴백 핀 (오프라인/에어갭): `ubuntu-base-24.04.4-base-arm64.tar.gz`, ~29.9 MB.

### 1.2 OCI 경로 (Debian 기본, Ubuntu 대체)

proot-distro는 v5.0.2(2026-05-17)부터 Python이고 OCI 이미지를 쓴다. tarball URL을 가진 셸 플러그인은 더 이상 없다.

익명 Docker Hub 풀 3단계:
```
1. GET https://auth.docker.io/token?service=registry.docker.io&scope=repository:library/<img>:pull
2. GET https://registry-1.docker.io/v2/library/<img>/manifests/<tag>
   Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json
   → manifests[] 중 platform.architecture=="arm64" && platform.os=="linux" 선택
   → 그 digest로 다시 manifests GET → layers[] 획득
3. GET https://registry-1.docker.io/v2/library/<img>/blobs/<layer digest>
```

**같은 코드가 Ubuntu에도 동작하므로 다운로더 하나로 두 배포판을 커버한다.** cdimage 경로는 Ubuntu 전용 최적화다.

예산: Ubuntu ~30 MB, Debian ~50 MB.

## 2. 안전 추출

> ⚠️ **`src/cli/alr_untar.c` 는 만들지 않는다 — [ADR 0009](adr/0009-no-in-process-untar.md).** 아래 규칙은 그대로 요구사항이고, 대부분은 GNU tar 의 플래그와 비루트 실행이 이미 만족한다. **채우지 못하던 하나** — 추출 루트를 벗어나는 링크 타깃 — 은 2026-08-04 에 추출 후 검사로 닫혔다(`reason=extract-traversal-reject`).

규칙:

| 규칙 | 이유 |
|---|---|
| 절대경로 멤버 거부 | traversal. GNU tar 의 기본 동작(`-P` 없이는 선행 `/` 를 제거) |
| `..` 컴포넌트 거부 | traversal. GNU tar 의 기본 동작(`..` 를 포함한 멤버를 거부) |
| 심링크/하드링크 타깃이 추출 루트를 벗어나면 거부 | traversal. **GNU tar 는 이걸 안 본다** — `x -> ../../../etc/passwd` 를 적힌 그대로 만든다. `relativize_tree()` 가 추출 후 어휘적으로 검사하고(`depth` 만큼 내려와 있으니 `..` 하나가 한 칸 올라간다, 음수가 되면 탈출) 해당 심링크를 지운 뒤 설치 전체를 실패시킨다. 하드링크 멤버는 `fix_hardlinks()` 가 이름·타깃 양쪽을 검사한다 — 하드링크는 만들고 나면 진짜와 구별되지 않으므로 |
| blk/chr/fifo/socket 멤버 **스킵** | 비루트에서 `mknod` 불가. (스톡 이미지엔 없지만 방어) |
| `chown`/`lchown` **절대 호출 안 함** | 비루트. 모든 파일이 Termux UID 소유가 된다 |
| setuid/setgid 비트 **마스킹** (`& ~07000`) | `/data`는 nosuid라 무의미. 12개 해당 바이너리는 필요도 원하지도 않는 것들 |
| xattr **스킵** | 스톡 이미지에 없음 |
| **하드링크 멤버는 tar가 실패해 파일이 아예 안 생긴다** — 추출 후 `fix_hardlinks()`로 복구 필수 | [§B6](01-platform-facts.md), [ADR 0004](adr/0004-link2symlink.md), [실측](evidence/2026-08-02-m3-first-boot.md) |
| 임베디드 NUL이 있는 이름 거부 | |
| 심링크 생성 실패는 치명적이 아님 (로깅) | |

추출 후 직접 생성: `/dev`, `/proc`, `/sys`, `/run`, `/tmp`(0777).

**원자성**: `<R>.part`에 추출 후 `rename`. 중단된 설치가 반쯤 된 rootfs를 남기지 않게.

## 3. 추출 후 수리 — 매우 짧다

### 3.1 반드시 써야 하는 것

둘 다 스톡 이미지에서 **0바이트**로 오며, 나중 이미지가 심링크로 줄 수 있으니 **먼저 `unlink`** 한다.

`<R>/etc/resolv.conf`:
```
nameserver 8.8.8.8
nameserver 8.8.4.4
```

`<R>/etc/hosts`:
```
127.0.0.1	localhost localhost.localdomain
::1	localhost localhost.localdomain ip6-localhost ip6-loopback
```

### 3.2 써야 하는 것

`<R>/etc/apt/apt.conf.d/99-alr-no-sandbox`:
```
APT::Sandbox::User "root";
```
> **첫 `apt update` 이전에 반드시.** 안 하면 apt가 `setgroups()`에서 즉사한다 — setuid 계열이 Android seccomp에 차단되어 있다 ([§A6](01-platform-facts.md)). apt 자체의 자동 저하는 접근성 검사에서만 발동하는데, Android에서는 `setgroups()`가 먼저 치명적이다. (슈퍼바이저가 setgroups를 0으로 에뮬레이션하므로 실제로는 살아남지만, 이 설정이 없으면 apt가 존재하지 않는 uid로 권한을 낮추려 해서 파일 접근이 깨진다. 둘 다 필요하다.)

`<R>/etc/dpkg/dpkg.cfg.d/99-alr`:
```
force-unsafe-io
```
> fsync 생략. Android 플래시에서 큰 속도 향상이자 안정성 향상.

`<R>/etc/passwd`, `<R>/etc/group`에 Termux UID/GID 줄 **추가** (덮어쓰기 아님):
```
/etc/passwd:  alr:x:<uid>:<gid>:alr:/root:/bin/bash
/etc/group:   alr:x:<gid>:
```
> 두 파일의 형식이 다르다. group 은 `name:passwd:gid:members` 4필드다 — 이 문서는 2026-08-03 까지 양쪽에 passwd 형식 한 줄만 적어 뒀었고, 그대로 구현했으면 게스트의 모든 `getgrgid()` 앞에 망가진 레코드를 놓을 뻔했다.

게스트의 `ls -l`, `getpwuid()`가 해석되게. **구현 전 실측** (2026-08-03):
```
$ alr run ls -ld /root
drwx------ 6 10297 10297 3452 Aug  3 10:25 /root
$ alr run --no-fakeroot whoami
whoami: cannot find name for user ID 10297
```
rootfs 안의 모든 파일이 디스크상으로는 Termux uid 소유인데 이미지의 어떤 항목도 그 uid 를 설명하지 않는다. **fakeroot 여부와 무관하다** — fakeroot 는 프로세스 자격증명(`getuid`)을 속이지 지 `st_uid` 를 바꾸지 않는다.

**덮어쓰기가 아니라 추가여야 하는 이유**: 이미지의 root/daemon/nobody 항목과 apt 가 패키지 설치 중 만드는 모든 uid 가 이 파일에 산다.

**있으면 넘어가기가 아니라 매번 다시 쓰는 이유**: uid 는 불변이 아니다. Termux 백업을 새 설치에 복원하면 데이터 디렉토리는 그대로인데 uid 는 새로 배정된다. 그래서 `alr install` 뿐 아니라 `alr update-components` 도 이걸 다시 쓴다 — 자기 줄만 지우고 다시 붙이므로 멱등이다.

### 3.3 절대 건드리지 말 것

- ❌ `<R>/etc/apt/sources.list.d/ubuntu.sources` — arm64 tarball이 이미 `ports.ubuntu.com/ubuntu-ports`와 noble/updates/backports/security, 올바른 `Signed-By` 키링을 갖고 온다. **덮어쓰면 오히려 망가진다.** 지리적 미러를 원할 때만 손댄다.
- ❌ `<R>/etc/passwd`, `<R>/etc/group` 본문 — 완비되어 있고 `_apt`(uid 42), `shadow`(gid 42)가 이미 있다.
- ❌ `<R>/etc/dpkg/dpkg.cfg.d/excludes` — 이미 있다.
- ❌ `<R>/etc/nsswitch.conf`, `<R>/etc/apt/sources.list`

### 3.4 alr 컴포넌트 배치

```
<R>/usr/lib/alr/libalr_preload.so
<R>/usr/lib/alr/libalr_preload.manifest.json   # 어느 빌드인지 기록
<R>/usr/lib/alr/manifest.json      {zig_version, target, source_sha256, output_sha256}
<R>/.alr/l2s/                      link2symlink 그림자 (활성화된 경우)
```

**rootfs 빌드타임 린트**: 모든 `.so`의 `PT_GNU_STACK`이 실행 불가인지 확인. RWX `PT_GNU_STACK`을 가진 `.so`는 `PROCESS__EXECSTACK` 미허용으로 **로드 실패**한다 ([§B3](01-platform-facts.md)).

## 4. 첫 부팅 검증

> ✅ **구현됨 — 2026-08-04.** `alr install` 은 `rename` **전에** 필수 파일을 확인하고(없으면 `reason=rootfs-incomplete`), `rename` **후에** 아래 9줄을 출력한다. 확인 대상: `lib/ld-linux-aarch64.so.1`, `bin/sh`, `usr/bin/env`, `etc/os-release`.
>
> **왜 tar 의 종료 코드로는 안 되는가**: 정상 추출도 0이 아니다 — 하드링크 멤버가 Android SELinux 정책에 막히는 것이 예상된 동작이라 코드가 예전부터 무시해 왔다. **왜 목록 대조로도 안 되는가**: 잘린 아카이브는 **목록도 짧아서** 목록과 디스크가 완벽히 일치한다. 갈라지는 지점은 필수 파일의 존재 여부뿐이다.
>
> 근거: [`tests/device/install_gate.sh`](../tests/device/install_gate.sh) 가 2 MB 로 자른 tarball 로 설치를 시도한다. 이 검사가 붙기 전 그 설치는 **성공을 보고했다.**
>
> **왜 파일 존재 확인만으로는 부족한가**: rootfs 는 완벽히 풀리고도 **돌지 않을 수 있다** — 아키텍처가 다른 로더, [ADR 0002](adr/0002-explicit-ldso-invocation.md) 가 요구하는 옵션이 없는 낡은 로더, 로드에 실패하는 preload. 셋 다 사용자의 첫 명령이 엉뚱해 보이는 이유로 실패하기 직전까지 **깨끗한 설치처럼 보인다.** 그래서 install 의 마지막 동작은 방금 만든 것을 **실제로 부팅해 보는 것**이다. 부팅 검사는 `alr run` 을 통과한다 — preload 와 슈퍼바이저를 포함해 사용자와 **같은 경로**다. 우회하는 검사는 아무도 못 쓰는 rootfs 에서도 통과한다.
>
> 실패해도 rootfs 를 **지우지 않는다**(`reason=rootfs-unbootable`). 이 시점은 `rename` 이후라, 지운다면 그것이 사용자가 요청하지 않은 디렉토리를 삭제하는 유일한 코드 경로가 된다.
>
> `INSTALL VERIFY SHA256` 는 검증할 다이제스트가 없으면 `PASS` 가 아니라 **`SKIP`** 이다(`--url`, 또는 SHA256SUMS 도달 불가). 검증되지 않은 다운로드를 `PASS` 로 세탁하지 않는다.
>
> `INSTALL GLIBC VERSION` 은 합격/불합격이 아니라 **기록된 사실**이다. 게스트 glibc 버전이 어떤 래퍼 이름이 존재하는지를 결정하고(`__xstat` 계열은 2.33 에서 사라졌다) 이 rootfs 에 대한 모든 버그 리포트에 들어가야 한다.
>
> `MEASURED` 2026-08-04, SM-X236N: `files=3413 skipped_special=0 setuid_masked=0`, `argv0=yes preload=yes library-path=yes inhibit-cache=yes`, `/bin/true exit=0 elapsed_ms=32`, glibc `2.39`.
>
> **이 리포트를 만들다 CLI 버그가 나왔다**: `alr install -d ubuntu-24.04` 는 — 다른 모든 서브커맨드가 받는 형태인데 — **`-d` 라는 이름의 rootfs 를 설치하고** 성공을 보고한 뒤 그것에 대한 9줄 리포트를 출력했다. 옵션 스캔이 자기가 아는 세 개만 찾고 `argv[i+1]` 을 무조건 distro 로 잡았으며, `-d` 는 `distro_name_ok()` 에서 합법이었다. 이제 install 도 옵션을 순서 무관하게 파싱하고 모르는 옵션은 `reason=install-unknown-option` 으로 거부하며, `-` 로 시작하는 distro 이름은 어떻게 들어왔든 거부한다.

`alr install`은 끝나기 전에 다음을 순서대로 수행하고, 실패 시 rootfs를 남기되 명확히 보고한다.

```
INSTALL DOWNLOAD:        PASS
INSTALL VERIFY SHA256:   PASS
INSTALL EXTRACT:         PASS  (files=<n> skipped_special=<n> setuid_masked=<n>)
INSTALL REPAIR:          PASS
INSTALL LDSO PRESENT:    PASS  <R>/lib/ld-linux-aarch64.so.1
INSTALL LDSO OPTIONS:    PASS  argv0=yes preload=yes library-path=yes inhibit-cache=yes
INSTALL BOOT /bin/true:  PASS  exit=0 elapsed_ms=<n>
INSTALL BOOT /bin/echo:  PASS  stdout="alr"
INSTALL GLIBC VERSION:   2.39
```

`INSTALL LDSO OPTIONS`는 `ld.so --help`를 파싱한다. `--argv0`이 없으면(glibc < 2.33) **경고**하고 `argv0_leaks=true`를 상태에 기록한다 ([§C2](01-platform-facts.md)).

## 5. 패키지 부트스트랩 (`alr install --with git,node,codex`)

**순서가 중요하다.** ca-certificates 없이는 HTTPS가 안 된다.

```
1. §3의 수리 완료
2. apt-get update                         (plain http — 동작함)
3. apt-get install -y --no-install-recommends ca-certificates git xz-utils
4. Node (선택):
     GET https://nodejs.org/dist/index.json  → lts가 truthy인 첫 항목
     (폴백 v24.18.1)
     node-v<V>-linux-arm64.tar.xz 다운로드
     SHASUMS256.txt 로 검증
     <R>/opt/node 에 추출, <R>/usr/local/bin 에 node/npm/npx 심링크
5. Codex (선택):
     GET https://github.com/openai/codex/releases/download/rust-v<V>/codex-aarch64-unknown-linux-musl.tar.gz
     codex-package_SHA256SUMS 로 검증
     단일 실행파일 추출 → <R>/usr/local/bin/codex
     <R>/root/.codex/config.toml 작성 (§5.2)
```

### 5.1 왜 apt의 nodejs가 아닌가

Ubuntu 24.04 아카이브의 nodejs는 **18.19.1 (EOL)**. 반면:
- nodejs.org tarball은 단일 ~30 MB fetch, 루트 불필요, 현재 LTS, 공개 SHA256.
- 그리고 apt의 18.19는 libuv 1.44.2라 io_uring을 안 부른다 — **안전한 쪽**이다. 사용자가 20/22를 깔면 io_uring SIGSYS가 발생하는데, 슈퍼바이저가 `-ENOSYS`로 구제한다 ([§C8](01-platform-facts.md)). 두 경로 다 동작해야 한다.

**NodeSource는 MVP에서 배제** (3자 저장소 추가는 실패 표면만 늘린다).

### 5.2 Codex는 Rust다 — Node가 필요 없다

`openai/codex`는 Rust(`codex-rs`)이고 npm 패키지는 배포 shim일 뿐이다. 바이너리 직접 설치가:
- Codex 경로에서 Node를 완전히 제거 → io_uring 위험 소멸
- npm 경로(~129 MB 플랫폼 tgz + Node 런타임을 같은 바이너리 exec하려고 끌어옴) 회피

**`https://chatgpt.com/codex/install.sh | sh` 금지** — 루트 가능한 정상 Linux를 가정한다.

**Codex의 Linux 샌드박스는 반드시 꺼야 한다** — Landlock과 bubblewrap이 Android 앱 프로세스에서 동작하지 않는다 ([§D4](01-platform-facts.md)).

`<R>/root/.codex/config.toml` — `with_codex()`가 **오늘 실제로 쓰는** 내용 (`src/cli/alr.c`):
```toml
# alr: Codex's Linux sandbox relies on Landlock and bubblewrap,
# neither of which functions inside an Android app process.
# Confirm the exact mode name with `codex --help` for your version.
sandbox_mode = "danger-full-access"
```

> ⚠️ **PENDING_DEVICE 유지 — 다만 절반은 확정됐다.**
>
> **확정된 것**: 디바이스에서 `codex --help`를 실제로 돌렸다 ([M7 실측](evidence/2026-08-02-m7-m8-workloads-perf.md), `codex-cli 0.146.0`). CLI 플래그는 **`-s, --sandbox <SANDBOX_MODE>`** 이고, 도움말에 나온 설정 키는 `sandbox_permissions` 였다.
>
> **확정되지 않은 것**: `<SANDBOX_MODE>`가 받는 값의 목록, 그리고 `sandbox_mode`가 유효한 TOML 키인지. 즉 위 블록의 `sandbox_mode = "danger-full-access"` 두 토큰은 **어느 쪽도 도움말로 뒷받침되지 않았다 — `UNVERIFIED`**. 설치는 성공하고 alr이 `alr: NOTE codex sandbox disabled` 를 찍지만, 그 NOTE는 **파일을 썼다**는 보고이지 codex가 그 파일을 읽고 샌드박스를 껐다는 증거가 아니다.
>
> **게다가 이 파일의 경로 자체가 의심스럽다.** codex는 정적 링크 musl 바이너리라 `LD_PRELOAD`가 원리적으로 닿지 않고, 모든 경로 연산이 rootfs가 아니라 Android 파일시스템으로 간다 ([M12 §8](evidence/2026-08-03-m12-spawn-resolver.md)). alr은 게스트에 `HOME=/root`를 넣지만(`src/cli/alr.c`), 가상화가 없는 codex에게 `/root`는 **rootfs 안이 아니라 Android의 `/root`** 다. 그렇다면 codex가 여는 `~/.codex/config.toml`은 우리가 쓴 `<R>/root/.codex/config.toml`이 아니다. 기동 시 매번 나오는 `could not create PATH aliases: Read-only file system` 이 같은 방향을 가리킨다. **codex가 실제로 어느 파일을 열었는지는 관측된 적이 없다 — `UNVERIFIED`.**
>
> **무엇이 이걸 끝내는가**: 한 세션에서 두 가지를 잡으면 닫힌다. (1) `--sandbox`에 없는 값을 줘서 나오는 오류 메시지나 도움말에서 **허용 모드 값 목록을 그대로 캡처**한다. (2) 같은 실행을 `strace -f -e trace=openat` 아래 돌려 codex가 여는 `config.toml`의 **절대 경로**를 본다.
> **무엇이 막고 있는가**: 우리 계측이 구조적으로 눈이 멀었다 — `ALR_LOG`는 preload 안에 있고 preload는 codex에 로드되지 않는다([M12 §8](evidence/2026-08-03-m12-spawn-resolver.md), `grep -c 'alr preload:'` = 0). 그래서 외부 관찰자(strace)가 필요하고, 값 목록도 도움말 전문을 뜬 기록이 아직 없다.
>
> 확정 전까지 값을 추측해 하드코딩하지 말 것 — **지금 코드가 쓰는 값도 추측이며**, 위에 그렇게 표시해 두었다. 확정되면 파일보다 **`--sandbox <MODE>` 플래그**로 옮기는 편이 낫다: argv는 정적 링크 바이너리에도 우리가 그대로 전달하는 유일한 통로다.

> ⚠️ **보안 고지**: Codex 샌드박스를 끄면 alr이 에이전트와 사용자 디바이스 사이의 유일한 방어선이 된다. `alr`은 보안 경계가 **아니다** ([00-product.md §5](00-product.md)). 이 사실을 설치 시 사용자에게 명시적으로 알린다.

### 5.3 fakeroot 사용

`apt`/`dpkg` 호출은 `ALR_FAKEROOT=1`로 실행한다. 비루트에서 `chown`/`mknod`가 `EPERM`이므로 필수다.

> **M6 결정 사항 — 종결: 자체 shim을 유지한다.** 원래의 A/B("SysV IPC가 안 막혀 있으면 업스트림 `fakeroot` 패키지를 그냥 쓴다")는 **전제가 무너져 성립하지 않는다.**
>
> 2026-08-03 레퍼런스 디바이스(MediaTek MT8775 / Android 16 / kernel 6.1.145-android14, uid≥10000, `Seccomp: 2`)에서 게스트의 `shmget`·`semget`·`msgget`이 **전부 `ENOSYS`** 로 돌아왔고, 같은 실행의 `ALR_LOG`에 슈퍼바이저가 이 셋에 대한 SIGSYS 트랩 **3건을 에뮬레이션**한 기록이 남았다. `ENOSYS`가 커널의 대답이 아니라 **차단 후 우리 슈퍼바이저의 대답**이라는 뜻이다 — SysV IPC는 zygote 블록리스트에 있고 게스트에서 **쓸 수 없다**.
>
> 업스트림 `fakeroot`의 기본 변종은 `faked` 데몬 + SysV 메시지큐로 동작한다([02-architecture.md §120](02-architecture.md)이 바로 그것을 대체한다고 적은 그 구조다). 따라서 이 디바이스에서는 동작할 수 없다. 유지보수 부채를 감수하고 mmap DB shim을 계속 쓴다.
>
> 곁가지 하나: 업스트림에는 SysV 대신 소켓을 쓰는 `fakeroot-tcp` 변종이 있다. 깔아본 적도 재본 적도 없다 — `UNVERIFIED`, 현재 추진하지 않는다.
>
> 근거: [M16 §1](evidence/2026-08-03-m16-ipc-audit.md) (프로브 소스 `tests/device/probe_ipc.c`). 같은 질문을 열어 둔 [RISKS R9](RISKS.md)와 [02-architecture.md §121](02-architecture.md)도 같은 답으로 닫힌다.

## 6. 재설치 / 삭제 / 목록

```
alr install ubuntu-24.04 --force     기존 rootfs 제거 후 재설치
alr remove ubuntu-24.04              rootfs + state 제거 (확인 프롬프트)
alr list                             설치된 배포판, 크기, glibc 버전, 마지막 doctor 결과
alr update-components ubuntu-24.04   libalr_preload.so 만 갱신
```

`alr update-components`가 중요하다 — `.so`를 고칠 때마다 rootfs를 다시 깔 이유가 없다.

## 7. 실패 분류

모든 실패는 안정적 `reason=` 코드를 갖는다. **이 목록은 코드가 실제로 방출하는 것과 정확히 일치해야 한다** — `scripts/check-reasons.sh` 가 양방향으로 검사하고 `make check` 에 걸려 있다.

```
bad-distro-name            bad-env
bad-option                 bad-workdir                boot-enoent
boot-failed                config-bad-value           config-unknown-key
config-write-failed        doctor-missing             doctor-unknown-option
download-corrupt           download-network           env-reserved
extract-permission         extract-traversal-reject   install-unknown-option
ldso-missing
no-supervisor-requested    not-a-rootfs               preload-install-failed
preload-missing-in-rootfs  preload-stale              remove-failed
rootfs-incomplete          rootfs-missing             rootfs-unbootable
too-many-env               unhooked-static-binary     unsupported-distro
with-failed                workdir-enoent
```

> **`already-installed` 는 2026-08-04 에 어휘에서 빠졌다** — 실패가 아니게 됐기 때문이다. 그 코드가 가리키던 상황은 *"이미 설치된 rootfs 에 `--with` 를 줬는데 무시했다"* 였는데, 이제 무시하지 않고 **실제로 설치한다**. 실패하면 `with-failed` 다.
>
> ⚠️ **이 목록은 2026-08-03 에 실측으로 다시 썼다.** 이전 판은 17개를 적어 두었는데 코드가 방출하는 것은 22개였고 **겹치는 것이 5개뿐**이었다(`download-network` `extract-permission` `ldso-missing` `boot-failed` `boot-enoent`). 목표 [G6](00-product.md) 은 "모든 실패가 안정적 `reason=` 코드로 분류된다" 이고 그 측정 수단이 이 어휘인데, **어느 쪽도 지키지 않는 어휘는 G6 을 참으로도 거짓으로도 만들 수 없다.** 그래서 G6 은 목표가 아니라 문장이었다.
>
> 지워진 12개는 **구현되지 않은 기능의 코드**였다 — `extract-traversal-reject`(자체 untar, §2), `apt-*-failed`, `node-fetch-failed`, `codex-fetch-failed`, `download-404`, `extract-disk-full`, `repair-write-failed`, `boot-sigsys`, `ldso-option-unsupported`, `unsupported-android-policy`. **그 기능이 들어오면 코드와 함께 이 목록으로 돌아온다** — 게이트가 그때 강제한다. 미리 적어 두는 것은 계약이 아니라 약속이다.
>
> 2026-08-04 에 `extract-traversal-reject` 가 실제로 그렇게 돌아왔다([ADR 0009](adr/0009-no-in-process-untar.md)). 자체 untar 는 만들지 않기로 했지만 §2 의 traversal 규칙 중 GNU tar 가 채우지 못하던 것 — 추출 루트를 벗어나는 링크 타깃 — 은 코드로 닫혔고, 코드가 방출하므로 어휘로 돌아왔다.
>
> `download-checksum-mismatch` 는 같은 뜻의 `download-corrupt` 로 이미 출하됐다(v0.3.0). **출하된 코드가 안정 계약**이므로 스펙 쪽을 버렸다.
