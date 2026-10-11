# ADR 0005: Termux 우선 목표 (최신 LTS, 네이티브 대비 1.2배, 확장 APK GUI, raw-syscall 포함)

## Status

Accepted (2026-10-11). 목표와 우선순위는 이 문서로 정하고, 각 목표의 달성은 기기 증거로만 닫는다.

이 ADR이 바꾸는 것:

- [ADR 0004](0004-unified-direction.md) D4의 표면 우선순위를 바꾼다. S2(Termux CLI)가 주 제품이 되고, S1(독립 APK)은 중단하며, S3(Termux 연결 GUI)는 후보가 아니라 GUI의 유일한 경로가 된다. ADR 0004의 Phase 계획(P1~P4)은 아래 우선순위로 대체한다. D1(엔진은 alr), D3(계층과 계약), D5(그래픽은 공개 API), D8(문서 규율)은 그대로 유지한다.
- [runtime/alr ADR 0006](../../runtime/alr/docs/adr/0006-raw-syscall-binaries.md)과 [ADR 0008](../../runtime/alr/docs/adr/0008-static-guest-binaries-non-goal.md)의 비목표를 목표로 바꾼다. 두 ADR의 결정은 로더 프로토타입이 아래 T5의 완료 기준을 채우고 별도 ADR이 나올 때 대체된다. 그 전까지 제품의 동작은 바뀌지 않는다.
- [runtime/alr 00-product](../../runtime/alr/docs/00-product.md)의 게스트를 Ubuntu 24.04에서 "최신 LTS"로 바꾼다.

## Context

### 1. 결정한 사람의 요구

2026-10-11 기준 요구는 다음과 같다.

- Termux 안에서 Linux CLI와 GUI를 네이티브급 성능으로 실행한다.
- Termux만으로 할 수 없는 부분은 동반 APK(확장)를 쓴다.
- 게스트는 최신 Ubuntu LTS다.
- Termux와 무관하게 동작하는 독립 APK는 중단한다.

### 2. 증거로 확인된 현재 상태 (Snapdragon SM-S937N, Android 17, `untrusted_app_27`, `Seccomp=2`, `engine=alr`)

| 항목 | 결과 | 근거 |
|---|---|---|
| zygote 차단 syscall 집합 | Android 16과 동일(239 = 239) | `alr doctor` 스윕, `scripts/diff-sweep.sh` |
| Ubuntu 24.04.4 수용 테스트(지원 중단 전 마지막 실행) | PASS 163, FAIL 2(codex 출력 형식), KNOWN_FAIL 1, SKIP 1 | [T1 증거](../../runtime/alr/docs/evidence/2026-10-11-t1-ubuntu-2604-android17.md) |
| Ubuntu 26.04.1 LTS 설치 | 다운로드, SHA256, 추출, uutils를 GNU coreutils로 재지정, 부팅 모두 PASS (glibc 2.43) | `alr install ubuntu-26.04` |
| Ubuntu 26.04.1 수용 테스트 | PASS 169, FAIL 0, KNOWN_FAIL 1(`/dev/full`, 비목표), SKIP 0. glibc 2.42의 termios 변경과 ucf의 환경 정리로 생긴 문제는 preload에서 고쳤다. 호환성 폭 96/96 | 같은 증거 |
| `RET_TRAP` 왕복 비용 | +410~+925 ns, D6 판정 `PROCEED` | [S2 증거](../../runtime/alr/docs/evidence/2026-10-11-s2-ret-trap-cost.md) |
| `git status` 10k 파일 | alr 36 ms, 네이티브 33 ms(Termux의 다른 git 빌드라 근사치) | [M19 §6](../../runtime/alr/docs/evidence/2026-08-03-m19-snapdragon.md) (Android 16 시점) |

GUI와 GPU에 대해서는 Termux 경로의 증거가 없다. 지금까지의 GPU 증거(glmark2 1074, Vulkan, zero-copy)는 모두 독립 APK, Mali-G615, 레거시 로더 시기의 것이다(`engine=legacy-loader`). Adreno에서는 아무것도 측정하지 않았다.

## Decision

### 목표

| | 목표 | 완료 기준 | 현재 |
|---|---|---|---|
| **T1** | 최신 Ubuntu LTS가 무패치로 부팅하고 apt가 동작한다 | `alr install`의 기본값이 최신 LTS이고, 그 rootfs에서 수용 테스트가 `FAIL=0`, `SKIP`은 비목표 항목만 남는다 | 달성(2026-10-11). 26.04.1에서 `FAIL=0 SKIP=0` |
| **T2** | CLI가 네이티브 대비 1.2배 이내로 동작한다 | 아래 워크로드 각각에서 `alr 시간 / 네이티브 시간 <= 1.2`, 같은 기기, 워밍업 후 중앙값 | `git status`만 근사치 1.09배. 나머지는 네이티브 측정 없음 |
| **T3** | Termux에서 띄운 GUI 앱이 자체 확장 APK 화면에 표시된다 | Termux의 게스트가 만든 `wl_shm` 프레임이 확장 APK의 Surface에 표시되고, GTK 앱 하나가 입력을 받아 동작한다 | 없음 |
| **T4** | GUI 가속이 네이티브 대비 1.2배 이내로 동작한다 | 같은 GL 워크로드를 Android 네이티브 앱으로 돌린 값 대비 1.2배 이내 | 없음. Adreno 미측정 |
| **T5** | raw-syscall 바이너리도 경로 가상화된다 | 26.04의 uutils coreutils가 GNU 재지정 없이 동작하고, Go 바이너리 하나와 codex가 rootfs를 `/`로 본다. 경로 syscall당 추가 비용이 T2 예산 안에 든다 | 메커니즘 비용만 측정(D6 `PROCEED`) |

