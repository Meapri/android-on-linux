# Implementation Milestones

## Planning Rule

Work in five-version bundles. Each bundle must end with:

- Host tests.
- Native tests where applicable.
- Debug APK build.
- Device evidence when the bundle claims runtime or GPU behavior.
- A short release note with PASS/FAIL/KNOWN_FAIL lines.

Do not mark a bundle complete from source tests alone when the behavior depends on Android device policy, GPU drivers, process execution, or Surface lifecycle.

## Current Baseline

Known repo state at this planning point:

- Android app skeleton exists.
- Rootfs extraction and manifest model exist.
- Termux PRoot-derived backend is packaged.
- PRoot rootfs smoke coverage exists in app code.
- Host EGL/GLES and Surface renderer code exists.
- Guest GPU/GUI bridge smoke scaffolding exists.
- ALR clean-room path/env skeleton exists.
- ALR low-overhead execution backend is not yet implemented.

## Bundle A: Documentation and Guardrails

Target version: V46-docs or next available planning checkpoint.

Goal:

Establish stable requirements, clean-room boundaries, execution backend spec, graphics bridge spec, and milestone map before adding low-overhead runtime code.

Deliverables:

- `docs/product-requirements.md`
- `docs/clean-room-protocol.md`
- `docs/alr-execution-backend-spec.md`
- `docs/android-graphics-bridge-spec.md`
- `docs/plans/implementation-milestones.md`
- README links to the documentation set.

Acceptance:

```text
DOCS PRODUCT REQUIREMENTS: PASS
DOCS CLEAN ROOM PROTOCOL: PASS
DOCS EXECUTION SPEC: PASS
DOCS GRAPHICS SPEC: PASS
DOCS MILESTONES: PASS
```

Verification:

```bash
python3 -m pytest tests -q
scripts/test-native-core.sh
```

If `pytest` is unavailable on the host, record that explicitly and run native tests.

## Bundle B: Optional proroot A/B Probe

Goal:

Treat proroot-class binaries as optional external probes, not implementation sources. Compare PRoot baseline and optional low-overhead runtime behavior through identical rootfs commands.

Tasks:

1. Add optional backend artifact location.
2. Record source URL, release tag, sha256, and license note.
3. Add backend selector in report model.
4. Add presence and version/help probes.
5. Add hello rootfs execution probe.
6. Add dpkg/apt A/B probes.
7. Add timing fields.

Expected report:

```text
PROOT BACKEND EXECUTION: PASS
OPTIONAL LOW-OVERHEAD BACKEND AVAILABLE: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD VERSION EXECUTION: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD ROOTFS EXECUTION: PASS/FAIL/SKIP
OPTIONAL LOW-OVERHEAD DPKG LOCAL INSTALL: PASS/FAIL/SKIP
```

Acceptance:

- Current PRoot backend remains unaffected.
- Optional backend absence is `SKIP`, not `FAIL`.
- Optional backend is unmodified.
- Optional backend does not feed ALR implementation internals.

## Bundle C: ALR Launcher Skeleton

Goal:

Add the open ALR runtime launcher as a packaged Android-native entrypoint. It does not need to execute guest binaries yet.

Tasks:

1. Add `libalr_runtime_launcher.so` build target.
2. Add launch-plan builder for ALR runtime.
3. Add config serialization for rootfs, cwd, env, and binds.
4. Add app report section for ALR runtime availability.
5. Add source tests that reject direct writable rootfs execution.

Expected report:

```text
ALR RUNTIME LAUNCHER AVAILABLE: PASS
ALR RUNTIME CONFIG BUILD: PASS
ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS
```

Acceptance:

- APK includes launcher.
- Launcher can print deterministic config report.
- No guest rootfs binary execution is claimed.

## Bundle D: ALR Path Hook MVP

Goal:

Implement `libalr_runtime_hook.so` for path-bearing libc calls and prove file operations inside rootfs without process-chain complexity.

Tasks:

1. Add hook shared library target.
2. Add rootfs config loading.
3. Hook `open/openat/stat/readlink/getcwd/chdir` first.
4. Add fd-to-guest-path tracking for opened directories.
5. Add trace mode.
6. Add host-native tests for path edge cases.
7. Add Android smoke command that reads `/etc/os-release` through ALR path mapping.

Expected report:

```text
ALR HOOK LOAD: PASS
ALR OPEN ROOTFS FILE: PASS
ALR STAT ROOTFS FILE: PASS
ALR READLINK PROC SELF EXE: PASS/KNOWN_FAIL
ALR CWD TRANSLATION: PASS
```

Acceptance:

- PRoot baseline still passes.
- ALR path tests pass independently.
- Errors preserve errno where tests check it.

## Bundle E: ALR Hello and Shell Execution

Goal:

Launch simple guest commands through the ALR runtime path.

Tasks:

1. Implement guest executable resolution.
2. Implement guest dynamic loader strategy for `/bin/glibc-hello`.
3. Implement script shebang handling for simple shell scripts.
4. Implement environment cleanup and ALR env injection.
5. Implement `/bin/sh -c` smoke.
6. Add timing comparison to PRoot.

