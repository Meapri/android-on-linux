# CP-2 GPU Ratio — glmark2 ALR vs Mali-direct (WS-5)

> CP-2 = "GPU 네이티브 풀가속" 정량 게이트. ALR(게스트 glibc → shim → SPSC ring →
> host `GpuExecutorService` → 실 Mali-G615) 에서 glmark2 가 **렌더링**하는 것은
> device-증명됐다(아래 ALR 칸). 남은 한 칸은 **같은 device에서 glmark2 를 Mali에
> 직접(ALR 없이)** 돌린 baseline 점수 — 이걸 분모로 둬야 §0 의 "ALR vs Mali-직접
> 비율 ≥ 70–80%" 목표를 닫을 수 있다. baseline 은 **PENDING_DEVICE**(통합 세션
> §9 device drain 산출물)다. 이 문서는 ALR 측 숫자를 고정하고, baseline 측정
> 절차/포맷을 박아 통합 세션이 그 칸만 채우면 게이트가 자동으로 평가되게 한다.
>
> 소유: WS-5 (L5). 측정 대상 숫자는 기존 device evidence 에서만 인용(host-only,
> 새 device 측정 없음). 비율 계산기는 `bench/gpu_bench.py`(`compute_gpu_ratio`,
> §0 게이트 = ratio ≥ 0.70 AND non-software renderer)에 이미 구현돼 있다.

작성 baseline: 통합 트리 main `310c759`. 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878 SoC,
Mali-G615 MC2, Android 16, 1200×1920 @ 90Hz, untrusted_app).

---

## 1. §0 목표 연결

`docs/research/orchestration-5session-plan.md` §0 성공 기준 (GPU 축):

| 축 | 측정 | 목표 |
|----|------|------|
| GPU 가속 | glmark2-es2 score, 실앱 fps | **Mali 직접 대비 high-% (목표 ≥ 70–80%), software=false** |

즉 CP-2 게이트는 **두 조건**의 AND 다:
1. **renderer ≠ software** (swiftshader/llvmpipe/lavapipe 등 SW 래스터라이저로 통과 금지) — ALR 측 이미 device-증명 (`software renderer=false`, Mali-G615).
2. **ratio = ALR_score / Mali직접_score ≥ 0.70** — 분자(ALR) 확보, 분모(Mali-직접) **PENDING_DEVICE**.

`bench.gpu_bench.GPU_ACCEL_MIN_RATIO = 0.70` 이 1번 게이트의 하한이다. §0 텍스트의
"≥ 70–80%" 는 70% 를 **must-pass 하한**, 80% 를 **stretch 목표**로 읽는다(아래 §4 해석).

---

## 2. ALR 측 (분자) — device-증명 ✅

ALR glmark2 결과는 두 번의 device drain 으로 고정됐다. 모두 `software renderer=false`
(실 Mali-G615), 게스트 GLES2 호출이 shim → ring → host executor 로 마샬링돼 실 GPU 에서 렌더.

| drain | scene(s) | per-scene FPS | glmark2 Score | ring | renderer | evidence |
|-------|----------|---------------|---------------|------|----------|----------|
| #7 (CP-2 FINAL) | build | build 1075 FPS @ 1920×1200 | **1074** | 1 MiB host | `software=false`, Mali-G615 | `docs/evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md` |
| #8 (CP-5 batch) | build + texture | build 1206 / texture 1123 FPS | **1163** | 8 MiB host | `software=false`, Mali-G615 | `docs/evidence/2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md` |

