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

## 4. ③(가속) 정밀 실행도 — ANGLE dlopen 메커니즘 + EXACT soname/symlink + present 경로

> §1③ 를 "device 로 닫으려면 *정확히 무엇이* 어디에 있어야 하나"로 풀어 쓴다. 근거는 모두 in-repo
> 코드(`app/src/main/cpp/alr_gpu/guest_shim/*`, `MainActivity.kt`, `tools/`) + CP-2 device 증거
> (`docs/evidence/2026-06-01-cp2-glmark2-egl-dlopen-resolved.md`,
> `docs/evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md`).
> **HOST-ONLY 정찰 — device 측정 없음. shim `.c` 는 WS-2 소유(나는 안 건드림); 여기서 specify 만.**

### 4.1 chromium → ANGLE → system EGL: dlopen 사슬 (정확히 어디서 우리 shim 을 잡나)

`--use-gl=angle --use-angle=gles-egl` 일 때 chromium 의 GPU/렌더 초기화 사슬:

```
chromium  ──(번들)──▶ libEGL.so / libGLESv2.so  (= ANGLE, /usr/lib/chromium/)
   ANGLE egl_loader (gl/egl/egl_loader_autogen + system_utils)
        ──dlopen──▶ "libEGL.so.1"   (먼저 unversioned "libEGL.so" 시도 후 ".1" fallback)
        ──dlopen──▶ "libGLESv2.so.2"  (마찬가지로 "libGLESv2.so" → ".2")
              └─ 이 둘이 **우리 alr_gpu GLES shim** 이어야 함
   ANGLE  ──eglGetProcAddress── 로 EGL_* / gl* 포인터 수집
   ANGLE display type = ANGLE_PLATFORM_TYPE_OPENGLES (gles-egl)
        ──eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM/WAYLAND/DEVICE…)── 또는 eglGetDisplay
```

핵심: **ANGLE 의 GLES-EGL 백엔드는 "system EGL 위에 GLES 를 얹는" 모드** — chromium 번들 ANGLE
`libEGL.so`(ANGLE 자체)는 *내부 EGL*, 그게 다시 *system* `libEGL.so.1` 을 dlopen 한다. 그래서 두
이름공간이 동시에 산다:
- chromium dir 의 ANGLE `libEGL.so` (번들, DT_SONAME `libEGL.so` 일 수 있음 — chromium 빌드별 상이)
- rootfs `LD_LIBRARY_PATH` 의 우리 shim `libEGL.so.1`/`libGLESv2.so.2`

→ **LD 우선순위 규칙(R-LD):** 우리 shim 디렉터리(`/usr/lib/androlinux`)가 ANGLE 의 dlopen 이 system
EGL 을 찾을 때 *이긴다*. CP-2 가 이미 `LD_LIBRARY_PATH` 에 `/usr/lib/androlinux` **선두** 배치를
device-검증함(`docs/evidence/2026-06-01-cp2-glmark2-egl-dlopen-resolved.md` §11). ANGLE 번들
`libEGL.so`(unversioned, chromium dir)와
우리 shim `libEGL.so` symlink(androlinux dir)가 **같은 basename** 으로 충돌할 위험 → §4.2 의 symlink
규칙이 이걸 못 박는다.

### 4.2 EXACT soname + symlink 목록 (gpushim-stage.tar 가 실어야 하는 것)

CP-2 가 device-증명한 현 stage(`extracted=14`)는 **versioned soname 만** 확실: `libEGL.so.1`,
`libGLESv2.so.2`. glmark2 는 `DT_NEEDED libEGL.so.1`/`libGLESv2.so.2` 라서 그걸로 충분했다. 하지만
**ANGLE 의 egl_loader 는 `dlopen("libEGL.so")`(unversioned) 도 시도**한다 — `build-shim.sh` 가 만드는
`libEGL.so`→`libEGL.so.1` symlink 는 현재 **`out/` 의 *link-time* 데브 symlink** 일 뿐, 스테이지
tar 에 들어간다는 보장이 없다. 그래서 ③ 의 *추가 요구*(glmark2 가 안 건드린 칸):