Expected report:

```text
ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: PASS
ALR LOW-OVERHEAD RUNTIME GLIBC EXECUTION: PASS
ALR LOW-OVERHEAD RUNTIME SHELL EXECUTION: PASS
ALR CLEAN GUEST ENVIRONMENT: PASS
ALR HELLO RELATIVE TO PROOT: <ratio>
```

Acceptance:

- `hello` exits 0.
- Dynamic glibc hello exits 0.
- Shell `-c` exits 0.
- Android environment leak test passes.

## Bundle F: ALR Child Process Continuity

Goal:

Make `execve`, `execvp`, `execvpe`, `posix_spawn`, and shell child commands preserve ALR runtime state.

Tasks:

1. Add deterministic continuation plans for exec wrappers.
2. Add PATH lookup in guest namespace.
3. Route child execution through packaged trampoline instead of direct writable rootfs exec.
4. Add child serialized env/config handoff.
5. Add `fork`/`vfork` handling policy.
6. Add tests for Node/Python-style child patterns where possible.

Expected report:

```text
ALR EXEC CONTINUITY PLAN: PASS
ALR EXECVE CHILD CONTINUITY PLAN: PASS
ALR EXECVP PATH LOOKUP PLAN: PASS
ALR POSIX_SPAWN CHILD PLAN: PASS
ALR EXEC CONTINUITY CONFIG HANDOFF: PASS
ALR TRAMPOLINE CONTINUE EXEC: PASS
ALR TRAMPOLINE CONTINUE CHECKSUM: PASS
ALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: PASS
ALR EXECVE CHILD CONTINUITY: PASS
ALR EXECVP PATH LOOKUP: PASS
ALR POSIX_SPAWN CHILD: PASS
ALR SHELL CHILD PROCESS: PASS
```

Acceptance:

- Shell can launch `cat`, `env`, and nested `sh -c`.
- PATH lookup uses guest PATH.
- Host paths are not exposed in guest argv/report except diagnostics.

## Bundle G: ALR Procfs and Fakeroot

Goal:

Add enough procfs and fakeroot behavior for real distro tools.

Tasks:

1. Virtualize `/proc/self/exe`.
2. Virtualize `/proc/self/status`.
3. Virtualize `/proc/self/cmdline` and `/proc/self/environ`.
4. Virtualize `/proc/mounts`.
5. Add fake uid/gid mode.
6. Patch stat/statx uid/gid where needed.
7. Add NSS identity smoke with `/usr/bin/id`.

Expected report:

```text
ALR PROC SELF EXE: PASS
ALR PROC STATUS: PASS
ALR PROC MOUNTS: PASS
ALR FAKE ROOT IDENTITY: PASS
ALR IDENTITY NSS EXECUTION: PASS
```

Acceptance:

- `/usr/bin/id` reports expected root identity when fake root is enabled.
- Procfs files do not leak confusing Android host paths except documented diagnostics.

## Bundle H: Package Manager Preflight

Goal:

Reach practical distro-tool behavior without claiming full apt install support too early.

Tasks:

1. Run `dpkg --version`.
2. Run `dpkg --print-architecture`.
3. Run `dpkg-query --version`.
4. Run `apt --version`, `apt-get --version`, `apt-cache --version`, `apt-config --version`.
5. Bind or virtualize `/dev/null`, `/dev/zero`, `/dev/urandom`.
6. Attempt local `.deb` install.
7. Classify failures.

Expected report:

```text
ALR DPKG VERSION EXECUTION: PASS
ALR DPKG ARCH EXECUTION: PASS
ALR APT VERSION EXECUTION: PASS
ALR DPKG LOCAL INSTALL EXECUTION: PASS/KNOWN_FAIL:<reason>
```

Acceptance:

- Version/preflight commands pass.
- Local install either passes or has a stable, actionable known-fail reason.
- PRoot and optional probe A/B results are included.

## Bundle I: Execution Performance Harness

Goal:

Measure whether ALR is actually below PRoot overhead.

Tasks:

1. Add benchmark rootfs commands.
2. Add app-side timing.
3. Add repeated-run aggregation.
4. Add warm/cold distinction.
5. Add report export or copyable text block.

Workloads:

- `hello`.
- `/bin/sh -c true`.
- 1,000 `stat`.
- 1,000 `open/read/close`.
- 100 child spawns.
- Package-manager preflight command.

Expected report:

```text
ALR BENCH HELLO MS=<n> PROOT=<n> RATIO=<n>
ALR BENCH STAT1000 MS=<n> PROOT=<n> RATIO=<n>
ALR BENCH SPAWN100 MS=<n> PROOT=<n> RATIO=<n>
ALR SYSCALL OVERHEAD SMOKE: LOWER_THAN_PROOT
```

Acceptance:

- At least two syscall-heavy workloads are faster than PRoot by a meaningful margin.
- Any slower workload has an explanation or open issue.

