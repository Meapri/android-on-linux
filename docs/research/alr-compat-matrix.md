# ALR 앱 호환 매트릭스 (WS-5 / L5)

> 임의 Debian arm64 앱이 ALR(native loader, 비root, public Android API only)에서 어디까지
> 도는지의 앱×결과 표. **device evidence가 있는 행만 USABLE/RUNS/RENDERS로 표기**한다.
> host-only/예정은 WIRED/PENDING. 이 표는 WS-5가 유지하며, 새 device evidence가 추가될 때마다 갱신.

작성 baseline: HEAD `0df74dc` (v124) → 통합 트리 v127 (CP-1 landed). 디바이스 `R5KL20B6S3X` (SM-X236N, Mali-G615, Android 16).

## 상태 범례
- **USABLE** — device에서 사람이 실제로 조작 가능(입력 포함).
- **RENDERS** — device에서 창/픽셀이 화면에 나옴.
- **RUNS** — device에서 프로세스가 정상 실행/종료(GUI 아닐 수 있음).
- **WALL** — 실행되나 성능/데드락 벽에 막힘(아키텍처적 한계).
- **WIRED** — launch 경로/stage는 준비, device 렌더 확인은 pending.
- **PENDING** — 미착수/의존 대기.

## CLI / 코어 (L1 native loader)
| 앱/바이너리 | 결과 | Evidence | 비고 |
|-------------|------|----------|------|
| `/bin/dynhello` (dynamic glibc) | RUNS (exit 0, `alr-dyn-ok`) | v79-dynamic-loader, v122-fastpath | 동적 ld.so in-process |
| `/usr/bin/env`, `/usr/bin/id` | RUNS (exit 0) | v122-fastpath | path-mediation traps=0 |
| `/bin/dash` | RUNS (exit 0) | v122-fastpath | |
| `/bin/alr-png-test` | RUNS (exit 0) | v95-image-decode | libpng 디코드 |
| `/bin/alr-wl-test` | RUNS (exit 0) | v82-wayland-compositor-up | wl client |
| `/bin/alr-pixman-test` | RUNS (exit 0) | v83-pixman-toolkit-window | |
| `/usr/bin/gimp-console-3.0` | RUNS (exit 127, expected) | v122-fastpath | missing deps, 중재 정상 |

→ 이 집합이 **회귀 게이트**(`bench/regression_gate.py`)의 기준선. mediation 불변식: `pcgate=1 interpose=1 traps=0 rewrites=0`.

## GUI 툴킷 (L2/L3)
| 앱 | 툴킷 | 결과 | Evidence | 비고 |
|----|------|------|----------|------|
| GIMP 3.0 | GTK3 | **USABLE** (터치로 File>New>캔버스>브러시) | v111-gimp-fully-usable-drawing | cairo SW 렌더 → wl_shm 합성 |
| gtk3 데모 창 | GTK3 | RENDERS | v89-gtk3-renders, v90-real-gtk3-window | |
| gtk3-widget-factory | GTK3 | RENDERS | (v127 CP-1) | guest-GUI XKB keymap SIGSEGV 수정 후 렌더 |
| foot | (terminal) | WIRED | (v126 launch wiring) | stage tar 준비 |
| Qt5/Qt6 (qtwayland) | Qt | PENDING | — | WS-4 M2 |
| SDL2 | SDL | PENDING | — | WS-4 M2 / WS-2 M4 |
| netsurf | (경량 브라우저) | PENDING | — | WS-4 M2 |

## 브라우저 (Goal-2)
| 앱 | 결과 | Evidence | 비고 |
|----|------|----------|------|
| chromium `--version` | RUNS (in-process) | chromium-runs-inprocess (v121) | `--jitless` 불필요(V8 JIT W^X 검증 v120) |
| chromium `--dump-dom` (V8+render) | WALL | v123/v124 commit msg | syscall-storm → ptrace 라운드트립 벽 (L1.M3 USER_NOTIF 후보) |

## GPU (L2)
| 항목 | 결과 | Evidence | 비고 |
|------|------|----------|------|
| GPU host 백본 (decode/ring/AHB-FBO) | Mali-verified | v115-gpu-native-m1m2-mali, v117-ahb-rendertarget | M1/M2 |
| AHB zero-copy present (host) | proven | v113/v114-ahb-zerocopy-present | |
| guest libEGL/libGLESv2 shim | wire-verified (소스) | gpu-native-live-integration v118 | device 연결 pending (WS-2 M2) |
| glmark2-es2 (ALR, software=false) | PENDING | — | WS-2 M3 / CP-2 |

## 디스플레이/입력 (L3)
| 항목 | 결과 | Evidence | 비고 |
|------|------|----------|------|
| 해상도/주사율 device-exact (1200×1920@90Hz) | RENDERS | v126/v127 | wl_output geometry device-exact |
| 입력 (touch/pointer/key) | USABLE (GIMP 경로) | v86-input-injection, v111 | IME/wl_seat 갭은 WS-3 M4 |
| guest-GUI XKB 키맵 | 수정됨 (SIGSEGV fix) | v127 CP-1 | `XKB_CONFIG_ROOT`를 rootfs-absolute 경로로 설정 |
| `zwp_linux_dmabuf` zero-copy present | PENDING | — | WS-3 M2 / CP-4 |

## 미지원 (현재 불가)
| 항목 | 상태 | 비고 |
|------|------|------|
| X11-only 앱 | PENDING | Xwayland stage 필요 (WS-4 M4) |
| in-app `apt`/`dpkg` | 부분(WALL) | dpkg clone3 PRoot 한계 (메모리: device-evidence-mali-android16) |

---
*갱신 규칙:* device evidence 추가 시 해당 행을 evidence 파일명과 함께 갱신. host-only 진전만으로는 USABLE/RENDERS로 승급하지 않는다(device evidence 없이 완료 주장 금지).
