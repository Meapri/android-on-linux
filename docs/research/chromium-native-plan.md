# Goal 2 — Universal ARM64 Linux apps at native speed on Android; proving target: Chromium

Status: planning (4 scouts in flight). Date: 2026-06-01. Supersedes the Goal-1 GIMP/GPU-cube track
(Phase 6 reached: GIMP usable by touch v111; GPU command pipeline present-to-screen v119).

## The goal
Run **arbitrary ARM64 glibc Linux applications** inside the non-root Android APK runtime (ALR) at
**native CPU + GPU speed** — in-process, no emulation, public Android APIs only. Goal-1 proved the
shape on GIMP (GTK/Cairo CPU app + the GPU command pipeline). **Goal-2's proving target is
Chromium** — deliberately the hardest general case: it exercises every axis at once.

Why Chromium is the right stress test (each is a generality test the runtime must pass):
- **Multi-process**: browser + zygote + renderer + GPU + utility processes → the loader must launch
  and supervise *many* guest processes (fork/clone/execve), not one map+jump.
- **IPC**: Mojo over unix sockets + shared memory (memfd) + SCM_RIGHTS fd passing.
- **JIT / W^X**: V8 generates executable code at runtime → the W^X/execmem layer must serve it
  (or `--jitless`), under Android `untrusted_app` execmem limits.
- **Sandbox**: Chromium ships its own seccomp-bpf + namespace + setuid sandbox → must coexist with
  ALR's own seccomp path-mediation (stacked filters) or be relaxed without root.
- **GPU**: the GPU process drives GLES/Vulkan → connects to the `alr_gpu` shim + Mali executor.
- **Scale**: hundreds of .so + dlopen plugins, ICU/locale/pak data, fonts, dbus, audio.

If Chromium runs, "universal ARM64 Linux app" is essentially proven.

## Hard constraints (unchanged from Goal-1)
Non-root; public Android APIs only (no /dev/dri, KMS, vendor-private); W^X-safe; PRoot fallback-only;
preserve user/generated changes; **device evidence for every milestone**; address user as 찬우야,
Korean 반말, minimal emoji. Commit/push when asked; repo public at github.com/Meapri/android-on-linux.

## Phased plan (DRAFT — scouts will pin specifics)
- **Phase 0 — recon (in flight):** acquisition path · process/sandbox/JIT model · Ozone/GPU display ·
  ALR-vs-Chromium gap. (4 background agents.)
- **Phase A — acquire + stage:** get a Chromium arm64 glibc build into `files/rootfs/debian-arm64`
  (likely `apt install chromium` in the Debian rootfs, or a prebuilt). Verify the ELF + interpreter +
  dep closure resolve under the guest ld.so. *(scout 1)*
- **Phase B — first boot, minimal:** launch the browser process via the ALR native-exec loader with
  the minimal flag set (`--no-sandbox --single-process --headless` or `--jitless` if W^X forces it).
  Milestone = Chromium reaches a known-good state on device (e.g. `--version`, or `--headless
  --dump-dom`/screenshot of about:blank). *(scouts 2, 4)*
- **Phase C — multi-process:** guest `fork/clone/execve` of zygote/renderer/gpu supervised by the
  loader; Mojo IPC (sockets + memfd + SCM_RIGHTS) working guest-side. *(scouts 2, 4 — likely the
  single biggest blocker.)*
- **Phase D — GPU:** GPU process uses the `alr_gpu` GLES shim (`--use-gl=…`) → Mali via the executor.
  Reuses the v119 present pipeline. *(scouts 3, 4)*
- **Phase E — on screen:** Ozone Wayland backend → the in-app Wayland compositor (add any missing
  wl interfaces) → real page rendered on the SurfaceView. *(scout 3)*
- **Phase F — usable:** input, fonts (fontconfig), network, dbus stub/relax, audio. A navigable page.

## Recon findings

### Scout 4 — ALR capability gap (LANDED)
Six areas audited against Chromium needs (anchors in `runtime_report.cpp`):
- **① multi-process exec re-entry — CRITICAL GAP, the single biggest blocker.** The loader is a
  single map+jump (`alr_enter_guest` ~:1811, `[[noreturn]]`). When a guest fork()s and the child
  execve()s, the kernel replaces the child's address space and the ALR loader is NOT re-invoked —
  the child runs UNMEDIATED (no path rewrite, no interposer). Chromium's zygote forks+execs
  renderers constantly. The "execve trap point" (~:1202/:1882) is a FUTURE hook, not implemented.
  → **Phase B sidesteps it with `--single-process`.** Phase C must build real exec re-entry (~200h).
