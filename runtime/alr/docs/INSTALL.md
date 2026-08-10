# 설치 가이드

`alr`을 폰이나 태블릿에 올려서 Ubuntu 24.04 arm64 게스트를 띄우기까지의 절차다.
개발자용 온디바이스 루프는 [scripts/dev-bootstrap.md](../scripts/dev-bootstrap.md)에 따로 있다.

> **`alr` 은 Android 16 전용이다.** 다른 릴리스는 **지원 범위 밖**이며, 검증하지 않고 동작을 주장하지 않는다
> ([ADR 0007](adr/0007-android-16-only.md)).
> 참조 디바이스는 **2대 — MediaTek(커널 6.1)과 Snapdragon 8 Elite(커널 6.6), 둘 다 Android 16.**
> 두 기기의 zygote 차단 syscall 239개 집합이 완전히 동일해 **SoC 벤더·커널 축은 실측으로 닫혔다**(단 두 기기 다 Samsung 이라 **OEM 축은 미측정이고 잴 수단이 없다** — 다른 OEM 이 안 된다는 뜻이 아니라 확인해 주지 못한다는 뜻이다)
> ([M19](evidence/2026-08-03-m19-snapdragon.md)).

## 1. 기기 요구사항

| 항목 | 요구 | 확인 방법 |
|---|---|---|
| CPU | **arm64 (aarch64)** 전용 | `uname -m` 이 `aarch64` |
| Android | **16 전용.** 12~15 는 **지원하지 않는다** — 미검증이 아니라 범위 밖이다 ([ADR 0007](adr/0007-android-16-only.md)) | 설정 → 휴대전화 정보 |
| Termux | **F-Droid 또는 GitHub 릴리스 빌드** (`targetSdkVersion=28`) | §2 |
| 루팅 | 불필요. **루팅했다면 오히려 검증 대상이 아니다** | §5 |
| 저장 공간 | ubuntu-base tarball 약 30 MB + 추출 후 rootfs. `git`까지 넣으면 수백 MB | |

x86 에뮬레이션(box64/FEX)은 비목표다. arm64 네이티브만 돌린다.

## 2. Termux — F-Droid/GitHub 빌드여야 한다 (Play Store 빌드 아님)

Termux는 **서로 다른 두 코드베이스**로 배포된다. `alr`은 그중 하나에서만 동작한다.

| | F-Droid / GitHub (`termux/termux-app`) | Play Store (`termux/termux-apps`) |
|---|---|---|
| `targetSdkVersion` | **28** | 37 |
| SELinux 도메인 | `untrusted_app_27` | `untrusted_app` |
| 앱 데이터 경로 execve | **허용** | **거부** |
| `alr` 지원 | ✅ | ❌ (v1 미지원) |

받는 곳:

- F-Droid 앱 스토어의 Termux, 또는
- <https://github.com/termux/termux-app/releases>

**Play Store 빌드를 지원하지 않는 이유**는 정책이 아니라 원리다 ([ADR 0005](adr/0005-play-store-unsupported.md)).
targetSdk ≥ 29인 앱은 SELinux `untrusted_app` 도메인을 받고, 이 도메인은 앱 데이터 경로의 실행 파일에 대한
`execute_no_trans` 권한이 없다. Play 빌드의 termux-exec는 `/system/bin/linker64`를 대신 실행시키는 우회를 쓰는데,
**그건 bionic 링커라 glibc 프로그램을 로드할 수 없다.** `alr`의 게스트는 전부 glibc다.

Play 빌드에 깔았을 때 나올 신호는 `alr-doctor`의 P3 프로브일 것이다. **Play 빌드가 `targetSdk=37` 을 싣는다는 전제는 이제 실측됐지만**([M19 §3](evidence/2026-08-03-m19-snapdragon.md)), **거기서 `alr` 이 실패하는 것까지 잰 것은 아니다** — GitHub 빌드를 설치하려면 서명이 달라 Play 빌드를 먼저 지워야 했다. 앱 데이터 경로의 ELF를
execve 해 보고 실패하면 이렇게 말한다:

```
[P3 ] execve of app-private ELF (…) FAILED (Permission denied) -> FATAL
      This host cannot run guest binaries at all.  Either it is
      a targetSdk>=29 build (Play Store Termux, unsupported — see ADR 0005)
      or Android policy changed.
```

