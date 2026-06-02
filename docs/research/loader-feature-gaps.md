# ALR Loader Feature Gaps — what is blocked, and what it unlocks (WS-5 / L5 SSOT)

> **"지금 ALR 로더에서 막혀 있는 큰 기능이 무엇이고, 각각이 무엇을 잠금해제하는가"** 를 한 곳에
> 모은 SSOT. 자매 문서 `gui-universality-status.md`(어떤 GUI 가 device 에 뜨는가) 와
> `alr-compat-matrix.md`(앱×결과 표) 가 개별 셀에서 "막힘"을 표시하면, **왜 막혔는지 / 무엇을
> 풀어주는지 / 난이도·의존**은 여기로 모은다. 셀 여러 개가 같은 한 기능에 막혀 있을 때, 그 기능을
> 중복 서술하지 않고 여기 한 줄로 가리킨다.
>
> 소유: WS-5 (L5). HOST-ONLY — 새 device 측정 없음, 기존 `docs/evidence/` 인용만. 벤치 문서 아님
> (성능 숫자는 `docs/PERFORMANCE.md`/`cp2-gpu-ratio-glmark2.md`/`cp3-cpu-overhead-ratio.md`).
>
> baseline: 통합 트리 v138 (round-6 drain device-verified `docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`;
> round-5 drain#14 `docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`
> + CP-6 M-R2 `docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md`).
> 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).
>
> **round-6 device-verified 요약(이 갱신):** G1(최고 레버리지 exec-re-entry)은 **ADR-003 B-1(execve
> x0 path-rewrite)이 device-fires**(`alr exec x0=/bin/sh reason=rewrite` traps=1 rewrites=1; no-exec
> 게스트 0 무회귀)로 전진 — 단 full dpkg chain 은 **B-3(child envp 재주입) in-flight(round-7)**,
> apt-install 은 여전히 `unpacked=false`(device-pending). G2(Qt6)는 **DONE** — round-6 v138 에서
> `EGL→wl_shm` 전환(EGL QPA/HwIntegration 플러그인 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`)으로
> analogclock 이 **device-렌더(`qt6gui-result: rendered=true` frames 2215→2216)** → 7-toolkit 범용
> GUI 셋 완성. G3(Vulkan)는 round-6 VK-M2 가 **실 Mali 에 device created=yes**(vkCreateDevice/queue/cmd
> 마샬, gfx queue family=0)이나 clear `vkQueueSubmit`=FAIL(render-pass/AHB-import setup; clear-submit
> fix round-7 진행). CP-6 보류 박스에 M-R5 svc-scan(read-only ROI 프로브) 명시.

---

## 0. 한눈에 — 레버리지 순

레버리지 = "이 하나를 풀면 동시에 몇 개의 막힌 셀이 풀리는가". exec-re-entry 가 압도적 1위다.

| # | 기능 갭 | 잠금해제하는 것 | 난이도 | 의존 | 상태 |
|---|---------|----------------|--------|------|------|
| **G1** | **exec-re-entry** (rootfs 바이너리 `execve` → ALR 로더 재진입) | `apt`/`dpkg` 실제 설치, GIMP plugin(fork+exec), 임의 멀티프로세스 Linux 앱, 셸 파이프라인 | **높음** | clone3/fork 시맨틱(메모리: device-evidence-mali-android16); 설계=ADR-003 | **PARTIAL** (round-6: B-1 execve x0 path-rewrite **device-fires**; B-3 child envp 재주입 IN-FLIGHT round-7, apt-install 여전히 `unpacked=false`) |
| **G2** | **Qt6 wl_shm 경로** (EGL hwintegration 회피 → 소프트웨어 client-buffer) | Qt6 GUI 앱 전반(analogclock→KDE/Qt 앱군) | 중간 | EGL 플러그인 비활성/wl_shm 강제(WS-4 env+overlay); G1 무관 | **DONE** (round-6 v138: `EGL→wl_shm` → analogclock **device-렌더** rendered=true frames 2215→2216) |
| **G3** | **Vulkan render pipeline** (ring 명령 body + ICD + AHB color-attach) | Vulkan-native 게임, Wine/DXVK/VKD3D, ANGLE-GLES | 높음 | enumerate/props backbone(**device-verified✓**) → VK-M2 명령 body | **PARTIAL** (backbone device✓ round-5; round-6 VK-M2 device created=yes 이나 clear `vkQueueSubmit`=FAIL → clear-submit fix round-7) |
| **G4** | **netsurf 네트워크/입력 interaction** | 실 웹 페이지 로드(자산 fetch)·클릭/스크롤 입력 | 중간 | WS-3 입력 라우팅 + 게스트 네트워크 정책 | **PARTIAL** (정적 `about:welcome` RENDERS✓) |
| **G5** | **GLES3 전체 scene 커버리지** (전체 glmark2 14-scene + GLES3+) | 풀 GPU 벤치 매트릭스, GLES3 앱/에뮬레이터, GTK4 GL 렌더러 | 중간 | shim op 커버리지(WS-2); G3 와 일부 공유 | **PARTIAL** (build+texture Score~1000+✓, 2-scene) |

> **CP-6 / chromium 은 사용자(찬우) 보류.** chromium `--dump-dom`/`--headless` 의 raw-`svc`
> syscall-storm 벽(`alr-compat-matrix.md` §브라우저, `chromium-native-plan.md`)은 이 SSOT 의
> 레버리지 목록에 **올리지 않는다** — 명시적으로 보류 상태이며 round-6 범위 밖이다. 단 보류는
> *프로브/계측*까지 막지 않는다: round-6 에서 CP-6 M-R2 가 chromium `--version` 을 device-실행하고
> verdict=**mediation-negligible**(traps=0/emul=1)을 냈으며(`docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md`),
> M-R5-svcscan(`svc` rewrite ROI **read-only** 프로브)·M-R1(USER_NOTIF 가용성 프로브)은 보류와 무관히
> *평가만* 진행 가능하다. CP-6 진행 SSOT 와 다음 분기는 **별도 문서 `docs/research/cp6-status.md`**
> (ADR-001/002/003 상호참조). 이 문서(loader-feature-gaps)는 storm 벽을 *갭으로 격상하지 않는다*.

---

## G1 — exec-re-entry (최고 레버리지)

**무엇이 막혔나.** rootfs 안의 바이너리가 `execve`(또는 `posix_spawn`/`fork`+`exec`)로 또 다른
rootfs 바이너리를 띄울 때, 그 자식이 다시 ALR 네이티브 로더를 통과해 in-process glibc 게스트로
실행되는 경로(=exec-re-entry)가 아직 없다. 현재 로더는 **앱이 직접 launch 하는 단일 게스트**만
in-process 로 띄운다.

**device 증거(B-1 device-fires, full chain 은 여전히 벽).** round-6 v138:
```
alr exec x0=/bin/sh reason=rewrite     traps=1 rewrites=1 clone_events=7
apt-install: unpacked=false
```
ADR-003 **B-1**(execve x0 path-rewrite)이 device 에서 발화한다 — 게스트 `execve(/bin/sh)` 가
EVENT_SECCOMP 에서 trap 되고 **program path(x0)가 rootfs 로 재작성**(argv/envp 불변)된 뒤 게스트가
fork(7 clones). no-exec 게스트는 `traps=0 rewrites=0`(무회귀). 그러나 `dpkg -i` 의 full
fork+exec maintainer-script chain 은 여전히 `unpacked=false` — 자식이 ALR interposer 를 상속하지
못해 rootfs path-mediation 없이 실행되기 때문. (evidence:
`docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`; round-4 첫 관측
`docs/evidence/2026-06-02-round4-milestones-drain.md`.) clone3 계열 PRoot 한계는 별도 메모리
(device-evidence-mali-android16)에 기록.

**무엇을 잠금해제하나(레버리지).**
- **`apt install` / `dpkg -i` 실제 설치** — 패키지 매니저가 maintainer-script 를 실행할 수 있게 됨
  (현재 `apt-get --version`/`dpkg-query` *실행*만 device-PROVEN, drain#9).
- **GIMP plugin** — GIMP 가 babl/gegl 필터·script-fu·python-fu plugin 을 별도 프로세스로 fork+exec.
  babl/gegl 모듈 *로드*는 확인됐으나(drain#13 `gimp-filter: ok=true`), 풀 필터 exercise 는 이 경로 의존.
- **임의 멀티프로세스 Linux 앱 / 셸 파이프라인** — `sh -c 'a | b'`, 빌드 스크립트, 데몬 더블-fork 등
  "프로세스를 띄우는 프로세스"가 전부 여기 걸린다. 단일-게스트 가정을 넘어 **범용성의 다음 큰 도약**.

**난이도: 높음.** fork/clone 후 자식 주소공간에서 로더를 재초기화(seccomp/PCGATE 재설치, ring fd
상속, ld.so 재진입)해야 하고, `execve` 가로채기가 in-process map 교체로 동작해야 한다. 단순 추가가
아니라 별개의 큰 로더 마일스톤.

**round-6 진행(IN-PROGRESS → PARTIAL: B-1 device-fires, B-3 in-flight).** 설계 **ADR-003**
(`docs/design/adr-003-multiprocess-exec-reentry.md`)는 자식을 두 클래스로 나눈다: **(A) zygote-fork
자식(renderer 다수)은 execve 를 안 거치므로 이미 매개된 주소공간 + 상속 seccomp + SEIZE-trace 로
_자동_ 매개**, **(B) fresh-execve 자식(zygote/gpu)만 진짜 벽**이고 이건 "loader 재진입"이 아니라 —
seccomp 필터가 execve 로 보존되고(커널 확정) supervisor 가 `PTRACE_O_TRACEEXEC` 로 자동 재포착 —
기존 trap 사이트 확장(신규 ptrace op 0, 신규 권한 0)으로 풀린다. **round-6 v138 에서 (B-1) execve
x0 path-rewrite 가 device-fires**(`alr exec x0=/bin/sh reason=rewrite` traps=1 rewrites=1) — 현
코드가 *at-style x1 만 읽고 exec 는 건너뛰던 것(`runtime_report.cpp`)을 넘어 execve **x0** 를 읽어
rootfs 로 재작성. **남은 급소 = (B-3) execve 된 child 로의 interposer 재주입**: 자식 envp 에
abs-rootfs `LD_PRELOAD` + `ALR_ROOTFS` 를 주입해야 자식이 rootfs path-mediation 을 상속한다 — 이게
없어 `apt-install: unpacked=false`. **B-3 는 round-7 진행 중**(device-pending). host 프로토타입(WS-5):
`tests/exec_map_model.py`(clone:exec 분류) + `tests/test_execve_pathrw.py`(x0 vs x1 분기 결정모델).
device 프로브 게이트 = M-R4-fork/execmap/envprop(ADR-003 §5, read-only).

**의존.** fork/clone3 시맨틱 안정화(메모리: device-evidence-mali-android16); 설계=ADR-003.
G2/G3/G4/G5 와 독립(이들은 G1 없이도 부분 진행 가능). 단 GIMP 풀 필터(babl/gegl) 와 apt 설치는
**G1 에 강하게 의존**. (B-1) x0-rewrite 실구현 착수는 M-R4-execmap 이 "exec 자식이 rootfs-내 절대경로
사용(`/proc/self/exe` 아님)"을 device 로 보인 후로 게이트.

---

## G2 — Qt6 wl_shm 경로 (EGL hwintegration 회피) — **DONE (round-6 v138)**

**무엇이 막혔었나(해소됨).** Qt6 overlay 는 stage + 클로저 완전(round-5 에서 35 reachable libs,
`QT_QPA_PLATFORM=wayland`)이었으나 fork 된 게스트 child 가 **Qt platform init 도중 SIGSEGV** 했다.
round-5 가 원인을 device 로 좁혔다: **overlay 미완이 아니라 EGL client-buffer hwintegration**(Qt 가
wayland **EGL** client-buffer integration 을 골라 ICD 없는 `eglGetDisplay` 호출). round-6 v138 이 이를
**`EGL→wl_shm` 백엔드 강제로 device-해소**했다.

**device 증거(해소).** round-6 v138(`docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`):
```
qt6gui-result: rendered=true frames=2215->2216  bin=.../qt6/examples/widgets/widgets/analogclock/analogclock
```
fix = qt6 overlay 에서 **EGL QPA platform 플러그인 + compositor HwIntegration 플러그인 제외** +
`QT_WAYLAND_DISABLE_HW_INTEGRATION=1`. Qt 가 **wl_shm backing store** 를 사용해 analogclock 창이
SurfaceView 에 합성(frame counter 전진, crash 없음). round-4
(`docs/evidence/2026-06-02-round4-milestones-drain.md`)가 SIGSEGV 를 처음 관측, round-5 가 "overlay
부족" 가설을 기각하고 EGL 으로 재분류, round-6 이 wl_shm 강제로 닫았다.

**무엇을 잠금해제했나.** Qt6 GUI 앱 전반 — analogclock(데모)에서 시작해 Qt/KDE 위젯 앱군.
GTK3/native-Wayland/SDL2 에 이어 **다섯 번째 독립 toolkit** 으로 범용 GUI 셋을 **7 toolkit** 으로 확장
(`gui-universality-status.md` §1). 잔여(증분, 회귀 아님): 더 많은 Qt/KDE 앱 + Qt 입력 interaction.

**난이도: 중간(해소됨).** overlay 가 아니라 **client-buffer 백엔드 선택** 문제였다 — Qt 의 EGL
hwintegration 플러그인을 overlay 에서 제외하고 **wl_shm 소프트웨어 client-buffer** 로 강제해 ICD 없는
`eglGetDisplay` 경로를 피한 뒤, netsurf/foot/SDL2 처럼 컴포지터에 display-backed launch.

**의존.** WS-4 overlay/env(EGL 플러그인 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`). G1 무관
(단일 게스트로 device-렌더). **round-6 DONE.**

