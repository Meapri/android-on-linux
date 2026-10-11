# 설치 가이드 (Termux)

Termux에 `alr`을 설치하고 Ubuntu 26.04 LTS arm64 게스트에서 명령을 실행하기까지의 절차다.
개발자용 온디바이스 루프(sshd, `scripts/dev-push.sh`)는 [scripts/dev-bootstrap.md](../scripts/dev-bootstrap.md)에 따로 있다.

> **지원 범위.** Android 16과 17, arm64 기기, F-Droid 또는 GitHub판 Termux, Ubuntu 26.04 게스트다.
> 다른 Android 릴리스는 범위 밖이다([ADR 0007](adr/0007-android-16-only.md)). 실측한 기기는 Samsung 두 대다.
> MediaTek(커널 6.1, Android 16)과 Snapdragon 8 Elite(커널 6.6, Android 16·17)이며, Android 17은 Snapdragon 한 대에서만 쟀다.
> 다른 제조사(OEM)는 확인하지 못했다([M19](evidence/2026-08-03-m19-snapdragon.md)).

전체 흐름은 다음과 같다.

1. 기기 조건을 확인한다(§1).
2. F-Droid 또는 GitHub판 Termux를 설치한다(§2).
3. `alr` tarball을 `$PREFIX`에 푼다(§3).
4. `alr-doctor`로 기기를 진단한다(§4).
5. `alr install --with git`으로 게스트를 받는다(§6).
6. `alr run`·`alr shell`로 쓴다(§7).

## 1. 기기 요구사항

| 항목 | 요구 | 확인 방법 |
|---|---|---|
| CPU | **arm64 (aarch64)** 전용 | `uname -m` 이 `aarch64` |
| Android | **16, 17.** 12~15는 지원하지 않는다. 미검증이 아니라 범위 밖이다([ADR 0007](adr/0007-android-16-only.md)) | 설정 → 휴대전화 정보 |
| Termux | **F-Droid 또는 GitHub 릴리스 빌드** (`targetSdkVersion=28`) | §2 |
| 루팅 | 필요 없다. 루팅한 기기는 검증 대상이 아니다 | §5 |
| 저장 공간 | ubuntu-base tarball 약 30 MB와 추출한 rootfs. `git`·`node`까지 넣으면 수백 MB | |
| 네트워크 | `alr install`이 `cdimage.ubuntu.com`과 Ubuntu 아카이브에 접속한다 | |

x86 에뮬레이션(box64/FEX)은 비목표다. arm64 바이너리만 실행한다.

## 2. Termux — F-Droid/GitHub판이어야 한다 (Play Store판 아님)

Termux는 서로 다른 두 코드베이스로 배포된다. `alr`은 그중 하나에서만 동작한다.

| | F-Droid / GitHub (`termux/termux-app`) | Play Store (`termux/termux-apps`) |
|---|---|---|
| `targetSdkVersion` | **28** | 37 |
| SELinux 도메인 | `untrusted_app_27` | `untrusted_app` |
| 앱 데이터 경로의 ELF execve | **허용** | **거부** |
| `alr` 지원 | ✅ | ❌ |

받는 곳은 둘 중 하나다.

- F-Droid 앱 스토어의 Termux
- <https://github.com/termux/termux-app/releases> 의 `arm64-v8a` APK

Play Store판을 지원하지 않는 이유는 정책이 아니라 원리다([ADR 0005](adr/0005-play-store-unsupported.md)).
targetSdk 29 이상인 앱은 SELinux `untrusted_app` 도메인을 받는다. 이 도메인에는 앱 데이터 경로에 있는 실행 파일의
`execute_no_trans` 권한이 없다. Play판 termux-exec는 `/system/bin/linker64`를 대신 실행하는 우회를 쓰지만,
그 링커는 bionic 링커라서 glibc 프로그램을 로드할 수 없다. `alr`의 게스트는 모두 glibc다.

