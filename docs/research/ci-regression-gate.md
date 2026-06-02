# ALR CI / Regression Gate (WS-5 / L5 — §3 M5)

> 오케스트레이션 플랜(`docs/research/orchestration-5session-plan.md`)의 **§3 WS-5 M5
> "CI(host 빌드+테스트; device는 수동 게이트)"** 를 CI 관점으로 박는 문서. **무엇이
> host-자동(매 push/merge)이고 무엇이 device-수동(통합 세션이 단일 디바이스에서 직렬)** 인지,
> 그리고 **no-regression 기준**(어떤 앱이 RUN 유지여야 하는지 + version-stamp 핀)을 정의한다.
>
> 자매 문서: 게이트 **기준(criteria)** 자체는 `docs/research/ws5-premerge-gate.md`(CP별 표),
> ratio 해석은 `cp2-gpu-ratio-glmark2.md`/`cp3-cpu-overhead-ratio.md`. 이 문서는 그것들을
> **CI 파이프라인(자동 vs 수동) 구조**로 묶는다.
>
> 소유: WS-5 (L5). HOST-ONLY — 새 device 측정/숫자 없음, 기존 evidence 인용만.

작성 baseline: 통합 트리 main(v130, drain#9). 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878,
Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app). 디바이스는 **단 1대**.

---

## 0. 두 게이트 — 한눈에

ALR 의 검증은 **두 층**으로 갈린다. 한 층은 device 없이 매 변경마다 자동으로 돌고, 다른 층은
물리 디바이스가 필요해서 **통합 세션이 손으로** 돌린다. 둘을 절대 섞지 않는 게 규율이다.

| 게이트 | 트리거 | 어디서 | 누가 | device 필요? |
|--------|--------|--------|------|-------------|
| **HOST 게이트** (자동) | 매 push / PR / 매 merge 후보 | CI(GitHub Actions) + worktree self-check | 모든 세션 + CI | **아니오** |
| **DEVICE 게이트** (수동) | device-gated CP(§8) / `DEVICE-REQ` 마커 | 단일 디바이스 `R5KL20B6S3X` | **통합 세션만** (§9 Device Lease) | **예** |

CI(`.github/workflows/ws5-host-ci.yml`)는 **HOST 게이트만** 돈다. device 테이프(adb/APK
install/logcat)는 CI 에서 **절대 시도하지 않는다** — 공유 하드웨어 1대를 CI 러너가 만질 수 없고,
다른 세션 device 테스트를 clobber 한다(메모리: device-test-force-stop-first).

---

## 1. HOST 게이트 (자동 — 매 변경)

device 없이 머지 후보 트리에서 전부 검증 가능. 하나라도 실패하면 merge 금지. 세 부분:

### 1.1 host pytest (필수 — 항상 자동)
- 명령: `uvx pytest tests/ -q` (host 에 system pytest 없음 → `uvx`; CI/로컬 공통 진입점
  `scripts/run-host-tests.sh`).
- CI 잡: `.github/workflows/ws5-host-ci.yml` (`on: [push, pull_request]`, `ubuntu-latest`,
  `astral-sh/setup-uv@v5` → `uvx pytest tests/ -q`).
- 범위: 순수-host 테스트 — native 소스 정합(`*_sources.py`), bench 모델(regression_gate /
  cpu_overhead / gpu_bench / display_verify / present_verify), report 파서, 그리고 본
  문서/compat-matrix 의 drift 가드(§3). **on-device 테스트는 여기 포함 안 됨**
  (`test_android_*` 류 probe 테스트도 host 에서 *소스/계약* 만 검증; 실제 device 실행은 수동).
- 통과 조건: **ALL passed**. 1개라도 fail/error 면 게이트 FAIL.
- 현재 수집: **514** passed (auto/r5-docs 기준; round-5 docs drift 가드 + loader-feature-gaps
  SSOT 가드 추가). 수는 테스트 추가 때마다 늘어난다 — 게이트는 "전부 통과"이지 고정 숫자가 아님.

