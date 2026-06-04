# es2gears via ANGLE — REAL-GLES-app HW-accel device-test plan

> GPU-universal GOAL = REAL GLES apps HW-accelerated via ANGLE → our Vulkan ICD →
> Mali. The path was first proven with a bespoke client (`alr-angle-vk`, surfaceless
> FBO render). This plan stages a **canonical upstream GLES2 demo — es2gears** — and
> wires it to run through the IDENTICAL ANGLE env, so the proof is a REAL app, not a
> test client. **Host-only deliverable**: the es2gears overlay + the launch wiring +
> this plan. The on-device run is the gate (a concurrent agent owns the ICD fix that
> makes ANGLE actually render; this plumbing is READY for it).
>
> Device (from prior evidence): `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2,
> Android 16, 1200×1920 @ 90Hz, untrusted_app). Do NOT regress gpushim's glmark2 path
> (everything here is OPT-IN behind `/data/local/tmp/.alr-angle`).

---

## 1. Why es2gears (the cleanest first real-app rung)

`es2gears` is the GLES 2.0 spinning-gears demo from `mesa-demos`:

* **genuinely GLES2** — ES 2.0 shaders; `glGetString(GL_RENDERER)` reports the live GL
  backend, so on ANGLE→Vulkan→Mali it prints `ANGLE (... Mali-G615 ... Vulkan ...)`
  and `software=false`;
* **tiny** — a single ~68 KiB ELF, no asset files (far lighter than glmark2);
* **DT_NEEDED `libEGL.so.1` + `libGLESv2.so.2`** (verified host-side) — the exact
  SONAMEs our SYSTEM ANGLE ships at (`/usr/lib/androlinux-angle`, first on
  LD_LIBRARY_PATH under ALR_ANGLE), so es2gears binds ANGLE's EGL/GLES, not any other.

The noble `mesa-utils-bin` .deb suffixes the demo binaries with the multiarch triplet
and ships TWO es2gears flavours (both confirmed by `tools.elf_needed`):

| binary | DT_NEEDED (beyond libm/libc/ld) | display path |
|---|---|---|
| `es2gears_wayland.aarch64-linux-gnu` | libGLESv2.so.2, libEGL.so.1, libwayland-egl.so.1, libwayland-client.so.0, libxkbcommon.so.0, **libdecor-0.so.0** | **Wayland window** → `wl_egl_window` → ANGLE `DisplayVkWayland` on the in-app compositor → **ON SCREEN** |
| `es2gears_x11.aarch64-linux-gnu` | libGLESv2.so.2, libEGL.so.1, libX11.so.6 | X11 window → needs Xwayland |

Under the ALR_ANGLE env (DISPLAY unset + `XDG_SESSION_TYPE=wayland`), **es2gears_wayland
is the on-screen target**; es2gears_x11 is the Xwayland alternate.

---

## 2. The es2gears overlay (`tools/build_es2gears_overlay.py`)

DT_NEEDED-minimal §5-E overlay built on `deb_closure.build_minimal_overlay` (the same
engine as the Qt6/SDL2 GUI demos), then a **Mesa-GL strip**, then the convenience
symlinks, then `.so`→0o755, then overlay_guard + stage_tar_spec validation.

**The Mesa-GL strip is load-bearing.** `mesa-utils-bin` Depends on `libegl1`/`libgles2`/
`libgl1` + `libvulkan1`, so the closure pulls Mesa's libglvnd `libEGL.so.1` /
`libGLESv2.so.2` / `libGL.so.1` / `libGLX.so.0` / `libGLdispatch.so.0` + a second
`libvulkan.so.1`. The base ships NONE of those SONAMEs, so base-subtraction does NOT
drop them — if they shipped, es2gears could bind **Mesa swrast (SOFTWARE)** and defeat
the proof. The builder strips all of them (`MESA_GL_STRIP_PREFIXES`) so:

* `libEGL.so*` / `libGLESv2.so*` → ANGLE owns them (androlinux-angle, first on the path);
* `libGL.so*` / `libGLX.so*` / `libGLdispatch.so*` → desktop-GL glvnd; only the
  glx*/eglgears siblings link them, NOT es2gears;
* `libvulkan.so*` → the Khronos Vulkan-Loader is owned by the **vk-loader** overlay
  (`/usr/lib/androlinux/libvulkan.so.1` + `alr_icd.json` → our Mali ICD).

**Host build verdict** (noble main+universe, real .deb): overlay PASS — 26 files,
`es2gears_wayland`/`es2gears_x11` ELFs present, launch symlinks present,
`libdecor-0.so.0`/`libxkbcommon-x11.so.0`/`libxcb-xkb.so.1` KEPT at 0o755, **6 Mesa GL
libs stripped, 0 Mesa leftover, 0 missing soname, overlay_guard OK, stage_tar_spec
CONFORMANT.** Final tar `/usr/lib/aarch64-linux-gnu/` carries ONLY libdecor + libxkb
helpers — zero libEGL/libGLESv2/libGL/libvulkan.

Convenience symlinks added (RELATIVE, in-dir): `/usr/bin/es2gears` →
`es2gears_wayland.aarch64-linux-gnu` (the bare name = the on-screen flavour),
`/usr/bin/es2gears_wayland`, `/usr/bin/es2gears_x11`.

Build it:

```sh
# from repo root, host
python -m tools.build_es2gears_overlay \
  --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar \
  --out  out/v2-stage/es2gears-stage.tar
