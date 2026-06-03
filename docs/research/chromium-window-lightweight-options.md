# Lightweight on-screen web shell for ALR — content_shell vs cog (WPE)

**Lane:** integration HOST staging (Track 1, **no device**). **Owns:** this file +
`tools/build_content_shell_stage.py` + `tests/test_build_content_shell_stage.py`.
App native / MainActivity / manifest: **read-only, untouched.**
**Date:** 2026-06-03. **Tree HEAD:** 5ba250d (`main`).

## 0. The problem this solves

Full `chromium` (the browser) opens a real `--ozone-platform=wayland` window, but
it is **multi-process** (browser + GPU + renderer + utility). Under the ALR
in-process re-map, *each* child re-maps ~0.8–1.3 GB of address space → the window
never appears on a phone (OOM). This is documented in
`docs/research/chromium-window-demo-plan.md` §2 and memory note
`chromium-native-goal` (CR-4): full GUI = memory blow-up; **headless works**
(CR-5) but has no window.

The goal here: stage **the lightest arm64 glibc web shell that still opens a real
Wayland window**, close to single-process so the ALR loader re-maps **one**
address space, not four — a memory-fit candidate to wire onto the ALR compositor
via ozone/wayland.

Honesty convention: **[FACT]** = host-verified in this tree / from the .deb
binary; **[INFER]** = reasoned from upstream behavior, resolved by the device run.

---

## 1. 1순위 content_shell — 확보 **가능** (Debian `chromium-shell`) `[FACT]`

content_shell is normally a Chromium build/CI artifact, **not** packaged by most
distros — Ubuntu noble does not ship it (Ubuntu ships chromium itself as a *snap*,
not a .deb). **But Debian packages it**, as **`chromium-shell`**:

| field | value (host-verified against the live bookworm index) |
|---|---|
| package | **`chromium-shell`** (source package `chromium`) |
| description | *"a minimal version of the chromium user interface (the **content shell**)"* |
| version (bookworm) | **147.0.7727.137-1~deb12u1** |
| arch | arm64 (`pool/main/c/chromium/chromium-shell_…_arm64.deb`, 55 MiB compressed) |
| entrypoint ELF | **`/usr/lib/chromium/chromium-shell`** (182.7 MiB, `ET_DYN`) |
| `/usr/bin/chromium-shell` | a **52-byte shell wrapper** — NOT the ELF; the ALR loader maps the `/usr/lib` ELF |
| shell data in the leaf | `content_shell.pak` (5.4 MiB), `shell_resources.pak`, `libtest_trace_processor.so` |
| shared data | `chromium-common` (icudtl, V8 snapshots, libEGL/libGLESv2/swiftshader, `resources.pak`, locales) |

### 1.1 ozone-wayland is compiled in — **proven on the actual binary** `[FACT]`

The big risk for any "minimal chromium" is that the wayland Ozone backend was not
built in (that is exactly why `chromium-headless-shell` cannot open a window — its
binary has **no** wayland backend). For `chromium-shell` this was **checked by
scanning the downloaded 182.7 MiB ELF**:

| token in `/usr/lib/chromium/chromium-shell` | present | meaning |
|---|---|---|
| `ozone_platform_wayland.cc` | **YES** | the **Wayland Ozone backend is compiled in** → `--ozone-platform=wayland` works |
| `ozone-platform` | YES | the platform-selector flag is wired |
| `ozone_platform_x11.cc` | YES | x11 backend also present (we select wayland explicitly) |
| `ozone_platform_headless.cc` | **NO** | this is **not** the headless shell |

The build-time gate `binary_has_ozone_wayland()` asserts the two wayland tokens on
every pack (and the `--selftest` exercises the detector), so a regressed/headless
binary fails the build. **This is the gate that, for full chromium, the device
track had to discover at runtime (`chromium-window-demo-plan.md` §2 stepA-0); for
content_shell it is settled host-side, before staging.**

### 1.2 process model — **single-process capable** `[FACT for the flag]` / `[INFER for the win]`

content_shell accepts **`--single-process`** (same content-module flag the CR-1/CR-5
headless probes use). The Chromium Ozone/Wayland design keeps the UI and GPU
components **in the same process** [FACT, upstream Ozone docs], so a single-process
content_shell drives the wayland window from **one** address space.

**[INFER, the memory-fit claim]** With `--single-process`, the ALR loader re-maps
**one** content_shell address space instead of full chromium's four → no
per-child 0.8–1.3 GB re-map storm. This is *the* reason content_shell is expected
to fit where full chromium OOMs. It is an inference until the device run measures
it (DEVICE-REQ §5), but it rests on two facts: (a) the flag is accepted, (b) full
chromium's OOM is specifically the *multi-process* re-map (per the CR-4 note), which
`--single-process` collapses.

---

## 2. 2순위 cog (WPE WebKit) — 확보 가능하지만 **단일프로세스 아님** → 백업 `[FACT]`

Investigated as the §goal's named alternative. **Available** but a worse fit:

| field | value (host-verified against the bookworm index) |
|---|---|
| package | **`cog`** 0.16.1-1, arm64 — *"Single-window web browser based on WPE WebKit … suitable as a web application container for embedded devices in kiosk mode"* |
| entrypoint | `/usr/bin/cog` (+ `cogctl`) |
| wayland backend | `/usr/lib/aarch64-linux-gnu/cog/modules/libcogplatform-wl.so` (a Wayland platform module — good) |
| engine deps (NOT in the `cog` .deb) | **`libwpewebkit-1.1-0` (≥2.34)**, `libwpebackend-fdo-1.0-1` (≥1.10), `libwpe-1.0-1` (≥1.14) |

**Why cog is the backup, not the pick — process model `[FACT]`:** since WPE/WebKit
**2.26 the single-process model was removed**; the only allowed model is
`MULTIPLE_SECONDARY_PROCESSES` (UIProcess + a separate **WPEWebProcess**, plus a
NetworkProcess, and a GPUProcess on recent versions). So cog is **structurally
multi-process** — the very property that makes full chromium OOM under ALR. It is
lighter than chromium per-process and tunable (`WPE_RAM_SIZE`,
`MemoryPressureSettings`), but it **cannot** be made single-process, and it drags
in the large `libwpewebkit-1.1` closure. content_shell's `--single-process` is a
strictly closer fit to the "one re-mapped address space" goal.

**Decision: stage content_shell.** cog stays documented as the fallback if a device
run ever shows content_shell's wayland path unusable on this binary (then cog's
multi-process model is the price for WebKit). epiphany / surf / WebKitGTK were not
staged: they are WebKitGTK, i.e. **also** multi-process (network + web process),
with no single-process advantage over cog and a heavier GTK closure.

---

## 3. The produced stage-tar `[FACT]`

`tools/build_content_shell_stage.py` → **`out/v2-stage/content-shell-stage.tar`**
(reuses the full-GUI builder's deb_closure engine + repack helpers verbatim; only
the package/entrypoint/output differ). Host build result:

```
content_shell stage: out/v2-stage/content-shell-stage.tar
  tar size:         293.0 MiB
  entrypoint:       /usr/lib/chromium/chromium-shell  PRESENT (182.7 MiB ELF)
  ozone-wayland:    YES (window-capable shell)  → --ozone-platform=wayland
  process model:    single-process capable (--single-process) → ONE re-mapped address space
  closure pkgs:     137
  reachable libs:   29 (base-missing .so added)
  DT_NEEDED gate:   0 unsatisfied  (full closure: base ∪ overlay)
  libpulsecommon:   flattened → usr/lib/aarch64-linux-gnu/libpulsecommon-16.1.so
  NSS dlopen mods:  5 [libfreebl3, libfreeblpriv3, libnssckbi, libnssdbm3, libsoftokn3]
  compat aliases:   [libopenh264.so.7 <- libopenh264.so.2]
  overlay_guard:    OK
  stage_tar_spec:   CONFORMANT
  => DEMO-READY (single-process window-capable content_shell, 0 missing .so)
```

* **§5-E**: `./`-rooted, no absolute/escaping members, no symlinks; `stage_tar_spec`
  CONFORMANT; `overlay_guard` OK (no base-library downgrade).
* **closure (§5-E "0 missing .so")**: every DT_NEEDED of the content_shell binary
  is satisfied by **base ∪ this overlay** (0 unsatisfied), plus the two `dlopen`
  sets the DT_NEEDED graph cannot see and that would otherwise FATAL at runtime —
  **NSS** (libsoftokn3/libfreebl3/…; without them NSS init FATALs `nss_error=-5925`
  during TLS) and **libpulsecommon flattened** onto the flat `LD_LIBRARY_PATH` dir
  (libpulse DT_NEEDEDs it but only resolves it on its absolute RUNPATH, which the
  ALR loader does not honor). Same two fixes the full-GUI builder ships.
* **vs full chromium**: `out/v2-stage/chromium-gui-stage.tar` is **354.2 MiB** with
  a **243.4 MiB** browser ELF; content_shell is **293.0 MiB** with a **182.7 MiB**
  ELF — **~61 MiB smaller on disk**, and (the point) **single-process at runtime**.

