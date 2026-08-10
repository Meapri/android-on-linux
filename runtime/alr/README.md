# alr — Termux-native Ubuntu ARM64 glibc runtime

`alr`은 **루팅 없이, Termux 안에서, 스톡 Ubuntu 24.04 ARM64 glibc rootfs를 PRoot보다 낮은 오버헤드로 실행**하는
독립형 런타임 + CLI다. 목표 워크로드는 `git`, `node`/`npm`, `make`, `apt` 같은 평범한 Linux CLI 프로그램을
폰의 CPU 성능 그대로 돌리는 것이다.

> **선행 릴리스 (v0.5.0).** 참조 디바이스는 **2대 — MediaTek(커널 6.1)과 Snapdragon 8 Elite(커널 6.6),
> 둘 다 Android 16**. 두 기기의 zygote 차단 syscall **239개 집합이 완전히 동일하고**, 호환성 폭
> 96/96 도 같다 ([M19](docs/evidence/2026-08-03-m19-snapdragon.md)). **SoC 벤더·커널 축은 닫혔다.**
> 수용 시험 **164건 PASS / 0 FAIL**, 설치 게이트 18건, 디바이스 없이 도는 호스트 게이트 10종.
> (**OEM 축은 아니다** — 두 기기 다 Samsung 이고, 다른 OEM 기기를 구할 수단이 없어 **앞으로도 못 잰다.**
> 다른 OEM 이 안 된다는 뜻이 아니라 **우리가 확인해 주지 못한다**는 뜻이다.)
>
> **Android 16 전용이다.** 12~15 는 **지원 범위 밖**이며 검증하지 않는다 —
> 미해결 항목이 아니라 결정이다([ADR 0007](docs/adr/0007-android-16-only.md)).
> 근거: 이 설계가 딛고 선 bionic allowlist 는 릴리스마다 커졌고(android12 365줄 → android16 392줄),
> 갈린 축(벤더·커널)은 전부 무관했는데 **정작 그 축만 고정돼 있었다.** 재 보지 않을 것을 지원한다고 적지 않는다.
>
> 성능 수치는 대부분 **1회 세션 측정**이고 **배수는 기기마다 다르다.** 그리고 **기기 상태에 크게 좌우된다** —
> 96개 패키지를 설치한 직후에 잰 exec 처리량이 330/s, 식은 뒤 같은 폰·같은 바이너리로 707/s 였다
> ([M19 §6.2](docs/evidence/2026-08-03-m19-snapdragon.md)). 하네스는 이제 워밍업 1회를 버리고 `[min-max]` 를 함께 낸다.
> 아래 수치는 **두 기기 모두 그 규율로 다시 잰 것**이다. 각 수치의 caveat은 표 안에 적어 뒀다 — 각주로 숨기지 않았다.

## 빠른 시작

1. **Termux를 F-Droid(또는 GitHub 릴리스)에서 설치한다.** Play Store 빌드는 지원하지 않는다 —
   원리적으로 불가능하다([ADR 0005](docs/adr/0005-play-store-unsupported.md)). 설치 도중 Google Play Protect가
   차단 대화상자를 띄우는데, 거기서 **`확인`을 누르면 설치가 취소된다**. 전체 절차는
   [docs/INSTALL.md](docs/INSTALL.md).
2. 릴리스 tarball을 `$PREFIX`에 풀고, Termux 터미널에서:

```bash
alr-doctor                  # 기기 능력 진단 (첫 실행에 한 번)
alr install --with git      # Ubuntu 24.04 rootfs + git
alr run git status          # 게스트에서 명령 하나
alr shell                   # 게스트 bash 진입
alr version                 # 버전 / preload 경로 / preload sha256 / rootfs 경로
```

> **`--with git` 를 뺀 `alr install` 로 시작하지 말 것.** ubuntu-base 는 최소 이미지라 **git 이 없다** — 그냥 `alr install` 한 뒤 `alr run git status` 를 치면 `reason=boot-enoent` 로 죽는다. 이 README 의 이전 판이 정확히 그 두 줄이었다.
>
> 이미 rootfs 를 깔았다면 나중에 넣는 명령은 이것이다 — **`--fakeroot` 가 필요하다.** apt/dpkg 는 uid 0 을 요구하고, alr 은 그걸 기본으로 켜지 않는다:
> ```bash
> alr run --fakeroot apt-get update
> alr run --fakeroot apt-get install -y git
> ```
> 또는 한 번만 켜 두려면 `alr config set runtime.fakeroot true`.

