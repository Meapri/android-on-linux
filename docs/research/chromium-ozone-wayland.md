# CR-4 — Chromium `--ozone-platform=wayland` onto the in-app compositor

**Lane:** auto/cr4-ozone (HOST-ONLY research). **Owns:** this file only.
**Status:** design + audit. No device verification (host-only worker).
**Date:** 2026-06-02.

CR-1 is achieved (chromium-headless-shell `--dump-dom` renders in-process, 3/3).
CR-4 is the *on-screen* milestone: drive `--ozone-platform=wayland` so chromium
binds the ALR in-app Wayland compositor (`app/src/main/cpp/alr_wayland/`), maps a
toplevel, and presents `wl_shm` frames that the lead's EGL hook uploads to the
SurfaceView — the **same** present path that already shows GIMP/foot/gtk3.

This doc answers the single question the plan left open
(`chromium-native-plan.md` §"Open questions" *(3)*): **which Wayland globals does
chromium ozone-wayland REQUIRE vs what the compositor advertises, ranked by
hard-fail.** Then it pins the `wl_shm → SurfaceView` present path (reuse, not new
code) and gives WS-3 the exact interface additions as a spec.

---

## 1. What the compositor advertises today (ground truth)

From `alr_compositor.cpp::register_globals()` + the version constants at the top
of that file:

| Global | Version advertised | Source |
|---|---|---|
| `wl_compositor` | **4** (`kCompositorVersion`) | `wl_global_create` |
| `wl_shm` | 1 (`kShmVersion`) | created by `wl_display_init_shm()` |
| `wl_seat` | **7** (`kSeatVersion`, ceiling 10; GDK takes MIN) | `wl_global_create` |
| `wl_output` | **4** (`kOutputVersion`) | `wl_global_create` |
| `xdg_wm_base` | **2** (`kXdgWmBaseVersion`) | `wl_global_create` |
| `wl_subcompositor` | 1 | `wl_global_create` |
| `wl_data_device_manager` | 3 | `wl_global_create` (added for GDK) |

Generated protocol glue present in `third_party/wayland_generated/`: **only**
`wayland` core (`wayland.xml`) + `xdg-shell` (`xdg-shell.xml`). No other protocol
XML is vendored, so any *new* interface below costs: add XML → regenerate
client+server glue → add the `.c` to CMake. That is the scope wall for "needs WS-3
work" vs "already shippable."

`zwp_linux_dmabuf_v1` is **deliberately un-advertised** (no `/dev/dri`; keeps
swaps on `wl_shm` so the AHB presenter composites — plan Scout-3, restated in the
big comment above `register_globals()`). Do **not** add it.

---

## 2. Chromium ozone-wayland required-globals audit

Source of truth = Chromium `ui/ozone/platform/wayland/host/wayland_connection.cc`
(the registry `Global()` handler) and the per-object `…/host/wayland_*.cc`
wrappers, cross-checked against this project's Scout-3 recon. Chromium's wayland
backend classifies a global as **hard-fail** (the backend refuses to initialize →
no window, process may abort or fall back to headless) or **soft-degrade** (a
feature is disabled, a warning is logged, the browser still opens a window).

### Ranked by hard-fail

| Rank | Global | Chromium behavior if ABSENT | We advertise? | Verdict |
|---|---|---|---|---|
| **H1** | `wl_compositor` (≥v4) | **HARD-FAIL** — `WaylandConnection::Initialize()` returns false; no surfaces possible. | yes (v4) | OK |
| **H2** | `wl_shm` | **HARD-FAIL for our config** — with no dmabuf/drm, shm is the *only* buffer path; absent ⇒ chromium has no software buffer backing ⇒ no pixels. (Formally chromium treats shm as required for the sw path.) | yes (v1) | OK |
| **H3** | `xdg_wm_base` (stable) **or** `zxdg_shell_v6` | **HARD-FAIL** — no shell ⇒ `WaylandWindow` cannot create an xdg_surface ⇒ no toplevel. Chromium needs **stable xdg-shell**; it dropped zxdg_shell_v6 support years ago, so stable is mandatory. | yes (`xdg_wm_base` v2) | OK |
| **H4** | `wl_seat` | **Effectively hard** for an interactive browser — without it there is no input (pointer/keyboard/touch). Chromium still *opens a window* (it is technically optional in `WaylandConnection`), but a browser with zero input is non-functional. | yes (v7) | OK |
| **H5** | `wl_output` | **Soft-but-critical** — absent ⇒ chromium has no display geometry/scale; it falls back to a default and `display::Screen` may be degenerate. Window opens but sizing/HiDPI is wrong. | yes (v4) | OK |

