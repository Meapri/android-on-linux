# ALR 앱 호환 매트릭스 (WS-5 / L5)

> 임의 Debian arm64 앱이 ALR(native loader, 비root, public Android API only)에서 어디까지
> 도는지의 앱×결과 표. **device evidence가 있는 행만 USABLE/RUNS/RENDERS로 표기**한다.
> host-only/예정은 WIRED/PENDING. 이 표는 WS-5가 유지하며, 새 device evidence가 추가될 때마다 갱신.

작성 baseline: HEAD `0df74dc` (v124) → 통합 트리 v127 (CP-1 landed). 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878 SoC, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).

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

**성능 (device, v127 WS-1 M2):** 일반 CLI native-exec wall-clock(`exec_ms`) ~**18-20ms**(`dynhello`/`env`/`id`/`dash`/`alr-png-test`) = native 프로세스 수준. path-mediation은 **traps=0 device-verified**(in-process translate, supervisor 라운드트립 0). path-xlate cold 4334.7 ns/op(≈19.9 syscall units, raw getppid 218.3 ns/op 대비; 256-entry cache로 분할 상환). 단, native/PRoot baseline 미측정 → **% 오버헤드 비율은 아직 미산출(PENDING)**. 상세: `docs/evidence/2026-06-01-ws5-cpu-overhead-quantified.md`(WS-1 원본: `2026-06-01-ws1-m2-cpu-mediation-overhead.md`).

## GUI 툴킷 (L2/L3)
| 앱 | 툴킷 | 결과 | exec_ms | Evidence | 비고 |
|----|------|------|---------|----------|------|
| GIMP 3.0.2 | GTK3 | **USABLE** (터치로 File>New>1920×1080 캔버스>브러시 스트로크) | — | v111-gimp-fully-usable-drawing | cairo SW 렌더 → wl_shm 합성, dialog/menu/popup 입력 라우팅 |
| gtk3-widget-factory | GTK3 | RENDERS (rendered=true, frames 25→26) | 145 | v127-xkb-config-root-gui-keymap-segv-fixed | XKB keymap SIGSEGV 수정 후 Mali에 렌더(렌더 후 SIGABRT는 잔여 경고) |
| gtk3 데모 창 (`/bin/alr-gtk3-test`) | GTK3 | RENDERS (exit 0, ~50-lib 클로저, 5 frames) | 197 | v89-gtk3-renders, v90-real-gtk3-window | gtk_init backend=wayland; ~2088 file open이 rootfs로 mediation |
| in-process 이미지 디코드 (gdk-pixbuf PNG/JPEG/BMP/GIF) | — | RUNS (PASS) | — | v95-image-decode | shared-mime-info DB 보강 후 전 포맷 디코드 |
| 입력 주입 (`/bin/alr-input-test`/`alr-interactive-test`) | wl_seat | USABLE (received=24: pointer 10/key 8/touch 6) | (의도된 dispatch 대기) | v86-input-injection, v87-interactive-toolkit-pacing | redraw 루프(redraws=5 hits=2) |
| foot | (terminal) | WIRED (foot --version ver=true, rendered=false) | — | v126-harfbuzz-fix-display-90hz, v127-xkb-config-root-gui-keymap-segv-fixed | libfcft4/libutf8proc shim closure OK; pty 경로(WS-3/WS-4) |
| Qt5/Qt6 (qtwayland) | Qt | PENDING | — | — | WS-4 M2 |
| SDL2 | SDL | PENDING | — | — | WS-4 M2 / WS-2 M4 |
| netsurf | (경량 브라우저) | PENDING | — | — | WS-4 M2 |

## 브라우저 (Goal-2)
| 앱 | 결과 | Evidence | 비고 |
|----|------|----------|------|
| chromium `--version` | RUNS (in-process, exit 0, `Chromium 147.0.7727.137`) | chromium-runs-inprocess (v121) | 186MB static-PIE; DT_NEEDED 전체 클로저 링크; `--jitless` 불필요(V8 JIT W^X 검증 v120) |
| chromium `--dump-dom` / `--headless` (V8+render) | WALL | chromium-runs-inprocess (v121), v123/v124 commit msg | raw `svc` syscall-storm → LD_PRELOAD interposer 후킹 불가 → seccomp RET_TRACE 라운드트립 벽 (CP-6 / L1.M3 USER_NOTIF 후보) |

