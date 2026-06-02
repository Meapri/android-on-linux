# Chromium BROWSER run-plan — CR-1..CR-5 (SSOT)

> **"chromium runs `--version`" 에서 "browser 가 실제로 화면에 페이지를 렌더"까지의 정직한
> 사다리.** 각 마일스톤마다 *정확한 flag set* · *blockers* · *device-drain gate(무슨 logcat 라인이
> 통과를 증명하나)* 를 한 곳에 못 박는다. chromium 은 **사용자 보류(held)** 상태이므로 이 문서는
> *실행 경로의 SSOT 설계*만 담고, 어떤 셀도 device evidence 없이 RUNS/PASS 로 승급하지 않는다.
>
> 소유: WS-5(L5). **HOST-ONLY** — 새 device 측정 없음, 기존 `docs/evidence/` 인용만. **벤치 문서
> 아님**(성능 숫자는 `docs/PERFORMANCE.md` / `docs/research/cp3-cpu-overhead-ratio.md`). GPU flag
> 사다리는 자매 문서 `docs/research/chromium-gpu-path.md`. CP-6 의 storm/exec 벽 진단 SSOT 는
> `docs/research/cp6-status.md`, 잠긴 *범용* 로더 기능은 `docs/research/loader-feature-gaps.md`.
>
> baseline: 통합 트리 v144. 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2, Android 16,
> 1200×1920@90Hz, untrusted_app, targetSdk 35).

---

## 0. 한 줄 현 위치 + 사다리 요약

**chromium-headless-shell(Chromium 147.0.7727.137, 186MB static-PIE + ~200-file glibc 클로저)이
ALR 네이티브 로더로 device 에서 `--version` 실행(child exit=0, in-process, 비root, public-API)됐다**
(`docs/evidence/2026-06-01-device-SM-X236N-chromium-runs-inprocess.md`). 이건 "바이너리가 뜨고 +
풀 closure 를 링크하고 + 깨끗이 종료" 레벨이다. **브라우저로서 페이지를 렌더하는 것**은 아직
아니다 — 그 사다리가 CR-1..CR-5 다.

| 마일스톤 | 무엇이 증명되나 | near-term? | 핵심 의존 |
|---|---|---|---|
| **CR-1** | single-proc headless 가 **로컬/data 페이지를 렌더 + DOM dump**(net/GPU/멀티proc 0) | **YES (near-term)** | 없음(현 로더로 가능) — 단 render 경로 측정창(§CR-1) |
| **CR-2** | += 진짜 **https URL** 페이지 렌더 | 중기 | net overlay(NSS/CA/DNS closure) |
| **CR-3** | += **GPU**(SwiftShader→ANGLE→우리 shim) accel render | 중기 | GPU flag 사다리(자매 문서) |
| **CR-4** | **GUI 창**(`--ozone-platform=wayland`)이 우리 compositor 에 픽셀을 올림 | 중기 | ozone wl 프로토콜(`xdg_wm_base`+`wl_shm`) |
| **CR-5** | `--single-process` 떼기 = **멀티프로세스 zygote** | **장기 (G1-gated)** | **G1 (re-mapped-guest exec, in-flight)** |

**정직 경계.** *near-term 은 CR-1 뿐*이다. CR-2/CR-3/CR-4 는 각자 staged-lib/compositor 작업이
선결이지만 멀티프로세스 벽은 안 건드린다(전부 `--single-process` 유지). **CR-5 만이 G1(in-process
re-map exec re-entry, `docs/research/loader-feature-gaps.md` G1)에 게이트**되고, G1 은 메커니즘+map/jump
가 device-proven 이나 재-맵 게스트 SIGILL + `/proc/self/exe` pass-through 가 남아 **아직 RUNS 아님**
(in-flight). 따라서 CR-5 는 *장기*다.

---

## CR-1 — single-proc headless render of a local/data page (NO net/GPU/multiproc) — **near-term**

**목표.** chromium 엔진이 **실제로 페이지를 파싱·레이아웃·렌더**하고 그 결과 DOM 을 stdout 으로
dump 한다. `--version` 은 init 만 돈다 — CR-1 은 처음으로 *render 경로*(HTML 파서 + Blink layout +
V8)를 통과시키는 마일스톤이다. 네트워크·GPU·멀티프로세스 0.

