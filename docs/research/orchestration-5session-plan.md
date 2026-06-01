# ALR — 5-세션 병렬 오케스트레이션 플랜

> 최종 목표: **모든 Linux arm64 앱을 Android에서 CPU+GPU 풀 네이티브 가속으로, 오버헤드 거의 0(또는 0)에 실행** — 비root, public Android API only.
>
> 이 문서는 그 목표까지 **5개의 Claude Code 세션이 동시에** 작업하기 위한 분할/조율 플랜이다. §7의 "세션 킥오프 프롬프트"를 각 세션 첫 메시지로 붙여넣으면 바로 시작할 수 있다.

작성일 기준 stamp: `0.4.126-gui-native-perf-v126`. 디바이스: SM-X236N (MediaTek mt6878, Mali-G615 MC2, Android 16), 1200×1920 @ 90Hz.

---

## 0. 성공 기준 (정량) — "풀 가속 + 제로 오버헤드"의 정의

| 축 | 측정 | 목표 |
|----|------|------|
| CPU 실행 | native-exec 로더로 glibc arm64 in-process 실행 | PRoot/에뮬 없이 동작 (달성) |
| CPU 오버헤드 | syscall-light 앱: native(adb shell) 대비 wall-clock/syscall 지연 | < 5% (syscall-light), syscall-storm 앱(chromium)도 usable |
| GPU 가속 | glmark2-es2 score, 실앱 fps | Mali 직접 대비 high-% (목표 ≥ 70–80%), software=false |
| 디스플레이 | wl_output 해상도/주사율 | device-exact (1200×1920 @ 90Hz) |
| present | guest GPU 버퍼 → 화면 | zero-copy(dmabuf/AHB), CPU readback 없음 |
| 범용성 | 임의 Debian arm64 앱(apt) 실행 | toolkit 매트릭스(GTK/Qt/SDL/터미널/브라우저) + 호환표 |

"오버헤드 제로"의 본질적 적은 두 곳에 있다: **(a) CPU 측 ptrace/seccomp 중재 라운드트립**, **(b) GPU 측 per-call 마샬링/카피**. 5 워크스트림은 이 둘을 정면으로 줄이도록 짜였다.

---

## 1. 현재 Baseline (정찰로 확정)

- **CPU 실행 (L1):** `runtime_report.cpp:build_native_loader_probe`가 guest ld.so로 glibc ELF를 in-process 맵+점프, fork+ptrace supervisor로 감독. **PCGATE**(PC-gated seccomp) + `libalr_interpose.c`로 path 계열 syscall을 in-process 처리(traps=0). 멀티스레드는 PTRACE_SEIZE+EVENT_STOP/LISTEN(v124). **남은 오버헤드 = 비-trampoline syscall마다 RET_TRACE 라운드트립** → chromium류 syscall storm이 벽(메모리: chromium-native-goal). `alarm(dynamic?25:5)`로 장수 GUI 게스트를 검증 시간 내 종료(v126).
- **GPU (L2):** 현재 GUI는 **cairo 소프트웨어 렌더**(`GDK_RENDERING=cairo`) → wl_shm(CPU 픽셀) → 컴포지터가 memcpy→AHB → Mali는 **최종 합성에만** 사용. gfxstream **host 백본(decode/ring/AHB-FBO/`GpuExecutorService`)은 완성+Mali검증**, **guest shim**(`alr_gpu/guest_shim/`, libEGL.so.1+libGLESv2.so.2)은 소스 완성+wire-verified. **미연결**: ring fork + env 주입 + shim/glmark2 stage가 남음(전부 준비됨: `/tmp/gpushim-stage.tar`, `/tmp/glmark2-stage.tar`).
- **컴포지터/디스플레이 (L3):** `alr_wayland/alr_compositor.cpp` — Wayland on SurfaceView, **wl_shm only**(dmabuf 없음). v126에서 wl_output 해상도/주사율을 device-exact로 plumb + frame timer를 패널 refresh(90Hz)로 구동(검증 finalizing).
- **rootfs/앱 (L4):** bookworm-slim, GTK3 closure(harfbuzz **8.3.0**). **chromium overlay가 harfbuzz 6.0.0을 덮어 GTK 스택을 깨뜨렸던 회귀를 v126에서 overlay 비활성화로 해결**(사용자 "크로미움 보류"와 일치). foot/gtk3-widget-factory/glmark2 stage tar 준비됨.
- **검증 (L5):** `tests/`(host pytest ~277) + `docs/evidence/`. device 검증은 **반드시 `am force-stop` 후 cold start**(warm resume는 onCreate 스킵 → 메모리: device-test-force-stop-first).

