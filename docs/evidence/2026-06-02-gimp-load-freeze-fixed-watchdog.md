# Device Evidence — GIMP "로딩중 멈춤" root-caused + fixed (v146): the 25s verification watchdog

Build `0.4.146-r12-v146`. Device SM-X236N / Mali-G615 MC2 / Android 16. Cold-start drain.

## Symptom
On the real device GIMP repeatedly froze on the loading/splash screen and never reached the editing window.

## Root cause — the 25s loader watchdog, NOT the plug-in/exec wall
The loader sets a per-guest watchdog: `::alarm(dynamic ? (is_chromium ? 120 : 25) : 5)`. This 25s window is the *verification harness* cadence — it cycles the intermediate GUI probe apps quickly (each renders a frame, then SIGALRM moves to the next). The drain shows the sibling GUI apps hitting it exactly:
```
gtk3-widget-factory  exit=-1 sig=14  exec_ms=25037   (SIGALRM at 25.0s)
netsurf-gtk          exit=-1 sig=14  exec_ms=25051   (SIGALRM at 25.0s)
```
GIMP's cold load (fontconfig cache + GEGL/babl init + plug-in scan) takes **longer than 25s**, so SIGALRM killed it MID-LOAD — it looked frozen on the splash. (The plug-in fork+exec / exec-re-entry wall is NOT the blocker: GIMP falls back gracefully when a plug-in can't run and still reaches the main window.) The 25s was a deliberate reduction from the original interactive 1800s ("Was 1800 for a live interactive GIMP session — parameterize per-launch") that was never restored for GIMP.

## Fix (loader run-path, part of the ongoing work — not a band-aid)
GIMP (the Phase-6 interactive headline, which runs LAST so a long lifetime blocks nothing after it) now gets a real interactive window:
```cpp
const bool is_gimp = host_path.find("gimp") != std::string::npos;
const unsigned alarm_sec = dynamic ? (is_gimp ? 1800u : (is_chromium ? 120u : 25u)) : 5u;
```

## Device result — GIMP now LOADS to the editing window ✓
```
xdg_toplevel.set_title "GIMP Startup"
xdg_toplevel.set_title "Welcome to GIMP 3.0.2"
xdg_toplevel.set_title "[Untitled]-1.0 (RGB color 8-bit non-linear integer, GIMP built-in sRGB, 1 layer) 1920x1080 – GIMP"
xdg_toplevel.set_title "*[Untitled]-1.0 (... 1 layer) 1920x1080 – GIMP"
```
GIMP advances splash → welcome → **main editing window (Untitled 1920×1080 canvas)** → `*` (unsaved edit) — i.e. fully loaded + interactive, restoring the v111 "GIMP usable by touch" state. No SIGALRM-at-25s for GIMP.

## Note (the deeper, still-ongoing piece)
Truly *unlimited* interactive GIMP (no watchdog at all) belongs to the proper user-launch run-path (the product-UX launcher, in progress) — the verification probe's 1800s is the interim. GIMP's own plug-ins fully working (not graceful-fallback) is the G1 exec-re-entry track (re-mapped-guest execution, in progress).
