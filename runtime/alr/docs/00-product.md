# 00 — 제품 정의

## 1. 한 줄 정의

> Termux 안에서, 루팅 없이, **스톡 Ubuntu ARM64 glibc rootfs**를 **PRoot 대비 낮은 오버헤드**로 실행하는 독립형 런타임과 CLI.

## 2. 타깃

- **디바이스**: arm64, **Android 16 전용.** 벤더 2곳에서 검증됐고, 다른 릴리스는 **지원 범위 밖**이다([ADR 0007](adr/0007-android-16-only.md)).

| | 상태 |
|---|---|
| MediaTek MT8775, Android 16 (커널 `6.1.145-android14`) | **검증됨** — 수용 164, 폭 96/96 |
| Qualcomm Snapdragon 8 Elite SM8750, Android 16 (커널 `6.6.98-android15`) | **검증됨** — 폭 96/96, 차단 syscall 집합 동일 ([M19](evidence/2026-08-03-m19-snapdragon.md)) |
| Android 12 / 13 / 14 / 15 (모든 기기) | **범위 밖 — 지원하지 않는다.** 검증 계획 없음 ([ADR 0007](adr/0007-android-16-only.md)) |
| 다른 OEM 의 Android 16 | **미측정, 그리고 잴 수단이 없다.** 미지원이라는 뜻이 아니다 — 확인해 주지 못한다는 뜻이다 (아래) |

> ✅ **SoC 벤더·커널 축은 닫혔다.** 두 기기는 SoC 벤더가 다르고 커널도 6.1-android14 대 6.6-android15 로 갈리는데, **zygote 차단 syscall 239개 집합이 완전히 동일하다**([§A6](01-platform-facts.md)). 호환성 폭 96/96, 슈퍼바이저 12/12, `path_traps=0 syscall_stops=0` 도 모두 같다.
>
> ⚠️ **OEM 축은 재지 못했고, 앞으로도 못 잰다 — 두 기기 다 Samsung 이다.** zygote allowlist 는 SoC 벤더가 아니라 **OEM 이 빌드한 플랫폼 이미지 안의 bionic** 에서 오므로, 이 실측이 닫은 것은 정확히 **SoC·커널 축**이다.
>
> **이것은 Android 12~15 와 성격이 다르다.** 릴리스 축은 **변한다는 것이 확인된** 축이다(allowlist 가 android12 365줄 → android16 392줄로 자랐다) — 그래서 외삽을 거부하고 지원 범위에서 뺐다. OEM 축은 **변한다는 증거가 없는** 축이다: `SECCOMP_ALLOWLIST_*.TXT` 는 AOSP bionic 이 배포하고, OEM 이 그것을 고친 사례를 우리는 알지 못한다. 다만 **모르는 것은 잰 것이 아니다.**
>
> 그래서 다른 OEM 을 **미지원이라고 적지 않는다** — 안 된다고 잰 적이 없기 때문이다. **된다고도 적지 않는다** — 된다고 잰 적도 없기 때문이다. 정확한 진술은 **"Samsung 2대에서 검증했고, 다른 OEM 은 우리가 확인해 주지 못한다"** 이다. 그 기기에서의 판단 재료는 `alr doctor` 의 스윕이며, `scripts/diff-sweep.sh` 로 [`docs/evidence/sweeps/`](evidence/sweeps/) 와 비교하면 표가 그 폰을 설명하는지 사용자가 직접 확인할 수 있다.
>
> 🚫 **Android 릴리스 축은 재지 않기로 했다 — 미해결이 아니라 범위 결정이다**([ADR 0007](adr/0007-android-16-only.md)).
> 이 설계의 근간인 zygote seccomp allowlist 는 **릴리스마다 커졌다**(android12 365줄 → android16 392줄). 두 기기가 갈린 축(SoC 벤더·커널)은 전부 무관했는데 **정작 allowlist 가 실제로 따라가는 축은 고정된 채였다.** 즉 실측 결과는 "SoC 벤더·커널은 상관없다" 의 강한 증거이지 **"Android 12 에서도 될 것" 의 증거가 아니다.** 그리고 그 증거를 만들 계획이 없으므로, 지원한다고 적지 않는다.
>
> **지원 선언은 "Android 16, SoC 벤더 무관 (OEM 은 Samsung 2대만 실측)" 이다.** 범위 밖 기기에서 `alr` 은 거절하지 않지만 `alr doctor` 가 **명시적으로 미지원이라고 말한다** — 그 기기의 스윕 결과를 함께 보여 주므로, 감수하고 쓸지는 사용자가 판단한다. 우리가 검증하지 않았다는 사실이 가려지지는 않는다.
>
> **성능 배수는 기기별이다.** 워밍업 규율을 적용해 **두 기기를 같은 조건으로** 잰 결과([M19 §6.3](evidence/2026-08-03-m19-snapdragon.md)):
>
> | | MediaTek | Snapdragon |
> |---|---|---|
> | `node -e 0` 콜드 vs proot | 6.63× | 5.49× |
> | exec 처리량 | 342 /s (proot 131) | 707 /s (proot 224) |
> | 재작성 총비용 (`git status` 10k) | 38.2 µs | 73.6 µs |
>
> **exec 는 Snapdragon 이 2배 빠르고**(proot 쪽도 함께 오르므로 기기 성능 차이다) **재작성 per-op 은 MediaTek 이 빠르다.** 배수가 MediaTek 에서 더 큰 것은 alr 이 빨라서가 아니라 **proot 가 더 느려서**다. 배수를 인용할 때는 기기를 함께 쓴다.
- **호스트**: Termux **F-Droid / GitHub 빌드** (`targetSdkVersion=28`). Play Store 빌드는 **v1 미지원** (ADR 0005).
- **게스트**: Ubuntu 24.04 (noble) arm64, glibc 2.39, `ports.ubuntu.com` 아카이브. Debian bookworm/trixie는 v1.1 목표.
- **워크로드**: `git`, `node`(및 npm/npx), `bash`, `apt`/`dpkg`, 일반 coreutils/빌드 툴체인.
  `codex` 는 **비목표**다 — 정적 링크라 경로 가상화가 원리적으로 닿지 않는다([ADR 0008](adr/0008-static-guest-binaries-non-goal.md)).

