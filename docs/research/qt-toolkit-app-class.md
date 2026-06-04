# Qt toolkit app-class — apt-GUI is closure-blocked; the reachable path is an overlay (TASK-B)

**Status:** host-side, build-green (overlay builds + validates). Device-verify pending
(device owned by concurrent agents at time of writing). Tooling:
`tools/app_closure_audit.py`, `tools/deb_closure.py` (`build_minimal_overlay`),
`tools/build_toolkit_overlays.py` (the new `qt6-gui` recipe), `tools/elf_needed.py`.
Companion: `docs/research/light-app-breadth-additions.md`, the WS-4 toolkit matrix.

## 1. The ask vs. the finding

TASK-B asked for a genuinely-light Qt app added to `BundledCatalog` (apt-installable),
e.g. a Qt calculator/utility. The closure audit shows **no apt Qt-GUI app is installable
on the ALR base today** — and the reason is structural, not a missing leaf.

### Every Qt-GUI package drags `x11-common` (the device-proven exit-127 postinst)

`app_closure_audit.py --live` against the real noble `main+universe` index, with the base
installed-set reconstructed from `tiny-rootfs.tar`:

| package | verdict | closure | delta | cascade triggers |
|---------|---------|--------:|------:|------------------|
| `qt6-wayland` (the toolkit lib itself) | HEAVY | 152 | 77 | `x11-common` |
| `qml6-module-qtquick` | HEAVY | 145 | 72 | `x11-common` |
| `keepassxc` | HEAVY | 138 | 79 | `x11-common` |
| `qjackctl` | HEAVY | 179 | 101 | `x11-common` |
| `qlipper` | HEAVY | 123 | 64 | `x11-common` |
| `vym` | HEAVY | 142 | 72 | `x11-common` |
| `nomacs` | HEAVY | 165 | 78 | `x11-common` |
| `kcalc` | HEAVY | 240 | 162 | `perl`, `perl-base`, `x11-common` |
| `speedcrunch` | HEAVY | 127 | 68 | `x11-common` |
| `qalculate-qt` | MISSING | — | — | (not in noble main+universe) |

The dependency path is fixed and unavoidable for any windowed Qt app:

```
<any Qt-GUI app>  →  libqt6gui6t64  →  libsm6 / libice6  →  x11-common
```

`libqt6gui6t64` (the Qt GUI core every widget/quick app links) hard-`Depends` on
`libsm6`+`libice6` (X11 session-management), which `Depends` on `x11-common`. There is no
Qt-GUI package that avoids it. (The LIKELY-PASS `q*` packages — `qpdf`, `qrencode`,
`qprint` — are **not** Qt; `libqpdf` is a plain C++ library.)

`x11-common`'s `postinst` is the **device-proven exit-127 cascade** (project memory; also
encoded in `app_closure_audit.CASCADE_TRIGGERS`): it sources the debconf confmodule and
calls `update-rc.d`/`invoke-rc.d`, none of which the base ships, so `dpkg --configure`
aborts. The install-configure neutralizer that already fixes exactly this
(`maintscript-shim` overlay: a no-op confmodule + `policy-rc.d=101` + `update-rc.d`/`ucf`
stubs) is gated to **gnome-platform packages** in `AptInstaller.GNOME_PLATFORM_PKGS`
(other-owned; out of scope for this task). So an apt Qt-GUI install is blocked.

> This is the SAME blocker as `xpdf`/`nsxiv` (see light-app-breadth doc): `x11-common` +
> `libpaper1` debconf/init-script postinsts. It is an INSTALL problem, independent of
> display.

## 2. The reachable Qt path: an overlay (apt never runs → no postinst)

The toolkit-overlay mechanism (`build_minimal_overlay` → `build_toolkit_overlays.py`)
ships binaries **directly via a stage-tar** and computes the DT_NEEDED-minimal library
set the base lacks. Because nothing is `apt install`-ed, **`x11-common`'s postinst never
fires** — and `x11-common` ships no runtime `.so` anyway (the base already has
`libICE.so.6`/`libSM.so.6`/`libX11.so.6` as real files). So a Qt-GUI binary delivered by
overlay runs without ever touching the apt-configure cascade.

### The `qt6-gui` recipe (new in `tools/build_toolkit_overlays.py`)

