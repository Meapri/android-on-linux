# 🎉 CR-1 ACHIEVED — Chromium renders a page in-process on non-root Android (v159)

Date: 2026-06-02 · Device SM-X236N / Mali-G615 / Android 16 / untrusted_app · branch ws-1

## The result
```
chromium-CR1 (single-process headless render): PASS rendered-DOM-has-marker
<html><head></head><body><h1>ALR-CR1-OK</h1><div style="color:red">render</div></body></html>
gimp-probe guest=/usr/lib/chromium/chromium-headless-shell exit=0 sig=0
   traps=69 rewrites=56 stdout_bytes=3769 exec_ms=41027
```
chromium-headless-shell `--single-process --no-zygote --disable-gpu --dump-dom` of a
`data:text/html,<h1>ALR-CR1-OK</h1>…` URL **parsed the HTML, built the DOM, and dumped
it**, then exited cleanly (exit 0) in ~41s — running ENTIRELY IN-PROCESS inside the
non-root Android APK via the ALR loader (fork + ptrace supervisor + PCGATE seccomp +
LD_PRELOAD interposer). This is the **Goal-2 proof**: a universal ARM64 glibc app — the
hardest general case (V8, multi-threaded, ICU/pak data, hundreds of .so) — renders a real
web page natively on non-root Android.

## The three blockers cleared this session (the full CR-1 unlock)
1. **Supervisor "deadlock" was a SIGTRAP livelock** (v157): chromium hit a `brk`; the loader
   suppressed the SIGTRAP without advancing the PC → infinite re-trap (4434×). Fix: forward
   genuine instruction traps (`PTRACE_CONT(w,SIGTRAP)`). Diagnosed by a new stall-watchdog +
   event-ring + supervisor-self-dump.
2. **ICU data load** (v158): chromium derives its module dir from `readlink("/proc/self/exe")`,
   which returned the Android APK path → couldn't find icudtl.dat → CHECK. Fix: loader sets
   `ALR_GUEST_EXE`, interposer returns it for `/proc/self/exe` (general — helps every app).
3. **Default log-file open** (v159): adding `--enable-logging=stderr --v=1` cleared the
   intermittent post-ICU wedge (chromium's default log destination open appears to hang under
   ALR; redirecting logging to stderr bypasses it) → chromium completed the render.

## No regression (same drain, v159)
`ALR GPU LIVE INTEGRATION: PASS`, `VK RENDER MARSHAL: PASS`, `VK DRAW MARSHAL: PASS`,
`glmark2 Score: 1089`, gtk3 `gtk_init ok backend=wayland`, foot 1.13.1, Xwayland 23.2.6 —
all run alongside the chromium render. Host gate: NDK 4-ABI clean, pytest 1024, interposer
zig build clean.

## Honest notes
- chromium init showed some non-determinism across runs (v158 hit a 200s wedge; v159
  rendered in 41s). The verbose-logging flag is the most likely cause of the difference
  (log-file-open bypass); reliability across repeated runs should be re-confirmed, and the
  --enable-logging dependence understood (is it the log-file open, or timing?).
- CR-1 is headless `--dump-dom` (engine + DOM). Remaining browser-run ladder:
  CR-2 network (sockets un-mediated, overlay staged), CR-3 GPU (ANGLE→shim→Mali),
  CR-4 on-screen (ozone-wayland → compositor → SurfaceView), CR-5 multiprocess
  (exec-re-entry / zygote). All staged downstream.

## Status
**CR-1: ACHIEVED.** The Chromium rendering engine runs and renders in-process on non-root
Android. The loader/supervisor is proven on the hardest target. Next: CR-2..CR-5 toward a
visible, interactive browser.
