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
> baseline: 통합 트리 v137 (round-6 진행; round-5 drain#14 `docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`
> + CP-6 M-R2 `docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md`).
> 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).
>
> **round-6 진행 요약(이 갱신):** G1(exec-re-entry)은 round-4 device 벽에서 **ADR-003 으로 설계
> 정식화 + host 프로토타입 착수**로 전진(여전히 device-pending). G2(Qt6)는 round-5 가 SIGSEGV 원인을
> "overlay 미완"이 아니라 **EGL hwintegration(eglGetDisplay, ICD 부재)**로 device-규명 → `EGL→wl_shm`
> 강제로 전환. G3(Vulkan)는 round-5 에서 enumerate/props 마샬링이 **실 Mali 에 device-verified(VK 1.3)**
> 되어 backbone 이 PARTIAL→backbone-DONE 으로 승급(다음은 VK-M2 명령 body). CP-6 보류 박스에
> M-R5 svc-scan(read-only ROI 프로브)을 명시.

---

## 0. 한눈에 — 레버리지 순

레버리지 = "이 하나를 풀면 동시에 몇 개의 막힌 셀이 풀리는가". exec-re-entry 가 압도적 1위다.

| # | 기능 갭 | 잠금해제하는 것 | 난이도 | 의존 | 상태 |
|---|---------|----------------|--------|------|------|
| **G1** | **exec-re-entry** (rootfs 바이너리 `execve` → ALR 로더 재진입) | `apt`/`dpkg` 실제 설치, GIMP plugin(fork+exec), 임의 멀티프로세스 Linux 앱, 셸 파이프라인 | **높음** | clone3/fork 시맨틱(메모리: device-evidence-mali-android16); 설계=ADR-003 | **IN-PROGRESS** (ADR-003 설계 채택 + host 프로토타입 착수; device-pending) |
| **G2** | **Qt6 wl_shm 경로** (EGL hwintegration 회피 → 소프트웨어 client-buffer) | Qt6 GUI 앱 전반(analogclock→KDE/Qt 앱군) | 중간 | EGL 플러그인 비활성/wl_shm 강제(WS-4 env+overlay); G1 무관 | **IN-PROGRESS** (round-5: SIGSEGV=EGL hwintegration 규명 → `EGL→SHM` 전환중) |
| **G3** | **Vulkan render pipeline** (ring 명령 body + ICD + AHB color-attach) | Vulkan-native 게임, Wine/DXVK/VKD3D, ANGLE-GLES | 높음 | enumerate/props backbone(**device-verified✓**) → VK-M2 명령 body | **PARTIAL** (backbone device✓ round-5; VK-M2 body PENDING) |
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

