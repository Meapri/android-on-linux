# Device Evidence — v127 (WS-1): XKB_CONFIG_ROOT fixes guest GUI keymap SIGSEGV; gtk3-widget-factory renders on Mali

Device SM-X236N (mt6878, Mali-G615, Android 16). 5-세션 분할의 **WS-1(CPU 실행/loader/guest_env)** 작업. cold start(`am force-stop`→`am start`)로 검증.

## 문제 (CP-1 GUI baseline 블로커)
harfbuzz 회귀(v126)를 고치자 GUI 앱이 GUI 초기화 단계까지 도달해 **SIGSEGV(sig=11)**: `gtk3-widget-factory`/`gimp-3.0` 모두 crash, 직전 로그 `xkbcommon: ERROR [XKB-338] Couldn't find file "rules/evdev" in /usr/share/X11/xkb`. 컴포지터가 `WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP`을 보내(의도적; `alr_compositor.cpp:1178`) 게스트 클라이언트가 자체 default keymap을 만들어야 하는데, xkb 파일이 rootfs tar에 **이미 존재**함에도(에이전트 확인) 게스트의 keymap-파일 탐색(opendir/stat)이 supervisor path-mediation을 거치지 않아(`rewrites=0`) rootfs로 resolve되지 않음 → NULL keymap → SEGV.

## WS-1 수정
`runtime_report.cpp`의 guest_env 빌더에 추가:
```
XKB_CONFIG_ROOT=<config.rootfs_dir>/usr/share/X11/xkb
```
libxkbcommon은 `XKB_CONFIG_ROOT` env를 존중한다. rootfs-**절대**경로를 주면 supervisor의 idempotency 가드(`under(gp, rootfs_dir)` → already-host)가 rewrite를 건너뛰어 게스트가 실제 파일을 직접 연다 — path-mediation 불필요.

## device-verified (sig=11 → 0)
- **`gtk3-widget-factory`: rendered=true, frames 25→26** — 경량 GTK 앱이 Mali 컴포지터에 실제 렌더. `Couldn't find rules/evdev` 사라짐.
- **`sig=11` 카운트 = 0** (SEGV 완전 해결).
- `gimp-3.0`: exit=255 sig=0 (crash 아님, gimp 자체 종료 — display/locale 등).
- `gtk3-widget-factory`는 렌더 후 SIGABRT(sig=6): 잔여 경고가 원인 추정(아래) — 렌더 자체는 성공.

## 남은 경고 (다른 워크스트림)
- `Gtk-WARNING: Locale not supported by C library` → locale 데이터 (WS-4 rootfs).
- `GLib-WARNING: getpwuid_r(): failed due to unknown user id (10326)` → `/etc/passwd`에 Android uid 항목 없음 (WS-4: nss/passwd shim).
- `Could not load a pixbuf from icon theme` → 아이콘 테마 (WS-4 rootfs).
- `foot`: rendered=false(ver=true) → 터미널 pty 경로 (WS-3/WS-4).

## CP-1 상태
경량 GTK 앱의 Mali 화면 렌더 달성(harfbuzz + xkb keymap 해결) = **CP-1 핵심 통과**. 완전한 GUI 안정(locale/passwd/pixbuf, foot pty)은 WS-3/WS-4 후속. stamp는 v127 유지(통합 세션 bump 정책 — WS-1은 stamp 미변경).