## 3. 목표 (Goals)

| G | 목표 | 측정 방법 |
|---|---|---|
| G1 | 스톡 Ubuntu 24.04 arm64 rootfs가 **패치 없이** 부팅 | `alr run /bin/true` 성공, glibc 재빌드 0회 |
| G2 | `apt install`이 게스트 안에서 동작 | `alr run apt-get install -y git` 성공 |
| G3 | path syscall에 대해 **ptrace 왕복 0회** | `alr bench --trace-count` 가 `path_traps=0` 보고 |
| G4 | `git status`(10k 파일)가 proot-distro 대비 명확히 빠름 | §4의 정직한 목표 배수 |
| G5 | `node`, `npm`이 실사용 가능 | `npm ci` 성공 ([ADR 0008](adr/0008-static-guest-binaries-non-goal.md): codex 는 비목표) |
| G6 | 실패가 **조용하지 않음** | 모든 실패는 안정적 `reason=` 코드로 분류 |

**현재 상태 (2026-08-03, 참조 기기 2대 — MediaTek `uid=10297` / Snapdragon 8 Elite `uid=10447`, 둘 다 `Seccomp=2 untrusted_app_27`)**

| G | 상태 | 근거 |
|---|---|---|
| G1 | **MEASURED 달성** | 스톡 Ubuntu 24.04.4 base, glibc 2.39 무패치 부팅. glibc 재빌드 0회 |
| G2 | **MEASURED 달성** | 아무것도 없는 상태에서 `alr install --with git` **2분 27초**, `git version 2.43.0`. `openssh-client` 포함 전 패키지 `ii` ([M10](evidence/2026-08-02-m10-apt-install-git.md)) |
| G3 | **MEASURED 달성** | 수용 테스트가 매 실행 `path_traps=0 syscall_stops=0` 보고 |
| G4 | **MEASURED 달성** | proot-distro 대비 `git status` **25.8×** (양쪽 git 2.53.0 동일, 워밍업 후 — [M19 §6.1](evidence/2026-08-03-m19-snapdragon.md)). 34.8× 는 git 빌드가 셋 다 달랐던 M8 수치라 인용하지 않는다 |
| G5 | **MEASURED 달성** | `node`/`npm` 은 동적 링크라 경로 가상화가 적용된다 — `npm ci` 실측 proot-distro 대비 **3.12×** ([M12](evidence/2026-08-03-m12-spawn-resolver.md)). codex 는 [ADR 0008](adr/0008-static-guest-binaries-non-goal.md) 로 G5 에서 뺐다(정적 링크, 원리적으로 후킹 불가) |
| G6 | **부분 달성** | 어휘가 이제 **강제된다** — `scripts/check-reasons.sh` 가 방출 코드와 [05 §7](05-provisioning-spec.md) 목록을 **양방향**으로 검사하고 `make check`·CI 에 걸려 있다(현재 32/32 일치). **남은 것**: `alr.c` 의 일부 stderr 실패 경로에 아직 코드가 없다 — 어휘는 지켜지지만 전수는 아니다 |