---

## 2. 아키텍처 레이어 = 소유권 경계

병렬성의 핵심은 **파일 소유권을 레이어로 자른 것**이다. 세션은 자기 레이어 파일만 수정하고, 레이어 간은 §5의 인터페이스 계약으로만 만난다.

| 레이어 | 1차 소유 파일 | 워크스트림 |
|--------|--------------|-----------|
| L1 CPU 실행/중재 | `runtime_report.cpp:build_native_loader_probe` + ptrace supervisor, `libalr_interpose.c`, PCGATE BPF | **WS-1** |
| L2 GPU 마샬링 | `alr_gpu/**` (decode/ring/fbo/host_service/screen), `alr_gpu/guest_shim/**`, GPU JNI(`nativeAlrGpu*`) | **WS-2** |
| L3 컴포지터/디스플레이 | `alr_wayland/**`, `WaylandPresenter`(runtime_report 내), `nativeWayland*` JNI, MainActivity surfaceCreated | **WS-3** |
| L4 rootfs/앱 | `app/src/main/assets/rootfs/**`, `RootfsInstaller.kt`, `tools/**`, 앱 stage tar 빌드 | **WS-4** |
| L5 검증/벤치/CI/문서 | `tests/**`, `docs/evidence/**`, bench harness, `docs/**` | **WS-5** |

**핫스팟 2곳**(여러 WS가 닿음) — §6에서 규약으로 관리:
- `runtime_report.cpp`: 거대 단일 TU. L1/L2/L3가 각자 다른 함수/JNI를 추가. → **함수 단위 소유 + 추가 위주 + 통합 세션 merge**. (중기: WS-1 주도로 JNI를 모듈별 TU로 분리 리팩토링.)
- `MainActivity.kt`: L2/L3/L4가 launch wiring 추가. → **섹션 소유 + 추가 위주**.

---

## 3. 5 워크스트림

### WS-1 — CPU 실행 코어 & 오버헤드 제거
- **미션:** glibc arm64를 in-process로 실행하며 ptrace/seccomp 중재 오버헤드를 **0에 수렴**시킨다. syscall-storm 앱(chromium류)의 ptrace 벽을 돌파.
- **소유:** `build_native_loader_probe` + ptrace supervisor 루프, `libalr_interpose.c`, PCGATE seccomp BPF, guest_env 빌더.
- **마일스톤:**
  - M1 멀티스레드/멀티프로세스 감독 안정화(SEIZE 기반, group-stop/clone 정확) — 회귀 0.
  - M2 **in-guest interposer 확장**으로 RET_TRACE 라운드트립 최소화(가능한 syscall을 in-process 처리). raw `svc`는 LD_PRELOAD로 못 후킹 → 대안 평가(부분 정적 패치/vDSO/seccomp data 등).
  - M3 **out-of-process / SECCOMP_RET_USER_NOTIF 모델 평가**(in-process 제약과의 양립성 ADR) — 라운드트립 ~10× 절감 후보.
  - M4 chromium-class 멀티프로세스 앱 init 통과 → render.
- **의존:** 없음(기반). **제공(인터페이스):** `GpuRingHook`(§5-A), `guest_env` 확장점(§5-D).
- **리스크:** in-process ptrace 라운드트립은 아키텍처적 벽(메모리: chromium-native-goal). M3가 본질적 해법이나 모델 변경 → ADR 필요.

