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
| `qt6-wayland` | **`--minimal`** | 47 | ~72.7 MB | Qt6 libs + ICU (inherent); reachable=27, **qtwayland platform plugins survive** (libqwayland-egl/generic, leaf files); missing_soname=NONE, CONFORMANT |

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

---

## 9. DISTRO CORRECTION — the base is **Ubuntu noble (24.04)**, not Debian bookworm

Confirmed (`strings libc.so.6` → "**Ubuntu GLIBC 2.39-0ubuntu8.7**"; `os-release`
ID=androlinux-tiny; `etc/apt/sources.list.d/ubuntu.sources` → noble ports; Ubuntu
keyrings). The plan's "bookworm-slim" label is wrong — the rootfs is Ubuntu noble
arm64. **All overlays must be built from Ubuntu noble**, not Debian bookworm, so the
glibc/lib ABIs and package versions match the base.

`deb_closure` now supports this:
- `fetch_packages_index(..., components=(...))` merges multiple components — Ubuntu
  splits libs across **main + universe** (bookworm-only `main` misses deps).
- default opener sends an apt-like **User-Agent** (ports.ubuntu.com 403s Python-urllib).
- `extract_deb` handles **`data.tar.zst`** via the `zstd` CLI (Ubuntu .deb use zstd).
- CLI: `--component main --component universe`.

**Canonical build commands (Ubuntu noble):**
```
M=http://ports.ubuntu.com/ubuntu-ports
# C.UTF-8 locale (§8):
python -m tools.build_locale_overlay --out /tmp/xkb-gegl-stage.tar --suite noble \
    --cache /tmp/deb-cache-ubuntu   # (build_locale_overlay --suite/--mirror; mirror=$M)
# toolkit (minimal):
python -m tools.deb_closure --minimal --package netsurf-gtk --base <base.tar> \
    --out /tmp/netsurf-stage.tar --mirror $M --suite noble \
    --component main --component universe --cache /tmp/deb-cache-ubuntu
```

**Rebuilt from noble (correct distro), device-pending:**
| overlay | size | reachable libs (base-lacking) | note |
|---|---|---|---|
| `xkb-gegl-stage.tar` | 370 KB | — | noble libc-bin C.utf8 + C.UTF-8 symlink; CONFORMANT |
| `netsurf-stage.tar` | ~7.1 MB | libcurl.so.4, libssh.so.4 | only 2 (noble base already has libjpeg/libldap/…); missing_soname=NONE, CONFORMANT |

**TODO:** `sdl2-stage.tar` / `qt6-stage.tar` were first built from Debian bookworm —
rebuild them from noble with the command above (`--package libsdl2-2.0-0` /
`qt6-wayland`) before device-staging.

---

## 10. GUI overlay = C.UTF-8 locale + SVG pixbuf loader (device-evidence-driven)

Device drain v127 (`docs/evidence/2026-06-01-batch-drain-cp2-progress-svg-locale-cp3baseline.md`)
pinned the REAL gtk3-widget-factory abort: **`gdk-pixbuf` cannot dlopen
`loaders/libpixbufloader_svg.so` → Gtk:ERROR loading image-missing.svg → abort(6)**.
The C.UTF-8 `setlocale` failure is a non-fatal Gtk-WARNING (secondary).

Host investigation: the current base tar DOES ship the svg loader + `librsvg-2.so.2`
+ a complete `loaders.cache` (svg stanza, correct underscore path) with NO missing
DT_NEEDED dep (`elf_needed` confirms). So the device rootfs is **stale** — the svg
loader was added to the base by a prior "ALR gtk3-fix" but the rootfs version marker
didn't bump, so RootfsInstaller skipped re-extraction. An OVERLAY (own `.staged`
marker) lands regardless → delivers the svg loader directly.

`tools/build_gui_overlay.py` assembles the single wired **`xkb-gegl-stage.tar`** slot:
- C.utf8 (+ C.UTF-8 symlink) from **Ubuntu noble** libc-bin (matching glibc 2.39), and
- `libpixbufloader_svg.so` + `librsvg-2.so.2` + `loaders.cache`, lifted from the base
  tar (their core deps libcairo/libxml2/… predate the svg fix → already on device).

Result: **~6.16 MB**, stage_tar_spec CONFORMANT, overlay_guard 0 BLOCK (1 WARN: the
librsvg re-ship is byte-identical to base — harmless). Build:
`python -m tools.build_gui_overlay --out /tmp/xkb-gegl-stage.tar --base <base.tar>`.

DEVICE-REQ: push this `xkb-gegl-stage.tar` → cold-start gtk3-widget-factory →
no SIGABRT (svg loader opens, icons render) + setlocale C.UTF-8 OK (noble C.utf8).