**Conclusion (H-tier):** every chromium ozone-wayland HARD requirement is already
advertised, at a version chromium accepts. **There is no missing hard-fail
global.** CR-4 is *not* gated on registry completeness — it is gated on the
present path (§3) + the guest-env/connect plumbing the INTEGRATION_NOTES already
flag. This matches the comment block above `register_globals()`.

### Soft-degrade globals chromium binds-if-present (ranked by user-visible impact)

These NEVER hard-fail. Chromium probes each, logs at most a warning, and runs.
Listed so WS-3 can prioritize *quality* work after first light.

| Rank | Global | What chromium uses it for | Impact if absent (our case) | Recommend add? |
|---|---|---|---|---|
| **S1** | `wp_viewporter` (`wp_viewporter`/`wp_viewport`) | Scale/crop a buffer to a different surface size in the compositor (HiDPI fractional scale, and chromium's `WaylandSurface` set_viewport_*). | Chromium falls back to integer `wl_surface.set_buffer_scale` + buffer-sized surfaces. On a single 1:1 SurfaceView output this is **cosmetic only** (no fractional scaling). | LATER (only when fractional scale matters) |
| **S2** | `wl_subcompositor` | Overlay/video/canvas surfaces as subsurfaces; chromium's `WaylandSubsurface`. | **Already advertised** (v1). Chromium will use it for overlays; our compositor composites subsurfaces by parent-key (already wired for GTK menus). For a basic page, chromium renders the whole window in the root surface anyway. | ALREADY PRESENT |
| **S3** | `zxdg_decoration_manager_v1` | Negotiate server-side vs client-side window decorations. | Absent ⇒ chromium uses its own **client-side decorations** (CSD). For a borderless full-screen SurfaceView that is exactly what we want. | NO (CSD is desired) |
| **S4** | `wp_presentation` (presentation-time) | Precise present timestamps for swap pacing / frame scheduling. | Absent ⇒ chromium uses its own swap pacing; we pace via the frame timer + `wl_callback.done`. Adds jitter at worst. | LATER (smoothness) |
| **S5** | `zxdg_output_manager_v1` (`xdg-output`) | Logical (DIP) output geometry + name. | Absent ⇒ chromium derives geometry from `wl_output` mode/scale (we send v4 incl. name). Fine for one output. | NO for one output |
| **S6** | `zwp_pointer_constraints_v1` + `zwp_relative_pointer_v1` | Pointer lock / relative motion (WebXR, FPS web games, pointer-lock API). | Absent ⇒ the JS Pointer Lock API fails gracefully (`requestPointerLock` rejects). No crash. Irrelevant to touch-first Android UX. | NO (unless pointer-lock web apps needed) |
| **S7** | `zwp_text_input_v3` + `zwp_input_method_*` | On-screen-keyboard / IME text entry into web inputs. | Absent ⇒ chromium uses no Wayland IME; physical-key events via `wl_keyboard` still type. **For Android soft-keyboard text entry into web forms this becomes important later**, but it is still soft (no crash). | LATER (touch text entry) |
| **S8** | `zwp_linux_dmabuf_v1` | Zero-copy GPU buffers (the GPU/ANGLE path). | **Intentionally absent.** Forces the `wl_shm` sw path so the AHB presenter owns the copy-to-SurfaceView. Adding it without `/dev/dri` would make chromium try a GBM/DRM path that fails. | **NEVER** (per constraint) |
| **S9** | `wl_drm` / `zwp_linux_explicit_sync_v1` / `wp_linux_drm_syncobj_v1` | DRM auth + GPU fence sync for the dmabuf path. | N/A while shm-only. | NEVER (tied to dmabuf) |

**Net:** the only globals worth adding *after* first light, in priority order, are
**S7 text-input** (soft keyboard → web forms) then **S1 viewporter** (fractional
HiDPI) then **S4 presentation-time** (swap smoothness). None are CR-4 blockers.

### Version-floor gotchas (these can look like a missing global)

* **`xdg_wm_base` v2 is fine.** Chromium's stable xdg-shell wrapper works against
  v1–v6; it negotiates `MIN(server, client)`. v2 gives `xdg_toplevel` +
  `xdg_popup`, enough for the browser window + menus. No bump needed.
* **`wl_compositor` v4** gives `wl_surface.damage_buffer` (chromium prefers it over
  the deprecated `wl_surface.damage`). Good — keep ≥4.
* **`wl_output` v4** advertises `name`/`description` + atomic `done`; chromium
  reads `name` for the `display::Display`. Keep ≥4.
* **`wl_seat` v7**: chromium's seat wrapper wants `wl_pointer`/`wl_keyboard`/
  `wl_touch` (since seat v3) and `wl_keyboard.repeat_info` (since v4). v7 covers
  all of it. The xkb keymap fd (see §4) is delivered over `wl_keyboard.keymap` —
  the compositor already ships the US keymap (`alr_xkb_keymap_us.h`).

---

## 3. The `wl_shm → SurfaceView` present path for chromium frames (REUSE)

**Chromium needs no new present code. It rides the exact GIMP/gtk3 path.** Here is
the path end-to-end, naming the existing seams:

1. **Chromium draws** a software frame (V8/Skia raster, no GPU) into an
   `wl_shm` pool buffer it created via `wl_shm_create_pool` / `wl_shm_pool_create_buffer`.
   Format: `WL_SHM_FORMAT_ARGB8888` (0) or `XRGB8888` (1) — chromium uses one of
   these for the sw path. These are the **two formats the existing presenter
   already handles** (see `PresentFrame::shm_format` and the
   `0=ARGB8888,1=XRGB8888` note in `alr_compositor.hpp`).

2. **Chromium commits**: `wl_surface.attach` + `damage(_buffer)` + `commit` on its
   xdg_toplevel surface. This lands in the compositor's `surface_commit` handler
   (already implemented; same handler GIMP/foot exercise).

3. **The compositor composites**: `present_composited()`
   (`alr_compositor.cpp` ~line 862) walks the z-order, builds a
   `std::vector<PresentSurface>` (compositor-owned, tightly-packed `width*4` BGRA,
   top-left origin — the compositor does the stride repack on commit), and calls
   `CompositorConfig::present_list(surfaces, out_w, out_h)`
   (`PresentListCallback`, `alr_compositor.hpp`). Chromium's single toplevel is
   one `PresentSurface` (z=0); its subsurfaces, if any, stack above by parent-key —
   identical to the GTK-menu code already in `present_composited()`.

4. **The lead's EGL hook uploads + presents**: the `present_list` lambda the lead
   installs in `runtime_report.cpp` (per `INTEGRATION_NOTES.md` §4) uploads each
   `PresentSurface.pixels` via `glTexImage2D(GL_RGBA, …)`, draws a textured quad
   into the SurfaceView's `ANativeWindow` (BGRA swizzle in the fragment shader,
   V-flip for the top-left→bottom-left origin flip), then `eglSwapBuffers`. This is
   the **same proven EGL+ANativeWindow path** as
   `render_to_android_surface_frames()`. **GPU/AHB surfaces** (`PresentSurface.ahb
   != nullptr`, §5-C) are imported zero-copy; chromium's shm surfaces take the
   `pixels` branch. No new presenter is written for CR-4.