| 경로(rootfs) | 종류 | SONAME/타깃 | 출처 | glmark2 가 썼나 | ANGLE 가 요구? |
|---|---|---|---|---|---|
| `/usr/lib/androlinux/libGLESv2.so.2` | real .so (0755) | SONAME `libGLESv2.so.2`, NEEDED `libc.so.6` only | shim 빌드 | ✅(이미 device) | ✅ |
| `/usr/lib/androlinux/libEGL.so.1` | real .so (0755) | SONAME `libEGL.so.1`, NEEDED `libGLESv2.so.2`+`libc.so.6` | shim 빌드 | ✅(이미 device) | ✅ |
| `/usr/lib/androlinux/libGLESv2.so` | **symlink → libGLESv2.so.2** | — | **신규 stage 요구** | ❌ | ✅(unversioned dlopen) |
| `/usr/lib/androlinux/libEGL.so` | **symlink → libEGL.so.1** | — | **신규 stage 요구** | ❌ | ✅(unversioned dlopen) |

- **`.so` 실행비트 0755 필수** — ALR 의 file-backed `PROT_EXEC`(untrusted_app)는 non-exec `.so` 를
  거부(`tools/STAGE_TAR_SPEC.md` §10.1). gpushim 은 이미 0700/0755 로 통과(glmark2 device-PASS).
- **symlink 안전 규칙(§5-E)**: 타깃은 *상대*(같은 디렉터리 basename, `libEGL.so.1`)여야 device
  extractor 가 안 버린다. `/`-absolute 타깃은 skip 됨(`tools/STAGE_TAR_SPEC.md` §0).
- **NEEDED 청결 유지**: shim 은 `libc.so.6` 외 NEEDED 가 없어야(`-target aarch64-linux-gnu.2.34`,
  no `-lpthread`). 이게 CP-2 drain#2 의 "Error loading EGL library"(libpthread.so.0 missing) 근본
  수정 — **ANGLE 의 dlopen 도 동일 tiny-rootfs 위에서 도므로 같은 청결이 필수.** (이미 영속화됨.)

### 4.3 우리 EGL shim 이 ANGLE 에 대해 *advertise* 해야 하는 것 (현 갭 = WS-2)

ANGLE 는 glmark2 보다 EGL surface/extension 질의가 많다. 현 shim 코드
(`alr_egl_shim.c`)가 **부족**한 두 지점 — *device-pending 갭, WS-2 가 shim.c 에 채울 것*:

1. **`eglQueryString(EGL_EXTENSIONS)` 가 현재 `""`** (`alr_egl_shim.c` `eglQueryString`,
   case `EGL_EXTENSIONS`). ANGLE gles-egl 백엔드는 system EGL 에서 **`EGL_KHR_surfaceless_context`**
   (offscreen FBO 경로) 와, no-display 부팅을 위해 **`EGL_EXT_client_extensions`** + (선택)
   **`EGL_KHR_platform_*`** 를 본다. 우리는 dmabuf/`/dev/dri` 를 **안 광고**하므로(§4.4) ANGLE 가
   surfaceless(=offscreen) 로 떨어지게 만드는 게 정확히 옳다 → 최소 advertise:
   `EGL_KHR_surfaceless_context EGL_KHR_create_context`. (display 질의 `eglQueryString(NULL, ...)`
   = client extension; surface 질의 = display extension — 둘 다 같은 canned 문자열로 답하면 충분.)