### WS-2 — GPU 풀 가속 (gfxstream 마샬링)
- **미션:** 게스트 GL/GLES(→Vulkan)를 Mali에 풀 가속. cairo SW를 GPU 경로로 대체.
- **소유:** `alr_gpu/**`, `guest_shim/**`, `nativeAlrGpu*` JNI.
- **마일스톤:**
  - M1 shim 빌드+stage(`gpushim-stage.tar` → `/usr/lib/androlinux/libEGL.so.1`,`libGLESv2.so.2`) + glmark2 stage. (준비 완료)
  - M2 **loader ring 연결**: `GpuRingHook`(WS-1 제공)로 fork 전 `ring_init(memfd)`+`eventfd` 생성, FD_CLOEXEC 해제, `ALR_GPU_RING_FD/BYTES/DOORBELL_FD` env, `GpuExecutorService` 기동.
  - M3 **glmark2-es2-wayland가 Mali로 렌더 + score** (software=false) — GPU 네이티브 1차 증명.
  - M4 실앱 GL: GTK4 GL 렌더러(`GDK_RENDERING=gl`)/SDL2 → Mali.
  - M5 GLES3+/Vulkan: ANGLE→Vulkan, Vulkan ICD 프런트(메모리: guest-gpu-accel-strategy).
- **의존:** WS-1(`GpuRingHook`), WS-3(`PresentSource` for zero-copy present). **제공:** `AhbFrameSource`(§5-B).
- **리스크:** present 경로(M3는 executor→window 직접; Wayland 합성 앱은 dmabuf 필요 → WS-3).

### WS-3 — 컴포지터 & 디스플레이 & 입력
- **미션:** device-exact 디스플레이 + GPU 버퍼 zero-copy present + 완전한 데스크탑 합성/입력.
- **소유:** `alr_wayland/**`, `WaylandPresenter`, `nativeWayland*` JNI, MainActivity surfaceCreated.
- **마일스톤:**
  - M1 해상도/주사율 device-exact(1200×1920@90Hz) + present를 패널 refresh로. (v126, 검증 finalizing)
  - M2 **`zwp_linux_dmabuf_v1`** 광고 + AHB를 EGLImage로 import → GPU 버퍼 zero-copy present(WS-2 `AhbFrameSource` 소비). CPU readback 제거.
  - M3 멀티윈도우/zorder/팝업/서브서피스 안정(메모리: alr-gui-android-native-polish 교훈).
  - M4 입력 완성: touch/pointer/key/IME, wl_seat 협상(GDK 갭).
- **의존:** WS-2(GPU 버퍼). **제공:** `PresentSource`(§5-C: shm + dmabuf 통합 present), wl_output 값.
- **리스크:** dmabuf↔AHB import의 Mali 드라이버 호환; chromium은 dmabuf 미광고가 유리(전략 분기).

### WS-4 — rootfs & 범용 앱 호환성
- **미션:** 임의 Debian arm64 앱이 뜨도록 rootfs를 완성. toolkit/패키지매니저/폰트/로케일/Xwayland.
- **소유:** `assets/rootfs/**`, `RootfsInstaller.kt`, `tools/**`, 앱 stage tar 빌드 파이프라인.
- **마일스톤:**
  - M1 **rootfs 라이브러리 버전 정합 가드**(harfbuzz↔pango 류 mismatch 재발 방지; overlay가 base lib을 다운그레이드 못 하게 검증 단계 추가).
  - M2 toolkit 매트릭스: GTK3/4, Qt5/6(qtwayland), SDL2, 터미널(foot), 경량 브라우저(netsurf).
  - M3 in-app `apt`/`dpkg`(PRoot fallback 또는 native; 메모리: device-evidence-mali-android16의 dpkg 한계).
  - M4 **Xwayland** stage → X11-only 앱(현재 불가) 호스팅.
  - M5 호환 매트릭스(앱×결과) 자동 빌드.