> `alr` 본체가 Play 빌드를 시작 시점에 감지해 [ADR 0005](adr/0005-play-store-unsupported.md)의 전용 메시지
> (`reason=unsupported-android-policy`)로 거부하는 것은 **아직 구현되어 있지 않다 (UNVERIFIED / 미구현).**
> 그때까지는 설치 전에 `alr-doctor`를 먼저 돌리는 것이 Play 빌드를 알아채는 방법이다.

> ⚠️ `sharedUserId` 때문에 **한 기기의 모든 Termux APK(앱 본체, Termux:API, Termux:Widget …)는 한 출처에서 와야 한다.**
> F-Droid판과 Play판을 섞어 깔면 설치 자체가 거부된다. 이미 Play판을 쓰고 있었다면 전부 지운 뒤 F-Droid/GitHub판으로 다시 깐다.

### 2.1 Google Play Protect가 이 설치를 막는다 — 기기에서 직접 눌러야 한다

GMS가 있는 기기에서 F-Droid/GitHub판 Termux를 설치하면 아래 대화상자가 뜬다
(2026-08-02, SM-X236N / Android 16에서 실측 — [RISKS.md R2b](RISKS.md)):

```
Google Play 프로텍트
안전하지 않은 앱 차단됨 — Termux
이 앱은 Android 이전 버전에 맞게 개발되었으며 최신 개인 정보 보호 기능을 포함하지 않습니다.
[세부정보 더보기]  [확인]
```

`targetSdk 28`이라서 나오는 경고다. 여기서 **누르는 버튼을 틀리면 설치가 취소된다**:

- ❌ **`확인`을 누르면 설치가 취소된다.** "확인"이라는 이름과 달리 진행이 아니라 중단이다.
- ✅ **`세부정보 더보기` → `무시하고 설치`** 를 눌러야 설치가 진행된다.

`adb install`로 넣는 경우 명령은 이 대화상자를 기다리며 멈춰 있다가, 기기에서 승인하면 완료된다.
설치 확인:

```bash
adb shell pm path com.termux    # 비어 있으면 아직 미설치
```

> 이건 `alr`이 고칠 수 있는 문제가 아니다. Termux 앱의 `targetSdk`에 달려 있고, 그 값이 낮은 것이
> 바로 `alr`이 동작하는 이유다.
>
> Play Protect 검증을 꺼서 우회하는 방법이 있지만 그건 **기기 보안 설정 변경**이므로 여기서 권하지 않는다.
> 필요하다면 트레이드오프를 이해한 상태에서 사용자가 직접 판단할 일이다.

## 3. `alr` 설치

Termux를 한 번 실행해 부트스트랩이 풀리기를 기다린 뒤, 릴리스 tarball을 `$PREFIX`에 푼다.

```bash
# Termux 안에서. 인증 없이 그대로 동작한다 (익명 curl 로 검증).
pkg install -y curl tar
V=0.5.0        # src/common/alr_version.h 의 ALR_VERSION 과 같아야 한다
curl -fsSLO https://github.com/Meapri/alr-termux/releases/download/v$V/alr-$V-aarch64.tar.gz
tar -C "$PREFIX" -xzf alr-$V-aarch64.tar.gz
alr version
```

tarball 안의 배치는 그대로 `$PREFIX` 아래에 대응된다:

```
bin/alr                       -> $PREFIX/bin/alr
bin/alr-doctor                -> $PREFIX/bin/alr-doctor
share/alr/libalr_preload.so   -> $PREFIX/share/alr/libalr_preload.so
share/alr/manifest.json       -> $PREFIX/share/alr/manifest.json
LICENSE
README.md
```

`libalr_preload.so`는 **게스트가 경로 가상화를 받는 유일한 수단**이다. `alr`은 이것을 바이너리 옆에서 먼저 찾고
없으면 `$PREFIX/share/alr/`에서 찾는다. 둘 다 없으면 경고를 내고 계속 진행하지만, 그 게스트는 rootfs가 아니라
Android 파일시스템을 보게 된다 — 즉 정상 동작이 아니다.