### 1.2 NDK native build (4-ABI) — 컴파일 정합
- 명령: `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ./gradlew :app:externalNativeBuildDebug`.
- 의미: `runtime_report.cpp`/`alr_gpu/**`/`alr_wayland/**`/`libalr_interpose.c` 가 NDK 로
  4 ABI(`arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64`)에 깨끗이 컴파일되는지. 핫스팟 TU 에
  여러 WS 가 함수를 추가하므로 컴파일 회귀를 먼저 잡는다.
- 누가: 각 세션은 자기 worktree 에서 host-only 로 컴파일만(APK install 금지, §9.1). 통합
  세션은 merge 후 재확인.
- 주의: `JAVA_HOME` 미설정이면 stale APK 가 조용히 남는다(메모리: build-needs-java-home).
  단, WS-5 는 **version stamp 를 건드리지 않으므로** assembleDebug/stamp bump 는 통합 세션 몫.

### 1.3 zig guest-shim 빌드 + wire-check — GPU 마샬링 seam
device 가 못 닿는 한 seam: **게스트 shim 인코더 ↔ host 디코더** 와이어 포맷 정합. device
probe 는 host-side Encoder 를 쓰므로 게스트 shim 의 실제 바이트는 device 에서 안 돈다.

- **shim build:** `app/src/main/cpp/alr_gpu/guest_shim/build-shim.sh` — `zig cc` 로
  aarch64 glibc 게스트용 `libGLESv2.so.2` + `libEGL.so.1` + `alr-gles-cube` 크로스컴파일
  (target `aarch64-linux-gnu.2.34` 로 pthread/dl 을 libc.so.6 에 fold — 버전 suffix 떼면
  DT_NEEDED libpthread.so.0 가 생겨 tiny rootfs 에서 EGL dlopen 체인 깨짐; 떼지 말 것).
- **wire-check:** `app/src/main/cpp/alr_gpu/guest_shim/build-wire-check.sh` — host-native
  (macOS/Linux `cc`/`c++`, device/NDK/GL 드라이버 불필요). STAGE 1 `wc_emit`(실 `alr_gles_shim.c`
  + 스텁 런타임)이 cube GL 시퀀스의 와이어 바이트를 덤프 → STAGE 2 `wc_decode`(커밋된
  `alr_gpu_decode.hpp` + recording 스텁)가 디코드 후 round-trip assert. 게스트 인코더가
  host 디코더가 읽는 바이트를 내는지 off-device 로 증명.
- 통과 조건: 두 스크립트 모두 0 exit (`wc_decode` round-trip assert PASS).

> **요약(host 자동):** `uvx pytest tests/ -q`(green) + NDK 4-ABI 컴파일 clean + zig
> build-shim/wire-check PASS. 이 셋이 매 merge 후보의 host 게이트(통합 세션의 머지 로그가
> "host gate: pytest N + NDK 4-ABI + shim/wire PASS" 로 기록).

### 1.4 version-stamp drift (host 검사 — 통합 세션만 bump)
- 핀 사이트(메모리: version-stamp-pin-sites)는 **한 번에 같이 움직인다**. 부분 bump =
  drift = FAIL.
- bump 은 **통합 세션만**(§6.4). 워크스트림 브랜치가 stamp/stamp-핀 테스트를 건드렸으면
  머지에서 reject(WS-5 포함 — WS-5 는 stamp 를 절대 안 만진다).
- staging 은 항상 **명시 경로**로만; `git add -A` 금지(§6.6, 다른 세션 미커밋 작업 보호).

---

## 2. DEVICE 게이트 (수동 — 통합 세션이 단일 디바이스에서 직렬)

device 가 필요한 측정은 **통합 세션이 §9 Device Lease 로 직렬 실행**한다. 세션은 `DEVICE-REQ`
마커로 요청만 하고 APK install 은 안 한다.

