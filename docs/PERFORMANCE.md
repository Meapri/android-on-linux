# ALR — 성능 & 네이티브 가속 증명

> **한 줄 요약:** Android 위에서 비-root·public API만으로 임의의 **Linux arm64(glibc) 앱을 in-process로 실행** — CPU는 **네이티브 arm64 그대로**(에뮬레이션/번역 없음), GPU는 **실제 Mali GPU에서 렌더**(소프트웨어 래스터 아님). 아래 수치는 전부 **실기기 cold-start logcat 증거**로 뒷받침된다.

측정 기기: **Samsung SM-X236N** · MediaTek **mt6878** · GPU **Mali-G615 MC2** · **Android 16** · 패널 1200×1920 @ **90Hz**. 빌드 스탬프 `0.4.130-cp5-5ws-fanout-v130`(versionCode 130). 모든 측정은 `am force-stop` 후 cold start(§ device 프로토콜).

---

## 0. 헤드라인 — "CPU·GPU 둘 다 네이티브 가속" (증명됨)

| 축 | 주장 | 측정값 (실기기) | software/emulation? | 증거 |
|----|------|----------------|---------------------|------|
| **CPU 연산** | 네이티브 arm64 속도 그대로 | **+0.00% 오버헤드** (native 4.06 = ALR 4.06 ns/op, **같은 바이너리**) | ❌ 에뮬 없음 — in-process 네이티브 실행 | [CP-3 microbench](evidence/2026-06-01-ws5-cpu-overhead-quantified.md) |
| **CPU syscall** | 중재 비용은 seccomp 고정비뿐 | +11.98% (~24 ns/syscall, seccomp 디스패치) | ❌ 에뮬 없음 — 커널 BPF 통과 비용 | [CP-3 ratio](research/cp3-cpu-overhead-ratio.md) |
| **GPU 렌더 (GLES)** | 실제 Mali GPU에서 가속 | glmark2-es2 **~1000–1200 FPS @ 1920×1200**, `software=false` | ❌ SW 래스터 아님 — `GL_RENDERER: Mali-G615 MC2` | [CP-2 FINAL](evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md), [drain#9](evidence/2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc.md) |
| **GPU (Vulkan)** | guest Vulkan → 실 Mali libvulkan | enumerate 마샬링 device-검증: VK_SUCCESS, Mali-G615 MC2, **API 1.3** | ❌ 실 vendor libvulkan (ARM 0x13b5) | [Vulkan](evidence/2026-06-02-round5-vulkan-device-marshal.md) |
| **디스플레이** | device-exact | **1920×1200 @ 90Hz** | — | [CP-1](evidence/2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs.md) |

**핵심:** CPU는 게스트 arm64 명령을 CPU가 **직접 실행**(QEMU/번역 레이어 0) → 순수 연산은 native와 **수치적으로 동일(0%)**. GPU는 게스트 GLES 호출이 실 **Mali-G615 실리콘**에서 돌아간다(`software renderer = false`, 게스트가 보는 `GL_RENDERER`가 실제 `Mali-G615 MC2`). 둘 다 "네이티브 가속"이 정성·정량으로 증명됐다.

---

## 1. 어떻게 "네이티브"인가 (아키텍처)

ALR은 컨테이너/VM/에뮬레이터가 아니다. 비-root, public Android API만 쓴다.

### 1.1 CPU — in-process 네이티브 실행
- 로더가 게스트 glibc arm64 ELF를 **게스트 `ld.so`로 in-process 맵 + 진입점 점프** → CPU가 게스트 arm64 명령을 **그대로 실행**한다. 명령어 번역/에뮬레이션 **없음**.
- `fork`+`ptrace` supervisor + **PCGATE**(PC-gated seccomp) + `LD_PRELOAD` interposer는 **syscall만** 중재한다. path 계열 syscall은 in-process로 재작성(트램폴린 발 syscall은 RET_ALLOW → **hot path에서 ptrace 라운드트립 0, traps=0**).
- 결과: **연산 = native 속도(0% 오버헤드)**. 비용은 syscall 1건당 ~24 ns의 **seccomp 디스패치 고정비**뿐이며, 이는 ALR의 비효율이 아니라 "seccomp를 켜는 한" 커널이 모든 syscall을 BPF에 통과시키는 비용이다.

### 1.2 GPU — 실 Mali로의 명령 스트림 마샬링 (gfxstream 계열)
- 게스트가 `libEGL.so.1`/`libGLESv2.so.2` **shim**에 링크 → GL/EGL 호출을 공유메모리 **SPSC ring**에 인코딩.
- 호스트 프로세스(실 Mali EGL 컨텍스트 소유)가 ring을 디코딩해 **실제 Mali-G615에서 replay** → `AHardwareBuffer`에 렌더 → 컴포지터가 **zero-copy present**.
- **client-side virtual GL ID**로 per-call 라운드트립 제거(생성 호출도 즉시 가상 ID 반환). 프레임 경계(`eglSwapBuffers`)에서만 1회 동기화.
- 결과: 게스트 GLES 앱이 **실 GPU 실리콘**에서 ~1000+ FPS로 렌더. 호스트 Mali 컨텍스트는 **OpenGL ES 3.2**.

---

## 2. CPU 벤치마크 — 같은 바이너리, native vs ALR (CP-3, CLOSED ✅)

**바이트-동일한** static 마이크로벤치를 `adb shell` 직접(native)과 ALR 로더 양쪽에서 실행:

| 워크로드 | native ns/op | ALR ns/op | 오버헤드 | 의미 |
|----------|-------------|-----------|---------|------|
| **compute** (CPU-bound, syscall 0) | 4.06 | 4.06 | **+0.00%** | ALR가 native와 **정확히 동일** — 순수 연산엔 seccomp/ptrace 미관여 |
| **syscall** (`getpid` 루프) | 200.36 | 224.36 | **+11.98%** | syscall당 ~24 ns = seccomp 디스패치 고정비 (BPF 슬림화로도 ~1 ns만 감소 → 본질적) |

- 일반 CLI 네이티브-exec wall-clock도 **~18–20 ms**(native-process 수준, traps=0).
- §0 목표 "CPU 오버헤드 < 5% (syscall-light)" → **device-증명 PASS** (지배적 비용인 compute가 0%).
- syscall-storm 0%는 seccomp를 켜는 한 원리적으로 불가(정직한 한계). chromium류 raw-`svc` storm은 **별개의 더 큰 ptrace 벽**으로, CP-6/[ADR-001](design/adr-001-syscall-overhead-user-notif.md)에서 USER_NOTIF 등으로 추적 중.

---

## 3. GPU 벤치마크 — glmark2-es2가 실 Mali에서 렌더 (CP-2 FINAL ✅)

게스트 `glmark2-es2-wayland`가 shim → ring → host executor → 실 Mali-G615로 렌더. 전부 `software renderer = false`.

| drain | scene | per-scene FPS | glmark2 Score | host ring | GL_RENDERER (게스트가 본 값) |
|-------|-------|---------------|---------------|-----------|------------------------------|
| #7 (CP-2 FINAL) | build | 1075 @ 1920×1200 | **1074** | 1 MiB | (합성 문자열 — 당시) |
| #8 (CP-5) | build + texture | 1206 / 1123 | **1163** | 8 MiB | (합성 문자열) |
| **#9 (passthrough)** | build + texture | 1012 / 1095 | **1052** | 8 MiB | **`Mali-G615 MC2`** (실 host 문자열) |

drain#9에서 게스트 glmark2가 출력한 OpenGL 정보(=호스트 실 Mali 값을 ring으로 passthrough):
```
GL_VENDOR:   ARM
GL_RENDERER: Mali-G615 MC2
GL_VERSION:  OpenGL ES 3.2 v1.r44p1-01eac0.ed1fb6cfc1040479b92ddf50a952e57c
```
- **8 MiB ring**으로 1024×1024 RGBA(4 MiB 단일 업로드) **texture scene**까지 렌더(1 MiB 시절엔 drop). geometry(build) + texturing(texture) 둘 다 GPU-class FPS.
- 보조 GPU 셀프테스트도 실 Mali에서 PASS: **`ALR GPU LIVE INTEGRATION: PASS`**(guest→ring→host→AHB present, 8프레임), **`ALR GPU SCREEN CUBE: PASS`**(live 파이프라인→AHB→external-OES 화면 present).
- ~1000–1200 FPS @ 1920×1200는 **소프트웨어 래스터라이저로는 불가능한 수치** → GPU 가속의 정성 증명.

### 3.1 마샬링 오버헤드 = 병목 아님 (ring vs direct, 같은 Mali)
같은 triangle op-스트림을 **같은 Mali에서** 두 경로로 렌더해 throughput 비교 ([evidence](evidence/2026-06-02-gpu-throughput-ring-vs-direct.md)):

| 경로 | FPS (512×512, 600 frames) | renderer |
|------|---------------------------|----------|
| ALR (2-thread ring + executor → AHB-FBO, 실 glmark2 경로) | **1158** | Mali-G615 MC2, software=false |
| DIRECT (단일 스레드 decode + 프레임당 glFinish, ring 없음) | 757 | Mali-G615 MC2, software=false |
| ratio (ALR / direct) | **1.53** | — |

- ratio **≥ 1** = ALR의 command-ring 마샬링 비용이 **2-thread 파이프라인에 완전히 가려진다**(producer가 프레임 N+1을 마샬링하는 동안 executor가 프레임 N의 GPU 작업 수행). 즉 **§0(b) "per-call 마샬링/카피"는 이 워크로드에서 throughput 병목이 아니다** (client-side virtual GL ID로 per-call 라운드트립 0 + 스레드 오버랩).
- **주의(과장 금지):** 이건 "ALR이 네이티브 앱보다 1.5배 빠르다"는 주장이 **아니다**. DIRECT는 의도적으로 보수적인 단일 스레드+프레임당 glFinish 기준선이다. 하드-최적화 네이티브 앱의 swapchain 파이프라인 천장 대비 절대 %는 별개의 더 어려운 측정으로 **미해결**(§5).

---

## 4. 보조 증거 — 디스플레이 / present / GUI / 범용성

| 항목 | 결과 (실기기) | 증거 |
|------|--------------|------|
| 디스플레이 | 1920×1200 @ **90Hz** device-exact, present를 패널 refresh로 구동 | CP-1 |
| zero-copy present | `zwp_linux_dmabuf_v1` + AHB→EGLImage import (CPU readback 0) | CP-4 |
| GUI 앱 | **GIMP 3.0.2** 풀 렌더(터치 사용), gtk3-widget-factory, gtk3-demo(2213 프레임), foot 터미널, **netsurf-gtk 웹브라우저**(rendered=true, 5 threads), **SDL2 창**(testdraw2 rendered=true) | [drain#9](evidence/2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc.md), [netsurf](evidence/2026-06-02-netsurf-browser-renders.md), [round-4](evidence/2026-06-02-round4-milestones-drain.md) |
| 범용성 | **apt-get / dpkg-query / Xwayland**가 ALR 로더로 실기기 실행(버전 출력) | drain#9 |

---

## 5. 정직한 범위 (아직 정량 안 된 것)

이 문서는 증명된 것만 단정한다. 다음은 **진행 중**:

1. **GPU 절대 %-of-native-app 비율** — 두 가지를 구분한다:
   - **마샬링 오버헤드는 병목 아님 = 증명됨** (§3.1): ALR ring 경로가 같은 Mali·같은 op로 단일 스레드 direct decode 대비 1.53× → 마샬링이 GPU 작업에 가려짐.
   - **하드-최적화 네이티브 앱의 파이프라인 천장 대비 절대 %는 미해결**: glmark2가 glibc/Wayland라 맨-Android Mali에서 직접 못 돌아가므로(§3.1 주의), swapchain 기반 네이티브 천장 baseline이 필요 — [CP-2 ratio 문서](research/cp2-gpu-ratio-glmark2.md) §3에 절차. 현 glmark2 Score(1052–1163)는 **build+texture 2-scene 부분 종합**(전체 14-scene 아님). 이 절대 비율(§0 ≥70% 목표)은 아직 단정하지 않는다.
2. **CPU syscall-storm(chromium류)** — light `getpid` ~12%와 별개로, raw-`svc` storm은 ptrace 라운드트립 벽([ADR-001](design/adr-001-syscall-overhead-user-notif.md)). chromium은 현재 보류.
3. GLES3+/Vulkan(호스트 ES 3.2 확인됨), qt6/netsurf 등 toolkit 매트릭스 확장.

---

## 6. 재현 / 증거 인덱스

device 측정은 통합 세션이 단일 기기 직렬로 수행(빌드→`adb install -r -d`→`am force-stop`→`logcat -c`→cold start→`logcat -s alr_loader`). host 측 비율/게이트 재현:
```sh
# CPU: compute 0% (gated PASS), syscall ~12% (storm, not gated)
python -m bench overhead --native-ns 406  --native-samples 100 --alr-ns 406  --alr-samples 100
python -m bench overhead --native-ns 20036 --native-samples 100 --alr-ns 22436 --alr-samples 100 --storm
# GPU: 비율(분모 baseline 들어오면)
python -m bench gpu --alr-score 1163 --mali-score <BASE> --alr-renderer "Mali-G615"
```

**device evidence 원본**(전부 cold-start logcat):
- CPU: [`2026-06-01-ws5-cpu-overhead-quantified.md`](evidence/2026-06-01-ws5-cpu-overhead-quantified.md)
- GPU CP-2 FINAL: [`2026-06-02-cp2-FINAL-glmark2-score-1074.md`](evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md)
- GPU 8MiB ring + texture: [`2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md`](evidence/2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md)
- GL_RENDERER passthrough + apt/X11 + GUI: [`2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc.md`](evidence/2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc.md)
- GPU marshalling throughput (ring vs direct): [`2026-06-02-gpu-throughput-ring-vs-direct.md`](evidence/2026-06-02-gpu-throughput-ring-vs-direct.md)
- 분석: [CP-2 ratio](research/cp2-gpu-ratio-glmark2.md) · [CP-3 ratio](research/cp3-cpu-overhead-ratio.md) · [오케스트레이션 플랜 §0/§8](research/orchestration-5session-plan.md)