- **의존:** WS-1(실행), WS-3(display). **제공:** stage tar 규약, `rootfs_manifest` 검증.
- **리스크:** stage overlay가 base lib을 덮어 회귀(이번 harfbuzz 사건). M1이 이를 막는 가드.

### WS-5 — 검증 & 벤치 & CI & 문서
- **미션:** "풀 가속 + 제로 오버헤드"를 **정량 증명**. 회귀/호환/evidence/CI.
- **소유:** `tests/**`, `docs/evidence/**`, bench harness(신규 도구), `docs/**`.
- **마일스톤:**
  - M1 **CPU 오버헤드 벤치**: 동일 바이너리를 native(adb shell, 가능 시) vs ALR 로더로 — syscall 지연/wall-clock. traps/rewrites 카운터 노출.
  - M2 **GPU 벤치**: glmark2 score(ALR) vs Mali 직접(host probe) 비율.
  - M3 회귀 매트릭스(GIMP/foot/gtk3demo/CLI no-regression 게이트) + version-stamp 핀.
  - M4 호환 매트릭스 문서 + device evidence 템플릿.
  - M5 CI(host 빌드+테스트; device는 수동 게이트).
- **의존:** 전부(검증 대상). **제공:** evidence/벤치 포맷, 게이트 기준.

---

## 4. 의존성 그래프 & 통합 순서

```
        WS-1 (CPU/loader)  ── GpuRingHook ─────────────┐
            │  guest_env 확장점                          ▼
            ├──────────────► WS-4 (rootfs/앱)        WS-2 (GPU)
            │                     ▲                      │ AhbFrameSource
            ▼                     │ display              ▼
        WS-3 (compositor) ◄── PresentSource ──── (dmabuf zero-copy)
            │  wl_output, 입력
            ▼
        WS-5 (검증/벤치) ◄──────── 전 WS 산출물 측정
```

- **크리티컬 패스(GPU 네이티브):** WS-1.M1(안정) → WS-2.M2(ring 연결) → WS-2.M3(glmark2 Mali) → WS-3.M2(dmabuf present) → WS-2.M4(실앱).
- **병렬 안전:** WS-4(rootfs 정합/매트릭스), WS-5(벤치 하니스/회귀), WS-3.M1(디스플레이, v126), WS-1.M2(interposer)는 서로 독립으로 즉시 착수 가능.
- **B↔C 상호의존(GPU present)**: WS-2의 `AhbFrameSource`와 WS-3의 `PresentSource`를 §5-B/C 계약으로 먼저 합의 → 양쪽 mock으로 병렬 진행 후 통합.

---

## 5. 인터페이스 계약 (병렬성의 핵심 — 먼저 합의, 그 다음 양쪽 독립 구현)

각 계약은 **헤더 1개**로 고정한다. 헤더 변경은 통합 세션 승인 필요. 계약이 서면 양쪽은 서로를 mock으로 두고 병렬 진행한다.

- **A. `GpuRingHook` (WS-1 제공 → WS-2 소비)** — `runtime_report.cpp`의 fork 직전에 호출되는 훅. 시그니처(제안): `struct GpuRing { int ring_fd; uint32_t ring_bytes; int doorbell_fd; };` + `bool alr_loader_attach_gpu_ring(GuestEnv&, GpuRing&)`. WS-1은 "fork 전에 이 훅을 부르고, 반환된 fd를 자식에 상속(FD_CLOEXEC 해제)하고 env에 넣는다"만 보장. ring 생성/executor 기동은 WS-2가 훅 안에서.
- **B. `AhbFrameSource` (WS-2 제공 → WS-3 소비)** — frame boundary(req_seq)에서 완성된 `AHardwareBuffer*`를 넘기는 콜백. `using AhbFrameSource = std::function<void(AHardwareBuffer*, int w, int h, uint64_t serial)>;`
- **C. `PresentSource` (WS-3 제공 → WS-2/L2 소비)** — 컴포지터의 통합 present 진입점: wl_shm 경로(기존)와 dmabuf/AHB 경로(신규)를 하나로. WS-2의 AHB를 zero-copy로 합성.
- **D. `guest_env` 확장점 (WS-1 제공 → WS-2/3/4 소비)** — `guest_env`에 키=값을 추가하는 등록 API. 각 WS가 자기 env(예: `ALR_GPU_RING_*`, `GDK_RENDERING`, `LD_LIBRARY_PATH` 조각)를 직접 push하지 않고 등록 → 충돌/순서 문제 제거.
- **E. stage tar 규약 (WS-4 제공 → 전 WS)** — overlay tar는 `./` 루트, 플랫 SONAME, base lib 다운그레이드 금지(M1 가드). `/data/local/tmp/*-stage.tar` → `RootfsInstaller.extractVerifiedTar` overlay, `.{name}-staged-<size>` 마커.