> ⚠️ `sharedUserId` 때문에 한 기기의 모든 Termux APK(본체, Termux:API, Termux:Widget 등)는 한 출처에서 와야 한다.
> F-Droid판과 Play판을 섞어 설치하면 설치가 거부된다. 이미 Play판을 쓰고 있었다면 모두 지운 뒤 F-Droid/GitHub판으로 다시 설치한다.

### 2.1 Google Play Protect 대화상자 — 기기에서 직접 눌러야 한다

GMS가 있는 기기에서 F-Droid/GitHub판 Termux를 설치하면 아래 대화상자가 뜬다
(2026-08-02, SM-X236N / Android 16에서 실측, [RISKS.md R2b](RISKS.md)).

```
Google Play 프로텍트
안전하지 않은 앱 차단됨 — Termux
이 앱은 Android 이전 버전에 맞게 개발되었으며 최신 개인 정보 보호 기능을 포함하지 않습니다.
[세부정보 더보기]  [확인]
```

`targetSdk 28` 때문에 나오는 경고다. 누르는 버튼을 틀리면 설치가 취소된다.

- ❌ **`확인`을 누르면 설치가 취소된다.** 이름과 달리 진행이 아니라 중단이다.
- ✅ **`세부정보 더보기` → `무시하고 설치`**를 눌러야 설치가 진행된다.

`adb install`로 설치하면 명령이 이 대화상자를 기다리며 멈춰 있다가, 기기에서 승인하면 끝난다. 설치를 확인하는 방법은 다음과 같다.

```bash
adb shell pm path com.termux    # 비어 있으면 아직 설치되지 않았다
```

> 이 경고는 `alr`이 고칠 수 없다. Termux 앱의 `targetSdk`에 달려 있고, 그 값이 낮은 것이 바로 `alr`이 동작하는 이유다.
> Play Protect 검증을 꺼서 우회하는 방법도 있지만, 기기 보안 설정을 바꾸는 일이라 여기서 권하지 않는다.

설치한 뒤 Termux를 한 번 실행해서 부트스트랩이 풀리기를 기다린다(수십 초). 프롬프트가 뜨면 다음으로 넘어간다.

## 3. `alr` 설치

`alr`은 tarball 하나로 배포된다. 안의 배치는 `$PREFIX` 아래에 그대로 대응한다.

```
bin/alr                       -> $PREFIX/bin/alr              호스트 CLI (bionic, 정적)
bin/alr-doctor                -> $PREFIX/bin/alr-doctor       기기 진단
libexec/alr/alr-lane          -> $PREFIX/libexec/alr/alr-lane 정적·Go 게스트용 레인 (ADR 0010)
share/alr/libalr_preload.so   -> $PREFIX/share/alr/...        게스트 경로 가상화 (glibc)
share/alr/manifest.json       -> $PREFIX/share/alr/...        preload 빌드 정보
share/alr/LICENSE.tawcroot    -> $PREFIX/share/alr/...        alr-lane의 MIT 고지
LICENSE
README.md
```

### 3.1 공개 릴리스 설치 (권장)

Termux 안에서 실행한다. 인증 없이 받을 수 있다.

```bash
pkg install -y curl tar
V=1.0.0
curl -fsSLO https://github.com/Meapri/alr-termux/releases/download/v$V/alr-$V-aarch64.tar.gz
curl -fsSLO https://github.com/Meapri/alr-termux/releases/download/v$V/SHA256SUMS
sha256sum -c SHA256SUMS                   # alr-1.0.0-aarch64.tar.gz: OK
tar -C "$PREFIX" -xzf alr-$V-aarch64.tar.gz
ls "$PREFIX/libexec/alr/alr-lane"         # 있어야 한다
alr version
```

> v0.5.0 이하 릴리스에는 `libexec/alr/alr-lane`이 없고, 기본 게스트가 `ubuntu-24.04`다.
> 그 버전에서는 정적·Go 바이너리(codex, Go 툴체인, busybox-static)가 경로 가상화 없이 돈다. v1.0.0으로 올린 뒤 §3.4를 따른다.

### 3.2 저장소에서 직접 빌드

