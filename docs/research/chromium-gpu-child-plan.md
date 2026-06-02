# Chromium GPU child `exit_code=5` 진단 + GPU-child 재맵/바인딩 fix 설계 (CR-3)

> **목적.** chromium 멀티프로세스(MP)가 device 에서 렌더 달성(CR-5 stepA, `d106c30`)했으나
> **GPU child 가 `exit_code=5` 로 죽는다.** 이 문서는 (a) `exit_code=5` 근인을 *코드를 읽어*
> 가설화하고 순위 + 판별 logcat 을 제시하고, (b) GPU-child 재맵/바인딩 fix 를 **host 설계**로
> 못 박고(본체 변경 위치: 파일/함수/라인, 새 ptrace op 0, 커널 execve 0, W^X-safe), (c) device
> 사다리 CR-3 stepA/B + DEVICE-REQ 를 정의하고, (d) software fallback 으로 우회 가능한 범위를
> 정직히 명시한다.
>
> **소유/제약.** HOST-ONLY 설계. **본체 소스(`runtime_report.cpp`/`alr_inproc_reexec.c`/
> `MainActivity.kt`/`alr_gpu/**`)는 읽기만 — 수정 0.** 산출은 이 문서 + host 모델
> (`tools/chromium_gpu_child_model.py`) + 단위테스트(`tests/test_chromium_gpu_child.py`)뿐.
> device 측정 없음. baseline = 통합 트리 HEAD `6691ad1` (CR-5 stepA `d106c30` 위).
>
> **정직 규칙.** 아래에서 **[코드-실증]** = in-repo 소스를 직접 읽어 확정한 사실,
> **[추정]** = device 없이는 못 닫는 가설, **[device-only]** = darwin host 로 모델 불가.
> chromium 은 사용자 보류(held) — 어떤 칸도 device evidence 없이 RUNS 로 승급하지 않는다.

---

## 0. 한 줄 결론