`alr version`은 버전, preload 경로, preload의 `sha256`, rootfs 경로를 출력한다.
`alr version` 이 **두 줄**을 낸다 — `preload (host)` 와 `preload (guest)`. **둘의 sha256 이 같아야 한다.**

```
preload (host)  .../share/alr/libalr_preload.so
  sha256       16167c4e...
preload (guest) .../ubuntu-24.04/usr/lib/alr/libalr_preload.so
  sha256       16167c4e...      ← 같아야 한다
```

다르면 `alr` 은 업그레이드됐는데 **게스트는 옛 preload 를 계속 로드**하고 있는 것이다. `alr version` 이 `reason=preload-stale` 로 알려 주고 exit 1 을 낸다:

```bash
alr update-components
```

> ⚠️ **이전 판의 검증 절차는 동어반복이었다.** `share/alr/manifest.json` 의 `output_sha256` 을 `alr version` 과 비교하라고 했는데, **양쪽 다 `$PREFIX` 를 읽는다.** 게스트가 로드하는 사본은 rootfs 안에 따로 있고, 그것이 어긋나는 것이 실제로 일어나는 사고다 — 문서가 시키는 업그레이드(`$PREFIX` 에 새 tarball 풀기)가 rootfs 안 사본을 건드리지 않기 때문이다.

> ⚠️ **소스에서 재빌드해 해시를 대조하려면 릴리스와 같은 호스트 OS 가 필요하다** — `MEASURED` 2026-08-03.
> 같은 소스·같은 zig 0.16.0 이 macOS 에서 `b30dd81e…`, 릴리스 러너(Linux)에서 `16167c4e…` 를 낸다.
> 각 호스트 안에서는 콜드 캐시 재빌드가 바이트 단위로 동일하다. 즉 이 저장소가 게이트하는 재현성은
> **"내 재빌드가 내 재빌드와 같다"** 이고, 다른 OS 에서 해시가 다른 것은 정상이며 변조의 증거가 아니다.
> 위의 manifest ↔ `alr version` 대조는 **배포된 바이트 자체를 보는 것**이라 이 문제와 무관하게 유효하다.

## 4. `alr-doctor` — 첫 실행에 한 번

```bash
alr-doctor
```

이 기기의 seccomp/SELinux 능력을 훑고, 마지막에 SIGSYS 에뮬레이션 테이블을 C 소스로 출력한다.
**반드시 Termux 터미널 세션 안에서 직접 실행한다.**

`adb shell`이나 `run-as com.termux`로 돌리면 안 된다. Android의 앱 seccomp 필터는 **zygote가 uid ≥ 10000인 앱을
fork할 때만** 설치되기 때문이다:

| 컨텍스트 | uid | Seccomp | 진단이 유효한가 |
|---|---|---|---|
| `adb shell` | 2000 (shell) | **0** | ❌ 모든 syscall이 허용으로 보인다 |
| `run-as com.termux` | 앱 uid | **0** | ❌ 같은 함정 |
| **Termux 세션 (또는 그 자식)** | 앱 uid | **2** | ✅ 유일하게 유효 |

`alr-doctor`의 P1 프로브가 이걸 감지해서, 필터가 없는 컨텍스트면 **진단을 중단한다.** 잘못된 안심을 주느니
아무 답도 주지 않는 쪽을 택한 것이다.

## 5. 루팅된 기기 / permissive 기기는 유효한 테스트 대상이 아니다

[00-product.md §6.6](00-product.md)의 정직성 규칙이다.

`getenforce`가 `Permissive`인 기기(대부분의 루팅/커스텀 ROM)에서는 **zygote seccomp 필터가 아예 설치되지 않는다.**
그러면 `alr`이 존재하는 이유인 문제들 — `set_robust_list`가 SIGSYS로 죽는 것, `NETLINK_AUDIT`이 거부되는 것 —
이 전부 사라진 것처럼 보인다. "잘 되는데요"라는 결과가 나오지만 그 결과는 다른 기기에 대해 아무것도 말해주지 않는다.

유효한 증거의 조건은 두 가지다:

```bash
getenforce                        # Enforcing 이어야 한다
grep Seccomp /proc/self/status    # Seccomp: 2 이어야 한다
```

