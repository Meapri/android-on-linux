# Open-source low-overhead rootless Linux runtime candidates

Track B research note for reducing Android APK Linux-runtime overhead versus the current PRoot/ptrace path. Scope: open-source runtimes or components that could run a distro-style userspace inside an Android app sandbox without requiring a rooted device or app-code changes in this branch.

## Executive conclusion

No surveyed open-source project is a drop-in, native-speed, non-root replacement for PRoot on stock Android. The strongest practical direction is a hybrid: keep PRoot/Termux PRoot for compatibility while prototyping a narrow `LD_PRELOAD` path-virtualization layer for workloads we control, and keep `proot-rs` as the most maintainable ptrace baseline. Namespace/container tools (`bubblewrap`, `nsjail`, `gVisor`) are valuable references but are blocked by Android app sandbox privilege/kernel assumptions. Seccomp user notification is promising as a future selective syscall-interception component, not a complete runtime today.

Top candidates for Track B follow-up:

1. **LD_PRELOAD path virtualization / fakechroot-style shim** — best low-overhead upside for controlled dynamically linked guest binaries because it avoids trapping every syscall. It cannot transparently cover static binaries, direct syscalls, setuid semantics, or all `/proc`/mount behavior.
2. **proot-rs** — best maintainability candidate for the existing PRoot model; Rust implementation may simplify hardening and Android-specific patches, but it remains ptrace-based and therefore cannot remove the fundamental per-syscall trap cost.
3. **seccomp user notification component** — potentially lower overhead than ptrace if used to intercept only selected syscalls, but Android kernel/config availability and supervision model need device probing; it does not provide rootfs/path virtualization by itself.

## Candidate matrix