# offline shape selftest (no network):
python -m tools.build_es2gears_overlay --selftest
```

---

## 3. The launch wiring (generalized `MainActivity.launchAngleGlesProbe`)

`launchAngleGlesProbe` already stages the angle + vk-icd + vk-loader overlays and runs
the bespoke clients under `ALR_ANGLE=1` + `ALR_VK_ICD=1`. It is GENERALIZED two ways:

1. **staging**: it now also stages `es2gears-stage.tar` (via `stageOverlay("es2gears")`
   and the `.alr-angle` onCreate thread), idempotent under the same `gpuOverlayStageLock`.
2. **arbitrary-app run**: a SECOND marker `/data/local/tmp/.alr-angle-app` names an
   arbitrary in-rootfs binary (its first non-empty line = the path; default
   `/usr/bin/es2gears_wayland`). After the bespoke clients run, the probe runs THAT app
   through the IDENTICAL `runAngle()` helper — same env, same loader entry. Guarded:
   it runs only if the named binary actually exists in the rootfs (a stale/typo'd path
   is a logged skip, never an unintended exec). The readiness gate (`haveRunTarget()`)
   accepts es2gears as a valid run target even when no bespoke client shipped.

The ANGLE env (set by `launchAngleGlesProbe`, applied by `runtime_report.cpp`, purely
env-driven — program-agnostic):

| var | value | effect |
|---|---|---|
| `ALR_ANGLE=1` | | prepend `/usr/lib/androlinux-angle` AHEAD of `/usr/lib/androlinux` on LD_LIBRARY_PATH → ANGLE's libEGL.so.1/libGLESv2.so.2 win; also flips gpu_accel (ring attach) |
| `ALR_VK_ICD=1` | | put the Khronos loader + our ICD on the path + attach the VK rings + export `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` → `alr_icd.json` |
| DISPLAY | **unset** | ANGLE's `DisplayVkXcb` would `xcb_connect()` (no X) BEFORE vkCreateInstance; unsetting it + `XDG_SESSION_TYPE=wayland` → ANGLE selects `DisplayVkWayland` on `WAYLAND_DISPLAY=wayland-0` (the in-app compositor) |
| `XDG_SESSION_TYPE=wayland` | | the Wayland WSI selector (set under ALR_ANGLE) |

So `es2gears_wayland` → `wl_display_connect()` (in-app compositor) → ANGLE
`DisplayVkWayland` → VK swapchain → our `libvulkan.so.1` ICD → Mali. **On screen.**

---

## 4. Device-test procedure

Prereq overlays on the host (built green, see §2 + the angle/vk builders):

```sh
python -m tools.build_angle_overlay     --out out/v2-stage/angle-stage.tar      # ANGLE libEGL/libGLESv2 (+ optional clients)
python -m tools.build_vk_icd_overlay    --out out/v2-stage/vk-icd-stage.tar     # our renamed Mali ICD libalr_mali_icd.so
python -m tools.build_vk_loader_overlay --out out/v2-stage/vk-loader-stage.tar  # Khronos loader + alr_icd.json
python -m tools.build_es2gears_overlay  --out out/v2-stage/es2gears-stage.tar   # es2gears + libdecor (Mesa-GL stripped)
```

Push the APK + overlays + markers, then launch (force-stop FIRST — onCreate overlays +
probes are skipped on a warm process; see memory "device-test: force-stop first"):

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk

# overlays
for t in angle vk-icd vk-loader es2gears; do
  adb push out/v2-stage/$t-stage.tar /data/local/tmp/$t-stage.tar
done

# opt-in markers (NEVER present on a normal user device — strict no-regression)
adb shell 'echo 1 > /data/local/tmp/.alr-angle'                                   # arm the ANGLE proof
adb shell 'echo /usr/bin/es2gears_wayland > /data/local/tmp/.alr-angle-app'       # the REAL app to run

# force-stop, then cold launch
adb shell am force-stop dev.chanwoo.androlinux
adb logcat -c
adb shell am start -n dev.chanwoo.androlinux/.MainActivity

# capture the loader evidence
adb logcat -v time | grep -E 'alr_loader|alr-icd|angle-gles|es2gears'
```

