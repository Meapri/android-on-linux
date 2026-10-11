# Vendored: tawcroot (reference input for ADR 0004 spike S2)

Status: **vendored source only. Not built into `alr`, not shipped in the APK, not wired into any Gradle or Make target.** It exists so spike S2 (cost of an in-process `SECCOMP_RET_TRAP` lane for static and raw-syscall binaries; see [ADR 0004 D6](../../../../docs/adr/0004-unified-direction.md) and [alr ADR 0006 addendum](../../docs/adr/0006-raw-syscall-binaries.md)) can start from a working implementation instead of a blank file.

## Provenance

| Field | Value |
|---|---|
| Upstream | https://github.com/wmww/tawc (directory `tawcroot/`) |
| Commit | `d9efb4ae60da85154210c2edfb10f46e8ac58755` (2026-10-08, "release: v4") |
| Author | Sophie Winter |
| License | MIT, text in [LICENSE.MIT](LICENSE.MIT) (copied verbatim from upstream `LICENSE.MIT`) |
| Local modifications | none |

Upstream's own licensing note ([notes/licensing.md](https://github.com/wmww/tawc/blob/main/notes/licensing.md)): tawc's sources are MIT; the distributed tawc APK is GPLv3-only because it links termux-app's `termux-shared` extra-keys code. That code is **not** part of what is vendored here. `tawcroot` is freestanding C with no third-party dependencies (no libc, no malloc).

## What is included

- `src/`, `include/`: the production sources (about 16k lines of C and four assembly stubs for aarch64 and x86_64).
- `perf/`: the micro-benchmark harness and upstream's recorded results.
- `LICENSE.MIT`.

## What is excluded

- `tests/` (needs the `cleat` test framework, a separate dependency whose license has not been reviewed here).
- `build.sh`, `Makefile`, `ando/`, and the design notes (`notes/tawcroot/*.md`). The notes are worth reading upstream: `sigsys-handler.md`, `seccomp-filter.md`, `path-translation.md`.

## How it works, in one paragraph

A freestanding static non-PIE binary installs a seccomp filter that returns `SECCOMP_RET_ALLOW` for syscalls issued from its own trampoline range and `SECCOMP_RET_TRAP` for the path-bearing syscalls issued anywhere else. The `SIGSYS` handler runs in the guest thread, translates the path against the rootfs, and re-issues the syscall from the allowed range. It also loads the target ELF itself (`loader_*.c`), which is how it installs the handler before the first guest instruction in a static binary. This is the mechanism ADR 0004 D6 wants to measure.

## Build check (host only)

All of `src/*.c` and the `x86_64` stubs compile with upstream's flags (`-O2 -ffreestanding -fno-stack-protector -D_GNU_SOURCE -Iinclude`) and link with GNU ld using `-static -nostdlib -nostartfiles -Wl,-Ttext-segment=0x10000000`. Upstream uses `--image-base` (lld). Verified 2026-10-11 on an x86_64 Linux VM (kernel 6.18).

Host smoke, same VM: a static glibc binary run under `tawcroot -r ROOT -- /bin/h` opened `/etc/hostname` and read the file inside `ROOT`, not the host's. That is the case `LD_PRELOAD` cannot reach.

## Reference numbers (not evidence for Android)

| Source | stat() through the trap path | native stat() |
|---|---|---|
| Upstream `perf/results.md`, AMD laptop, 2026-06-10 | 2,285 ns | 352 ns |
| This repository, x86_64 VM, 2026-10-11, 200k iterations, 3 runs | 4,510 to 4,807 ns | 444 to 504 ns |

Neither number is a measurement on an Android device under `untrusted_app_27` with the zygote seccomp filter stacked. ADR 0004 D6 gates the decision on a device measurement. `PENDING_DEVICE`.

## Rules for using this code

1. Keep `LICENSE.MIT` and the copyright notice with every copy or derivative.
2. Do not link this into the shipped product until spike S2 passes and a new ADR says so.
3. Record every local change in this file, with the reason.
4. Do not copy files from upstream's GPL-covered dependencies (`deps/` there).
