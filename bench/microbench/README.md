# microbench — M1 same-binary CPU-overhead probe

`microbench.c` is one tiny, self-contained arm64 binary whose purpose is the M1
measurement: run the **same binary** two ways and diff the per-op cost.

1. **native baseline** — straight on the device via `adb shell`, and
2. **through the ALR loader** — the APK's native-loader path,

then feed both `ns_per_op` numbers into the WS-5 bench harness to get a
CPU-overhead percentage against the §0 target.

## Workloads

`microbench <mode> [iters]`, where `mode` is:

- `compute` (default, iters 50,000,000) — a `volatile` arithmetic busy-loop
  that issues **no syscalls**. Pure userspace CPU, so ALR's ptrace/seccomp
  mediation never fires; overhead should approach 0%.
- `syscall` (iters 1,000,000) — a loop calling `syscall(SYS_getpid)` every
  iteration. A **syscall storm** that hammers ALR's ptrace round-trip /
  seccomp mediation.

It prints exactly one line:

```
MICROBENCH mode=compute iters=50000000 ns=123456789 ns_per_op=2.47
```

## Build (arm64)

Cross-compile a static binary on the host:

```sh
aarch64-linux-gnu-gcc -O2 -static microbench.c -o microbench
```

(or build with the rootfs `gcc` inside the guest). `-static` keeps it
loader-independent so the native baseline and the ALR run exercise identical
code.

> Staging the built binary onto the device (`/data/local/tmp/...`) is **WS-4's
> stage-tar responsibility**. Do **not** push from this worktree — this README
> only describes how the numbers are produced and consumed.

## Run

Native baseline (device):

```sh
adb shell /data/local/tmp/microbench compute
adb shell /data/local/tmp/microbench syscall
```

Through the ALR loader: launch the same binary via the APK's native-loader
path and capture its `MICROBENCH ...` line from the report.

## Score it

Feed the two `ns_per_op` numbers (as total `ns`, with matching
`--native-samples`/`--alr-samples`, or as single samples) into the harness:

```sh
# compute — syscall-light, gated < 5%
python -m bench overhead --native-ns <NATIVE_NS> --alr-ns <ALR_NS> \
    --binary microbench-compute

# syscall — syscall-storm, reported (not gated): add --storm
python -m bench overhead --native-ns <NATIVE_NS> --alr-ns <ALR_NS> \
    --binary microbench-syscall --storm
```

## Expected results

- **compute** ≈ < 5% overhead — **gated** (§0 syscall-light target).
- **syscall** = high overhead — **reported, not gated**: this is the known L1
  ptrace round-trip wall, not a harness pass/fail.