- **대표 ALR 점수 = 1163** (drain#8, build 1206 FPS + texture 1123 FPS, 8 MiB ring).
  drain#7 의 1074 는 build-only(텍스처 scene 이 1 MiB ring 에서 drop 되던 시점) 라서
  더 작다. 비율 비교의 분자는 **가장 최신/완전한 device 빌드의 점수**를 쓴다 → 1163.
- renderer 게이트: ALR 측 `GL_RENDERER` 문자열은 shim 의 합성 문자열
  (`ALR command-stream (host GPU passthrough)`) 이지만, **실제 draw 는 Mali 에서**
  돈다(`software renderer=false`, LIVE INTEGRATION/SCREEN CUBE 가 실 Mali 에 렌더 —
  evidence 참조). 따라서 `bench.gpu_bench` 의 software-renderer 게이트에는 **ALR의
  실측 검증 사실**(`software=false`)을 기준으로 `--alr-renderer "Mali-G615"` 를
  넘긴다(합성 문자열을 그대로 넣으면 안 됨 — host GL_RENDERER passthrough 는 WS-2 의
  외형 follow-up, CP-2 게이트 아님).

### scene 매트릭스 현황
- **build** (geometry): device-증명 (1206 FPS, drain#8).
- **texture** (1024×1024 텍스처 upload, 4 MiB 단일 op): device-증명 (1123 FPS, 8 MiB ring, drain#8).
- 나머지 scene (shading/bump/refract/conditionals/function/loop/desktop/buffer/ideas/jellyfish/terrain/shadow):
  **PENDING_DEVICE** — duration↑ 또는 분할 launch 로 한 빌드에서 전 scene 을 돌려야
  glmark2 의 정식 종합 Score(전 scene 평균)가 나온다. 현재 1163 은 build+texture 2-scene
  부분 종합이다. baseline 도 **동일 scene 집합**으로 측정해야 ratio 가 apples-to-apples 다(§3).

---

## 3. Mali-직접 baseline (분모) — PENDING_DEVICE

ALR 없이, **같은 device 의 같은 glmark2 바이너리/scene 집합**을 Mali 에 직접 돌린 점수.
이게 분모다. 아직 측정 안 됨 → 통합 세션 §9 device drain 이 채운다.

| 항목 | 값 |
|------|-----|
| Mali-직접 glmark2 Score (build+texture) | **PENDING_DEVICE** |
| Mali-직접 build FPS | **PENDING_DEVICE** |
| Mali-직접 texture FPS | **PENDING_DEVICE** |
| Mali-직접 GL_RENDERER | **PENDING_DEVICE** (기대: `Mali-G615` 류, software=false) |
| 측정 빌드/stamp | **PENDING_DEVICE** |
| evidence 파일 | **PENDING_DEVICE** (`docs/evidence/<date>-cp2-mali-direct-baseline.md`) |

### 3.1 측정 절차 (통합 세션이 device 에서 실행)
ALR 경로를 **타지 않는** glmark2 직접 실행 baseline 이 필요하다. 후보 두 가지(통합 세션이 가능한 쪽 선택):

1. **Android-native EGL/GLES2 surface 에서 glmark2-es2** — glmark2-es2 를 Android
   EGLSurface(SurfaceView)로 직접 띄워(ALR loader/ring/shim 우회) Mali 에 네이티브 렌더.
   이게 "Mali 직접"의 가장 깨끗한 정의(같은 device, 같은 GPU, 같은 glmark2 scene, ALR 마샬링 0).
2. **(차선) 동급 Mali 디바이스의 공개 glmark2-es2 score** — 같은 device 가 안 되면
   동일 Mali-G615 MC2 / mt6878 의 공개 측정치를 baseline 로 인용하고 evidence 에
   "off-device reference, not same-unit" 로 명시(ratio 는 indicative 로 강등).

권장은 (1). 절차:
```
# 통합 세션, §9 device lease, cold start (am force-stop 후)
adb shell am force-stop <pkg>
adb logcat -c
adb shell am start -n <pkg>/<MaliDirectGlmark2Activity>   # ALR loader 우회, 직접 EGL surface
adb logcat -s alr_loader | tee mali-direct.log
# 같은 scene 집합으로: build:duration=5 texture:duration=5 (ALR drain#8 과 동일)
```
- **반드시 ALR drain#8 과 동일한 scene 집합/duration** 으로 측정 (build:duration=5 + texture:duration=5).
  안 그러면 분자/분모의 scene 가중이 달라 ratio 가 무의미.
- 해상도도 동일하게 1920×1200 (또는 양쪽 같은 값) — glmark2 Score 는 해상도/fill-rate 민감.
- renderer 가 `software=false`(Mali) 인지 확인 — baseline 이 SW 래스터면 비교 자체가 무효.

### 3.2 캡처 포맷 (`bench.gpu_bench` 가 그대로 파싱)
baseline 로그는 다음 라인만 있으면 파서가 먹는다(`parse_glmark2_score` / `parse_gl_renderer`):
```
    GL_RENDERER:   Mali-G615 (...)
                                  glmark2 Score: <N>
```
`bench/gpu_bench.py:parse_gpu_from_report` 가 `Score:` / `GL_RENDERER:` 양식을 인식한다.

---

## 4. 비율 골격 (baseline 채워지면 자동 평가)

분모가 PENDING 인 동안은 ALR 분자만 고정한다. baseline 이 들어오면 아래 표 한 칸만 채우면 된다.

| metric | value |
|--------|-------|
| ALR score (build+texture, 8 MiB ring) | **1163** (device-증명, software=false) |
| Mali-direct score (build+texture) | **PENDING_DEVICE** |
| ratio = ALR / Mali-direct | **PENDING_DEVICE** |
| §0 하한 (must-pass) | ratio ≥ **0.70** AND renderer ≠ software |
| §0 stretch | ratio ≥ 0.80 |
| renderer ≠ software (ALR) | **PASS** (`software=false`, Mali-G615) |
| verdict | **PENDING_DEVICE** (renderer 게이트는 이미 PASS; ratio 게이트만 baseline 대기) |

### 4.1 baseline 들어오면 실행할 명령 (host, 새 측정 아님 — 파싱/게이트만)
```sh
# 1163 = ALR build+texture; <BASE> = Mali-직접 같은 scene 집합 점수
python -m bench gpu --alr-score 1163 --mali-score <BASE> --alr-renderer "Mali-G615"
# 또는 캡처 로그로:
python -m bench gpu \
  --alr-report  docs/evidence/2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md \
  --mali-report docs/evidence/<date>-cp2-mali-direct-baseline.md
```
`compute_gpu_ratio` 가 ratio + verdict(`passes_target`) 를 계산한다(게이트 = ratio ≥ 0.70 AND non-software).

### 4.2 해석 가이드 (ratio 가 나왔을 때)
- **ratio ≥ 0.80** → §0 stretch 충족. "Mali 직접 대비 ~동급" 주장 가능.
- **0.70 ≤ ratio < 0.80** → §0 하한 PASS. 마샬링 오버헤드가 측정 가능하나 목표 범위 내.
  ring/배치 최적화(8 MiB 가 이미 texture scene unblock)로 stretch 추격.
- **ratio < 0.70** → 게이트 FAIL. per-call 마샬링/카피(§0 (b) "GPU 측 per-call
  마샬링/카피")가 병목 — client-side virtual GL ID(round-trip 0)·배치·8MiB ring 이
  이미 그 방향. scene 별 분해(어떤 scene 이 ratio 를 떨어뜨리는지)로 회귀.
- **단, glmark2 Score 는 fill-rate/해상도/scene-mix 에 민감** — ratio 는 동일 scene 집합·
  동일 해상도일 때만 유효. §3.1 의 apples-to-apples 조건을 어기면 숫자는 무효.

### 4.3 정직한 caveat
- 현 ALR 1163 은 **build+texture 2-scene 부분 종합** 이다. 전체 14-scene glmark2 종합
  Score 가 아니다(§2 scene 매트릭스 PENDING). 따라서 ratio 도 "build+texture 부분
  종합 기준"으로 한정 해석. 전 scene 종합 ratio 는 양쪽 전 scene 측정 후.
- ALR `GL_RENDERER` 합성 문자열은 외형(WS-2 passthrough follow-up). renderer≠software
  게이트는 합성 문자열이 아니라 **`software renderer=false` device 사실**로 통과(§2).

---

## 5. 요약
ALR glmark2 = build 1206 / texture 1123 FPS → **Score 1163**, Mali-G615, `software=false`,
device-증명(drain#8). renderer≠software 게이트는 **이미 PASS**. 남은 건 **Mali-직접
baseline(분모)** 한 칸 = **PENDING_DEVICE**, 통합 세션 §9 device drain 이 §3.1 절차로 채운다.
baseline 이 들어오면 `python -m bench gpu --alr-score 1163 --mali-score <BASE> --alr-renderer "Mali-G615"`
한 줄로 §0 GPU 비율 게이트(≥0.70)가 평가된다.
