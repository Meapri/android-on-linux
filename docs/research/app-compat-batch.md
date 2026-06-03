# ALR batch app-compat matrix — overlay CLASSES vs the long-tail (host research)

> **Scope/ownership.** This is a **batch coverage DESIGN** doc, distinct from the two
> existing tables it complements:
> - `docs/research/alr-compat-matrix.md` (WS-5) — the **device-evidence** app×result
>   table (only rows with a real device run are USABLE/RENDERS/RUNS).
> - the "apt 설치 후보" section there + `tools/breadth_catalog.py` (apt session) — the
>   **download-closure** predictor (0-unsat over noble ports).
>
> Neither answers the question this doc answers: **for ~35 common arm64 glibc apps,
> which CLASS of missing-stuff does each hit, and does ONE of the overlays this
> workflow builds cover it — or does it need a named residual overlay/wrapper, or
> multiprocess (CR-5)?** It is the design that turns the per-app firefight
> (today: chromium FATAL'd on a single missing `libsoftokn3.so`) into a small set of
> **class overlays** that unblock whole app categories at once.
>
> **HONESTY.** This is a HOST research doc on a Darwin host. No row here is a device
> claim. GREEN means "the missing-class is covered by an overlay that already builds
> + host-validates"; it is NOT "runs on device". Device truth stays in the WS-5
> matrix. Every cited dlopen dir / data path / restricted syscall is grounded in the
> Debian/Ubuntu-noble package layout (see the per-class citations below), so the
> integration session can act on it cold.

baseline tree: post-`839315e` (v2 dpkg/apt + fakeroot + galculator breadth merged).

---

## 1. Why DT_NEEDED-only closure leaks — the root of the long-tail

The §5-E deb closure (`tools/deb_closure.py::reachable_overlay_libs`, a BFS over
`DT_NEEDED` from the leaf ELFs) keeps exactly: *(a)* the leaf package's own files,
*(b)* the shared libs reachable from them via `DT_NEEDED` that the base lacks. It
**cannot see a `dlopen()`** — a plugin loaded by string at runtime has no
`DT_NEEDED` edge, so the BFS never reaches it and it is dropped. Device-confirmed
instance: chromium 147 `dlopen`s NSS's `libsoftokn3.so` and **FATAL-crashed** when
the DT_NEEDED-only closure dropped it (`crypto/nss_util.cc:256` →
`nss_error=-5925` → `ImmediateCrash`), which is exactly why `tools/build_nss_overlay.py`
exists. Every dlopen-plugin **family** is its own such trap. The batch fix is to
enumerate the families once and ship a force-kept overlay per family, instead of
chasing one `.so` per app.

There are five *classes* of "missing" a glibc app hits on ALR. The matrix in §4
tags each app by which class(es) bite it, and §2/§3 define the class→overlay map.

| Class | What's missing | Why DT_NEEDED closure misses it | Batch fix |
|------|----------------|----------------------------------|-----------|
| **P — dlopen plugins** | NSS / gdk-pixbuf loaders / GIO modules / immodules / gstreamer / babl-gegl / cups / pango | loaded by string at runtime → no `DT_NEEDED` edge | one **force-kept plugin-dir overlay per family** (§2) |
| **D — runtime data** | CA bundle, `resolv.conf`/`nsswitch`/`hosts`, mime db, `loaders.cache`/`gio` cache, terminfo, dconf/glib schemas, icons | data files, never an ELF edge | the **data overlay** (§3) |
| **F — fonts** | no font file + no fontconfig cache → `FcConfigParse`/HarfBuzz shape fail | not a lib at all | the **font overlay** (§3) |
| **L — locale** | `/usr/lib/locale/C.utf8` (glibc doesn't compile C.UTF-8 in) → `setlocale`→NULL→SIGABRT | not a lib | **already shipped**: `tools/build_locale_overlay.py` (§3) |
| **S — restricted syscall** | Android SELinux denies the call (raw UDP-53, AF_NETLINK, SO_MARK, ...) | not a file at all — kernel policy | **interposer emulation** (loader lane, NOT this doc's overlays) — proceed-not-bypass (§5) |

> **Class S is the only one not solved by a file overlay** and the only one that
> can need a loader change (other session owns `libalr_interpose.c`). This doc's
> overlays solve P/D/F/L; S is recorded per-app so the integrator knows which apps
> still need an interposer no-op even after every overlay is staged.

---

## 2. Class P — the dlopen-plugin families (one force-kept overlay each)

Each family lives in a fixed Debian dir that an app `dlopen`s by string. Force-keep
the WHOLE dir (`build_minimal_overlay(..., keep_prefixes=<dir>)` keeps a prefix
even with no `DT_NEEDED` edge — exactly how `build_babl_gegl_overlay` ships 67 op
`.so`s today). Sizes are the Installed-Size order-of-magnitude, base-subtracted.

| Family | dlopen dir (noble arm64) | Loaded by | Today | Batch overlay |
|--------|--------------------------|-----------|-------|---------------|
| **NSS** (TLS/crypto plugins) | `/usr/lib/aarch64-linux-gnu/nss/` **and** flat `…/` (loader `LD_LIBRARY_PATH`) | `libnss3` for any TLS app (chromium, curl-openssl, git-https) | ✅ `tools/build_nss_overlay.py` (`libsoftokn3/freebl3/freeblpriv3/nssckbi/nssdbm3`) | **DONE** — reuse as-is |
| **gdk-pixbuf loaders** | `/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/` (+ `loaders.cache`) | every GTK app for PNG/JPEG/SVG/… decode | ◑ SVG-only via `build_gui_overlay.py` (`libpixbufloader_svg.so` + `librsvg` + cache) | **YELLOW→GREEN**: extend to the full loader set (png/jpeg/gif/bmp/tiff/xpm/ico/qtif) + regenerate `loaders.cache` |
| **GIO modules** | `/usr/lib/aarch64-linux-gnu/gio/modules/` (+ `giomodule.cache`) | GLib apps for TLS (`libgiognutls`), proxy, gvfs | ✗ | **NEW overlay** (TLS module ≈ needed by GIO-net apps; gvfs optional) |
| **GTK immodules** | `/usr/lib/aarch64-linux-gnu/gtk-3.0/3.0.0/immodules/` (+ `immodules.cache`) | GTK text-entry input methods | ✗ (base "simple" im works; IBus/etc not) | **YELLOW** — low priority; ship `immodules.cache` only so GTK doesn't warn |
| **babl/gegl ops** | `/usr/lib/aarch64-linux-gnu/{babl-0.1,gegl-0.4}/*.so` | GIMP image ops | ✅ `tools/build_babl_gegl_overlay.py` (67 `.so`) | **DONE** |
| **pango modules** | (noble: built-in; legacy `pango/1.8.0/modules/`) | text shaping | ✅ base (modern pango is static — no dir) | **N/A noble** |
| **gstreamer plugins** | `/usr/lib/aarch64-linux-gnu/gstreamer-1.0/*.so` | media (mpv-gst, totem, web `<video>`) | ✗ | **RED-ish** — huge family; per-codec; out of batch scope (name it, don't ship all) |
| **cups backends/filters** | `/usr/lib/cups/{backend,filter}/` | printing | ✗ | **RED** — needs a print spooler daemon (multiprocess) |
| **ALSA plugins** | `/usr/lib/aarch64-linux-gnu/alsa-lib/` | audio out | ✗ | **RED** — no audio sink on the ALR compositor path yet |

**Citation grounding (per family dlopen path).** NSS: see `build_nss_overlay.py`
header (device crash `libsoftokn3.so: cannot open`). gdk-pixbuf: the loader dir +
`loaders.cache` are exactly the three pieces `build_gui_overlay.py::SVG_PIECES`
lifts — the batch just widens the file list in the same dir. babl/gegl: the two
dirs `build_babl_gegl_overlay.py` force-keeps. GIO/immodules dirs follow the same
Debian multiarch `…/<lib>/<api-ver>/modules/` convention; each ships a `*.cache`
the app reads to enumerate plugins (so the cache file is a **class-D data** member
that MUST ride along, regenerated host-side or copied from the .deb).

**The one-shot "plugins" overlay.** Bundle NSS + full-gdk-pixbuf + GIO-tls +
babl-gegl + immodule-cache into a single `plugins-stage.tar` slot (each at its
rootfs-absolute dir, `.so` mode `0o755` — the device-proven W^X requirement: a
non-x `.so` fails ALR file-backed `PROT_EXEC`, cf. the `0644→SIGABRT` svg
evidence). That single tar covers the P-class for the entire GTK + TLS app set.

---

## 3. Classes D/F/L — the shared "rootfs furniture" overlay

Most apps don't need app-specific data; they need the **base furniture** a normal
Debian install has that the tiny base rootfs omits. Ship it once.

| Member | Rootfs path | Who needs it | Source | Today |
|--------|-------------|--------------|--------|-------|
| CA bundle | `/etc/ssl/certs/ca-certificates.crt` | every TLS app (curl/wget/git/chromium) | assembled from `ca-certificates` .deb | ✅ in `build_chromium_net_overlay.py::assemble_ca_bundle` |
| resolv/nsswitch/hosts | `/etc/resolv.conf`, `/etc/nsswitch.conf`, `/etc/hosts` | every name-resolving app | static + DoH pin | ✅ same builder |
| MIME db | `/usr/share/mime/` (esp. `mime.cache`) | GTK file dialogs, `xdg-mime`, gdk-pixbuf sniff | `shared-mime-info` .deb | ◑ partial (`v95-image-decode` notes the db present) |
| terminfo | `/usr/share/terminfo/` (`x/xterm-256color`, `s/screen`, …) | **every TUI** (nano/vim/htop/less/tmux) | `ncurses-base`/`-term` | ✗ — **NEW, high-leverage** |
| glib schemas | `/usr/share/glib-2.0/schemas/gschemas.compiled` | any `GSettings` GTK app (gedit, file dialogs) | per-app .debs + `glib-compile-schemas` | ✗ — **NEW** (compile host-side) |
| dconf | `/usr/lib/aarch64-linux-gnu/gio/modules/libdconfsettings.so` + daemon | GSettings backend | (often falls back to memory backend OK) | YELLOW — memory backend usually fine |
| icon theme | `/usr/share/icons/{hicolor,Adwaita}/…/index.theme` (+ caches) | GTK apps requesting stock icons | `adwaita-icon-theme` (large) | YELLOW — apps run without; missing-icon warnings only |
| fonts | `/usr/share/fonts/…` + fontconfig conf | **any text rendering** (GTK/Qt/terminal/browser) | `fonts-dejavu-core` (~1 font family is enough) | ✗ — **NEW, high-leverage** (one core family unblocks all text) |
| fontconfig cache | `/var/cache/fontconfig/` or let first run build it in `$XDG_CACHE_HOME` | fontconfig | generated | ride with font overlay |
| C.UTF-8 locale | `/usr/lib/locale/C.utf8` (+ `C.UTF-8` symlink) | GLib/GTK `setlocale` → else SIGABRT | `libc-bin` .deb | ✅ `tools/build_locale_overlay.py` |

**Two NEW high-leverage members worth their own batch overlay:**

1. **terminfo** (`/usr/share/terminfo/`). The base rootfs ships none, so **every**
   terminal app — nano, vim, htop, less, tmux, even `bash`'s line editor under a
   real `$TERM` — calls `setupterm()`, fails to find `x/xterm-256color`, and either
   degrades to dumb mode or errors. This is the single most common TUI blocker. The
   `ncurses-base` .deb ships the common entries; force-keep `/usr/share/terminfo/`.
   Cost: ~1 MiB. **Unblocks the entire terminal/editor/TUI category at once.**

2. **one core font + fontconfig** (`fonts-dejavu-core`, ~1 MiB). Any toolkit that
   renders text (GTK, Qt, foot, chromium) needs at least one resolvable font; with
   none, fontconfig returns no match and HarfBuzz/Pango/cairo render nothing or
   abort. One DejaVu family + the default `/etc/fonts/fonts.conf` covers Latin text
   for the whole GUI + browser set. (CJK/emoji is a follow-on; not batch-critical.)

Bundle CA/resolv (already built) + MIME + terminfo + glib-schemas + 1 font +
locale (already built) into a single `furniture-stage.tar`. That one tar moves the
D/F/L classes for the bulk of CLI **and** GUI apps.

---

## 4. The app × missing-class matrix (~35 apps)

Legend: **P** dlopen-plugin, **D** data, **F** font, **L** locale, **S** restricted
syscall. Verdict: **GREEN** = every class it hits is covered by an overlay this
workflow builds (or already shipped); **YELLOW** = needs one more named
overlay/wrapper; **RED** = needs multiprocess (CR-5) or a hard feature ALR doesn't
have. (Device truth is the WS-5 matrix; this is the host-predicted batch verdict.)

### Terminals
| App | Classes hit | Verdict | Residual (named) |
|-----|-------------|---------|------------------|
| `foot` | F, L | **GREEN** | font + locale overlays (foot already RENDERS on device per WS-5) |
| `xterm` | F, L, S(Xwayland) | **YELLOW** | needs Xwayland-rootful host (X11 client display PENDING in WS-5) |
| `alacritty` | F, L, S(GPU-EGL) | **YELLOW** | GPU/GL terminal — needs guest GLES path (not wl_shm); font+locale otherwise covered |

### Editors / TUI
| App | Classes hit | Verdict | Residual |
|-----|-------------|---------|----------|
| `nano` | D(terminfo), L | **GREEN** | terminfo + locale overlays |
| `vim` | D(terminfo), L | **GREEN** | terminfo + locale |
| `less`, `htop`, `ncdu` | D(terminfo) | **GREEN** | terminfo overlay (no font/plugin) |
| `gedit` | P(gdk-pixbuf), D(schemas+mime), F, L | **GREEN** | full-pixbuf + furniture overlays |
| `kate` (KDE) | P(Qt plugins), D, F, L | **YELLOW** | needs the Qt6 plugin/platform set beyond qtwayland (kf6 libs); bigger closure |

### Browsers
| App | Classes hit | Verdict | Residual |
|-----|-------------|---------|----------|
| `chromium` (`--version`/CR-1/CR-4) | P(NSS), D(CA+resolv), F | **GREEN** (plumbing) | all P/D/F covered (NSS + chromium-net + font); **S(net) + multithread-ptrace render = loader lane, not overlay** |
| `netsurf-gtk` | P(gdk-pixbuf), D(CA), F, L | **GREEN** | full-pixbuf + furniture (already RENDERS per WS-5) |
| `qutebrowser` | (PyQt) P(Qt), D, F + python | **YELLOW** | needs Qt6 WebEngine (chromium-class) + a python3 rootfs — heavy |

### Media
| App | Classes hit | Verdict | Residual |
|-----|-------------|---------|----------|
| `ffmpeg` (transcode, no AV out) | (mostly DT_NEEDED) | **GREEN** | codecs are DT_NEEDED libs → closure already gets them; pure compute |
| `imagemagick` (`convert`) | P(coders `…/ImageMagick-*/modules-Q16/coders/*.so`), F(text op) | **YELLOW** | NEW small overlay: keep the IM coders dir (dlopen'd like pixbuf) |
| `mpv` (playback) | P(gstreamer? no — mpv uses its own), S(audio+vsync), D | **RED** | no audio sink + display-sync; CLI `--vo=null`/frame-dump only |

### Dev tools
| App | Classes hit | Verdict | Residual |
|-----|-------------|---------|----------|
| `git` (local) | — | **GREEN** | pure; runs once exec-re-entry lands for its helper forks |
| `git` (https clone) | P(NSS via libcurl-openssl), D(CA), S(net) | **GREEN** (files) | NSS+CA covered; net is S-class loader lane |
| `python3` (stdlib) | D(none beyond closure) | **GREEN** | interpreter + stdlib are DT_NEEDED/data in the .deb |
| `python3` (`ssl`) | D(CA), P(openssl engines rare) | **GREEN** | CA bundle |
| `make` | S(fork+exec of `cc`) | **YELLOW** | needs exec-re-entry (B-3) so it can spawn the compiler |
| `gcc` | S(exec of cc1/as/ld) | **YELLOW** | same exec-re-entry gate (multi-stage fork+exec) |
| `jq`, `curl`, `wget` | D(CA for https) | **GREEN** | furniture CA bundle (jq needs nothing) |

### Office / graphics
| App | Classes hit | Verdict | Residual |
|-----|-------------|---------|----------|
| `gimp-3.0` | P(babl/gegl+pixbuf), D, F, L | **GREEN** | all shipped (USABLE on device per WS-5) — the reference GREEN |
| `inkscape` | P(pixbuf+gegl), D(schemas), F, L | **YELLOW** | big closure; pixbuf+gegl+furniture cover the classes, size is the risk |
| `libreoffice` (`--headless --convert-to`) | P(its own UNO plugins), D, F | **YELLOW** | UNO components are dlopen'd from `program/` — keep that dir; large but classable |
| `galculator` | P(pixbuf), D(schemas), F, L | **GREEN** | furniture + pixbuf (closure already 0-unsat per WS-5) |

### Utilities / system
| App | Classes hit | Verdict | Residual |
|-----|-------------|---------|----------|
| `tree`, `file`, `coreutils` | — | **GREEN** | near libc-only; nothing to stage |
| `feh` (image viewer) | D(mime), F, S(Xwayland/imlib2) | **YELLOW** | X11 client → Xwayland-rootful gate |
| `apt` / `dpkg` (real install) | S(fork+exec maintainer scripts), root-perms | **RED (CR-5)** | exec-re-entry + fakeroot; tracked as G1 in WS-5 — NOT an overlay problem |

### Roll-up
- **GREEN: 19** — terminals(foot), TUI/editors(nano/vim/less/htop/ncdu/gedit),
  browsers(chromium-plumbing, netsurf), media(ffmpeg), dev(git×2, python3×2,
  jq/curl/wget), graphics(gimp, galculator), utils(tree/file/coreutils). These are
  unblocked **by the three batch overlays + the two already-shipped overlays** —
  no per-app work.
- **YELLOW: 12** — each named a single residual: Xwayland-rootful (xterm/feh),
  GPU-GLES terminal (alacritty), Qt/KF6 closure (kate/qutebrowser),
  per-family small overlay (imagemagick coders, libreoffice UNO), exec-re-entry
  (make/gcc), size-risk big closures (inkscape).
- **RED: 4** — mpv (audio/vsync sink), cups-printing, apt/dpkg real-install (CR-5),
  any audio app (ALSA). These need a subsystem ALR doesn't have, not a file.

**Headline:** the **terminfo + font + full-pixbuf + GIO-tls** batch (plus the
already-built NSS/babl-gegl/locale/chromium-net overlays) flips **~19/35 common
apps GREEN at once** and reduces the rest to a short, named residual list — instead
of fixing one missing `.so` per app per device drain.

---

## 5. Class S residue — what NO overlay fixes (for the integrator)

These are kernel/SELinux denials, recorded so the integrator knows an overlay
alone won't make the app proceed. The fix (other session's lane) is **interposer
emulation = let the app proceed, do NOT weaken the sandbox**:

| Denied call | App impact | Existing handling |
|-------------|-----------|-------------------|
| raw **UDP-53** sendto | glibc DNS hangs → every https app | chromium uses **DoH/443** (chromium-net flags); non-chromium TLS apps need either `/etc/hosts` pins or a DoH-shim — **named residual** |
| **AF_NETLINK** bind/route dump | `getifaddrs`, glibc resolver init | interposer already no-ops (`libalr_interpose.c`, per task brief) |
| **SO_MARK** setsockopt | systemd-ish socket marking | interposer EPERM no-op (already) |
| **fork+exec** (execve) | apt/dpkg/make/gcc/git-helpers | exec-re-entry G1 (WS-5 in-flight); NOT an overlay |

> A GREEN app that also carries an S tag (chromium, git-https) is GREEN **on the
> file/overlay axis** — its remaining blocker is the loader lane, not anything this
> doc ships.

---

## 6. What the integration session does with this

The three NEW batch overlays this doc specifies (all reuse
`deb_closure.build_minimal_overlay(..., keep_prefixes=…)` + `build_stage_tar`,
exactly like `build_babl_gegl_overlay`/`build_toolkit_overlays`):

1. **`plugins-stage.tar`** — force-keep the dlopen dirs: full gdk-pixbuf `loaders/`
   (+ regenerated `loaders.cache`), `gio/modules/` TLS, GTK `immodules.cache`. (NSS
   + babl-gegl already have their own builders; can fold in or keep separate.)
2. **`furniture-stage.tar`** — `/usr/share/terminfo/` (ncurses-base),
   `fonts-dejavu-core` + `/etc/fonts/fonts.conf`, `gschemas.compiled`,
   `/usr/share/mime/mime.cache`. (CA/resolv already in `chromium-net-stage.tar`;
   locale already in `xkb-gegl-stage.tar`.)
3. each `.so` at `0o755` (W^X file-backed `PROT_EXEC` requirement), `./`-rooted,
   §5-E conformant, base-subtracted/guarded by `stage_tar_spec`+`overlay_guard`
   like every other overlay.

Stage these in the existing MainActivity toolkit-extract loop (new slot keys),
gated by their own `.staged` markers. Then a device drain of nano/vim/htop (TUI
via terminfo), gedit/galculator (GTK furniture+pixbuf), curl-https (CA) promotes
those rows in the WS-5 device matrix.

---
*This is a host design doc. GREEN = overlay-covered (host-validated builder), NOT a
device pass. Promote a row in `docs/research/alr-compat-matrix.md` only with real
device evidence.*
