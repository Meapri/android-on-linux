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
| `deb_closure.py` | **M2/M4 build engine**: resolve a Debian Depends closure, subtract the base, download .debs, emit a §5-E overlay | `--package <name> --base <tar\|dir> --out <tar> [--suite bookworm] [--cache <dir>]` / `--selftest` |
| `overlay_guard.py` | base-lib downgrade gate (frozen-SONAME + sidecar) | `--base <tar\|dir> --overlay <tar> [--strict]` / `--selftest` |
| `stage_tar_spec.py` | §5-E convention validator (composes overlay_guard) | `--overlay <tar> [--base ...] [--strict]` / `--selftest` |
| `build_stage_tar.py` | extracted Debian root → flat-SONAME `./`-tar + sidecar | `--src <dir> --out <tar> [--versions <json>]` / `--selftest` |
| `base_inventory.py` | inventory base SONAMEs/binaries + `diff_against()` subtraction | `--rootfs <tar\|dir> [--json]` / `--selftest` |
| `compat_matrix.py` | M5 app×toolkit×result matrix model + universality gate | `--demo` / `--selftest` |
| `alr_compat.py` | M5 ALR current-state matrix (real data, attributed) | `--report` / `--json` / `--selftest` |
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

**Build path (no Debian env needed for these):** the host has no
docker/debootstrap/dpkg-deb/qemu, but `tools/deb_closure.py` builds the overlays
straight from the Debian mirror:

```
python -m tools.deb_closure --package libsdl2-2.0-0 \
    --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar \
    --out /tmp/sdl2-stage.tar --cache /tmp/deb-cache
```

It resolves the runtime closure, downloads the `.deb`s, **subtracts everything the
base already provides** — by SONAME (downgrade-frozen) AND by path with merged-usr
aliasing (`/lib` ≡ `/usr/lib`), so libc6 / the core runtime is never shadowed — and
flattens the remainder into a §5-E overlay (validated by stage_tar_spec +
overlay_guard). The produced `.tar` is device-pending (runtime = CP-1 gate).

Closure sizes (bookworm arm64, full download before base subtraction):

| target | closure pkgs | download | notes |
|---|---|---|---|
| `libsdl2-2.0-0` | 55 | ~11 MB | smallest; **built+validated** (see evidence below) |
| `xwayland` | 89 | ~60 MB | M4; pulls xkb-data, libxcb, xfonts |
| `qt6-wayland` | 132 | ~74 MB | largest; libqt6* + ICU |
| `netsurf-gtk` | 180 | ~77 MB | closure incl. full GTK3 (mostly base → subtracted) |

**SDL2 overlay — built + validated (evidence):** `deb_closure` built
`/tmp/sdl2-stage.tar` over the network — closure 55 pkgs, 351 base files subtracted,
0 unsupported/missing. The overlay carries only the NEW libs (libSDL2-2.0.so.0,
libdecor, libpulse, libwayland-server, libgbm, libdrm, audio codecs …) and ships
**no libc.so.6 / ld-linux / libstdc++** (base runtime intact). `stage_tar_spec`:
CONFORMANT (0 errors/warnings); `overlay_guard`: 0 violations. Runtime device test
is the CP-1 gate.

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
`python -m tools.xkb_probe --rootfs <dir|tar>`. (WS-1 did exactly this in v127 —
gtk3-widget-factory now renders on device.)

---

## 7. overlay minimization (prune) + closure-size reality

`deb_closure` over-includes: the full transitive `Depends` closure pulls perl (via
ca-certificates), the entire Adwaita icon theme, ICU, systemd units, man/doc/locale,
etc. `build_overlay` now drops base-duplicate paths **and** a default
`DEFAULT_PRUNE_PREFIXES` set (man/doc/info/locale/lintian/gtk-doc/pkgconfig/include/
systemd/tmpfiles/var) — runtime-irrelevant for a no-systemd, C.UTF-8, no-dev guest.
Deliberately kept: `usr/share/icons` (GTK needs it), fonts, mime, perl, libicu, gconv.

