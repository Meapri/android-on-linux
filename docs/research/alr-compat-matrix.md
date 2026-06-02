# ALR 앱 호환 매트릭스 (WS-5 / L5)

> 임의 Debian arm64 앱이 ALR(native loader, 비root, public Android API only)에서 어디까지
> 도는지의 앱×결과 표. **device evidence가 있는 행만 USABLE/RUNS/RENDERS로 표기**한다.
> host-only/예정은 WIRED/PENDING. 이 표는 WS-5가 유지하며, 새 device evidence가 추가될 때마다 갱신.

작성 baseline: HEAD `0df74dc` (v124) → 통합 트리 v127 (CP-1 landed) → v128/v129 (CP-2 GPU-native FINAL + CP-5 8MiB ring) → v130 (5-WS fan-out drain#9: GL_RENDERER passthrough + getpwuid fix + apt/dpkg/Xwayland 기능 device-PROVEN) → v132 (6-WS breadth fan-out drain#10: babl-gegl 67 op .so staged + qt6/xwayland auto-stage + WS-1 traps↓ gtk3-widget-factory 135→99) → v133 (round-3 drain#11: GLES3 core 커버리지 회귀0 + netsurf-gtk 가 ALR 로더로 GTK init 까지 실행) → v134 (drain#12: **netsurf-gtk 웹브라우저가 컴포지터에 RENDERS — rendered=true, 5 threads**) → v135 (round-4 drain#13: **SDL2 창 데모(testdraw2)가 컴포지터에 RENDERS — rendered=true**; qt6 analogclock=Qt-init SIGSEGV(게스트 child, 앱 회귀 아님); apt/dpkg install=exec-re-entry 벽; **Vulkan enumerate/props 마샬링 backbone host-verified**) → v136 (round-5 drain#14: **Vulkan enumerate/props 가 실 Mali libvulkan 에 device-verified — `ALR VK ENUM MARSHAL: PASS` VK 1.3**; qt6 SIGSEGV 원인=**EGL hwintegration** device-규명(클로저는 완전)) → v137 (CP-6 M-R2: **chromium `--version` device-실행 + verdict=mediation-negligible** traps=0/emul=1; render storm 은 데드락) → v138 (round-6 drain: **Qt6 analogclock 이 EGL→wl_shm 전환으로 RENDERS — `qt6gui-result: rendered=true` frames 2215→2216**(`QT_WAYLAND_DISABLE_HW_INTEGRATION=1` + EGL QPA/HwIntegration 플러그인 overlay 제외); **exec-re-entry ADR-003 B-1 가 device-fires — `alr exec x0=/bin/sh reason=rewrite` traps=1 rewrites=1**(execve x0 path-rewrite, no-exec 게스트는 0 무회귀); **VK-M2 render 가 실 Mali 에 device created=yes**(vkCreateDevice/queue/cmdpool 마샬, gfx queue family=0)이나 clear `vkQueueSubmit`=FAIL). 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878 SoC, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).

## 상태 범례
- **USABLE** — device에서 사람이 실제로 조작 가능(입력 포함).
- **RENDERS** — device에서 창/픽셀이 화면에 나옴.
- **RUNS** — device에서 프로세스가 정상 실행/종료(GUI 아닐 수 있음).
- **WALL** — 실행되나 성능/데드락 벽에 막힘(아키텍처적 한계).
- **WIRED** — launch 경로/stage는 준비, device 렌더 확인은 pending.
- **PENDING** — 미착수/의존 대기.

## CLI / 코어 (L1 native loader)
모든 일반 게스트는 mediation 불변식 `pcgate=1 interpose=1 traps=0 rewrites=0`(=supervisor ptrace 라운드트립 0)으로 device-verified. `exec_ms`는 fork→reap wall-clock(fork + ELF map + ld.so + run + exit + supervisor).

| 앱/바이너리 | 결과 | exec_ms | Evidence | 비고 |
|-------------|------|---------|----------|------|
| `/bin/dynhello` (dynamic glibc) | RUNS (exit 0, `alr-dyn-ok`) | 19 | v79-dynamic-loader, ws1-m2-cpu-mediation-overhead | 동적 ld.so in-process, traps=0 |
| `/usr/bin/env` | RUNS (exit 0) | 19 | ws1-m2-cpu-mediation-overhead | traps=0 |
| `/usr/bin/id` | RUNS (exit 0) | 20 | ws1-m2-cpu-mediation-overhead | traps=0 |
| `/bin/dash` | RUNS (exit 0) | 18-19 | ws1-m2-cpu-mediation-overhead | traps=0 |
| `/bin/alr-png-test` | RUNS (exit 0) | 23 | v95-image-decode, ws1-m2-cpu-mediation-overhead | libpng/gdk-pixbuf 디코드, traps=0 |
| `/bin/alr-wl-test` | RUNS (exit 0) | 40 | v82-wayland-compositor-up, v83-pixman-toolkit-window | wl_shm solid client |
| `/bin/alr-pixman-test` | RUNS (exit 0) | 38 | v83-pixman-toolkit-window | pixman gradient+AA+text → SurfaceView |
| `/usr/bin/gimp-console-3.0 --version` | RUNS (exit 0) | 48 | v126-harfbuzz-fix-display-90hz | 무거운 lib 로드, harfbuzz 8.3.0 정합 후 exit 0 |
| `gimp-3.0` (CLI 부팅 단계) | RUNS (traps=1, 1×set_robust_list류) | 135 | ws1-m2-cpu-mediation-overhead | 유일하게 traps=1 |

→ 이 집합이 **회귀 게이트**(`bench/regression_gate.py`)의 기준선. mediation 불변식: `pcgate=1 interpose=1 traps=0 rewrites=0`.

## 패키지 매니저 / X11 (L4 — 기능 device-PROVEN)
drain#9(v130)에서 dpkg/apt/Xwayland 가 **ALR native loader 로 실행+버전 보고**까지 device-증명됨
(`pkgfunc-*` 마커, 전부 `ok=true exec=GUEST EXEC PASS`). 즉 stage-only 가 아니라 **글리브 게스트로
on-device 실행**. 단, 실제 `apt install`(네트워크/소스 fetch+unpack)은 아직 **PENDING**.

| 앱/바이너리 | 결과 | Evidence | 비고 |
|-------------|------|----------|------|
| `dpkg-query --version` | **RUNS** (`ok=true`, marker `[Debian dpkg-query]`) | 2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc | glibc 게스트로 실행+버전 보고 (drain#9) |
| `dpkg-query -l libc6` | **RUNS** (`ok=true`, marker `[libc6]`) | 2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc | dpkg-db overlay(ws-3/ws-4 staged) 조회 OK |
| `apt-get --version` | **RUNS** (`ok=true`, marker `[apt ]`) | 2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc | apt-config overlay 적재, 실행+버전 보고 |
| `Xwayland -version` | **RUNS** (`ok=true`, marker `[Xwayland]`) | 2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc | x11 overlay; X11-only 앱 호스팅 기반 (전체 X11 앱 표시는 PENDING) |
| 실제 `apt install <pkg>` (네트워크 fetch+unpack) | **WALL: exec-re-entry** (round-6 v138 device: `apt-install: unpacked=false`); B-1 execve path-rewrite 는 device-fires 하나 full dpkg chain 은 **PENDING (B-3 in-flight, round-7)** | 2026-06-02-round6-qt6-execreentry-vkrender, 2026-06-02-round4-milestones-drain | round-6 v138 에서 ADR-003 **B-1**(execve x0 path-rewrite)가 device-fires — 게스트 `execve(/bin/sh)` 가 EVENT_SECCOMP 에서 trap 되고 program path(x0)가 rootfs 로 재작성됨(`alr exec x0=/bin/sh reason=rewrite`, traps=1 rewrites=1 clone_events=7; no-exec 게스트는 0 무회귀). 단 `dpkg -i` 의 full fork+exec maintainer-script chain 은 여전히 `unpacked=false` — **B-3**(interposer 재주입: exec 된 child 의 envp 에 abs-rootfs `LD_PRELOAD` + `ALR_ROOTFS` 주입)이 필요하고 **round-7 진행 중**(device-pending). 잠금해제 범위·난이도·의존: `loader-feature-gaps.md`(G1) |

mediation 불변식: `pcgate=1 interpose=1 traps=0 rewrites=0` (CLI 집합과 동일).

**성능 (device, v127 WS-1 M2):** 일반 CLI native-exec wall-clock(`exec_ms`) ~**18-20ms**(`dynhello`/`env`/`id`/`dash`/`alr-png-test`) = native 프로세스 수준. path-mediation은 **traps=0 device-verified**(in-process translate, supervisor 라운드트립 0). path-xlate cold 4334.7 ns/op(≈19.9 syscall units, raw getppid 218.3 ns/op 대비; 256-entry cache로 분할 상환). **CP-3 apples-to-apples CLOSED** ✅: 동일 microbench(static musl)를 native(adb shell)+ALR loader 양쪽 실행 — **compute 0% overhead**(ALR 4.06 = native 4.06 ns/op, **gated <5% PASS**), syscall(getpid 동일 call) **~12%**(ALR 224.36 vs native 200.36, storm reported). 즉 **일반 연산/CLI는 native급 0% 오버헤드 device-verified**, syscall-storm만 ~12%. (이전 근사 ~9%는 정밀치로 대체.) 이 ~12%(24ns)는 **seccomp 디스패치 고정비용**이라 BPF 슬림화로 ~1ns만 감소(223.37 vs 224.36) — seccomp 켜는 한 syscall 0%는 원천 불가, per-app `ALR_PCGATE=0` 옵션만(raw-svc 백스톱이라 전역 off는 위험). PRoot A/B는 SELinux로 보류. 상세: `docs/evidence/2026-06-01-cp3-apples-to-apples-gtk3-svg-perm.md`, `docs/evidence/2026-06-01-ws5-cpu-overhead-quantified.md`.

**오버헤드 진전 (device, drain#10/v132 — WS-1 CP-6 M2):** path-mediation supervisor 라운드트립(`traps`)이 **줄었다** — `gtk3-widget-factory` **135→99**, `glmark2-es2` ~40→31, **CLI/dpkg/apt/Xwayland/foot=0**(불변식 유지). 메커니즘 = opendir 트램폴린(디렉터리 open 이 supervisor 로 안 떨어짐) + credential getter 캐시. 단 **동일-빌드 A/B 격리는 아님**(중간 변경 존재) — 방향성 감소 + targeted 메커니즘으로 정직하게 보고. (벤치 측정 아님, drain#10 evidence 인용.) 상세: `docs/evidence/2026-06-02-breadth-fanout-drain.md`.

## GUI 툴킷 (L2/L3)
| 앱 | 툴킷 | 결과 | exec_ms | Evidence | 비고 |
|----|------|------|---------|----------|------|
| GIMP 3.0.2 | GTK3 | **USABLE** (터치로 File>New>1920×1080 캔버스>브러시 스트로크; CP-2/CP-5 drain 에서 device-render 재확인) | — | v111-gimp-fully-usable-drawing, 2026-06-02-cp2-FINAL-glmark2-score-1074, 2026-06-02-cp5-batch-8mibring-texture-ws4-overlays | cairo SW 렌더 → wl_shm 합성, dialog/menu/popup 입력 라우팅. drain#7/#8 final GUI probe 에서 GIMP 3.0.2 device-render 재확인(no FATAL/SIGSEGV) |
| gtk3-widget-factory | GTK3 | **RUNS** (SVG SIGABRT 해소; GUI 25s 생존 sig=14=alarm timeout, exec_ms=25031; traps **135→99** rewrites 52→50, drain#10 WS-1 CP-6 M2) | 25031 | 2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs, 2026-06-02-breadth-fanout-drain | sig=6→sig=14: 2겹 fix(WS-4 .so x-bit 0700 + WS-1 GDK_PIXBUF rootfs-absolute) — `libpixbufloader_svg.so cannot open` 제거. drain#10: opendir 트램폴린 + credential getter 캐시로 supervisor 라운드트립 traps 135→99 ↓ (glmark2 ~40→31; CLI/dpkg/apt/Xwayland/foot=0). 동일-빌드 A/B 격리는 아님(중간 변경 존재) — 방향성 감소 + 메커니즘 targeted |
| gtk3 데모 창 (`/bin/alr-gtk3-test`) | GTK3 | **RENDERS** (frames 12→**2213**, 렌더 루프 정상) | 197 | 2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs, v89-gtk3-renders, v90-real-gtk3-window | gtk_init backend=wayland; ~2088 file open이 rootfs로 mediation |
| in-process 이미지 디코드 (gdk-pixbuf PNG/JPEG/BMP/GIF) | — | RUNS (PASS, 전 포맷) | — | v95-image-decode, 2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs | shared-mime-info DB + **.so x-bit fix 후 bmp/gif/png/jpeg 전부 decode OK** |
| 입력 주입 (`/bin/alr-input-test`/`alr-interactive-test`) | wl_seat | USABLE (received=24: pointer 10/key 8/touch 6) | (의도된 dispatch 대기) | v86-input-injection, v87-interactive-toolkit-pacing | redraw 루프(redraws=5 hits=2) |
| foot | (terminal) | **RENDERS** (rendered=true) | — | 2026-06-01-gtk3-svg-sigabrt-resolved-gui-runs | GUI 안정화(keymap+locale+SVG) 후 렌더; libfcft4/libutf8proc shim closure OK |
| Qt6 (qtwayland) | Qt | **RENDERS** (`qt6gui-result: rendered=true frames=2215→2216` `bin=.../qt6/examples/widgets/widgets/analogclock/analogclock`; round-6 v138 EGL→wl_shm fix) | — | 2026-06-02-round6-qt6-execreentry-vkrender, 2026-06-02-round5-vulkan-device-marshal | WS-4 M2 / WS-3 GUI-launch. round-4/5 의 Qt-init SIGSEGV(round-5 가 원인을 **EGL hwintegration** = Qt 가 wayland **EGL** client-buffer integration 을 골라 ICD 없는 `eglGetDisplay` 호출로 device-규명; closure 는 완전)는 round-6 에서 **EGL QPA platform 플러그인 + compositor HwIntegration 플러그인을 qt6 overlay 에서 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`** 로 해소 — Qt 가 **wl_shm backing store** 를 써 analogclock 창이 SurfaceView 에 합성(frame counter 전진, crash 없음). **다섯 번째 독립 toolkit** 으로 device-증명 범용 GUI 셋(GIMP/gtk3-widget-factory/gtk3-demo/foot/netsurf/SDL2/**Qt6** = 7 toolkit)에 합류. 상세: `loader-feature-gaps.md`(G2) |
| SDL2 (testdraw2) | SDL | **RENDERS** (`sdl2gui-result: rendered=true frames=2217→2218` `bin=/usr/libexec/installed-tests/SDL2/testdraw2`; `SDL_VIDEODRIVER=wayland`; overlay extracted=120) | — | 2026-06-02-round4-milestones-drain | WS-4 M2 / WS-3 GUI-launch. round-3 에서 `testver` CLI 만 RUN 이던 게 round-4 drain#13 에서 `testdraw2` 데모 창이 컴포지터에 display-backed launch → SurfaceView 합성(frame counter 전진). SDL2 가 GTK/native-Wayland/browser 와 **네 번째 독립 toolkit** 으로 범용 GUI 증명셋에 합류. 잔여(증분, 회귀 아님): 전체 SDL2 입력/오디오 경로 |
| netsurf-gtk | (경량 GTK3 웹브라우저) | **RENDERS** (`netsurf-result: rendered=true` frames 2214→2217; 컴포지터 wl-frame counter 전진 = 창이 SurfaceView 에 합성; **5 guest threads** 멀티스레드 in-process; 25s 풀 생존 sig=14=SIGALRM=alarm timeout, crash 아님; drain#12) | 25031 | 2026-06-02-netsurf-browser-renders | WS-4 M2 / WS-3 GUI-launch. 실 GTK3 웹브라우저 바이너리(`/usr/bin/netsurf-gtk`, static-PIE 5.8MB)가 ALR 로더로 PARSE/MAP/INTERP/entry → GDK→wl_shm→SurfaceView (foot/GIMP 와 동일 경로) `about:welcome` 렌더. 범용 GUI 증명셋에 **브라우저** 추가. 잔여(증분, 회귀 아님): 전체 페이지 자산/네트워크/입력 interaction = round-4 진행 |
| babl/gegl GIMP op 모듈 (67 .so) | (GIMP 백엔드) | **overlay STAGED + 모듈 LOAD 확인** (babl-gegl-stage extracted=67 skipped=0, .so 0755 → dlopen-able; round-4 drain#13: `gimp-filter: via=gimp-console-batch ok=true` = gimp-console batch 가 babl/gegl op 모듈을 로드). 실제 필터 출력 검증은 **PENDING (round-5 진행 중)** | — | 2026-06-02-round4-milestones-drain, 2026-06-02-breadth-fanout-drain | WS-4. 67개 op 모듈 x-bit 0755 로 stage → dlopen 가능; drain#13 에서 gimp-console batch 실행 + 모듈 load(ok marker). 실제 필터 output(픽셀) device 검증은 미완 — full filter exercise 는 **exec-re-entry**(GIMP plugin fork+exec) 의존, `loader-feature-gaps.md` |

## 브라우저 (Goal-2)
| 앱 | 결과 | Evidence | 비고 |
|----|------|----------|------|
| chromium `--version` | RUNS (in-process, exit 0, `Chromium 147.0.7727.137`; **M-R2 verdict=mediation-negligible** traps=0/emul=1) | 2026-06-02-cp6-mr2-chromium-syscall-mix (v137), chromium-runs-inprocess (v121) | 186MB static-PIE; DT_NEEDED 전체 클로저 링크; `--jitless` 불필요(V8 JIT W^X 검증 v120). M-R2(v137): supervisor 가 두 trap 사이트에서 nr 히스토그램+getrusage 집계 → init 은 라운드트립 storm 아님(중재≠병목). CP-6 진행 SSOT: `cp6-status.md` |
| chromium `--dump-dom` / `--headless` (V8+render) | WALL (멀티스레드-ptrace 데드락 → render storm N·trap 미측정) | 2026-06-02-cp6-mr2-chromium-syscall-mix (v137), chromium-runs-inprocess (v121) | raw `svc` syscall-storm → LD_PRELOAD interposer 후킹 불가 → seccomp RET_TRACE 라운드트립 벽. M-R2 init verdict 는 mediation-negligible 이나 **render** storm 은 데드락(PTRACE_SEIZE+EVENT_STOP/LISTEN 튜닝)에 막혀 미측정 — 이게 풀려야 M-R5(svc-rewrite) vs M-R1(USER_NOTIF) A/B 가 결정 가능. 분기/흐름: `cp6-status.md`(ADR-001/002/003) |

## GPU (L2)
host 백본(decode/ring/AHB-FBO/zero-copy present)은 Mali-G615 MC2에 픽셀 검증(software renderer=false). **CP-2 FINAL 달성**(drain#7/#8): guest glibc glmark2 가 shim→ring→host executor 로 실 Mali 에 렌더 — build 1206 / texture 1123 FPS → **Score 1163**, `software=false`. drain#9(v130)에서 게스트가 실 Mali `GL_RENDERER`(Mali-G615 MC2, GLES 3.2)를 passthrough 로 보는 것까지 device-검증(합성 문자열 아님). ALR-vs-Mali-직접 **비율**은 `docs/research/cp2-gpu-ratio-glmark2.md` (Mali-직접 baseline = PENDING_DEVICE, 통합 세션이 채움).

| 항목 | 결과 | Evidence | 비고 |
|------|------|----------|------|
| GPU host decode+draw (M1, shader+VBO+texture+draw) | Mali-verified (PASS, software=false) | v115-gpu-native-m1m2-mali | 32 ops, center=0,220,0 corner=26,26,102 exact |
| GPU host SPSC command ring (M2) | Mali-verified (PASS, 739B pushed==drained) | v115-gpu-native-m1m2-mali | virtual GL IDs, round-trip 0 |
| GPU host AHB render target (M4 host half) | Mali-verified (PASS, AHB-direct readback) | v117-gpu-native-ahb-rendertarget | FBO color attach via EGLImage |
| AHB zero-copy present (external-OES) | Mali-verified (live, `ahb=8 gltex=0 path=zerocopy`) | v113-ahardwarebuffer-zerocopy-proven, v114-ahb-zerocopy-present-live | GIMP 합성이 zero-copy로 present, glTexImage2D fallback 0회 |
| GPU live producer→ring→executor→AHB present | Mali-verified (8 frames presented+verified, two-thread) | v118-gpu-native-live-integration | loader fork(STEP B)만 남음 |
| guest libEGL/libGLESv2 shim (M3) | wire-verified (소스) | v118-gpu-native-live-integration | device 연결 pending (WS-2 M2) |
| 화면 cube present (M4 STEP B, loader fork) | PENDING | v119-gpu-screen-cube-present | guest fork + ring fd 상속 |
| CP-2 INFRA: loader ring attach + EGL dlopen + Mali software=false | **DEVICE-VERIFIED** | 2026-06-01-cp2-glmark2-egl-dlopen-resolved | `alr_loader_attach_gpu_ring`(glmark2 감지)+ring/doorbell+GpuExecutorService; libpthread 수정 후 EGL library dlopen 성공; Mali self-test 전부 `software renderer=false`(Mali-G615). 인프라만 검증 — Score는 아래 행 참조 |
| glmark2-es2 **build** scene (ALR, software=false) | **RUNS** (renders on Mali, build 1075 FPS @ 1920×1200 → **Score 1074**) | 2026-06-02-cp2-FINAL-glmark2-score-1074 | CP-2 **FINAL** (drain#7). shim shader-source fix(`997a1c0`: `glGetShaderiv(GL_SHADER_SOURCE_LENGTH)` 로컬 응답) 후 build scene 컴파일·렌더. `software renderer=false`, Mali-G615 |
| glmark2-es2 **build+texture** scene (ALR, software=false, 8 MiB ring) | **RUNS** (build 1206 / texture 1123 FPS → **Score 1163**) | 2026-06-02-cp5-batch-8mibring-texture-ws4-overlays | CP-5 batch (drain#8, v129). texture scene 의 4 MiB 텍스처 upload(`OP_TEX_IMAGE_2D`)이 8 MiB host ring(ws-2 `dc4198e`)으로 통과(1 MiB면 drop). `software=false`. CP-2 가 geometry→texture scene 까지 확장 device-증명 |
| glmark2-es2 build+texture (ALR, drain#9 regression check) | **RUNS** (build 1012 / texture 1095 FPS → **Score 1052**) | 2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc | v130. drain#8 의 1163 대비 dip 은 run-to-run/thermal(반복 drain 후) — 여전히 GPU-class ~1000+ FPS, 기능 회귀 0. texture scene 이 8 MiB ring 재행사 |
| glmark2-es2 build+texture (ALR, drain#10 regression check) | **RUNS** (**Score 1009**) | 2026-06-02-breadth-fanout-drain | v132. WS-2 GLES2 19-op state-setter 커버리지(blend/depth/color/stencil 등) + decode 변경 후 회귀 0. Score 1009 (vs 1009–1163 prior; run-to-run/thermal, 여전히 GPU-class) |
| GPU LIVE/THROUGHPUT/SCREEN-CUBE 자기검사 (real Mali, software=false) | **DEVICE-VERIFIED** (`ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU THROUGHPUT: PASS` + `ALR GPU SCREEN CUBE: PASS`) | 2026-06-02-breadth-fanout-drain | v132/drain#10. WS-2 의 19개 신규 GLES2 state setter 와이어 인코딩(no-op→실 wire) + decode 변경이 GPU 파이프라인 무회귀. THROUGHPUT PASS 가 새 마커로 추가됨(회귀 게이트 device 체크리스트 항목) |
| guest libGLESv2 GLES2 state-setter 커버리지 (blend/depth/color/stencil 등 19 op) | wire-verified (shim 인코딩 + decode replay + off-device wire-check) | 2026-06-02-breadth-fanout-drain | WS-2. no-op 스텁이던 setter 들이 실 wire 인코딩으로 승급, decoder replay 케이스 추가, build-wire-check round-trip assert 로 off-device 증명. device GLES3+/Vulkan 은 별도 PENDING |
| ALR `GL_RENDERER` host passthrough (게스트가 실 Mali 문자열 인지) | **DEVICE-VERIFIED** (`GL_VENDOR: ARM` / `GL_RENDERER: Mali-G615 MC2` / `GL_VERSION: OpenGL ES 3.2`) | 2026-06-02-5ws-fanout-renderer-getpwuid-pkgfunc | drain#9, v130. 게스트 libGLESv2 shim 이 ring-header identity 블록으로 host 실 `glGetString` 을 그대로 보고(합성 문자열 아님; 벤더 하드코딩 없음 → 비-Mali GPU 에서도 정직). 부수: host Mali context = GLES **3.2**(GLES3+/Vulkan follow-up 참고) |
| glmark2 전체 14-scene 종합 Score | PENDING | — | duration↑ 또는 분할 launch 로 전 scene 1빌드 실행 필요(현 1163은 build+texture 2-scene 부분 종합). **ALR vs Mali-직접 비율**은 `docs/research/cp2-gpu-ratio-glmark2.md` (Mali-직접 baseline = PENDING_DEVICE) |
| guest Vulkan enumerate/props 마샬링 backbone (`alr_gpu_vk_{proto,decode,marshal_probe}`) | **DEVICE-VERIFIED** (`ALR VK ENUM MARSHAL: PASS`, `mode=mali-libvulkan`, VK_SUCCESS, Mali-G615 MC2, **api=1.3**, vendorID=0x13b5 ARM) | 2026-06-02-round5-vulkan-device-marshal, 2026-06-02-round4-milestones-drain | round-4 가 backbone 을 host-merge(`native_vk_marshal_test` ALL PASS, NDK 4-ABI), **round-5 가 device 로 승급**: vkCreateInstance/vkEnumeratePhysicalDevices/vkGetPhysicalDeviceProperties request 스트림이 게스트→SPSC ring→**호스트의 실 벤더 Mali libvulkan** 으로 디코드→reply 디코드. JNI `run_vk_marshal_mali_probe` + `ALR_VK_DECODE_REAL` 배선. 무회귀(GPU LIVE/THROUGHPUT/SCREEN-CUBE PASS, glmark2 1083, netsurf/foot/sdl2 rendered=true). **다음 = VK-M2 명령 body**(실 Vulkan draw/submit 마샬) + ICD + AHB color-attach: `loader-feature-gaps.md`(G3) |
| guest Vulkan VK-M2 render (device/queue/cmd 마샬 → 실 Mali) | **PARTIAL — device created=yes, clear-submit FAIL** (round-6 v138: `ALR VK RENDER MARSHAL: FAIL` `mode=mali-libvulkan ops decoded=11 transport=ok` `device created=yes gfx queue family=0 submit result=fail`) | 2026-06-02-round6-qt6-execreentry-vkrender, 2026-06-02-round5-vulkan-device-marshal | round-6 v138: VK-M2 body 가 게스트 `vkCreateDevice` + `vkGetDeviceQueue` + command-pool/buffer 를 **실 Mali libvulkan** 으로 마샬 → device created=yes, queue family 해결(gfx=0). 단 AHB-backed color attachment 로의 clear `vkQueueSubmit` 은 **FAIL**(render-pass/image-layout/AHB-import setup 디버깅 필요) — enumerate 경로는 무영향(`ALR VK ENUM MARSHAL: PASS` 유지). clear-submit fix 는 **round-7 진행 중**. 난이도·의존: `loader-feature-gaps.md`(G3) |
| guest Vulkan ICD + AHB present (VK-M3, full render pipeline) | PENDING | — | VK-M2 device/queue 마샬(device-created✓, clear-submit pending) 다음 단계. 게스트 `libvulkan_alr.so` ICD + manifest + AHB color-attach 렌더타깃 → 컴포지터 sample. Vulkan-native 게임/Wine·DXVK 잠금해제(전략: `gpu-guest-accel-strategy.md`). 난이도·의존: `loader-feature-gaps.md`(G3) |

## 디스플레이/입력 (L3)
| 항목 | 결과 | Evidence | 비고 |
|------|------|----------|------|
| 해상도/주사율 device-exact (1200×1920@90Hz, density=213) | RENDERS | v126-harfbuzz-fix-display-90hz, v127-xkb-config-root-gui-keymap-segv-fixed | `Display.getRealSize`+`refreshRate` JNI → wl_output mode+timerfd 90Hz |
| 입력 (touch/pointer/key) | USABLE (GIMP 경로) | v86-input-injection, v111-gimp-fully-usable-drawing | wl_pointer/wl_keyboard/wl_touch 전달; IME/wl_seat 갭은 WS-3 M4 |
| guest-GUI XKB 키맵 | 수정됨 (SIGSEGV fix, v127) | v127-xkb-config-root-gui-keymap-segv-fixed | `XKB_CONFIG_ROOT`를 rootfs-absolute 경로로 설정 → sig=11 카운트 0 |
| `zwp_linux_dmabuf` zero-copy present (AHB→external-OES) | **DEVICE-VERIFIED** (`ALR AHB ZEROCOPY IMPORT: PASS`; gtk3demo rendered=true frames 12→13, ws-3 WaylandPresenter, 단일 게이트 후 회귀 0) | 2026-06-01-drain5-cp4-dmabuf-present-single-gate, v114-ahb-zerocopy-present-live | CP-4 / ws-3 M2. AHB→EGLImage→external-OES import는 device 검증됨. 남은 정직한 nuance: guest-side dmabuf 프로토콜 광고(zwp_linux_dmabuf 게스트 advertise)는 WS-3 M2 잔여 디테일 |

## 미지원 / 부분 (현재 한계)
| 항목 | 상태 | 비고 |
|------|------|------|
| X11-only 앱 (실제 X11 클라이언트 표시) | PENDING | `Xwayland -version` 은 RUNS(drain#9, 위 패키지매니저 섹션) — Xwayland 바이너리는 게스트로 실행됨. 실제 X11 클라이언트를 Xwayland 에 붙여 화면에 띄우는 end-to-end 는 device 미검증 (WS-4 M4 잔여) |
| in-app `apt`/`dpkg` 실제 설치 | WALL: exec-re-entry (round-6 v138: B-1 device-fires, B-3 round-7 진행) | `dpkg-query`/`apt-get`/`Xwayland` **버전 보고 실행**은 device-PROVEN(drain#9). round-6 v138 에서 ADR-003 **B-1 execve path-rewrite 가 device-fires**(`alr exec x0=/bin/sh reason=rewrite` traps=1 rewrites=1) — 게스트 execve 가 trap 되고 x0 가 rootfs 로 재작성됨. 단 `apt install` 의 full dpkg fork+exec maintainer-script chain 은 여전히 `unpacked=false` — **B-3**(exec 된 child envp 에 abs-rootfs `LD_PRELOAD`/`ALR_ROOTFS` 재주입)이 필요하고 **round-7 진행 중**(device-pending). SSOT: `loader-feature-gaps.md`(G1) |
| OpenCL / 벤더 GPU compute | 미추진 | non-root/public-API 계약 위반; GIMP GEGL은 CPU (v114 honest-scope) |

---
*갱신 규칙:* device evidence 추가 시 해당 행을 evidence 파일명과 함께 갱신. host-only 진전만으로는 USABLE/RENDERS로 승급하지 않는다(device evidence 없이 완료 주장 금지).
