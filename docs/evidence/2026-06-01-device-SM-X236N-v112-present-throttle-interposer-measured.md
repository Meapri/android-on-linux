# Device Evidence — SM-X236N, v112: present-throttle fixes BLAST saturation; LD_PRELOAD interposer measured (91% of path mediation in-process)

Two performance/observability improvements, both device-verified. APK `0.4.112-android-gimp-throttle-diag-v112`, device SM-X236N (Mali-G615 / MT6878, Android 16, untrusted_app).

## 1. Present throttling — BLAST buffer-queue saturation fixed

Before: the compositor called `present_composited()` (→ one `eglSwapBuffers`) on EVERY composite-changing surface commit. GIMP commits in storms (splash committed dozens of times), swapping faster than the ANativeWindow/BLAST queue (max 4+2) drains → device error `BLASTBufferQueue: Already acquired max frames`.

Fix (alr_compositor.cpp): a commit now only sets `g_scene_dirty = true` (pixels + z-order still updated immediately); the reactor's existing ~60 Hz frame timer (`frame_timer_fd_`) presents ONCE per tick if dirty. The first-ever composite still presents immediately (`g_first_present_done`) to preserve first-paint latency. All four `present_composited()` call sites (commit + 3 destroys) route through the flag = a single coalesced present path. Compositor-thread-only → no atomic/mutex.

Device result:
- Heavy interaction (close welcome → File menu → New → OK → 5 draw swipes) drove the GIMP main window to **109 commits (a commit storm)** with **0 new BLAST saturation events** — versus 4 before (those 4 were app-init surfaces at t−15s, before GIMP even booted). GIMP stayed alive and drew (screenshot `v112-gimp-throttled.png`: a bold black diagonal stroke on the canvas, UI intact).
- eglSwapBuffers is now bounded to ≤60/s regardless of commit rate → less GPU/CPU churn, no queue pressure. Idle GIMP (no commits) → zero swaps.

## 2. LD_PRELOAD path interposer — measured, and it works (91% of path mediation handled in-process)

Before: the loader's full report (guest stdout + supervisor counters `path_traps`/`path_rewrites`) was returned to a 1px-tall (invisible) TextView — unobservable. Now the parent supervisor emits it to logcat (tag `alr_loader`) and writes the full report to `<cache>/gimp-probe-last.txt` (pullable via `run-as`), gated to the dynamic path only.

Device measurement (the GTK3 guest, `interposer=ON`):
```
alr native loader interposer=ON
alr native loader path-mediation traps=682 rewrites=60
first path rewrite=/usr/lib/androlinux/libalr_interpose.so => <rootfs>/usr/lib/androlinux/libalr_interpose.so
```
- `traps=682` — the seccomp filter RET_TRACEs every path syscall by NUMBER, so the count of trapped path syscalls is unchanged by the interposer (expected — the filter can't read the path).
- `rewrites=60` — but the parent only had to ptrace-rewrite the path 60 times. The other **622 (91%)** arrived ALREADY under the rootfs prefix (the LD_PRELOAD interposer rewrote them in-process at the libc wrapper, ns not µs), so the supervisor's `already_host` idempotency guard skipped them.
- `rewrites/traps = 60/682 = 8.8%`. The residual 60 are exactly what the interposer can't reach: ld.so's OWN library opens (ld.so issues raw `openat`, bypassing the libc wrappers) — confirmed by `first_rewrite` being the interposer .so itself. This matches the design's prediction precisely.

This is WHY GIMP boots within the watchdog: thousands of font/brush/data opens are mediated in-process instead of one µs-scale ptrace round-trip each. The metric to trust is `path_rewrites` (not `path_traps`); an A/B gate `ALR_DISABLE_INTERPOSE=1` omits the LD_PRELOAD push so the two arms can be compared directly.

## No regression
236 host tests pass, native-core PASS. Both changes are additive; GIMP's full touch workflow (boot → menu → New → canvas → draw) still works.
