# 제품·범용성 로드맵 — SSOT (4-트랙 통합 + 메인 엔진 의존성 맵)

> **"ALR 이 제품으로서 임의 Linux arm64 GUI 앱을 비root·public-API 로 Android 에 띄우고 설치·실행한다"**
> 를 향한 **제품/범용성 트랙(연구·제품 담당) 전체**의 단일 진실 소스.
> 이 세션이 소유한 **4 트랙**(product-ux / fakeroot / apt-pipeline / chromium-storm)의 현 상태를
> 하나로 통합하고, **무엇이 메인(chromium 엔진) 진전에 게이트되며 무엇이 독립인가**의 의존성 맵을
> 고정한다. **이 문서는 문서-only** — 본체 파일(`MainActivity.kt`/`runtime_report.cpp`/
> `libalr_interpose.c`/`alr_inproc_reexec.c`/version stamp)은 **읽기만** 하고 수정하지 않는다.
>
> 담당 분담(메모리 `fakeroot-research-ownership`): **메인 세션 = chromium 엔진 + exec-re-entry
> in-process re-map + 본체 launch-wiring**. **이 세션(연구/제품) = 제품 UX + fakeroot + v2 apt
> 파이프라인 + 범용성/호환표**. 본체에 대한 **요구·제안 diff·DEVICE-REQ** 까지만 소유.
>
> 작성 baseline: 이 세션 worktree `research/apt-pipeline` (base `main` 450b6d8 = v150). 단
> **메인 세션 `ws-1` 은 이 baseline 보다 크게 앞서 있다** — `da1be8b` v159 = **CR-1 ACHIEVED**
> (chromium in-process render), `bee3949` v160 = CR-2 NETLINK workaround. §3 의존성 맵은 그
> 앞선 상태를 반영한다(read-only 로 `git log ws-1` 확인). 호스트=Darwin → host 증명 천장 =
> **빌드/구조/메커니즘**; device 동작은 **DEVICE-REQ** 로만 주장.

---

## 0. 목표(§0) — 무엇을 향해 가는가

5-세션 오케스트레이션 §0(`docs/research/orchestration-5session-plan.md`)의 최상위 목표:

> **모든 Linux arm64 앱을 Android 에서 CPU+GPU 풀 네이티브 가속으로, 오버헤드 ~0 에 실행**
> — 비root(untrusted_app), public Android API only, W^X-safe, in-process, 단일 APK.

이 문서가 담당하는 **"제품·범용성"** 축은 그 목표를 **사용자가 실제 쓰는 제품**으로 구체화한 것이다:

1. **설치 범용성** — 임의 Debian/Ubuntu arm64 앱을 device 에서 실제로 **설치**(`apt`/`dpkg` 또는
   stage-tar overlay). [apt-pipeline + fakeroot 트랙]
2. **실행 범용성** — GTK/Qt/SDL/터미널/브라우저 등 임의 toolkit GUI 앱이 device 에서 **뜬다**.
   [메인 엔진 + WS-2/3/4 — 이 문서는 호환표로 추적]
3. **제품 표면** — 검증 하니스가 아니라 **런처 + 인앱 카탈로그 + 권한/파일연동(SAF)** 을 가진
   데스크탑형 제품. [product-ux 트랙]

§0 대비 이 담당이 채운 % 는 §5 에 정직하게 정량한다.

---

## 1. 한 줄 결론

제품·범용성은 **세 표면의 곱**이다 — (A) **설치**(apt/fakeroot, **host-ready**), (B) **실행**
(toolkit 범용성, **device-증명 7 toolkit**), (C) **제품 UX**(런처/카탈로그/SAF, **host-ready 스켈레톤**).
이 담당의 host-측 작업은 **A·C 가 host-ready 로 완성**(빌더·shim·staging·Compose UI·설치엔진·SAF 모델
모두 빌드·검증)됐고, **B 는 메인 엔진 + WS-2/3/4 가 device 로 7 toolkit 을 이미 증명**했다.
**남은 단일 device 선결은 G1(exec-re-entry child re-map)** — 그러나 **메인 세션이 이 baseline
이후 G1 의 게이트(supervisor 데드락)를 device 로 풀었다**(v157, CR-1 v159 = chromium multithread
fork+ptrace 앱이 `traps=69 rewrites=56` 으로 in-process 렌더). 따라서 **"메인 엔진이 풀리는 순간"**
이라는 가정은 **상당 부분 이미 현실**이고, 남은 정확한 잠금은 **fresh-execve child(=dpkg 의
`zstd`/`sh`, GIMP plugin, 멀티프로세스 chromium)의 static-startup re-map**(CR-5 / B-3 종단)이다.
§3 이 무엇이 그 한 점에 게이트되고 무엇이 독립인지를 정확히 가른다.