루팅된 기기에서 `alr`을 **쓰는 것**은 막지 않는다. 다만 거기서 나온 결과를 버그 리포트나 성능 수치의 근거로
보내지는 말아 달라 — 우리 쪽에서 재현할 방법이 없다.

## 6. 게스트 프로비저닝

```bash
alr install                       # 기본값: ubuntu-24.04
alr install --with git,node       # 설치 직후 패키지까지
```

`--url`은 필요 없다. `alr install`이 `cdimage.ubuntu.com`의 `SHA256SUMS`를 받아 **현재 포인트 릴리스를 스스로
찾아내고**(`ubuntu-base-24.04-base-arm64.tar.gz`라는 이름은 존재하지 않는다 — 404다), 같은 파일에 적힌 해시로
내려받은 tarball을 **검증한다.** 해시가 맞지 않으면(캐시된 옛 tarball이 흔한 경우다) 다시 받는다.
오프라인·미러 환경을 위해 `--url <tarball>`은 그대로 남아 있다.

`SHA256SUMS`에 닿지 못하면 경고를 내고 고정된 폴백 이름으로 진행한다 — 조용히 넘어가지 않는다.

## 7. 사용

```bash
alr run git status          # 게스트에서 명령 하나
alr shell                   # 게스트 bash 진입
alr version                 # 버전 / preload 경로 / preload sha256 / rootfs 경로
```

`ALR_LOG=1`을 붙이면 실행 끝에 슈퍼바이저 통계가 나온다. 정상이라면 `path_traps=0 syscall_stops=0`이다 —
경로 syscall에 ptrace를 걸지 않는다는 것이 PRoot와의 결정적 차이이고, 이 줄이 그것을 매 실행 확인해 준다.

```bash
ALR_LOG=1 alr run /bin/true
# alr supervisor: pids=… sigsys=… emulated=… path_traps=0 syscall_stops=0
```

## 8. 알려진 제약

설치 전에 알고 있어야 하는 것들이다. 전체 목록은 [RISKS.md §4](RISKS.md).

- **Go로 컴파일된 게스트 바이너리**(`gh`, docker CLI, hugo 등)는 동작하지 않는다. Go는 libc를 우회해 raw `svc`를
  발행하므로 `LD_PRELOAD` 인터포저가 원리적으로 잡을 수 없다. (rootfs를 훑어 이런 바이너리를 미리 알려주는
  `alr-doctor` 프로브는 계획만 있고 **아직 없다** — 지금은 실행해 보고 알게 된다.)
- **`sudo`/setuid는 동작하지 않는다.** `/data`는 `nosuid`이고 setuid 계열은 seccomp로 막혀 있다.
- **systemd/init/서비스 관리는 없다.** 데몬을 띄우는 패키지는 설치는 되지만 서비스로 뜨지 않는다.
- **GUI/X11/GPU는 비목표다.** CLI 전용이다.
- **`alr`은 샌드박스가 아니다.** 경로 재작성은 편의이지 방어 경계가 아니다.
- Android 12+ 의 phantom process 제한(기본 32) 때문에, 앱이 백그라운드일 때 게스트 프로세스가 회수될 수 있다.

## 9. 문제가 생기면

| 증상 | 확인할 것 |
|---|---|
| `alr-doctor` P3 가 FATAL | Play Store 빌드 Termux일 가능성이 높다. §2대로 F-Droid/GitHub판으로 교체 (`alr` 자신이 `reason=unsupported-android-policy` 로 거부하는 것은 **미구현** — §2 참조) |
| `alr: WARNING libalr_preload.so not found` | tarball을 `$PREFIX`가 아닌 곳에 풀었다. §3 |
| 게스트가 Android 파일시스템을 본다 | 위와 같은 원인 |
| `alr-doctor`가 진단을 중단한다 | `adb shell`/`run-as`에서 돌렸다. Termux 터미널에서 직접 실행 (§4) |
| 다운로드가 404 | `SHA256SUMS` 발견에 실패해 폴백 핀으로 갔다. 네트워크 확인 후 재시도 (§6) |

버그 리포트에는 `alr version` 전체 출력과 `alr-doctor`의 totals 줄을 함께 넣어 달라.
루팅/permissive 기기에서 나온 결과라면 §5를 먼저 읽어 주기 바란다.