### 10.1 — .so executable bit (device drain: the actual SIGABRT cause)
Re-drain found the svg loader was present on device but `dlopen` still failed:
ALR's file-backed PROT_EXEC (untrusted_app) **rejects a non-executable `.so`**
(gpushim's 0700 lib loads; Debian's 0644 → extracted 0600 → fails). Fixes:
- `RootfsInstaller.extractEntry`: give any `*.so` / `*.so.*` file the exec bit on
  extraction (not just tar entries with x). Shared by extractVerifiedTar (base) +
  extractOverlayTar (overlays) → fixes the base pixbuf loaders (bmp/gif) on
  re-extraction AND overlay `.so`s.
- `build_gui_overlay`: package `.so` as **0o755** so the tar itself carries x.
(Stock Linux dlopen doesn't need the file x bit; ALR/Android does.)

C.UTF-8 still unresolved: C.utf8 (noble) is present but `setlocale` fails — not a
perm issue; needs a device strace of the locale `open()` path (LOCPATH /
locale-archive / path-mediation). WS-4 follow-up, DEVICE-REQ.

---

## 11. Toolkit matrix (b) — all overlays rebuilt from Ubuntu noble (distro-correct)

All device-pending overlays in `/tmp`, built from `ports.ubuntu.com noble main+universe`,
`--minimal`, base-subtracted, .so 0755, missing_soname=0, overlay_guard 0 BLOCK:

| overlay | size | reachable new libs | note |
|---|---|---|---|
| `xkb-gegl-stage.tar` | 6.16 MB | (svg loader + librsvg + C.utf8) | GUI: locale + svg loader (0755) |
| `netsurf-stage.tar` | 7.1 MB | libcurl, libssh | GTK3 browser |
| `sdl2-stage.tar` | 8.2 MB | 20 | (noble base already had most deps; vs 30.6MB from bookworm) |
| `qt6-stage.tar` | 74.2 MB | 28 | Qt6 + ICU (inherent); qtwayland plugins kept |

## 12. C.UTF-8 setlocale — HANDOFF to WS-1 (path-mediation / LOCPATH)

WS-4 data part is DONE and CORRECT: `/usr/lib/locale/C.utf8/LC_CTYPE` is a valid
glibc locale (magic `0x20090720`, 360460 B, from **noble** libc-bin matching the base
glibc 2.39) + a `C.UTF-8 → C.utf8` symlink; it sits on glibc's compiled default path
`/usr/lib/locale` (confirmed in libc.so.6 strings). Yet device `setlocale(LC_ALL,
"C.UTF-8")` returns NULL ("Locale not supported"). This is **NOT a rootfs/data issue**
— it is the guest glibc's locale `open()` of `/usr/lib/locale/{locale-archive, C.utf8/LC_*}`
not being path-mediated to the rootfs (the `.so` loader opens at `/usr/lib/aarch64-
linux-gnu/...` ARE mediated, so the locale open path is reaching Android, not the
rootfs). **WS-1 (L1 loader/mediation):** either ensure the path-mediation covers
glibc's locale `open()`/`openat()` for `/usr/lib/locale`, or set `LOCPATH=/usr/lib/locale`
in `guest_env` (§5-D). Non-fatal (Gtk-WARNING; falls back to C) — the gtk3 abort was
the svg `.so` (§10.1), already fixed.

---

## 13. §10(c) apt/dpkg — reconstructed dpkg admin DB

The base ships apt/dpkg binaries + Ubuntu-noble apt sources, but
`var/lib/dpkg/status` is **empty** (built by file-extraction, not `dpkg -i`) → apt
thinks NOTHING is installed → `apt install X` would re-fetch the entire dep tree.

`tools/build_dpkg_db.py` reconstructs the dpkg admin DB from the base's files via the
Ubuntu noble **Contents** index (file→package) + the Packages index (control fields):
- Maps base files → packages, with **SONAME mapping** (the base flattens
  `libgtk-3.so.0` while Contents lists `libgtk-3.so.0.2409.x`) and **merged-usr
  aliasing** (`/lib`↔`/usr/lib`) — without these, glib/gtk/gcc/stdc++ go unmapped.
- Emits `./var/lib/dpkg/status` (one `Status: install ok installed` stanza per pkg,
  carrying Version/Depends/…) + `./var/lib/dpkg/info/<pkg>.list`.

Built `/tmp/dpkg-db-stage.tar` (~700 KB): **194 packages** reconstructed incl.
`libc6`, `libgtk-3-0t64`, `libglib2.0-0t64` (Ubuntu noble t64 names), `libgcc-s1`,
`libstdc++6`, gimp, gdk-pixbuf, pango, cairo. CONFORMANT. Wired into the MainActivity
toolkit-stage loop (guarded extractOverlayTar). Build:
`python -m tools.build_dpkg_db --base <base.tar> --out /tmp/dpkg-db-stage.tar`.

DEVICE-REQ: push dpkg-db-stage.tar → `dpkg -l` shows ~194 pkgs; `apt-get update`
(noble); `apt-get install --no-install-recommends <leaf>` fetches only NEW deps (not
the base stack). NOTE: actually RUNNING dpkg (maintainer-script fork/exec) under the
ALR loader is the L1/device gate (PRoot clone3 KNOWN_FAIL; native-loader path
untested) — the reconstructed DB is the rootfs-side prerequisite.

---

## 14. §3 M4 — Xwayland (X11 apps), rootful

X11-only apps via Xwayland. The ALR compositor has **no XWM** → only **rootful**
Xwayland works (one root X window as a single xdg_toplevel; an in-rootfs WM, or a
single fullscreen app, arranges windows inside it). Rootless needs the compositor to
be the X window manager — not viable.

Overlay built from noble (`--minimal`, device-pending):
- `/tmp/x11-stage.tar` (~11.8 MB) = `Xwayland` + `xterm` + `x11-apps` (xeyes/oclock/…)
  + the 23 deps the noble base lacks. missing_soname=0, CONFORMANT. Wired into the
  MainActivity toolkit-stage loop (`x11`).  (`/tmp/xwayland-stage.tar` = server only, 6.2MB.)
- Build: `python -m tools.deb_closure --minimal --package xwayland --package x11-apps
  --package xterm --base <base.tar> --out /tmp/x11-stage.tar --mirror
  http://ports.ubuntu.com/ubuntu-ports --suite noble --component main --component universe`.

Launch recipe (rootful; xkb-data already in base):
```sh
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
mkdir -p /tmp/.X11-unix; chmod 1777 /tmp/.X11-unix      # FILESYSTEM socket (NOT abstract — untrusted_app)
Xwayland :0 -ac -shm -retro -noreset &                  # -shm=pixman→wl_shm present; -ac=no X auth
export DISPLAY=:0 LIBGL_ALWAYS_SOFTWARE=1
xeyes    # or: xterm
```
DEVICE-REQ: push x11-stage.tar → launch Xwayland rootful on the compositor →
`xeyes`/`xterm` renders (wl_shm). NOTE: the rootful launch wiring (Xwayland as a
Wayland client + the X filesystem socket under the rootfs-mediated /tmp) is L2/L3 +
integration; this overlay is the WS-4 rootfs prerequisite.

---

## 15. WS-4 overlay roster — host-complete (all device-pending in /tmp, noble-built)

All §5-E, base-subtracted, .so 0o755, missing_soname=0, overlay_guard 0 BLOCK,
CONFORMANT. Built from Ubuntu noble (ports). Wired into MainActivity (guarded
extractOverlayTar). The integration session adb-pushes these and re-drains.

| overlay | size | provides |
|---|---|---|
| `xkb-gegl-stage.tar` | 6.2 MB | C.UTF-8 locale + svg pixbuf loader (0755) |
| `netsurf-stage.tar` | 7.1 MB | netsurf-gtk (GTK3 browser) |
| `sdl2-stage.tar` | 8.2 MB | libSDL2 + wayland/audio deps |
| `qt6-stage.tar` | 74.2 MB | Qt6 + qtwayland plugins + ICU |
| `x11-stage.tar` | 11.8 MB | Xwayland (rootful) + xterm + x11-apps |
| `foot-stage.tar` | 1.1 MB | foot (wayland terminal) |
| `gtk3demo-stage.tar` | 14.9 MB | gtk3-demo / gtk3-widget-factory |
| `dpkg-db-stage.tar` | 0.7 MB | reconstructed dpkg admin DB (194 pkgs) |
| `apt-config-stage.tar` | <1 KB | neutralize third-party apt sources (noble-only) |

WS-4 milestones M1–M5 + §10(a)(b)(c) host-complete. Device/L1-gated remainders
(integration/other WS): C.UTF-8 `setlocale` path-mediation (WS-1, §12); dpkg
maintainer-script fork/exec under the loader (L1); toolkit/Xwayland launch-on-
compositor wiring (L2/L3); and the base `.so` exec-bit fix (RootfsInstaller, ws-4
3649725) applies on the next base re-extraction (prepareBundledTinyRootfs re-extracts
every cold start).