릴리스에 아직 들어가지 않은 변경을 쓰려면 tarball을 직접 빌드한다. 빌드는 macOS 또는 Linux 호스트에서 한다. 필요한 도구는 다음과 같다.

- Android NDK. `NDK` 환경 변수로 경로를 준다. r27.2(`27.2.12479018`)로 빌드를 확인했다.
- zig **0.16.0**. 게스트 preload를 빌드한다. 다른 버전이면 `scripts/build-preload.sh`가 멈춘다.

```bash
cd runtime/alr
ZIG=/path/to/zig-0.16.0/zig scripts/build-preload.sh          # build/libalr_preload.so
NDK=/path/to/android-ndk ZIG=/path/to/zig-0.16.0/zig scripts/make-release.sh
# -> dist/alr-<버전>-aarch64.tar.gz 와 dist/SHA256SUMS
```

`make-release.sh`는 구성 요소가 하나라도 없으면 아무것도 쓰지 않고 멈춘다. 만든 tarball을 기기로 옮기고, Termux 안에서 `$PREFIX`에 푼다.
`/sdcard`를 읽으려면 먼저 `termux-setup-storage`로 저장소 권한을 준다.

```bash
adb push dist/alr-1.0.0-aarch64.tar.gz /sdcard/Download/      # 호스트에서
termux-setup-storage                                          # Termux에서, 처음 한 번
tar -C "$PREFIX" -xzf ~/storage/downloads/alr-1.0.0-aarch64.tar.gz
```

### 3.3 tarball을 `$PREFIX`가 아닌 곳에 풀 때

`alr`은 구성 요소를 다음 순서로 찾는다.

1. 바이너리 옆
2. 바이너리 기준 `../share/alr/`, `../libexec/alr/`
3. `$PREFIX/share/alr/`, `$PREFIX/libexec/alr/`

그래서 `~/alr` 같은 곳에 풀어도 그 안의 `bin/alr`은 같은 tarball의 preload와 `alr-lane`을 쓴다. 이 조회는 v1.0.0부터 있다.
preload를 찾지 못하면 `alr`은 경고를 내고 진행하지만, 그 게스트는 rootfs가 아니라 Android 파일시스템을 본다. 정상 동작이 아니다.

### 3.4 업그레이드 확인

`alr version`은 preload를 두 줄로 보여 준다. `preload (host)`와 `preload (guest)`의 sha256이 같아야 한다.

```
preload (host)  .../share/alr/libalr_preload.so
  sha256       16167c4e...
preload (guest) .../ubuntu-26.04/usr/lib/alr/libalr_preload.so
  sha256       16167c4e...      ← 같아야 한다
```

새 tarball을 `$PREFIX`에 풀어도 rootfs 안의 사본은 바뀌지 않는다. 두 값이 다르면 `alr version`이 `reason=preload-stale`을 내고 exit 1로 끝난다. 그때 다음을 실행한다.

```bash
alr update-components
```

> 소스에서 다시 빌드해 해시를 대조하려면 릴리스와 같은 호스트 OS가 필요하다(2026-08-03 측정).
> 같은 소스와 zig 0.16.0이 macOS와 Linux에서 다른 해시를 낸다. 다른 OS에서 해시가 다른 것은 정상이며 변조의 증거가 아니다.

## 4. `alr-doctor` — 첫 실행에 한 번

```bash
alr-doctor
```

이 기기의 seccomp·SELinux 능력을 훑는다. **반드시 Termux 터미널 세션 안에서 직접 실행한다.**

`adb shell`이나 `run-as com.termux`로 실행하면 안 된다. Android의 앱 seccomp 필터는 zygote가 uid 10000 이상인 앱을 fork할 때만 설치되기 때문이다.

| 컨텍스트 | uid | Seccomp | 진단이 유효한가 |
|---|---|---|---|
| `adb shell` | 2000 (shell) | **0** | ❌ 모든 syscall이 허용으로 보인다 |
| `run-as com.termux` | 앱 uid | **0** | ❌ 같은 함정 |
| **Termux 세션 (또는 그 자식)** | 앱 uid | **2** | ✅ 유일하게 유효하다 |

