# Device Evidence — PTRACE_SEIZE multithread supervisor (v124): merged; GIMP/--version no-regression; chromium render is a ptrace-overhead wall

The supervisor's ptrace attach was converted from classic PTRACE_TRACEME to **PTRACE_SEIZE +
PTRACE_EVENT_STOP/PTRACE_LISTEN** (plus a new-clone-child disambiguation) — the standard, correct
multithread-ptrace shape that removes the group-stop deadlock at its root. It is GIMP/`--version`
no-regression on device and merged to main. It does NOT make Chromium `--dump-dom` usable: that is
blocked by a deeper, architectural ptrace-overhead wall (documented below). Device SM-X236N, APK
`0.4.124-seize-mt-supervisor-v124`.

## What changed (runtime_report.cpp `build_native_loader_probe`)
- **Attach**: child fork + a **pipe-gated handshake** (child blocks on `read(go_pipe)` before
  map+jump; parent `PTRACE_SEIZE`es it — options applied ATOMICALLY at attach, zero un-optioned window
  — then writes the go byte). Stronger than the old TRACEME stop→SETOPTIONS gap.
- **PTRACE_EVENT_STOP split**: a stop from a **never-seen tid** = a freshly-cloned child's initial
  stop → `PTRACE_CONT` (resume); a stop from a **known tid with GETSIGINFO==EINVAL** = a real
  group-stop → `PTRACE_LISTEN` (park, never spuriously run). This unambiguous split (impossible under
  TRACEME) is the group-stop-deadlock fix. `known_tids` set tracks seen tids.
- EVENT_SECCOMP path-mediation (+ fd/translate caches), SIGSYS emulation, fault capture, and the
  group-stop signal suppress are all preserved.

## Device-verified — no regression
- **GIMP path-mediation**: `/bin/dynhello` exit=0 (alr-dyn-ok), `/usr/bin/env`/`/usr/bin/id`/`/bin/dash`
  (x2)/`/bin/alr-png-test` exit=0, all `traps=0 rewrites=0` — identical to the TRACEME baseline.
- **chromium --version**: `ALR NATIVE LOADER GUEST EXEC: PASS`, `guest stdout=Chromium 147.0.7727.137`.
SEIZE attach + the EVENT_STOP/LISTEN path are inert for single/few-thread guests (they raise no
group-stops and produce no extra clone tids), so behavior is identical.

## The wall — chromium --dump-dom (render) is NOT fixed, and why
`chromium --dump-dom` (V8 + ~22 worker threads) still does not progress to usable render. Measured the
guest child at **Threads=1, utime crawling 1→5, state oscillating R/D/t** — it never reaches its first
`clone()` of the worker threads. group-stop deadlock was the SEIZE target and is addressed, but the
remaining blocker is **ptrace-supervisor round-trip cost multiplied across Chromium's syscall storm**:
even single-thread init issues thousands of (often raw, interposer-bypassing) syscalls, each a
RET_TRACE → ptrace round-trip, so init alone is glacial. This is an **architectural property of an
in-process ptrace supervisor**, not a bug: the definitive cure (out-of-process SECCOMP_RET_USER_NOTIF,
which is ~10× cheaper per event) is incompatible with ALR's in-process model. The v122 fast-path
(fd/translate caches) only trims per-trap cost; it can't change the round-trip count.

## Honest status
- SECURED: chromium EXECUTION (`--version`, exit 0, real version) — a universal arm64 glibc app runs
  native in-process. V8 JIT viable (v120). GIMP unaffected.
- MERGED here: SEIZE multithread-ptrace hardening — correct group-stop handling + the foundation for
  any future multi-process work (Phase C), GIMP/--version no-regression.
- OPEN FRONTIER (perf backlog): chromium render speed = the in-process ptrace-overhead wall. Reaching
  usable render needs either drastically fewer guest syscalls (e.g. an in-guest interposer that catches
  Chromium's raw syscalls — but LD_PRELOAD can't hook raw `svc`), or an out-of-process supervisor — a
  model change. 277 host tests pass; all 4 ABIs build.
