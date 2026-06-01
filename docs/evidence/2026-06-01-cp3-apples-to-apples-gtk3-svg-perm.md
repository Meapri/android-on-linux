# Device Evidence — CP-3 ✅ apples-to-apples; gtk3 SVG=overlay .so 권한(WS-4); locale; glmark2(WS-2)

xkb-gegl(noble GUI) + microbench-stage + 새 APK(CP-3 wiring + ws-3) cold start. Device SM-X236N / Mali-G615.

## CP-3 CPU 오버헤드 = apples-to-apples 완성 ✅
microbench(static musl, native baseline과 **동일 바이너리**)를 ALR loader로 실행(MainActivity guest-probe → runtime_report `alr-microbench` logcat):
- **compute: ALR 4.06 ns/op = native 4.06 ns/op → overhead 0%** (PCGATE in-process, syscall 없어 ptrace/seccomp 미발동). **gated <5% PASS.**
- **syscall(getpid): ALR 224.36 vs native 200.36 → ~12%** (raw syscall storm, reported).
- 결론: **일반 연산/CLI 워크로드는 native급(0% overhead)**, syscall-storm만 ~12%. WS-5 근사 ~9%를 same-binary 정밀치로 대체. `python -m bench overhead` 입력.

## gtk3-widget-factory SIGABRT = overlay .so 권한(x 없음)
- `gtk3-widget-factory exit=-1 sig=6`, "libpixbufloader_svg.so cannot open". 파일은 rootfs에 존재(395496B)인데 dlopen 실패.
- 원인 확정(코드): **`build_gui_overlay.py:89 ti.mode = m.mode or 0o644`** → deb의 loader .so가 0644(x 없음)로 tar됨 → **`RootfsInstaller.extractOverlayTar:163`(tar x 비트 있을 때만 setExecutable)이 x 안 줌** → rootfs .so=`0600`(rw-------). dlopen이 x 없는 .so 거부(gpushim libGLESv2는 `0700`=x → dlopen OK 대비). chmod 0755 후에도 cold start가 overlay 재적용해 0600 복귀.
- **WS-4 fix(DEVICE-REQ)**: build_gui_overlay가 .so(loader/lib)를 `0o755`로 패키징(또는 extractOverlayTar이 `*.so`에 x 부여). base pixbuf loader(bmp/gif 등)도 같은 0600이라 동일 영향(alr-png bmp/gif decode FAIL).

## C.UTF-8 locale = 여전 "Locale not supported"
- rootfs `/usr/lib/locale/C.utf8/LC_CTYPE`(360460B, noble) + `C.UTF-8→C.utf8` symlink 존재인데 setlocale 실패.
- .so 아니라 x 권한 무관. 경로/`LOCPATH`/locale-archive 또는 path-mediation. **WS-4 추가 디버그.**

## glmark2 = shim 정상, glmark2-side(변동 없음)
- shim eglChooseConfig/eglGetConfigAttrib 완벽한데 여전 "Failed to find suitable EGL config". **WS-2 GLStateEGL match_score/native_visual_id binary 디버그.**

## 다음 device 배치
WS-4 overlay .so 0755 + locale 경로 fix; WS-2 glmark2 match_score. 재drain → gtk3 SIGABRT 해소 + glmark2 Score.
