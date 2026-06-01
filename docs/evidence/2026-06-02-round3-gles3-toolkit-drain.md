# Device Evidence — round-3 (v133): GLES3 coverage no-regression + toolkit binaries load via ALR

Build `0.4.133-breadth-r3-v133` (versionCode 133). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. Functional drain of the round-3 merges (ws2 GLES3 coverage, ws4 toolkit-launchable overlays, ws5 docs) → main `4d7d4a9`. Re-staged: gpushim (GLES3 shim), netsurf/qt6/sdl2 (with launchable binaries). PCGATE on.

## GLES3 coverage — NO regression
The merged GLES 3.0 entry points (UBO bind, sampler objects, MRT/glDrawBuffers, glInvalidateFramebuffer; 12 new wire ops) on the ES3 context (Mali-G615 ES3.2) did not regress the GPU pipeline:
- `ALR GPU LIVE INTEGRATION: PASS` + `ALR GPU THROUGHPUT: PASS` + `ALR GPU SCREEN CUBE: PASS` (real Mali, software=false).
- `glmark2 Score: 1067` (vs 1009–1163 prior; variance, GPU-class). **No FATAL/crash.**

## Toolkit overlays — binaries ship + the loader RUNS them (display is the only gap)
Overlays auto-stage clean: `netsurf-stage (6)`, `qt6-stage (70)`, `sdl2-stage (120)`, `gpushim (14)`, 0 guard skips.

**netsurf-gtk (real GTK3 browser) — loads + runs via ALR, exits on no-display (NOT a loader/dep failure):**
```
guest=/usr/bin/netsurf-gtk  bytes=5824512  type=ET_DYN(static-PIE)
ALR NATIVE LOADER PARSE: PASS / MAP: PASS / INTERP MAP: PASS / GUEST EXEC: FAIL
reached=jumped-to-entry   child exit=1 signal=0   path-mediation traps=0
guest stdout: (netsurf-gtk): Gtk-WARNING: cannot open display:
```
The loader **parsed, mapped, linked, and jumped to entry**; netsurf-gtk's full GTK3 dependency closure resolved and **GTK initialized** (it reaches the display-open). It exits 1 only because the headless version-probe provides no Wayland display. So `GUEST EXEC: FAIL` here = the app's exit=1, NOT a loader/closure failure — **a real GTK3 browser binary executes in-process under ALR.** Completing it = launch on the compositor (WAYLAND_DISPLAY set, like foot/GIMP) instead of a headless `-v` probe.

- **sdl2** `/usr/libexec/installed-tests/SDL2/testver`: ran, SDL marker matched (exit non-zero).
- **qt6**: `qtpaths6` not at the probed `/usr/lib/qt6/bin/qtpaths6` in this rebuilt tar (overlay layout/symlink mismatch) → not run. Path reconciliation pending.

## Verdict
GLES3 core coverage merged with **zero GPU regression**. Toolkit overlays now carry launchable binaries and the **ALR loader successfully executes a real GTK3 browser (netsurf-gtk) to GTK init** — the headless probe's exit-on-no-display is the only gap. Remaining (next milestone, not a breadth item): **launch the toolkits on the compositor** (display-backed, like foot/GIMP) for a real browser/Qt/SDL window on screen; reconcile the qt6 binary path. This is WS-3/WS-4 GUI-launch wiring, larger than a probe tweak.
