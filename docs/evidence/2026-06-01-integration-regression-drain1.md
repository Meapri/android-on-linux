# Device Evidence — Integration Regression (5-session merged main, device drain #1)

통합 세션이 Device Lease Protocol(§9)에 따라 실행한 첫 device drain. APK `0.4.127`(main `450dc98`, WS-1/2/4/5 merged). `adb install -r -d` + force-stop cold start. Device SM-X236N (`R5KL20B6S3X`).

## 결과 — 5세션 통합 회귀 PASS
- **WS-4 overlay lib-downgrade guard 동작**: `foot-stage: overlay done (extracted=254 skipped=0)`, `gtk3demo-stage (extracted=14 skipped=0)`, `xkb-gegl-stage done`. **skipped=0** = frozen-SONAME 가드가 정상 overlay는 통과시키고 base-lib downgrade는 없음(harfbuzz 사건 재발 방지 device-확인).
- **WS-1 CPU 회귀 없음**: 일반 CLI 전부 `traps=0 rewrites=0`, `exec_ms` dynhello 19 / env 18 / id 19 / dash 17-18 / alr-png 21 / gimp-console --version 45 ms. (alr-input/interactive 6049/6057ms = 의도된 dispatch 대기.)
- **WS-3 display 회귀 없음**: `display: 1920x1200 @ 90000mHz density=213`(device-exact 90Hz).
- **GUI**: `gtk3-widget-factory` **rendered=true (frames 11→12)** — Mali 컴포지터 렌더. **`sig=11` 카운트 0**(SEGV 없음 — WS-1 XKB_CONFIG_ROOT + WS-4 xkb-data 유지). 잔여 `sig=6`(SIGABRT) + `gimp-3.0 exit=255 sig=0`(crash 아님) = locale/gegl, WS-4 (a) 영역. `foot` rendered=false(pty, frames 11→11) = WS-3/4.
- **WS-2 ops**: alr_gpu ring-hook/FBO/completeness ops가 통합 컴파일됨(glmark2 launch 미통합 → CP-2 GpuRingHook attach는 inert).

## 다음 drain
- **CP-2 GPU (glmark2 Mali)**: glmark2 launch(MainActivity) + gpushim/glmark2 overlay(extractOverlayTar) + guest_env `LD_LIBRARY_PATH`에 `/usr/lib/androlinux`(shim) 우선 통합 → 통합 build로 glmark2-es2 fps(software=false) 측정.
- **native(adb shell)+PRoot baseline**(WS-1): WS-5 CP-3 % overhead ratio unblock.
- **CP-1 display marker**: WS-5가 이 logcat(`display: …@90000mHz`)을 파싱(이미 device-verified).

(stamp v127 유지; 통합 세션만 bump. WS-5 bench가 본 evidence의 marker를 CP tracker에 반영.)