**정확한 flag set:**
```
chromium-headless-shell \
  --single-process --no-zygote --no-sandbox \
  --disable-gpu \
  --disable-dev-shm-usage \
  --user-data-dir=<rootfs-writable-dir> \
  --dump-dom \
  data:text/html,<html><body><h1>alr</h1></body></html>
```
- `--single-process --no-zygote` : 멀티프로세스 zygote(=CR-5/G1)를 우회 — 모든 browser/renderer/
  utility 가 **한 프로세스**(extra execve 0).
- `--no-sandbox` : chromium 자체 seccomp-bpf + namespace 샌드박스를 끈다. ALR 의 PCGATE
  path-mediation 필터와 stack-conflict 하면 우리가 trace 할 syscall 을 chromium 이 DENY 하므로
  **필수**(옵션 아님; `chromium-native-plan.md` Scout 2/4).
- `--disable-gpu` : software raster(GPU 0). GPU 사다리는 CR-3.
- `--disable-dev-shm-usage` : `/dev/shm` 없음(비root) 우회.
- `--dump-dom` : render 후 직렬화된 DOM 을 stdout 으로 — **엔진이 페이지를 실제 렌더했다는 증거**.
- target = `data:` URL(또는 `file:` rootfs 경로) — net 0. CR-2 가 `https:` 로 올린다.

V8 JIT 는 끄지 않는다(`--jitless` 불필요) — JIT W^X 사이클이 device-PASS
(`docs/evidence/2026-06-01-device-SM-X236N-v120-jit-wx-cycle.md`, `cycles_ok=8/8`).

**Blockers (정직):**
- **render 경로 측정창(THE near-term blocker).** `--version` 은 `traps=0`로 즉시 끝나지만
  `--dump-dom` 은 과거 "멀티스레드-ptrace 데드락"으로 stall 했다. **PR #2
  (`docs/design/adr-chromium-storm-deadlock.md`)가 그 데드락을 오진(misdiagnosis)으로 재진단** —
  best-가설은 데드락이 아니라 **무거운 single-init 이 `alarm(25s)` 측정창을 첫 워커 clone 전에
  만료**시킨 것(utime 가 1→5 로 기어가며 *진짜 진행 중*). 따라서 CR-1 의 1순위 작업은 supervisor
  re-tweak 가 아니라 **측정창 분리**: `ALR_GUEST_ALARM_S`(예 120s/180s)로 dynamic alarm 파라미터화
  → render 가 끝까지 진행하는지 device 1회로 가른다(measure-first). chromium 은 이미 120s 를 받는다.
- supervisor ptrace 오버헤드: render 는 init 보다 훨씬 많은 syscall 을 친다. CR-1 은 *완료*가 목표지
  속도가 아니다(속도는 CP-6 의 svc-rewrite/USER_NOTIF A/B 분기, `cp6-status.md` §2).
- `--user-data-dir` 는 rootfs-내 쓰기 가능 경로여야(profile/cache 생성).

**device-drain gate (무슨 로그가 통과를 증명하나):**
```
ALR NATIVE LOADER GUEST EXEC: PASS
alr native loader child exit=0 signal=0
alr native loader guest stdout=<!DOCTYPE html> ... <h1>alr</h1> ... </html>
```
즉 **child exit=0 + guest stdout 에 직렬화된 DOM(`<h1>alr</h1>` 가 살아있는 `</html>` 로 끝나는
문서)** 이 찍히면 = "Chromium 이 페이지를 렌더했다". exit≠0 이거나 stdout 이 비면 FAIL. 측정창
만료(`alarm` SIGALRM)로 끊기면 `ALR_GUEST_ALARM_S` 를 늘려 재측정(measure-first).

> **★ WS-1 핸드오프 — CR-1 정확한 flag string + drain gate**
> flag string(개행-구분 argv, 로더가 파싱):
> `<binary>\n--single-process\n--no-zygote\n--no-sandbox\n--disable-gpu\n--disable-dev-shm-usage\n--user-data-dir=/data/.../rootfs-tmp\n--dump-dom\ndata:text/html,<html><body><h1>alr</h1></body></html>`
> "Chromium rendered the page" = logcat 의 `alr native loader guest stdout=` 라인이 `<h1>alr</h1>`
> 를 포함한 `</html>` 종료 직렬 DOM 을 담고 **`child exit=0`**. (stall 이면 `ALR_GUEST_ALARM_S=180`
> 로 재드레인.)

---