`alr-doctor`의 P1 프로브가 필터 없는 컨텍스트를 감지하면 진단을 멈춘다.

Play Store판 Termux에서 실행하면 P3 프로브가 앱 데이터 경로의 ELF를 실행하지 못해 FATAL로 끝난다.

```
[P3 ] execve of app-private ELF (…) FAILED (Permission denied) -> FATAL
```

`alr` 본체가 Play판을 시작 시점에 감지해 거부하는 기능은 아직 없다. 그래서 설치 전에 `alr-doctor`를 먼저 실행해 보는 것이 Play판을 알아채는 방법이다.

## 5. 루팅된 기기와 permissive 기기

`getenforce`가 `Permissive`인 기기(대부분의 루팅·커스텀 ROM)에서는 zygote seccomp 필터가 설치되지 않는다.
그러면 `alr`이 해결하려는 문제들이 사라진 것처럼 보이고, 그 기기에서 얻은 결과는 다른 기기에 대해 아무것도 말해 주지 않는다.
루팅된 기기에서 `alr`을 쓰는 것은 막지 않는다. 다만 버그 리포트나 성능 수치의 근거로 쓸 때는 아래 두 조건을 확인한다.

```bash
getenforce                        # Enforcing 이어야 한다
grep Seccomp /proc/self/status    # Seccomp: 2 이어야 한다
```

## 6. 게스트 설치

```bash
alr install --with git            # Ubuntu 26.04 rootfs와 git
alr install --with git,node       # node·npm까지
```

> **`--with git` 없이 시작하지 않는다.** ubuntu-base는 최소 이미지라 git이 없다.
> 그냥 `alr install` 한 뒤 `alr run git status`를 실행하면 `reason=boot-enoent`로 끝난다.

`alr install`은 `cdimage.ubuntu.com`의 `SHA256SUMS`를 받아 현재 포인트 릴리스를 스스로 찾고, 같은 파일의 해시로 tarball을 검증한다.
`SHA256SUMS`에 닿지 못하면 `reason=discovery-failed`로 멈춘다. 해시 없이 받는 폴백은 없다.
오프라인이나 미러 환경에서는 `--url <tarball>`로 tarball을 직접 준다.

