# Device Evidence — batch drain(v127): CP-2 glmark2 한 단계 전진, gtk3 SIGABRT 진짜 원인(SVG), C.UTF-8 미해결, CP-3 native baseline

5세션 통합본(v127) + stage tar 3종(gpushim shim eglChooseConfig 수정 / xkb-gegl C.UTF-8 / microbench) 일괄 drain. Device SM-X236N / Mali-G615. 단일 게이트로 이 라운드 ws-2/3/4/5 9건 무충돌 merge(merge-tree CLEAN→host build+pytest→push).

## CP-2 glmark2: eglChooseConfig 통과 → "Failed to find suitable EGL config"
- shim eglChooseConfig count-query 수정(ws-2 8875488) 효과 확인: 이전 `"eglChooseConfig() didn't return any configs"`(stdout 136B) → 지금 `"Failed to find suitable EGL config"`(stdout 127B). **한 단계 전진.**
- 즉 glmark2가 eglChooseConfig로 config 개수는 받지만, eglGetConfigAttrib로 본 속성이 요구(RGBA/depth)와 안 맞아 reject.
- **WS-2 다음(DEVICE-REQ)**: eglChooseConfig가 glmark2 attrib_list(EGL_RED/GREEN/BLUE/ALPHA/DEPTH_SIZE, RENDERABLE_TYPE=ES2)에 맞는 config 반환 + eglGetConfigAttrib가 그 속성 실제 보고. ws-2 ff671f2(surface/context lifecycle)는 merge됨 — lifecycle ≠ config 속성이라 다음 drain은 suitable-config fix 이후.

## gtk3-widget-factory SIGABRT 진짜 원인 = libpixbufloader_svg.so 없음
- `Bail out! Gtk:ERROR ...gtkiconhelper.c:495: Failed to load .../image-missing.svg: .../loaders/libpixbufloader_svg.so: cannot open shared object file` → SVG 아이콘 로드 실패 → Gtk:ERROR assertion → abort(sig=6).
- C.UTF-8 `"Locale not supported"`는 Gtk-WARNING(부차, abort 원인 아님).
- **WS-4(DEVICE-REQ)**: librsvg + gdk-pixbuf svg loader(.so) overlay → gtk3-widget-factory SIGABRT 제거.

## C.UTF-8: overlay 적용됐으나 setlocale 실패
- rootfs `/usr/lib/locale/`: `C.UTF-8 → C.utf8` symlink + `C.utf8/` 확인(xkb-gegl overlay extracted=14 OK). guest `LC_ALL=LANG=C.UTF-8`.
- 그런데 `"Locale not supported by C library"` → setlocale 실패.
- **WS-4**: C.utf8 LC_* 포맷이 rootfs glibc와 불일치(Agent는 libc-bin 2.36 bookworm 사용)일 가능 — rootfs glibc 버전 대조 + 맞는 locale, 또는 LOCPATH/locale-archive 경로 확인.

## CP-3 native baseline (microbench static arm64, adb shell 직접)
- compute = **4.06 ns/op**(50M iters), syscall(getpid) = **200.36 ns/op**(1M iters).
- ALR getppid 218.34 ns/op(perf microbench)와 비교 시 ~9% — 단 getppid vs getpid·libc wrapper vs raw syscall이라 apples-to-apples 아님. 정밀 CP-3는 ALR loader가 동일 microbench를 guest로 실행(option i = WS-1 guest-probe wiring) 필요. PRoot baseline은 app-private rootfs exec(SELinux)로 보류.
- **WS-5**: native compute 4.06 / syscall 200.36을 `bench overhead`에 근사 입력. 정밀화는 WS-1 microbench guest-probe.

## 다음 device 배치(통합 세션 큐)
WS-2 suitable-config(eglChooseConfig 매칭 + eglGetConfigAttrib) + WS-4 pixbuf-svg loader + WS-4 locale-format. 셋 모이면 한 빌드로 재drain.
