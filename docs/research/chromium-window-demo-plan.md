# CR-4 — Chromium browser-window DEMO plan (clean on-screen, offline)

**Lane:** integration HOST design (read-only on app sources). **Owns:** this file
+ `tools/chromium/demo.html`. No device run (host-only worker).
**Date:** 2026-06-03. **Tree HEAD:** 2fdd20f (`main`).

## 0. Scope & relation to the existing audit

The companion doc `docs/research/chromium-ozone-wayland.md` already proves the
**Wayland-globals** question: every chromium ozone-wayland *hard-fail* global is
advertised at an accepted version (see its §2). **This doc does not re-litigate
that.** It is the *execution / demonstration* layer:

1. Re-confirm the advertised globals against the **live compositor code**
   (ground-truth, §1) and pin the chromium-needs-vs-advertise gap table with a
   hard/soft verdict — including **two gaps the globals audit did not surface**
   because they are not registry items: the **binary build target** (§2) and a
   **missing `OZONE_PLATFORM` env** (§3).
2. A **clean-demo flag matrix** for a stable *software* window (§4).
3. **Present / input** notes verified against the present path actually wired in
   `runtime_report.cpp` (§5) — including a **correction** to the globals doc: the
   `present_list` EGL hook is **already installed** (not a TODO).
4. The **demo asset** `tools/chromium/demo.html` (§6).
5. **CR-4 DEVICE-REQ** stepA (window present) / stepB (input) for the device
   track to execute (§7).

**Honesty convention used throughout:** `[CODE]` = verified against a file at the
cited line in this tree; `[INFER]` = reasoned from Chromium upstream behavior /
build-target knowledge, **not** verified on device. Every `[INFER]` is something
the device track resolves in one run.

---

## 1. What the compositor advertises — re-verified against live code `[CODE]`

`app/src/main/cpp/alr_wayland/alr_compositor.cpp::register_globals()` (L1921–1942)
+ the version constants at L72–80. (App sources read-only; values quoted, not
changed.)

| Global | Version | Where |
|---|---|---|
| `wl_compositor` | **4** (`kCompositorVersion` L72) | `wl_global_create` L1922 |
| `wl_shm` | 1 (`kShmVersion` L73) | `wl_display_init_shm()` L1976 |
| `wl_seat` | **7** (`kSeatVersion` L74) | L1924 |
| `wl_output` | **4** (`kOutputVersion` L79) | L1926 |
| `xdg_wm_base` | **2** (`kXdgWmBaseVersion` L80) | L1928 |
| `wl_subcompositor` | 1 | L1930 |
| `wl_data_device_manager` | 3 | L1933 |

`zwp_linux_dmabuf_v1` is **deliberately NOT advertised** (the big comment at
L1883–1920 + L1910): wl_shm-only forces chromium onto software buffers so the AHB
presenter owns the copy-to-SurfaceView. Do not add it.

Vendored protocol glue (`third_party/wayland_generated/`): **only** core `wayland`
+ `xdg-shell`. Any *new* interface therefore costs XML → regenerate glue → CMake —
the "needs WS-3 work" wall.

---

## 2. Gap #1 (HARD, the real CR-4 risk): the binary is `chromium-headless-shell` `[CODE]`+`[INFER]`

This is the gap the globals audit cannot see, because it is not a registry item.

`[CODE]` Every chromium probe on device launches
**`/usr/lib/chromium/chromium-headless-shell`**:
`MainActivity.kt` L86 (`--version`), L108 (CR-1 `--dump-dom`), L138 (CR-2),
L180 (CR-5 multiprocess). `tools/alr_compat.py` L99 and the device evidence
`docs/evidence/2026-06-01-device-SM-X236N-chromium-runs-inprocess.md` L11/L20
confirm the staged binary is **`chromium-headless-shell`** (Debian bookworm
chromium 147.0.7727.137, 186 MB, ET_DYN).

`[INFER]` **`chromium-headless-shell` is Chromium's `headless_shell` build
target.** That target is compiled with Ozone restricted to the **headless**
platform; it ships the `--ozone-platform=headless` backend (with
`--ozone-dump-file=out.png` for offscreen PNG), and it is the binary the project's
own `chromium-native-plan.md` §3 (L103) names for **Stage 1 (offscreen)** — vs
L106 "Stage 2 (on-screen): `--ozone-platform=wayland`". The strong likelihood is
that **`--ozone-platform=wayland` on `chromium-headless-shell` fails** with
`Unsupported Ozone platform: wayland` / a null platform → **no window** — because
the wayland Ozone backend is not built into the headless_shell target.