2. **`eglChooseConfig` 가 단일 canned config** 를 돌려주는데(이미 glmark2 device-PASS), ANGLE 는
   config 의 `EGL_RENDERABLE_TYPE` 에 **`EGL_OPENGL_ES2_BIT`(+ ES3 시 `..._ES3_BIT_KHR`)** 와
   `EGL_SURFACE_TYPE` 에 **`EGL_PBUFFER_BIT`**(surfaceless 대체) 가 켜져 있길 기대. canned config
   attrib 에 이 둘을 추가해야 ANGLE 가 "ES2/ES3 렌더 가능 + offscreen surface" 로 인식.

> 이 둘은 *shim .c* 변경이라 **WS-2 영역** — 본 문서는 specify 만. **나(WS-5)는 shim 을 안 건드린다.**
> WS-1 로의 액션은 *없음*(loader/interposer 무관) — 이 갭은 WS-2 가 `alr_egl_shim.c` 에서 닫는다.

### 4.4 present 경로 — 무엇을 UN-advertise 해야 swap 이 wl_shm + AHB 합성으로 떨어지나

ANGLE 가 **GBM/DRM zero-copy swapchain** 을 잡으면 `/dev/dri` 를 열려 하고(비root/SELinux 거부 →
크래시/폴백 난동). 그래서 **명시적으로 안 광고**:

| 인터페이스 | 상태 | 이유 |
|---|---|---|
| `zwp_linux_dmabuf_v1` (wl global) | **UN-advertise** | 광고되면 GTK/ANGLE 가 dmabuf swap 시도 → `/dev/dri` 필요 |
| GBM / `/dev/dri/*` (device node) | **부재(비root)** | 애초에 열 수 없음; ANGLE GBM 플랫폼 비활성 유지 |
| `EGL_EXT_image_dma_buf_import` (EGL ext) | **UN-advertise**(§4.3 의 canned 문자열에 넣지 않음) | dmabuf import 경로 차단 |
| `wl_shm` (wl global) | **advertise** | CPU 픽셀 swap → 컴포지터가 memcpy→AHB |
| `wl_compositor`/`wl_surface`/`xdg_*`/`wl_seat`/`wl_output`/`wl_data_device_manager` | **advertise** | GUI 부팅/입력(이미 WS-3 컴포지터가 광고; GIMP/qt6 device-PASS) |

present 흐름(이미 device-검증된 백본 재사용):
```
chromium(ANGLE→우리 GLES shim) ─draw─▶ ring ─▶ host Mali executor (AhbRenderTarget, v117 PASS)
   swap(eglSwapBuffers) ─▶ executor 가 AHB 를 WaylandPresenter 로 zero-copy present (v114 PASS)
                          또는 software swap 시 wl_shm → 컴포지터 memcpy→AHB (현 GUI 기본)
```
**즉 ③ 에서 진짜 가속은 "draw 가 Mali 에서 돈다"는 점(glmark2 1074 가 같은 shim 으로 입증)이고,
present 는 dmabuf 미광고 덕에 항상 wl_shm/AHB 합성으로 안전하게 떨어진다.** zero-copy
AHB↔dmabuf 직결은 §1④/`gpu-guest-accel-strategy` 의 후기 끝점.

### 4.5 tools/ 빌더 spec — gpushim-stage.tar 에 unversioned symlink 추가 (shim .c 불변)

§4.2 의 **신규 요구는 단 2개의 symlink** — shim `.c` 재컴파일이 *전혀 필요 없다*(기존
`libEGL.so.1`/`libGLESv2.so.2` 바이너리 그대로). build-shim.sh 의 `out/` 데브 symlink 를 *스테이지
tar 에 영속*시키는 작은 빌더/스테이지 단계만 있으면 된다. spec(구현은 빌더 소유 세션이; 본 문서는
명세):

