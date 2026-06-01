# Real-toolkit-on-`wl_shm` rung: pixman 2D client → ALR compositor → SurfaceView

Status: **host-built + host-render-validated, device-pending.** Next rung after
v82 (`0.4.82-android-wayland-compositor-v82`, the FIRST-WINDOW solid-colour
`alr-wl-test` pass).

## Goal of this rung

v82 proved the wire end-to-end with a *hand-rolled* `wl_shm` client
(`/bin/alr-wl-test`) that just memset a solid colour into the buffer. This rung
replaces that with a **real 2D graphics toolkit rasterising the frame**:
[pixman](https://gitlab.freedesktop.org/pixman/pixman) 0.42.2 — the same
production software rasteriser used by **Cairo**, the **X.Org server**, and
(historically) **Qt's raster paint engine**. The client
(`/bin/alr-pixman-test`) drives pixman to draw, into the committed `wl_shm`
buffer:

* a real **linear gradient** background (`pixman_image_create_linear_gradient`,
  deep-blue → teal),
* two **anti-aliased filled circles** (triangle-fan coverage accumulated into an
  `a8` mask, then the solid composited through it once — the seam-free
  image-backend fill technique Cairo uses),
* the text **"ALR"** as **anti-aliased filled vector glyphs** (stroked outlines →
  triangles → one `a8` coverage mask; **no FreeType/fontconfig needed**).

On success the guest prints `alr-toolkit: pixman drew frame (480x320 …)` and
exits 0. This is a genuine 2D toolkit doing AA rasterisation into the shm
buffer — not pixel-plotting.

## Why pixman (and not SDL2 / GTK3 / full Cairo) for *this* rung

The PoC rootfs (`tiny-rootfs.tar`, Ubuntu glibc 2.39 base) ships **no GUI
toolkit libraries at all**. The only client-usable shared objects are
`libc.so.6`, `libm.so.6`, `libffi.so.8`, `libstdc++.so.6`, `libgcc_s.so.1`.
There is **no** `libcairo`, `libpixman`, `libfreetype`, `libfontconfig`,
`libSDL2`, `libgtk-3`, `libgdk`, `libglib`, `libpango`, `libxkbcommon`,
`libepoxy`, `libwayland-client`. (Survey: `tar tf … | grep -iE
'libSDL|libgtk|libcairo|libpango|libwayland|libxkb|libgdk|libegl|libGLES|libglib|libffi|libpixman|libfontconfig|libfreetype'`
returns only `libffi.so.8`.)

The compositor currently exposes **only `wl_shm`** to clients — no
`wl_drm`/`linux-dmabuf`/`wl_egl` GL path — so a toolkit must render in
**software** to a shm buffer (Cairo/pixman software, not GL). That rules out the
GL-backed default paths of every heavy toolkit.

Decision, lightest real-toolkit first (the task's preference order was SDL2 →
Cairo → GTK3):

| option | verdict for this rung |
|---|---|
| **(a) SDL2** | Needs `libSDL2-2.0.so.0` + its Wayland video backend (`libwayland-client`, `libxkbcommon`, `libwayland-cursor`) + `libdecor` for SSD, none in rootfs. SDL2's software path (`SDL_CreateRGBSurface` + `SDL_Renderer` "software") *can* present to a `wl_shm` framebuffer, but only after provisioning ~5–8 `.so`s. Deferred — see "SDL2 path" below. |
| **(b) Cairo-on-`wl_shm`** | The canonical answer. Even an image-only Cairo build is **~95 `.c` files** + a generated `cairo-features.h`/`config.h` with deep feature interdependencies (meson-driven). Buildable via `zig cc` but high effort/risk for one window. Its built-in **`cairo-font-face-twin`** vector font would give real `cairo_show_text("ALR")` with no FreeType — attractive for later. Deferred — see "Cairo path" below. |
| **(c) GTK3** | ~30 libs (gtk/gdk/glib/gobject/gio/pango/cairo/gdk-pixbuf/harfbuzz/fontconfig/freetype/libxkbcommon/libepoxy) + a fontconfig cache. Heaviest; far off. Documented for completeness only. |
| **→ pixman now** | **A real, production 2D toolkit** that is small and self-contained: ~30 generic-C files, one tiny `config.h`, **zero rootfs provisioning** (compiled into the client; runtime deps = `libc`/`libm`/`libffi`, all already present). Delivers AA gradient + shapes + vector text today. **Chosen.** |

pixman is a legitimate "real toolkit renders" milestone: it is the actual
rasteriser inside Cairo. Stacking the Cairo API on top (path/text convenience)
is a strictly-later, larger rung that reuses this exact compositor seam.

## What was built

`/tmp/alrbins/alr-pixman-test` — dynamically-linked **aarch64 glibc PIE
(`ET_DYN`)**. The `wayland-client` core + protocol marshalling tables
(libwayland 1.25.0) **and all of pixman 0.42.2** (generic C path, no SIMD) are
compiled directly into the binary, so at runtime it needs only the rootfs's
`libc.so.6` + `libm.so.6` + `libffi.so.8`. Mirrors the proven `alr-wl-test`
recipe (`zig cc --target=aarch64-linux-gnu.2.36 -fPIE -pie`).

```
file:    ELF 64-bit LSB pie executable, ARM aarch64, dynamically linked,
         interpreter /lib/ld-linux-aarch64.so.1, stripped
ET_DYN:  yes (e_type=3, e_machine=183 AArch64)
NEEDED:  libffi.so.8   libm.so.6   libc.so.6      (all present in tiny-rootfs.tar)
max sym: GLIBC_2.35 (hypot)  ≤  rootfs glibc 2.39   + LIBFFI_BASE_8.0
size:    511112 bytes
sha256:  24b63c634f5dcc81c0e3ec3d04ac7725fc4d9c4086e469357ecaa7cf5fcd0af4
```

There are **0** undefined `pixman_*` / `wl_*` / `xdg_*` symbols (verified with
`objdump -T`): the toolkit is genuinely linked in, not dynamically referenced.

### Pixel format (load-bearing — matches the compositor)

The client allocates a `wl_shm` `WL_SHM_FORMAT_XRGB8888` buffer (fmt=1) and
wraps it in a `PIXMAN_a8r8g8b8` image. On little-endian aarch64 a pixman
`a8r8g8b8` pixel `0xAARRGGBB` is bytes **`[B,G,R,A]`** in memory — exactly what
the compositor's EGL present expects (`runtime_report.cpp` uploads as `GL_RGBA`
and the fragment shader swizzles `.bgra`; see `alr_compositor.cpp` present
notes). Colours therefore display with correct hue. The client also `|=`s the
high (X/A) byte to `0xFF` so the buffer reads opaque under either XRGB or ARGB
interpretation.

