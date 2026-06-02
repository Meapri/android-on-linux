# Device Evidence — CP-6 M-R2 (v137): chromium runs via ALR + syscall-mix verdict = mediation-negligible (init)

Build `0.4.137-cp6-mr2-v137` (versionCode 137). Device SM-X236N / Mali-G615 MC2 / Android 16. Cold start. ADR-002 M-R2 (storm decomposition, measurement-first): the supervisor histograms syscall nrs at its two existing trap sites + emits `getrusage(RUSAGE_CHILDREN)` CPU/ctxt. Chromium un-gated (`chromium-stage.tar` pushed, 541MB).

## Milestone: chromium-headless-shell runs in-process via ALR ✓
```
chromium-stage: overlay done (extracted=201 skipped=5)   (5 = harfbuzz-downgrade entries the guard blocked)
guest=/usr/lib/chromium/chromium-headless-shell --no-sandbox --version
ALR NATIVE LOADER PARSE: PASS / MAP: PASS / GUEST EXEC: PASS
child exit=0 signal=0
guest stdout = Chromium 147.0.7727.137
```
The 186MB chromium binary + its ~200-file closure loads and runs in-process via the ALR loader (non-root, public API), `--version` exits 0. (The heavier `--dump-dom` render storm still hits the multithread-ptrace deadlock — backlogged.)

## M-R2 instrumentation — device-verified, captured for every guest
The supervisor now emits, per guest:
```
alr sc trace_hist <nr:count ...>     (RET_TRACE/EVENT_SECCOMP path-family, top-16)
alr sc emul_hist  <nr:count ...>     (SIGSYS-emulated nrs)
alr sc stime_us=.. utime_us=.. nonvol_ctxt=.. traps=.. emul=..   (getrusage RUSAGE_CHILDREN)
```
chromium `--version` line:
```
alr sc trace_hist            (empty)
alr sc emul_hist 99:1
alr sc stime_us=917552 utime_us=292542 nonvol_ctxt=379 traps=0 emul=1
```

## Verdict (bench/syscall_mix.py): MEDIATION-NEGLIGIBLE
- **traps=0, emul=1** — ALR's seccomp/ptrace mediation caused **zero RET_TRACE round-trips** and one SIGSYS-emulate (nr 99). The interposer absorbs path syscalls in-process; PCGATE RET_ALLOWs the rest. So the mediation is NOT the bottleneck.
- The 917ms stime + 379 nonvol_ctxt are **chromium's own init work** (lib loading, futex/epoll blocking) — a single round-trip cannot cause 379 context switches.
- Per ADR-002's decision branch: with no round-trip storm, **USER_NOTIF / svc-rewrite would remove ~nothing** for the init path. The round-trip question is only live for the `--dump-dom` raw-svc render storm, which is deadlock-blocked.

### classify_storm bug fixed (degenerate ratio)
The merged classifier read `ratio = nonvol_ctxt/(traps+emul) = 379/1 = 379` as `>= ROUNDTRIP_DOMINATED_MIN` → false **"roundtrip-dominated."** A round-trip produces ~2 ctxt-switches, so `ratio > ROUNDTRIP_CTXT_MAX(=4)` means the switches are the guest's own, not mediation. Added that ceiling + the `mediation-negligible` verdict + a regression test (this exact chromium data). Existing cases (ratio 2.0 → roundtrip, 0.2 → syscall-weight, 1.0/0/0 → ambiguous) unchanged. pytest 728.

## Next (per ADR-002)
M-R2's init verdict (mediation-negligible) is captured; the **storm** verdict needs `--dump-dom` to run, gated on the multithread-ptrace deadlock fix (PTRACE_SEIZE + EVENT_STOP/LISTEN tuning). Only then does the M-R5 (svc-rewrite) vs M-R1 (USER_NOTIF) A/B become decidable on the real raw-svc storm.