`alr install`에 **`--url`은 더 이상 필요 없다.** `ubuntu-base-24.04-base-arm64.tar.gz`라는 이름은 존재하지 않고
(404) `latest` 심링크도 없기 때문에, `alr`이 `cdimage.ubuntu.com`의 `SHA256SUMS`를 읽어 **현재 포인트 릴리스를
스스로 찾아내고 그 해시로 내려받은 tarball을 검증한다** ([M12 §5](docs/evidence/2026-08-03-m12-spawn-resolver.md)).
캐시된 tarball도 매번 재검증한다. 오프라인·미러용으로 `--url <tarball>`은 그대로 남아 있다.

## 포지셔닝

> grun은 빠르지만 포크된 glibc와 포크된 패키지 세트를 요구한다. proot-distro는 Ubuntu 아카이브 전체를 쓰지만
> path syscall마다 ptrace 비용을 낸다. **alr은 둘 다 얻는 첫 시도다.**
>
> — [docs/00-product.md §4](docs/00-product.md)

`grun`과의 **순수 속도 대결은 무승부에 가깝다.** 차별점은 속도가 아니라 "스톡 rootfs 호환성 + 네이티브급 속도"의
결합이고, 그래서 아래의 호환성 폭 수치가 성능 수치만큼 중요하다.

## 실측

기기는 두 대다 — MediaTek arm64 / 커널 6.1 / `uid=10297`, Snapdragon 8 Elite / 커널 6.6 / `uid=10447`.
둘 다 Android 16, `Seccomp=2 untrusted_app_27`. 각 표에 어느 기기인지 적었다.

### proot-distro A/B — `git status` 10,000 파일

**양쪽에 같은 git 2.53.0** — proot-distro 가 제공하는 ubuntu 가 26.04 뿐이라 alr 쪽도 26.04 게스트로 맞췄다.
워밍업 1회를 버리고 7회 중앙값 `[min-max]`, Snapdragon 8 Elite ([M19 §6.1](docs/evidence/2026-08-03-m19-snapdragon.md)):

| | git | 시간 | |
|---|---|---|---|
| native (Termux bionic) | 2.55.0 | 33 ms `[28-40]` | — |
| **alr** (Ubuntu glibc) | **2.53.0** | **36 ms** `[34-44]` | 네이티브와 3 ms 차 |
| proot-distro | **2.53.0** | 930 ms `[873-971]` | **25.8× 느림** |

> **이쪽이 아래 M8 수치보다 조건이 깨끗하다.** alr/proot 두 다리가 **같은 배포판·같은 git 버전**이다 —
> M8 을 약하게 만들었던 "빌드 상이" 를 제거했다. 그러므로 인용할 배수는 **25.8×** 다.
> **이 워크로드는 기기 상태에 둔감하다** — 워밍업 없이 잰 37/39/947 과 위 값이 전부 스프레드 안이다.
> 같은 세션의 exec 처리량이 330↔707 로 갈린 것과 대조된다.
> **남은 caveat**: native 다리만 여전히 다른 빌드(Termux git 2.55.0)라 "네이티브와 3 ms" 는 근사치다.

<details><summary>참조 기기 #1 (MediaTek) 의 원래 M8 측정 — 세 빌드가 달랐다</summary>

10,000 파일 저장소, 각 5회 중앙값 ([M8](docs/evidence/2026-08-02-m7-m8-workloads-perf.md) §PRoot A/B):

| | git | 시간 |
|---|---|---|
| native (Termux bionic) | 2.55 | 42 ms |
| **alr** (Ubuntu glibc) | 2.43 | **49 ms** |
| proot-distro (Ubuntu) | 2.53 | 1,704 ms → **34.8×** |

기동(`/bin/true`), 9회 중앙값: native 24 ms / **alr 28 ms** / proot-distro 304 ms → **10.9×**.

**34.8× 를 헤드라인으로 쓰지 않았고, 지금도 쓰지 않는다** — 세 실행의 git 빌드가 서로 달랐기 때문이다.
34.8× → 25.8× 의 하락은 기기 차이와 **git 버전 통일**이 섞여 있어 분리되지 않는다. 더 엄밀한 쪽을 쓴다.
</details>

### proot-distro A/B — `npm ci`

105개 패키지 프로젝트, 3회 ([M12 §4](docs/evidence/2026-08-03-m12-spawn-resolver.md)):

| | 실행 시간 | 배수 |
|---|---|---|
| proot-distro | 6.87 / 6.24 / 6.26 s | — |
| **alr** | **2.00 / 2.00 / 1.99 s** | **3.12×** |