### Host render validation

Since aarch64 can't run on the macOS build host, the **identical draw routines**
were compiled host-native against the same pixman sources and executed; output
written to a PPM and inspected:

```
gradient: OK
non-uniform-pixels=153590 (100.0%)   # every pixel varies → real gradient, not a fill
```

The rendered frame shows the blue→teal gradient, both solid AA discs (seam-free),
and a crisp white anti-aliased **"ALR"**. The drawing logic is correct; only the
target triple differs between the validated host build and the shipped aarch64
binary (same `.c`, same pixman).

## Build (reproducible)

```
/tmp/alrbins/build-alr-pixman-test.sh  /abs/path/to/repo  [out_dir=/tmp/alrbins]
```

The script (host needs `zig >= 0.16`, `curl`, network on first run):

1. fetches the `wayland-client` core (`wayland-client.c` + `-core.h`) from the
   pinned libwayland **1.25.0** source (not vendored), cached in `$OUT/wl125`;
2. fetches **pixman 0.42.2** generic-C sources (30 `.c`, 7 private headers, the
   `dither/blue-noise-64x64.h`), cached in `$OUT/src-cairo/pixman` — the
   generated `pixman-version.h` + the ALR `config.h` (generic C path: `TLS
   __thread`, `SIZEOF_LONG 8`, `HAVE_BUILTIN_CLZ`, no `USE_*` SIMD) live there;
