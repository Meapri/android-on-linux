# Device Evidence — v126: harfbuzz 회귀 수정 + device-exact 90Hz 디스플레이 (GUI 앱 SEGV는 rootfs xkb/GEGL 누락)

Device SM-X236N (mt6878, Mali-G615, Android 16), APK `0.4.126-gui-native-perf-v126`. Cold start(`am force-stop`→`am start`)로 검증.

## 진전 (device-verified)
- **harfbuzz 회귀 수정**: chromium-stage overlay가 harfbuzz 6.0.0을 깔아 rootfs의 8.3.0을 덮어 pango가 `hb_ot_color_has_paint`를 잃던 문제 → **chromium overlay 비활성화**(사용자 "크로미움 보류")로 8.3.0 유지. `gimp-console-3.0 --version` **exit=0**(직전 exit=127에서 회복), GTK symbol lookup error 사라짐. logcat에 `chromium-stage` 로그 없음(overlay 안 뜸 확인).
- **device-exact 해상도/주사율**: `display: 1920x1200 @ 90000mHz density=213`(landscape, 패널 90Hz 정확). MainActivity가 `Display.getRealSize`+`refreshRate`를 JNI로 전달, compositor가 wl_output mode+timerfd를 90Hz로 구동. (이전: 60000mHz 하드코딩 + SurfaceView-derived size.)
- **foot 링크 OK**: `foot --version` → `foot version: 1.13.1 +pgo +ime +graphemes` (ver=true). libfcft4+libutf8proc shim closure 정상.
- overlay 정상: `foot-stage: overlay done`, `gtk3demo-stage: overlay done`.

## 남은 이슈 (다음 작업)
- **GUI 앱 SIGSEGV**: `gtk3-widget-factory exit=-1 sig=11`, `gimp-3.0 exit=-1 sig=11`, foot 터미널 frames 미증가(12→12). harfbuzz를 고치니 GUI 초기화 단계까지 도달했고 거기서 crash. 단서(gimp stdout):
  - **xkbcommon ERROR [XKB-338]**: `Couldn't find file "rules/evdev"` in `/usr/share/X11/xkb` → keymap 룩업 실패. wayland 키보드 keymap 셋업 시 NULL keymap → deref SEGV 의심.
  - **GEGL 모듈 누락**: `/usr/lib/aarch64-linux-gnu/gegl-0.4/*.so cannot open`(npd/png-save/jpg-load/text/… 다수) — gimp 기능 일부(경고성일 수 있으나 rootfs 불완전 신호).
  - 둘 다 **rootfs 데이터/모듈 누락**(v126 변경과 무관, 기존 이슈가 harfbuzz 수정으로 드러남). → WS-4(rootfs: xkb rules `xkb-data`/`xkeyboard-config`, gegl 모듈) + WS-3(컴포지터 keymap 전달) 영역.

## 분류 (5-세션 플랜 매핑)
- v126 = CP-1(GUI 기반 + 디스플레이)의 일부: harfbuzz 정합 + 90Hz 달성. GUI 렌더 완성은 xkb/GEGL rootfs 보강 후. 플랜: `docs/research/orchestration-5session-plan.md`.
- 빌드 함정 기록: 이번 세션 빌드들이 JAVA_HOME 미설정으로 stale APK를 남겨 v124가 계속 돌았음(메모리: build-needs-java-home). JAVA_HOME=openjdk@17 명시 후 v126 정상 빌드/install 확인(versionCode=126).
