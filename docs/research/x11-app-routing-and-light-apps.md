# X11-app auto-routing via Xwayland + new light apps (TASK-A / TASK-C)

**Status:** host-side, build-green (`compileDebugKotlin` + host pytest). Device-verify
pending (device owned by concurrent agents). Tooling: `tools/elf_needed.py` (DT_NEEDED),
`tools/app_closure_audit.py` (install verdict), `tools/build_xwayland_overlay.py` (the
Xwayland overlay). Code: `NativeAppSession.XwaylandLaunch`, `CatalogApp.needsXwayland`,
`BundledCatalog`. Companion: `docs/research/light-app-breadth-additions.md`.

## 1. The launch-routing gap (TASK-A)

xcalc (X11) was device-proven to render via a ROOTFUL Xwayland, but that path was armed by
a `/data/local/tmp/.alr-xwayland` marker in `MainActivity` — a debug hook, not the product
launch path. xzgv + nsxiv FAIL from the launcher because they are **X11-only** (link
`libX11`/GTK2-x11, NOT `libwayland-client`) and the product path sends every app to the
native Wayland compositor with no `DISPLAY` → "cannot open display".

DT_NEEDED evidence (`tools/elf_needed.needed_of` on each `.deb`'s EXEC binary):

| app | EXEC binary DT_NEEDED (relevant) | X11-only? |
|-----|----------------------------------|:---------:|
| xzgv | `libgtk-x11-2.0.so.0`, `libgdk-x11-2.0.so.0`, **`libX11.so.6`**, _(no wayland)_ | **yes** |
| xli  | **`libX11.so.6`**, `libjpeg`, `libpng16`, _(no wayland)_ | **yes** |
| nsxiv| **`libX11.so.6`**, _(no wayland)_ | **yes** |
| feh  | **`libX11.so.6`**, _(no wayland)_ | **yes** |
| qiv  | `libgdk-x11-2.0.so.0`, **`libX11.so.6`**, _(no wayland)_ | **yes** |
| xpdf | `libXm`/`libXt`/`libX11` (Motif/Xt closure), _(no wayland)_ | **yes** |
| qpdfview | Qt6 (xcb platform plugin at runtime; **no `qt6-wayland`** in closure) | yes (via Xwayland) |
| mate-calc | `libgtk-3.so.0` _(no libX11, no direct wayland — GTK3 picks wayland at runtime)_ | no |
| geany | `libgeany.so.0` → GTK3 _(no direct libX11)_ | no |

> **UPDATE (general maintscript-shim):** nsxiv/feh/qiv/xpdf/qpdfview are now RE-ADDED to the
> catalog (all `needsXwayland=true`). See §2′ below + `docs/research/maintscript-shim-
> generalization.md`. The routing helper is unchanged; only their INSTALL became reachable.

A GTK3 app (gpicview/viewnior/mate-calc/geany) does NOT link `libwayland-client` directly
either, but GDK **dlopens** its wayland backend (present in the base) and picks it via
`GDK_BACKEND`/`XDG_SESSION_TYPE=wayland` — so it binds the compositor with no Xwayland. An
X11-only app has no such backend: it speaks raw Xlib and MUST have an X server.

### The fix: a catalog flag + a self-contained routing helper

1. **`CatalogApp.needsXwayland: Boolean = false`** (`AppModels.kt`) — declares an app
   X11-only. Default false ⇒ every existing entry is byte-identical.

2. **`NativeAppSession.XwaylandLaunch`** — a new helper object (sibling to
   `GnomePlatformShim`, in its OWN region of the file so the concurrent `ALR_REEXEC_INPROC`
   edit merges cleanly). It is the runtime counterpart of the flag and replicates EXACTLY
   the device-proven `MainActivity` Xwayland sequence:
   - `needsX11(appId, protocol, entryPath)` → true iff the appId is in the X11-only set
     (kept in lock-step with the catalog's `needsXwayland=true` entries) OR
     `protocol == SurfaceProtocol.X11` (the existing `LaunchRequest.protocol` enum).
   - `ensureUp(...)` → (a) pre-creates `<rootfs>/tmp` + `<rootfs>/tmp/.X11-unix` at sticky
     `01777` (the X server's `MkdirIfNeeded` ownership mode; `Os.chmod` because Java can't
     set the sticky bit) — without it the server aborts at `/tmp/.X0-lock` create EPERM;
     (b) starts ROOTFUL `Xwayland :0 -shm -geometry WxH` on its OWN thread (a persistent wl
     client; `-shm` = wl_shm backend so the software compositor presents it, no `-rootless`
     ⇒ no XWM); (c) waits (bounded) for the `<rootfs>/tmp/.X11-unix/X0` socket with
     `exists()` (a bound AF_UNIX socket is a special file — `isFile()` is false for it).
     Process-global, started at most once across sessions (one compositor).
   - `envFor()` → `DISPLAY=:0`, injected into the guest env before the blocking loader call.

   Gated entirely on `needsX11`: a Wayland app never enters socket prep, never starts
   Xwayland, never gets `DISPLAY` — its launch is unchanged.

3. **`OVERLAY_NAMES`** now includes `xwayland` (ships `/usr/bin/Xwayland`) + `qt6-gui`,
   staged best-effort (no-op when the tar is absent).

`ALR_REEXEC_INPROC=1` (already set for every launch) is what lets Xwayland's
`fork+exec(xkbcomp)` keymap compile re-enter the in-process loader — device-proven
necessary, and the keystone that resolved the wave-3/wave-4 xkbcomp blocker (project
memory).

## 2. Apps marked / added

| appId | install verdict (`--live`) | delta | display | needsXwayland | in catalog? |
|-------|----------------------------|------:|---------|:-------------:|:-----------:|
| **xzgv** | LIKELY-PASS | 16 | X11-only → Xwayland | **true** | KEPT (marked) |
| **xli** | LIKELY-PASS | 4 | X11-only → Xwayland | **true** | ADDED |
| **mate-calc** | LIKELY-PASS | 56 | GTK3 Wayland | false | ADDED |
| **geany** | LIKELY-PASS | 54 | GTK3 Wayland | false | ADDED |
| **nsxiv** | LIKELY-PASS (was HEAVY; shim-neutralized) | 33 | X11-only → Xwayland | **true** | **RE-ADDED** |
| **feh** | LIKELY-PASS (was HEAVY; shim-neutralized) | 37 | X11-only → Xwayland | **true** | **RE-ADDED** |
| **qiv** | LIKELY-PASS (was HEAVY; shim-neutralized) | 40 | X11-only → Xwayland | **true** | **RE-ADDED** |
| **xpdf** | LIKELY-PASS (was HEAVY; shim-neutralized) | 18 | X11 (Motif) → Xwayland | **true** | **RE-ADDED** |
| **qpdfview** | LIKELY-PASS (was HEAVY; shim-neutralized) | 77 | Qt6-xcb → Xwayland | **true** | **ADDED** (apt-Qt-GUI) |

### 2′. nsxiv/feh/qiv/xpdf/qpdfview RE-ADDED (the general-shim unlock — supersedes the drop)

The earlier pass DROPPED nsxiv (and held back feh/qiv/xpdf) because their apt closure audited
HEAVY — delta drags `libpaper1`(exit 2) + `x11-common`(exit 127) + `xfonts-*`, the SAME
debconf/init-script postinsts that device-proved-FAIL for `xpdf` — and the neutralizer was
gated to gnome-platform pkgs. **That gate is now removed:** the `maintscript-shim` overlay is
staged for EVERY install (`AptInstaller.MAINTSCRIPT_SHIM_OVERLAY`), so those postinsts exit 0
and the X11-image-viewer class audits LIKELY-PASS (`tools/app_closure_audit.py --live`). All
are re-added with `needsXwayland=true`; `qpdfview` is the apt-Qt-GUI representative (only
blocker `x11-common` via libsm6; no `qt6-wayland` plugin → Qt's xcb plugin connects to the
rootful Xwayland). Routing wiring SSOT: `BundledCatalog.needsXwayland(appId)` drives the UI
launch-request builders to set `protocol=X11` (so no edit to the chromium-owned
`NativeAppSession.X11_ONLY_APP_IDS` is needed). Full model:
`docs/research/maintscript-shim-generalization.md`.

### Why xli (TASK-C)

xli is the smallest closure in the whole catalog: a 16-package closure, **4-package install
delta** (`libjpeg8`/`libpng16`/`libx11-6`/`libxext6` minus what the base provides), 0
cascade triggers. It is a classic Xlib image viewer (JPEG/PNG/GIF/TIFF). It ships NO
`.desktop` (terminal-call style, like `htop`/`sakura`), so `DesktopEntryScanner` won't
auto-reconcile a tile; it installs+launches via the catalog's explicit appId→apt map. Takes
an image path arg (`xli test.png`).

## 3. Device-verify checklist

Prereq: the device is free. Overlays pushed: `xwayland-stage.tar` (`/usr/bin/Xwayland`),
plus per-app the install is online apt.

### A. xzgv / xli (X11-only → Xwayland routing) — the TASK-A proof

```bash
# Install (online apt; both are LIKELY-PASS, configure should exit 0):
#   in-app catalog → install "xzgv"   (and "xli")
# OR drive AptInstaller for the pkg.

# Launch from the launcher tile (xzgv) / explicit appId map (xli).
```

**Expected (PASS):**
- `logcat alr_runtime`: `[xzgv] X11 routing: Xwayland :0 ready=true (DISPLAY=:0)`.
- `logcat alr_runtime`: `xwayland: prepped /tmp(1777)=true /tmp/.X11-unix(1777)=true` then
  `xwayland: X0 socket=true (...)`.
- The xzgv thumbnail/viewer window (xli image window) renders ON the SurfaceView via
  Xwayland → wl_shm — exactly like the device-proven xcalc. NO "cannot open display".
- A Wayland app (e.g. galculator) launched in the same session shows NO X11 routing log
  (no Xwayland process, no DISPLAY) — the gate holds.

### B. mate-calc / geany (GTK3 Wayland, TASK-C) — no Xwayland

```bash
#   in-app catalog → install "mate-calc"  (and "geany")
```

**Expected (PASS):**
- Install→configure exit 0 (galculator-class delta; 0 cascade triggers).
- NO X11-routing log (these are `needsXwayland=false`): they bind the compositor directly
  via the GDK Wayland backend (`wl_shm` → SurfaceView), like galculator/l3afpad.
- mate-calc tile auto-reconciles (ships `mate-calc.desktop`); geany too (`geany.desktop`).

### C. Regression guard

- An existing Wayland app (galculator, gpicview, GIMP) launches byte-identically — the
  `needsXwayland` default-false path adds nothing to their launch.

## 4. Honest limits

- All device-verify items are **pending** (device owned by concurrent agents). This change
  is host-analyzable: the routing helper replicates the proven `MainActivity` Xwayland
  sequence (asserted source-identical in the host test), the install verdicts are from the
  live noble index, and the DT_NEEDED X11-only classification is from the real `.deb`
  binaries.
- The `XwaylandLaunch` helper does NOT itself verify a frame rendered (the session has no
  frame-counter accessor); the device checklist's frame/window check is the render gate.
- ✅ RESOLVED: the `x11-common`/`libpaper1` neutralizer has been GENERALIZED beyond
  gnome-platform (`AptInstaller.MAINTSCRIPT_SHIM_OVERLAY`, staged for every install), so
  nsxiv/feh/qiv/xpdf/qpdfview are RE-ADDED + audit LIKELY-PASS (host). Their install→render is
  the device-verify checklist in `docs/research/maintscript-shim-generalization.md` §6.
