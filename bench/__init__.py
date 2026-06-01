"""ALR verification & benchmark harness (WS-5 / L5).

Host-side tooling that MEASURES the output of the other workstreams. It only READS
the device report markers emitted by app native/kt sources (L1/L2/L3) — it never
modifies those sources. New top-level package (the `tools/` tree is WS-4-owned).

Submodules:
- report_parse:   parse an APK execution report into structured markers (pure).
- cpu_overhead:   M1 — native(adb shell) vs ALR loader overhead model (pure math).
- regression_gate: M3 — no-regression matrix gate over a captured report (pure).
- adb_driver:     thin, opt-in adb helpers for device runs (no import side effects).

See docs/research/ws5-verification-bench-status.md for ownership/boundaries.
"""
