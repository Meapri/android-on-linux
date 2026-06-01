# ALR runtime v1 clean-room specification

## Status

This document defines the initial clean-room AndroLinux Runtime (ALR) v1 skeleton. It is intentionally small: the first deliverable is a deterministic rootfs path translator plus environment helpers that can be tested on the host and compiled by the Android native build.

## Clean-room boundaries

- Clean-room implementation only. ALR runtime code is derived from this specification, public Android/Linux behavior, and project-owned tests.
- No proprietary PRoot/proot reverse engineering. Existing packaged PRoot-compatible artifacts remain integration candidates only and are not used as implementation sources for ALR internals.
- Runtime v1 code must be readable, deterministic, and covered by host-side tests before it is used as an execution backend.

## Runtime v1 scope

### In scope

- Rootfs path translation from a guest-visible path namespace to an app-private host rootfs directory.
- Lexical path normalization independent of the host filesystem.
- Minimal guest environment construction shared by future backends.
- Host-native tests that validate translation and environment behavior.
- Android CMake integration that compiles the skeleton sources.

### Out of scope for this skeleton

- Syscall interception or emulation.
- ELF loading, dynamic linker emulation, or glibc ABI shims.
- Mount namespace emulation.
- PRoot/proot internals.
- Executing guest binaries directly from writable app data.

## Rootfs path translation

Inputs:

- `rootfs_dir`: absolute host path to the installed rootfs in app-private storage.
- `cwd`: absolute guest current working directory.
- `path`: guest path supplied by runtime code or a future syscall adapter.

Rules:

- Absolute guest paths are interpreted from guest `/`.
- Relative guest paths are resolved against `cwd`.
- Empty relative path resolves to `cwd`.
- Repeated slashes are collapsed.
- `.` components are removed.
- `..` components pop one guest component, but parent traversal must clamp at guest root. Traversal such as `/../../etc/passwd` becomes `/etc/passwd` and must not escape `rootfs_dir`.
- The normalized guest result is always absolute and has no trailing slash except `/`.
- The host result is `rootfs_dir + normalized_guest_path`; guest `/` maps to `rootfs_dir`.
- Translation is lexical only. It does not resolve host symlinks, check file existence, or inspect rootfs contents.
- Embedded NUL bytes are invalid.

## Environment skeleton

The initial environment helper produces stable defaults:

- `ALR_PACKAGE`: Android package name.
- `ALR_ROOTFS`: host rootfs path.
- `HOME`: default `/root` unless explicitly supplied.
- `TMPDIR`: default `/tmp` unless explicitly supplied.
- `PATH`: default `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin` unless explicitly supplied.

Backend-specific variables are layered by launch-plan code rather than hidden in the shared helper.

## Test requirements

- Host-native C++ tests cover absolute normalization, relative resolution, `..` clamping, rootfs host translation, and invalid relative `rootfs_dir` handling.
- Python source tests ensure the documented API and Android CMake integration remain present.
- Future backend work must add behavior tests before connecting path translation to syscall or loader code.
