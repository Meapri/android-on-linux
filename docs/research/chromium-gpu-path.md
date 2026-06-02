# Chromium GPU path on ALR/Mali — flag 사다리 + staged-lib 매트릭스 (SSOT)

> **chromium 을 ALR/Mali 에서 *어떤 GPU 백엔드*로 띄울지의 정찰 + 권장 flag 사다리.** 각 옵션마다
> *무슨 lib 가 스테이징돼야* 작동하는지를 못 박는다. 이건 `chromium-run-plan.md` CR-3(=GPU)의
> 세부 사다리다. chromium 은 **사용자 보류(held)** 상태이므로 device evidence 없이 어떤 백엔드도
> RUNS 로 승급하지 않는다.
>
> 소유: WS-5(L5). **HOST-ONLY** — 새 device 측정 없음. **벤치 문서 아님**. GPU 마샬 아키텍처의
> 결정 SSOT 는 `docs/research/gpu-guest-accel-strategy.md`(ring + bionic-TLS executor; Vulkan-first;
> ANGLE for GLES; zink-deferred) — 이 문서는 그걸 *chromium 의 `--use-gl`/`--use-angle` flag* 에만
> 매핑한다(A-vs-B 경계는 재론하지 않음).
>
> baseline: 통합 트리 v144. 디바이스 SM-X236N / Mali-G615 MC2 / Android 16 / untrusted_app.

---

## 0. 한 줄 결론 + 권장 사다리

**chromium 은 GPU 백엔드를 *내부에 번들*한다(ANGLE = `libGLESv2.so`/`libEGL.so`, SwiftShader =
`libvk_swiftshader.so`). 그래서 가장 안전한 첫 칸은 chromium *자체* software 백엔드(`--disable-gpu`
또는 `--use-gl=angle --use-angle=swiftshader`)이고, 우리 Mali shim 은 사다리의 *마지막* 칸**이다 —
soname/closure 정합이 가장 까다롭기 때문. 권장 사다리(낮은 위험 → 높은 가속):

```
①  --disable-gpu                                  software raster (GPU 0)        ← CR-1/CR-2 기본
②  --use-gl=angle --use-angle=swiftshader
       --enable-unsafe-swiftshader                software Vulkan(번들 SwiftShader)  ← CR-3 1칸
③  --use-gl=angle --use-angle=gles-egl            ANGLE → system libEGL.so.1 →
                                                   **우리 GLES shim → Mali executor** ← CR-3 2칸(가속)
④  (장래) ANGLE/Vulkan → **우리 VK marshal** → vendor Mali libvulkan  ← gpu-guest-accel-strategy 끝점
```

각 칸은 *직전 칸이 device-PASS 한 뒤에만* 오른다. 정직: **①만이 지금 default-안전**(`--version`/
`--dump-dom` 이 이미 GPU 0 로 동작). ②③④ 는 전부 device-pending.

---

## 1. 옵션별 정찰 — 무엇이 무엇을 요구하나

### 옵션 ① `--disable-gpu` — software raster (CR-1 / CR-2)
- **백엔드:** chromium 의 software raster/compositor. GPU 0, ANGLE 0.
- **staged-lib:** **추가 없음** — chromium closure 만으로 동작. `--version`/`--dump-dom` 이 이미 이
  경로(`docs/evidence/2026-06-01-device-SM-X236N-chromium-runs-inprocess.md`, `traps=0`).
- **ALR 정합:** W^X/JIT 무관(software raster 는 JIT 안 씀; V8 만 JIT, 그건 v120 PASS). 가장 안전.
- **한계:** WebGL/3D 가속 없음, 픽셀-푸시 느림. CR-1/CR-2 의 정직한 기본값.

### 옵션 ② `--use-gl=angle --use-angle=swiftshader` — software Vulkan (CR-3 1칸)
- **백엔드:** ANGLE 가 **SwiftShader**(software Vulkan ICD)를 백엔드로. chromium 은
  `libvk_swiftshader.so`(+ `vk_swiftshader_icd.json`) 를 **번들**(`/usr/lib/chromium/`).