| field | value |
|-------|-------|
| binary | `/usr/lib/qt6/bin/qmleasing` (from `qt6-declarative-dev-tools`) |
| what it is | a **real Qt6 Quick GUI** app — an interactive easing-curve editor window |
| links | `libQt6Quick` / `libQt6Widgets` / `libQt6Gui` / `libQt6Qml` / `libQt6Core` |
| platform plugin | `qt6-wayland` → `libqwayland-generic.so` (**generic wl_shm**) + `libxdg-shell.so` |
| QML modules (dlopen'd) | `qml6-module-qtquick`(+`-window`/`-controls`/`-layouts`/`-templates`) + `qtqml-workerscript` |
| wl_shm forcing | EGL/dmabuf/vulkan wayland-integration plugins **excluded** (`exclude_leaf_substrings`) so Qt picks the generic SHM QPA — matches the software compositor |
| runtime env | `QT_QPA_PLATFORM=wayland` (injected by the loader for the qt6 family, `runtime_report.cpp`) |

`qmleasing` is the ONE real Qt6 Quick GUI binary packaged in noble apt (the Qt example
binaries like `analogclock`/`wiggly` are source-only; there is no `qml6`/`qmlscene`
package). It opens a window with no args.

**Why generic wl_shm and not EGL:** the ALR compositor is `wl_shm`-only (software), the
same rung the WS-4 matrix names for qt6-wayland. `build_minimal_overlay`'s
`exclude_leaf` drops the wayland-egl platform plugin + the dmabuf/vulkan integration
clients; because the DT_NEEDED BFS starts at the *kept* leaf ELFs, excluding those
plugins also means `libQt6WaylandEglClientHwIntegration` never enters the overlay — Qt
then cannot select the EGL platform and falls back to the generic SHM QPA. (`libEGL.so.1`
is still shipped because `libQt6Gui`/`libQt6Quick` DT_NEEDED it unconditionally; it is
simply unused by the SHM platform.)

### Build result (host, `--base tiny-rootfs.tar`)

```
[qt6-gui] /tmp/qt6-gui-stage.tar
    files:           535
    exec (probe):    /usr/lib/qt6/bin/qmleasing       exec in overlay: YES
    reachable libs:  37   (libQt6{Core,Gui,Quick,Qml,Widgets,WaylandClient,…} + icu + libproxy + …)
    overlay_guard:   OK
    stage_tar_spec:  CONFORMANT
    => PASS
```

~106 MB (the full Qt6 Quick + QML stack), comparable to the netsurf overlay (~195 MB).
Contents verified: `platforms/libqwayland-generic.so` ✓, `wayland-shell-integration/
libxdg-shell.so` ✓, `qml/QtQuick/libqtquick2plugin.so` + `qmldir` ✓, QtQuick.Controls +
Layouts ✓ (17 `qmldir`), `libwayland-client` NOT shipped (base provides it — no
downgrade), wayland-egl platform plugin EXCLUDED ✓.

> **Build note (tool quirk, not a bug in this change):** `build_minimal_overlay` reuses a
> `_merged_root` inside `cache_dir` across runs; building two different overlays into the
> SAME cache dir can yield `file_count=0` on the second. Always build `qt6-gui` with a
> FRESH `--cache` dir (e.g. `/tmp/deb-cache-qt6gui`). The `qt6-gui` selftest is offline and
> unaffected.

### Why this is NOT a `BundledCatalog` entry

`BundledCatalog` is an **apt-install** catalog: every entry has an `RootfsDep(APT, …)` and
the host tests enforce `depKind == "APT"` + appId == apt-name == binary basename. A
qt6-gui overlay app is none of those (it cannot apt-install). Adding it would be a false
claim and would break `test_bundled_catalog_sources.py`. Instead it rides the SAME path as
the existing netsurf/sdl2/qt6 toolkit overlays: staged best-effort by
`NativeAppSession.OVERLAY_NAMES` (now includes `qt6-gui`) and launched by MainActivity's
qt6 GUI probe.

## 3. Device-verify checklist (Qt6-on-Wayland)

Prereq: the device is free; build the overlay host-side and push it.

```bash
# 1. Build the overlay (FRESH cache dir — see build note).
rm -rf /tmp/deb-cache-qt6gui
uvx python3 -m tools.build_toolkit_overlays --toolkit qt6-gui \
    --base rootfs/tiny-rootfs.tar --out-dir /tmp --cache /tmp/deb-cache-qt6gui
# expect: exec in overlay: YES, overlay_guard OK, stage_tar_spec CONFORMANT, => PASS

# 2. Push it to the device staging dir (NativeAppSession stages it best-effort; the
#    MainActivity toolkit loop / qt6 GUI probe also extracts it).
adb push /tmp/qt6-gui-stage.tar /data/local/tmp/qt6-gui-stage.tar

# 3. Launch the app process; the qt6 GUI probe (MainActivity, lines ~1912) picks the first
#    existing candidate. qmleasing is at /usr/lib/qt6/bin/qmleasing — confirm the probe
#    candidate list reaches it, OR drive it directly via the loader once staged.
```

**Expected (PASS):**
- `logcat alr_loader`: `qt6gui-result: rendered=true frames=N->M` (M>N) — a Qt Quick
  window presented through `wl_shm` to the `SurfaceView`.
- The qmleasing easing-curve editor window is visible/usable on the panel.
- `QT_QPA_PLATFORM=wayland` honored: NO "could not connect to display" / NO xcb fallback;
  the platform plugin loaded is `libqwayland-generic.so` (SHM), NOT egl.

**Known risks to watch on device (honest):**
- **QML module discovery** — if `qmleasing` reports "module QtQuick is not installed", the
  `QML2_IMPORT_PATH`/`QT_PLUGIN_PATH` may need to point at the overlay's
  `usr/lib/aarch64-linux-gnu/qt6/qml` + `…/qt6/plugins` (the loader's path mediation should
  already map these; if not, set them in the qt6 family env — a `runtime_report.cpp` change,
  which is cpp/other-owned, so file it back to that owner).
- **fontconfig** — Qt text needs a usable fontconfig cache; the base GIMP stack provides
  fonts, but a first-run cache build may be slow.
- **No EGL** — software rasterizer only (expected for this rung); animations may be slow but
  must render.

If `qmleasing` cannot find its QML modules even with the import path set, the fallback Qt
GUI proof is `qt6` (already shipped): `qtpaths6 --version` runs a real `libQt6Core` through
the loader (display-free) — proving the Qt runtime loads/inits natively, which is the
minimum Qt-class evidence.