> ⚠️ **codex 는 비목표다**([ADR 0008](adr/0008-static-guest-binaries-non-goal.md)). `--with codex` 로 설치·실행은 되지만 **경로 가상화 없이 돈다.** 아래는 그 근거이며, `alr` 은 정적 ELF 를 실행할 때 한 줄로 경고한다.
>
> **`codex --version` 이 통과한다는 것을 "codex 가 게스트 안에서 동작한다" 로 읽지 말 것.** 배포되는 codex 바이너리는 `INTERP` 프로그램 헤더도 `NEEDED` 항목도 없는 **정적 링크**(269 MB, ET_EXEC)다. `LD_PRELOAD` 는 원리적으로 닿지 않으므로 codex 의 모든 경로 연산은 rootfs 가 아니라 Android 파일시스템으로 간다. 실제로 시작할 때마다 `could not create PATH aliases: Read-only file system` 을 낸다 — 그 경로가 Android 의 읽기 전용 루트로 샌 증거다. codex 는 그 실패를 치명적으로 다루지 않아 계속 진행할 뿐이다.
>
> 이것은 [RISKS](RISKS.md) 의 "정적 링크 게스트 바이너리" 한계에 해당하며, `node`/`npm` 에는 적용되지 않는다(둘 다 동적 링크라 정상적으로 가상화된다). 수용 테스트가 `ALR CODEX LINKAGE` 로 이 상태를 추적한다.

수용 테스트: **PASS=164 FAIL=0 KNOWN_FAIL=1 SKIP=0**. 호스트 게이트 **10/10**([M13](evidence/2026-08-03-m13-symbol-gate.md) 에서 `wrappers.def` + 심볼 존재 게이트 추가 — 즉시 누락 심볼 24개를 찾아냈다). 남은 `KNOWN_FAIL` 은 `/dev/full` **하나뿐**이며 미구현이 아니라 **의도된 비목표**다([RISKS](RISKS.md)). 즉 수용 시험의 `KNOWN_FAIL` 은 이제 전부 "안 할 일"이고 "못 한 일"은 0이다 — codex 정적 링크는 [ADR 0008](adr/0008-static-guest-binaries-non-goal.md) 로 비목표가 되어 관찰 라인(`ALR CODEX LINKAGE`)으로 남는다.

**호환성 폭 (§4 포지셔닝의 근거): 큐레이션된 96개 Ubuntu noble 패키지 중 설치 96/96, 실행 96/96** ([M11](evidence/2026-08-02-m11-breadth.md), [M14](evidence/2026-08-03-m14-ioctl-php.md)).

> ⚠️ `php-cli` 는 출하 빌드에서 동작하지만 **원인을 규명하고 고친 것이 아니다.** abort 여부가 preload 의 심볼 테이블 크기에 민감하다는 것이 실측되었고(경계 ~152 심볼), php 가 쓸 리 없는 심볼 하나만 빼도 재발한다. 심볼을 덜어내는 변경이 php 를 다시 깨뜨릴 수 있으며 그때 범인은 그 변경이 아니다. 자세한 내용과 진단용 컴파일 가드는 [M14 §2](evidence/2026-08-03-m14-ioctl-php.md).