- **staged-lib:** chromium 번들의 `libvk_swiftshader.so` + ANGLE `libEGL.so`/`libGLESv2.so`(번들) —
  **chromium-stage.tar 에 이미 들어있는지 확인 필요**(현 stage 는 headless-shell + closure;
  `/usr/lib/chromium/` 의 GPU 번들이 포함됐는지 device 로 확인). 없으면 chromium .deb 의
  `libgl1-mesa-*`/번들 GPU lib 를 stage 에 추가.
- **ALR 정합:** SwiftShader 는 **JIT**(런타임 코드 생성)한다 → W^X execmem 필요. v120 JIT W^X
  사이클 PASS(`cycles_ok=8/8`, `rwx_mmap_ok=true`)가 이를 덮을 *가능성* 높음 — 단 SwiftShader 의
  코드젠 패턴은 V8 과 달라 **device-확인 필요**. Arm 에서 WebGL 은 `--enable-unsafe-swiftshader`
  없으면 off.
- **한계:** software Vulkan = CPU 렌더, 진짜 Mali 가속 아님. 그러나 ANGLE 경로를 *켜는* 안전한
  계단(③ 로 가기 전 ANGLE 자체가 뜨는지 격리 검증).

### 옵션 ③ `--use-gl=angle --use-angle=gles-egl` — ANGLE → 우리 GLES shim → Mali (CR-3 2칸, 가속)
- **백엔드:** ANGLE 가 **system `libEGL.so.1`/`libGLESv2.so.2`** 를 `dlopen` → 그게 **우리 alr_gpu
  GLES shim**(client-side 가상 GL ID + ring → host Mali executor; CP-2 에서 glmark2 가 이 경로로
  Mali 에 1075 FPS 렌더, `docs/research/cp2-gpu-ratio-glmark2.md`).
- **staged-lib (정합이 까다로운 핵심):**
  - 우리 shim 이 **정확히 `libEGL.so.1` + `libGLESv2.so.2` soname** 으로 + **unversioned symlink**
    (`libEGL.so`, `libGLESv2.so`)로 `LD_LIBRARY_PATH` 상에 있어야(`chromium-native-plan.md` Scout 3).
    shim = `gpushim-stage.tar`(CP-2 계보).
  - ANGLE 가 chromium 번들이면 chromium 의 `libEGL.so`(ANGLE)와 우리 shim 의 `libEGL.so.1` 가
    **충돌** 가능 — ANGLE 는 *system* EGL 을 dlopen 하므로 우리 shim 이 system soname 을 차지하고
    ANGLE 번들은 `--use-angle=gles-egl` 로 "system GLES 위에 얹기"가 되도록 LD 경로 우선순위를
    잡아야(device-확인 사항).
- **ALR 정합:** dmabuf/GBM(`/dev/dri`) 경로는 **만족하지 않음** — `zwp_linux_dmabuf_v1` 미광고로
  swap 을 `wl_shm` 에 묶고 우리 AHB presenter 가 합성(`chromium-native-plan.md` Scout 3). 진짜
  zero-copy(AHB↔dmabuf)는 후기 끝점.
- **한계:** GLES2/ES3 만(우리 shim 커버리지; GLES3 = R12 g5-gles3 레인 in-flight). Vulkan 직행은 ④.

### 옵션 ④ (장래) ANGLE/Vulkan → 우리 VK marshal → vendor Mali libvulkan — 끝점
- **백엔드:** ANGLE 의 Vulkan 백엔드 또는 chromium Vulkan → **우리 VK marshal**(ring + bionic-TLS
  executor 가 vendor Mali `libvulkan.so` 소유; `docs/research/gpu-guest-accel-strategy.md`).
- **staged-lib:** 우리 VK marshal lib(`libvulkan.so.1` soname) + vendor Mali ICD(host-side, executor
  가 소유). chromium 측은 `--use-vulkan` 류 + ANGLE Vulkan.
- **ALR 정합:** gpu-guest-accel-strategy 의 Vulkan-first 끝점. Mali transform_feedback 천장 때문에
  zink 은 deferred. **전부 미구현/장래** — 여기선 사다리 끝칸으로만 기록(아키텍처 재론 금지).