T2의 워크로드는 `git status`(10k 파일), `npm ci`, `node -e 0` 콜드 스타트, `apt-get install` 한 건이다. 네이티브 쪽은 Termux 패키지의 같은 프로그램이며, 버전이 다르면 그 사실을 결과에 함께 적는다. glibc 게스트 바이너리를 Android에서 직접 실행할 수는 없으므로 같은 바이너리 비교는 불가능하다.

### 우선순위

1. **T1 마무리.** 기본 배포판을 `ubuntu-26.04`로 바꾸고 24.04 지원을 끊는다(2026-10-11 완료). 남은 FAIL을 닫는다(2026-10-11 완료).
2. **T5.** tawcroot의 로더(`loader_*`), execve 재진입(`exec_handler.c`), 시그널 가상화(`signal_shadow.c`)를 기반으로 프로토타입을 만든다. 첫 번째로 확인할 것은 alr의 ptrace 슈퍼바이저(zygote 필터의 `SIGSYS` 구제)와 tawcroot의 `SIGSYS` 핸들러가 한 프로세스에서 공존할 수 있는가다.
3. **T3.** 독립 APK의 Wayland 컴포지터를 떼어 내 확장 APK로 만든다. 서로 다른 uid 사이의 소켓 문제는 Termux:X11처럼 `app_process`로 같은 uid에서 서버를 띄우는 방식을 먼저 시도한다(ADR 0004 D4의 S3 구상).
4. **T4.** 확장 APK에 GPU 실행기를 붙이고 Adreno에서 처음부터 측정한다.

T2는 순서가 아니라 관문이다. T1과 T5의 변경마다 측정하고, 1.2배를 넘기는 변경은 원인을 기록한다.

### 게스트 릴리스 정책

- **지원하는 게스트는 최신 Ubuntu LTS 하나다.** 현재는 26.04이고, `alr install`이 SHA256SUMS에서 가장 높은 포인트 릴리스를 고르므로 26.04.1이 설치된다.
- 이전 LTS(24.04)는 지원하지 않는다. 이름을 지정하면 실행은 되지만 검증하지 않고, 배포판을 지정하지 않은 명령은 이전 트리를 고르지 않는다. 다음 LTS가 나오면 같은 방식으로 26.04의 지원을 끊는다.
- 중간 릴리스(26.10 등)와 beta는 지원하지 않는다. 사용자가 이름을 지정해 설치하는 것은 막지 않는다.
- SHA256SUMS에 닿지 못할 때 해시 없이 받는 폴백은 두지 않는다.

### 표면

| 표면 | 결정 |
|---|---|
| Termux CLI (`alr`) | 주 제품 |
| 확장 APK | GUI 표시와 GPU를 맡는 동반 앱. Termux 없이 단독으로 쓰는 기능은 넣지 않는다 |
| 독립 APK (`dev.chanwoo.androlinux`) | 중단. 레거시 로더처럼 동결하고, 컴포지터와 GPU 실행기 코드는 확장 APK의 재료로 남긴다. 삭제는 T3이 끝난 뒤 별도로 판단한다 |

## Alternatives considered

**A. GUI는 Termux:X11을 쓴다.** 기각한다. 표시는 빠르게 얻지만 GPU 경로(T4)를 통제할 수 없고, 결국 자체 확장이 필요해진다. 두 경로를 유지하는 비용을 피한다.

**B. 최신 릴리스(중간 릴리스 포함)를 따라간다.** 기각한다. 9개월 지원 주기마다 기본값이 바뀌면 수용 테스트와 증거를 그만큼 자주 다시 만들어야 한다. LTS 사이에서만 바꾼다.

**C. raw-syscall 바이너리를 비목표로 유지한다.** 기각한다. D6 측정으로 비용 근거가 사라졌고, 26.04의 uutils, Go CLI, codex가 모두 이 범주에 들어간다. GNU coreutils 재지정은 Ubuntu가 그 선택지를 남겨 두는 동안만 쓸 수 있는 우회다.

**D. 독립 APK를 계속 개발한다.** 기각한다. 같은 엔진을 두 표면에서 검증하는 비용을 Termux 쪽 목표에 쓴다.

## Consequences

- 이 ADR은 tawcroot를 제품에 링크하는 것을 허가하지 않는다. 프로토타입은 만들 수 있지만, 출하는 T5 완료 기준을 채운 뒤 별도 ADR로 결정한다([VENDORING.md](../../runtime/alr/third_party/tawcroot/VENDORING.md) 규칙 2).
- `targetSdk 28` 의존은 Termux 쪽에서도 그대로다. Termux가 `targetSdk`를 올리면 `alr`은 동작하지 않는다([runtime/alr ADR 0005](../../runtime/alr/docs/adr/0005-play-store-unsupported.md)).
- T3의 확장 APK도 `targetSdk 28`이어야 하는지는 아직 모른다. 컴포지터만 맡는다면 게스트 실행이 없으므로 필요 없을 가능성이 높고, T3에서 확인한다.
- runtime/alr 문서의 "게스트: Ubuntu 24.04" 서술은 T1과 함께 고친다.

## 정직한 한계

- 위 측정은 Samsung Snapdragon 한 대, Android 17에서만 했다. MediaTek 기기는 가끔 쓸 수 있으므로 T1과 T5의 결과를 그 기기에서도 확인한다.
- T2의 네이티브 비교는 같은 바이너리 비교가 아니다.
- T3, T4, T5는 모두 미검증이다.
