# Device Evidence — CP-2 glmark2: software=false + GL context 달성(STENCIL fix), Score=0은 WS-2 draw/present

gpushim(ws-2 STENCIL_SIZE=0 반영 재빌드 256KB, .so 0755) push + cold start. Device SM-X236N / Mali-G615.

## glmark2 = config/context 통과 (STENCIL fix 효과)
- STENCIL_SIZE 8→0(ws-2 997d284)로 glmark2 2023.01 GLVisualConfig score가 -802→+230 → **suitable config 통과**(이전 "Failed to find suitable EGL config" 완전 해소).
- self-test 전부 **software renderer=false**(Mali-G615): `alr ahb/draw/ring/fbo/live/screen software renderer=false`.
- **`GL_RENDERER: ALR command-stream (host GPU passthrough)`** — eglChooseConfig → eglCreateWindowSurface → eglMakeCurrent → **GLES context 생성 + glGetString 응답**. config→surface→makecurrent→context 전 경로 device-달성.
- 단 **`glmark2 Score: 0`**(frames 13→13) — build scene의 GLES draw(glClear/draw/eglSwapBuffers)→ring→host 렌더/present가 frame을 advance 못 함.

## CP-2 상태
- **"software=false + GL context" = device-달성**(GPU 풀가속 경로 device 위에서 살아있음).
- "score>0" = WS-2 GLES draw/present 경로(shim glDraw*/glClear/eglSwapBuffers → ring → host AHB-FBO 렌더 → compositor present frame).
- **WS-2 다음(DEVICE-REQ)**: glmark2 build scene이 frame을 그려 Score>0. shim의 draw/swapbuffers가 ring으로 host executor에 전달되어 AHB에 렌더+present되는지. frames 13→13 = present 미발생 지점.

## 통합 세션 다음 device 배치
- PCGATE BPF 슬림화(f28e12b) device A/B: microbench syscall 224.36ns → ? (BPF 평가 ~24ns 절감 검증).
- ws-4 .so x-bit(5f09cf3): gtk3-widget-factory SVG SIGABRT 해소 + alr-png bmp/gif 재검증.
- WS-2: glmark2 draw/present(Score>0).