(For the X11 alternate, `echo /usr/bin/es2gears_x11 > /data/local/tmp/.alr-angle-app`
and ensure the Xwayland overlay/`DISPLAY=:0` path is up — but the wayland flavour is the
primary on-screen rung under the ALR_ANGLE env.)

---

## 5. PASS criteria (the gate)

In logcat tag `alr_loader`, the `angle-gles[angle-app:es2gears_wayland.aarch64-linux-gnu]`
lines must show:

1. **GUEST EXEC PASS** — `ALR NATIVE LOADER GUEST EXEC: ... PASS` (es2gears loaded +
   ran through the ALR loader; no ELF-entry SIGSEGV; the `fault pc@<so>+off` field empty).
2. **ANGLE Vulkan/Mali renderer** — a `GL_RENDERER=` line containing **`ANGLE`** AND
   **`Vulkan`** AND **`Mali`** (e.g. `ANGLE (ARM, Mali-G615 ... Vulkan 1.3 ...)`).
   This is the `software=false` proof: NOT `SwiftShader` / `llvmpipe` / `lavapipe` /
   `softpipe`.
3. **frames advance** — es2gears prints its periodic `N frames in 5.0 seconds = M FPS`
   line; M > 0 (the gears are spinning). The ICD `[alr-icd]` trace shows the per-frame
   VK submit/present entrypoints firing.
4. **on screen** — the gears render in the app's SurfaceView via the compositor
   (es2gears_wayland → `wl_egl_window` → `DisplayVkWayland` swapchain). Capture a
   `adb exec-out screencap -p > es2gears.png` frame.

Record the result as a `docs/evidence/<date>-device-...-es2gears-angle.md` (matching the
existing device-evidence files) with the logcat excerpt + screencap.

### Honest dependency / fallback

* **On-screen (rung A)** needs ANGLE's `DisplayVkWayland` swapchain to bind our ICD's
  `VK_KHR_wayland_surface` + `vkCreateSwapchainKHR`/present on the in-app compositor.
  That present path is the **concurrent agent's ICD fix** (the ICD is being completed so
  ANGLE actually renders). This plan is READY: when the ICD presents, es2gears_wayland's
  swap loop drives it with zero further wiring.
* **Offscreen (rung B, fallback if the swapchain path isn't ready)**: es2gears ALWAYS
  creates a window — there is no offscreen es2gears. The offscreen ANGLE-on-Vulkan proof
  is the existing `alr-angle-vk` client (surfaceless FBO render, already device-targeted
  by the same probe and run BEFORE es2gears). So the evidence chain is: `alr-angle-vk`
  proves ANGLE→Vulkan→ICD→Mali **render** offscreen (no WSI); es2gears_wayland proves the
  same on a **REAL app** the moment the WSI/swapchain present lands. If es2gears reaches
  `GL_RENDERER=ANGLE(Vulkan/Mali)` + `glClear`/draw but the swapchain present errors, log
  that as the precise remaining ICD entrypoint (the `[alr-icd] TRAP CALLED <name>` line,
  armed via `ALR_ICD_TRAP=1` in the probe) — that is the documentary boundary, and the
  offscreen render is still the standing proof.

---

## 6. Host verification done (this deliverable)

* `tools/build_es2gears_overlay.py` — offline `--selftest` ALL PASS; NETWORK build PASS
  (es2gears + libdecor staged, Mesa GL stripped, guard OK, §5-E CONFORMANT).
* `tests/test_build_es2gears_overlay.py` — strip/symlink/x-bit + the
  MainActivity/runtime_report wiring guards (all PASS, 37 in the angle+es2gears+PartB
  set, 419 in the broader gpu/overlay subset).
* `./gradlew :app:assembleDebug` — **GREEN** (Kotlin + native).
* Version stamp (163) UNTOUCHED. ICD phys-device files / host_service / guest_icd
  UNTOUCHED (concurrent-agent ownership). Changes scoped to `tools/` +
  `MainActivity.launchAngleGlesProbe` (no runtime_report env change needed — the ANGLE
  env is already program-agnostic and generalizes to a real app as-is).
```