**Why this just works for chromium:** the present contract is buffer-format- and
client-agnostic. The presenter only sees `PresentSurface` (packed BGRA + a dst
rect + a z + a stable key). It does not know or care that the producer is chromium
vs GIMP. The two shm formats chromium emits are already the two the contract
documents. **So the present-path work for CR-4 is zero new code — it is exactly
the existing GIMP path.**

### The two real CR-4 seams (not registry, not present)

Both are already documented; restating so they are not mistaken for "missing
globals":

* **G-1 guest env reaches the client.** `INTEGRATION_NOTES.md` §3 "IMPORTANT": the
  in-process native-loader path does not build a guest `envp`, so
  libwayland-client sees no `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` and won't connect.
  CR-4 needs `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=<cacheDir>/alr-xdg`, and
  for chromium specifically **`--ozone-platform=wayland`** on argv (so ozone does
  not auto-pick headless). Optionally `OZONE_PLATFORM=wayland`. This env wiring is
  the same blocker GIMP had; once GIMP shows on screen, chromium inherits it.
* **G-2 connect() to the AF_UNIX socket.** `connect()` to the host-path socket is
  NOT seccomp-trapped and the socket lives in app-private `cacheDir`, so SELinux
  permits it (same as GIMP). The CR-2 socket blockers (UDP-53 DNS, TLS connect)
  are about *network* sockets to the internet, **orthogonal to the AF_UNIX
  compositor socket** — CR-4 can render a `file://`/`data:`/cached page with the
  network still broken.