## Bundle J: GUI Bridge Cleanup

Goal:

Keep GUI/GPU proof stable while execution backend work proceeds.

Tasks:

1. Preserve host EGL/GLES proof.
2. Preserve Surface frame proof.
3. Preserve guest GPU IPC proof.
4. Preserve Wayland/X11-shaped ACK proof.
5. Add backend name to graphics report.

Expected report:

```text
BACKEND=<proot|optional|alr>
HOST GPU EGL/GLES EXECUTION: PASS
GUEST GUI GPU SURFACE EXECUTION: PASS
SURFACE FRAME LOSSLESS: true
```

Acceptance:

- Graphics bridge works with PRoot.
- Graphics bridge can be run with ALR once ALR shell execution is available.

## Bundle K: GLES Shim MVP

Goal:

Make a tiny Linux GL app render through the Android host Surface path.

Tasks:

1. Add guest shim source or payload generation.
2. Export required EGL/GLES symbols.
3. Add host command execution for supported GL subset.
4. Add unsupported symbol reporting.
5. Add clear/swap test app.

Expected report:

```text
GUEST EGL INIT VIA SHIM: PASS
GUEST GLES CLEAR VIA SHIM: PASS
GUEST EGL SWAP VIA ANDROID SURFACE: PASS
GUEST GLES HARDWARE RENDER: PASS
```

Acceptance:

- Clear color appears via Android Surface.
- Renderer remains hardware-backed.
- Frame ACK is lossless.

## Bundle L: Vulkan Research MVP

Goal:

Add a minimal Vulkan host and guest ICD proof after GLES MVP.

Tasks:

1. Host Vulkan capability probe.
2. Host Vulkan Surface capability probe.
3. Host Vulkan Surface swapchain clear/present.
4. Guest ICD registration.
5. Guest device enumeration.
6. Guest clear/present subset.

Expected report:

```text
ANDROID HOST VULKAN SURFACE EXECUTION: PASS
host vulkan surface renderer=android-vulkan-swapchain-clear-present
guest vulkan clear present hardware render=true
GUEST VULKAN ICD SMOKE EXECUTION: PASS
GUEST VULKAN CLEAR PRESENT EXECUTION: PASS
```

Acceptance:

- Vulkan host path works on at least one physical device.
- GLES path remains stable.

## ALR Mediation Layer Status (host-verified, device-pending)

These clean-room mediation layers are implemented and host-verified (native + pytest + debug APK build). None are device-proven yet; each Android-visible gate is plan/mechanism evidence, not runtime proof on a target device.

- V51 procfs virtualization plan (`alr_procfs.cpp`): synthesizes the guest `/proc/self/exe`, `/proc/self/status` (fakeroot), and `/proc/mounts` views with a host-leak guard. Gate: `ALR PROCFS VIRTUALIZATION PLAN: PASS`.
- V52 W^X-safe exec strategy (`alr_wx.cpp`): packaged-trampoline entrypoint, ranked native-exec methods (`memfd-execveat`, `anon-mmap-loader`) with `proot-baseline` fallback, and explicit rejection of `direct-rootfs-execve`/`file-backed-mmap-exec`. SELinux outcomes are device-pending. Gate: `ALR WX-SAFE EXEC STRATEGY: PASS`.
- V53 perf scaffolding (`alr_perf.cpp`): measures ALR in-process path-translation cost and a real syscall round-trip; the PRoot ptrace baseline and final verdict stay `PENDING_DEVICE`. Gate: `ALR PERF HARNESS: PASS`.
- V54 procfs interpose mechanism (`resolve_interposed_access`): serves `/proc/self/exe`, `/proc/<pid>/exe`, `/proc/self/status`, and `/proc/mounts` from the synthesized guest view with no host `/proc` access; all other paths fall back to rootfs translation. Not yet wired into a live `LD_PRELOAD` hook. Gate: `ALR PROCFS INTERPOSE MECHANISM: PASS`.

Device-pending for this layer: real on-device `/proc` virtualization, `memfd-execveat`/`anon-mmap-loader` SELinux behavior, and the PRoot-vs-ALR per-syscall measurement.

## Continuous Guardrails

Every bundle must preserve:

- Safe rootfs extraction.
- No direct writable app-data executable entrypoint policy.
- PRoot baseline unless explicitly replaced by a later accepted milestone.
- Clean guest environment.
- Deterministic report strings.
- Clean-room boundary documentation.

## Stop Conditions

Pause and redesign when:

- Android policy blocks the planned execution path on target SDK devices.
- ALR cannot beat PRoot on any syscall-heavy benchmark after v1/v2 wrapper implementation.
- Graphics bridge requires vendor-private APIs to make progress.
- Clean-room contamination is suspected.
- Licensing makes planned distribution impossible.

## Immediate Next Tasks

1. Wire these docs into README.
2. Add source tests that assert the new docs exist and include required headings.
3. Restore `pytest` availability in the host environment or document the missing dependency.
4. Decide whether the next coding bundle is optional proroot A/B or ALR launcher skeleton.
