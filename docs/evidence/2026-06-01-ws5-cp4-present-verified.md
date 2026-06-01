<!-- WS-5 CP-4 present-verification evidence. Consumes the real-device CP-4 markers
captured by WS-3 / integration (drain #5, v127); the verdict is produced by the
host-pure bench.present_verify model. Honest-scope kept. -->

# Evidence — WS-5 CP-4 zero-copy present VERIFIED (AHB→external-OES, gtk3demo on device)

WS-5 consumes the real-device CP-4 markers captured by WS-3 / integration in the
single-gate drain #5 and runs them through the host-pure `bench.present_verify`
model to produce a CP-4 verdict. The underlying device run is APK **v127** on
**SM-X236N** (mt6878, **Mali-G615**, Android 16); see cross-references for the
on-device capture.

## Device markers (consumed)
Captured on device (cold start, `am force-stop` → `am start`; memory:
device-test-force-stop-first) in
[`2026-06-01-drain5-cp4-dmabuf-present-single-gate.md`](2026-06-01-drain5-cp4-dmabuf-present-single-gate.md):

```
ALR AHB ZEROCOPY IMPORT: PASS
gtkdemo-result: rendered=true frames=12→13
```

- `ALR AHB ZEROCOPY IMPORT: PASS` — AHardwareBuffer → EGLImage → external-OES
  import succeeded on the real Mali-G615 (the zero-copy import path, not a CPU
  read-back).
- `gtkdemo-result: rendered=true frames=12→13` — gtk3demo's GUI was presented via
  the ws-3 `WaylandPresenter` → compositor → present, advancing the presented
  frame count from **12** to **13** with `rendered=true`.
- present model: `in-process producer thread + GL executor thread, decode→AHB-FBO
  then cross-context external-OES present to ANativeWindow`.
- The external-OES self-test `pixel=0,0,0` line is **info-only** (v114 separately
  verified external-OES display); it is not part of the CP-4 verdict.

These markers were captured **after** the single-gate merge `c6fdb7d` (APK v127),
with **no regression** versus the gtk3demo baseline.

## Verdict (`bench.present_verify`)
WS-5's CP-4 verdict requires all of:

| Check | Result |
| --- | --- |
| AHB zero-copy import | PASS — `ALR AHB ZEROCOPY IMPORT: PASS` present |
| GUI rendered | PASS — `rendered=true` (gtk3demo) |
| Frames advanced | PASS — `frames=12→13` (presented frame count strictly advanced) |
| Regression vs gtk3demo baseline | PASS — no regression after single-gate merge `c6fdb7d` (v127) |
| Overall | **PASS** — zero-copy present **VERIFIED** |

**WS-5 verdict: CP-4 AHB external-OES zero-copy present is VERIFIED** (import PASS
+ `rendered=true` + frames advanced 12→13 + no regression vs the gtk3demo
baseline).

## What this proves
- The **AHB → EGLImage → external-OES zero-copy present PATH** works on the real
  device: a decoded frame lands in an AHardwareBuffer-backed FBO and is presented
  cross-context to the `ANativeWindow` without a CPU copy.
- The gtk3demo GUI is presented end-to-end through ws-3's `WaylandPresenter` →
  compositor → present, and the presented frame count advances (12→13), confirming
  live present (not a single static blit).
- The single-gate merge introduced **no regression** to the gtk3demo present path.

## Honest scope
- This confirms the **AHB external-OES zero-copy present PATH** on device. The CPU
  copy being removed is the one in the legacy `wl_shm` present path; the verified
  path imports the AHardwareBuffer to an external-OES texture instead.
- The remaining WS-3 M2 nuance is the **guest-side `zwp_linux_dmabuf` protocol
  advertisement** — i.e. a guest app passing a dmabuf directly to the compositor.
  Today the compositor imports an AHB to external-OES; advertising
  `zwp_linux_dmabuf` so the guest hands over a dmabuf directly is the open WS-3 M2
  item and is **not** claimed verified here.
- This is a **device-marker-driven** verdict produced by a host-pure model; the
  physical render/import/present was exercised on device by WS-3 / integration
  (drain #5, v127), not re-measured here.

## Cross-references
- Device capture (single integration gate): [`2026-06-01-drain5-cp4-dmabuf-present-single-gate.md`](2026-06-01-drain5-cp4-dmabuf-present-single-gate.md)
  (ws-3 CP-4: AHB→external-OES zero-copy present + M3 input; merge `c6fdb7d`, v127).
- WS-3 CP-4 work: ws-3 **M2** (dmabuf/AHB zero-copy present; `WaylandPresenter` →
  compositor → present; the remaining `zwp_linux_dmabuf` advertisement nuance).
- 5-session plan: `docs/research/orchestration-5session-plan.md` (CP-4 = zero-copy
  present).

## Host
- `cd /Users/naen/Documents/alr-ws5 && PATH="$HOME/.local/bin:$PATH" uvx pytest tests/ -q` → green.
- WS-5 verdict model: `bench.present_verify`. Test: `tests/test_ws5_cp4_evidence.py`.