---

## G3 — Vulkan render pipeline

**무엇이 막혔나.** 게스트 Vulkan 호출을 실 Mali Vulkan 드라이버까지 끌고 가는 풀 파이프라인.
**enumerate/props 마샬링 backbone 은 round-5 에서 실 Mali 에 device-verified(VK 1.3)**, **round-6 의
VK-M2 가 device/queue/command 마샬을 실 Mali 에 device created=yes** 했다 — 남은 막힘은 **clear
`vkQueueSubmit`(render-pass/image-layout/AHB-import setup) + ICD + AHB color-attach present**.

**증거(현 상태).** round-5 drain#14(`docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`):
`ALR VK ENUM MARSHAL: PASS`, `mode=mali-libvulkan`, `VK_SUCCESS`/`Mali-G615 MC2`/**api=1.3**/
`vendorID=0x13b5(ARM)`. round-6 v138(`docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`):
```
ALR VK RENDER MARSHAL: FAIL
mode=mali-libvulkan  ops decoded=11  transport=ok
device created=yes  gfx queue family=0  submit result=fail
```
즉 VK-M2 body 가 게스트 `vkCreateDevice` + `vkGetDeviceQueue` + command-pool/buffer 를 **실 Mali
libvulkan** 으로 마샬(device created=yes, queue family 해결) — device/queue/command marshalling 은
device 에 입증됐다. 단 AHB-backed color attachment 로의 **clear `vkQueueSubmit` 은 FAIL**(render-pass/
image-layout/AHB-import setup 디버깅 필요). enumerate 경로는 무영향(`ALR VK ENUM MARSHAL: PASS` 유지).
clear-submit fix 는 **round-7 진행 중**. 전략 전체는 `docs/research/gpu-guest-accel-strategy.md`
(VK-M1/M2/M3 트랙).