**device 증거(벽).** round-4 drain#13:
```
apt-install: unpacked=false configured=false exec=GUEST EXEC FAIL
```
`dpkg -i` 가 unpack 후 helper(tar / maintainer-script preinst/postinst)를 `fork+execve` 로 띄우는데,
그 자식이 ALR 로더로 재진입하지 못해 `GUEST EXEC FAIL`. (evidence:
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

**round-6 진행(BLOCKED → IN-PROGRESS).** 설계가 **ADR-003**(`docs/design/adr-003-multiprocess-exec-reentry.md`)
으로 정식화되며 한 가지 통찰로 난이도가 재정의됐다: 자식을 두 클래스로 나누면 **(A) zygote-fork 자식
(renderer 다수)은 execve 를 안 거치므로 이미 매개된 주소공간 + 상속 seccomp + SEIZE-trace 로 _자동_
매개**, **(B) fresh-execve 자식(zygote/gpu)만 진짜 벽**이고 이건 "loader 재진입"이 아니라 — seccomp
필터가 execve 로 보존되고(커널 확정) supervisor 가 `PTRACE_O_TRACEEXEC` 로 자동 재포착 — 기존 trap
사이트 확장(신규 ptrace op 0, 신규 권한 0)으로 풀린다. 신규 작업은 (B-1) execve **x0** path mediation
(현 코드는 *at-style x1 만 읽고 exec 는 `is_exec` 로 건너뜀, `runtime_report.cpp` L2089-2092) + (B-2)
EVENT_EXEC 캐시 무효화. **급소(미검증) = execve envp 전파**: chromium 런처가 `LD_PRELOAD`/`ALR_ROOTFS`
를 자식 envp 로 넘기는지에 interposer 재주입이 인질로 잡힌다(ADR-003 §4 가정-1, WebSearch 상 chromium
이 거를 공산). host 프로토타입(WS-5): `tests/exec_map_model.py`(clone:exec 분류) + `tests/test_execve_pathrw.py`
(x0 vs x1 분기 결정모델). device 프로브 게이트 = M-R4-fork/execmap/envprop(ADR-003 §5, read-only).

**의존.** fork/clone3 시맨틱 안정화(메모리: device-evidence-mali-android16); 설계=ADR-003.
G2/G3/G4/G5 와 독립(이들은 G1 없이도 부분 진행 가능). 단 GIMP 풀 필터(babl/gegl) 와 apt 설치는
**G1 에 강하게 의존**. (B-1) x0-rewrite 실구현 착수는 M-R4-execmap 이 "exec 자식이 rootfs-내 절대경로
사용(`/proc/self/exe` 아님)"을 device 로 보인 후로 게이트.

---

## G2 — Qt6 wl_shm 경로 (EGL hwintegration 회피)

**무엇이 막혔나.** Qt6 overlay 는 stage + 클로저 완전(round-5 에서 35 reachable libs, `QT_QPA_PLATFORM=wayland`),
하지만 fork 된 게스트 child 가 **Qt platform init 도중 SIGSEGV** 한다. round-5 가 원인을 device 로
좁혔다: **overlay 미완이 아니라 EGL client-buffer hwintegration** 이다.

**device 증거(원인 좁힘).** round-5 drain#14(`docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`):
```
qt6gui-result: rendered=false frames=2217->2217   F/DEBUG (pid 24125) signal 11 SIGSEGV
```
machine-id 주입 + `QT_WAYLAND_DISABLE_WINDOWDECORATION` 으로도 **안 고쳐졌다**. qt6 overlay 클로저는
완전(`libQt6WaylandEglClientHwIntegration.so` 포함)하고, crash 는 **fork 된 게스트 child(pid 24125)**
의 Qt init 중 — Qt 가 wayland **EGL** client-buffer integration 을 골라 ICD 없는 `eglGetDisplay` 를
부르는 거동과 정합(ALR shim 은 GLES-마샬링이지 Qt 가 쓸 수 있는 EGL platform 이 아님). **앱 회귀 아님**
(app 생존, 이후 모든 probe 정상). round-4(`docs/evidence/2026-06-02-round4-milestones-drain.md`)가 SIGSEGV 를 처음
관측, round-5 가 "overlay 부족"이라는 초기 가설을 device 로 기각하고 EGL 으로 재분류했다.

**무엇을 잠금해제하나.** Qt6 GUI 앱 전반 — analogclock(데모)에서 시작해 Qt/KDE 위젯 앱군.
GTK3/native-Wayland/SDL2 에 이어 **다섯 번째 독립 toolkit** 으로 범용 GUI 셋 확장.

**난이도: 중간.** overlay 가 아니라 **client-buffer 백엔드 선택** 문제다 — Qt 의 EGL hwintegration
플러그인을 비활성(env 또는 overlay 에서 제외)하고 **wl_shm 소프트웨어 client-buffer** 로 강제해
ICD 없는 `eglGetDisplay` 경로를 피한 뒤, netsurf/foot/SDL2 처럼 컴포지터에 display-backed launch.
(다른 toolkit 이 전부 cairo/SW → wl_shm 으로 뜨는 것과 같은 경로로 Qt 를 끌어내림.) 대안은 Qt 가
받아들이는 EGL platform 제공이나 — 그건 G3(Vulkan/ANGLE) 또는 별도 EGL 어댑터 의존이라 큰 작업.

**의존.** WS-4 overlay/env(EGL 플러그인 비활성 + `QT_QPA_PLATFORM`/buffer-integration env). G1 무관
(단일 게스트로도 떠야 함). round-6 진행(`EGL→SHM` 전환).

---

## G3 — Vulkan render pipeline

**무엇이 막혔나.** 게스트 Vulkan 호출을 실 Mali Vulkan 드라이버까지 끌고 가는 풀 파이프라인.
**enumerate/props 마샬링 backbone 은 round-5 에서 실 Mali 에 device-verified(VK 1.3)** 되어 더 이상
막힌 부분이 아니다 — 남은 막힘은 **VK-M2 명령 body(실 Vulkan draw/submit 명령 마샬) + ICD + AHB
color-attach present**.

**증거(현 상태, backbone device-verified로 승급).** round-5 drain#14
(`docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`): `ALR VK ENUM MARSHAL: PASS`,
`mode=mali-libvulkan`, vkCreateInstance/vkEnumeratePhysicalDevices/vkGetPhysicalDeviceProperties
request 스트림이 게스트→SPSC ring→**호스트의 실 벤더 Mali libvulkan** 으로 디코드→reply 디코드,
`VK_SUCCESS` / `Mali-G615 MC2` / **api=1.3** / `vendorID=0x13b5(ARM)` 가 전부 마샬링 경로를 흘렀다.
(round-4 `docs/evidence/2026-06-02-round4-milestones-drain.md` 가 backbone 을 host-verify, round-5 가 device 로
승급 — JNI `run_vk_marshal_mali_probe`/`ALR_VK_DECODE_REAL` 배선.) 전략 전체는
`docs/research/gpu-guest-accel-strategy.md`(VK-M1/M2/M3 트랙).

**무엇을 잠금해제하나.** Vulkan-native 게임, **모든 Wine/DXVK/VKD3D**(전략 문서 옵션 A),
그리고 ANGLE→Vulkan 으로 robust GLES 경로(옵션 B). GUI 의 cairo-SW 가 아니라 게스트 *자체* 3D.

**난이도: 높음.** enumerate/props backbone(device✓) 다음은 (a) ring 으로 실 Vulkan **명령** 마샬
(VK-M2 body — buffer/image 생성·shader·pipeline·draw·submit), (b) 게스트 `libvulkan_alr.so` ICD +
manifest(VK-M3), (c) `VK_ANDROID_external_memory_AHB` color-attach 렌더타깃 → 컴포지터 sample.
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
  (목표 device-verified). round-6 에서 G1 은 ADR-003 설계 채택 + host 프로토타입으로 BLOCKED→IN-PROGRESS,
  G2 는 EGL 원인 device-규명으로 BLOCKED→IN-PROGRESS, G3 backbone 은 round-5 device-verify 로 PARTIAL
  내 backbone-구간 승급. **IN-PROGRESS 는 RENDERS/USABLE/DONE 이 아니다** — device 해소 전엔 셀 승급 금지.
- **새 측정 금지**(WS-5 HOST-ONLY) — 기존 `docs/evidence/` 인용만. device evidence 없이
  BLOCKED→DONE 승급 금지.
- **레버리지 순서 유지** — exec-re-entry(G1)가 최고 레버리지라는 판단은 "한 기능이 푸는 막힌 셀
  수"에 근거. 새 갭은 같은 기준으로 #0 표에 끼워 넣는다.
- **CP-6/chromium 은 보류** — 사용자 보류 상태이므로 이 SSOT 의 레버리지 목록에 올리지 않는다.
  CP-6 진행/분기는 `docs/research/cp6-status.md`(별도 SSOT, ADR-001/002/003 상호참조)가 소유.