> 이 96/96 을 인용할 때의 정직한 표현: "빌드 툴체인·언어 런타임·CLI 유틸을 아우르는 **큐레이션된 96개** 패키지에서 무수정 설치 96/96, 실행 96/96 — 두 기기(MediaTek·Snapdragon), Android 16." 아카이브 전체(수만 개)에 대한 주장이 아니다.
>
> **`실행 96/96` 은 2026-08-04 에 다시 쟀다.** 그전까지 이 스위트의 아홉 줄이 `... | head -1` 로 끝나는 파이프라인이었고 `pipefail` 이 없어서 **종료 상태가 `head` 의 것**이었다 — 왼쪽이 어떻게 죽든 0 이다. 즉 그 아홉 줄은 실패를 볼 수 없었다. 게스트 셸을 `bash -o pipefail` 로 바꾸고 다시 돌린 결과가 여전히 96/96 이다: **아홉 줄 다 진짜로 통과하고 있었다.** 숫자는 그대로고 근거가 생겼다.

## 4. 성능 목표 — 정직하게 쓸 것

검증 결과 기존 통념 여러 개가 반박되었다. 마케팅과 문서는 **아래 숫자만** 쓴다.

**반박된 주장 (쓰지 말 것)**

- ❌ "PRoot는 모든 syscall에 ptrace를 건다" — 틀렸다. PRoot는 필터 테이블의 ~100개 syscall(경로 관련 + 프로세스 라이프사이클)만 트랩한다. `read`/`write`/`futex`는 `RET_ALLOW` 속도로 지나간다.
- ❌ "10배 빠르다" — 어떤 증거로도 뒷받침되지 않는다.
- ❌ "grun보다 빠르다" — `grun`은 동등하거나 미세하게 빠르다. **속도로 grun과 정면 비교하지 말 것.**

**방어 가능한 주장 (이것만 쓸 것)**

| 워크로드 | vs proot-distro 목표 | 근거 |
|---|---|---|
| `git status` (10k 파일) | ~~1.5–4×~~ → **실측 34.8×** | [M8 실측](evidence/2026-08-02-m7-m8-workloads-perf.md). 아래 주석 필독 |
| `npm ci` | ~~1.5–3×~~ → **실측 3.12×** | [M12 실측](evidence/2026-08-03-m12-spawn-resolver.md). 동일 node 바이너리·락파일·캐시로 잰 값이라 M8 의 git 비교보다 조건이 깨끗하다 |
| `node -e 0` 콜드 스타트 | ~~1.05–1.4×~~ → **실측 6.60×** | [M17 실측](evidence/2026-08-03-m17-bench-ab.md). 동일 node 바이너리. **추정이 크게 빗나갔다** — 아래 주석 |
| exec 처리량 | 실측 **351 exec/s** (proot 135) | [M17](evidence/2026-08-03-m17-bench-ab.md). 동일 바이너리를 양쪽에 복사해 측정 |
| 로그인/세션 진입 | 1회 **30–100 ms** 절약 | proot-distro 셸 진입 오버헤드 |
| 종합 (UnixBench류) | **상한 1.8× — 목표가 아니다.** 실측은 `PENDING_DEVICE` | 아래 주석 참조 |

> ⚠️ **1.8×를 목표로 인용하지 말 것.** 공개된 0.56×는 *proot ÷ native* 비율이므로 1/0.56 = 1.79는 *native ÷ proot*다. 이것을 alr의 목표로 쓰면 **alr = native, 즉 alr 오버헤드 0을 암묵적으로 주장**하게 되는데, 우리 문서가 스스로 반박한다: execve마다 DSO가 하나 더 매핑·재배치되고 전역 심볼 스코프가 커지며([§D2](01-platform-facts.md)), auditallow 로그가 프로세스당 ~40 레코드씩 쌓인다([RISKS R5, R6](RISKS.md) — PRoot는 게스트 바이너리를 직접 exec하지 않아 이 비용이 없다). UnixBench류 종합 지수는 process creation과 execl 처리량이 지배하는데, 그것이 정확히 alr 고유 비용이 얹히는 곳이다. 실제 값은 반드시 1.8×보다 낮다.