**현재 device-proven 한 MP 프로브(`d106c30`)는 `--disable-gpu` 로 돈다 — 즉 chromium 이
GPU child 를 띄우더라도 그 child 는 *software 경로*로 초기화를 시도하고, 그 안에서 죽는다.
`exit_code=5` 의 가장 강한 단일 근인은 "GPU child 의 envp 에 GPU ring env(`ALR_GPU_RING_FD/
BYTES/DOORBELL_FD`)가 없어서 우리 EGL/GLES shim 이 ring-less(no-GPU) 로 뜨고, 그 위에서
chromium 의 GLES/EGL 초기화(또는 ANGLE 의 디바이스 probe)가 실패→`GpuMain` 이 비-정상
종료"** 다 — 단 이건 **[추정]**(아래 §2 가 순위와 판별을 준다). **[코드-실증]** 으로 확정된
*구조적 근인*은: **(1) GPU ring env 는 launch program 이 glmark2 일 때만 부모 envp 에 들어가고
(`runtime_report.cpp` L1731), (2) exec-trap 의 envp 재주입(B-3, `decide_exec_envp_injection`)은
`LD_PRELOAD`+`ALR_ROOTFS` 만 주입(`alr_exec.cpp` L450/453) → ring env 는 fresh GPU child 에
*절대* 전달되지 않는다.** 따라서 *가속* GPU child 는 현 코드로는 ring 을 찾을 수 없다.

**그러나 software fallback 으로 우회 가능한 범위가 넓다(§5): renderer 가 software raster 로
DOM 을 이미 렌더하므로(`d106c30` 증거), CR-5 의 MP-render 게이트는 GPU child 와 무관하게
성립한다.** GPU child 자체가 없어도 되는 경로(`--in-process-gpu`/`--disable-gpu` + sw raster)가
존재한다 — CR-3(가속)만이 GPU child 의 정상화를 *요구*한다.

---

## 1. 무대 — GPU child 가 어떻게 뜨고 어디까지 가나 (코드-실증)

### 1.1 GPU child 의 출생: `fork()+execve("/proc/self/exe", ["--type=gpu-process", …fd])`

**[코드-실증]** `docs/research/chromium-multiprocess-plan.md` §1 fork-그래프(L67) + Scout 2:
`--no-zygote` 일 때 browser 가 자식을 띄우는 방식은 `fork()+execve("/proc/self/exe",
["--type=zygote|gpu-process|utility", …상속fd])`. GPU child 도 클래스 B(fresh-execve, 자기
재실행)다. 그 execve 가 ALR supervisor 의 exec-trap 에 걸리고:

- **GATE-1(`d106c30`)** 이 `/proc/self/exe` → launch guest 의 rootfs `host_path` 로 치환
  (`runtime_report.cpp` L2731-2817). x19=host_path, x20=argv(원본), x21=envp(원본+B-3 주입),
  x22=rootfs 로 `alr_inproc_reexec_trampoline` 에 PC-redirect (커널 execve 취소
  `NT_ARM_SYSTEM_CALL=-1`).
- 그 결과 GPU child 는 **커널 execve 없이** in-process 재맵되어 같은 chrome 바이너리 이미지를
  새로 매핑하고 `--type=gpu-process` 로 `GpuMain()` 진입한다.

→ **GPU child 가 "뜨는" 데까지는 GATE-1 이 이미 푼다**(renderer 와 동일 경로). renderer 가
PASS 한 `d106c30` 증거(`inproc_redirected=6`, `mapped, jumping ×6`)는 GPU child 가 *재맵 진입*
까지는 간다는 강한 정황이다 — 6개의 redirect 중 일부가 GPU/utility child 일 개연성. **[추정]**:
정확히 어느 child(renderer vs gpu vs utility)가 6개를 구성하는지는 `first_exec_x0`(L2583)+
per-child 로깅으로 device 확정해야 한다.

### 1.2 재맵된 GPU child 의 envp 는 무엇을 담는가 (← 근인의 핵심)

**[코드-실증]** in-process worker(`alr_inproc_reexec_worker`)는 envp 를 **x21** 에서 읽는다
(`alr_inproc_reexec.c` L635/L718/L846). 그 x21 은 exec-trap 에서 x2(execve 의 envp)의 스냅샷
(`runtime_report.cpp` L2700-2713 주석: "the worker reads envp from x21 (a snapshot of x2 taken
HERE)"). 그리고 B-3(`runtime_report.cpp` L3048-3188)가 그 envp 를 읽어
`decide_exec_envp_injection`(`alr_exec.cpp`)으로 분류 → **`LD_PRELOAD`(interpose .so) + 
`ALR_ROOTFS` 만** 추가/치환(`alr_exec.cpp` L450/L453)하고 x21 을 augment 된 배열로 re-point.

**즉 재맵된 GPU child 의 envp =**
```
[부모 chromium 이 execve 로 넘긴 envp]  +  [B-3: LD_PRELOAD=<rootfs interpose .so>, ALR_ROOTFS=<rootfs>]
```
**여기에 `ALR_GPU_RING_FD/BYTES/DOORBELL_FD` 는 들어있지 않다.**

### 1.3 GPU ring env 는 *애초에 부모에게도* 없다 (chromium ≠ glmark2)

**[코드-실증]** ring 을 만들고 env 를 푸는 유일한 지점:
```cpp
// runtime_report.cpp L1729-1740
alr::gpu::GpuRing gpu_ring{};
bool gpu_ring_attached = false;
if (config.program.find("glmark2") != std::string::npos) {   // ← glmark2 GATE
    alr::gpu::GpuRingAttachConfig gcfg; gcfg.fb_w = 1280; gcfg.fb_h = 720;
    if (alr::gpu::alr_loader_attach_gpu_ring(gpu_ring, gcfg)) {
        gpu_ring_attached = true;
        for (const auto& kv : alr::gpu::gpu_ring_guest_env(gpu_ring))
            guest_env.push_back(kv);       // ← only here are RING_FD/BYTES/DOORBELL pushed
    }
}
```
launch program 이 `glmark2` 를 *포함하지 않으면* `alr_loader_attach_gpu_ring` 자체가 호출되지
않는다 → **ring fd 가 존재하지 않고**, executor(host Mali thread)도 안 뜬다. chromium 의
program 은 `/usr/lib/chromium/chromium-headless-shell` 이라 이 분기에 안 걸린다.

→ **결론(코드-실증):** chromium MP 에서 GPU child 가 우리 GLES shim 을 dlopen 하더라도, 그
shim 은 `ALR_GPU_RING_FD` 를 env 에서 못 찾아 **ring-less("no GPU") 모드**로 뜬다
(`alr_shim_runtime.c` L81-85, L105 `if (!s->ring_ok) return;`). 모든 GL emit 이 no-op.

### 1.4 우리 EGL shim 은 디바이스 노드를 *안* 연다 (코드-실증)

**[코드-실증]** `alr_egl_shim.c` `eglGetDisplay`(L55)는 `/dev/dri`/`/dev/mali` 를 열지 않고
sentinel `&g_display_tag` 만 반환. `eglInitialize`/`eglChooseConfig`/`eglGetConfigAttrib` 모두
canned 응답. **우리 shim 경로에서는 디바이스 노드 부재가 블로커가 아니다.** (단 ANGLE 이 우리
shim 에 *닿기 전에* 자체 GBM/DRM 플랫폼으로 `/dev/dri` 를 열려 할 수 있음 — §2 H3.)

---

## 2. `exit_code=5` 근인 가설 — 순위 + 판별 logcat

> chromium 은 자식 종료를 `RESULT_CODE_*`(content/public/common/result_codes.h) 로 보고하지만,
> *프로세스 exit status* 5 는 chromium enum 과 1:1 이 아니다(`exit_code` 는 OS waitpid status).
> 아래는 "재맵된 GPU child 가 `GpuMain` 안에서 비정상 종료(status 5)"의 **물리적 근인** 후보를
> 우리 코드 사실 위에 순위화한 것이다. 각 가설은 **그것이 참이면 software fallback 으로 우회
> 가능한지**와 **판별 logcat** 을 동반한다. **device 없이는 어느 것도 단정 불가 — 순위는 코드
> 사실의 강도(structural certainty) 순.**

### H1 (1순위) — GPU child 가 ring-less shim 위에서 GLES/EGL 초기화 실패 [추정, 구조 근거 강]
- **메커니즘:** §1.2/1.3 [코드-실증] — GPU child envp 에 `ALR_GPU_RING_FD` 없음 → shim ring-less
  → 모든 `gl*`/`egl*` emit 이 no-op(또는 canned). chromium `GpuMain` 의 `GLDisplayEGL` 초기화
  /`InitializeGLOneOffPlatform` 에서 실제 컨텍스트가 안 만들어지면(또는 첫 draw/`glGetError`
  /`eglMakeCurrent` 결과가 모순) GPU init 실패 → child 비정상 종료.
- **단, `--disable-gpu` 일 때도 GPU child 가 뜨나?** chromium 은 `--disable-gpu` 에서도 보통
  *GPU process 를 띄우되 software(SwiftShader/sw raster)* 로 돌린다(완전 in-process 가 아니면).
  그 software 경로조차 EGL surface/컨텍스트 질의를 하므로 우리 ring-less shim 과 만나면 모순.
- **판별 logcat (DEVICE-REQ §4 ALR-CR3-diag-shim):**
  - guest stderr 에 `[alr-shim] no ALR_GPU_RING_FD/ALR_GPU_RING_BYTES in env — running ring-less
    (no GPU)` 가 **GPU child 컨텍스트에서** 찍히는가 (`alr_shim_runtime.c` L83). → 찍히면 H1 확정
    (shim 이 ring 을 못 찾음).
  - chromium stderr(`--enable-logging=stderr --v=1`)에 `GpuInit`/`gl_*`/`EGL`/`Passthrough`
    /`InitializeGLOneOff` 실패 메시지 + child 종료 직전 라인.
- **software fallback 우회?** **예 — §5.** `--in-process-gpu`(GPU 를 browser 프로세스 안에서)면
  별도 GPU child 자체가 사라진다. 또는 `--disable-gpu-compositing` + sw raster.

### H2 (2순위) — GPU child 가 우리 shim 을 *아예 안 잡고* 번들 ANGLE/SwiftShader 진입 실패 [추정]
- **메커니즘:** `--use-gl` 플래그가 없으면(현 프로브엔 없음) chromium 의 GL 백엔드 선택이
  플랫폼 디폴트로 가는데, headless+no `--use-gl` 이면 ANGLE-on-EGL 또는 SwiftShader 를 시도.
  ANGLE 은 **system `libEGL.so.1` 을 dlopen** 하려 하고(`chromium-gpu-path.md` §4.1), 우리 shim
  이 `LD_LIBRARY_PATH` 선두에 없으면 *못 찾거나*(→ "Error loading EGL library" 류 실패), 찾아도
  §4.3 의 EGL extension/ config bit 갭(WS-2 미충족)으로 `eglInitialize`/`eglChooseConfig` 실패.
  SwiftShader 경로면 `libvk_swiftshader.so` 번들이 stage 에 있어야(§5.2 chromium-gpu-path
  미확인) + JIT(execmem) 필요.
- **판별 logcat:**
  - chromium stderr 에 `Error loading EGL library` / `Failed to load .../libEGL.so` /
    `eglInitialize failed` / `Couldn't initialize ANGLE` 류. → ANGLE 이 우리 shim 을 못 잡음.
  - `ld.so` 의 dlopen 실패(`cannot open shared object libEGL.so.1`) 가 guest stderr 에.
