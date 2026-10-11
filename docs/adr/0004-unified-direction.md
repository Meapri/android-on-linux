# ADR 0004: 통합 방향 (단일 엔진, 계층 분리, 배포 표면 3종)

## Status

Accepted (2026-10-11). 결정 자체는 확정이고, 아래 Phase 계획의 각 항목은 Proposed 상태이며 기기 증거로만 닫는다.

기준 시점: HEAD `23dcd51`, `versionName 0.4.163-sd-v163`, `targetSdk 28`.

이 ADR이 바꾸는 것:

- [ADR 0001](0001-runtime-model.md)의 백엔드 순서(PRoot baseline → ALR launcher → LD_PRELOAD → clean-room syscall)를 대체한다. 나머지 결정(no-root, no-VM, APK 네이티브 진입점)은 유지한다.
- [ADR 0003](0003-cpu-syscall-mediation-overhead.md)의 범위를 "레거시 in-process 로더"로 한정한다. 주 엔진의 결정은 `runtime/alr/docs/adr/`에 있다.
- [ADR 0002](0002-gpu-public-api.md)는 그대로 유지한다.

## Context

### 1. 코드 기준의 현재 상태

이 저장소에는 실행 엔진이 두 개 있고, 상위 문서 대부분은 그중 하나만 설명한다.

| | alr 엔진 (`runtime/alr`) | 레거시 in-process 로더 (`app/src/main/cpp/runtime_report.cpp` 등) |
|---|---|---|
| 게스트 실행 | 커널 `execve`로 게스트 `ld.so`를 명시 호출(`--preload`) | 게스트 `ld.so`를 앱 프로세스에 직접 매핑하고 진입점으로 점프 |
| 경로 가상화 | `LD_PRELOAD` 문자열 접두사 재작성(트랩 0회) | 같은 interposer + PCGATE(PC 조건부 seccomp) + ptrace 중재 |
| Android seccomp 대응 | 신호 전용 ptrace 슈퍼바이저가 `SIGSYS`만 구제 | 자체 seccomp 필터와 ptrace |
| 요구 조건 | `targetSdk 28`(SELinux 도메인 `untrusted_app_27`) | `targetSdk` 무관 |
| 현재 역할 | **제품 경로.** `NativeAppSession`은 alr로 채택된 트리를 만나면 이 경로를 쓴다 | alr로 채택되지 않은 번들 트리, 연구 프로브(JIT W^X, Chromium CP-6) |

`app/build.gradle.kts`의 `buildAlrRuntime` 작업은 `runtime/alr`를 그대로 빌드해 APK에 `libalr.so`와 `libalr_preload.so`로 넣는다. 그러므로 Termux용 CLI(`alr install/run/shell`)와 APK는 같은 엔진 소스를 쓴다. 이 사실이 이번 통합의 출발점이다.

PRoot는 제거되었다([THIRD-PARTY](../THIRD-PARTY.md), `docs/evidence/v111-proot-removed.txt`).

### 2. 문서가 코드보다 뒤처진 지점

- `README.md`, `docs/architecture.md`, `docs/product-requirements.md`, `docs/architecture/execution-backend.md`는 "PRoot baseline이 fallback으로 남는다"는 전제를 유지하고 있었다.
- `docs/architecture/gpu-display-bridge.md`, `docs/android-graphics-bridge-spec.md`는 "Wayland-shaped ingress", "text command TCP" 단계에서 멈춰 있었다. 실제로는 vendored libwayland 기반 컴포지터, 공유 메모리 링, `AHardwareBuffer` zero-copy present가 기기에서 동작한다([PERFORMANCE](../PERFORMANCE.md)).
- `runtime/alr/README.md`는 상위 프로젝트가 `targetSdk 35`와 in-process 리맵에 묶여 있다고 적었지만, 현재 APK는 `targetSdk 28`로 alr 엔진을 쓴다.
- `docs/PERFORMANCE.md`의 CPU 수치(compute +0.00%, syscall +11.98%)는 레거시 로더 시기의 측정이다. alr 엔진의 수치는 `runtime/alr/README.md`에 따로 있다. 두 엔진의 수치가 한 표처럼 읽히는 상태였다.

### 3. 증거 공백