### 2.1 디바이스 = 단일 직렬 자원
- 디바이스 1대(`R5KL20B6S3X`)를 5세션이 동시에 install/force-stop/logcat 치면 서로 덮어쓴다
  → **통합 세션 단독 소유, 직렬 실행**(§9).
- 절차: merged HEAD → `JAVA_HOME=openjdk@17 ./gradlew :app:assembleDebug`(stamp 통합 세션
  bump) → `adb install -r -d` → `am force-stop` → `logcat -c` → `am start`(**cold start**;
  warm resume 는 onCreate 스킵 → overlay/probe 미실행) → `logcat -s alr_loader` 캡처 →
  `docs/evidence/`.
- **배치 drain:** 여러 세션의 `DEVICE-REQ` 를 한 빌드로 묶어 실행(개별 install 은 5×~265MB
  비효율). drain#7~#9 가 이 방식.

### 2.2 device-gated 체크포인트 (§8)
각 device-gated CP 의 추가 게이트(공통 전제 = host green + force-stop+cold + `bench gate`
PASS + evidence 파일링):

| CP | device 게이트 | 상태(현재) |
|----|--------------|-----------|
| CP-1 디스플레이 | `bench.display_verify` → 1200×1920·90Hz exact | **DONE** (device-verified) |
| CP-2 GPU 네이티브 | `bench gpu` ratio ≥ 0.70 AND software=false | **부분** (ALR Score device-증명; Mali-직접 baseline=PENDING_DEVICE → ratio 분모 대기) |
| CP-3 CPU 오버헤드 | `bench overhead` syscall-light < 5% | **DONE** (compute +0.00%, device-증명) |
| CP-4 zero-copy present | dmabuf 경로 + CPU readback 0 | **DONE** (AHB→external-OES import device-verified) |
| CP-5 범용성 | toolkit 매트릭스 + apt 앱 실행 + compat-matrix 갱신 | **부분** (dpkg/apt/Xwayland **실행+버전** device-증명; 실제 apt install·qt6/netsurf/sdl2 launch=PENDING) |
| CP-6 제로 오버헤드 수렴 | chromium-class usable 또는 USER_NOTIF ADR | **ADR-001 작성됨**; 구현=PENDING |

(CP별 host/device 칼럼 표는 `ws5-premerge-gate.md` §Per-checkpoint gate table.)

### 2.3 DEVICE-REQ 프로토콜
세션이 device 캡처가 필요하면 (a) main merge(host gate clean) + (b) merge-commit 메시지에
`DEVICE-REQ: <보고 싶은 marker/PASS 조건>`. 통합 세션이 큐에 넣어 §9 drain 으로 실행. WS-5 는
캡처된 logcat 을 `bench`/regression-gate 로 파싱해 게이트를 닫는다(install 안 함, §9.6).

---

## 3. NO-REGRESSION 기준 (무엇이 RUN 유지여야 하는가)

device 빌드가 바뀌어도 **이미 증명된 동작은 깨지면 안 된다**. no-regression 게이트 =
**`python -m bench gate <report>`** (구현 `bench/regression_gate.py`).

### 3.1 mediation 불변식 (전 게스트 공통)
- `all: pcgate=1 interpose=1 traps=0 rewrites=0` — supervisor ptrace 라운드트립 0.
- 깨지면(`traps>0`/`rewrites>0`/`pcgate=0`/`interpose=0`) → `mediation_ok=False` → 게이트 FAIL.
- **per-guest traps:** 일반 게스트는 전부 `traps=0`. **유일한 허용 예외 = `/usr/bin/gimp-3.0`
  의 `traps=1`**(set_robust_list류 1회). 그 외 게스트가 `traps>0` 면 FAIL
  (`TRAPS_TOLERANCE = {"/usr/bin/gimp-3.0": 1}`).

