# Device Evidence — supervisor round-trip fast-path: GIMP no-regression + Chromium still runs (v122)

The ptrace supervisor's per-trap cost is reduced (toward the Chromium render-throttle work) while the
device-proven GIMP path-mediation is preserved exactly. Device SM-X236N, APK
`0.4.122-supervisor-fastpath-v122`.

## What changed (runtime_report.cpp `build_native_loader_probe`, PTRACE_EVENT_SECCOMP handler)
- **Per-tid `/proc/<tid>/mem` fd cache**: the open+close of `/proc/<tid>/mem` was done every trap;
  now the fd is opened lazily once per tid and reused (closed on WIFEXITED/WIFSIGNALED, evicted+reopened
  on ESRCH/EIO/EBADF). Removes **2 syscalls per trap** after a tid's first.
- **translate_rootfs_path LRU cache** (256 entries, guest→host): repeated paths skip the
  string-normalization. Pure function of the (constant-for-run) rootfs+cwd, so behavior-identical.
- Everything else byte-for-byte unchanged: the trace-syscall set, the rewrite, `is_exec`/sysdir/
  `already-host` guards, scratch write, SETREGSET, the traps/rewrites counters, the SIGSYS handler,
  the seccomp filter install, and `libalr_interpose.c`/PCGATE.

## Device-verified — GIMP path-mediation NO REGRESSION
Every gimp-probe matches the v121 baseline exactly under the optimized supervisor:
```
gimp-probe guest=/bin/dynhello          exit=0  (stdout: alr-dyn-ok)
gimp-probe guest=/usr/bin/env           exit=0
gimp-probe guest=/usr/bin/id            exit=0
gimp-probe guest=/bin/dash              exit=0  (x2)
gimp-probe guest=/bin/alr-png-test      exit=0
gimp-probe guest=/usr/bin/gimp-console-3.0  exit=127  (unchanged: missing deps, not a mediation fault)
gimp-probe guest=/bin/alr-wl-test       exit=0
gimp-probe guest=/bin/alr-pixman-test   exit=0
all: pcgate=1 interpose=1 traps=0 rewrites=0
```
These run `traps=0` (the interposer rewrites paths in-process via the trampoline, so the supervisor
handler isn't entered) — confirming the optimization changes nothing for the proven GIMP path. The
fast-path matters only for `traps>0` workloads (a guest issuing RAW syscalls that bypass the
interposer → seccomp RET_TRACE → supervisor), i.e. Chromium's render path.

## Device-verified — Chromium still runs under the optimized supervisor
```
ALR NATIVE LOADER GUEST EXEC: PASS
child exit=0 signal=0
guest stdout=Chromium 147.0.7727.137
```

## Honest scope
This cuts the supervisor's PER-TRAP cost (2 fewer syscalls + cached translate). It does NOT, by itself,
make Chromium `--dump-dom` (V8 + render) usable: that throttle is a mix of (a) supervisor round-trips
for the 22 threads' raw path syscalls [this helps], (b) Chromium's own ALLOW-syscall kernel time, and
(c) the binary's compute weight on this device — so usable render remains a cumulative-perf phase. The
device-quantified win for --dump-dom is hard to isolate (the throttle is compound); the win here is the
concrete per-trap syscall reduction + a clean GIMP no-regression. 277 host tests pass; all 4 ABIs build.