- **software fallback 우회?** 부분 — `--disable-gpu` 로 GPU 가속 자체를 끄면 ANGLE 경로 회피.
  단 H1 처럼 software GPU child 가 여전히 뜰 수 있다.

### H3 (3순위) — GPU child 가 `/dev/dri`/GBM/DRM 을 열려다 거부 [추정, device-only]
- **메커니즘:** ANGLE 의 GBM/DRM 플랫폼 또는 chromium 의 `gpu::GpuMemoryBufferSupport` 가
  `/dev/dri/card*`/`renderD*` 를 open 시도 → untrusted_app + 비root + SELinux 거부(EACCES/ENOENT)
  → GPU init 실패. **단 우리 EGL shim 은 디바이스를 안 여므로([코드-실증] §1.4), 이건 ANGLE/
  chromium 이 *우리 shim 전에* 자체 디바이스 probe 를 할 때만** 발생.
- **판별 logcat:**
  - `strace`-등가 대신: chromium stderr 에 `Failed to open /dev/dri` / `gbm_create_device` /
    `drmOpen` / `Permission denied .../renderD128`. → 디바이스 노드 근인.
  - 또는 supervisor 의 path-trap 로그에 `/dev/dri/...` openat 시도(트랩되면).
- **software fallback 우회?** 예 — `zwp_linux_dmabuf_v1` 미광고(이미; `chromium-ozone-wayland.md`
  §S8) + `--disable-gpu` 로 GBM/DRM 경로를 안 타게. 디바이스 노드는 **끝까지 부재**(비root) 가
  ALR 의 영구 제약이므로, GPU child 는 항상 디바이스-less 경로(우리 shim/SwiftShader)로 가야 함.

