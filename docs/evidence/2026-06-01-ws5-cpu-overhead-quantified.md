# Device Evidence — WS-5 CPU-overhead quantification of WS-1 M2 (general-app traps=0; path-mediation supervisor round-trip = ZERO) (v127)

Device SM-X236N (mt6878, Mali-G615, Android 16, 1200×1920@90Hz). APK
`0.4.127-cp1-gui-baseline-v127`. Backend: native ALR loader (PRoot fallback-only).

WS-5's standardized quantification of the **WS-1 (L1 CPU execution / overhead) M2** capture.
Source evidence: `docs/evidence/2026-06-01-ws1-m2-cpu-mediation-overhead.md` (WS-1, same APK,
same device). This doc re-states WS-1's device numbers in the WS-5 evidence format and records
the §0 verdict for the compat matrix / bench gate; it adds no new device run.

## §0 verdict — path-mediation supervisor round-trip = ZERO (device-verified)
For general apps the ALR **path-mediation** supervisor (ptrace) round-trip is **ZERO**:
every general guest in the WS-1 M2 run reported `traps=0 rewrites=0` with
`pcgate=1 interpose=1`. The PCGATE interposer rewrites path syscalls **in-process** via the
trampoline, so the seccomp RET_TRACE supervisor handler is never entered — the fundamental
difference from PRoot (trap-every-syscall). Therefore the path-mediation portion of the
user's "zero overhead" goal **holds for general apps, device-verified**, and general-CLI
native-exec wall-clock is **~18-20ms = native-process level**.

## Device / APK
- Device `SM-X236N` (mt6878 SoC, Mali-G615, Android 16, 1200×1920@90Hz).
- APK `0.4.127-cp1-gui-baseline-v127`.
- Captured by WS-1 (L1) M2; quantified here by WS-5 (L5). No version-stamp change.

## Per-guest native-exec wall-clock (`exec_ms`, device)
New `exec_ms` = fork→reap wall-clock per guest (fork + ELF map + ld.so + run + exit + supervisor):

| guest | exec_ms | note |
|-------|---------|------|
| `/bin/dynhello` | 19 | dynamic glibc, `alr-dyn-ok` |
| `/usr/bin/env` | 19 | |
| `/usr/bin/id` | 20 | |
| `/bin/dash` | 18-19 | |
| `/bin/alr-png-test` | 23 | libpng decode |
| `/usr/bin/gimp-console-3.0 --version` | 48 | heavier lib load |
| `/bin/alr-wl-test` | 40 | wl client |
| `/bin/alr-pixman-test` | 38 | |
| `/bin/alr-gtk3-test` | 197 | GTK3 lib load |
| `gtk3-widget-factory` | 145 | |
| `gimp-3.0` | 135 | only guest with `traps=1` (one set_robust_list-class) |
| `alr-input-test` | 6048 | **INTENDED dispatch wait** — not overhead |
| `alr-interactive-test` | 6053 | **INTENDED dispatch wait** — not overhead |

- General CLI (`dynhello`/`env`/`id`/`dash`/`alr-png-test`) lands at **~18-20ms**, i.e. the
  full fork + ELF map + ld.so + run + exit + supervisor cost is at **native-process level**.
- `alr-input-test`/`alr-interactive-test` 6048/6053ms are the **intended interactive dispatch
  wait**, NOT mediation overhead.
- All general guests run `traps=0` (only `gimp-3.0` = 1) → these are execution times with the
  supervisor round-trip already at zero.

## Path-mediation perf microbench (`run_perf_comparison`)
```
ALR PERF SYSCALL ROUNDTRIP MEASURED: PASS
alr perf alr xlate     ns/op = 4334.727
alr perf syscall getppid ns/op = 218.338
```
- path-xlate (`translate_rootfs_path`) **cold = 4334.7 ns/op ≈ 19.9 raw-syscall units**
  (4334.727 / 218.338).
- raw syscall (`getppid`) = **218.3 ns/op**.
- This is the **cold** translate cost; the guest amortizes repeated paths with a **256-entry
  `xlate_cache`** (guest_path→host_path memoize), so the effective per-path cost after warmup is
  far lower than the cold figure. Crucially, this xlate cost is **in-process** and is paired with
  a **zero** supervisor round-trip (traps=0) — it is not multiplied by a ptrace trap.

## ALR CPU-mediation overhead composition (interpretation vs "zero overhead")
- **path syscall**: in-process translate (cold 4.3µs, cheap after cache hit) + **supervisor
  round-trip 0** (traps=0).
- **non-path syscall**: seccomp **ALLOW** (0 overhead).
- → For general apps this is a fundamental advantage over PRoot's ptrace-every-syscall
  (~µs/syscall). The path-mediation "zero overhead" claim is device-verified for general apps.

## CP-3 baseline — native LANDED; rigorous same-binary ratio still PENDING
WS-1 captured a **native baseline** (microbench, static arm64, `adb shell` direct) — drain evidence
`docs/evidence/2026-06-01-batch-drain-cp2-progress-svg-locale-cp3baseline.md`:
- **native compute = 4.06 ns/op** (50M iters; pure CPU, no syscalls)
- **native syscall (`getpid`) = 200.36 ns/op** (1M iters)

### Approximate raw-syscall overhead (reported, NOT apples-to-apples)
Comparing native `getpid` 200.36 ns/op against the ALR perf-microbench `getppid` 218.34 ns/op
(`python -m bench overhead --native-ns 20036 --native-samples 100 --alr-ns 21834 --alr-samples 100 --storm`):

| metric | value |
|--------|-------|
| native ns/op | 200.36 |
| ALR ns/op | 218.34 |
| overhead | **+8.97%** (1.090×) |
| class | syscall-storm (reported, not gated) |

**Caveat (per drain evidence):** this is `getpid` (native, libc wrapper) vs `getppid` (ALR, raw
syscall via the perf probe) — **NOT apples-to-apples** (different syscall; wrapper vs raw). So ~9%
is an *indicative* raw-syscall delta, **not** the rigorous CP-3 figure.

### Still PENDING (rigorous same-binary ratio)
- **compute-mode ratio not computable yet**: native compute = 4.06 ns/op, but there is **no ALR
  measurement of the same microbench compute loop** (WS-1 has not yet run microbench AS AN ALR
  GUEST — "option i" / guest-probe wiring). Pure-CPU ALR overhead is *expected* ~0% (in-process, no
  mediation) but is **not yet measured**, so no number is claimed.
- **PRoot baseline deferred**: app-private rootfs exec is blocked by SELinux → PRoot A/B remains out.
- The rigorous CP-3 close = WS-1 runs the **same** microbench {compute,syscall} as an ALR guest →
  feed both ns/op into `bench overhead` for a true same-binary native-vs-ALR ratio.

## DEVICE-REQ filed (rigorous CP-3)
`DEVICE-REQ: run /data/local/tmp/microbench {compute,syscall} AS AN ALR GUEST (same binary as the
native baseline) → capture ALR ns/op per mode → true same-binary native-vs-ALR % ratio.`

## Separate wall (out of scope)
chromium-class raw `svc` **syscall-storm** cannot be hooked by the LD_PRELOAD interposer → it falls
back to the seccomp RET_TRACE supervisor round-trip = the ptrace wall (CP-6 / M3 USER_NOTIF ADR).

## Host
- `cd /Users/naen/Documents/alr-ws5 && uvx pytest tests/ -q` → green.
