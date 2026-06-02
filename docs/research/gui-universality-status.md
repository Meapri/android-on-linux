# ALR GUI Universality — device-proven Linux GUI set (WS-5 / L5 SSOT)

> **"어떤 Linux GUI 앱이 device 에서 실제로 뜨는가"** 를 한 곳에 모은 프로젝트 범용성 SSOT.
> 벤치(성능 숫자) 문서가 **아니다** — 렌더 여부(창/픽셀이 Android 화면에 나오는가)만 다룬다.
> 성능/오버헤드는 `docs/PERFORMANCE.md`, ratio 는 `cp2-gpu-ratio-glmark2.md`/`cp3-cpu-overhead-ratio.md`.
>
> 소유: WS-5. HOST-ONLY — 새 device 측정 없음, 기존 `docs/evidence/` 인용만.
> 앱×결과 전체 표(CLI/패키지매니저/GPU/디스플레이 포함)는 자매 문서
> `docs/research/alr-compat-matrix.md`. 이 문서는 그중 **GUI(창이 뜨는) 앱**만 추려
> 범용성(어떤 toolkit/앱군이 device 에서 뜨는지)을 한눈에 보인다.

baseline: 통합 트리 v138 (round-6 drain device-verified). 디바이스 `R5KL20B6S3X`
(SM-X236N, mt6878 SoC, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).
공통 경로: **glibc 게스트 → ALR native loader(비root, public Android API only) →
GDK/toolkit → wl_shm(소프트웨어 픽셀) → Wayland-on-SurfaceView 컴포지터 → Android SurfaceView**.
GPU(glmark2)는 별도 shim→ring→host Mali executor 경로(아래 §3).

## 상태 범례
- **USABLE** — device 에서 사람이 실제로 조작 가능(입력 포함).
- **RENDERS** — device 에서 창/픽셀이 화면에 나옴(컴포지터 frame counter 전진).
- **STAGED / 진행중** — overlay/launch 경로는 준비, device 렌더는 아직(정직한 PENDING).

---

## 1. Device-증명된 범용 GUI 셋 (창이 실제로 뜸)

이 일곱이 "임의 Linux arm64 GUI 앱이 ALR 로 Android 에 뜬다"의 현재 증명 범위다.
서로 다른 toolkit/앱군(이미지 편집기·위젯 데모·터미널·웹브라우저·SDL2·Qt6)을 가로지른다.