---

## 4. xkb keymap + input for chromium (already satisfied)

Chromium's `WaylandKeyboard` requires a keymap fd over `wl_keyboard.keymap`
(format `XKB_V1`). The compositor already synthesizes a US keymap from
`alr_xkb_keymap_us.h` and the seat is v7 (pointer+keyboard+touch+repeat_info). So:

* **Pointer/touch** → chromium `WaylandPointer`/`WaylandTouch` via `wl_seat`. The
  input-injection entrypoints (`alr_wayland_inject_pointer_*`,
  `alr_wayland_inject_touch`, `alr_wayland_inject_key`) already feed the focused
  client; touch maps to chromium synthetic mouse/touch the same way it does for
  GTK. No change.
* **Keyboard** → keymap delivered; physical keys type. Soft-keyboard IME text
  entry into web `<input>` needs **S7 zwp_text_input_v3** (a *later* quality item,
  not a CR-4 blocker; physical/HW keys work without it).

---

## 5. Spec for WS-3 (interface additions) — NONE required for CR-4

Per the constraint, this lane does not edit `alr_wayland/`. The audit's result is
that **WS-3 needs to add nothing to land CR-4**: every hard-fail global is present
at an accepted version, and the present path is the existing GIMP `present_list`
seam. The following are the *post-first-light quality* additions, written as
precise WS-3 specs so they can be picked up later **in priority order**. Each
costs: vendor the protocol XML → regenerate client+server glue into
`third_party/wayland_generated/` → add the generated `.c` to the `alr_wayland`
CMake sources → add a `bind_*` + impl + `wl_global_create` in
`alr_compositor.cpp`.

> WS-3-style spec format: **file** = `app/src/main/cpp/alr_wayland/alr_compositor.cpp`
> (+ the named protocol glue under `third_party/`); **interface** = the named
> Wayland global. None of these change `alr_compositor.hpp`'s public API or the
> `PresentSurface`/`present_list` contract.

### SPEC-1 (priority 1, "touch text entry") — `zwp_text_input_v3`
* **Interface:** `zwp_text_input_manager_v3` (global) → `zwp_text_input_v3`
  (per-seat object).
* **Protocol XML:** `text-input-unstable-v3.xml` (wayland-protocols).
* **Server side:** advertise the manager global; on `get_text_input(seat)` create
  a `zwp_text_input_v3` resource. Implement `enable`/`disable`/`set_*` as state
  capture; bridge `commit_string`/`preedit_string`/`done` from the Android
  soft-keyboard (an `alr_wayland_inject_text(const char* utf8)` entrypoint added to
  `alr_compositor.hpp` — this WOULD be a public-API addition, so flag for
  integration approval at that time).
* **Why:** lets the Android IME type into web form fields. Until then, only HW
  keyboards type into chromium.
* **Hard-fail?** No. Chromium runs without it; only IME text entry is unavailable.

### SPEC-2 (priority 2, "fractional HiDPI / exact sizing") — `wp_viewporter`
* **Interface:** `wp_viewporter` (global) → `wp_viewport` (per-surface).
* **Protocol XML:** `viewporter.xml` (wayland-protocols stable).
* **Server side:** advertise the global; on `get_viewport(surface)` attach a
  `wp_viewport` to the `SurfaceState`. Honor `set_source`(crop) + `set_destination`
  (scale) by adjusting that surface's `dst_w/dst_h` (and a src crop rect) in
  `place_toplevel()` / when building the `PresentSurface`. No present-contract
  change — it only refines the existing `dst_*` rect.
* **Why:** chromium uses viewport to render at fractional device-scale without
  re-rastering; on a fixed 1:1 SurfaceView this is cosmetic, so it is P2.
* **Hard-fail?** No. Chromium falls back to integer `set_buffer_scale`.

### SPEC-3 (priority 3, "swap smoothness") — `wp_presentation`
* **Interface:** `wp_presentation` (global) → `wp_presentation_feedback`
  (per-commit object).