지원하는 게스트는 **Ubuntu 26.04 LTS 하나**다([ADR 0005](https://github.com/Meapri/android-on-linux/blob/main/docs/adr/0005-termux-first-goals.md)).
배포판을 지정하지 않은 명령은 항상 `ubuntu-26.04`를 쓴다. 이미 설치된 24.04 트리는 `-d ubuntu-24.04`로 이름을 주면 실행되지만 검증하지 않는다.

`--with codex`는 codex의 Rust 바이너리 릴리스를 받는다. codex는 정적 바이너리라 레인(`alr-lane`)에서 rootfs를 본다(v1.0.0 이상 필요).

## 7. 사용

```bash
alr run git status              # 게스트에서 명령 하나
alr run -w /root uname -a       # 게스트 cwd 지정
alr shell                       # 게스트 bash
alr run --fakeroot apt-get update
alr run --fakeroot apt-get install -y build-essential
alr list                        # 설치된 rootfs와 preload 상태
alr version                     # 버전, preload 경로·sha256, rootfs 경로
```

명령마다 바이너리 종류에 따라 실행 경로가 정해진다.

- **동적 glibc 바이너리**(bash, git, python3, node 등)는 `LD_PRELOAD` 경로 가상화로 돈다.
- **정적 바이너리와 Go 바이너리**(codex, Go 툴체인, busybox-static)는 `alr-lane`에서 돈다([ADR 0010](adr/0010-tawcroot-lane.md)).
  `ALR_LANE=0`이면 레인을 끄고 예전처럼 경로 가상화 없이 실행한다.

apt·dpkg는 uid 0을 요구한다. `alr`은 uid 0 흉내(fakeroot)를 기본으로 켜지 않으므로, 패키지를 설치할 때는 `--fakeroot`를 붙인다. 늘 켜 두려면 `alr config set runtime.fakeroot true`를 쓴다.

`ALR_LOG=1`을 붙이면 실행 끝에 슈퍼바이저 통계가 나온다. 정상이면 `path_traps=0 syscall_stops=0`이다.
경로 syscall에 ptrace를 걸지 않는다는 것이 PRoot와의 결정적 차이이고, 이 줄이 실행마다 그것을 확인해 준다.

```bash
ALR_LOG=1 alr run /bin/true
# alr supervisor: pids=… sigsys=… emulated=… path_traps=0 syscall_stops=0
```

## 8. 알려진 제약

전체 목록은 [RISKS.md §4](RISKS.md)에 있다.

- **`sudo`와 setuid는 동작하지 않는다.** `/data`는 `nosuid`이고, setuid 계열은 seccomp가 막는다. apt·dpkg처럼 uid 0을 요구하는 명령에는 `--fakeroot`를 붙인다(§7). 기본값은 꺼져 있다.
- **systemd와 서비스 관리는 없다.** 데몬을 띄우는 패키지는 설치되지만 서비스로 뜨지 않는다.
- **`/dev/full`은 열 수 없다.** Android sepolicy가 모든 앱에 막는다. Termux 네이티브 셸에서도 똑같이 `Permission denied`다.
- **레인 프로세스의 `/proc/<pid>/status`** 에는 `SigCgt`에 SIGSEGV·SIGBUS(그리고 SIGSYS)가 잡힌 것으로 보인다. 게스트가 설정한 핸들러는 그대로 동작한다([ADR 0010](adr/0010-tawcroot-lane.md) 결정 7).
- **경로 syscall만 반복하는 레인 워크로드**(정적 busybox의 `find`, `ls -lR`)는 `ALR_LANE=0` 대비 1.08~1.21배로, 성능 예산 경계에 있다([증거](evidence/2026-10-11-t5-lane-cost.md)).
- **GUI·X11·GPU는 이 CLI의 범위가 아니다.**
- **`alr`은 샌드박스가 아니다.** 경로 재작성은 편의 기능이지 보안 경계가 아니다.
- Android 12 이상의 phantom process 제한(기본 32개) 때문에, Termux가 백그라운드에 있을 때 게스트 프로세스가 회수될 수 있다.

## 9. 문제가 생기면

| 증상 | 확인할 것 |
|---|---|
| `alr-doctor` P3가 FATAL | Play Store판 Termux일 가능성이 높다. §2대로 F-Droid/GitHub판으로 바꾼다 |
| `alr: WARNING libalr_preload.so not found` | tarball을 풀지 않았거나 일부만 풀었다. §3 |
| 게스트가 Android 파일시스템을 본다 | 위와 같은 원인 |
| `go build`가 `mkdir /tmp/go-build…: permission denied` | `alr-lane`이 없는 tarball(v0.5.0 이하)이다. §3.1 |
| codex가 `could not create PATH aliases: Read-only file system` | 위와 같은 원인 |
| `alr version`이 `reason=preload-stale` | 업그레이드 뒤 rootfs 안 preload가 옛 것이다. `alr update-components` (§3.4) |
| `alr-doctor`가 진단을 멈춘다 | `adb shell`이나 `run-as`에서 실행했다. Termux 터미널에서 직접 실행한다 (§4) |
| `alr install`이 `reason=discovery-failed` | `SHA256SUMS`에 닿지 못했다. 네트워크를 확인하거나 `--url`을 준다 (§6) |
| `alr run git status`가 `reason=boot-enoent` | git이 없는 rootfs다. `alr run --fakeroot apt-get install -y git` (§6) |

버그 리포트에는 `alr version` 전체 출력과 `alr-doctor`의 totals 줄을 함께 넣어 달라.
루팅되었거나 permissive인 기기에서 나온 결과라면 §5를 먼저 읽어 주기 바란다.