> **이쪽이 위의 `git status` 비교보다 조건이 깨끗하다.** node 바이너리와 npm을 **복사해서 양쪽을 동일하게**
> 만들었고(v22.20.0 / 10.9.3), 락파일과 npm 캐시도 같다 — M8의 git 비교를 약하게 만들었던 "빌드 상이" 문제를
> 제거한 것이다. 양쪽 다 105개 설치 후 `tsc --version` 동작을 확인했다.
> **남은 caveat**: proot 게스트는 Ubuntu **26.04**, alr 게스트는 **24.04** 다. node/npm/락파일/캐시는 같지만
> 베이스 배포판이 다르다. 단일 MediaTek 기기, 1회 세션.
>
> [docs/00-product.md §4](docs/00-product.md)가 방어 가능하다고 적어 둔 범위가 1.5–3× 였으니, `git status`의
> 34.8× 와 달리 이 추정은 빗나가지 않았다.

### 호환성 폭

**빌드 툴체인·언어 런타임·CLI 유틸을 아우르는 큐레이션된 96개 Ubuntu noble 패키지에서 무수정 설치 96/96,
실행 96/96 — 기기 2대(MediaTek·Snapdragon)에서 각각**
([M11](docs/evidence/2026-08-02-m11-breadth.md), [M14](docs/evidence/2026-08-03-m14-ioctl-php.md),
[M19 §4](docs/evidence/2026-08-03-m19-snapdragon.md)).

> **아카이브 전체(수만 개)에 대한 주장이 아니다.** 이건 우리가 고른 96개에 대한 진술이다.
> 표현은 [docs/00-product.md §3](docs/00-product.md)의 승인된 문장을 그대로 쓴 것이다.

같은 줄에 두는 게 정직한 사실 하나: 스톡 Ubuntu 24.04.4 base rootfs가 **패치 0회, glibc 재빌드 0회**로 부팅한다.
아무것도 없는 상태에서 다운로드·추출·`apt install git` 까지 한 번에 **2분 27초**, 끝나면 `git version 2.43.0`
이 동작한다 ([M10](docs/evidence/2026-08-02-m10-apt-install-git.md) — 측정 당시 명령은 `alr install --url … --with git`
였다. `--url` 이 필요 없어진 것은 그 뒤의 [M12 §5](docs/evidence/2026-08-03-m12-spawn-resolver.md) 이고, 그 조합으로는
다시 재지 않았다).

### 수용 테스트

```
PASS=78  FAIL=0  KNOWN_FAIL=2  SKIP=0
```

[M15](docs/evidence/2026-08-03-m15-cmdline-2604.md) 시점 (라운드별로 60 → 73 → 74 → 76 → 78).
매 실행 `path_traps=0 syscall_stops=0` 을 함께 보고한다 — **path syscall에 ptrace를 걸지 않는다는 것이
PRoot와 갈리는 불변식**이고, 이 줄이 그것을 실행마다 확인해 준다.

> **이 숫자가 뜻하지 않는 것.** `PASS=78` 은 제품이 아니라 **그 시점에 존재하던 테스트**에 대한 진술이다.
> 실제로 M11 시점의 `PASS=60 KNOWN_FAIL=1` 은 참이었지만, 그때 `posix_spawn` 미구현으로 **`make` 가 깨져 있었고**
> 검사하는 테스트가 없어 PASS로도 FAIL로도 세어지지 않았다
> ([M12 §1](docs/evidence/2026-08-03-m12-spawn-resolver.md), [M11 §6 정정](docs/evidence/2026-08-02-m11-breadth.md)).
> 커버리지 밖은 보이지 않는다.

## 열려 있는 항목

| | 상태 |
|---|---|
| `/dev/full` 미에뮬레이션 | **의도된 영구 비목표.** 고칠 계획이 없다 (유일한 `KNOWN_FAIL`) |
| `php-cli` | 출하 빌드에서 **동작하지만 원인은 규명되지 않았다** — 아래 |
| `codex` | 정적 musl 바이너리라 `LD_PRELOAD` 가 닿지 않는다. 실행은 되나 경로 가상화가 없다 |
| Ubuntu 26.04 rootfs | 부팅하고 `bash`/`apt`/`grep`/`sed`/`awk`/`tar` 등은 동작하지만, 26.04가 기본 coreutils로 채택한 **uutils는 원리적으로 지원 불가** — libc를 우회해 raw `svc`를 발행하므로 `LD_PRELOAD`가 볼 수 없다. Go 바이너리와 같은 한계 |