## GPU (L2)
host 백본(decode/ring/AHB-FBO/zero-copy present)은 Mali-G615 MC2에 픽셀 검증(software renderer=false). guest shim은 소스 wire-verified, device 라이브 cube/glmark2는 pending.

| 항목 | 결과 | Evidence | 비고 |
|------|------|----------|------|
| GPU host decode+draw (M1, shader+VBO+texture+draw) | Mali-verified (PASS, software=false) | v115-gpu-native-m1m2-mali | 32 ops, center=0,220,0 corner=26,26,102 exact |
| GPU host SPSC command ring (M2) | Mali-verified (PASS, 739B pushed==drained) | v115-gpu-native-m1m2-mali | virtual GL IDs, round-trip 0 |
| GPU host AHB render target (M4 host half) | Mali-verified (PASS, AHB-direct readback) | v117-gpu-native-ahb-rendertarget | FBO color attach via EGLImage |
| AHB zero-copy present (external-OES) | Mali-verified (live, `ahb=8 gltex=0 path=zerocopy`) | v113-ahardwarebuffer-zerocopy-proven, v114-ahb-zerocopy-present-live | GIMP 합성이 zero-copy로 present, glTexImage2D fallback 0회 |
| GPU live producer→ring→executor→AHB present | Mali-verified (8 frames presented+verified, two-thread) | v118-gpu-native-live-integration | loader fork(STEP B)만 남음 |
| guest libEGL/libGLESv2 shim (M3) | wire-verified (소스) | v118-gpu-native-live-integration | device 연결 pending (WS-2 M2) |
| 화면 cube present (M4 STEP B, loader fork) | PENDING | v119-gpu-screen-cube-present | guest fork + ring fd 상속 |
| glmark2-es2 (ALR, software=false) score | PENDING | — | WS-2 M3 / CP-2 |

## 디스플레이/입력 (L3)
| 항목 | 결과 | Evidence | 비고 |
|------|------|----------|------|
| 해상도/주사율 device-exact (1200×1920@90Hz, density=213) | RENDERS | v126-harfbuzz-fix-display-90hz, v127-xkb-config-root-gui-keymap-segv-fixed | `Display.getRealSize`+`refreshRate` JNI → wl_output mode+timerfd 90Hz |
| 입력 (touch/pointer/key) | USABLE (GIMP 경로) | v86-input-injection, v111-gimp-fully-usable-drawing | wl_pointer/wl_keyboard/wl_touch 전달; IME/wl_seat 갭은 WS-3 M4 |
| guest-GUI XKB 키맵 | 수정됨 (SIGSEGV fix, v127) | v127-xkb-config-root-gui-keymap-segv-fixed | `XKB_CONFIG_ROOT`를 rootfs-absolute 경로로 설정 → sig=11 카운트 0 |
| `zwp_linux_dmabuf` zero-copy present (guest→AHB-backed shm pool) | PENDING | v114-ahb-zerocopy-present-live (M3 deferred note) | WS-3 M2 / CP-4 (현재 compositor가 wl_shm→AHB로 1 CPU copy) |

## 미지원 (현재 불가)
| 항목 | 상태 | 비고 |
|------|------|------|
| X11-only 앱 | PENDING | Xwayland stage 필요 (WS-4 M4) |
| in-app `apt`/`dpkg` | 부분(WALL) | dpkg clone3 PRoot 한계 (메모리: device-evidence-mali-android16) |
| OpenCL / 벤더 GPU compute | 미추진 | non-root/public-API 계약 위반; GIMP GEGL은 CPU (v114 honest-scope) |

---
*갱신 규칙:* device evidence 추가 시 해당 행을 evidence 파일명과 함께 갱신. host-only 진전만으로는 USABLE/RENDERS로 승급하지 않는다(device evidence 없이 완료 주장 금지).