## CR-2 — += real https URL (network) — 중기

**목표.** CR-1 의 software-render 엔진에 **진짜 네트워크**를 붙여 원격 https 페이지를 렌더.

**정확한 flag set:** CR-1 과 동일하되 target 을 실제 URL 로:
```
chromium-headless-shell \
  --single-process --no-zygote --no-sandbox \
  --disable-gpu --disable-dev-shm-usage \
  --user-data-dir=<rootfs-writable-dir> \
  --dump-dom \
  https://example.com
```

**Blockers:**
- **net overlay closure.** https = NSS/NSPR(이미 chromium closure 에 일부) + **CA 인증서 번들**
  (`ca-certificates`, `/etc/ssl/certs`) + DNS resolution(`getaddrinfo`/`/etc/resolv.conf` →
  Android resolver 가 비root 에서 도달 가능해야). chromium 은 자체 net stack 이라 libcurl 류는
  불필요하나 **CA bundle 부재면 TLS handshake 실패**.
- socket syscall(`socket`/`connect`/`sendto`)은 PCGATE 비-path → RET_ALLOW(중재 0). 우려는
  SELinux 가 untrusted_app 의 outbound socket 을 막느냐인데 일반 인터넷 권한은 허용(앱 manifest
  `INTERNET`).
- `--user-data-dir` 의 cache/cookie write 가능 경로 필요.

**device-drain gate:**
```
alr native loader child exit=0 signal=0
alr native loader guest stdout=<!DOCTYPE html>... (example.com 의 실제 <title>Example Domain</title>)
```
즉 stdout DOM 에 **원격 페이지 고유 마커**(`Example Domain` 등)가 있으면 = 네트워크+렌더 PASS. TLS
실패는 `child exit≠0` + stderr 의 `ERR_CERT_*`/`net::ERR_*` 로 식별 → CA bundle 스테이징 후 재드레인.

---

## CR-3 — += GPU — 중기

**목표.** CR-2 의 software render 를 **GPU-accelerated** 로 — 자매 문서 `chromium-gpu-path.md` 의
flag 사다리(`--disable-gpu` 제거 → `--use-gl=swiftshader` → `--use-gl=angle` → 우리 shim)를 탄다.

**정확한 flag set (사다리 1칸: SwiftShader, 가장 안전):**
```
chromium-headless-shell \
  --single-process --no-zygote --no-sandbox --disable-dev-shm-usage \
  --user-data-dir=<rootfs-writable-dir> \
  --use-gl=angle --use-angle=swiftshader \
  --enable-unsafe-swiftshader \
  --dump-dom  https://example.com
```
(다음 칸 `--use-gl=angle --use-angle=gles-egl` = ANGLE → 우리 `libEGL.so.1`/`libGLESv2.so.2` shim;
정확한 lib 의존/사다리는 `chromium-gpu-path.md`.)

**Blockers:**
- 어느 GPU 백엔드냐 = `chromium-gpu-path.md` 의 staged-lib 매트릭스(SwiftShader 는 chromium 번들
  `libvk_swiftshader.so`, software Vulkan + execmem; ANGLE 는 번들 `libGLESv2.so`/`libEGL.so` 또는
  우리 shim).
- **single-process 에서 GPU 는 in-proc GPU 스레드**라 멀티proc(CR-5/G1)와 독립 — CR-3 은
  `--single-process` 를 유지한 채 GPU 만 켠다.
- 우리 shim 경로(`--use-gl=angle --use-angle=gles-egl`)는 `LD_LIBRARY_PATH` 로 발견되며 soname
  이 **정확히 `libEGL.so.1`/`libGLESv2.so.2` + unversioned symlink** 여야(`chromium-native-plan.md`
  Scout 3). dmabuf/GBM(`/dev/dri`) 경로는 **안 만족** — `zwp_linux_dmabuf_v1` 미광고로 swap 을
  `wl_shm` 에 묶고 AHB presenter 가 합성(CR-4 와 맞물림).

**device-drain gate:**
```
alr native loader child exit=0
chromium stderr: GL_RENDERER 가 software 아님(SwiftShader/ANGLE/우리 shim) 로깅
+ (shim 백엔드면) alr_gpu ring 에 GLES OP 가 흘러 Mali executor 가 소비(gpushim 카운터>0)
```
즉 **GPU 백엔드가 software-disabled 가 아니고**(`--disable-gpu` 제거 효과) + render 가 exit=0 으로
완료. 우리 shim 백엔드면 ring 카운터(glmark2 와 동일 계측, `cp2-gpu-ratio-glmark2.md` 계보)로 OP
flow 를 증명.