- **② dynamic linking at scale — WORKS.** Guest ld.so + LD_LIBRARY_PATH (~:1466) loads the full
  DT_NEEDED closure natively; GIMP's ~290 .so already proven (v79). Chromium's closure is finite.
- **③ W^X / JIT — PARTIAL.** Anonymous mmap RW→mprotect RX→call PASSES on device (`build_execmem_probe`
  ~:766); **memfd-execveat is BLOCKED (EACCES)** (~:993, SELinux). V8's ITERATIVE RW↔RX cycling is
  UNTESTED. → start Phase B with `--jitless`; a JIT-cycle probe (below) decides if we can drop it.
- **④ seccomp stacking — FUNCTIONAL but conflict-prone.** Filters stack most-restrictive-wins; if
  Chromium installs its own sandbox filter it may DENY syscalls ALR wants to TRACE. → Phase B uses
  `--no-sandbox`; revisit virtualization in Phase C.
- **⑤ IPC — PARTIAL.** AF_UNIX sockets PASS (`build_unix_socket_probe` ~:672); SCM_RIGHTS fd-passing
  UNTESTED (supervisor may misread ancillary data); memfd-exec dead. single-process avoids most.
- **⑥ GPU ring — single-child only.** One SPSC ring, one consumer; multi-renderer would interleave.
  → single-process avoids it; Phase D needs per-renderer ring tagging or N-rings.

**Verdict:** Phase B = `--no-sandbox --single-process --jitless --headless` sidesteps ALL six gaps and
proves the loader at Chromium scale (~20h). Then the gaps surface in order: exec re-entry (C) is the
mountain. Least-risk first milestone confirmed.

### Scout 1 — acquisition (LANDED)
`apt install chromium` (or `chromium-shell` for headless-only) into the Debian arm64 rootfs is the
realistic path: native arm64 glibc, dep closure auto-resolved. Versions: **bookworm** chromium 148
needs glibc ≥2.35; **trixie** needs glibc ≥2.38. Sizes ~252 MB (bookworm) / ~276 MB (trixie); the
headless `chromium-shell` is smaller. **Official Google Chrome arm64 .deb = announced Q2 2026 but
still 404** (poll `dl.google.com/linux/chrome/deb/.../binary-arm64/Packages`). Hard deps even headless:
glibc, nss/nspr, ICU + `icudtl.dat`/`resources.pak` (mandatory data), freetype/harfbuzz/fontconfig,
expat, base x/xcb libs, libgbm. **CAVEAT: `apt`/`dpkg` themselves fork+exec → they hit the exec-re-entry
blocker; the apt path needs multi-process first, OR stage Chromium by host-extracting the .deb tree
(`dpkg-deb -x`) into the rootfs (no apt).** Non-root blockers map onto known ALR work: `--no-sandbox`
mandatory, `unshare(CLONE_NEWUSER)` EPERM (= the clone3 KNOWN_FAIL/PCGATE area), `/dev/shm` →
`--disable-dev-shm-usage`, Mali HW GL behind bionic vendor ns → software render for compute, display
still via our AHB Wayland presenter.

### Scout 2 — process/sandbox/JIT (LANDED)
Children = `fork()`+`execve()` of the SAME binary with `--type=renderer/gpu-process/utility`;
`--no-zygote` makes each a fresh exec (easier to mediate than a zygote COW fork). **V8's W^X scheme was
REVERTED ~2023 → JIT pages are RWX again by default; anonymous `PROT_EXEC` is GRANTED to `untrusted_app`
(ART/WebView JIT need it) — so V8 JIT will very likely work, and the gate is OUR W^X layer, not
Android.** `--jitless` is the fallback (~40–80% slower, no WebAssembly). **seccomp filters stack
most-restrictive-wins and a child can't loosen a parent → Chromium's own seccomp could kill the
syscalls our path-mediation traps; disabling it (`--no-sandbox`) is MANDATORY, not optional.** Mojo IPC
= AF_UNIX + SCM_RIGHTS fd passing + memfd shared memory (needed at Stage 2; `--single-process` avoids
all of it). The real Stage-2 blocker is **`execve` of our own Chromium ELF** — the same native-ELF
re-exec problem the loader already solves for launch; route child launch through it + `--no-zygote`.