**Verdict: HARD for CR-4.** A *headless* shell cannot open an on-screen window no
matter how complete the registry is. **CR-4 needs a Chromium binary built WITH the
ozone-wayland platform**, i.e. one of:

* **Full `chrome`** (the browser) — the project already models its self-exec as
  `<rootfs>/opt/chromium/chrome` (`tools/proc_self_exe_model.py` L232,
  `chromium-multiprocess-plan.md` L209). The Debian `chromium` package's main
  binary includes ozone-wayland. **Preferred** (it is *the browser window* the
  demo wants), at the cost of staging the full `chromium` package (heavier than
  the headless-shell closure).
* **`content_shell` (`--ozone-platform=wayland`)** — Chromium's minimal shell
  *with* the full Ozone platform set; lighter than full chrome, still opens a real
  wayland window. Good middle ground if full-chrome closure is too big.

**This must be settled before stepA can pass.** §7 stepA-0 makes it the first
device check. *If* a headless-shell variant in this Debian build happens to
include ozone-wayland (some distro builds enable extra Ozone platforms), the rest
of this plan applies unchanged to it; the point is the device run reveals it in
one line.

---

## 3. Gap #2 (SOFT, one-line fix): no `OZONE_PLATFORM=wayland` in guest env `[CODE]`

`[CODE]` `runtime_report.cpp` L1518–1574 builds `guest_env` injected into **every**
guest. It already sets everything GIMP/Qt/SDL need —
`WAYLAND_DISPLAY=wayland-0` (L1535), `XDG_RUNTIME_DIR=<cacheDir>/alr-xdg` (L1534),
`GDK_BACKEND=wayland` (L1536), `QT_QPA_PLATFORM=wayland` (L1541),
`SDL_VIDEODRIVER=wayland` (L1537) — **so chromium already inherits
`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`.**

> **Correction to `chromium-ozone-wayland.md` §3 "G-1".** That doc lists the guest
> env (`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`) as an open blocker "the same env
> blocker GIMP had." On this HEAD it is **already wired** at L1534–1535 and reaches
> chromium. G-1 is **closed** for the connect side. What remains is only the
> chromium-*specific* platform selector below.

**The gap:** chromium does **not** read `GDK_BACKEND`/`QT_QPA_PLATFORM`/
`SDL_VIDEODRIVER`. Its Ozone platform selector is **`OZONE_PLATFORM`** (or
`--ozone-platform=` on argv), and **`OZONE_PLATFORM` is absent** from the L1518
block. Without it, and without the argv flag, ozone auto-selects its default
(headless on the headless-shell; possibly X11 on full chrome → no X server →
abort/headless).

**Two ways to close it (either suffices; both = belt-and-braces):**
* **argv (no app-code edit):** pass `--ozone-platform=wayland` in the
  newline-delimited argv the device track already constructs (it builds these
  strings in `MainActivity.kt`; the demo launch string in §4 includes it).
* **env (app-code edit, device track owns):** add
  `guest_env.push_back("OZONE_PLATFORM=wayland");` next to L1541 in
  `runtime_report.cpp`. **Location only — this lane does not edit app code.**
  Harmless to non-chromium guests (they ignore `OZONE_PLATFORM`).

**Verdict: SOFT.** Fully covered by the `--ozone-platform=wayland` argv flag in the
launch command, so it is not strictly a blocker; the env add is a robustness nicety.

---

## 4. Clean-demo flag matrix — stable *software* window, offline `[CODE-flags]`+`[INFER-effect]`

Goal: a borderless, maximized, **software-rastered** chromium window that presents
`wl_shm` frames (so our AHB/SurfaceView presenter composites), loading a **local**
page (no DNS/TLS). Flags below reuse the project's proven CR base
(`MainActivity.kt` L108–112, `chromium/README.md`) and add only window/ozone bits.