**`/dev/full`** 을 서빙하려면 프로세스에서 가장 뜨거운 syscall인 `write()`를 인터포즈해야 하는데, 대상 워크로드 중
이 디바이스 노드를 쓰는 것이 없다. 게다가 실패 표면이 열려 있어서(`puts`/`putchar`/`fwrite_unlocked`/`dprintf`/…)
심볼 하나만 빠뜨려도 **성공한 쓰기로 조용히 통과**한다 — 열거 가능하고 요란하게 실패하는 `mkstemp`·NSS 계열과 다르다.
근거는 [docs/RISKS.md §4](docs/RISKS.md)와 [M12 §8](docs/evidence/2026-08-03-m12-spawn-resolver.md).

**`php-cli`** 는 출하 빌드에서 동작한다. 그러나 **고쳤다고 말할 수 없다.** 대조 실험으로 우리 인터포지션 탓이
아님은 증명했지만(NSS를 뺀 빌드에서도, 심볼이 하나도 없는 **빈 `.so`** 로도 동일하게 죽는다), 이어진 조사에서
abort 여부가 **preload의 심볼 테이블 크기에 민감**하다는 것이 드러났다 — 경계는 ~152개 심볼이고,
php가 `--version`에서 쓸 리 없는 심볼(`scandir`) **하나만 빼도 재발**한다. 즉 심볼을 덜어내는 변경이
php를 다시 깨뜨릴 수 있고, **그때 범인은 그 변경이 아니다.**
`breadth.sh`가 php를 포함하므로 회귀는 잡히지만 오귀속하기 쉽다 ([M14 §2](docs/evidence/2026-08-03-m14-ioctl-php.md)).

## 핵심 메커니즘 3줄 요약

1. **커널 execve를 진짜로 쓴다.** Termux(F-Droid, targetSdk 28)는 앱 데이터 경로 실행이 허용되므로,
   PRoot의 ptrace 슈퍼바이저도 Android APK판의 in-process ELF 리맵도 필요 없다.
2. **경로 가상화는 `LD_PRELOAD` glibc 인터포저**가 문자열 프리픽스 재작성으로 처리한다 — syscall당 컨텍스트 스위치 0회.
   `git status` 10k 파일에서 경로 호출 **9,912회 중 실제 재작성은 26회(0.26%)**, 나머지 99.7%는 상대경로라
   첫 바이트 검사 하나로 통과한다. 경로 계층의 총비용은 **≈ 40 µs**로 49 ms 중 0.08%다
   ([M8](docs/evidence/2026-08-02-m7-m8-workloads-perf.md)).
3. **Android zygote seccomp 필터가 죽이는 syscall만** 시그널 전용 ptrace 슈퍼바이저가 `SIGSYS`에서 구제한다.
   path syscall은 절대 트랩하지 않는다. 이것이 PRoot와의 결정적 차이다.

## 비목표

[docs/00-product.md §5](docs/00-product.md). 이건 "아직 안 됨"이 아니라 **하지 않기로 결정한 것**이다.

- **GUI / X11 / Wayland / GPU 가속** — 상위 프로젝트의 영역. `alr`은 CLI 전용.
- **Play Store Termux** ([ADR 0005](docs/adr/0005-play-store-unsupported.md)).
- **보안 격리** — `alr`은 샌드박스가 **아니다.** 경로 재작성은 방어 경계가 아니다.
- **x86 에뮬레이션**(box64/FEX) — 네이티브 arm64만.
- **systemd / init / 서비스 관리.**
- **setuid 의미론** — `/data`는 `nosuid`, setuid/setgid는 seccomp 차단. `sudo`류는 동작하지 않는다.
- **Go로 컴파일된 게스트 바이너리의 완전 지원** — Go는 libc를 우회해 raw `svc`를 발행하므로 `LD_PRELOAD`로 원리적 불가.

## 빌드

두 개의 ABI가 있고 섞이면 안 된다 ([docs/01-platform-facts.md §F1/§F2](docs/01-platform-facts.md)).

```bash
make test                       # 호스트 코어 테스트 (macOS/Linux, 디바이스 불필요)
make doctor NDK=/path/to/ndk    # alr-doctor 크로스 빌드 (bionic 측)
scripts/build-preload.sh        # 게스트 측 libalr_preload.so (zig 0.16.0 고정)
```

