<!-- WS-5 standard device-evidence template.
Copy to docs/evidence/YYYY-MM-DD-device-<MODEL>-<short-slug>.md and fill in.
Rules: device evidence ONLY (no host-only claims here); always `am force-stop` + cold
start before capture (memory: device-test-force-stop-first); paste REAL report lines,
not paraphrases; keep an honest-scope section. -->

# Device Evidence — <one-line what-was-proven> (v<NN>)

Device `<adb-serial>` (<model>, <SoC>, Android <ver>, <resolution>@<Hz>). APK
`<versionName>` (sha256 `<apk-sha256>`). Backend: native ALR loader (PRoot fallback-only).

## What changed
- <file:function> — <change, one bullet each>. Note which workstream/layer (L1/L2/L3/L4).

## Device-verified output
<!-- Paste the real MainActivity summary / logcat lines that prove the claim. -->
```
build: <versionName>
ALR NATIVE LOADER GUEST EXEC: PASS
child exit=0 signal=0
guest stdout=<...>
alr native loader path-mediation traps=<N> rewrites=<M>
all: pcgate=1 interpose=1 traps=0 rewrites=0
<gpu/perf/wl_output lines as relevant>
```

## Regression gate (WS-5)
<!-- Output of `bench.regression_gate.evaluate_text(report)` against the captured report. -->
```
Regression gate — <PASS/FAIL> (build: <versionName>)
<per-probe rows>
mediation invariant: <ok/FAIL>
```

## Bench (if applicable)
<!-- CPU overhead (bench.cpu_overhead) or GPU score; native(adb shell) vs ALR. -->
| metric | value |
|--------|-------|
| native ns/sample | <...> |
| ALR ns/sample | <...> |
| overhead | <±X%> |
| verdict | <PASS/FAIL vs §0 target> |

## Honest scope
- What this does NOT prove. Known walls (e.g. syscall-storm ptrace round-trip). What is
  reported-only vs gated. Cumulative-perf items.

## Host
- `uvx pytest tests/ -q` → <N> passed. ABIs built: <arm64-v8a / ...>.
