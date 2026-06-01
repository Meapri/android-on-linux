# Device Evidence — SM-X236N: Chromium runs in-process via ALR (Goal-2 first proof)

The Goal-2 watershed: a real, unmodified **Chromium** ARM64 glibc binary runs natively in-process
inside the non-root Android APK via the ALR loader, prints its version, and exits cleanly. This is the
first proof that a *universal* ARM64 glibc Linux app — not a hand-tuned one — runs natively on Android
through ALR. Device SM-X236N (Mali-G615 MC2, Android 16, untrusted_app); device-verified at APK
`0.4.121-chromium-runs-inprocess-v121` (re-confirmed identical on the v121 build).

## Device-verified
```
gimp-probe guest=/usr/lib/chromium/chromium-headless-shell exit=0 sig=0 pcgate=1 interpose=1 traps=0 rewrites=0 stdout_bytes=23
chromium-boot:
ALR NATIVE LOADER GUEST EXEC: PASS
alr native loader reached=jumped-to-entry
alr native loader child exit=0 signal=0
alr native loader path-mediation traps=0 rewrites=0 (pcgate=on interpose=on)
alr native loader seccomp-emulated syscalls=1 nums=99
alr native loader guest stdout=Chromium 147.0.7727.137
```
The 186 MB `chromium-headless-shell` (Debian bookworm chromium 147.0.7727.137, arm64, ET_DYN
static-PIE) was mapped by the ALR native loader, handed to the guest ld.so, dynamically linked its
full DT_NEEDED closure from the rootfs, ran, printed `Chromium 147.0.7727.137`, and exited 0.

## How it was staged + launched (non-root, run-as blocked)
1. Host: built `chromium-stage.tar` (517 MB) = chromium-headless-shell + Chromium-specific deps
   (the dep closure the GIMP rootfs lacked) + bundled data (`icudtl.dat`, `resources.pak`, snapshots).
   `ar x` + `tar` extraction of bookworm `.deb`s (no dpkg). Pushed via `adb push` to
   `/data/local/tmp/chromium-stage.tar` (app-readable `-rw-rw-rw-`).
2. App: a `MainActivity` thread overlays the tar into `files/rootfs/debian-arm64` via
   `RootfsInstaller.extractVerifiedTar` (marker keyed on tar size → re-push auto-re-extracts), then
   launches through the existing loader: `nativeAlrNativeLoaderProbe(..., "<binary>\n--no-sandbox\n
   --version")` (the loader parses newline-delimited argv + sets LD_LIBRARY_PATH — no new JNI).
3. One dep fix was needed: `libpulsecommon-16.1.so` lives in a `pulseaudio/` subdir reached by
   libpulse's absolute RUNPATH (not resolved); a flat symlink in `usr/lib/aarch64-linux-gnu/` (on
   LD_LIBRARY_PATH) fixed it — the only non-flat .so in the closure.

## The harder case (recorded, not yet solved)
`--dump-dom about:blank` (which exercises V8 + rendering) did NOT complete quickly: the guest reached
431 MB RSS, state R, with `utime` increasing (real progress) but `stime` dominating (~70× utime),
i.e. it is progressing but heavily slowed — Chromium issues millions of syscalls during a real page
bring-up and the loader's ptrace supervisor + Chromium's own syscall weight throttle it. `--version`
is cheap (traps=0, 1 seccomp-emulated syscall) so it completes. Next milestone = a loader syscall
fast-path so the render path runs at usable speed; then `--headless --dump-dom`/screenshot, then
multi-process (the exec-re-entry blocker), then GPU + on-screen via the Wayland/AHB path.

## Significance
CPU-native execution of a browser-class glibc binary (V8 included — JIT confirmed viable at v120,
`ALR JIT WX CYCLE: PASS`) on non-root Android, in-process, public APIs only. The universal-app goal is
demonstrated at the "binary runs + links its full closure + clean exit + real output" level; the
remaining work is performance (syscall fast-path) and the multi-process/GPU/display phases (C–E).