- **한계:** VK marshal 자체가 breadth 작업(R12 g3-vk 레인 in-flight, ICD 방향).

---

## 2. flag → staged-lib 매트릭스 (한눈에)

| 칸 | flag | 백엔드 | 필요 staged-lib | ALR 정합 게이트 | CR | 상태 |
|---|---|---|---|---|---|---|
| ① | `--disable-gpu` | sw raster | 없음(chromium closure) | 없음 | CR-1/2 | **device-PASS**(--version/--dump-dom 경로) |
| ② | `--use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader` | sw Vulkan(SwiftShader) | 번들 `libvk_swiftshader.so` + ANGLE `libEGL/GLESv2`(chromium-stage 에 포함 확인) | W^X(SwiftShader 코드젠; v120 JIT PASS 가 덮을 듯하나 device-확인) | CR-3 | device-pending |
| ③ | `--use-gl=angle --use-angle=gles-egl` | ANGLE → **우리 GLES shim** → Mali | `gpushim-stage.tar`(`libEGL.so.1`+`libGLESv2.so.2`+unversioned symlink), LD 우선순위 | soname 정합 + dmabuf 미광고(wl_shm swap) + GLES2/ES3 커버리지 | CR-3 | device-pending(CP-2 가 glmark2 로 shim→Mali 입증) |
| ④ | (장래) ANGLE/Vulkan → **우리 VK marshal** | Vulkan → Mali libvulkan | VK marshal `libvulkan.so.1` + vendor ICD(executor) | gpu-guest-accel-strategy Vulkan-first 끝점 | CR-3+ | 미구현/장래(R12 g3-vk in-flight) |

---

## 3. 권장 (flag 사다리)

1. **CR-1/CR-2 는 ① `--disable-gpu`** 로 간다 — GPU 0, 가장 안전, 이미 device-PASS. render 경로
   완료(measure-first)와 net overlay 를 먼저 닫는다.
2. **CR-3 진입 = ② SwiftShader** — ANGLE 자체가 뜨는지 + SwiftShader JIT 가 우리 W^X 를 통과하는지
   격리 검증(가속 아님, 안전한 계단). `--enable-unsafe-swiftshader` 로 Arm WebGL 켬.
3. **CR-3 가속 = ③ 우리 GLES shim** — `--use-angle=gles-egl` 로 ANGLE 가 우리 `libEGL.so.1`/
   `libGLESv2.so.2` 를 dlopen → Mali executor. CP-2 가 동일 shim 으로 glmark2→Mali 를 device-입증한
   토대 재사용. **soname/symlink + LD 우선순위 정합이 유일한 까다로운 점.**
4. **④ VK marshal 은 장래** — gpu-guest-accel-strategy 의 끝점(Vulkan-first). chromium 보류 + G1
   해소 + VK marshal breadth(R12 g3-vk) 이후.

> **정직 규칙.** ①만 현재 default-안전(device-PASS). ②③④ 는 device-pending — staged-lib 정합 +
> W^X(SwiftShader) + soname(shim) 게이트가 각각 device 로 닫혀야 승급. dmabuf/`/dev/dri` 는 끝까지
> **안 만족**(비root/SELinux) → swap 은 항상 `wl_shm` + AHB 합성. GPU 마샬 아키텍처 자체는
> `gpu-guest-accel-strategy.md` 가 SSOT — 이 문서는 chromium flag 매핑만.

---

## 4. 교차참조

| 문서 | 무엇 |
|---|---|
| `docs/research/chromium-run-plan.md` | CR-1..CR-5; 이 문서는 CR-3(GPU)의 세부 사다리 |
| `docs/research/gpu-guest-accel-strategy.md` | GPU 마샬 아키텍처 SSOT(ring + executor; Vulkan-first; ANGLE for GLES) |
| `docs/research/cp2-gpu-ratio-glmark2.md` | 우리 GLES shim 이 glmark2 를 Mali 에 렌더한 device 토대(③ 의 근거) |
| `docs/research/chromium-native-plan.md` | Scout 3 Ozone/GPU 정찰(ANGLE dlopen system EGL; dmabuf 미광고) |
