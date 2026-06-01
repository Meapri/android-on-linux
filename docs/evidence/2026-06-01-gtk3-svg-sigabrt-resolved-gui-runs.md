# Device Evidence — gtk3-widget-factory SVG SIGABRT 해소(GDK_PIXBUF + .so x-bit), GUI 정상 실행

interpose-stage(PCGATE 슬림 BPF) + APK(GDK_PIXBUF env + ws-4 .so x-bit) cold start. Device SM-X236N / Mali-G615.

## gtk3-widget-factory: SVG SIGABRT(sig=6) 완전 해소
- 이전: `exit=-1 sig=6`(SIGABRT) — `libpixbufloader_svg.so cannot open` → `Gtk:ERROR Bail out`(즉시 crash).
- 지금: `exit=-1 sig=14`(SIGALRM, exec_ms=25031) — Bail out/svg cannot open 없음. probe alarm(25s) timeout = GUI가 25초간 살아서 안 끝남 = 정상 실행(crash 아님).
- `traps=135 rewrites=52` — path mediation 작동(first_rewrite=/usr/lib/locale/locale-archive → rootfs).
- 2겹 fix: (1) WS-4 .so x-bit(svg loader 0700 → ALR file-backed PROT_EXEC dlopen 가능) + (2) WS-1 GDK_PIXBUF_MODULE_FILE/MODULEDIR rootfs-absolute(gdk-pixbuf 절대경로 path-mediation 우회, XKB_CONFIG_ROOT 패턴).
- 부수: alr-png bmp/gif/png/jpeg 전부 decode OK. gtkdemo frames 12→2213(렌더 루프 정상), foot rendered=true.

## CP-3 재확인 + PCGATE 슬림(효과 ~0)
- compute 4.06(=native, 0%), syscall 223.37(슬림 BPF) vs 224.36(구) = ~1ns만 감소.
- syscall ~12%(24ns)는 BPF instruction 평가가 아니라 seccomp 디스패치 고정 비용 → BPF 슬림화로 못 줄임. seccomp 켜는 한 syscall 0% 원천 불가. raw-svc path-mediation 백스톱이라 전역 off 위험 — per-app env(ALR_PCGATE=0) 옵션만.

## 남은
- CP-2 glmark2 Score>0: WS-2 GLES draw→ring→host AHB render→present(software=false + GL context는 device-달성).
- locale 경로 미세조정(LOCPATH) = WS-1; getpwuid passwd(uid 10326) = WS-4 rootfs nss.
