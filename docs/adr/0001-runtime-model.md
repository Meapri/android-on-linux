# ADR 0001: Runtime Model

## Status

Accepted.

## Decision

Android on Linux Runtime will use a no-root, no-VM Android APK runtime for v1. The first executable boundary is Android-native code packaged in the APK native library area. The mutable Linux rootfs lives in app-private storage and is launched only through explicit backend plans.

The backend order is:

1. PRoot baseline for compatibility and A/B comparison.
2. ALR launcher for deterministic native entry and config reporting.
3. `LD_PRELOAD` path/process/procfs/fakeroot mediation for low-overhead hot paths.
4. Clean-room syscall support only after tests prove wrapper coverage is insufficient.

## Context

The project goal is to run ARM64 glibc Linux apps inside stock Android without root. Android 10+ target behavior forbids direct `execve()` of files inside the writable app home directory, so the host entrypoint cannot be an extracted writable rootfs binary. Termux works around related constraints through its own target/API and package model, but this project must remain a modern APK with visible Android UX and policy compliance.

## Consequences

- Rootfs extraction is data installation, not the first executable boundary.
- Every launch must record backend, rootfs, program, cwd, env allowlist, and report labels.
- PRoot remains a fallback and oracle until ALR proves real command coverage and lower overhead.
- Unmodified arbitrary Debian binaries are a long-term compatibility target, not a v1 guarantee.
- If VM-level compatibility becomes mandatory, that requires a separate AVF architecture track.

## Verification Gates

- Host policy tests reject direct app-data exec plans.
- APK build includes native launcher artifacts.
- Device reports must distinguish PRoot, ALR, optional external backend, and skipped features.
- ALR cannot claim execution success until a real Android device report proves it.