Built, base-subtracted + pruned, guard-clean (device-pending):

| overlay | files | size | note |
|---|---|---|---|
| `sdl2-stage.tar` | 586 | ~31 MB | libSDL2 + audio/wayland deps; no core-lib shadow |
| `netsurf-stage.tar` | 9083 | ~195 MB | still large — full Depends pulls perl + Adwaita + ICU |

**Resolved — DT_NEEDED-minimal closure (`--minimal`):** the full `Depends` closure
is too broad (netsurf ~195 MB: perl via ca-certificates, full Adwaita, ICU). The fix
is `deb_closure.build_minimal_overlay` / `--minimal`: keep the **leaf package's own
files** (binaries + plugins + data) + only the libs **reachable via DT_NEEDED** from
them (transitively) that the base lacks; drop the rest. Uses `tools/elf_needed.py`
(pure-Python ELF DT_NEEDED/DT_SONAME reader, no pyelftools). A need the base already
provides stops the walk (base ships it + its deps); dlopen'd toolkit modules survive
because they live in the base (gdk-pixbuf/pango) or the leaf package (Qt's qtwayland
plugin) — use `keep_prefixes=` to force-keep extra data dirs.

| overlay | mode | files | size | note |
|---|---|---|---|---|
| `netsurf-gtk` | full Depends | 9083 | ~195 MB | perl + Adwaita + ICU bloat |
| `netsurf-gtk` | **`--minimal`** | **9** | **~7.1 MB** | netsurf bin + libcurl/libjpeg/libldap/liblber/libpthread; missing_soname=NONE, guard-clean, CONFORMANT (27× smaller) |

Build: `python -m tools.deb_closure --minimal --package netsurf-gtk --base <base.tar> --out /tmp/netsurf-stage.tar`.

## 8. (a) GUI-stability — C.UTF-8 locale (root cause found + fixed)

§10(a): residual gtk3-widget-factory/gimp **SIGABRT**. **Root cause (host-confirmed):**
- The base ships **zero** compiled locales — `/usr/lib/locale` is absent (0 entries).
- The guest env exports `LC_ALL=C.UTF-8` / `LANG=C.UTF-8` (WS-1, runtime_report.cpp).
- Debian glibc does **not** compile C.UTF-8 into `libc.so.6` (the literal name is not
  in the base's 2.39 libc) — it ships the precompiled dir **`/usr/lib/locale/C.utf8`
  in the `libc-bin` package** (~392 KB). With it missing, `setlocale(LC_ALL,"C.UTF-8")`
  → NULL → GLib/GTK aborts at startup. (babl/gegl are NOT the cause — base already has
  `libbabl-0.1.so.0`, `libgegl-0.4.so.0` + 37 `gegl-0.4/*` ops.)

**Fix (built, validated, device-pending):** `tools/build_locale_overlay.py` extracts
`libc-bin`'s `/usr/lib/locale/C.utf8` (glibc normalizes the requested "C.UTF-8" →
dir "C.utf8") + adds a `C.UTF-8` → `C.utf8` symlink, and emits a **370 KB** §5-E
overlay = `xkb-gegl-stage.tar` (the already-wired, guarded MainActivity slot — no
Kotlin change). `stage_tar_spec`: CONFORMANT; `overlay_guard`: 0 violations (new
paths, base lacks them). This replaces the 227 MB `locales-all` approach — only the
one needed locale is staged. Build:
`python -m tools.build_locale_overlay --out /tmp/xkb-gegl-stage.tar --base <base.tar>`.

DEVICE-REQ (on the ws-4 commit): cold-start gtk3-widget-factory + gimp with
`xkb-gegl-stage.tar` pushed → no SIGABRT (locale resolves). Source `libc-bin` is
glibc 2.36 (bookworm); the LC_* format is backward-compatible with the 2.39 base —
if the device shows a locale-version error, rebuild with `--suite trixie`.