| Flag | Why (for the clean demo) | Tier |
|---|---|---|
| `--ozone-platform=wayland` | Select the Wayland Ozone backend → bind our compositor. **Required** (§3). Needs a binary that *has* this backend (§2). | **HARD** |
| `--no-sandbox` | The ALR loader supervises syscalls; chromium's own sandbox (user-ns/seccomp) can't nest here. Already used by every CR probe. | **HARD** |
| `--disable-gpu` | No `/dev/dri`/dmabuf; forces software raster so frames arrive as `wl_shm` (the path the presenter handles). **Keeps swaps on shm.** | **HARD (for shm path)** |
| `--disable-dev-shm-usage` | `/dev/shm` is absent/limited; use temp files instead. Used by every CR probe. | HARD-ish |
| `--user-data-dir=/tmp/cr4-profile` | A writable, throwaway profile dir (no first-run state in a read-only spot). | HARD |
| `--no-first-run` | Skip the first-run setup UI/flow → straight to the page. | soft (cleanliness) |
| `--no-default-browser-check` | Suppress the "set as default" prompt. | soft |
| `--start-maximized` | Fill the SurfaceView (we are a single-output borderless compositor). | soft |
| `--window-size=1200,1920` | Match the device panel (portrait). The compositor also fits to `wl_output`; this hints chromium's initial size. Use the real panel px. | soft |
| `--disable-crash-reporter` | No crashpad upload attempts (network + extra threads). | soft |
| `--enable-logging=stderr --v=1` | Route chromium logs to stderr so the device drain captures bind/map/commit lines. | soft (debug) |
| **URL** `file:///root/demo.html` | **Offline** local page (§6). `data:text/html,...` also works; `file://` keeps the URL bar clean for the clip. **No `http(s)://`** (would need CR-2 DNS/TLS). | **HARD (offline)** |

**Single-process vs multiprocess for the *window* demo `[INFER]`:** the headless CR
probes use `--single-process`. For an on-screen browser, the **renderer-in-a-child
multiprocess** path is already device-proven (CR-5 stepA, evidence
`2026-06-02-cr1...`/multiprocess plan) via the in-process re-exec (GATE-1 self-exe
substitute, `--no-zygote --renderer-process-limit=1`). Either works for CR-4;
**recommend starting `--single-process`** to remove the child-exec variable from
the *first* window bring-up, then switch to multiprocess (drop `--single-process`,
keep `--no-zygote --renderer-process-limit=1`) once a window is confirmed. The GPU
*child* is a separate track (CR-3) and stays disabled here via `--disable-gpu`.