| 앱 | 종류 / toolkit | 결과 | 렌더 evidence | 한 줄 |
|----|---------------|------|--------------|-------|
| **GIMP 3.0.2** | 이미지 편집기 / GTK3 | **USABLE** | `v111-gimp-fully-usable-drawing`; 재확인 `2026-06-02-cp2-FINAL-glmark2-score-1074`, `2026-06-02-cp5-batch-8mibring-texture-ws4-overlays` | 터치로 File>New>1920×1080 캔버스 생성 + 브러시 스트로크. dialog/menu/popup 입력 라우팅 동작. 풀 데스크탑 앱이 손가락으로 사용 가능 |
| **gtk3-widget-factory** | 위젯 갤러리 / GTK3 | **RENDERS** | `2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs`; drain#10 `2026-06-02-breadth-fanout-drain` | GTK3 위젯 전수 데모. SVG SIGABRT 해소(sig6→sig14) 후 25s 풀 생존(alarm timeout = crash 아님). traps 135→99(WS-1 CP-6 M2) |
| **gtk3-demo** (`/bin/alr-gtk3-test`) | GTK3 데모 창 | **RENDERS** | `2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs`, `v89-gtk3-renders`, `v90-real-gtk3-window` | 렌더 루프 정상(frames 12→2213). gtk_init backend=wayland; ~2088 file open 이 rootfs 로 mediation |
| **foot** | 터미널 에뮬레이터 | **RENDERS** | `2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs`; 무회귀 재확인 `2026-06-02-netsurf-browser-renders` | `rendered=true`. keymap+locale+SVG 안정화 후 렌더; libfcft4/libutf8proc shim closure OK. GTK 아닌 native Wayland 클라이언트도 뜸을 증명 |
| **netsurf-gtk** | 웹브라우저 / GTK3 | **RENDERS** | `2026-06-02-netsurf-browser-renders` (drain#12, v134) | `netsurf-result: rendered=true` frames 2214→2217; **5 guest threads** 멀티스레드 in-process; static-PIE 5.8MB; `about:welcome` 가 SurfaceView 에 합성; 25s 풀 생존(sig14=SIGALRM, crash 아님). 실 웹브라우저가 ALR 로 Android 에 렌더 |
| **SDL2** (testdraw2) | 위젯/그래픽 데모 / SDL2 | **RENDERS** | `2026-06-02-round4-milestones-drain` (drain#13, v135) | `sdl2gui-result: rendered=true frames=2217→2218` `bin=/usr/libexec/installed-tests/SDL2/testdraw2`; `SDL_VIDEODRIVER=wayland`; overlay extracted=120. GTK 도 native-Wayland 도 아닌 **SDL2 클라이언트**가 ALR 로더로 컴포지터에 렌더 → 네 번째 독립 toolkit |
| **Qt6** (analogclock) | 위젯 데모 / Qt6 (qtwayland) | **RENDERS** | `2026-06-02-round6-qt6-execreentry-vkrender` (round-6, v138) | `qt6gui-result: rendered=true frames=2215→2216` `bin=.../qt6/examples/widgets/widgets/analogclock/analogclock`. round-4/5 의 Qt-init SIGSEGV(EGL hwintegration → ICD 없는 `eglGetDisplay`)를 **EGL QPA/HwIntegration 플러그인 overlay 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`** 로 해소 → Qt 가 **wl_shm backing store** 사용. GTK/native-Wayland/browser/SDL2 에 이어 **다섯 번째 독립 toolkit** = 7-toolkit 범용 GUI 셋 |

**범용성 의미.** 이 일곱은 **다섯 가지 독립 toolkit/클라이언트 종류**를 가로지른다:
(1) GTK3 풀 앱(GIMP·widget-factory·gtk3-demo·netsurf), (2) native Wayland 클라이언트(foot),
(3) 멀티스레드 웹브라우저(netsurf, 5 threads), (4) SDL2 그래픽 데모(testdraw2),
(5) **Qt6 위젯 데모(analogclock, wl_shm)**. 즉 ALR 의 GUI 경로는 한 앱에 특화된 게 아니라
**임의 glibc GTK3/Wayland/SDL2/Qt6 GUI 바이너리**를 비root·public-API 로 Android SurfaceView 에 띄운다.
모두 cairo/Qt 소프트웨어 렌더 → wl_shm → 컴포지터 합성(GPU 합성은 zero-copy AHB present, CP-4).

### 보조 GUI 프리미티브 (창 렌더의 하위 검증 — device-증명)
| 항목 | 결과 | evidence | 비고 |
|------|------|----------|------|
| 입력 주입(touch/pointer/key, wl_seat) | USABLE (received=24: pointer 10/key 8/touch 6) | `v86-input-injection`, `v87-interactive-toolkit-pacing`, `v111-gimp-fully-usable-drawing` | GIMP 경로에서 실사용; IME/wl_seat 갭은 WS-3 M4 |
| in-process 이미지 디코드(gdk-pixbuf PNG/JPEG/BMP/GIF) | RUNS (전 포맷 PASS) | `v95-image-decode`, `2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs` | .so x-bit fix 후 bmp/gif/png/jpeg 전부 decode OK |
| XKB 키맵(guest GUI) | 수정됨(SIGSEGV fix) | `v127-xkb-config-root-gui-keymap-segv-fixed` | `XKB_CONFIG_ROOT` rootfs-absolute → sig=11 카운트 0 |
| 해상도/주사율 device-exact(1200×1920@90Hz) | RENDERS | `v126-harfbuzz-fix-display-90hz`, `v127-xkb-config-root-gui-keymap-segv-fixed` | wl_output mode + timerfd 90Hz |

---

## 2. toolkit 진행 잔여 (창은 떴으나 남은 증분 작업)

§1 의 7-toolkit 셋은 모두 device 에서 창이 뜬다. 여기는 그중 **방금 §1 으로 승급한 toolkit 의 잔여
증분 작업**과, 아직 §1 에 없는 후보를 정직하게 둔다. 정직하게 — device 렌더 evidence 없이는 §1 승급 금지.
(SDL2 는 round-4 drain#13 `testdraw2`, **Qt6 는 round-6 v138 analogclock** 으로 §1 에 승급됨.)

| toolkit | 현재 도달점 | 남은 일 (증분, 회귀 아님) | evidence |
|---------|-----------|--------|----------|
| Qt6 (qtwayland) | **§1 으로 승급됨** (round-6 v138 `analogclock` 창이 wl_shm 으로 device-렌더 — §1 표 참조). round-4/5 의 Qt-init SIGSEGV(원인=**EGL hwintegration**, ICD 없는 `eglGetDisplay`)는 **EGL QPA/HwIntegration 플러그인 overlay 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`** 로 device-해소 — Qt 가 **wl_shm backing store** 사용 | 더 많은 Qt/KDE 위젯 앱(analogclock 데모를 넘어), Qt 입력 interaction(클릭/키)·다중 윈도. analogclock 단일 데모 = device-렌더 첫 증명 | `2026-06-02-round6-qt6-execreentry-vkrender` |

> netsurf 도 round-3(drain#11)에서는 "GTK init 까지 실행되나 headless 라 `cannot open display` exit"
> 단계였고, drain#12 에서 컴포지터에 display-backed launch 하자 §1 으로 올라갔다(RENDERS).
> SDL2 도 round-4 drain#13 에서 같은 한 발(컴포지터 launch)을 디뎌 §1 으로 승급됐다.
> Qt6 는 launch 한 발 전에 **Qt init SIGSEGV** 벽이 하나 더 있었다 — round-4 는 이를 "closure 부족"으로
> 보았으나 round-5 가 device 로 원인을 **EGL hwintegration** 으로 좁혔고(closure 는 완전),
> **round-6 v138 이 EGL→wl_shm 백엔드 강제로 device-해소**하여 §1 으로 승급했다. 잠긴 큰 로더 기능들의
> SSOT 는 `docs/research/loader-feature-gaps.md`(G2), CP-6 진행은 `docs/research/cp6-status.md`.

---

## 3. GPU 가속(별도 경로 — 참고)

GUI 앱의 창은 §1 의 cairo SW→wl_shm 경로로 뜨지만, **GPU 렌더(GL/GLES)**는 별도로
guest shim → SPSC ring → host Mali executor 로 가속된다. glmark2-es2 가 실 Mali 에 렌더
(`software renderer=false`, build+texture Score ~1000+, GL_RENDERER=Mali-G615 MC2 passthrough).
round-5 에서 **게스트 Vulkan enumerate/props 마샬링이 실 Mali libvulkan 에 device-verified**됐고
(`ALR VK ENUM MARSHAL: PASS`, VK 1.3, `2026-06-02-round5-vulkan-device-marshal`), round-6 v138 의
**VK-M2 render 가 실 Mali 에 device/queue/command 마샬을 성공**(device created=yes, gfx queue family=0)
했으나 clear `vkQueueSubmit` 은 **FAIL**(AHB color-attach/render-pass setup 미완 — clear-submit fix 는
round-7 진행, `2026-06-02-round6-qt6-execreentry-vkrender`). 상세/숫자는
`docs/research/alr-compat-matrix.md` §GPU 와 `cp2-gpu-ratio-glmark2.md`. GTK4 GL 렌더러/실앱 GL → Mali
연결은 WS-2 M4(진행).

---

## 4. 한 줄 요약 + 갱신 규칙

**device-증명 범용 GUI 셋(창이 실제로 뜸, 7 toolkit): GIMP 3.0.2(USABLE) · gtk3-widget-factory ·
gtk3-demo · foot · netsurf-gtk(웹브라우저, 5 threads) · SDL2(testdraw2) · Qt6(analogclock) —
GTK3/native-Wayland/멀티스레드 브라우저/SDL2/Qt6 를 가로지름.** round-6 v138 이 Qt6 를 §1 으로 승급
(Qt-init SIGSEGV 원인=**EGL hwintegration** 을 round-5 가 device-규명 → round-6 가 **EGL→wl_shm**
백엔드 강제로 device-해소, `2026-06-02-round6-qt6-execreentry-vkrender`). 잠긴 큰 로더 기능 SSOT =
`docs/research/loader-feature-gaps.md`, CP-6(chromium storm/exec-re-entry) 진행 =
`docs/research/cp6-status.md`.

*갱신 규칙:* device evidence 추가 시에만 RENDERS/USABLE 로 승급(evidence 파일명 명기).
host-only 진전(overlay stage, 로더가 entry 도달)만으로는 §1 으로 올리지 않고 §2 에 둔다.
이 문서는 `alr-compat-matrix.md` 의 GUI 행과 동기 유지(같은 evidence 인용).