### H4 (4순위) — dynamic-PIE 재맵 자체가 GPU child 의 추가 .so 클로저에서 실패 [추정, GATE-2 의존]
- **메커니즘:** GPU child 는 renderer 와 같은 chrome 바이너리지만 `--type=gpu-process` 가
  dlopen 하는 .so 집합(GL/ANGLE/드라이버 stub)이 renderer 와 다를 수 있다. dynamic ld.so 재맵
  (`alr_inproc_reexec.c` L633-706)이 그 클로저를 완전히 못 풀면(특정 .so 의 IRELATIVE/TLS/
  init_array 경로) child 가 죽는다. **단 renderer 가 같은 메커니즘으로 PASS(`d106c30`)했으므로
  *공통* dynamic 경로는 동작** — GPU 전용 .so 만의 추가 갭일 때만 H4.
- **판별 logcat:**
  - guest stderr 에 `ALR-INPROC: interp map fail` / `prog map fail` / ld.so 의 `symbol lookup
    error` / `undefined symbol` — GPU child 컨텍스트에서만.
  - renderer 는 `mapped, jumping` 인데 GPU child 만 그 직후 SIGILL/SIGSEGV.
- **software fallback 우회?** 부분 — `--in-process-gpu`(별도 GPU child 제거)면 이 표면 자체가
  사라짐. GATE-2(dynamic 재맵 일반화)는 G1 lane 소관.

### H5 (5순위) — GPU child 의 mojo/IPC fd 정합성 실패 (재맵은 됐으나 IPC 끊김) [device-only]
- **메커니즘:** GPU child 는 browser 와 mojo(GPU channel) + shared-memory(memfd) + SCM_RIGHTS
  로 통신. 재맵 후 fd *번호*는 보존(`chromium-multiprocess-plan.md` §2.1 [코드-실증]: execve 부재
  → fd 불변)되나, GPU child 가 GPU channel 핸드셰이크에 실패하면 browser 가 GPU child 를
  종료시킬 수 있다(→ exit 5 류). **이건 mojo 프로토콜 레벨 — host(darwin) 모델 불가.**
- **판별 logcat:**
  - browser stderr 에 `GPU process exited unexpectedly` / `Lost UI shared context` /
    `GpuChannelHost` / `gpu_process_host` 재시작 루프(N회 후 포기).
- **software fallback 우회?** 예 — `--in-process-gpu` 면 GPU channel 이 in-process(IPC 불요).

### 순위 요약표

| # | 가설 | 근거 강도 | 판별 핵심 logcat | sw fallback 우회 |
|---|---|---|---|---|
| **H1** | ring-less shim 위 GLES/EGL init 실패 | **[코드-실증] 구조 근거 최강**(ring env 부재 확정) | `[alr-shim] … running ring-less (no GPU)` in GPU child | **예**(`--in-process-gpu`/`--disable-gpu`+sw raster) |
| **H2** | ANGLE 이 우리 shim 못 잡음 / 번들 백엔드 init 실패 | [추정] (소나/EGL 갭 알려짐) | `Error loading EGL library` / `eglInitialize failed` | 부분(`--disable-gpu`) |
| **H3** | `/dev/dri`/GBM/DRM open 거부 | [추정][device-only] (우리 shim 은 안 엶) | `Failed to open /dev/dri` / `Permission denied renderD*` | 예(dmabuf 미광고+`--disable-gpu`) |
| **H4** | GPU 전용 .so dynamic 재맵 실패 | [추정] (renderer 공통경로는 PASS) | `ALR-INPROC: interp map fail` / `undefined symbol` (GPU child만) | 부분(`--in-process-gpu`) |
| **H5** | mojo/GPU-channel IPC 정합 실패 | [추정][device-only] | `GPU process exited unexpectedly` (browser) | 예(`--in-process-gpu`) |

> **첫 device 라운드의 1줄 판별기:** GPU child 컨텍스트의 guest stderr 에
> `[alr-shim] … running ring-less (no GPU)` 가 **찍히는지** 가 H1 을 1-bit 으로 가른다(우리 코드가
> 그 로그를 이미 emit 함, `alr_shim_runtime.c` L83). 찍히면 H1 우선(ring 미배선). 그와 *별개로*
> chromium stderr 의 마지막 50줄(특히 `Gpu`/`gl`/`EGL`/`ANGLE`/`dri`)이 H2/H3 을 가른다.

