# Device Evidence — CP-2 glmark2: EGL dlopen 해결(libpthread), 다음은 shim eglChooseConfig (WS-2)

통합 세션 device 큐 CP-2 drain (#2–#4). Device SM-X236N. glmark2-es2-wayland를 ALR loader로 실행, GpuRingHook(WS-2 CP-0)으로 host Mali executor에 연결.

## drain 경과
- **#2 (CP-2 첫):** glmark2 `exit=1`, stdout="Error: Error loading EGL library". 원인 = shim `libGLESv2.so.2`가 `DT_NEEDED libpthread.so.0`인데 ALR rootfs에 libpthread.so.0 없음(glibc ≥2.34는 pthread를 libc.so.6에 통합; tiny-rootfs에 stub 없음) → `dlopen("libEGL.so.1")` 체인 끊김. (guest ld.so는 LD_DEBUG 무시 = in-process jump.)
- **#4 (shim 재빌드):** `-lpthread` 제거 + zig target `aarch64-linux-gnu.2.34`(pthread/dl을 libc로 fold). 재검증: `libGLESv2.so.2 NEEDED = libc.so.6만`(libpthread/libdl 제거, pthread 심볼은 `@GLIBC_2.17/2.34` libc 버전). → **EGL dlopen 성공**. 다음 단계로 진행: stdout="Error: eglChooseConfig() didn't return any configs" + "Couldn't get GL visual config!".

## CP-2 인프라 = 전부 device-검증됨
- loader `alr_loader_attach_gpu_ring`(config.program=glmark2 감지) + ring(memfd)/doorbell(eventfd) + GpuExecutorService.
- guest_env `LD_LIBRARY_PATH`에 `/usr/lib/androlinux`(shim) 우선(glmark2 gate).
- gpushim(extracted=7) + glmark2(extracted=292) overlay, **WS-4 guard skipped=0**.
- Mali 백본 self-test 전부 `software renderer=false`(Mali-G615).
- **EGL library dlopen 성공**(libpthread 수정 후).

## 남은 갭 = WS-2 (gfxstream shim EGL/GLES)
glmark2 full path를 위해 shim이 구현해야: `eglChooseConfig`가 ≥1 config 반환(현재 0) → `eglCreateWindowSurface`(wl_egl_window) → `eglMakeCurrent` → GLES 호출을 ring으로 → host가 Mali replay. 이건 WS-2(alr_gpu/guest_shim) 영역. **build-shim.sh 영속화 필요(WS-2): zig target `aarch64-linux-gnu.2.34` + `-lpthread` 제거**(현재 /tmp 재빌드만, repo 미반영).

## 재분배(§10 갱신)
- **WS-2**: shim EGL config/surface/makecurrent + GLES-via-ring 완성(glmark2 eglChooseConfig→Score) + build-shim.sh 영속화. ← CP-2 최종 unblock.
- WS-1(통합): CP-2 loader 인프라 + device 큐 통제(이 evidence).
