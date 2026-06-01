# WS-4 / L4 — Stage-tar convention (§5-E) + rootfs tooling reference

This is the contract WS-4 *provides* to the other workstreams (orchestration plan
§5-E "stage tar 규약") plus a reference for the L4 rootfs tooling under `tools/`.
Status: **host-verified**; device verification is the integration / WS-5 gate (CP-1).

---

## 1. The §5-E stage-tar convention

The base rootfs is installed once; apps are added as **overlay "stage tars"**
extracted on top of it (`RootfsInstaller.extractOverlayTar`, pushed to
`/data/local/tmp/<name>-stage.tar`, gated by a `.<name>-staged-<size>` marker).

A conformant overlay tar MUST:

1. **`./`-rooted, relative paths only** — member names like `./usr/bin/foo`; no
   absolute paths, no `..`, no char/block/FIFO device nodes.
2. **Safe symlinks** — targets resolve in-tree (relative, not `/`-absolute).
   Escaping/absolute symlink targets are skipped by the device extractor.
3. **Flat SONAME** — ship a shared library as a single **real file** named
   `libNAME.so.MAJOR` (e.g. `libharfbuzz.so.0`), **not** the Debian
   `libNAME.so.MAJOR -> libNAME.so.MAJOR.MINOR.PATCH` symlink + versioned-file pair.
4. **No base-lib downgrade** — an overlay must not replace a base-provided SONAME
   with an older library. This is *enforced*, not just documented (see §2).

`.<name>-staged-<size>` marker: re-pushing a changed tar (new size) auto-triggers a
fresh overlay extract.

---

## 2. The lib-downgrade guard (M1) — why it exists and how it works

### The harfbuzz regression class
The base ships `libharfbuzz.so.0` as the **real harfbuzz 8.3.0** binary (flat
SONAME). The `chromium-stage.tar` overlay shipped the Debian layout
(`libharfbuzz.so.0 -> libharfbuzz.so.0.60000.0` symlink + the 6.0.0 real file).
A raw extract deleted the base real file and repointed the SONAME to the older
6.0.0 → pango lost `hb_ot_color_has_paint` → every GTK app + GIMP exit=127.

### Why the rule is *structural*, not version arithmetic
The base `var/lib/dpkg/status` is **empty (0 bytes)** and the base flattened its
SONAMEs, so the real version is gone from the filename — `libharfbuzz.so.0` looks
like version `(0,)`, "smaller" than the overlay's `(0, 60000, 0)`, even though it
is actually *newer*. So filename arithmetic is unreliable. The decidable rule:

> **A SONAME the base provides as a real file is FROZEN.** An overlay may not
> repoint it (symlink) nor ship a versioned variant (`libNAME.so.MAJOR.MINOR...`)
> for it. Numeric comparison is used only when **both** sides are versioned, or
> when both carry a `lib-versions.json` sidecar (§3).

### Two enforcement points
- **Build-time (host gate):** `tools/overlay_guard.py` — fails the build (exit 1)
  if an overlay would downgrade a base lib. On the real `chromium-stage.tar` vs the
  base it flags **harfbuzz, liblcms2, libopenjp2** (2 latent downgrades beyond the
  one that was hand-patched).
- **Device:** `RootfsInstaller.extractOverlayTar` applies the same frozen-SONAME
  rule during extraction — skips + `Log.w` the downgrade entries, keeps base libs,
  still lands the rest of the overlay. The active overlay sites in `MainActivity`
  (foot, gtk3demo, and the gated-off chromium) all route through it. The Kotlin
  `overlayVerdict` mirrors the Python rules 1:1.

---

## 3. `lib-versions.json` sidecar — closing the flat-over-flat gap

When an overlay ships a *flat* `libNAME.so.MAJOR` over a base *flat* file of the
same SONAME, the filename can't prove the minor/patch order. `build_stage_tar`
emits a `<tar>.lib-versions.json` sidecar (`{"<dir>/<soname>": "M.N.P"}`) recording
the real version it flattened from. `overlay_guard` consumes the overlay (and, if
present, base) sidecar to turn that ambiguous **WARN into a numeric BLOCK/allow**.
Without sidecars it stays a non-blocking WARN (the device applies it — a flat
overlay is the conformant shape and may be a legitimate restore/upgrade, e.g.
`harfbuzz-fix-stage.tar`).

---

## 4. Tooling reference (`tools/`, host `--selftest` verified)

| Tool | Purpose | CLI |
|---|---|---|
| `overlay_guard.py` | base-lib downgrade gate (frozen-SONAME + sidecar) | `--base <tar\|dir> --overlay <tar> [--strict]` / `--selftest` |
| `stage_tar_spec.py` | §5-E convention validator (composes overlay_guard) | `--overlay <tar> [--base ...] [--strict]` / `--selftest` |
| `build_stage_tar.py` | extracted Debian root → flat-SONAME `./`-tar + sidecar | `--src <dir> --out <tar> [--versions <json>]` / `--selftest` |
| `compat_matrix.py` | M5 app×toolkit×result matrix + universality gate | `--demo` / `--selftest` |
| `xkb_probe.py` | rootfs xkb-data completeness probe (§6) | `--rootfs <dir\|tar>` / `--selftest` |
| `safe_tar.py` | tar member inspection + safety validation | (library) |