---

## 3. GPU-child 재맵/바인딩 fix 설계 (본체 변경 위치 — 제안만, 코드 미수정)

> **원칙(불변):** 새 ptrace op 0, 커널 execve 0, 새 execmem 0, W^X-safe. GATE-1 의 재맵
> 메커니즘은 **이미 GPU child 에 적용된다**(renderer 와 동일). 따라서 이 fix 는 *재맵* 이 아니라
> **재맵된 GPU child 가 우리 Mali 가속 ring 에 바인딩되게 env 를 잇는 것**이 핵심이다.

### 3.A (필수, 가속의 선결) — GPU ring env 를 chromium 에도 attach + B-3 가 그 env 를 child 로 전파

#### 3.A-1 부모(launch)에서 chromium 도 ring 을 attach
- **위치:** `runtime_report.cpp` L1729-1740 (위 §1.3 의 glmark2-GATE 블록).
- **제안:** `config.program.find("glmark2")` 단일 조건을, "GPU 가속을 *요청한* 게스트"로 일반화
  하는 *별도 게이트* 로 넓힌다 — 예: launch program 이 chromium 이고 argv 에 `--use-gl=angle`
  (또는 새 env `ALR_GPU_ACCEL=1`)이 있을 때도 `alr_loader_attach_gpu_ring` 호출 + 
  `gpu_ring_guest_env` push. **단 이것만으론 부모(browser) 프로세스에만 ring env 가 붙는다 —
  GPU child 는 fresh-execve 라 §3.A-2 가 없으면 못 받는다.**
- **W^X/op 영향:** 없음(env 추가 + 기존 attach 호출 재사용). ptrace 무관.
- **리스크-3(코드-실증, `chromium-multiprocess-plan.md` §리스크-3):** 현 ring 은 **단일 SPSC
  consumer**(`gpu-guest-accel-strategy.md` §5 "one ring, one host executor"). multi-renderer +
  GPU child 가 같은 ring 을 interleave 하면 명령 스트림이 섞인다. **CR-3 1차는
  `--renderer-process-limit=1` + (이상적으로) GPU 를 단일 child 로 묶어** 단일 producer 를
  유지해야 한다. N-ring/per-child tagging 은 breadth(별도 lane).

#### 3.A-2 (핵심) B-3 의 envp 주입에 GPU ring env 를 포함
- **위치:** `alr_runtime/alr_exec.cpp` `decide_exec_envp_injection`(현재 `add_entries` 에
  `LD_PRELOAD`/`ALR_ROOTFS` 만 push, L450/453) + 그 호출부 `runtime_report.cpp` L3104-3107.
- **제안(설계):** 부모가 ring 을 attach 했을 때(§3.A-1), supervisor 는 `ALR_GPU_RING_FD/BYTES/
  DOORBELL_FD` 의 *현재 값*(부모가 만든 fd 번호 — fork 자식이 상속 + execve 부재로 보존)을 알고
  있다. 그 3개 env 를 **B-3 의 분류기에 "추가 주입 후보"로 넘겨**, 자식 envp 가 그것을 결여하면
  `add_entries` 에 더하게 한다. 그러면 §1.2 의 augment 된 x21 에 ring env 가 실려 GPU child 의
  shim 이 ring 을 찾는다.
  - **구현 형태(제안):** `decide_exec_envp_injection` 에 optional 파라미터
    `const std::vector<std::string>& extra_required`(또는 ring env 3-튜플)를 추가하고, 호출부
    (L3104)에서 `gpu_ring_attached` 일 때 `gpu_ring_guest_env(gpu_ring)` 결과를 넘긴다. 분류기는
    "child envp 에 키가 없으면 add" 의 *순수* 로직만 확장 — host-testable.
  - **새 ptrace op / execve / execmem 0:** envp 배열 재작성은 B-3 가 *이미* 하는 일
    (`runtime_report.cpp` L3111-3188 의 string-blob + pointer-array). 추가는 `add_entries` 가
    3개 더 길어지는 것뿐. x21 re-point 도 기존 경로(L2700-2713 의 inproc_redirected_this_trap
    mirror).
- **fd 번호 정합(코드-실증 + [추정]):** ring fd 는 non-CLOEXEC 로 생성(`alr_gpu_ring_hook.hpp`
  헤더 계약 §"clear FD_CLOEXEC")되고, in-process 재맵은 execve 를 안 하므로 **fd 번호가
  보존**(`chromium-multiprocess-plan.md` §2.1 [코드-실증]). 따라서 부모의 ring fd 번호 N 을 그대로
  env 에 박으면 GPU child 에서 같은 N 이 같은 memfd 를 가리킨다. **[추정 — device 확인 필요]:**
  chromium 이 fork↔execve 사이에 fd N 을 close/재사용하지 않는지(`--shared-files`/mojo fd
  관리와 충돌). 충돌하면 ring fd 를 chromium 의 fd 사용역과 *겹치지 않는 높은 번호*로 dup2 해야
  할 수 있음 — DEVICE-REQ ALR-CR3-ringfd.