**무엇을 잠금해제하나.** Vulkan-native 게임, **모든 Wine/DXVK/VKD3D**(전략 문서 옵션 A),
그리고 ANGLE→Vulkan 으로 robust GLES 경로(옵션 B). GUI 의 cairo-SW 가 아니라 게스트 *자체* 3D.

**난이도: 높음.** device/queue 마샬(device-created✓) 다음은 (a) clear `vkQueueSubmit` 성공(render-pass/
image-layout/AHB-import 정합 — round-7), (b) 게스트 `libvulkan_alr.so` ICD + manifest(VK-M3),
(c) `VK_ANDROID_external_memory_AHB` color-attach 렌더타깃 → 컴포지터 sample.
Mali proprietary Vulkan 한계(no transform_feedback/geometry/tess)는 전략 문서에 기록.

**의존.** GPU ring/AHB present 인프라(CP-2/CP-4, device-verified✓) 재사용. G5(GLES3) 와 일부 공유
(ANGLE 경로). G1 무관.

---

## G4 — netsurf 네트워크/입력 interaction

**무엇이 막혔나.** netsurf-gtk 가 정적 `about:welcome` 을 **RENDERS**(drain#12, rendered=true,
5 threads)하지만, 실제 웹 페이지 로드(네트워크 자산 fetch)와 클릭/스크롤 입력 interaction 은 미검증.