### 3.2 baseline 프로브 집합 (expected-exit 고정)
`bench/regression_gate.py:GIMP_NOREGRESSION_PROBES` — device-증명 v122 무회귀 집합. 각
게스트가 **정확히 이 exit** 가 아니면(또는 probe 누락 = absent) FAIL:

| guest | expected exit |
|-------|---------------|
| `/bin/dynhello` | 0 |
| `/usr/bin/env` | 0 |
| `/usr/bin/id` | 0 |
| `/bin/dash` | 0 |
| `/bin/alr-png-test` | 0 |
| `/usr/bin/gimp-console-3.0` | 127 (의도된: deps 부재, mediation 결함 아님) |
| `/bin/alr-wl-test` | 0 |
| `/bin/alr-pixman-test` | 0 |
| `/usr/bin/gimp-3.0` (확장) | 0 (traps=1 허용) |

### 3.3 GUI/GPU RUN-유지 게이트 (플랜 §3 M3 명시 집합)
플랜이 명시한 무회귀 대상 — **device 빌드마다 RUN/RENDER 유지**여야 하고, 깨지면 회귀:

- **GIMP 3.0.2** (GTK3) — USABLE 유지(터치 그리기). drain#7/#8/#9 final GUI probe 에서
  device-render 재확인(no FATAL/SIGSEGV).
- **foot** (terminal) — RENDERS 유지(`rendered=true`).
- **gtk3demo** (`/bin/alr-gtk3-test`) — RENDERS 유지(렌더 루프 정상, frames↑).
- **CLI 집합** (§3.2) — expected-exit + traps=0 유지.
- **glmark2-es2** — RUN(Mali, software=false) 유지. drain#9 regression-check Score 1052,
  drain#10(v132) Score 1009 (drain#8 1163 대비 dip 은 thermal/run-to-run, 기능 회귀 아님 —
  여전히 GPU-class ~1000+ FPS).

(상세는 `docs/research/alr-compat-matrix.md` — 이 게이트의 SSOT 행들.)

### 3.5 device 마커 체크리스트 (드레인마다 확인 — no-regression)

위 RUN-유지 게이트(§3.3)는 "프로세스가 산다"를 넓게 잡는다. 그 위에, **각 device drain 의
logcat 캡처에서 통합 세션이 직접 눈으로 확인**해야 하는 **PASS 마커 / 비-증가 불변식**을 못박는다.
하나라도 **사라지거나(absent) FAIL 로 바뀌거나 악화**되면 회귀 — 머지/스탬프 bump 금지.
(아래는 전부 device-수동; 새 측정이 아니라 기존 drain#10 evidence
`docs/evidence/2026-06-02-breadth-fanout-drain.md` 가 통과를 보인 항목들.)