#### 3.A-3 (대안, 더 단순) `--in-process-gpu` 로 GPU child 제거
- **위치:** `MainActivity.kt` 의 chromium MP 프로브 argv(현 L172-198, `.alr-crmp` 게이트). **읽기
  전용 — 제안만.**
- **제안:** 별도 GPU child 를 띄우지 않게 `--in-process-gpu` 를 argv 에 추가. GPU 가 browser
  프로세스(=ALR 가 직접 launch, ring env 가 **부모에 직접** 붙음, §3.A-1)에서 돌면 fresh-execve
  child + envp 전파(§3.A-2) 문제가 통째로 사라진다. **이게 CR-3 의 가장 빠른 가속 경로** — 단일
  프로세스 GPU + 단일 ring producer(리스크-3 회피).
  - **trade-off:** in-process GPU 는 chromium 의 권장 격리 모델이 아니다(GPU 크래시가 browser 를
    같이 죽임). 그러나 ALR 의 1차 가속 증명에는 정확히 맞다(격리는 후속). renderer 는 여전히
    별도 child(MP-render 게이트 유지).

### 3.B (필수, ANGLE 바인딩) — soname/symlink + EGL advertise 갭

이 둘은 **WS-2 영역**(shim `.c` + stage tar)이고 `chromium-gpu-path.md` §4.2/§4.3/§4.5 가 이미
specify 했다. WS-1(본체)로의 액션 0. 요지만 재기재:
- **soname:** `/usr/lib/androlinux/libEGL.so.1`+`libGLESv2.so.2` + **unversioned symlink**
  (`libEGL.so`→`.1`, `libGLESv2.so`→`.2`) 를 gpushim-stage.tar 에 영속(ANGLE 은 unversioned
  dlopen). `LD_LIBRARY_PATH` 선두에 `/usr/lib/androlinux`(CP-2 device-검증).
- **EGL advertise:** `alr_egl_shim.c` `eglQueryString(EGL_EXTENSIONS)` 에 최소
  `EGL_KHR_surfaceless_context EGL_KHR_create_context`, `eglChooseConfig`/config bit 에
  `EGL_OPENGL_ES2_BIT`+`EGL_PBUFFER_BIT`(ANGLE gles-egl 가 기대). dmabuf/`EGL_EXT_image_dma_buf_
  import` 는 **UN-advertise**(GBM/`/dev/dri` 경로 차단 → H3 회피).

### 3.C fix 적용 순서 (의존 그래프)

```
선결: GATE-2(dynamic 재맵 일반화, G1 lane) device-안정 — renderer 가 PASS 했으나 GPU 전용 .so 는 미확정(H4)
  │
  ├─[가장 빠른 가속] 3.A-3 --in-process-gpu  → GPU child 제거, 부모 ring(3.A-1)에 직결
  │        + 3.B soname/EGL advertise (WS-2)
  │        → CR-3 stepA (single-process-GPU 가속)
  │
  └─[격리 GPU child 가속] 3.A-1 + 3.A-2 (ring env 를 fresh GPU child 로 전파) + 3.A ringfd 정합
           + 3.B
           + 리스크-3 해소(단일 producer 또는 N-ring)
           → CR-3 stepB (out-of-process-GPU 가속)
```

---

## 4. device 사다리 CR-3 stepA/B + DEVICE-REQ

> 전 구간 `--no-sandbox` 필수(`chromium-multiprocess-plan.md` §리스크-2 [코드-실증]: chromium
> 자체 seccomp 가 ALR path/execve 트랩을 most-restrictive-wins 로 DENY). `am force-stop` 먼저
> (`device-test-force-stop-first` 메모). 외부 strace 금지 — supervisor 내부 집계 + guest/chromium
> stderr 로만.

### 선행 진단 게이트 (가속 코드 *전*, 근인 1-bit 확정)
```
DEVICE-REQ: ALR-CR3-diag-shim — SM-X236N (am force-stop first); ALR_REEXEC_INPROC=1;
 현 d106c30 MP 프로브를 --disable-gpu 대신 GPU child 가 *뜨도록* 두고(=--disable-gpu 제거 OR
 --in-process-gpu 미사용) --enable-logging=stderr --v=1 로 1회;
 capture: (i) GPU child 컨텍스트 guest stderr 에 `[alr-shim] … running ring-less (no GPU)` 유무
          → 있으면 H1 확정(ring 미배선), (ii) chromium stderr 마지막 50줄의 Gpu/gl/EGL/ANGLE/dri 라인
          → H2(EGL load 실패) vs H3(/dev/dri 거부) vs H4(map/symbol) vs H5(GPU process exited) 분류,
          (iii) supervisor 집계 first_exec_x0 = GPU child 가 /proc/self/exe vs 절대경로 vs /proc/<pid>/exe.
 gate = (i)+(ii) 가 H1..H5 중 하나로 근인을 좁힘. 이건 *가속 코드 없이* 진단만 — 보류 해제 불요
        (현 프로브 변형, 새 본체 코드 0).
```