- **GUEST (glibc)**: `zig cc --target=aarch64-linux-gnu.2.17`, zig **0.16.0 정확히 고정**.
  재현성은 정확한 핀에서만 성립한다 — zig 업그레이드는 번들 compiler-rt와 LLVM codegen을 바꾼다.
- **HOST (bionic)**: Android NDK clang `--target=aarch64-linux-android24`,
  `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384` (16 KB 페이지 기기 대응).
- Termux 자체 온디바이스 clang은 **개발 이너 루프 전용**이며 릴리스 경로가 아니다.

`tests/device/` 와 슈퍼바이저 테스트는 **호스트에서 돌지 않는다.** `uid >= 10000 && Seccomp == 2` 가 아니면
스스로 실행을 거부한다 — 그 조건 밖에서는 zygote seccomp 필터가 없어 모든 문제가 사라진 것처럼 보이기 때문이다.
호스티드 러너로 대체할 수 있는 종류의 테스트가 아니다.

## 문서

| # | 문서 | 내용 |
|---|---|---|
| — | [docs/INSTALL.md](docs/INSTALL.md) | **설치 가이드** (Termux 출처, Play Protect, 기기 요구사항) |
| 0 | [docs/00-product.md](docs/00-product.md) | 제품 정의, 목표/비목표, 경쟁 포지셔닝, **정직성 규칙** |
| 1 | [docs/01-platform-facts.md](docs/01-platform-facts.md) | **검증된 플랫폼 사실 + 증거 등급.** 여기 없는 사실은 가정하지 말 것 |
| 2 | [docs/02-architecture.md](docs/02-architecture.md) | 레이어 모델, exec 체인, 컴포넌트 경계 |
| 3 | [docs/03-supervisor-spec.md](docs/03-supervisor-spec.md) | SIGSYS rescue 슈퍼바이저 스펙 |
| 4 | [docs/04-preload-spec.md](docs/04-preload-spec.md) | glibc `LD_PRELOAD` 인터포저 스펙 (심볼 표 포함) |
| 5 | [docs/05-provisioning-spec.md](docs/05-provisioning-spec.md) | rootfs 다운로드/추출/수리 |
| 6 | [docs/06-cli-spec.md](docs/06-cli-spec.md) | CLI 표면, 설정 파일, 디렉토리 레이아웃 |
| 7 | [docs/07-acceptance.md](docs/07-acceptance.md) | acceptance 문자열, 벤치마크, regression gate |
| 8 | [docs/08-milestones.md](docs/08-milestones.md) | 구현 순서 |
| 9 | [docs/09-codex-playbook.md](docs/09-codex-playbook.md) | 작업 규칙, 불변식, 막혔을 때 행동 |
| — | [docs/RISKS.md](docs/RISKS.md) | 미해결 리스크와 **명시적으로 수용한 한계** |
| — | [docs/adr/](docs/adr/) | 되돌리기 어려운 결정과 그 근거 |
| — | [docs/evidence/](docs/evidence/) | **디바이스 실측 기록.** README의 모든 수치가 여기서 나온다 |

## 상위 프로젝트와의 관계

`alr`은 [Meapri/android-on-linux](https://github.com/Meapri/android-on-linux)(ALR = Android on Linux Runtime)의
실행 기술을 **Termux 타깃으로 재설계**한 것이다. 상위 프로젝트는 APK(`untrusted_app`, targetSdk 35) 안에서
동작해야 해서 W^X/SELinux 제약에 맞춘 대규모 우회 장치(in-process ELF 리맵, PC-gated seccomp, ptrace path 중재)를
갖고 있다. Termux에서는 그 제약이 사라지므로 **훨씬 단순한 설계가 가능**하다.
무엇을 가져오고 무엇을 버리는지는 [docs/02-architecture.md §7](docs/02-architecture.md)에 파일 단위로 정리했다.

## 결과를 인용할 때

[docs/00-product.md §6](docs/00-product.md)의 정직성 규칙이 이 저장소 전체에 적용된다. 특히:

- **MEASURED와 MODELED를 섞지 않는다.** 디바이스에서 잰 숫자만 MEASURED다.
- **루팅된/permissive 기기의 결과는 증거가 아니다.** `getenforce == Enforcing` 이고 `Seccomp: 2` 인 기기만
  유효하다. permissive에서는 zygote seccomp 필터가 아예 설치되지 않아 모든 문제가 사라져 보인다.
- 상태는 `PASS` / `FAIL` / `SKIP` / `KNOWN_FAIL:<reason>` / `PENDING_DEVICE` 다섯 가지뿐이다.
  "대체로 동작함"은 없다.
