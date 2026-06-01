# Device Evidence — CP-2 Score drain: shim EGL 완벽한데 glmark2 reject(glmark2-side, WS-2); GUI overlay(noble)+CP-3 준비

gpushim(ws-2 전체 EGL/GLES 재빌드 256KB) + 새 APK(CP-3 microbench + ws-3 통합) cold start. Device SM-X236N / Mali-G615.

## CP-2 glmark2: shim EGL 정상인데 여전 "Failed to find suitable EGL config"
shim 코드 직접 검증(alr_egl_shim.c):
- **eglChooseConfig**: `configs==NULL`→count(1); `config_size>0`→`configs[0]=ALR_EGL_CONFIG` + `*num_config=1`; `return EGL_TRUE`. count+fill 정상.
- **eglGetConfigAttrib**: RED/GREEN/BLUE/ALPHA=8, DEPTH=24, STENCIL=8, BUFFER_SIZE=32, RENDERABLE/CONFORMANT=ES2_BIT, SURFACE=WINDOW|PBUFFER, COLOR_BUFFER_TYPE=RGB, CONFIG_ID=1, CAVEAT=NONE, default→0/EGL_TRUE. 완전한 8888/d24/s8 ES2.
- 그런데 glmark2 `exit=1` "Failed to find suitable EGL config" / "Could not initialize canvas"(stdout 127B). → 원인이 shim이 아니라 **glmark2 libmatrix `GLStateEGL::gotValidConfig`/`GLVisualConfig::match_score`** 쪽.
- **WS-2 다음(DEVICE-REQ)**: glmark2-es2-wayland binary로 config 선택 디버그 — (a) match_score 가중치(원치 않는 alpha=8/stencil=8이 penalty로 score≤0?), (b) `EGL_NATIVE_VISUAL_ID=0`이 wayland config 매칭을 깨는지, (c) eglGetConfigs vs eglChooseConfig 경로 중 어느 것을 쓰는지. shim attr는 전부 충족 — glmark2 select 로직 매칭이 관건.

## gtk3-widget-factory SIGABRT: GUI overlay 미적용(이번 drain)
- `gtk3-widget-factory exit=-1 sig=6` 여전 — `libpixbufloader_svg.so` 없음 → image-missing.svg 로드 실패 → Gtk:ERROR abort. (gtkdemo는 rendered=true frames 12→13.)
- **GUI overlay 준비됨**: `/tmp/xkb-gegl-stage.tar`(6.2MB, **noble**): C.UTF-8(libc-bin 2.39) + `libpixbufloader_svg.so`(395KB) + `librsvg-2.so.2`. device slot `/data/local/tmp/xkb-gegl-stage.tar`(MainActivity wired). 다음 drain push → SIGABRT+setlocale 해소 기대.

## CP-3 microbench: stage 준비 + wiring(이 커밋)
- `/tmp/microbench-stage.tar`(rootfs `/usr/bin/microbench`, static musl). MainActivity overlay listOf에 `"microbench"` 추가 → loader가 `<rootfs>/usr/bin/microbench`로 guest-probe 실행. native baseline 확보(compute 4.06 / syscall 200.36 ns/op) → drain에서 `alr-microbench` ns_per_op 캡처 → `bench overhead` %.

## 다음 device 배치
xkb-gegl(noble GUI) + microbench-stage push + APK → gtk3 SIGABRT 해소 + CP-3 ns_per_op + glmark2 재시도(WS-2 glmark2-side match_score fix 후).
