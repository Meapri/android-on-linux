# Device Evidence — SM-X236N, v120: V8-style JIT W^X works → Chromium needs no --jitless

A Goal-2 (Chromium) gating question is answered on hardware: V8-style iterative W^X executable memory
works on this device's `untrusted_app` domain, so Chromium will run with full V8 JIT (and WebAssembly)
without `--jitless`. APK `0.4.120-chromium-jitwx-probe-v120`, device SM-X236N (Mali-G615 MC2, Android 16).

## Device-verified
```
ALR JIT WX CYCLE: PASS
cycles_ok=8/8                  (8 iterations of: write stub RW -> clear-cache -> mprotect RX -> call -> mprotect RW; each returned its per-iteration constant)
concurrent_rx_ok=4/4           (4 distinct code pages RX-callable simultaneously)
rwx_mmap_ok=true (errno 0)     (direct mmap(PROT_READ|PROT_WRITE|PROT_EXEC) anon, written + called, no mprotect)
wx_granularity_ok=true         (one page RX-executing while a different page is RW-written)
page_size=4096
VERDICT: V8 JIT viable without --jitless
```
`alr_jit/alr_jit_probe.hpp` `run_jit_wx_cycle_probe()` (JNI `nativeJitWxProbe`). Pure anonymous
`mmap`/`mprotect` — no memfd-exec (that is EACCES on device, `build_memfd_exec_probe`). The probe
models exactly how V8/SwiftShader use executable memory.

## Why it matters for Chromium
- **V8 JIT (TurboFan/baseline) + WebAssembly run at full speed** — no need for `--jitless` (which is
  ~40–80% slower and drops WASM). The Phase-B launch command drops `--js-flags=--jitless`.
- Confirms Scout 2's research: V8's W^X scheme was reverted ~2023 (RWX pages are the default again),
  and Android grants `execmem` (anonymous PROT_EXEC) to `untrusted_app` because ART/WebView JIT need
  it. Both the modern RWX path (`rwx_mmap_ok`) and the older RW↔RX flip path (`cycles_ok`) work here.
- The gate for JIT was "our W^X layer + the app domain", not a flag — and it is OPEN.

## No regression
277 host tests pass (added `tests/test_android_alr_jit_wx_probe.py`); APK versionCode 120, all 4 ABIs
built; `nativeJitWxProbe` in the shipped arm64 `libalr_loader.so`. The probe is an additive startup
self-test (tag `alr_loader`); GPU probes + GIMP path unchanged. (The blocked path memfd-execveat is
unrelated — V8 uses anonymous PROT_EXEC, which works.)