### Scout 3 — Ozone/display/GPU (LANDED)
- **Stage 1 (first pixels, no GPU/compositor):** `--ozone-platform=headless --ozone-dump-file=out.png`
  → a correct PNG. Or `--headless --use-angle=swiftshader --screenshot` (SwiftShader JITs → needs
  execmem too; on Arm its WebGL is off unless `--enable-unsafe-swiftshader`).
- **Stage 2 (on-screen, sw buffers):** `--ozone-platform=wayland` to our compositor; pixels arrive as
  `wl_shm`. **Mandatory wl interfaces (else Chromium won't open a window): `wl_compositor`, `wl_shm`,
  `xdg_wm_base` (stable xdg-shell).** Strongly-needed: `wl_seat` (input) + `wl_output` (sizing). Likely
  ADDITIONS to our compositor vs the GIMP set: `xdg_wm_base` + `wl_shm`. Keyboard needs an xkb keymap
  fd; touch via `wl_touch`. Fonts via fontconfig (ship ≥1 font). **D-Bus NOT required** (warns, runs).
- **Stage 3 (GPU via our shim):** `--use-gl=angle --use-angle=gles-egl` → ANGLE `dlopen`s the system
  `libEGL.so.1` → **our shim is discovered purely via `LD_LIBRARY_PATH`** (sonames must be exactly
  `libEGL.so.1`/`libGLESv2.so.2` + unversioned symlinks). **Do NOT satisfy Chromium's GBM/dmabuf/DRM
  path** (needs /dev/dri) — keep `zwp_linux_dmabuf_v1` UN-advertised so swaps stay `wl_shm`, and let
  our AHB presenter composite. True zero-copy (AHB↔dmabuf) is the later endgame.

### Synthesized Phase B command (sidesteps all 6 gaps)
`chromium --headless=new --no-sandbox --single-process --no-zygote --disable-gpu
--disable-dev-shm-usage --ozone-platform=headless --ozone-dump-file=/<writable>/out.png
--user-data-dir=/<writable> https://example.com`  (NO `--jitless` — the W^X probe below confirmed
full V8 JIT works on device). Success = a correct PNG of a real page rendered in-process.

### JIT W^X cycle probe — DEVICE-VERIFIED (v120): `--jitless` NOT needed
`alr_jit/alr_jit_probe.hpp` `run_jit_wx_cycle_probe()`, wired as `nativeJitWxProbe`. Device PASS on
SM-X236N (Mali-G615, `untrusted_app`):
```
ALR JIT WX CYCLE: PASS
cycles_ok=8/8            (iterative RW↔RX cycle, correct return each time)
concurrent_rx_ok=4/4     (multiple code pages RX-callable at once)
rwx_mmap_ok=true (errno 0)   (direct mmap(PROT_READ|WRITE|EXEC) works — V8's current default path)
wx_granularity_ok=true   (one page RX-executing while another is RW-written)
VERDICT: V8 JIT viable without --jitless
```
So Chromium runs with **full V8 JIT (and WebAssembly), no `--jitless`** — the gate (our W^X layer +
untrusted_app execmem) is OPEN, matching Scout 2's "W^X reverted ~2023 → RWX default; execmem granted
to untrusted_app". Drop `--js-flags=--jitless` from the Phase-B command. Evidence:
`docs/evidence/2026-06-01-device-SM-X236N-v120-jit-wx-cycle.md`.

## Open questions the scouts answer
- Simplest acquisition (apt vs prebuilt) + dep/size reality. *(1)*
- Exact staged flag plan to boot non-root without nested-namespace privileges. *(2)*
- Does V8 need RWX, and does ALR's W^X layer serve it, or is `--jitless` required? *(2, 4)*
- Does Chromium's seccomp sandbox stack-conflict with ALR's filter? *(2, 4)*
- Which Wayland interfaces our compositor must add for Ozone-wayland. *(3)*
- How the GPU process discovers our `libEGL.so.1`/`libGLESv2.so.2`. *(3, 4)*
- Biggest single blocker + least-risk first milestone. *(4)*

## Reuse from Goal-1 (already device-proven)
Native-exec glibc loader; guest ld.so dynamic linking; Debian arm64 rootfs at
`files/rootfs/debian-arm64`; W^X execmem layer; PCGATE seccomp path mediation; in-app Wayland
compositor on a SurfaceView; the `alr_gpu` GLES shim + Mali executor + AHB zero-copy present (v119).