### CR-3 stepA — single-process GPU 가속 (3.A-3 + 3.A-1 + 3.B)
```
DEVICE-REQ: ALR-CR3-stepA — SM-X236N (am force-stop first); ALR_REEXEC_INPROC=1 + ALR_GPU_ACCEL=1
 (또는 글마크 게이트 일반화 후); chromium-headless-shell
   --no-sandbox --no-zygote --renderer-process-limit=1 --disable-dev-shm-usage
   --in-process-gpu --use-gl=angle --use-angle=gles-egl
   --user-data-dir=<rootfs-writable>  --dump-dom data:text/html,<canvas-or-webgl-marker>;
 선결: 부모가 ring attach(3.A-1) → guest stderr 에 ring-less 로그가 *사라짐*; gpushim-stage 의
   unversioned symlink + EGL advertise(3.B) 적용.
 expect: (i) ANGLE 이 우리 libEGL.so.1 dlopen 성공(Error loading EGL 없음),
         (ii) GL_RENDERER ≠ software / executor 가 ring 명령 소비(host Mali thread 활성),
         (iii) child exit=0 + DOM/마커 산출, (iv) /dev/dri open 시도 0(dmabuf 미광고).
 gate = (ii)+(iii) — draw 가 Mali ring→executor 로 돌고 정상 종료. 반증: ring-less 로그 잔존
        ⇒ 3.A-1 미배선; Error loading EGL ⇒ 3.B soname/LD 경로; SIGILL ⇒ GATE-2(H4).
```

### CR-3 stepB — out-of-process GPU child 가속 (3.A-1 + 3.A-2 + ringfd 정합)
```
DEVICE-REQ: ALR-CR3-stepB — stepA PASS 후, --in-process-gpu 제거(별도 GPU child) +
 B-3 ring-env 전파(3.A-2) 적용; 같은 --use-gl=angle --use-angle=gles-egl;
 expect: 별도 GPU child 가 (i) 재맵 진입(mapped, jumping) AND (ii) envp 에 ALR_GPU_RING_FD 가
   실려 ring-less 로그 *없이* shim 이 ring 에 attach, (iii) GPU channel(mojo) 핸드셰이크 완료,
   (iv) draw→Mali→exit=0.
 gate = (ii)+(iii)+(iv). 반증: ring-less 잔존 ⇒ 3.A-2 미전파 또는 ringfd 번호 충돌(→ ALR-CR3-ringfd);
   GPU process exited unexpectedly ⇒ H5(mojo, device-only 디버그).
```
```
DEVICE-REQ: ALR-CR3-ringfd — (stepB 부수) GPU child 의 fd 번호 N(ALR_GPU_RING_FD)이 chromium 의
 --shared-files/mojo fd 와 충돌하지 않는가: gate = shim 이 mmap(N) 성공(ALRG 매직 검증 통과,
 `alr_shim_runtime.c` L68). 반증 = "region is not a valid ALRG ring (magic/version/size mismatch)"
 ⇒ fd N 이 다른 객체로 재사용됨 → 부모에서 ring fd 를 높은 번호로 dup2 후 그 번호를 env 에.
```

---

## 5. software fallback 으로 우회 가능한 범위 (정직)

**렌더는 software 로 이미 된다 — GPU child 정상화는 CR-3(가속)만의 요구다.** 우회 가능 범위:

| 목표 | software 경로 | GPU child 필요? | 근거 |
|---|---|---|---|
| **MP DOM 렌더(CR-5 게이트)** | `--disable-gpu` + sw raster, renderer 가 DOM 직렬화 | **불요** | `d106c30` device-PASS (`<h1>ALR-CRMP-OK</h1>` 렌더, `--disable-gpu`) [코드-실증] |
| **on-screen GUI(CR-4)** | `--ozone-platform=wayland --disable-gpu`, wl_shm 프레임 → AHB 합성 | **불요** | `chromium-ozone-wayland.md` §3/§6 (GIMP 와 동일 present 경로) [추정, device-pending] |
| **WebGL/3D 가속(CR-3)** | 없음(software raster 는 3D 가속 0) | **필요** | GPU child(또는 in-process-gpu)가 Mali ring 에 바인딩돼야 — §3 [추정] |
| **GPU child crash 회피(가속 미요구 시)** | `--in-process-gpu`(GPU 를 browser 안에서) 또는 `--disable-gpu-compositing` | **불요(child 제거)** | §3.A-3 [추정 — device 확인] |

