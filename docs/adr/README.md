# ADR index

This repository keeps decisions in three places. The numbers overlap, so always cite the full path.

| Namespace | Scope | Read it for |
| --- | --- | --- |
| `docs/adr/` (this directory) | Project level | Runtime model, GPU path, and the unified direction |
| `runtime/alr/docs/adr/` | The alr execution engine | Supervisor, explicit `ld.so` invocation, `LD_PRELOAD` path virtualization, raw-syscall and static binaries |
| `docs/design/adr-*.md` | Workstream level | Syscall-overhead evaluation, Chromium roadmap, exec re-entry, in-app catalog UX |

## Project-level ADRs

| ADR | Title | Status |
| --- | --- | --- |
| [0001](0001-runtime-model.md) | Runtime model | Accepted. Backend order superseded by 0004 |
| [0002](0002-gpu-public-api.md) | GPU and display path | Accepted |
| [0003](0003-cpu-syscall-mediation-overhead.md) | CPU syscall mediation overhead | Accepted. Scope limited to the legacy in-process loader by 0004 |
| [0004](0004-unified-direction.md) | Unified direction | Accepted |

## alr engine ADRs

| ADR | Title |
| --- | --- |
| [0001](../../runtime/alr/docs/adr/0001-signal-only-ptrace-supervisor.md) | Signal-only ptrace supervisor |
| [0002](../../runtime/alr/docs/adr/0002-explicit-ldso-invocation.md) | Explicit `ld.so` invocation |
| [0003](../../runtime/alr/docs/adr/0003-ld-preload-path-virtualization.md) | `LD_PRELOAD` path virtualization |
| [0004](../../runtime/alr/docs/adr/0004-link2symlink.md) | link2symlink |
| [0005](../../runtime/alr/docs/adr/0005-play-store-unsupported.md) | Play Store unsupported |
| [0006](../../runtime/alr/docs/adr/0006-raw-syscall-binaries.md) | Raw-syscall binaries |
| [0007](../../runtime/alr/docs/adr/0007-android-16-only.md) | Android 16 only |
| [0008](../../runtime/alr/docs/adr/0008-static-guest-binaries-non-goal.md) | Static guest binaries |
| [0009](../../runtime/alr/docs/adr/0009-no-in-process-untar.md) | No in-process untar |