3. flattens the vendored wayland headers + the checked-in
   `wayland_generated/{wayland-client-protocol.h,xdg-shell-client-protocol.h}` +
   `{wayland-protocol.c,xdg-shell-protocol.c}`;
4. compiles pixman on its own `-I` (its `config.h`) and wayland/the client on
   theirs (wayland's `config.h`) — the two `config.h`s must not cross;
5. extracts the rootfs `libffi.so.8` to link against (gives `NEEDED
   libffi.so.8`);
6. links a stripped dynamic PIE with `-lffi -lm`.

Source: `/tmp/alrbins/alr-pixman-test.c`.

## Rootfs additions required

**None for libraries.** The runtime closure (`libc.so.6`, `libm.so.6`,
`libffi.so.8`, interp `/lib/ld-linux-aarch64.so.1`) is **already in
`tiny-rootfs.tar`** (verified present). No fontconfig cache, no
`FONTCONFIG_PATH`, no `XDG_DATA_DIRS`, no extra `.so`s — the AA vector text is
drawn from in-binary path geometry, not a system font.

**The only addition is the binary itself**, placed exactly like `/bin/alr-wl-test`
(mode `0755`, uid/gid `0`):

```
./bin/alr-pixman-test     ← /tmp/alrbins/alr-pixman-test
```

A pre-staged tar with this entry already added (existing 217 entries intact,
`alr-wl-test` unchanged, +1 entry) is at
`/tmp/alrbins/tiny-rootfs.with-pixman.tar`
(size 37140480, sha256
`a57b01ca0173d33363abf9634f1d8e79b11f802ceecaffd89c2fefa95ed95d30`).

## Lead's ship + invoke steps

1. **Add the binary to the rootfs tar** (both copies — `rootfs/tiny-rootfs.tar`
   is the source of truth, `app/src/main/assets/rootfs/payloads/tiny-rootfs.tar`
   is the shipped copy). Either copy the pre-staged tar above over both, or
   re-append with canonical metadata, e.g.:

   ```python
   import tarfile, os
   with tarfile.open("tiny-rootfs.tar", "a") as tf:
       ti = tarfile.TarInfo("./bin/alr-pixman-test")
       ti.size = os.path.getsize("/tmp/alrbins/alr-pixman-test")
       ti.mode, ti.uid, ti.gid = 0o755, 0, 0
       with open("/tmp/alrbins/alr-pixman-test","rb") as f: tf.addfile(ti, f)
   ```
   (macOS `tar` is bsdtar; GNU `--owner=0 --group=0 -rf` is unavailable, hence
   the tarfile snippet.)

2. **Update the manifest** `rootfs/manifests/debian-arm64-bookworm-slim.json`:
   bump the `tiny-rootfs.tar` asset `sha256` + `size_bytes` to the new tar's,
   and bump `version` (this is a version-stamp pin site — see the memory note
   *version-stamp-pin-sites*; align with the APK build stamp bump).

3. **Point the Wayland first-window guest at the new program.** The existing
   first-window path runs the guest via the in-process native loader with
   `guest_argv[0]` as the guest path. Change that program from
   `/bin/alr-wl-test` to **`/bin/alr-pixman-test`** (in `runtime_report.cpp`,
   owned by the lead — the same call site that today launches `alr-wl-test`).
   No CMake/MainActivity change.

4. **Guest env: unchanged.** The loader already injects everything needed:
   `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=<cacheDir>/alr-xdg`,
   `LD_LIBRARY_PATH=<rootfs>/lib/aarch64-linux-gnu:…`,
   `GLIBC_TUNABLES=glibc.pthread.rseq=0` (+ the harmless `GDK_BACKEND`/
   `SDL_VIDEODRIVER`). pixman uses **no** env vars. No `FONTCONFIG_PATH` /
   `XDG_DATA_DIRS` needed.

### Expected device result (mirrors the v82 log, with a real toolkit frame)

```
surface committed shm: 480x320 stride=1920 fmt=1   (XRGB8888)
guest stdout:
  alr-toolkit: pixman drew frame (480x320 released=1 frame_done=1)
```

The SurfaceView should show the blue→teal gradient with two AA discs and a white
"ALR" — a real 2D toolkit's output, composited by the in-app compositor.

## Deferred: the heavier real-toolkit paths (later rungs)

### Cairo-on-`wl_shm` (next, builds on this exact seam)
Stack the Cairo API on the already-working pixman. Image-only Cairo ≈ 95 `.c`
files; needs a generated `cairo-features.h` + `config.h` and `-I` onto these
pixman headers (Cairo's image backend *is* pixman). Use the built-in
**`cairo-font-face-twin`** for real `cairo_select_font_face` + `cairo_show_text`
text with **no FreeType/fontconfig**. Same "compile the lib into the client"
model → still only `libc`/`libm`/`libffi` at runtime, still zero rootfs libs.
Payoff: the convenient `cairo_t` path API (real `cairo_arc`, `cairo_*_gradient`,
`cairo_show_text`).

### SDL2 (software renderer → `wl_shm`)
A tiny `SDL_Init(VIDEO)` + `SDL_CreateWindow` + `SDL_CreateRenderer(…,
SDL_RENDERER_SOFTWARE)` + fill + `SDL_RenderPresent` client. Unlike pixman/Cairo,
SDL2 is awkward to fully static-embed; the realistic route is **provisioning the
`.so` closure into the rootfs** (full transitive set, taken from the matching
Debian/Ubuntu arm64 packages):
`libSDL2-2.0.so.0`, and for its Wayland video backend
`libwayland-client.so.0`, `libwayland-cursor.so.0`, `libwayland-egl.so.1`
(only if GL — not needed for the software renderer), `libxkbcommon.so.0`,
`libdecor-0.so.0` (client-side decorations) and their deps (`libffi` already
present). Force the software path so no `wl_egl`/GL is required:
`SDL_VIDEODRIVER=wayland` (already injected) **+ `SDL_RENDER_DRIVER=software`**
(or `SDL_FRAMEBUFFER_ACCELERATION=0`). SDL2 then presents via `wl_shm`. This is
the first rung that requires real rootfs library provisioning + an
`ldconfig`/`LD_LIBRARY_PATH` that resolves the SDL closure.

### GTK3 (full toolkit)
Only once a GL-less GTK is acceptable: `GDK_BACKEND=wayland` (injected) +
`GDK_RENDERING=cairo` so GDK uses the Cairo software renderer. Requires the full
~30-lib closure (gtk-3/gdk-3/glib/gobject/gio/gmodule/pango/pangocairo/cairo/
gdk-pixbuf/harfbuzz/fontconfig/freetype/pixman/libxkbcommon/libepoxy/libpng/
libz/…) **plus a built fontconfig cache** under `/var/cache/fontconfig` (+ at
least one font and `FONTCONFIG_PATH=/etc/fonts`) or text won't shape. Heaviest;
out of scope until the closure-provisioning + fontconfig story exists.

## Files

* binary:  `/tmp/alrbins/alr-pixman-test` (0755)
* source:  `/tmp/alrbins/alr-pixman-test.c`
* build:   `/tmp/alrbins/build-alr-pixman-test.sh`
* pixman:  `/tmp/alrbins/src-cairo/pixman/` (sources + `config.h` + `pixman-version.h`)
* staged tar (binary added): `/tmp/alrbins/tiny-rootfs.with-pixman.tar`
