# Device Evidence — 6-WS breadth parallel fan-out, functional drain (v132)

Build `0.4.132-breadth-fanout-v132` (versionCode 132). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. Functional drain (NOT a benchmark) of the 6 parallel breadth merges → main `6d27a85`. Rebuilt stage tars re-staged: interpose (ws1), gpushim (ws2 GLES coverage), babl-gegl (ws4), qt6/xwayland (ws4 toolkit). PCGATE on, interpose on.

## Overlay staging — all clean (wire-toolkit auto-stage additions work)
```
gpushim-stage:   overlay done (extracted=14 skipped=0)
qt6-stage:       overlay done (extracted=48 skipped=0)
xwayland-stage:  overlay done (extracted=16 skipped=0)
babl-gegl-stage: overlay done (extracted=67 skipped=0)
interpose-stage: overlay done (extracted=8  skipped=0)
```
qt6/xwayland/babl-gegl are newly in MainActivity's auto-stage list (WS-4 wire-toolkit) and extracted with 0 guard skips. babl-gegl = 67 GIMP op modules (.so 0755 → dlopen-able; root cause was non-exec 0644, not absence).

## WS-1 CP-6 M2 (opendir trampoline + credential cache) — traps reduced
per-guest `path-mediation traps`:
| guest | traps | rewrites |
|-------|-------|----------|
| dpkg-query / apt-get / Xwayland | 0 | 0 |
| foot | 0 (and 15 on a second invocation) | 0 / 11 |
| **gtk3-widget-factory** | **99** | **50** |
| glmark2-es2-wayland | 31 | 21 |

- gtk3-widget-factory **135 → 99** (vs the pre-ws1 `2026-06-01-gtk3-svg-sigabrt-resolved` evidence's traps=135/rewrites=52); glmark2 ~40 → 31; CLI/dpkg/apt/Xwayland/foot at **0**. Directionally consistent with the opendir-trampoline (directory opens no longer fall to the supervisor) + credential getter cache. (Not an isolated same-build A/B — intervening changes exist — but the trend is down and the mechanism is targeted.)

## WS-2 GLES coverage + WS-3 compositor — no regression
- `ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU THROUGHPUT: PASS` + `ALR GPU SCREEN CUBE: PASS` (real Mali, software=false) — the 19 newly-wired GLES2 state setters (ws2) + the decode changes did not regress the GPU pipeline.
- `glmark2 Score: 1009` (vs 1009–1163 prior; run-to-run/thermal, GPU-class) — no regression.
- WS-3 pointer leave+frame symmetry fix compiled in (arm64); GUI runs, no crash.
- getpwuid: no `unknown user id` warning (WS-4 fix holds). **No FATAL/SIGSEGV anywhere.**

## WS-4 toolkit launch — overlays stage, CLI smoke pending
```
toolkit-netsurf: missing (no /usr/bin/netsurf[-gtk3|-gtk] present)
toolkit-qt6:     missing (no qtdiag6/qmake6 present)
toolkit-sdl2:    missing (no sdl2-config present)
```
The qt6/netsurf/sdl2 overlays stage their **runtime libraries**, but do not contain the specific CLI binaries the smoke probe looked for. Honest partial: libs present + auto-staged, but a launchable demo/CLI binary is not in the current overlays → GUI launch of these toolkits is still **pending** (needs a demo binary in the overlay, or the correct in-overlay path). Not a regression.

## Verdict
6-WS breadth fan-out integrated + functionally device-verified in one drain: overlays stage (qt6/xwayland/babl-gegl), GPU pipeline + GLES coverage no-regression (LIVE/THROUGHPUT/CUBE PASS, glmark2 1009), WS-1 traps reduced (gtk3-widget-factory 135→99), WS-4 babl-gegl GIMP modules dlopen-able. Remaining (pending): qt6/netsurf/sdl2 **launchable** binaries in overlays, GIMP babl/gegl filter exercise, GTK menu prelight manual check (ws3), real `apt install`.
