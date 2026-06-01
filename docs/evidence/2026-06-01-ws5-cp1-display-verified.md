<!-- WS-5 CP-1 display-verification evidence. Consumes a device marker captured by
WS-3/integration (v126); the verdict is produced by the host-pure
bench.display_verify model. Honest-scope kept. -->

# Evidence — WS-5 CP-1 display VERIFIED (1200x1920 @ 90Hz device-exact)

WS-5 consumes the real-device display marker captured by WS-3 / integration and
runs it through the host-pure `bench.display_verify` model to produce a CP-1
verdict. The underlying device run is v126 on SM-X236N (mt6878, Mali-G615,
Android 16); see cross-references below for the on-device capture.

## Device marker (consumed)
Captured on device (cold start, `am force-stop` → `am start`) in
[`2026-06-01-v126-harfbuzz-fix-display-90hz.md`](2026-06-01-v126-harfbuzz-fix-display-90hz.md):

```
display: 1920x1200 @ 90000mHz density=213
```

This authoritative marker is emitted by the compositor once it has the real panel
geometry (MainActivity forwards `Display.getRealSize` + `refreshRate` via JNI;
compositor drives the `wl_output` mode + timerfd at 90Hz). It carries BOTH:
- **resolution** `1920x1200` (landscape — a rotation of the device-exact portrait
  panel `1200x1920`), and
- **refresh** `90000 mHz` == **90 Hz**.

`density=213` is the reported display density (informational; not gated).

## Verdict (`bench.display_verify.verify_from_report`)
`verify_from_report` prefers the `display:` marker (which carries refresh), so the
verdict has refresh VERIFIED — not "unverified":

| Check | Result |
| --- | --- |
| Resolution | PASS — `{1920,1200}` == device-exact `{1200,1920}` (rotation-agnostic) |
| Refresh | PASS — 90000 mHz → 90 Hz == expected 90 Hz (**VERIFIED**) |
| Overall | **PASS** (`passed=True`) |

Programmatic outcome:
- `resolution_ok = True`
- `refresh_ok = True`
- `refresh_verified = True`
- `passed = True`

## What this proves
- CP-1 device-exact display is confirmed: **1200x1920 @ 90Hz** (panel-exact,
  rotation-agnostic match against the landscape `1920x1200` advertisement).
- Refresh is now VERIFIED end-to-end (device → marker → host model), upgrading the
  prior "refresh unverified — no report marker" state.

## Honest scope
- This is a **device-marker-driven** verdict produced by a host-pure model; the
  physical render/timing was exercised on device by WS-3 / integration (v126),
  not re-measured here.
- The per-client `client bound: wl_output v2 (1200x1920 px, ... dpi=440)` line is a
  **separate, also-supported** format. It carries resolution + DPI but NOT refresh,
  so a verdict derived from it alone has `refresh_verified=False` (refresh
  unverified — never a hard fail). `verify_from_report` falls back to it only when
  no `display:` marker is present. The `display:` marker is the one that makes
  refresh VERIFIED.
- `density=213` is reported only; it is not part of the pass/fail gate.

## Cross-references
- Device capture: [`2026-06-01-v126-harfbuzz-fix-display-90hz.md`](2026-06-01-v126-harfbuzz-fix-display-90hz.md)
  (WS-3 / integration: MainActivity geometry JNI + compositor 90Hz wl_output mode).
- 5-session plan: `docs/research/orchestration-5session-plan.md` (CP-1 = GUI base +
  display).

## Host
- Model: `bench/display_verify.py` (`parse_display_marker`, `verify_from_report`).
- Tests: `tests/test_alr_display_verify.py`.
- `cd /Users/naen/Documents/alr-ws5 && PATH="$HOME/.local/bin:$PATH" uvx pytest tests/test_alr_display_verify.py -q` → all pass (see CI / run log).