**Composite launch string (newline-delimited argv, the device track's format):**

```
/<chromium-with-wayland>            # full chrome or content_shell — NOT headless-shell (§2)
--ozone-platform=wayland
--no-sandbox
--single-process                    # drop later for multiprocess; then add --no-zygote --renderer-process-limit=1
--disable-gpu
--disable-dev-shm-usage
--user-data-dir=/tmp/cr4-profile
--no-first-run
--no-default-browser-check
--disable-crash-reporter
--start-maximized
--window-size=1200,1920
--enable-logging=stderr
--v=1
file:///root/demo.html
```

with guest env already providing `WAYLAND_DISPLAY=wayland-0` +
`XDG_RUNTIME_DIR=<cacheDir>/alr-xdg` (L1534-1535); optionally also
`OZONE_PLATFORM=wayland` (§3).

---

## 5. Present & input notes — verified against the wired path `[CODE]`

### 5.1 The present path is FULLY wired (correction to the globals doc)

> **Correction to `chromium-ozone-wayland.md` §3/§5 and the `present_buffer()`
> "EGL WIRING TODO" comment (`alr_compositor.cpp` L2543).** Those describe the
> present hook as a TODO the lead must install. On this HEAD the EGL `present_list`
> hook is **already implemented and installed**:

`[CODE]` `runtime_report.cpp`:
* `g_wl_presenter.present_list(...)` — full implementation at **L6020–6137**:
  clears once, iterates the z-ordered `PresentSurface` list, prefers a zero-copy
  **AHB EGLImage** when `ps.ahb != nullptr`, else falls back to **`glTexImage2D`**
  (CPU→GPU upload on content change, keyed by `surface_key`/`content_serial`),
  draws a textured quad per surface, **`eglSwapBuffers`** once (L6134).
* It is installed into the compositor at **L6684**:
  `cfg.present_list = [](surfaces,out_w,out_h){ g_wl_presenter.present_list(...); }`
  (and the single-surface `cfg.present` at L6683), then
  `alr_start_wayland_compositor(cfg)` L6688.

So chromium's shm frames have a **complete, already-installed** present path —
the same one GIMP/foot/galculator use. **CR-4 writes no present code.**

### 5.2 shm format / stride / flip `[CODE]`

* Chromium's software raster commits `WL_SHM_FORMAT_ARGB8888` (0) or `XRGB8888`
  (1) `[INFER, upstream]`. The compositor's `PresentSurface` contract
  (`alr_compositor.hpp` L36, L46–49) documents exactly these two: little-endian
  **B,G,R,A** in memory, and the compositor **repacks to tightly-packed `width*4`
  top-left-origin** on commit (so the presenter never sees padded rows).
* The presenter swizzles `.bgra` in the fragment shader and **V-flips** (shm
  top-left origin → GL bottom-left), per the EGL-wiring notes at
  `alr_compositor.cpp` L2556–2566 and the live shader in `present_list`. Chromium
  frames take the `ps.pixels` branch; GPU/AHB surfaces take `ps.ahb`. **Match — no
  format work for chromium.**

### 5.3 frame callback / pacing `[CODE]`

`surface_commit` (L578–603) queues each `wl_callback` frame-callback onto
`g_pending_frames` and sets `g_scene_dirty`; the reactor's ~60 Hz timer presents
once per tick (the first-ever frame presents promptly, L598–601). So chromium's
`wl_callback` redraw loop is honored at panel rate without busy-looping — the
project already hardened this against the BLASTBufferQueue "Already acquired max
frames" overrun under commit storms (L590-593). The CSS animation + JS clock in
the demo page (§6) exercise exactly this round-trip, making the live-ness visible
in the recorded clip.

### 5.4 window vs dialog z-order / routing — the GIMP lesson holds `[CODE]`

Chromium's **main browser window** is a single `xdg_toplevel` → one
`PresentSurface` at `z=0` in `present_composited()` (L862–892). Chromium **menus /
popups** (the ⋮ menu, context menus, `<select>` dropdowns) come as **`xdg_popup`**
and are painted above their owning toplevel by the full `parent_key` chain
(`popup_owning_toplevel`, L709–735; emit loop L917+). A chromium **modal dialog**
(print, save-as) that arrives as a **child `xdg_toplevel`** is handled by
`looks_like_dialog()` (L808–822) → centered over its parent and kept **in the
z-order** (it is a toplevel, *not* a popup) — exactly the GIMP-dialog routing.

**The GIMP UAF guard is in force and protects chromium too** `[CODE]`: when
placing a dialog, the code **never dereferences the stored `parent_toplevel`
resource** (it may dangle, the v106 crash); it does a **pointer-VALUE** comparison
against live mapped toplevels and falls back to a size-based parent scan
(L831–847). Chromium tearing down a transient parent while a later dialog still
references it cannot UAF here. **No change needed.**

### 5.5 input routing — touch → wl_seat → chromium `[CODE]`

* Focus = top mapped toplevel (`zorder_top`, L748); a newly mapped toplevel
  steals focus (L496–510), so a chromium dialog/menu receives input when it opens.
* The inject entrypoints feed the focused client (`alr_compositor.hpp`
  L145–152): `alr_wayland_inject_pointer_motion/button/axis`,
  **`alr_wayland_inject_touch(id,x,y,phase)`** (0=down/1=move/2=up),
  `alr_wayland_inject_key` (Linux evdev codes). Chromium binds
  `wl_pointer`/`wl_keyboard`/`wl_touch` off `wl_seat` v7 and maps touch to its
  synthetic pointer/touch the same way GTK does `[INFER, upstream]`. The keymap fd
  is delivered over `wl_keyboard.keymap` from the bundled US keymap
  (`alr_xkb_keymap_us.h`), so HW/physical keys type.
* **Soft-keyboard text entry into web `<input>`** needs `zwp_text_input_v3`
  (Android IME → wayland), which is **not advertised** — a *post-demo* quality
  item (the globals doc SPEC-1), **not** a stepA/stepB blocker. The demo's
  interactivity (tap, scroll/drag the list) needs only pointer/touch, which are
  present.

---

## 6. Demo asset — `tools/chromium/demo.html` `[CODE, this lane]`

Authored in this lane (host-verified well-formed, **zero network**: no
`link rel`/`script src`/`img src`/`@import`/web-font/`fetch`/XHR). 8.4 KB, fully
inline. Heading **"ALR · Chromium on Android"**; portrait layout sized for the
device panel; a CSS-gradient card row + an inline SVG mark (SVG paint path) + a
**pure-CSS animated bar** and a **1-line JS clock** (both no-network — they make
the window visibly *live* in the recorded clip, exercising §5.3 frame callbacks)
+ a **touch-scrollable list** (so a drag is visibly interactive, exercising §5.5).
Carries the grep marker **`ALR-CR4-OK`**.

It sits next to the existing `tools/chromium/cr-test.html` and is intentionally
**different**: `cr-test.html` is the CR-1 *headless `--dump-dom`* assertion page
(marker `ALR chromium renders.`); `demo.html` is the CR-4 *on-screen* page (a
branded full-viewport layout meant to be SEEN). The device track stages it the
same way `cr-test.html` is staged (e.g. into `chromium-net-stage.tar` at
`/root/demo.html`, or any app-writable path), then opens it with the §4 command.
**Offline:** `file://`/`data:` need no DNS/TLS, so the window demo passes with the
network still broken (orthogonal to CR-2).

---

## 7. CR-4 DEVICE-REQ — for the device track

> Host-only worker: the following are **device gates** to execute. App sources /
> manifest / stamp untouched by this lane. Where an app-code change is implied,
> only the **location** is named (§2 binary choice, §3 `OZONE_PLATFORM`); the
> device track applies it.

### stepA-0 (prerequisite): confirm the binary has ozone-wayland `[INFER → resolves to CODE on run]`
Run the §4 command's binary once with just
`--ozone-platform=wayland --no-sandbox --disable-gpu --user-data-dir=/tmp/cr4 file:///root/demo.html`.
* **If chromium logs `Unsupported Ozone platform: wayland` / null platform / falls
  to headless** → it is the headless-shell (§2). **Stage a Chromium binary built
  WITH ozone-wayland** (full `chrome`, modeled as `<rootfs>/opt/chromium/chrome`,
  or `content_shell`) and retry. **This is the gate that unblocks everything else.**
* **If it proceeds to bind globals** → ozone-wayland is present; go to stepA.

### stepA (WINDOW PRESENT) — the on-screen milestone
Launch the §4 command. **PASS = ordered observables** (compositor logcat tag
`alr_wayland`; chromium stderr via `--enable-logging=stderr --v=1`):
1. chromium binds `wl_compositor v4`, `xdg_wm_base v2`, `wl_shm`, `wl_seat v7`,
   `wl_output v4` (no "wl_X not available").
2. chromium maps an `xdg_toplevel` → compositor sends the initial xdg configure →
   `xdg_surface.ack_configure`.
3. `surface committed shm: <w>x<h> stride=… fmt=0|1`.
4. `present_list … N surface(s) out=WxH` → the **already-installed** EGL hook
   (§5.1) uploads → **the `demo.html` page (heading "ALR · Chromium on Android")
   is visible on the SurfaceView**, with the CSS bar animating + the clock ticking.

**Likely stepA failure modes & where each is NOT a globals gap:**
* `Unsupported Ozone platform: wayland` → **§2 binary** (headless-shell), not a
  registry gap. (Resolved by stepA-0.)
* "Failed to connect to Wayland display" → guest env not reaching the client —
  but L1534-1535 already inject it (§3), so check the socket path / that the
  compositor is up (`STATUS: running`) first; **not** a registry gap.
* Window never appears though it connected → `--ozone-platform=wayland` missing on
  argv **and** `OZONE_PLATFORM` unset (§3) → ozone picked headless/x11.
* Connected + mapped but **blank** → shm format/stride/V-flip mismatch in the EGL
  hook — but that path is proven for GIMP/foot and the two formats are the
  contract's (§5.2); suspect a 0×0 initial commit before the first real frame.
* Startup crash unrelated to wayland → CR-2 brk/CHECK or a missing font (ship ≥1
  font), not ozone.

### stepB (INPUT round-trip)
With the window up, drive the inject entrypoints (§5.5):
1. `alr_wayland_inject_touch` a **down→move→up drag** over the demo's scroll list
   → the list **scrolls** (touch → `wl_touch` → chromium scroll).
2. `alr_wayland_inject_pointer_axis` (vertical) → page/list scrolls (wheel path).
3. Tap a row / `alr_wayland_inject_pointer_button` left down+up on a link-like row
   → hover/active state changes (pointer path).
4. (optional, HW-key) `alr_wayland_inject_key` a printable key with a focused
   field → it types (keymap path). **Soft-keyboard IME text entry is out of scope
   for stepB** (needs `zwp_text_input_v3`, §5.5 — a post-demo add).

**PASS = the demo page visibly reacts to injected touch/scroll** (the list moves,
states change), proving the full SurfaceView-touch → `wl_seat` → chromium loop.

---

## 8. One-line conclusion

**The Wayland registry and the present path are NOT the CR-4 blockers** — every
hard global is advertised at an accepted version (§1) and the EGL `present_list`
hook is **already installed and proven** (§5.1, a correction to the prior TODO).
**The two real CR-4 items are:** (HARD) **swap `chromium-headless-shell` for a
Chromium binary that includes the ozone-wayland platform** — full `chrome` or
`content_shell` (§2), and (SOFT) **select that platform** via
`--ozone-platform=wayland` on argv (or add `OZONE_PLATFORM=wayland` next to
`runtime_report.cpp` L1541) (§3). With those, the §4 flags open a borderless,
software, **offline** window that presents `demo.html` on the SurfaceView (stepA)
and round-trips injected touch (stepB).