---

## 6. 충돌 관리

1. **격리:** 세션당 git worktree(브랜치 `ws-1`..`ws-5`). 통합 세션이 `main`으로 merge.
2. **소유권 매트릭스(§2):** 파일/함수 단위. 다른 WS 소유 파일은 **읽기만**. 변경이 필요하면 인터페이스 계약(§5)으로 요청.
3. **핫스팟 규약:**
   - `runtime_report.cpp` — WS-1: `build_native_loader_probe`+supervisor / WS-2: `nativeAlrGpu*`+`GpuRingHook` 구현 / WS-3: `nativeWayland*`+`WaylandPresenter`. **함수 추가 위주, 기존 함수 시그니처 변경은 계약 통해.**
   - `MainActivity.kt` — launch/wiring을 WS별 private fun으로 분리, onCreate/surfaceCreated에서는 호출만.
4. **version stamp는 통합 세션만 bump**(메모리: version-stamp-pin-sites). 세션은 stamp/tests 핀을 건드리지 않음(머지 충돌의 최대 원인).
5. **merge 주기:** 마일스톤 완료 시 또는 1일 1회. 통합 세션이 인터페이스 계약 기준으로 검수 → device 게이트(WS-5 회귀) → merge.
6. **`git add -A` 금지** — 항상 명시 경로만(다른 세션 uncommitted 작업 보호).

---

## 7. 세션 킥오프 프롬프트 (각 세션 첫 메시지로 붙여넣기)

> 공통 머리말(모든 세션에 포함): "너는 ALR 프로젝트의 워크스트림 WS-N 담당이다. `docs/research/orchestration-5session-plan.md`를 먼저 읽어라. HARD CONSTRAINTS 준수(비root, public Android API only, W^X-safe, PRoot fallback-only, device evidence 없이 완료 주장 금지, `git add -A` 금지·명시 경로만, version stamp는 건드리지 말 것, device 테스트는 `am force-stop` 후 cold start). 사용자는 '찬우'이고 한국어 반말, 이모지 최소. 네 소유 파일(§2) 밖은 읽기만 하고, 레이어 경계는 §5 인터페이스 계약으로만 넘어라."