**증거(현 상태).** `docs/evidence/2026-06-02-netsurf-browser-renders.md` — `rendered=true`
frames 2214→2217, 그러나 잔여(증분, 회귀 아님)로 "전체 페이지 자산/네트워크/입력 interaction" 명시.

**무엇을 잠금해제하나.** 실 브라우징(원격 페이지 로드) + 사용자 입력으로 페이지 조작 = netsurf 가
데모를 넘어 **실사용 브라우저**가 됨.

**난이도: 중간.** (a) 게스트 네트워크 정책(소켓 syscall → Android 네트워크, untrusted_app 권한 내),
(b) WS-3 입력 라우팅을 GTK 위젯 hit-test 까지 전달(GIMP 에서 입력 주입 USABLE✓ 이므로 토대는 있음).

**의존.** WS-3 입력(M4). G1 무관(단일 프로세스 브라우저). round-4/5 증분.

---

## G5 — GLES3 전체 scene 커버리지

**무엇이 막혔나.** glmark2 가 **build+texture 2-scene** 으로 Mali 에 렌더(Score~1000+,
software=false, device-verified)하지만, **전체 14-scene** 과 GLES3+ 기능(현재 GLES2 19-op
state-setter 커버리지)은 미완.

**증거(현 상태).** `docs/evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md`,
`docs/evidence/2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md`(Score 1163),
`docs/evidence/2026-06-02-breadth-fanout-drain.md`(19-op GLES2 커버리지). host Mali context 는
이미 GLES **3.2** 로 확인됨(GL_RENDERER passthrough, drain#9).

**무엇을 잠금해제하나.** 풀 GPU 벤치 매트릭스(ALR vs Mali-직접 ratio 의 분자 완성,
`cp2-gpu-ratio-glmark2.md`), GLES3 앱/에뮬레이터, **GTK4 GL 렌더러**(실앱 GL → Mali, WS-2 M4).

**난이도: 중간.** shim op 커버리지를 GLES3 entry point 까지 확장(현 GLES2 19-op → GLES3),
decoder replay + wire-check 케이스 추가(host 게이트, off-device round-trip). 전체 14-scene 은
duration↑ 또는 분할 launch.

**의존.** WS-2 shim/decoder. G3(Vulkan/ANGLE) 와 ANGLE 경로 공유 가능. G1 무관.

---

## 갱신 규칙

- 갭이 **풀리면**(device evidence): 해당 G# 를 BLOCKED/PARTIAL → DONE 으로 내리고, 가리키던
  `gui-universality-status.md`/`alr-compat-matrix.md` 셀을 같은 evidence 로 승급. 이 문서가
  "막힘"의 SSOT 이므로 다른 문서는 여기를 가리키기만 한다(중복 서술 금지).
- **상태 어휘**: `BLOCKED`(device 벽 확인, 진전 없음) → `IN-PROGRESS`(설계 채택/host 프로토타입/
  원인 device-규명은 됐으나 device 해소 evidence 없음) → `PARTIAL`(일부 device-verified) → `DONE`
  (목표 device-verified). **round-6 v138 전이**: **G2 는 EGL→wl_shm device-렌더로 IN-PROGRESS→DONE**
  (analogclock rendered=true), G1 은 B-1 execve x0 path-rewrite device-fires 로 IN-PROGRESS→PARTIAL
  (B-3 child envp 재주입은 round-7 in-flight, apt-install 아직 `unpacked=false`), G3 는 VK-M2 device
  created=yes 이나 clear-submit FAIL 로 PARTIAL 유지(round-7 clear-submit fix). **IN-PROGRESS/PARTIAL
  은 RENDERS/USABLE/DONE 이 아니다** — device 해소 전엔 셀 승급 금지(G1 apt-install·G3 clear-submit
  은 아직 device-실패라 셀 승급 불가).
- **새 측정 금지**(WS-5 HOST-ONLY) — 기존 `docs/evidence/` 인용만. device evidence 없이
  BLOCKED→DONE 승급 금지.
- **레버리지 순서 유지** — exec-re-entry(G1)가 최고 레버리지라는 판단은 "한 기능이 푸는 막힌 셀
  수"에 근거. 새 갭은 같은 기준으로 #0 표에 끼워 넣는다.
- **CP-6/chromium 은 보류** — 사용자 보류 상태이므로 이 SSOT 의 레버리지 목록에 올리지 않는다.
  CP-6 진행/분기는 `docs/research/cp6-status.md`(별도 SSOT, ADR-001/002/003 상호참조)가 소유.
