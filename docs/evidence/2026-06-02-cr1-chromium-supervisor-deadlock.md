# Device Evidence — CR-1 verdict: chromium --dump-dom is blocked by the multithread-ptrace SUPERVISOR DEADLOCK (not the alarm window)

Builds v147/v148/v149. Device SM-X236N / Mali-G615 / Android 16. Four cold-start drains measuring chromium single-process headless render (CR-1 of chromium-run-plan).

## What works vs what doesn't
- `chromium-headless-shell --no-sandbox --version` → **runs** (Chromium 147.0.7727.137, in-process, 186MB+~200 .so closure, `exec_ms≈200`, traps=0). Few threads, fast.
- `chromium-headless-shell --no-sandbox --single-process --no-zygote --disable-gpu --disable-dev-shm-usage --user-data-dir=/tmp/cr1-profile --no-first-run --dump-dom data:text/html,...` → **never completes**. Across **4 drains** with alarm windows of 25s, 120s, 180s, and **600s**, the `--dump-dom` never produced the rendered DOM (`ALR-CR1-OK`).

## The signature: the loader goes silent + the supervisor never reaps
Every CR-1 drain shows the same: `chromium-boot:` (--version) logs, then the CR-1 `--dump-dom` probe starts and the **alr_loader log goes completely silent for the rest of the window** (e.g. last line 16:55:32, silent for the next ~220s of a 230s capture). The `chromium-CR1` report — which logs only when the probe returns (i.e. when the guest exits or hits its SIGALRM and the supervisor `waitpid`s it) — **never appears, even past the 180s/600s alarm**. So the guest child was never reaped: **the single-threaded ptrace supervisor is stuck** (a `waitpid`/ptrace stop it cannot resolve), holding the loader's guest-launch lock → the whole loader freezes (the app process stays alive but emits no more loader logs, and the later GPU/GUI probes never run because they queue behind the held lock).

## Verdict — it is the SUPERVISION wall, not the window
PR #2 (chromium-storm) re-diagnosed the `--dump-dom` "deadlock" as likely a misdiagnosis = the 25s alarm window killing a heavy single-init before the first worker clone. These measurements **refute the window hypothesis for the render path**: a **600s** window (24× the original) still produced no render and the same supervisor freeze. `--single-process --no-zygote` (which collapses chromium's multi-PROCESS tree, sidestepping the G1 exec-re-entry wall) does NOT help, because chromium is still heavily multi-THREADED (~20 threads), and the **serialized ptrace supervisor deadlocks on that thread/syscall storm** — the long-backlogged "multithread ptrace deadlock" the runtime_report.cpp comments + memory reference (the SEIZE + group-stop/LISTEN handling reduced but did not eliminate it).

## So: the ONE remaining blocker for "Chromium actually running" = the multithread ptrace supervisor
The chromium-run enablers are all staged + host-verified (network overlay + un-mediated sockets; ozone-wayland bind gate already met; GPU-path recon; multiprocess /proc/self/exe design) — they unblock CR-2..CR-5. But ALL of them are downstream of the render path running, which is gated on the supervisor handling chromium's multi-threaded syscall storm without deadlocking. This is a deep architectural fix (candidates: a multi-threaded supervisor — one tracer thread per tracee thread, since ptrace is tracer-thread-affine; or reducing traced syscalls so chromium's hot path never round-trips the supervisor; or root-causing the specific SEIZE/LISTEN stop that wedges). It is NOT a flag/staging fix.

## Action
The CR-1 `--dump-dom` probe is **gated behind `/data/local/tmp/.alr-cr1`** so it does NOT run on normal cold starts (it would hold the loader lock for the full chromium alarm and block the GUI/GPU sequence). `--version` (the working path) still runs. The CR-1 probe + flag set are preserved for the dedicated investigation once the supervisor is fixed. chromium alarm is 180s (gated to chromium guests only).
