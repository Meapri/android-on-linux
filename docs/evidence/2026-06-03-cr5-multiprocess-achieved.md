# 🎉 CR-5 stepA ACHIEVED — Chromium renders MULTIPROCESS on non-root Android (v163)

Date: 2026-06-03 · Device SM-X236N / Mali-G615 / Android 16 / untrusted_app · branch ws-1
Loader: `app/src/main/cpp/runtime_report.cpp` (build_native_loader_probe ptrace supervisor) +
`app/src/main/cpp/alr_inproc_reexec.c` (in-process re-map trampoline)
Build: `0.4.x` versionCode **163** (stamp unchanged) · marker `/data/local/tmp/.alr-crmp`
Commits: **d106c30** (device fix — GATE-1 `/proc/self/exe` subst + mmap large target) ·
**7939c37** (host SSOT — `tools/proc_self_exe_model.py` decision table + 39-case test + CR-5 plan) ·
on top of **1fa7b5b** (supervisor cross-talk fix — the common wall with galculator dpkg).

## TL;DR

chromium-headless-shell ran with `--no-zygote --renderer-process-limit=1 --dump-dom` and
**no `--single-process`** — i.e. in genuine MULTIPROCESS mode — and rendered the page. The
renderer is a **FRESH child** that re-execs `/proc/self/exe`; ALR substitutes the rootfs
chrome binary and re-maps it in-process (kernel execve = 0). This is the first time a
universal ARM64 glibc app's **multiprocess model** (browser + a fresh-execve renderer
child) runs natively under ALR on non-root Android.

The headline is not a new mechanism — it is that the **"multithread-ptrace architecture
wall"** from CR-1 was never a single architectural barrier. It dissolved into a chain of
**concrete, individually-fixed bugs** (SIGTRAP livelock → ICU → log-open → **supervisor
cross-talk**). The last link, `1fa7b5b`, is the same wall that blocked galculator's deep
dpkg chain — fixing it cleared **both** at once.

## ★ Device evidence — this ran MULTIPROCESS, not single-process (v163)

`crmp-report.txt` (supervisor-internal aggregation; no external strace):
```
ALR NATIVE LOADER GUEST EXEC: PASS              child exit=0 signal=0
inproc=on  inproc_redirected=6  inproc_skipped=0   clone_events=49
ALR-INPROC: mapped, jumping entry=0x7b7d076a40     (×6)
<html><head></head><body><h1>ALR-CRMP-OK</h1></body></html>   (rendered DOM)
chromium-CRMP ... PASS rendered-DOM-has-marker
```

Why this is multiprocess, not the CR-1 single-process path — read off the same report:

- **`--single-process` is GONE.** The probe command is `--no-zygote
  --renderer-process-limit=1 --dump-dom` only. CR-1 (the achieved baseline, v159) kept
  `--single-process`, which collapses the whole tree into one process; CR-5 removes it, so
  chromium spawns real child processes.
- **`clone_events=49`** — chromium spun up its full ~20-thread / multi-process tree
  (browser threads + a renderer process + a GPU child + utility), not a single flat
  process. CR-1's single-process run never produced this fan-out.
- **`inproc_redirected=6` + `ALR-INPROC: mapped, jumping entry=… (×6)`** — six distinct
  exec targets were caught at the exec-trap and re-mapped in-process. Under `--no-zygote`
  each child is a **fresh `execve("/proc/self/exe", …)`**; the ×6 re-maps are exactly those
  fresh-execve children (the renderer/gpu/utility), each diverted to the rootfs chrome and
  jumped into — instead of running the Android linker64 image.
- **`child exit=0 signal=0`** + the rendered DOM containing `<h1>ALR-CRMP-OK</h1>` inside a
  complete `</html>` — the multiprocess tree completed, the renderer parsed the page and
  serialized the DOM back to the browser, and the leader exited cleanly.
- **Zero WATCHDOG / SIGILL / SIGSEGV** across the whole drain — the ~20-thread / 49-clone
  storm was supervised without a single stall-watchdog firing.

This is the direct refutation of the CR-1 verdict
(`2026-06-02-cr1-chromium-supervisor-deadlock.md`), which concluded that even a 600 s
window could not get a render through "the serialized ptrace supervisor [that] deadlocks on
that thread/syscall storm." The storm did not need a multi-threaded supervisor — it needed
the cross-talk bug (below) fixed.

## The real wall was a chain of concrete bugs, not one architecture wall

CR-1 framed the blocker as a deep, architectural "multithread ptrace deadlock" requiring
either one-tracer-thread-per-tracee or fewer traced syscalls. In hindsight that single
"wall" was four separate, concrete bugs, each found by a watchdog/event-ring dump and each
fixed with a small targeted change. The chain (oldest → newest):