The tar is large (293 MiB) → **NOT committed** (per the lane's `out/` rule). Rebuild
with: `python -m tools.build_content_shell_stage --base rootfs/tiny-rootfs.tar`.

---

## 4. Launch — flags for a clean, software, offline window `[CODE-flags]`+`[INFER-effect]`

Same flag philosophy as `chromium-window-demo-plan.md` §4 (a borderless,
software-rastered window committing `wl_shm` frames the ALR AHB/SurfaceView
presenter composites), but **content_shell-specific** and **starting
single-process** (the whole reason we picked it):

```
/usr/lib/chromium/chromium-shell     # the content_shell ELF (NOT /usr/bin wrapper, NOT headless-shell)
--ozone-platform=wayland             # HARD: select the wayland Ozone backend (proven compiled in, §1.1)
--single-process                     # the memory-fit lever: ONE re-mapped address space
--no-sandbox                         # the ALR loader supervises syscalls; chromium's own sandbox can't nest
--disable-gpu                        # no /dev/dri/dmabuf → software raster → frames arrive as wl_shm
--disable-dev-shm-usage              # /dev/shm absent/limited → temp files
--ozone-override-screen-size=1200,1920   # hint the panel size (portrait); flag verified in the binary; compositor also fits to wl_output
--content-shell-hide-toolbar         # hide content_shell's mini toolbar → full-viewport page (flag verified in the binary)
--enable-logging=stderr --v=1        # route logs to stderr so the device drain captures bind/map/commit
file:///root/demo.html               # OFFLINE local page (no DNS/TLS); data:text/html,... also works
```

* `content_shell` takes a **URL as a positional arg** (no `--app=`/profile flags
  needed); `file://`/`data:` keep it offline so the window demo passes even with
  the network still broken (orthogonal to CR-2). The branded demo page
  `tools/chromium/demo.html` (marker `ALR-CR4-OK`, the same asset the full-GUI demo
  uses) is staged at `/root/demo.html` by `build_chromium_net_overlay` exactly as
  `cr-test.html` is.
* Guest env is **already** provided by `runtime_report.cpp` (read-only here):
  `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=<cacheDir>/alr-xdg` (L1534-1535) →
  content_shell inherits the wayland socket. Optionally also `OZONE_PLATFORM=wayland`
  (belt-and-braces; the argv flag already covers it — same as `chromium-window-demo-plan.md` §3).
* **Sizing flags `[FACT for presence]`+`[INFER for effect]`:** the binary contains
  `ozone-override-screen-size`, `window-size`, and `content-shell-hide-toolbar`
  (scanned), but **not** `start-maximized` (a *chrome/browser* flag absent from the
  content shell — so the §4 command does NOT use it). Which of the present knobs
  actually sizes the toplevel is the device-resolved bit; either way the compositor
  fits the toplevel to the single output (`wl_output`), so sizing is **not** a
  blocker for a window appearing.

---

## 5. DEVICE-REQ — for the device / integration track

> Host-only worker (Track 1): the below are **device gates** the
> device/integration track executes (it stages this tar onto the ALR compositor
> and runs it). App native / manifest / stamp untouched by this lane.

**stepA-0 (already settled host-side — recorded for completeness):** is the wayland
Ozone backend in the binary? **YES** — proven by the `ozone_platform_wayland.cc`
token (§1.1). Unlike full chromium (whose §2 stepA-0 was a runtime unknown), this
gate is closed before staging. The device need only confirm it *binds*.

**stepA (WINDOW PRESENT):** launch the §4 command. PASS = the ordered observables
of `chromium-window-demo-plan.md` §7 stepA, applied to content_shell:
1. binds `wl_compositor v4`, `xdg_wm_base v2`, `wl_shm`, `wl_seat v7`, `wl_output v4`;
2. maps an `xdg_toplevel` → initial xdg configure → `ack_configure`;
3. `surface committed shm: <w>x<h> stride=… fmt=0|1`;
4. the **already-installed** EGL `present_list` hook (`runtime_report.cpp`
   L6020-6137, per `chromium-window-demo-plan.md` §5.1) uploads → **`demo.html` is
   visible on the SurfaceView**, CSS bar animating + clock ticking.

**stepA' (the memory-fit measurement — THE claim to verify) `[INFER → device]`:**
with `--single-process`, confirm the content_shell process re-maps **one** address
space and **fits in device RAM** (no OOM-kill, window stays up). This is the
hypothesis content_shell was chosen to prove vs full chromium's multi-process OOM.
If `--single-process` content_shell still OOMs, the fallback ladder is: drop
`--single-process` but keep `--no-zygote --renderer-process-limit=1` (one renderer
child), then cog (§2) as the WebKit alternative.

**stepB (INPUT round-trip):** identical to `chromium-window-demo-plan.md` §7 stepB
(inject touch drag / pointer axis / button via the compositor's
`alr_wayland_inject_*` entrypoints → the demo page scrolls/reacts), since the
present + input paths are shared and shell-agnostic.

---

## 6. One-line conclusion

**content_shell IS obtainable as a real arm64 .deb** (Debian `chromium-shell`
147), **its wayland Ozone backend is host-proven compiled in**, and it is
**`--single-process`-capable** — so it is the lightest *window-capable* chromium
and the best memory-fit candidate (one re-mapped address space vs full chromium's
four). `tools/build_content_shell_stage.py` stages it to
`out/v2-stage/content-shell-stage.tar` (293 MiB, 0 unsatisfied DT_NEEDED, §5-E
CONFORMANT). cog (WPE) is obtainable too but is **structurally multi-process** (WPE
≥2.26 removed single-process) → documented as the fallback. The on-screen window
and the single-process memory-fit measurement are the device gates (§5).