* **Protocol XML:** `presentation-time.xml` (wayland-protocols stable).
* **Server side:** advertise the global; on `feedback(surface)` create a
  `wp_presentation_feedback` and, when the lead's EGL hook reports the swap
  completed (hook the existing frame-pacing tick), send `presented` with the
  SurfaceView's actual present timestamp (from `AChoreographer`/`eglSwapBuffers`
  return), else `discarded`. Advertise the clock id (`CLOCK_MONOTONIC`).
* **Why:** gives chromium accurate present timestamps for its frame scheduler,
  reducing jank. Soft.
* **Hard-fail?** No. Chromium uses internal swap pacing without it.

### Explicitly DO NOT add
* `zwp_linux_dmabuf_v1`, `wl_drm`, `zwp_linux_explicit_sync_v1`,
  `wp_linux_drm_syncobj_v1` — would push chromium onto a GBM/DRM/dmabuf path that
  needs `/dev/dri` we cannot give; keeps swaps on `wl_shm` so the AHB presenter
  owns the copy (plan constraint).
* `zxdg_decoration_manager_v1` — we *want* chromium's CSD on a borderless
  SurfaceView.
* `zxdg_output_manager_v1` — single output; `wl_output` v4 geometry suffices.

---

## 6. CR-4 launch command + first-light checklist (for the lead / device drain)

Reuse the CR-1 base flags; swap the ozone backend and drop the dump-file. NO GPU
yet (that is CR-3); keep software raster so frames arrive as `wl_shm`:

```
chromium --no-sandbox --single-process --no-zygote --disable-gpu \
  --disable-dev-shm-usage --user-data-dir=/<writable> \
  --ozone-platform=wayland \
  file:///<rootfs>/usr/share/.../test.html      # or data: / a cached page
```

with guest env `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=<cacheDir>/alr-xdg`,
`OZONE_PLATFORM=wayland`. (Hostname URLs still need CR-2 DNS; `file://`/`data:`
do not, so CR-4 can be proven with network broken.)

**PASS = on-screen, ordered observables (compositor logcat tag `alr_wayland`):**
1. `client bound: wl_compositor v4`, `xdg_wm_base v2`, `wl_shm`, `wl_seat v7`,
   `wl_output v4`. (No "wl_X not available" from chromium.)
2. `client mapped xdg_toplevel` → `sent initial xdg configure w=… h=…` →
   `xdg_surface.ack_configure`.
3. `surface committed shm: <w>x<h> stride=… fmt=0|1`.
4. `present_list … 1 surface(s) out=WxH` → the EGL hook uploads → a real chromium
   page is **visible on the SurfaceView**.
5. Tap/scroll via the inject entrypoints scrolls the page (input round-trips).

**Likely first-light failure modes (and where they are NOT a missing global):**
* Chromium logs *"Failed to connect to Wayland display"* → G-1 env not reaching
  the client (INTEGRATION_NOTES §3), **not** a registry gap.
* Chromium picks headless anyway → `--ozone-platform=wayland` missing on argv /
  `OZONE_PLATFORM` unset.
* Window opens but blank → present_list hook not installed, or shm format/stride/
  V-flip mismatch in the lead's EGL lambda (the format is one of fmt 0/1 the
  contract already documents).
* Crash at startup unrelated to wayland → that is CR-2's brk/CHECK or a
  font/fontconfig gap (ship ≥1 font), not ozone.

---

## 7. One-line conclusion for WS-1

**CR-4 needs no compositor registry change and no new present code: every
chromium ozone-wayland hard-fail global (`wl_compositor`, `wl_shm`, `xdg_wm_base`,
`wl_seat`, `wl_output`) is already advertised at an accepted version, and chromium
shm frames ride the existing GIMP `present_list → EGL → SurfaceView` path
unchanged.** The only CR-4 work is non-`alr_wayland`: (1) make the guest env
(`WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`) + `--ozone-platform=wayland` actually reach
the in-process chromium (INTEGRATION_NOTES §3 G-1, the same env blocker GIMP had),
and (2) install/confirm the `present_list` EGL lambda in `runtime_report.cpp`
(INTEGRATION_NOTES §4). text-input(v3)/viewporter/presentation-time are ranked
post-first-light quality adds, never blockers.