- **WS-1:** "L1 CPU 실행/중재 담당. 목표: ptrace/seccomp 라운드트립을 0에 수렴 + chromium-class 돌파. M1(멀티스레드 SEIZE 안정·회귀0)부터. `GpuRingHook`(§5-A)·`guest_env 확장점`(§5-D) 인터페이스를 먼저 만들어 WS-2/3/4가 의존할 수 있게 공개해라. 기존 GIMP/foot/CLI 무회귀 게이트 유지."
- **WS-2:** "L2 GPU 마샬링 담당. 목표: glmark2-es2가 Mali로 렌더(software=false). M1(`gpushim-stage.tar`+`glmark2-stage.tar` 빌드/stage)→M2(`GpuRingHook`으로 ring 연결)→M3(glmark2 score). `/tmp/gpushim-stage.tar`,`/tmp/glmark2-stage.tar`는 준비됨. host 백본(`GpuExecutorService`)·guest shim 소스는 완성됨 — 연결만. WS-3 `PresentSource` 합의 전까지는 executor→window 직접 present로 진행."
- **WS-3:** "L3 컴포지터/디스플레이/입력 담당. M1(해상도/주사율 device-exact 1200×1920@90Hz — v126 코드 device 검증 마무리)→M2(`zwp_linux_dmabuf` + AHB EGLImage import로 GPU 버퍼 zero-copy present, WS-2 `AhbFrameSource` 소비)→M3 멀티윈도우→M4 입력. `PresentSource`(§5-C)를 공개해라."
- **WS-4:** "L4 rootfs/앱 담당. M1(base lib 버전정합 가드 — overlay가 harfbuzz류를 다운그레이드 못 하게; 이번 chromium harfbuzz 6.0.0 사건 재발 방지)부터. M2 toolkit 매트릭스(GTK/Qt/SDL/foot/netsurf), M3 apt/dpkg, M4 Xwayland. stage tar 규약(§5-E) 준수."
- **WS-5:** "L5 검증/벤치/CI/문서 담당. M1(CPU 오버헤드 벤치: 동일 바이너리 native vs ALR), M2(GPU 벤치: glmark2 ALR vs Mali직접 비율), M3(회귀 매트릭스 게이트), M4(호환 매트릭스+evidence 템플릿). 모든 WS의 device evidence를 `docs/evidence/`에 표준 포맷으로 수집. version-stamp 핀 관리는 통합 세션과 조율."

---

## 8. 마일스톤 & 통합 체크포인트

| 체크포인트 | 게이트(WS-5 검증) | 의미 |
|-----------|------------------|------|
| CP-0 인터페이스 합의 | §5 A–E 헤더 머지 | 5 WS 완전 병렬 시작 가능 |
| CP-1 GUI 기반 + 디스플레이 | GIMP/foot/gtk3demo 렌더 + 90Hz/1200×1920 (device) | L3.M1 + L4.M1 + harfbuzz 정합 |
| CP-2 GPU 네이티브 1차 | glmark2-es2 Mali score, software=false (device) | L2.M3 (+L1 ring, L3 present) |
| CP-3 CPU 오버헤드 정량 | native vs ALR 벤치 리포트 | L1.M2 + L5.M1 |
| CP-4 zero-copy present | dmabuf 경로, CPU readback 0 (device) | L2.M4 + L3.M2 |
| CP-5 범용성 | toolkit 매트릭스 + apt 앱 실행 (device) | L4.M2–M5 |
| CP-6 제로 오버헤드 수렴 | chromium-class usable 또는 USER_NOTIF ADR | L1.M3–M4 |

각 CP는 device evidence(`docs/evidence/`) + host 회귀 통과를 게이트로 한다. CP-0(인터페이스 합의)이 병렬성의 전제 — 가장 먼저.

---

## 9. Device Lease Protocol (단일 디바이스 직렬화 — 통합 세션이 통제)

ALR 디바이스는 **단 하나**(SM-X236N, `R5KL20B6S3X`). 5세션이 동시에 install/force-stop/logcat을 치면 서로의 테스트를 덮어쓴다([[device-test-force-stop-first]]). 그래서 디바이스는 **통합 세션(WS-1/오케스트레이터)이 단독 소유하고 직렬 실행**한다.