GPU 링, glmark2(Score 1074), Vulkan ICD, zero-copy present, Chromium CR-1 증거는 모두 2026-06 레거시 로더 시기의 것이다(`PCGATE on, interpose on`). alr 엔진 위에서의 GUI는 GIMP, gpicview, MATE 계산기 등이 `sd` 빌드로 확인되어 있으나(`docs/evidence/v101` 이후), **alr 엔진에서 GPU 링과 glmark2를 다시 측정한 증거는 없다.** 게스트 쪽 shim은 alr 엔진에서 `LD_LIBRARY_PATH`와 `LD_PRELOAD`를 직접 줄 수 없으므로(alr 예약 변수) 라이브러리 배치 방식이 달라질 수 있다. 엔진과 그래픽 계층이 느슨하게 결합되어 있다는 설계 의도는 기기 증거로 아직 확인되지 않았다.

### 4. 외부 비교에서 얻은 것

| 프로젝트 | 접근 | 이 저장소에 주는 시사점 |
|---|---|---|
| Termux + proot-distro | ptrace로 경로 syscall 중재 | alr 엔진이 이미 대체(`git status` 10k 파일에서 proot-distro 930 ms 대 alr 36 ms, Snapdragon, 같은 git 2.53.0) |
| Termux glibc-runner(grun) | 포크된 glibc와 포크된 패키지 | 속도는 비슷하고 스톡 Ubuntu 호환성이 alr의 차별점([runtime/alr 00-product §4](../../runtime/alr/docs/00-product.md)) |
| Termux:X11 | 동반 APK + `app_process` 서버, Surface를 Binder로 전달 | 동반 앱 패턴이 실제로 동작한다는 선례. 표면 S3의 근거 |
| tawc (wmww/tawc) | `tawcroot`(seccomp `RET_TRAP` + 프로세스 내부 `SIGSYS` 처리), libhybris로 벤더 GPU 드라이버 적재, 창당 Android Task | `RET_TRAP` 방식은 [runtime/alr ADR 0006](../../runtime/alr/docs/adr/0006-raw-syscall-binaries.md)이 평가하지 않은 선택지다. libhybris는 ADR 0002와 충돌한다. 창당 Task는 멀티윈도우의 참고가 된다 |
| AVF/gfxstream | VM 경로 | 별도 트랙(변경 없음) |

tawc 코드를 반입하지는 않는다. 파일 단위 라이선스를 직접 확인하기 전에는 구조와 방법만 참고하고, 반입이 필요해지면 [clean-room-protocol](../clean-room-protocol.md)을 따른다.

## Decision

### D1. 제품 엔진은 alr 하나로 한다

- 새 기능은 alr 엔진 위에만 만든다.
- 레거시 in-process 로더는 **동결**한다. 버그 수정과 호스트 테스트 유지만 허용하고 새 기능은 넣지 않는다.
- 삭제는 지금 하지 않는다. 이유는 D2의 보험 역할 때문이다. 삭제 조건은 Phase 4에서 다시 판단한다.

### D2. `targetSdk 28`을 기반으로 확정하고, 위험을 감시한다

`app/build.gradle.kts`의 주석대로 이 선택은 Google Play 배포를 포기하는 대가로 커널 `execve`를 얻는다. 배포는 사이드로드, F-Droid, GitHub 릴리스로 한정한다([runtime/alr ADR 0005](../../runtime/alr/docs/adr/0005-play-store-unsupported.md)).

위험은 Android가 `targetSdk` 하한을 올려 28 앱의 설치를 막는 경우다. Android 14가 23 미만 설치를 막기 시작했으므로 하한이 오르는 방향은 이미 존재한다(다음 상향 시점은 미확인). 이 경우의 복귀 경로가 동결된 레거시 로더다. 그래서 D1에서 삭제하지 않는다.

감시 항목: 새 Android 릴리스마다 `targetSdk 28` APK의 설치 가능 여부와 `ALR DIRECT APP-DATA EXECVE` 프로브 결과를 확인한다.

### D3. 세 계층으로 나누고, 계층 사이의 계약을 문서화한다

```text
L2 표면      APK 앱(카탈로그, 런처)   Termux CLI(alr)   [후보] Termux 연결 GUI
                   |                     |                    |
L1 호스트 서비스   Wayland 컴포지터, GPU 링 실행기(Mali), 오디오, DoH, USB, SAF, 입력/IME
                   |
L0 엔진           alr: execve(ld.so --preload) + 경로 가상화 + 신호 전용 슈퍼바이저
```