- **Candidate: PRoot / Termux PRoot**
  - **Source:** [proot-me/proot](https://github.com/proot-me/proot), [termux/proot](https://github.com/termux/proot)
  - **License:** GPL-2.0 upstream PRoot; Termux fork carries upstream/packaging notices.
  - **Approach:** ptrace-based syscall interception; implements user-space `chroot`, bind mounts, and optional binfmt/QEMU integration without privileges.
  - **Android non-root feasibility:** **High**. This is the proven Android/Termux path and is already represented by packaged PRoot artifacts in this repo.
  - **Overhead expectation:** **High overhead** for syscall-heavy workloads because path and process syscalls cross ptrace stop/resume boundaries.
  - **Integration value:** Compatibility baseline and fallback; Termux patches are the most relevant Android prior art.
  - **Risks:** ptrace policy and seccomp interactions vary by device/Android release; performance ceiling is limited by design; GPL obligations for distribution.

- **Candidate: proot-rs**
  - **Source:** [proot-me/proot-rs](https://github.com/proot-me/proot-rs)
  - **License:** GPL-3.0.
  - **Approach:** Rust implementation of PRoot; still ptrace-based path translation for Linux syscalls.
  - **Android non-root feasibility:** **Medium**. It should be conceptually portable to Android because ptrace is the same primitive, but Android-specific loader, bionic, signal, seccomp, and packaging work would be required.
  - **Overhead expectation:** **High-to-medium overhead**. Rust may improve reliability and patchability but does not remove ptrace costs.
  - **Integration value:** Good candidate for a cleaner long-term PRoot-compatible backend or for borrowing architecture/tests.
  - **Risks:** GPL-3.0 compatibility with app distribution must be reviewed; less mature Android/Termux production history than C PRoot; no native-level performance breakthrough.

- **Candidate: fakechroot**
  - **Source:** [dex4er/fakechroot](https://github.com/dex4er/fakechroot)
  - **License:** LGPL-2.1.
  - **Approach:** `LD_PRELOAD` wrappers replace libc functions such as `chroot`, `open`, and related path operations to simulate a chroot without root.
  - **Android non-root feasibility:** **Medium-low** as-is; the idea is feasible for dynamically linked ELF processes where `LD_PRELOAD` is honored, but fakechroot targets glibc/Linux assumptions and would need bionic/Android packaging work.
  - **Overhead expectation:** **Low-to-medium overhead** for intercepted libc calls, much lower than ptrace for normal dynamic binaries, but incomplete for direct syscalls/static binaries.
  - **Integration value:** Strong reference for a custom preload shim and test corpus; possible component for controlled guest commands.
  - **Risks:** Not transparent enough for arbitrary distro rootfs; must cover many libc entry points and ABI variants; `execve`, dynamic linker, `/proc`, symlink, cwd, and mount semantics are hard; preload can be disabled or bypassed.

- **Candidate: LD_PRELOAD path virtualization approaches**
  - **Source:** technique/pattern; examples include fakechroot and app-specific path-rewrite shims.
  - **License:** Depends on implementation; a repo-local implementation could use the project license, while borrowed code inherits its license.
  - **Approach:** Interpose libc/libdl/libpthread entry points and rewrite guest paths into app-private storage before kernel entry.
  - **Android non-root feasibility:** **Medium**. Android apps can ship native libraries and set environment for child processes, but coverage differs for bionic vs glibc guests and for dynamic linker restrictions.
  - **Overhead expectation:** **Low** for supported path calls because it avoids global syscall tracing.
  - **Integration value:** Best path to native-like speed for a constrained supported workload set; can be layered under the current launcher as an experimental backend.
  - **Risks:** Incomplete isolation and compatibility; static binaries and direct `syscall(2)` bypass hooks; path canonicalization bugs can become security issues; requires extensive conformance tests.

- **Candidate: libredirect-like path-rewrite shim**
  - **Source:** [xXJSONDeruloXx/libredirect](https://github.com/xXJSONDeruloXx/libredirect)
  - **License:** GPL-3.0.
  - **Approach:** `LD_PRELOAD` shim libraries for Wine-on-Android/Winlator-style package path rewriting; hooks many glibc filesystem functions plus a smaller bionic layer.
  - **Android non-root feasibility:** **Medium** for its narrow purpose; it already targets Android-adjacent Wine/Box64 deployments, but it is not a full chroot/runtime.
  - **Overhead expectation:** **Low** for covered calls; no ptrace round-trip.
  - **Integration value:** Practical Android-oriented example of path rewriting, build layout, and hook coverage; useful as a prototype reference, not a direct dependency unless GPL-3.0 is acceptable.
  - **Risks:** Narrow package-path rewrite semantics; GPL-3.0; not a security boundary; would need major expansion for rootfs illusion.

- **Candidate: bubblewrap**
  - **Source:** [containers/bubblewrap](https://github.com/containers/bubblewrap)
  - **License:** LGPL-family COPYING in upstream; GitHub reports `NOASSERTION` because repository licensing is nuanced.
  - **Approach:** Linux namespaces, bind mounts, pivot/chroot-like setup, seccomp; used by Flatpak as a low-level sandbox helper.
  - **Android non-root feasibility:** **Low** on stock Android app sandboxes. It depends on user namespaces or setuid/helper capabilities plus mount namespace operations that ordinary APK UIDs generally cannot use.
  - **Overhead expectation:** **Near-native** after setup on systems where namespaces are available.
  - **Integration value:** Good reference for desired mount namespace model and CLI semantics.
  - **Risks:** Kernel/user-namespace availability is device/ROM dependent; app sandbox lacks CAP_SYS_ADMIN/setuid helper; likely unusable as a distributed Play-style APK backend.

- **Candidate: nsjail**
  - **Source:** [google/nsjail](https://github.com/google/nsjail)
  - **License:** Apache-2.0.
  - **Approach:** namespaces, cgroups, rlimits, chroot/pivot_root, and seccomp-bpf policy.
  - **Android non-root feasibility:** **Low**. Typical use expects namespace/cgroup/mount operations and often privileged setup; not designed for an ordinary Android application UID.
  - **Overhead expectation:** **Near-native** after setup where supported.
  - **Integration value:** Security-policy and sandboxing reference only.
  - **Risks:** Android app privilege mismatch; cgroup and namespace restrictions; more isolation tool than rootfs translation runtime.

- **Candidate: gVisor**
  - **Source:** [google/gvisor](https://github.com/google/gvisor)
  - **License:** Apache-2.0.
  - **Approach:** user-space application kernel/container runtime (`runsc`) implementing a Linux syscall surface with ptrace/KVM/platform-specific backends.
  - **Android non-root feasibility:** **Very low**. gVisor targets OCI/container hosts and requires host integration far beyond an APK sandbox.
  - **Overhead expectation:** **Medium-to-high** versus native depending on backend/workload; not intended as a tiny in-app path translator.
  - **Integration value:** Architectural reference for syscall emulation boundaries and tests, not a candidate dependency.
  - **Risks:** Size/complexity, host requirements, incompatibility with Android application distribution model.

- **Candidate: seccomp user notification component**
  - **Source:** Linux kernel `seccomp_unotify` API; [libseccomp](https://github.com/seccomp/libseccomp) is LGPL-2.1 and can help construct filters on supported systems.
  - **License:** Kernel API is not a library dependency; libseccomp is LGPL-2.1 if used.
  - **Approach:** install a seccomp filter that sends selected syscall events to a supervisor via a notification fd; supervisor can inspect/respond without ptracing every syscall.
  - **Android non-root feasibility:** **Medium-low/unknown**. It needs kernel support (`CONFIG_SECCOMP` and user notification), usable `seccomp` syscall access, and an in-app supervisor model. Stock Android device coverage must be probed empirically.
  - **Overhead expectation:** **Low for non-intercepted syscalls; medium for intercepted syscalls**. Potentially much better than PRoot if only path-sensitive calls trap.
  - **Integration value:** Promising research component for selective mediation combined with preload path virtualization; also useful for capability probing.
  - **Risks:** Not a complete rootfs runtime; difficult to safely emulate path syscalls because kernel path resolution already happens inside the syscall; API/device support uncertainty; TOCTOU and fd-passing complexity.

## Recommendation

For Track B, do not replace the current PRoot backend yet. Add a research branch/prototype that layers a minimal preload path mapper under the existing launcher and measures controlled commands (`/bin/sh`, package-manager-free utilities, app-owned rootfs files). In parallel, spike `proot-rs` cross-compilation to Android to compare stability and maintainability against Termux PRoot. Treat bubblewrap/nsjail/gVisor as references unless target devices explicitly permit user namespaces/caps. Treat seccomp user notification as a device-capability probe and selective-interception experiment rather than the primary runtime.

## Validation checklist for future work

- Probe target devices for ptrace, seccomp, seccomp user notification, user namespace, mount namespace, and `unshare` behavior from an APK UID.
- Benchmark syscall-heavy and CPU-heavy workloads separately to distinguish tracing overhead from general native execution.
- Build a preload conformance suite covering `openat`, `statx`, `rename`, `symlink`, `readlink`, `execve`, `posix_spawn`, cwd changes, `/proc/self/exe`, and dynamic loader behavior.
- Keep licensing review explicit before vendoring GPL/LGPL components into distributed APK artifacts.
