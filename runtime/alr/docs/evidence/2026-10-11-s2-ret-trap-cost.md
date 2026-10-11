# S2 — 프로세스 내부 `SECCOMP_RET_TRAP` 왕복 비용: 0.4~0.9 µs, D6 판정 `PROCEED`

- **날짜**: 2026-10-11
- **엔진 라벨**: 해당 없음. alr 엔진도 레거시 로더도 거치지 않은 독립 마이크로벤치다(`engine=none`, 메커니즘 단독 측정).
- **기기**: Samsung SM-S937N (Galaxy S25 Edge), Snapdragon 8 Elite SM8750 (`ro.board.platform=sun`). [M19](2026-08-03-m19-snapdragon.md)의 참조 #2와 같은 기기다.
- **OS**: **Android 17 (SDK 37)**, 커널 `6.6.127-android15-8-p33f4ffe-abogkiS937NKSUADZIF-4k`. M19 시점(Android 16, 커널 6.6.98) 이후 OS가 올라갔다. 아래 §4를 본다.
- **컨텍스트**: `uid=10057  u:r:untrusted_app_27:s0:c57,c256,c512,c768  Seccomp=2  Seccomp_filters=1` (Termux 0.118.3, `targetSdk 28`, Termux 앱에서 fork된 셸)
- **도구**: Termux `clang 21.1.8`, `-O1 -Wall -Wextra -std=c11 -D_GNU_SOURCE` ([`scripts/dev-push.sh`](../../scripts/dev-push.sh)의 `CFLAGS_DEV`와 같다)
- **소스**: [`bench/microbench/trap_cost.c`](../../bench/microbench/trap_cost.c), sha256 `46748ccd5235ac80116c7c5ecca1c1ec3a458e3b8ac10e497349f74e682705a8`(커밋 `91e9de6`의 파일과 일치)
- **원본 출력**: [2026-10-11-s2-ret-trap-cost.txt](2026-10-11-s2-ret-trap-cost.txt)
- **목적**: [ADR 0004 D6](https://github.com/Meapri/android-on-linux/blob/main/docs/adr/0004-unified-direction.md)의 판정표를 한 행으로 닫는다.

## 0. 측정 자격

`uid>=10000 ∧ Seccomp=2`를 벤치가 직접 읽어 출력하고, 이 조건이 아니면 판정을 출력하지 않는다. 10회 실행 모두 조건을 만족했다. `Seccomp_filters=1`은 우리 필터를 설치하기 전에 읽은 값이므로 zygote 필터 하나다. 측정은 zygote 필터 위에 우리 필터를 쌓은 상태에서 이루어졌다.

`dev-push.sh trap-cost`는 Termux에 sshd가 떠 있지 않아 쓰지 못했다. 같은 소스를 `adb push`로 올리고, Termux 셸에서 직접 빌드하고 실행했다. 실행 스크립트는 고정 없이 1회, CPU 0(최대 3.53 GHz)에 3회, CPU 6(최대 4.47 GHz)에 3회, 다시 고정 없이 2회를 순서대로 돌렸다.

## 1. 결과

각 값은 20,000회 호출을 9라운드 돌린 중앙값이다(ns/호출).

| 실행 | 기준선 | 필터, 트램폴린 ALLOW | TRAP, 직접 응답 | TRAP, 재발행 | **D6 비용**(재발행 - 기준선) |
|---|---:|---:|---:|---:|---:|
| 고정 없음 1 | 254 | 159 | 544 | 664 | **+410** |
| 고정 없음 2 | 172 | 117 | 542 | 677 | **+505** |
| 고정 없음 3 | 172 | 129 | 540 | 677 | **+505** |
| CPU 0, 1 | 225 | 262 | 1,008 | 1,098 | **+873** |
| CPU 0, 2 | 173 | 205 | 877 | 1,094 | **+921** |
| CPU 0, 3 | 173 | 206 | 877 | 1,098 | **+925** |
| CPU 6, 1 | 142 | 118 | 533 | 666 | **+524** |
| CPU 6, 2 | 193 | 118 | 541 | 676 | **+483** |
| CPU 6, 3 | 144 | 118 | 541 | 677 | **+533** |

**10회 모두 `PROCEED`(5 µs 이하)다.** 가장 나쁜 중앙값은 CPU 0의 +925 ns이고, 모든 라운드를 통틀어 재발행 경로의 최댓값도 1,259 ns다. 판정 경계(5,000 ns)까지 4배 이상 여유가 있다.

## 2. 읽는 법

- **D6 판정은 `PROCEED`다.** 기준선 측정이 흔들려도 결론은 같다. 재발행 경로의 절대값(최대 1.1 µs)만으로도 5 µs 아래에 있기 때문이다.
- **비교**: 같은 syscall에서 `USER_NOTIF`는 154,270 ns였다([ADR 0006](../adr/0006-raw-syscall-binaries.md)). 프로세스 내부 `RET_TRAP`은 그보다 약 170~380배 싸다. 경로 syscall 3만 회에 곱하면 15~28 ms이고, proot-distro의 `git status`(930 ms) 대비 무시할 수 있는 크기다.
- **기준선이 필터 경로보다 느리게 나온 실행이 있다**(필터 평가 비용이 음수). 기준선을 프로세스 시작 직후 가장 먼저 재기 때문에 CPU 주파수가 오르기 전 값이 섞인 것으로 보인다. 기준선의 최소-최대 폭(104~483 ns)이 다른 항목(수 ns~수십 ns)보다 훨씬 넓은 것이 그 근거다. 이 때문에 D6 비용은 실제보다 조금 크게 잡혔을 가능성이 있고, 판정에는 보수적인 방향이다.
- **CPU 0의 시그널 경로가 CPU 6보다 약 1.7배 느리다**(재발행 1,098 ns 대 677 ns). 고정하지 않은 실행은 CPU 6 쪽 값과 같으므로 스케줄러가 빠른 코어에 올린 것으로 보인다. 판정은 느린 코어 기준으로도 같다.

## 3. 이 측정이 닫지 않는 것

이 값은 **메커니즘 단독 비용**이다. 실제 레인에서는 아래 비용이 더해지며, 모두 `PENDING_DEVICE`다.

- 경로 변환(rootfs 기준 재작성). 호스트 x86_64에서 tawcroot 전체 경로의 `stat`은 약 4.6 µs였다([VENDORING.md](../../third_party/tawcroot/VENDORING.md)). 기기 값은 tawcroot를 bionic으로 빌드해야 얻을 수 있다.
- `sigaltstack`, Go 런타임의 `rt_sigaction` 가상화, 스레드 전체 적용(`TSYNC`).
- 정적 바이너리에 핸들러를 먼저 설치하는 사용자 영역 로더.
- **MediaTek 참조 기기(SM-X236N)는 재지 않았다.** 이 판정은 Snapdragon 한 대의 결과다.

D6 판정표에 따라 다음 단계는 로더 프로토타입이다. 제품에 넣는 결정은 별도 ADR로 한다([VENDORING.md](../../third_party/tawcroot/VENDORING.md) 규칙 2).

## 4. 부수 관찰: 이 기기는 Android 17이다

- M19 때 Android 16이던 참조 #2가 Android 17(SDK 37)로 올라가 있었다.
- `targetSdk 28`인 Termux는 OS 업그레이드 뒤에도 설치된 상태로 실행되고, 셸이 `untrusted_app_27` 도메인과 zygote seccomp 필터를 그대로 가진다. [ADR 0004 D2](https://github.com/Meapri/android-on-linux/blob/main/docs/adr/0004-unified-direction.md)의 감시 항목에 대한 관찰 하나다. 새로 설치하는 경우는 확인하지 않았다.
- [ADR 0007](../adr/0007-android-16-only.md)은 지원 범위를 Android 16으로 한정한다. 참조 기기가 범위 밖 릴리스가 되었으므로 차단 집합 스윕(`alr doctor`)과 수용 테스트를 Android 17에서 다시 돌려야 한다. 이 문서는 그 결과를 담지 않는다.