L0과 L1 사이의 계약은 다음 세 가지로 제한한다. 이 세 가지만 지키면 엔진이 바뀌어도 L1은 바뀌지 않는다.

1. **소켓 경로**: 컴포지터는 rootfs 내부 `/tmp/alr-rt/alr-xdg/wayland-0`에 바인드한다. 호스트와 게스트가 같은 소켓을 서로 다른 이름으로 본다.
2. **공유 메모리 링**: GPU 링은 호스트와 게스트가 하나의 공유 메모리를 `mmap`으로 공유하는 구조다. 현재 구현은 앱이 `fork()` 이전에 링 fd를 만들고 `ALR_GPU_RING_FD`로 게스트에 상속시킨다(`app/src/main/cpp/alr_gpu/RING_HOOK_INTEGRATION.md`). alr 엔진은 게스트를 별도 프로세스로 `exec`하므로 이 상속이 그대로 성립하는지 증거가 없다. 성립하지 않으면 rootfs 안의 이름 있는 파일이나 Unix 소켓의 `SCM_RIGHTS`로 핸드오프를 바꾼다. 이 항목은 P1의 첫 번째 확인 대상이다.
3. **환경 변수**: `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `GDK_BACKEND`, `ALR_GPU_*`. alr 예약 변수(`LD_PRELOAD`, `LD_LIBRARY_PATH` 등)는 L1이 직접 쓰지 않는다.

### D4. 배포 표면은 3종이며, 같은 엔진과 같은 L1 서비스를 공유한다

| 표면 | 내용 | 상태 |
|---|---|---|
| S1 APK 앱 | 카탈로그, 런처, 컴포지터, GPU 가속. 데스크톱형 Linux 앱 | 동작 중(`sd` 빌드) |
| S2 Termux CLI | `alr install/run/shell`. 에이전트 CLI와 개발 도구가 주 대상 | 동작 중(수용 164건 PASS, 기기 2대) |
| S3 Termux 연결 GUI | Termux에서 띄운 게스트 GUI를 ALR 컴포지터에 표시 | **후보.** Phase 2 스파이크 결과로 결정 |

S3의 구상은 Termux:X11 선례를 따른다. Termux 프로세스가 `app_process`로 ALR APK의 컴포지터와 GPU 실행기를 같은 uid로 띄우고, ALR Activity가 소유한 Surface만 Binder로 받는다. 이렇게 하면 서로 다른 uid 사이에 Wayland 소켓을 열 필요가 없다. 이 구상은 아직 검증되지 않았다.

S3가 실패하면 대안은 "Termux에서 `alr run --gui`가 APK 서비스에 게스트 실행을 위임하는 방식"이고, 그 비용은 PTY 중계 구현이다.

### D5. 그래픽은 ADR 0002를 유지하고, tawc에서는 창 모델만 받아들인다

- Android 공개 API만 쓰는 명령 마샬링 경로(guest shim → 링 → 호스트 Mali 실행기 → `AHardwareBuffer` zero-copy)가 기본이자 유일한 기본 경로다.
- libhybris 방식(벤더 GPU 드라이버를 glibc 프로세스에 적재)은 채택하지 않는다. 벤더 비의존 원칙과 충돌하고, 기기마다 깨지는 지점이 다르다. 연구용으로 다루려면 ADR 0002를 먼저 개정해야 한다.
- 멀티윈도우(v2)는 tawc처럼 **창 하나가 Android Task 하나**가 되는 모델을 후보로 한다. [design ADR-004](../design/adr-004-inapp-catalog-ux.md) D1(v1 단일 포그라운드)의 v2 항목을 이 방향으로 좁힌다.

### D6. raw-syscall 바이너리는 `RET_TRAP` 레인을 먼저 측정한 뒤 결정한다

[runtime/alr ADR 0006](../../runtime/alr/docs/adr/0006-raw-syscall-binaries.md)은 `USER_NOTIF`(154 µs/syscall, 실측)와 `PR_SET_SYSCALL_USER_DISPATCH`(이 커널에 없음)만 평가하고 raw-syscall 바이너리를 비목표로 두었다. 세 번째 선택지가 평가되지 않았다.

- **프로세스 내부 `SECCOMP_RET_TRAP` + `SIGSYS` 핸들러**: 허용된 트램폴린 PC 밖에서 발행된 경로 syscall을 같은 스레드의 시그널로 받아 재작성하고, 트램폴린에서 다시 발행한다. 컨텍스트 스위치와 별도 프로세스가 필요 없다. tawcroot가 이 방식이다.
- Android의 zygote 필터가 `ALLOW`하는 syscall에 대해서는 `RET_TRAP`(0x30000)이 `ALLOW`(0x7fff0000)보다 낮은 값이므로 우리 필터가 이긴다([ADR 0006](../../runtime/alr/docs/adr/0006-raw-syscall-binaries.md)의 우선순위 설명과 같은 원리).
- `execve`는 시그널 핸들러를 초기화하므로, 정적 바이너리에는 핸들러를 먼저 설치하는 작은 사용자 영역 로더가 필요하다. 동적 glibc 바이너리는 `ld.so` 단계에서 이미 인터포저가 개입하므로 대상이 아니다. 대상은 Go, uutils, 정적 musl(codex 등)이다.

판정 규칙(스파이크 S2의 마이크로벤치로 닫는다):

| 왕복 비용(`getppid` 기준) | 결정 |
|---|---|
| 5 µs 이하 | 로더 프로토타입을 진행한다. `git status`급 경로 syscall 3만 회에서 약 150 ms가 되어 proot-distro(930 ms)보다 빠르다 |
| 5 µs 초과 31 µs 이하 | raw-syscall로 분류된 바이너리에만 적용하는 선택 기능으로 둔다 |
| 31 µs 초과 | 폐기한다. ADR 0006의 비목표를 유지하고 이 결과를 보론으로 기록한다 |

31 µs는 proot-distro 930 ms를 경로 syscall 3만 회로 나눈 값이다. 이 비용 이하여야 PRoot보다 의미 있게 빠르다.

미검증 위험: Go 런타임이 `SIGSYS` 핸들러를 직접 등록하므로 `rt_sigaction`을 가상화해야 한다. 스레드 전체에 필터를 적용하는 `TSYNC` 동작도 확인해야 한다.

### D7. 에이전트 CLI를 일급 워크로드로 정의한다

`runtime/alr/docs/00-product.md`의 G5(`node`/`npm`)를 확장해 아래 목표 G7을 추가한다.

| 대상 | 현재 상태 | 근거 |
|---|---|---|
| node 기반 CLI(npm으로 설치) | 달성 범위(G5) | `npm ci` 3.12배(proot-distro 대비, [M12](../../runtime/alr/docs/evidence/2026-08-03-m12-spawn-resolver.md)) |
| 정적 musl 바이너리(codex) | 경로 가상화 없이 동작. 호스트 형식 환경을 주는 방식으로 실사용 가능 | [ADR 0008 보론](../../runtime/alr/docs/adr/0008-static-guest-binaries-non-goal.md) |
| Go 바이너리 | 정적 바이너리와 같은 방식으로 실행될 것으로 예상하나 Go 자체로는 미검증. 경로 가상화는 없다 | D6 결과에 따라 달라진다 |
| PTY, DNS, CA 번들 | PTY 에뮬레이션과 DoH는 구현됨. 에이전트 CLI 시나리오(장시간 세션, 터미널 크기 변경)로는 미검증 | `tests/native_alr_pts_test.c`, `alr_doh` |

특정 CLI 제품(예: Antigravity)의 지원 여부는 이 저장소에서 확인된 바가 없으므로 문서에 적지 않는다. 설치 가능성은 위 분류(동적/정적/Go)로 판단한다.

### D8. 문서 규율을 저장소 전체로 통일한다

- 모든 성능, 호환성 주장에는 **엔진 라벨**(`engine=alr` 또는 `engine=legacy-loader`)을 붙인다.
- 상태 어휘는 `PASS`, `FAIL`, `SKIP`, `KNOWN_FAIL:<reason>`, `PENDING_DEVICE` 다섯 가지만 쓴다([runtime/alr 00-product §6](../../runtime/alr/docs/00-product.md)). `MEASURED`와 `MODELED`를 섞지 않는다.
- 증거 파일 없이 "완료"를 적지 않는다.
- ADR 번호 체계는 [docs/adr/README.md](README.md)에 정리한다.

## Phase 계획

| Phase | 내용 | 완료 기준 |
|---|---|---|
| P0 | 문서 정합(이 ADR과 함께 반영) | 문서 링크 검사 통과, 문서 고정 테스트 통과 |
| P1 | **alr 엔진 위에서 GPU와 GUI 재측정**: (1) 링 fd 핸드오프(D3 계약 2), (2) shim 라이브러리 배치(`LD_LIBRARY_PATH`가 alr 예약 변수), (3) glmark2-es2-wayland, (4) Vulkan ICD enumerate와 draw, (5) zero-copy present, (6) 기존 GUI 7종 | 각 항목이 `sd` 빌드 증거 파일(엔진 라벨 포함)로 `PASS`. 실패 항목은 원인과 함께 `KNOWN_FAIL`로 기록 |
| P2 | 스파이크 S1(표면 S3 실현성), S2(`RET_TRAP` 왕복 비용, D6 판정표) | S1: Termux 프로세스가 만든 `wl_shm` 프레임이 ALR Activity Surface에 표시됨. S2: 판정표의 한 행으로 귀결 |
| P3 | 에이전트 CLI 패키지(G7), 멀티윈도우 v2 설계 | G7 표의 각 행에 대해 기기 증거. 멀티윈도우는 설계 문서 |
| P4 | 레거시 로더 처분 결정 | P1 통과, 번들 트리의 alr 전환, Chromium CP-6 재기준선(alr 엔진, `--single-process` 없이) 완료 후 삭제 또는 보존을 별도 ADR로 결정 |

P1이 가장 먼저인 이유는 이 ADR의 D3(계층 분리)가 기기 증거 없이 설계 의도로만 서 있기 때문이다.

## Alternatives considered

**A. 레거시 in-process 로더를 주 엔진으로 되돌린다(`targetSdk 35`, Play 배포 가능).** 기각한다. `git status` 같은 경로 중심 작업에서 alr 엔진이 이미 네이티브에 3 ms 차이까지 붙었고, 레거시 로더는 리맵과 ptrace 중재로 복잡도가 크다. 다만 D2의 보험 역할 때문에 동결해서 보존한다.

**B. tawc 구조를 채택한다(libhybris + tawcroot).** 기각한다. libhybris는 ADR 0002의 벤더 비의존 원칙과 충돌한다. `tawcroot`의 핵심인 `RET_TRAP` 방식만 D6의 스파이크로 가져온다.

**C. Termux 전용 신규 프로젝트로 분리한다.** 기각한다. `runtime/alr`가 이미 독립 빌드 가능한 Termux CLI이고 APK도 같은 소스를 쓴다. 분리하면 엔진 수정이 두 저장소로 갈라진다.

**D. AVF 기반 VM으로 전환한다.** 기각한다. no-VM이 제품 정의이고, 별도 트랙으로만 남긴다.

## Consequences

- 신규 문서와 코드는 alr 엔진을 기준으로 쓴다. 레거시 로더에 대한 서술에는 "legacy"를 명시한다.
- `docs/PERFORMANCE.md`의 기존 수치는 삭제하지 않고 엔진 라벨을 붙인다.
- P1에서 GPU 증거가 엔진 이전 후 재현되지 않으면, 그래픽 계층의 "엔진 독립"이라는 서술을 철회하고 원인을 추적한다.
- `targetSdk 28`은 단일 장애 지점이다. D2의 감시를 릴리스 체크리스트에 넣는다.

## 정직한 한계

- 두 참조 기기는 모두 Samsung이다. 다른 OEM은 확인해 주지 못한다.
- GPU 증거는 MediaTek Mali-G615 한 기기에서 나왔다. Adreno, Tensor 계열은 [device-evidence](../architecture/device-evidence.md)의 기준을 아직 채우지 못했으며, "제조사 독립 가속"이라는 표현은 쓰지 않는다.
- D4의 S3, D6의 `RET_TRAP` 레인, D7의 PTY와 장시간 세션은 모두 미검증이다.
- 이 ADR을 작성한 시점에 위 비교 표의 tawc 항목은 공개 저장소를 읽은 내용에 근거하며, 기기에서 직접 실행해 본 것이 아니다.
