# WS-5 DEVICE-REQ protocol & CP status dashboard

## WS-5 DEVICE-REQ protocol

Per orchestration plan §9 (Device Lease Protocol): there is exactly **one** device
(SM-X236N, `R5KL20B6S3X`), owned and serialized by the **integration session**. If
multiple sessions install/force-stop/logcat concurrently they clobber each other's
runs, so the device is single-owner.

WS-5's standing rules (§9.1, §9.6):

- **WS-5 is host-only.** It never installs an APK, never runs `adb install` /
  `am force-stop` / `am start`. It validates on the host (`uvx pytest tests/ -q`) and
  parses the *merged build's* logcat evidence to drive `bench` and the regression gate.
- **Requesting a device run.** When WS-5 needs a fresh device capture it (a) merges to
  `main` (`git merge-tree` clean + host gate pass) and (b) puts a `DEVICE-REQ:` line in
  the **merge-commit message** naming the markers / PASS conditions it wants. The
  integration session batches all sessions' `DEVICE-REQ`s into one build, runs them on
  the device, and drops the logcat capture into `docs/evidence/`.
- **The integration session bumps the version stamp and runs the device.** WS-5 never
  touches the version stamp or pin-tests.

### Copy-paste-ready DEVICE-REQ lines (WS-5's current needs)

CP-3 % ratio unblock (native vs ALR CPU overhead — the blocker for the % figure):

```
DEVICE-REQ: native(adb shell) + PRoot ns_per_op for /data/local/tmp/microbench compute and syscall (vs ALR loader exec) — to compute native-vs-ALR overhead %
```

CP-2 GPU (glmark2 on Mali, for the bench gpu accel ratio):

```
DEVICE-REQ: glmark2-es2 on Mali — capture "glmark2 Score: N" and "GL_RENDERER: <Mali...>" (software=false) for bench gpu ratio
```

CP-4 present (zero-copy dmabuf present path):

```
DEVICE-REQ: zwp_linux_dmabuf zero-copy present markers (AHB→EGLImage, no CPU readback)
```

## CP status dashboard (auto-generated, keyword heuristic)

Regenerate with:

```
cd /Users/naen/Documents/alr-ws5 && PATH="$HOME/.local/bin:$PATH" \
  uvx --with pytest python -c "from bench.cp_status import scan_cp_status, build_cp_dashboard_markdown; from pathlib import Path; print(build_cp_dashboard_markdown(scan_cp_status(Path('docs/evidence'))))"
```

Note: this is a **keyword heuristic** over evidence filenames/titles only (see
`bench/cp_status.py`). A "yes" means at least one doc *mentions* the topic — it is
**not** a PASS/FAIL sign-off. The evidence documents themselves are authoritative.
The table below is the current output; it is regenerable from the command above.

# Checkpoint Status Dashboard (keyword heuristic)

Heuristic only: filename/title keyword match, NOT an authoritative PASS/FAIL sign-off. A 'yes' means at least one evidence doc *mentions* the topic.

| Checkpoint | device evidence? | count | matching docs |
| --- | --- | --- | --- |
| CP-0 | no | 0 |  |
| CP-1 | yes | 5 | 2026-05-31-device-SM-X236N-v82-wayland-compositor-up.md, 2026-06-01-device-SM-X236N-v113-ahardwarebuffer-zerocopy-proven.md, 2026-06-01-device-SM-X236N-v87-interactive-toolkit-pacing.md, 2026-06-01-v126-harfbuzz-fix-display-90hz.md, 2026-06-01-v127-xkb-config-root-gui-keymap-segv-fixed.md |
| CP-2 | yes | 3 | 2026-06-01-device-SM-X236N-v115-gpu-native-m1m2-mali.md, 2026-06-01-device-SM-X236N-v117-gpu-native-ahb-rendertarget.md, 2026-06-01-device-SM-X236N-v118-gpu-native-live-integration.md |
| CP-3 | yes | 7 | 2026-05-31-device-SM-X236N-v77-seccomp-path-mediation.md, 2026-05-31-device-SM-X236N-v78-loader-path-mediation.md, 2026-06-01-device-SM-X236N-architecture-cpu-gpu-verification.md, 2026-06-01-device-SM-X236N-v112-present-throttle-interposer-measured.md, 2026-06-01-seize-mt-supervisor.md, 2026-06-01-ws1-m2-cpu-mediation-overhead.md, 2026-06-01-ws5-cpu-overhead-quantified.md |
| CP-4 | yes | 5 | 2026-06-01-device-SM-X236N-v112-present-throttle-interposer-measured.md, 2026-06-01-device-SM-X236N-v113-ahardwarebuffer-zerocopy-proven.md, 2026-06-01-device-SM-X236N-v114-ahb-zerocopy-present-live.md, 2026-06-01-device-SM-X236N-v118-gpu-native-live-integration.md, 2026-06-01-device-SM-X236N-v119-gpu-screen-cube-present.md |
| CP-5 | yes | 2 | 2026-06-01-device-SM-X236N-v83-pixman-toolkit-window.md, 2026-06-01-device-SM-X236N-v87-interactive-toolkit-pacing.md |
| CP-6 | yes | 5 | 2026-06-01-device-SM-X236N-chromium-runs-inprocess.md, 2026-06-01-device-SM-X236N-v120-jit-wx-cycle.md, 2026-06-01-device-supervisor-fastpath-noregression.md, 2026-06-01-mt-ptrace-hardening.md, 2026-06-01-seize-mt-supervisor.md |

## What WS-5 does when each lands

- **native+PRoot baseline lands** → run `bench overhead` to compute the native-vs-ALR % ratio and update the CP-3 CPU-overhead evidence doc.
- **glmark2 score lands** → run `bench gpu` (ALR-vs-Mali-direct ratio, software-renderer gate) for CP-2.
- **dmabuf zero-copy present markers land** → parse them into the CP-4 zero-copy present evidence.