**핵심:** `exit_code=5` 의 GPU child 는 **MP-render 게이트를 막지 않는다**(renderer 가 sw 로
렌더). 따라서:
- **CR-5(MP 렌더) 진행에는 GPU child fix 불요** — 현 `--disable-gpu` 유지.
- **GPU child crash 가 *부작용*(예: browser 가 GPU child 재시작 루프로 throughput 잡아먹음)을
  내면**, `--in-process-gpu`(§3.A-3)로 child 자체를 제거해 *부작용만* 우회(가속 없이).
- **CR-3(가속)만이** §3.A/3.B 의 ring-env 배선 + soname/advertise 를 *요구*한다.

> **[추정] 주의:** "GPU child 가 죽어도 renderer 가 sw 로 렌더하니 무해" 는 `--dump-dom`
> (headless) 경로의 [코드-실증]. **on-screen(CR-4)에서 GPU child crash 가 compositing 을
> 막는지**는 device-only — `--disable-gpu-compositing`/`--in-process-gpu` 로 sw compositing 을
> 강제하면 우회되리라는 게 [추정]. 첫 CR-4 device 라운드가 확정.

---

## 6. host 모델 + 단위테스트 (본체 미수정)

§1.2/1.3/2 의 *결정 로직*(launch program + GPU child type + env 상태 → ring-attached 여부 +
예측 근인)을 순수 함수로 박았다. darwin host 에서 ptrace/seccomp/Mali 없이 **결정만** 검증:

- **`tools/chromium_gpu_child_model.py`** — `predict_gpu_child_ring(launch_program, child_argv,
  parent_env, b3_injected_keys)` → `RingBinding{attached, reason, primary_hypothesis}`. §1.3 의
  glmark2-게이트, §1.2 의 B-3 주입(LD_PRELOAD/ALR_ROOTFS only), §3.A-1/3.A-2 의 fix 후 상태를
  모델링. **이건 *현재 코드의 동작* 과 *제안 fix 후 동작* 을 둘 다 표로 고정**해, device 라운드가
  무엇을 봐야 하는지의 SSOT.
- **`tests/test_chromium_gpu_child.py`** — 위 모델의 결정 테이블(현 chromium=ring-less→H1,
  glmark2=ring-attached, fix 후 chromium+accel=ring-attached, in-process-gpu=no-child) 단위검증.

> 이 모델은 본체 소스를 *읽어* 만든 동작 명세일 뿐 — 본체 0 수정. `proc_self_exe_model.py`
> /`test_proc_self_exe_gate.py`(GATE-1)와 동일한 host-SSOT 패턴.

---

## 7. 교차참조

| 문서/소스 | 무엇 |
|---|---|
| `docs/research/chromium-multiprocess-plan.md` | CR-5 MP 게이트(GATE-1/2/3), 리스크-3(single-ring), §2.1 fd 보존 [코드-실증] |
| `docs/research/chromium-gpu-path.md` | CR-3 flag 사다리 + ANGLE dlopen soname/symlink(§4.2) + EGL advertise 갭(§4.3) — 본 문서 §3.B 의 출처 |
| `docs/research/chromium-ozone-wayland.md` | CR-4 sw present(wl_shm→AHB), dmabuf 미광고(H3 회피) |
| `docs/research/gpu-guest-accel-strategy.md` | "one ring, one host executor"(§5) — 리스크-3 의 근거 |
| `app/src/main/cpp/runtime_report.cpp` L1729-1740 | ring attach 의 glmark2-게이트(§1.3) — **읽기전용**, §3.A-1 변경 위치 제안 |
| `app/src/main/cpp/runtime_report.cpp` L2700-2817, L3048-3188 | GATE-1 self-exe subst + B-3 envp 주입 — **읽기전용**, §3.A-2 변경 위치 제안 |
| `app/src/main/cpp/alr_runtime/alr_exec.cpp` L450/453 | `decide_exec_envp_injection` add_entries(LD_PRELOAD/ALR_ROOTFS only) — §3.A-2 |
| `app/src/main/cpp/alr_gpu/guest_shim/alr_shim_runtime.c` L81-105 | ring-less no-op + `running ring-less (no GPU)` 로그(H1 1-bit 판별기) |
| `app/src/main/cpp/alr_gpu/guest_shim/alr_shim_env.h` | ring env 키 계약(`ALR_GPU_RING_FD/BYTES/DOORBELL_FD`) |
| `app/src/main/cpp/alr_gpu/guest_shim/alr_egl_shim.c` | eglGetDisplay(디바이스 안 엶, §1.4) + advertise 갭(§3.B, WS-2) |
| `app/src/main/cpp/alr_gpu/alr_gpu_ring_hook.hpp` | CP-0 ring 계약(non-CLOEXEC fd 상속), "launched by in-process jump … fds inherited at fork" |
| `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt` L172-198 | `.alr-crmp` MP 프로브(현 `--disable-gpu`) — §3.A-3 argv 제안 위치 |
| `tools/proc_self_exe_model.py` / `tests/test_proc_self_exe_gate.py` | GATE-1 host-SSOT 패턴(본 문서 §6 가 따름) |