> ⚠️ **2026-08-02 실측이 이 표의 추정을 뒤집었다.** 동일 기기·동일 워크로드에서 `git status` **34.8×**, 프로세스 기동 **10.9×** 빠르다(proot-distro 대비). 1.5–4× 추정은 과소평가였다.
>
> **그래도 34.8×를 헤드라인으로 쓰지 말 것.** 세 실행의 git 빌드가 다르고(2.55/2.43/2.53), proot-distro는 자체 rootfs를 쓰며, 기기는 MediaTek이고, 1회 세션 측정이다. **권장 표현**: "동일 기기·동일 워크로드에서 proot-distro 대비 git status 30배 이상, 프로세스 기동 10배 이상 — 단일 MediaTek 기기 1회 세션, git 빌드 상이."

> ⚠️ **이 표의 추정 4개 중 3개가 실측에서 빗나갔고, 전부 같은 방향이다** — `git status` 1.5–4× → 34.8×, `npm ci` 1.5–3× → 3.12×(유일하게 맞음), `node` 콜드 스타트 1.05–1.4× → **6.60×**. 특히 콜드 스타트는 "여기가 가장 약하다" 고 적어 둔 항목인데 6배가 넘게 나왔다.
>
> **그래도 콜드 스타트를 히어로로 승격하지 않는다.** 추정이 틀렸다는 것은 우리가 proot 의 syscall 당 비용을 과소평가했다는 뜻이지, 이 워크로드가 alr 의 강점이라는 뜻이 아니다. `node -e 0` 은 55 ms 중 대부분이 V8 초기화이고 alr 이 기여하는 부분은 작다. 숫자가 커진 이유는 분모(proot 363 ms)가 커서다.
>
> 이 표의 추정치들은 이제 **실측으로 대체되었을 때만** 인용 가치가 있다. 남은 추정("로그인/세션 진입 30–100 ms", "UnixBench 상한 1.8×")도 같은 정도로 신뢰할 이유가 없다.

**히어로 벤치마크는 `git status`와 `npm ci`로 고정한다.** `node` 콜드 스타트는 배수가 크게 나왔음에도 디엠퍼사이즈한다 — 위 주석의 이유 때문이다.

**포지셔닝 문장 (승인된 표현):**

> grun은 빠르지만 포크된 glibc와 포크된 패키지 세트를 요구한다. proot-distro는 Ubuntu 아카이브 전체를 쓰지만 path syscall마다 ptrace 비용을 낸다. **alr은 둘 다 얻는 첫 시도다.**

이 포지셔닝이 성립하려면 **호환성 폭(breadth)을 숫자로 방어**해야 한다 ([07-acceptance.md §5](07-acceptance.md)). **측정 완료**: 큐레이션된 96개 패키지에서 설치 96/96, 실행 96/96 ([M11](evidence/2026-08-02-m11-breadth.md), `pipefail` 로 재측정 2026-08-04). 인용 시 §3의 정직한 표현을 쓸 것 — 아카이브 전체에 대한 주장이 아니다.

## 5. 비목표 (Non-goals)