| # | Build | Symptom framed as "the supervisor wall" | Actual concrete bug | Fix |
|---|---|---|---|---|
| 1 | v157 | supervisor silent ~220 s, never reaps | **SIGTRAP livelock** — `brk` trap suppressed without advancing PC → re-fire 4434× | forward genuine instruction traps `PTRACE_CONT(w, SIGTRAP)` (`cr1-sigtrap-livelock-fixed`) |
| 2 | v158 | crashes ~1 s after livelock fix | **ICU data load** — `readlink("/proc/self/exe")` returned the APK path → wrong module dir | loader sets `ALR_GUEST_EXE`; interposer returns it for `/proc/self/exe` *reads* |
| 3 | v159 | post-ICU wedge runs full window, stdout=0 | **default log-file open** hangs under ALR | `--enable-logging=stderr --v=1` bypasses the log-file open → CR-1 render completes |
| 4 | **1fa7b5b** | MP tree (+ galculator dpkg) → 40 s sup-stall WATCHDOG → SIGKILL | **concurrent-supervisor `waitpid(-1)` CROSS-TALK** | add `__WNOTHREAD` so each supervisor waits only its own thread's tracees |

Each row moved the blocker forward by one concrete, named failure — none of them was the
"rewrite the supervisor architecture" change CR-1 anticipated. CR-1 itself (the engine + DOM
in single-process) was achieved at v159 (`2026-06-02-CR1-ACHIEVED-chromium-renders-in-process.md`)
once 1–3 landed; CR-5 (multiprocess) needed bug 4 plus the two MP-specific fixes below.

### Bug 4 — the cross-talk correction (the common wall with galculator)

This is the link worth stating precisely, because it is the one that was genuinely
multi-threaded-supervision-shaped and the one shared with galculator's deep dpkg chain.