| 마커 / 불변식 | 기대 | 깨지면 |
|--------------|------|--------|
| `ALR GPU LIVE INTEGRATION: PASS` | 매 drain present | 회귀(파이프라인 깨짐) |
| **`ALR GPU THROUGHPUT: PASS`** (drain#10 신규) | 매 drain present | 회귀(ring/decoder throughput 회귀) |
| `ALR GPU SCREEN CUBE: PASS` | 매 drain present | 회귀(loader-fork present 깨짐) |
| glmark2 `software renderer=false` + Score ~GPU-class | software=false, Score 1000+ (≥ ~1000) | software=true 또는 Score 급락 = 회귀 |
| **GLES2 커버리지 (shim/wire-check)** | `build-wire-check.sh` round-trip assert PASS (host, off-device); 19개 state-setter(blend/depth/color/stencil 등) wire 인코딩 + decode replay 유지 | wire-check 실패 = 게스트 인코더↔host 디코더 정합 깨짐 = host 게이트 FAIL(§1.3) |
| **per-guest `traps` 비-증가** | CLI/dpkg/apt/Xwayland/foot = **0**; gtk3-widget-factory ≤ 99(drain#10 기준선, 135 에서 내려옴); glmark2 ≤ 31; `gimp-3.0` traps≤1 허용 | 기준선 초과(traps↑) = mediation 라운드트립 회귀 |

- **THROUGHPUT 마커**는 drain#10 에서 LIVE/SCREEN-CUBE 와 나란히 추가된 device 체크포인트다
  (WS-2 GLES2 19-op 커버리지 + decode 변경이 GPU 파이프라인을 회귀시키지 않았음을 증명).
- **GLES coverage(shim/wire-check)** 는 device 가 못 닿는 seam 이라 **host 게이트(§1.3)에서 자동**으로
  잡힌다 — 게스트 shim 의 실제 와이어 바이트가 host 디코더가 읽는 바이트인지 off-device round-trip
  assert. device drain 에서는 GPU self-test PASS 가 그 보강 증거.
- **traps 비-증가**는 §3.1 의 불변식(일반 게스트 traps=0)을 드레인-대-드레인 *방향성*으로 확장한다.
  drain#10 은 gtk3-widget-factory 를 135→99 로 **낮췄다**(WS-1 CP-6 M2 opendir 트램폴린 + credential
  캐시). 새 drain 이 이 기준선을 **올리면** 회귀로 본다. (동일-빌드 A/B 격리는 아님 — 방향성 + targeted
  메커니즘으로 정직 보고.)

### 3.4 version-stamp 핀
- 모든 device evidence/회귀 판정은 **빌드 stamp 와 함께** 기록(`build: <stamp>` 라인).
  regression_gate 가 `build_stamp` 를 파싱해 어느 빌드의 결과인지 고정.
- stamp 핀 사이트는 한 번에 같이 bump(§1.4) — 부분 bump = drift = host 게이트 FAIL.
- WS-5 는 stamp 를 만지지 않는다(§6.4); 통합 세션만 bump.

---

## 4. host 자동 vs device 수동 — 매핑 요약

| 검증 항목 | host-자동 (CI/세션) | device-수동 (통합 세션) |
|-----------|--------------------|------------------------|
| pytest green (현 514) | ✅ `uvx pytest tests/ -q` | — |
| native 4-ABI 컴파일 | ✅ `gradlew externalNativeBuildDebug` | — |
| guest-shim 빌드 + wire-check (GLES coverage seam) | ✅ `build-shim.sh` + `build-wire-check.sh` (zig, off-device; 19-op state-setter wire round-trip) | — |
| version-stamp drift 없음 | ✅ (핀 정합 검사; bump 은 통합만) | — |
| bench 모델(gate/overhead/gpu/display/present) | ✅ 순수함수 단위테스트 | 입력 숫자 = device 캡처에서 |
| no-regression `bench gate` PASS | (모델은 host 테스트) | ✅ 실제 device 리포트로 평가 |
| CP-1 display 1200×1920@90Hz | — | ✅ `bench.display_verify` |
| CP-2 glmark2 Mali score + ratio | — | ✅ `bench gpu` (Mali-직접 baseline 후) |
| CP-3 CPU 오버헤드 < 5% | — | ✅ `bench overhead` (same-binary) |
| CP-4 dmabuf zero-copy present | — | ✅ present 마커 파싱 |
| GIMP/foot/gtk3demo/CLI/glmark2 RUN 유지 | — | ✅ device 리포트 회귀 검사 |
| GPU LIVE/**THROUGHPUT**/SCREEN-CUBE PASS 마커 (§3.5) | — | ✅ drain logcat 마커 체크리스트 |
| per-guest traps 비-증가 (§3.5; gtk3-widget-factory ≤99) | — | ✅ drain 마커 체크리스트 |

**한 줄:** CI = host 게이트(pytest + NDK 4-ABI + zig shim/wire)만 자동. device(단일
하드웨어)는 통합 세션이 §9 로 직렬 수동. no-regression = `bench gate`(mediation 불변식 +
expected-exit 프로브 + GIMP/foot/gtk3demo/CLI/glmark2 RUN 유지 + version-stamp 핀).