Run host tests/selftests with a Python ≥3.10 interpreter (repo uses PEP 604 unions;
`/usr/bin/python3` 3.9 fails collection).

### Real-deb pipeline validation (evidence)
`build_stage_tar` was run end-to-end on the real bookworm `libjpeg62-turbo_2.1.5-2_arm64.deb`:
`extract_deb` (ar + stdlib lzma) → the Debian `libjpeg.so.62` symlink +
`libjpeg.so.62.3.0` real file were **flattened to a single real `libjpeg.so.62`**,
1 symlink dropped, sidecar `{usr/lib/aarch64-linux-gnu/libjpeg.so.62: "62.3.0"}`
emitted; the output passed `stage_tar_spec` (conformant, 0 errors/warnings) and
`overlay_guard` (0 violations) against the base. The pipeline is proven on real
Debian arm64 binaries; only runtime (device) execution is unverified.

---

## 5. M2 (toolkit matrix) & M4 (Xwayland) roadmap

Grounded in `alr_wayland/alr_compositor.cpp`: advertises wl_compositor v4, wl_shm,
xdg_wm_base v2, wl_seat v5, wl_output v2, wl_subcompositor, wl_data_device_manager.
**No `zwp_linux_dmabuf`/`wl_drm`** (non-wl_shm buffers rejected) and **no XWM**.

**Build blocker:** this dev host has no docker/debootstrap/dpkg-deb/qemu, so the
arm64 overlays can't be built here — build them in a Debian arm64 env with
`build_stage_tar.py`. (arch-`all` packages, and downloadable arm64 `.deb`s, can be
flattened from host — see §4 evidence.)

**M2 order (easiest→hardest), all software-raster via wl_shm:**
1. `netsurf-gtk` (GTK3 frontend; reuses the base GTK3 closure → ~free). `GDK_BACKEND=wayland`.
2. `libsdl2-2.0-0` + `libxkbcommon0`/`libwayland-egl1`/`libdecor-0-0`. `SDL_VIDEODRIVER=wayland`.
3. `qt6-wayland` closure (largest). `QT_QPA_PLATFORM=wayland`, `QT_QUICK_BACKEND=software`, prefer QtWidgets.

No dmabuf → all toolkit GL/EGL accel is unavailable until the GPU-marshalling track
lands; force software fallbacks (`LIBGL_ALWAYS_SOFTWARE=1`).

**M4 Xwayland — ROOTFUL only.** Rootless needs the compositor to be the X window
manager (no XWM in ALR) → not viable. Rootful presents one root X window as a
single `xdg_toplevel`, needs no XWM, works with the globals ALR already has.
- Overlay (~15MB): `xwayland` + `xkb-data` + xcb/x11 client libs + `xfonts-base`
  (+ optional tiny WM, demo `x11-apps`/`xterm`).
- Launch: `Xwayland :0 -ac -shm -retro -noreset &`, `DISPLAY=:0`, `LIBGL_ALWAYS_SOFTWARE=1`.
  `-shm` forces pixman→wl_shm present; `-ac` drops X auth. Use the **filesystem**
  X11 socket in the rootfs-mediated `/tmp/.X11-unix` (chmod 1777), not abstract
  sockets (SELinux denials under untrusted_app).

Each landed overlay → a guarded `extractOverlayTar` site in `MainActivity` + a
`compat_matrix` entry. Device verification = integration/WS-5 gate.

---

## 6. xkb / NO_KEYMAP — correction for the compositor (WS-3)

A sibling session attributed a `wl_keyboard.keymap` NO_KEYMAP crash to "rootfs
missing xkb-data". **The base rootfs already ships the full keymap data** (verified
by `xkb_probe`): `/usr/share/X11/xkb/` (316 files — rules/evdev, keycodes/evdev,
types/complete, compat/complete, symbols/{pc,us,inet}, …) + `libxkbcommon.so.0`.
Everything needed to compile the default evdev/pc105/us keymap is present.

So an xkb-data overlay is **redundant**. The real fix is on the compositor side:
to emit a real `WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1` keymap, the keymap compiler (the
Android/NDK side, with its own libxkbcommon) must read the rootfs copy — point it at
`XKB_CONFIG_ROOT=<filesDir>/rootfs/<name>/usr/share/X11/xkb` (or an xkb_context
include path) before `xkb_keymap_new_from_names`. Confirm with
`python -m tools.xkb_probe --rootfs <dir|tar>`.