- **GUI / X11 / Wayland / GPU 가속.** 상위 프로젝트(android-on-linux)의 영역. `alr`은 CLI 전용이다.
- **Play Store Termux 지원** (ADR 0005).
- **정적 링크 게스트 바이너리** ([ADR 0008](adr/0008-static-guest-binaries-non-goal.md)). `LD_PRELOAD` 가 닿을 동적 심볼이 없다 — codex 가 대표 사례다. 실행은 막지 않되 경로 가상화는 붙지 않는다.
- **Android 12~15 지원** ([ADR 0007](adr/0007-android-16-only.md)). 검증하지 않는다 — bionic allowlist 는 릴리스마다 커지므로 Android 16 실측을 구버전으로 외삽할 수 없고, 외삽하지 않기로 했다.
- **보안 격리.** `alr`은 샌드박스가 **아니다**. 경로 재작성은 방어 경계가 아니다.
- **x86 에뮬레이션** (box64/FEX). 네이티브 arm64만.
- **systemd / init / 서비스 관리.**
- **setuid 의미론.** `/data`는 `nosuid`이며 setuid/setgid는 seccomp로 막혀 있다. `sudo`류는 동작하지 않는다.
- **libc를 우회해 raw `svc`를 발행하는 게스트 바이너리.** Go가 대표적이고, **Ubuntu 26.04가 기본 coreutils로 채택한 uutils(Rust)도 여기 해당한다**(74개 inline `svc` 확인). `LD_PRELOAD`는 libc를 거치는 호출만 보므로 닿지 않는다.

  > **정정.** 이전에 "가로챌 방법이 원리적으로 없다"고 적었는데 틀렸다. seccomp user notification은 **동작한다** — 스택된 필터에서 `USER_NOTIF`(0x7fc00000)가 `ALLOW`(0x7fff0000)보다 우선하고, 앞서 본 `EPERM`은 정책이 아니라 `no_new_privs`가 0이어서였다. 비목표인 진짜 이유는 **비용**이다: 알림 왕복 실측 **154 µs/syscall**(베이스라인의 352배, 그것도 하한). 값싼 대안인 `PR_SET_SYSCALL_USER_DISPATCH`는 이 커널(arm64 6.1)에 없다. 근거와 남은 선택지는 [ADR 0006](adr/0006-raw-syscall-binaries.md).

  따라서 **Ubuntu 26.04는 v1 대상이 아니다**: 나머지는 다 동작하지만 coreutils가 통째로 못 쓴다. `alr install`이 경고한다.
- **`/dev/full` 에뮬레이션.** 서빙하려면 프로세스에서 가장 뜨거운 syscall 인 `write()` 를 인터포즈해야 하는데 대상 워크로드 중 쓰는 것이 없고, 빠뜨린 stdio 심볼은 전부 **조용히 성공한 쓰기**가 된다 — 열거 가능하고 요란하게 실패하는 다른 인터포지션과 성격이 다르다. 근거는 [RISKS](RISKS.md).

## 6. 정직성 규칙 (프로젝트 헌장)

상위 프로젝트에서 그대로 가져오는 규칙이다. Codex는 이것을 어기면 안 된다.

1. **MEASURED와 MODELED를 절대 섞지 말 것.** 디바이스에서 잰 숫자만 `MEASURED`. 그 외 전부 `MODELED` 또는 `PENDING_DEVICE`.
2. **속도 향상을 지어내지 말 것.** A/B 실측 전에는 `relative_to_proot=PENDING_DEVICE`.
3. **실패를 PASS로 바꾸지 말 것.** `PASS` / `FAIL` / `SKIP` / `KNOWN_FAIL:<reason>` / `PENDING_DEVICE` 다섯 가지만 쓴다 ([07-acceptance.md §1](07-acceptance.md)에 각각의 의미). `PENDING_DEVICE`는 PASS도 FAIL도 아니며 pass-rate 분모에서 제외하지만, 마일스톤 `Exit`에 남아 있으면 그 마일스톤은 미완이다.
4. **미검증 항목은 UNVERIFIED로 표기.** 추측 금지.
5. **클린룸 유지.** PRoot·termux-exec의 *동작*은 참고하되 소스를 복사하지 않는다. 알고리즘을 재설명하고 직접 구현한다. (라이선스 검토가 끝나기 전까지.)
6. **루팅된/permissive 디바이스로 검증하지 말 것.** `getenforce == Enforcing` 이고 `/proc/self/status`의 `Seccomp: 2`인 디바이스만 유효한 증거다. permissive 디바이스에서는 zygote seccomp 필터가 아예 설치되지 않아 모든 문제가 사라져 보인다.

## 7. 이름과 네임스페이스

- CLI 바이너리: `alr`
- 환경변수 접두사: `ALR_`  (상위 프로젝트와 동일 — 문서/툴 재사용을 위해 의도적으로 유지)
- 게스트 내 설치 경로: `/usr/lib/alr/`
- 호스트 데이터 루트: `$PREFIX/var/lib/alr/`