**규칙:**
1. **세션 worktree = host-only.** 각 WS는 자기 worktree에서 host 검증만: `uvx pytest tests/ -q`(WS-5 harness; `/opt/homebrew/bin/python3`엔 pytest 없음), `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ./gradlew :app:externalNativeBuildDebug`(native 컴파일). **APK install 금지** — 다른 세션 device 테스트를 clobber한다.
2. **device 테스트 요청 = main merge + DEVICE-REQ.** device 검증이 필요하면 (a) main에 merge(`git merge-tree` clean + host gate 통과), (b) merge commit 메시지에 `DEVICE-REQ: <보고 싶은 marker/PASS 조건>`을 남긴다. 통합 세션이 큐에 넣는다.
3. **통합 세션만 device 실행.** 절차: merged HEAD → `JAVA_HOME=openjdk@17 ./gradlew :app:assembleDebug`(stamp는 통합 세션이 bump) → `adb install -r -d`(`-d`=downgrade 허용, 충돌 방지) → `am force-stop` → `logcat -c` → `am start`(cold start; warm resume는 onCreate 스킵) → `logcat -s alr_loader` 캡처 → `docs/evidence/`.
4. **배치.** 여러 세션의 DEVICE-REQ를 한 빌드에 묶어 실행(매 세션 개별 install은 5×265MB 비효율). 통합 세션이 주기적으로 drain.
5. **stamp는 통합 세션만.** 세션은 version stamp/pin-test를 절대 건드리지 않는다(merge 충돌 1순위).
6. **WS-5는 device install 안 함** — merged build의 logcat evidence를 파싱해 bench/gate를 돌린다.

현재 device 큐(통합 세션 처리 예정): **CP-2 GPU**(WS-1 loader-attach✓ + WS-2 ops✓ + glmark2 launch/overlay), **native+PRoot baseline**(WS-1 → WS-5 CP-3 unblock), **CP-1 display capture**(WS-3 wl_output 이미 device-verified → WS-5 marker).

## 10. 일감 재분배 (2026-06-01, 오케스트레이터 통제)

- **WS-1 (CPU/loader) — CP-1+M2+M3+CP-2-infra 완료:** 다음 = (a) **native(adb shell)+PRoot baseline 측정**(WS-5 CP-3 % ratio 블로커 해소), (b) CP-2 GPU 통합 device(glmark2 Mali) 실행, (c) 오케스트레이터(§9/§10 + device 큐).
- **WS-2 (GPU) — ring-hook contract + glmark2 ops(FBO/renderbuffer/completeness) 완료:** 다음 = (a) glmark2 launch wiring 협의(통합 세션이 MainActivity에 추가; §5 계약 유지), (b) doorbell-driven wakeup(현재 spin-poll), (c) M3 GLES3/ANGLE→Vulkan. device는 통합 세션이 실행.
- **WS-3 (compositor) — M1 90Hz✓ + §5-C PresentSource✓:** 다음 = M2 `zwp_linux_dmabuf` + AHB→EGLImage zero-copy present(WS-2 `AhbFrameSource`→`frame_sink` 어댑터로 unblock), M3 multiwindow, M4 input/keymap.
- **WS-4 (rootfs) — lib-downgrade guard + stage-tar + xkb-data 완료:** 다음 = (a) **`locales-all`(C.UTF-8) + babl/gegl `.so`**(gtk3-widget-factory SIGABRT/gimp 잔여 → GUI 완전 안정), (b) toolkit 매트릭스(Qt/SDL2/netsurf stage), (c) apt/dpkg.
- **WS-5 (verify/bench) — bench harness(366) + CP-3 부분정량 완료:** 다음 = (a) WS-1 native/PRoot baseline 받아 **CP-3 % overhead ratio 완성**, (b) CP-2/CP-4 device evidence 파싱, (c) `alr-compat-matrix` 채우기.

GUI SIGSEGV는 WS-1 `XKB_CONFIG_ROOT`(v127, device로 gtk3-widget-factory 렌더 확인) + WS-4 xkb-data로 **해결됨**; 잔여 SIGABRT(locale/gegl)는 WS-4 (a).

## 부록 — 현재 미커밋 작업(이 플랜 직전)
v126: chromium overlay 비활성화(harfbuzz 회귀 수정), 해상도/주사율 device-exact plumb(MainActivity+JNI+compositor timerfd), `alarm` 25s(검증용), foot/gtk3-widget-factory launch wiring. → CP-1로 흡수. (device 검증 finalizing.)