---

## 2. 4-트랙 보드 (host-ready 정도 + device-req 한눈에)

| 트랙 | 무엇 | PR/브랜치 | host-ready | device-req | 메인 의존 |
|------|------|-----------|-----------|-----------|-----------|
| **product-ux** (PR #1) | 런처 + 인앱 카탈로그(Compose 4화면) + `AlrRuntime`/`FakeAlrRuntime` + 설치엔진 + 권한맵 + SAF fd-프록시 + 데스크탑엔트리 | `research/product-ux` (5 commits, **open**) | **HIGH** — Compose UI 전 화면 Fake 런타임으로 동작, 설치엔진/카탈로그/SAF host-model 검증; ADR-004 §0 5결정 확정 | 본체 빌드 통합(build.gradle Compose + Manifest 등록 + Fake→실 런타임 배선); v1 동작은 **G1 독립**(stage-tar) | **부분** (§3) |
| **fakeroot** (PR #3) | non-root dpkg unpack 의 fakeroot 게이트: `getuid→0` + `chown/chmod` no-op+메타DB + `stat`-family 오버레이 (`libalr_fakeroot.so`, 별도 LD_PRELOAD) | `research/fakeroot` (1 commit) + worktree tools | **HIGH** — 23/23 심볼, `fakeroot-stage.tar` 실빌드(73456B aarch64 ELF), `--selftest` ALL PASS, dpkg-unpack host-model 종단 증명 | §5 체인 LD_PRELOAD 배선 + device staging; 메타연산(getuid/chown/stat)은 **G1 독립** | **부분** (§3) |
| **apt-pipeline** (PR #4) | noble `apt`/`dpkg` 런타임 closure + admindir scaffold + `hello.deb` → `apt-dpkg-stage.tar`; breadth catalog(12 앱 0-unsat closure 예측) | `research/apt-pipeline` (this worktree) | **HIGH** — closure 58-pkg 18.7MiB 0-unsat, self-contained 14 front-end aarch64 ELF, §5-E CONFORMANT, 39+3 pytest PASS | 종단 `unpacked=true` 는 **G1 게이트**(dpkg 가 압축해제기 child fork+exec) | **종단 게이트** (§3) |
| **chromium-storm** (PR #2) | 데드락=오진 재진단 — chromium 워커 clone 이 120s window 안에 device 도달 확증(데드락 아닌 ptrace-overhead 벽) | `research/chromium-storm` (**머지됨**, main 0-ahead) | **DONE(머지)** — 재진단이 메인의 v150→v157 데드락 fix 의 길을 깔았음 | — (메인이 이 진단 위에서 CR-1 달성) | **메인에 흡수됨** |

곱(AND) 의미: **제품 종단** ⟺ (설치 ∧ 실행 ∧ UX). C(UX)·A(설치 host-측)은 1(host-ready),
B(실행)는 device-증명 7 toolkit. **하드 제약**(불변, 전 트랙): 비root, public Android API only,
W^X-safe(exec-mem/seccomp/ptrace **없는** 순수 libc interposition — fakeroot 한정), SELinux 미접촉,
in-process, **device evidence 없이 완료 주장 금지**, version stamp 불변, **소유 밖 파일 읽기만**.

---

## 3. 메인(chromium 엔진) 의존성 맵 — 무엇이 게이트되고 무엇이 독립인가 (핵심)

> 이 절이 이 문서의 존재 이유다. 이 세션의 4 트랙 각 항목이 **메인의 무엇**(G1 exec-re-entry /
> 데드락 / 본체 launch-wiring)에 게이트되는지, 아니면 **독립**인지를 정확히 가른다.
> **메인 현 상태(read-only `git log ws-1` 로 확인한 사실):**

### 3.0 메인 엔진 현 좌표 (이 baseline v150 이후 ws-1 이 앞선 사실)

| 메인 마일스톤 | 커밋 | device 사실 | 이 담당에의 의미 |
|--------------|------|------------|------------------|
| **supervisor 데드락 FIX** | `450b6d8` v150 → `aa2e5e1` v157 | v150 = LISTEN-park 한 tid 를 group-stop END 에 resume; **v157 = SIGTRAP livelock FIXED** (genuine brk trap forward + stall-watchdog) | G1 을 막던 **멀티스레드 ptrace 벽이 device 로 뚫림** — dpkg/GIMP 같은 멀티스레드/fork 게스트의 supervisor 게이트가 더는 데드락 아님 |
| **B-3 child envp 재주입** | `ac1ef5c` (R7-B, merged) | execve/execveat child 의 envp 에 abs-rootfs `LD_PRELOAD`+`ALR_ROOTFS` 재주입(ADR-003 §3 B-3) | dpkg 가 fork+exec 한 `zstd`/`sh` child 가 interposer/rootfs 를 물려받는 **정확한 메커니즘이 트리에 존재** |
| **B-1 execve path-rewrite** | v138 device-fires | `alr exec x0=/bin/sh reason=rewrite` traps=1 rewrites=1 (no-exec 게스트는 0 무회귀) | child 프로그램 경로가 rootfs 로 재작성됨 — fork+exec 의 **첫 절반이 device-증명** |
| **CR-1 ACHIEVED** | `da1be8b` v159 | chromium `--single-process` headless **렌더** in-process: `traps=69 rewrites=56 exec_ms=41027`, DOM marker `ALR-CR1-OK` | **가장 어려운 glibc 앱(V8+멀티스레드+ICU+수백 .so)이 device 렌더** = G1 의 in-process re-map·supervisor·interposer 전체가 실앱에서 작동 |
| **CR-2 (진행)** | `bee3949` v160 | NETLINK-bind EACCES workaround → chromium 이 네트워크 request **발행**(연결 자체는 DNS 2차 블로커) | 네트워킹 앱의 connectivity-probe 벽 일부 해소(설치 트랙엔 부수적) |
| **CR-5 멀티프로세스 re-exec** | `645e009` (design-only) | `/proc/self/exe`→guest-binary re-map; **아직 device 미착수** | **fresh-execve child 의 static-startup re-map** = dpkg 압축해제기·GIMP plugin·멀티프로세스 chromium 의 종단 잠금 (= 이 문서의 진짜 게이트) |

**정리:** 이 SSOT baseline(v150)이 가정했던 "G1 = WALL(데드락)" 은 **메인이 v157/CR-1 으로 상당 부분
해제**했다. 남은 정확한 잠금은 **CR-5(=B-3 의 종단: fresh-execve static-startup child re-map)** 한
점이다. 즉 "메인이 G1 을 풀면" 은 더는 막연한 미래가 아니라 **CR-1 으로 큰 절반이 device-증명됐고,
CR-5 한 점이 남은** 구체적 상태다.

### 3.1 이 담당 4 트랙 × 메인 의존성 분해

| 트랙 항목 | 메인 의존? | 게이트 대상 | 독립으로 device 가능한 부분 |
|-----------|-----------|------------|----------------------------|
| **fakeroot: 메타연산** (getuid→0, chown no-op, stat root:root 오버레이) | **독립** | — | dpkg 자기 프로세스 안에서 일어남 → fakeroot preload 만으로 비root 게이트 통과·`st_uid==0` device 증명 가능(§ apt-pipeline-ssot §7 중간 하위게이트) |
| **fakeroot: 체인 LD_PRELOAD 배선** | **게이트(launch-wiring)** | 본체 §5 launch 계약(`runtime_report.cpp` LD_PRELOAD 결정 + `MainActivity` drain) | 메커니즘·shim 은 host-ready; 배선만 본체 |
| **apt-dpkg: stage 추출·closure** | **독립** | — | overlay 추출은 기존 `extractOverlayTar` 패턴 — G1 무관(stage-tar 는 fork-exec 0) |
| **apt-dpkg: 종단 `unpacked=true`** | **게이트(G1 종단)** | **CR-5/B-3 종단** (dpkg 가 `data.tar.zst`→`zstd` fork+exec, maintainer `sh` fork+exec) | extract child re-map 필요 → **CR-5 게이트** (메타연산만 독립) |
| **breadth catalog: 12 앱 0-unsat closure** | **독립** | — | host closure 예측은 메인 무관; device 설치는 위 `unpacked` 에 종속 |
| **product-ux: Compose UI 4화면 + FakeAlrRuntime** | **독립** | — | Fake 런타임으로 전 화면 device 동작(목 데이터) — 런타임 0 의존 |
| **product-ux: v1 카탈로그 설치(stage-tar)** | **독립** | — | ADR-004 D3: v1 = closure→stage-tar overlay 풀기(`extractOverlayTar`) → **dpkg fork-exec 우회 = G1 독립** |
| **product-ux: v2 카탈로그 설치(인-게스트 apt)** | **게이트(G1 종단)** | **CR-5/B-3 종단** | ADR-004 D3 v2 = 인-게스트 `apt-get install` → exec-re-entry 선결 |
| **product-ux: 실 런타임 배선**(Fake→AlrRuntime) | **게이트(launch-wiring)** | 본체 컴포지터 bindSurface(WS-3) + install() 배선 | UI/모델 host-ready; 배선만 본체/통합 |
| **product-ux: SAF fd-프록시 임포트** | **게이트(launch-wiring)** | 본체 fd 주입(게스트 open→host SAF fd) | fd-프록시/강등(copy) **모델은 host-ready**; copy-폴백은 G1 독립 |

**한 줄:** 이 담당의 host 작업 중 **G1 에 게이트되는 것은 오직 "종단 설치 = `unpacked=true` /
인-게스트 apt"** 와 **본체 launch-wiring(배선)** 둘뿐이다. **나머지(메타연산·stage 추출·closure
예측·Compose UI·v1 stage-tar 설치·SAF copy-폴백)는 전부 G1 독립**으로 device 가능하다.

---

## 4. "메인 엔진이 풀리는 순간 즉시 맞물리는" 항목 (+ 각 DEVICE-REQ)

> 메인의 **CR-5(=B-3 종단 fresh-execve child re-map)** 가 device-증명되는 순간, 이 담당의 host-ready
> 산출물이 **추가 host 작업 없이** 곧바로 device 종단으로 전환되는 항목. 각 항목의 device 전제와
> 게이트 식을 명기한다(= 통합/device 세션이 drain 으로 켤 체크리스트).

| # | 즉시 맞물리는 것 | host-ready 산출물 | DEVICE-REQ | 게이트 식 |
|---|------------------|-------------------|------------|-----------|
| 1 | **`dpkg -i hello.deb` → `unpacked=true`** | `fakeroot-stage.tar` + `apt-dpkg-stage.tar` + `hello.deb` (전부 실빌드) | `ALR-V2-apt-unpack` (apt-pipeline-ssot §7) | `unpacked=true ∧ stat(풀린파일)==root:root ∧ mknod=placeholder 인지` |
| 2 | **`apt install <pkg>` (12 후보 중)** — nano/htop/jq/tree/galculator/xterm… | `breadth_catalog.py` 0-unsat closure 12/12 → 각 stage-tar 빌드 | `ALR-V2-apt-install-breadth` (신규 — closure 행별 device 설치) | 후보별: `closure 적용 ∧ unpacked=true ∧ (GUI 면) 창 RENDERS` |
| 3 | **product-ux v2 인-게스트 apt 설치** | `install_engine.py` + `apt_catalog.py` + `install_plan.py` (host-model 검증) | `ALR-V2-product-install` (ADR-004 D3 v2) | UI install() → 인-게스트 apt-get → `unpacked=true` |
| 4 | **GIMP plugin / GEGL 필터 종단**(67 op .so 실 필터 output) | babl/gegl overlay STAGED(WS-4) + 모듈 LOAD 확인 | (호환표 babl/gegl 행) | GIMP plugin fork+exec re-map → 필터 픽셀 output |
| 5 | **product-ux 실 런타임 풀 배선** | Compose UI + 설치엔진 + SAF 프록시 (host-ready) | `ALR-PRODUCT-UX-LIVE` (integration-guide §0 5단계) | Launcher→Catalog→install→RunningSurface 창 RENDERS |

**주의(정직):** #1 의 **메타연산 하위게이트**(getuid==0, dpkg 가 `requires superuser` 넘음,
chown 후 stat root:root)는 **CR-5 없이도 fakeroot 단독으로 먼저 device 증명 가능** — 이게
"메인 풀림 전" 에 켤 수 있는 유일한 종단-부분 device 게이트다. #1 의 종단 `unpacked=true` 만 CR-5 대기.

---

## 5. §0 목표 대비 이 담당이 채운 % (정직)

> 정량은 **device-증명 ÷ (host-ready + device-증명 + 미착수)** 로, 축마다 따로. host-ready 는
> "코드/구조/메커니즘 완성, device 만 남음" — 완료 아님. **천장 표기 필수.**

### 5.1 설치 범용성 (A)
- **host-ready: ~95%** — fakeroot shim(23/23) + apt-dpkg closure(0-unsat) + admindir + `hello.deb` +
  staging 래퍼 + breadth 12 앱 closure + dpkg-unpack host-model 종단 증명. **빌드·구조·메커니즘 완성.**
- **device-증명: ~30%** — dpkg/apt/Xwayland **버전 보고 실행**은 device-PROVEN(drain#9, v130); B-1
  execve path-rewrite device-fires(v138). 그러나 **`unpacked=true` 는 PENDING**(CR-5 종단 게이트).
- **천장:** 호스트=Darwin → host 는 staging+메커니즘까지. 종단 설치 = device-only.
- **남은 것:** §4 #1/#2 DEVICE-REQ (CR-5 게이트). 메타연산 하위게이트는 CR-5 전에도 가능.

### 5.2 실행 범용성 (B) — *이 문서는 추적, 증명은 메인+WS-2/3/4*
- **device-증명: 7 toolkit** — GIMP(USABLE)/gtk3-widget-factory/gtk3-demo/foot/netsurf-gtk(브라우저,
  5 threads)/SDL2/Qt6. GTK3/native-Wayland/멀티스레드 브라우저/SDL2/Qt6 5종 독립 toolkit.
  + **chromium in-process render(CR-1, v159)** = 가장 어려운 앱 device-증명.
- **남은 것:** X11 클라이언트 화면 표시(WS-4 M4), Vulkan VK-M2 clear-submit(round-7), 멀티윈도우(WS-3 M3).
- **이 담당 기여:** breadth catalog 가 "어떤 앱이 closure 상 설치 가능한가" 를 예측해 **B 의 표적**을
  공급(12 GUI/CLI). 실행 자체는 메인 영역.

### 5.3 제품 UX (C)
- **host-ready: ~85%** — Compose 4화면 + RunningSurface + AlrRuntime/Fake + ViewModel(MVVM) +
  설치엔진 + 추천카탈로그 + 권한맵 + SAF fd-프록시/강등 모델 + 데스크탑엔트리 + manifest 스키마.
  ADR-004 §0 5결정 확정(D1 단일포그라운드/D2 하이브리드 UI/D3 v1-stage-tar/D4·D5).
- **device-증명: ~10%** — Fake 런타임 화면은 device 동작 가능(목); 실 런타임 종단은 본체 배선 대기.
- **천장:** 빌드 통합(build.gradle Compose + Manifest)이 본체 소유 → 이 담당은 통합 가이드까지.
- **남은 것:** §4 #5 (`ALR-PRODUCT-UX-LIVE`) — 본체 5단계 배선 후 device.

### 5.4 roll-up (정직한 한 문장)
**이 담당의 host-측 제품·범용성 작업은 A(~95%)·C(~85%) 가 host-ready, B 는 메인+WS 가 7 toolkit
device-증명. 남은 device 잠금은 단 하나 — 메인의 CR-5(fresh-execve child re-map)** — 가 풀리면
§4 의 5 항목이 추가 host 작업 0 으로 device 종단으로 전환된다. **device 없이 "설치 풀림/제품 동작"
주장은 하지 않는다**(host 천장 = 빌드/구조/메커니즘).

---

## 6. 정직: host 천장 vs device 미확정

| 사실 분류 | 무엇 | 근거 |
|-----------|------|------|
| **HOST-PROVEN (이 담당)** | fakeroot/apt-dpkg stage tar 실빌드·구조·심볼·체인 정합; closure 0-unsat; dpkg-unpack 메커니즘 host-model; Compose UI Fake 동작; 설치엔진/SAF host-model | 빌더 `--selftest` ALL PASS + pytest(`uvx --with pytest`) |
| **DEVICE-PROVEN (메인+WS — 인용)** | CR-1 chromium render(v159); 7 toolkit RENDERS; dpkg/apt/Xwayland 버전실행(drain#9); B-1 execve rewrite(v138); supervisor 데드락 fix(v157) | `docs/evidence/2026-06-02-CR1-ACHIEVED-*`, compat-matrix 인용 |
| **DEVICE-PENDING (게이트)** | `unpacked=true`; 인-게스트 apt install; product-ux 실 런타임 종단; GIMP plugin 필터 output | §4 DEVICE-REQ — CR-5/launch-wiring 선결 |
| **천장(불가)** | 호스트=Darwin 에서 aarch64 .so/dpkg 실행; SELinux PRoot A/B; OpenCL/벤더 GPU compute(계약 위반) | 환경/계약 제약 |

**규칙:** host-only 진전만으로 device 게이트를 PASS 로 승급하지 않는다. device evidence 추가 시
§4 표·§5 % 를 evidence 파일명과 함께 갱신.

---

## 7. 교차참조 (ADR / compat / ssot 상호참조)

- **설치(A) SSOT:** `docs/design/v2-apt-pipeline-ssot.md` (G1/G2/G3 게이트 + §5 launch 계약 +
  DEVICE-REQ `ALR-V2-apt-unpack`). 빌더: `tools/build_apt_dpkg_overlay.py`,
  `tools/build_fakeroot_overlay.py`, `tools/breadth_catalog.py`, `tools/deb_closure.py`,
  `tools/dpkg_unpack_model.py`. fakeroot 계약: `tools/fakeroot/README.md`.
- **제품 UX(C) SSOT:** `docs/design/adr-004-inapp-catalog-ux.md` (§0 5결정 + §5-F AppSession 상태기계 +
  §7 v1/v2 로드맵). 통합: `docs/design/product-ux-integration-guide.md`. 스키마/모델:
  `docs/design/alr-app-manifest-schema.md`, `docs/design/catalog-apt-v1.md`,
  `docs/design/permission-mapping.md`, `docs/design/file-bridge-saf.md`, `docs/design/saf-proxy.md`,
  `docs/design/androidmanifest-permissions-proposal.md`. 도구: `tools/apt_catalog.py`,
  `tools/curated_catalog.py`, `tools/install_engine.py`, `tools/install_plan.py`,
  `tools/permission_map.py`, `tools/desktop_entry.py`, `tools/alr_manifest.py`,
  `tools/saf_bridge_model.py`, `tools/saf_proxy_fd_model.py`.
- **실행 범용성(B) 호환표:** `docs/research/alr-compat-matrix.md` (앱×결과 전체),
  `docs/research/gui-universality-status.md` (창 뜨는 GUI 7 toolkit).
- **메인 의존(G1/데드락/exec-re-entry):** `docs/design/adr-003-multiprocess-exec-reentry.md`
  (B-1/B-2/B-3 + CR-5), `docs/research/loader-feature-gaps.md` (G1),
  `docs/research/cp6-status.md` (chromium storm + ADR-001/002).
- **목표(§0):** `docs/research/orchestration-5session-plan.md` §0.

---

## 8. 갱신 규칙

- **device evidence 추가 시에만** §4 게이트를 PASS 로, §5 % 를 상향. evidence 파일명 명기.
- **메인 좌표 변동 시**(예: CR-5 device-증명) §3.0 표 + §4 의 "즉시 맞물리는" 항목을 갱신.
- host-only 진전(stage 빌드, UI Fake 동작)은 §2 host-ready 만 갱신, device 게이트 미승급.
- **소유:** 이 문서 + `docs/design/v2-apt-pipeline-ssot.md` + product-ux/fakeroot/apt-pipeline
  도구·설계 = 이 세션. 본체(§3 의존·§4 launch-wiring 대상) = 메인. version stamp 불변.
