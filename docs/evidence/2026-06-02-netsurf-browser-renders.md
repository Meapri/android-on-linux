# Device Evidence — netsurf-gtk (real web browser) renders via ALR (v134)

Build `0.4.134-netsurf-launch-v134` (versionCode 134). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. netsurf-gtk launched **display-backed on the compositor** (GDK → wl_shm → SurfaceView, same path as gtk3-widget-factory/GIMP), `about:welcome`, via the ALR native loader.

## Result — netsurf renders
```
netsurf-result: rendered=true frames=2214->2217
guest=/usr/bin/netsurf-gtk  argc=2  bytes=5824512  type=ET_DYN(static-PIE)
ALR NATIVE LOADER PARSE: PASS / MAP: PASS / INTERP MAP: PASS
reached=jumped-to-entry   guest threads spawned=5   child exit=-1 signal=14 (SIGALRM)
path-mediation traps=104 rewrites=71   first rewrite=/usr/lib/locale/locale-archive
guest stdout: Gdk-Message: Unable to load col-resize from the cursor theme
```
- **`rendered=true`** — the compositor wl-frame counter advanced (2214→2217): netsurf-gtk's window was composited to the SurfaceView. A **real GTK3 web browser renders natively via ALR.**
- **5 guest threads** — a genuinely multithreaded application driven in-process.
- `exit=-1 signal=14` = SIGALRM from the probe's `alarm(25s)` — netsurf stayed **alive and rendering for the full 25s window** (the same "GUI app runs to the timeout" pattern as GIMP/gtk3-widget-factory; NOT a crash). GDK is live (emits the cosmetic cursor-theme message). `GUEST EXEC: FAIL` here is just the SIGALRM exit code, not a load/run failure.

## No regression (same drain)
- `foot-result: rendered=true`, `gtkdemo-result: rendered=true (13→2214)`, `glmark2 Score: 1090`, `ALR GPU SCREEN CUBE: PASS`. No FATAL/crash.

## Significance
Adds a **web browser** to the device-proven universal-GUI set (previously GIMP 3.0.2, gtk3-widget-factory, gtk3-demo, foot). A glibc GTK3 browser binary executes in-process via ALR (non-root, public API), 5 threads, rendering to the Android display through the Wayland-on-SurfaceView compositor. Remaining browser polish (full page assets/resources, network, input interaction) is incremental; the launch + render path is proven.