---

## CR-4 — GUI window via `--ozone-platform=wayland` — 중기

**목표.** headless 가 아닌 **진짜 창**을 우리 in-app Wayland compositor 에 올려 페이지를 화면에
렌더(SurfaceView). 입력 전, 픽셀-온-스크린이 마일스톤.

**정확한 flag set:**
```
chromium-headless-shell ... (또는 full chromium) \
  --single-process --no-zygote --no-sandbox --disable-dev-shm-usage \
  --user-data-dir=<rootfs-writable-dir> \
  --ozone-platform=wayland \
  --disable-gpu                      (CR-4 는 sw 버퍼; GPU 는 CR-3 와 합성 가능) \
  https://example.com
```
(`WAYLAND_DISPLAY` 가 우리 compositor 소켓을 가리켜야.)

**Blockers:**
- **필수 wl 인터페이스(없으면 chromium 이 창을 안 엶):** `wl_compositor`, `wl_shm`,
  **`xdg_wm_base`(stable xdg-shell)**. 강력권장: `wl_seat`(입력) + `wl_output`(sizing).
  GIMP 셋 대비 **추가 = `xdg_wm_base` + `wl_shm`**(`chromium-native-plan.md` Scout 3). 입력/키맵은
  WS-3 의 compositor 작업 + WS-4 의 xkb-data(이미 rootfs 존재) 위에.
- 픽셀은 `wl_shm`(software 버퍼)로 도착 → 우리 AHB presenter 가 SurfaceView 에 합성. dmabuf 경로
  **미광고**(swap 을 wl_shm 에 묶음).
- D-Bus **불필요**(경고만, 실행됨).

**device-drain gate:**
```
chromium stderr: ozone-platform=wayland 로 wl_compositor/xdg_wm_base 바인드 성공
compositor: 새 xdg_toplevel + wl_shm buffer commit (chromium surface) 로그
+ SurfaceView 에 페이지 픽셀(스크린샷/육안)
```
즉 **compositor 가 chromium 의 `xdg_toplevel` + `wl_shm` 버퍼 commit 을 받고** SurfaceView 에
페이지가 보이면 PASS. 창이 안 열리면 보통 `xdg_wm_base` 미광고(stderr 에 `xdg_wm_base not
available`) → 인터페이스 추가 후 재드레인.

---

## CR-5 — drop `--single-process` (multiprocess zygote) — **장기 (G1-gated)**

**목표.** `--single-process` 를 떼서 chromium 을 **본래의 멀티프로세스**(browser + zygote +
renderer + gpu + utility)로 — 진짜 브라우저 아키텍처. 이게 chromium-native 의 최종 일반성 증명.

**정확한 flag set:**
```
chromium-headless-shell (또는 full chromium) \
  --no-sandbox --disable-dev-shm-usage \
  --user-data-dir=<rootfs-writable-dir> \
  --no-zygote                        (각 자식이 fresh-execve; zygote COW fork 보다 매개 쉬움) \
  --ozone-platform=wayland  ...      (CR-3/CR-4 의 GPU/display 와 합성)
  https://example.com
```
`--single-process` **제거** = 자식 프로세스가 `fork()`+`execve(/proc/self/exe)` 로 뜬다.

**Blockers (왜 장기인가):**
- **G1 (re-mapped-guest exec re-entry) — in-flight, RUNS 아님.** chromium 의 fresh-execve 자식
  (zygote/gpu)은 커널 execve 로 안 풀린다 — round-9 가 Option S(커널-execve stub)를 **W^X 로 DEAD**
  입증(`app_data_file:execute` neverallow, `exec_events=0`). round-10 이 ADR-003-v3 **in-process
  재-맵(커널 execve 0)** 의 메커니즘(v141) + map/jump(v143)을 device-proven 했으나, **재-맵된 static
  glibc 게스트가 startup 에서 SIGILL**(R11 이 mapper 정확성은 고침 — single-span mapping; 단
  `ALR_REEXEC_INPROC` 는 opt-in/default-OFF, 글로벌 ON 은 onCreate 프로브 시퀀스를 wedge → 승급은
  **시퀀스-레벨 통합**이 남음). 상세 = `docs/research/loader-feature-gaps.md` G1 +
  `docs/research/r12-remaining-gaps-status.md` g1-seqint 레인.