```
NEW tools step (gpushim 스테이지 패킹 시):
  입력:  app/src/main/cpp/alr_gpu/guest_shim/out/{libEGL.so.1, libGLESv2.so.2}
  tar 멤버(./-rooted, §5-E flat-soname 규칙):
    ./usr/lib/androlinux/libGLESv2.so.2      (mode 0755, real)
    ./usr/lib/androlinux/libEGL.so.1         (mode 0755, real)
    ./usr/lib/androlinux/libGLESv2.so  -> libGLESv2.so.2   (symlink, RELATIVE 타깃)
    ./usr/lib/androlinux/libEGL.so     -> libEGL.so.1      (symlink, RELATIVE 타깃)
  검증(host gate):
    - python -m tools.stage_tar_spec --check  →  CONFORMANT (symlink 타깃 in-tree, 상대)
    - readelf -d libEGL.so.1   →  NEEDED = libGLESv2.so.2, libc.so.6 (그 외 0)
    - readelf -d libGLESv2.so.2→  NEEDED = libc.so.6 (그 외 0)
    - tools/overlay_guard.py    →  0 BLOCK (androlinux 는 private dir, base downgrade 무관)
```
- 이건 `build-shim.sh` 의 *링크 산출물* 을 바꾸지 않는다(WS-2 의 shim 영역 불변). 단지 **스테이지
  패킹 시 symlink 를 tar 에 포함**시키는 것 — `MainActivity.kt` 의 `extractOverlayTar` 가 이미
  symlink 를 처리하므로 추가 Kotlin 변경 불필요(§5-E 안전-symlink 규칙이 적용).
- glmark2(versioned NEEDED) 와 ANGLE(unversioned dlopen) 가 **동일 tar 로 둘 다 충족** → 회귀 0.

### 4.6 ③ device 승급 게이트 (정직)

오를 조건(전부 device, 순서대로):
1. **stage:** unversioned symlink 2개가 들어간 gpushim-stage.tar 가 device 에 extract(=symlink
   2개 추가; `extracted` 증가) + overlay_guard 0 BLOCK.
2. **EGL advertise:** WS-2 가 §4.3 의 `EGL_EXTENSIONS`(surfaceless) + config bits 를 shim.c 에 채움
   → ANGLE `eglInitialize`/`eglChooseConfig` 통과(현 갭).
3. **chromium:** `--use-gl=angle --use-angle=gles-egl` 로 GPU 프로세스가 `libEGL.so.1` dlopen →
   draw 가 ring→Mali executor 로 → `software=false` + 화면 픽셀(또는 `--dump-dom` 등가 GPU=1 마커).
4. **present:** dmabuf 미광고 확인(swap = wl_shm/AHB) — `/dev/dri` open 시도 0.

게이트 1 이 본 문서가 닫을 수 있는 유일한 *host* 칸(symlink 스테이지 spec). 2–4 는 device + WS-2
shim 작업.

---

## 5. 교차참조

| 문서 | 무엇 |
|---|---|
| `docs/research/chromium-run-plan.md` | CR-1..CR-5; 이 문서는 CR-3(GPU)의 세부 사다리 |
| `docs/research/gpu-guest-accel-strategy.md` | GPU 마샬 아키텍처 SSOT(ring + executor; Vulkan-first; ANGLE for GLES) |
| `docs/research/cp2-gpu-ratio-glmark2.md` | 우리 GLES shim 이 glmark2 를 Mali 에 렌더한 device 토대(③ 의 근거) |
| `docs/research/chromium-native-plan.md` | Scout 3 Ozone/GPU 정찰(ANGLE dlopen system EGL; dmabuf 미광고) |
| `app/src/main/cpp/alr_gpu/guest_shim/build-shim.sh` | shim soname/NEEDED 빌드 레시피(§4.2 의 산출물; symlink 는 데브 link-time only) |
| `app/src/main/cpp/alr_gpu/guest_shim/alr_egl_shim.c` | `eglQueryString`/`eglChooseConfig`(§4.3 의 advertise 갭; WS-2 소유) |
| `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt` | gpushim-stage.tar extractOverlayTar(§4.5 의 stage 경로; symlink 처리 기존) |