`nativeAlrNativeLoaderProbe` is called from `onCreate` on **several threads concurrently**.
Each call forks a guest and runs its own `waitpid(-1, __WALL)` loop. A `waitpid(-1)` in
supervisor **A** would reap a tracee stop that actually belonged to supervisor **B**, then
`PTRACE_CONT` it → `ESRCH` (it is not A's tracee), consuming the stop. The real tracer **B**
then **never** receives that stop, and the tracee wedges in state `t` forever — which
manifested as the 40 s sup-stall WATCHDOG → SIGKILL.

Device proof (from `1fa7b5b`): `sup=19615 tid=19708` issuing 146× `CONT rc=0`, interleaved
with `sup=19614` doing `seccomp-CONT tid=19708 rc=-1 errno=3 (ESRCH)` — two supervisors
fighting over the **same** tid. Fix: add `__WNOTHREAD` to the `waitpid`, so each supervisor
waits only on tracees of its own thread (ptrace re-points a tracee's `->parent` to the
tracer thread, so a supervisor's own tracee subtree is still fully reaped).

The same wall blocked galculator's `dpkg -i` (the deep `dpkg → dpkg-deb/dpkg-split/tar →
maintainer-script sh` fork+exec chain, all in-process re-mapped). `1fa7b5b` cleared **both**:
galculator now reaches dpkg `configured=true installed=true`, and the chromium MP tree is
supervised with zero watchdog firings. The CR-1 "the supervisor deadlocks on the
thread/syscall storm" diagnosis was, at root, this cross-talk between *concurrent* supervisors
— not a limit of the *single* serial supervisor on one tree.

## GATE-1 mechanism — `/proc/self/exe` substitute + map_file mmap (commit d106c30)

Two fixes were needed on top of the supervisor being healthy, both revealed by successive
device drains. Together = 149 insertions / 22 deletions across three files
(`runtime_report.cpp`, `alr_inproc_reexec.c`, `MainActivity.kt`).

### 1. GATE-1 — the in-process re-map's self-exe SUBSTITUTE (`runtime_report.cpp`)

Under `--no-zygote`, each chromium child re-launches itself via `execve("/proc/self/exe",
…)`. The old exec-trap code **SKIPPED every `/proc/*`** path (it could not name a host file
for `/proc/self/exe`, which on Android resolves to the *Android* linker64, interp
`/system/bin/linker64`). So the fresh-execve child ran the Android loader image and crashed:
`CANNOT LINK EXECUTABLE: libandroid_runtime.so not found` — every GPU/renderer/utility child
died this way.

GATE-1 narrows the skip to **exec-target forms only** — `"/proc/self/exe"` or
`"/proc/<all-digits>/exe"` — and for those, **substitutes the launch guest's rootfs
`host_path`** (`config.rootfs_dir + guest_rel`, already in-scope at the exec trap as
predicted by the plan §1.1; all chromium children re-exec the *same* chrome binary, so a
single scalar suffices — no new variable/threading). The substituted exec then falls into
the existing in-process re-map path with `regs[19]=host_path`. Non-exe `/proc/*`
(`maps`/`cpuinfo`/`self/root`/…) are **not** exec targets and keep the conservative SKIP.
argv/envp (`x20`/`x21`) are untouched; no new ptrace op, no kernel execve.

Drain #1 then revealed a second gate re-skipping the substitute: the existing **non-rootfs**
gate classified `gp` (= `/proc/self/exe`) and saw `med.reason=="sysdir"` for `/proc/*` →
re-skip `"non-rootfs"`, undoing GATE-1. Fix: guard the stub *and* non-rootfs gates with
`!self_exe_subst`, so a resolved substitute bypasses checks that classify the literal
`/proc` path. This mirrors the host model exactly — `decide_exec_trap` returns
`ACTION_SUBSTITUTE` **before** the stub/non-rootfs checks.

Host SSOT (commit 7939c37, no device): `tools/proc_self_exe_model.py`
(`is_self_exe_target` / `decide_exec_trap` decision table) +
`tests/test_proc_self_exe_gate.py` — **39 cases passed** (`test_execve_pathrw` +
`test_exec_map` = **63 total**). The decision table is the SSOT for substitute / skip /
re-map; the device commit d106c30 wired it but its model was untracked, so 7939c37 captures
it.

### 2. map_file mmap of the large target (`alr_inproc_reexec.c`)

Drain #2 then showed the worker reached the rootfs chrome path but failed `target open/read
fail`: the re-map read the target ELF into a fixed `g_target_buf` of `FILEBUF_CAP = 8 MiB`,
and chrome is **~186 MiB** → `read_file` returned −1. Fix: add `map_file()` (mmap the target
`PROT_READ | MAP_PRIVATE`; `map_elf_image` reads the source ELF only through its `img`
pointer, so a file mapping is a drop-in), `munmap` after the segments are copied out, and
drop the now-dead 8 MiB `g_target_buf`. (`ld.so` is small and still uses `read_file`.)

### Probe wiring (`MainActivity.kt`)

A new MP probe gated on `/data/local/tmp/.alr-crmp`, scoping `ALR_REEXEC_INPROC=1` to the
probe (restored in `finally`) so it does not affect normal cold starts.

## No regression (same drain, cold start, v163)

| Track | Result |
|---|---|
| ALR GPU LIVE INTEGRATION | **PASS** (Mali-G615, software=false) |
| ALR VK ENUM + RENDER + DRAW MARSHAL | **PASS** |
| ALR GPU SCREEN CUBE | **PASS** (software=false) |
| Chromium 147 `--version` (in-process) | **PASS** |
| galculator `apt`/dpkg | unpacked=true **configured=true installed=true** |
| WATCHDOG / SIGILL / SIGSEGV | **0** |
| version stamp (versionCode) | **163** (unchanged) |

## Honest notes — scope of this result

- **This is `--dump-dom`, software raster, a `data:` URL — not GPU, not real network.**
  - `--disable-gpu` was set (per plan §4.1): the renderer rendered via **software
    fallback**, deliberately separating the MP exec wall from the GPU ring. CR-3 GPU is a
    later step.
  - The page was a `data:text/html,…` URL embedded in the command — **no network fetch
    happened**. This is the engine + multiprocess + DOM, not a real-URL load.
- **`--renderer-process-limit=1`** caps the child *count* on purpose (plan §4.1/risk-1): it
  exercises the GATE-1 substitute on a fresh-execve child while isolating it from the
  separate supervision-throughput wall (child *count*, distinct from per-child syscall
  rate). stepB (lifting the limit → many renderer children) is **not yet** run, so the
  serial supervisor's behavior under *many* concurrent fresh-execve children remains
  device-unverified.
- The GPU child still **crashed (exit_code=5)** during this run; because GPU is disabled the
  render completed on the software path regardless. The exit-5 GPU child is a known
  non-blocker for stepA, owned by the CR-3 lane.
- **Device-proven vs inferred:** the rendered DOM + `inproc_redirected=6` + `clone_events=49`
  + `child exit=0` are device facts (v163). That the *same* path scales to many renderers
  (stepB) and to GPU (CR-3) is **not** yet shown — those are the next rungs, not claims.

## Remaining ladder (toward a visible, networked, GPU browser)

| Rung | What it adds | Status / blocker |
|---|---|---|
| **CR-5 stepB** | lift `--renderer-process-limit` → many renderer children | next — tests serial-supervisor throughput under *many* fresh-execve children (plan §4.2 risk-1) |
| **CR-3 GPU** | drop `--disable-gpu` → GPU-accel render (ANGLE→shim→Mali) | GPU child currently exits 5; per-renderer GPU ring (single SPSC consumer) needs N-ring/tagging (`chromium-gpu-path.md`) |
| **CR-2 network** | real `https://` URL | blocked by **NETLINK socket EACCES** — Android SELinux denies NETLINK_ROUTE to untrusted_app (sandbox restriction, NOT a loader/supervisor bug; `2026-06-02-cr2-network-netlink-blocker.md`) |
| CR-4 on-screen | ozone-wayland → compositor → SurfaceView | downstream of the render path |

## Status

**CR-5 stepA: ACHIEVED.** Chromium renders in genuine multiprocess mode (a fresh-execve
renderer child, re-mapped in-process) on non-root Android. The CR-1 "multithread-ptrace
architecture wall" was a chain of four concrete bugs — SIGTRAP livelock (v157), ICU (v158),
log-open (v159), and the supervisor cross-talk (`1fa7b5b`, shared with galculator dpkg) —
all now fixed, plus the two MP-specific fixes (GATE-1 self-exe substitute + map_file mmap of
the 186 MiB target). Next: CR-5 stepB (many renderers), CR-3 (GPU), CR-2 (network) toward a
visible, networked, GPU-accelerated browser.