- **`/proc/self/exe` pass-through.** chromium 자식은 `/proc/self/exe`(= 우리 케이스에선 Android
  linker64 가 아니라 rootfs chromium 바이너리)를 re-exec → rootfs-바이너리 재-맵과 **구분되는 별도
  pass-through** 필요.
- Mojo IPC(AF_UNIX + SCM_RIGHTS fd-passing + memfd 공유메모리), per-renderer GPU ring(현 SPSC 단일
  consumer → N-ring/tagging), seccomp stacking — 전부 멀티proc 에서만 surface(`chromium-native-plan.md`
  Scout 4 의 6 갭).

**device-drain gate:**
```
ALR-INPROC: mapped, jumping entry=...        (자식마다, 커널 execve 0)
+ chromium: --type=zygote / --type=renderer / --type=gpu-process 자식이 ALR 매개 아래 RUNS
+ child exit=0, 페이지 렌더(CR-1..CR-4 의 gate 동시 충족)
```
즉 **각 fresh-execve 자식이 `ALR-INPROC: mapped, jumping` 으로 in-process 재-맵 + 커널 execve 0** 에
도달하고(SIGILL 없이) browser 가 멀티proc 으로 페이지를 렌더하면 PASS. 이건 G1 이 RUNS 로 승급된
*후에만* 가능 — 그래서 CR-5 는 장기다.

---

## device-req gate 요약 (WS-1 드레인 체크리스트)

- [ ] **CR-1** — `--single-process --no-zygote --no-sandbox --disable-gpu --dump-dom data:` →
  logcat `child exit=0` + `guest stdout=` 에 `<h1>alr</h1>` 담은 `</html>` 종료 DOM
  (stall 이면 `ALR_GUEST_ALARM_S=180`). **near-term.**
- [ ] **CR-2** — += `https://example.com` → stdout DOM 에 `Example Domain` (net overlay: CA bundle).
- [ ] **CR-3** — += GPU(`--use-gl=angle --use-angle=swiftshader` …) → GL_RENDERER ≠ software + exit=0
  (사다리/lib = `chromium-gpu-path.md`).
- [ ] **CR-4** — `--ozone-platform=wayland` → compositor 가 chromium `xdg_toplevel`+`wl_shm` commit
  수신 + SurfaceView 픽셀 (compositor: `xdg_wm_base`+`wl_shm`).
- [ ] **CR-5** — drop `--single-process` → 각 fresh-execve 자식 `ALR-INPROC: mapped, jumping` (커널
  execve 0, SIGILL 없이) + 멀티proc 렌더. **G1-gated, 장기.**

> **정직 규칙.** device evidence(파일명 명기) 추가 시에만 RUNS/PASS 승급. host-only 진전
> (flag 설계/측정 설계)만으로는 어떤 CR 도 "풀림"으로 올리지 않는다. CR-5 는 G1(in-flight)에
> 게이트 — G1 이 RUNS 가 되기 전엔 CR-5 는 *장기/미착수*. chromium 은 사용자 보류이므로 실제 드레인
> 착수는 보류 해제 + 해당 device 게이트 PASS 후.

---

## 교차참조

| 문서 | 무엇 |
|---|---|
| `docs/research/chromium-gpu-path.md` | CR-3 의 GPU flag 사다리 + 어느 staged-lib 가 어느 백엔드를 받나 |
| `docs/research/cp6-status.md` | render storm/exec 벽 진단 SSOT(measure-first, in-process-remap 트랙) |
| `docs/research/loader-feature-gaps.md` | G1(exec re-entry) 잠긴-기능 SSOT — CR-5 의 게이트 |
| `docs/research/r12-remaining-gaps-status.md` | R12 in-flight 6레인(g1-seqint 가 CR-5 의 시퀀스 통합) |
| `docs/research/chromium-native-plan.md` | Goal-2 Phase A–F + 4-scout 정찰(Phase B/C/D/E 와 CR 매핑) |
| `docs/design/adr-chromium-storm-deadlock.md` | render "데드락" = 오진; measure-first(CR-1 의 측정창) |
| `docs/design/adr-003-multiprocess-exec-reentry.md` | ADR-003-v3 in-process 재-맵(CR-5/G1 의 설계) |
