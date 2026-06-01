# PC-gated seccomp path mediation

Status: design + scaffolding landed on host; device verification pending (no NDK
on this build box). Files in play:

- Loader: `app/src/main/cpp/runtime_report.cpp`
  (`alr_install_path_trace_filter` and the `PTRACE_EVENT_SECCOMP` supervisor).
- Interposer: `app/src/main/cpp/alr_interpose/libalr_interpose.c`
  (LD_PRELOAD shim; ships in the rootfs tar, not the APK).
- Build: `scripts/build-interpose.sh`.
- Host model/tests: `tests/test_pcgate_bpf_logic.py`.

This note is the shared contract all three agents (loader, interposer,
build/test) conform to. It resolves a filter-install **ordering** problem and
specifies the A/B and on-device verification procedures.

---

## 1. Problem

The ALR loader runs an unmodified Ubuntu 24.04 arm64 glibc guest (e.g. GIMP)
in-process and must redirect every absolute guest path (`/etc/...`,
`/usr/share/...`) to its rootfs host location (`<ALR_ROOTFS>/etc/...`). The
proven, byte-for-byte-glibc-safe primitive is a stacked seccomp filter that
`SECCOMP_RET_TRACE`s the nine path-taking syscalls; the parent ptrace supervisor
then rewrites the pathname argument in the tracee's memory before the kernel
dereferences it (TOCTOU-safe, at syscall-entry).

That works but is slow: each traced path syscall costs a `PTRACE_EVENT_SECCOMP`
stop, a `/proc/<tid>/mem` read, a rewrite, and a `PTRACE_CONT` — microseconds
**per file syscall**, in the parent, serialized. GIMP opens thousands of files
during startup (fonts, brushes, data, config), so the round-trip dominates and
the main window does not appear within the watchdog.

The in-process fix is the LD_PRELOAD interposer (`libalr_interpose.c`): it wraps
the libc path entry points and rewrites the path to `<ALR_ROOTFS>+path` in a
stack buffer (nanoseconds, no syscall round-trip), so by the time the real
syscall fires the path is already correct and needs no ptrace mediation.

But the interposer and the loader **both** install seccomp filters, and seccomp
filters **stack**: for any syscall the kernel evaluates *every* installed filter
and takes the **most-restrictive** action. A later filter can never *loosen* an
earlier one (`RET_TRACE` beats `RET_ALLOW`). So if the loader keeps tracing all
nine path syscalls, the interposer's later `RET_ALLOW` is overridden and nothing
speeds up — the ptrace stop still happens on every path syscall even though the
path is already rewritten and the supervisor merely no-ops it.

We need a way for the interposer's already-rewritten syscalls to skip tracing,
while still tracing everything the interposer cannot reach (static binaries, raw
inline syscalls, anything before the interposer's constructor runs).

---

## 2. Ordering resolution (the core idea)

Gate the trace decision on **where the syscall came from**, using the
`instruction_pointer` that seccomp records in `seccomp_data`:

- The interposer routes *all* of its real syscalls through **one** trampoline
  function that contains the only `svc #0` it ever executes. That trampoline's
  mapped address range is `[alr_tramp_lo, alr_tramp_hi)`.
- A path syscall whose IP is inside `[alr_tramp_lo, alr_tramp_hi)` is **trusted**
  (the path was already rewritten in-process just before the `svc`), so the
  filter returns `RET_ALLOW`.
- A path syscall from **any other** IP (libc inline syscalls, a static guest, a
  raw `svc` in app code, the pre-constructor window) returns `RET_TRACE` and the
  loader's supervisor mediates it as before.

Because the gate must be the *most-restrictive* opinion to take effect for the
fast path, the **loader must stop tracing the path syscalls** in this mode (it
traces only `execve`/`execveat` for W^X exec re-entry control). Then the
interposer's per-IP filter is the only filter with an opinion on path syscalls,
and its `RET_ALLOW` for trampoline-origin syscalls actually wins.

### Why leaving the pre-constructor window un-mediated is sound

The interposer `.so` is mapped by `ld.so` only **after** the loader jumps to
`ld.so`, and its constructor runs during `ld.so`'s init phase **after** all
shared libraries are already mapped. Those library opens used the absolute
`LD_LIBRARY_PATH` the loader injected (already-correct rootfs paths needing no
rewrite), so the brief pre-constructor window has no path that needs mediation.
(`/etc/ld.so.cache` may open the host copy and miss; `ld.so` then falls back to
`LD_LIBRARY_PATH` search — harmless.) From the constructor onward, **all**
application file I/O is PC-gated.

### Fail-safe direction

If the trampoline range is ever wrong or stale — e.g. after an `execve`
re-entry re-maps the interposer at a new address but the gate window still holds
the old mapping — a path syscall from the *real* trampoline PC no longer matches
the window and therefore falls to `RET_TRACE`. That is **slower but never a
silent ALLOW**. Completeness can only degrade toward *more* tracing, never less.
The host model encodes this as the soundness theorem `S3` in
`tests/test_pcgate_bpf_logic.py`.

---

## 3. Contract

### Shared env var

`ALR_PCGATE` ∈ {`"0"`, `"1"`}, default `"1"`.

- The **loader** reads `ALR_PCGATE` from the **host** env (same mechanism as
  `ALR_DISABLE_INTERPOSE`).
- The loader **propagates** it into the guest env
  (`guest_env.push_back("ALR_PCGATE=<0|1>")`) so the interposer constructor can
  read it via `getenv`.

### Who installs which filter

**`ALR_PCGATE == 1` (fast path):**

- **Loader** (`alr_install_path_trace_filter` call site, ~line 1691): install a
  **reduced** filter that `RET_TRACE`s **only** `execve` and `execveat`
  (W^X exec re-entry control), `RET_ALLOW` for everything else — including the
  nine path syscalls (the loader does **not** trace them in this mode).
- **Interposer** (constructor): install the PC-gated path filter:

  ```text
  if   seccomp_data.arch != AUDIT_ARCH_AARCH64                     -> RET_ALLOW
  elif instruction_pointer in [alr_tramp_lo, alr_tramp_hi)         -> RET_ALLOW
  elif nr in {openat, openat2, newfstatat, statx, faccessat,
              faccessat2, readlinkat, mkdirat, unlinkat}           -> RET_TRACE
  else                                                             -> RET_ALLOW
  ```

**`ALR_PCGATE == 0` (baseline, for A/B):**

- **Loader** installs the **current** full nine-path-syscall `RET_TRACE` filter
  (unchanged behavior).
- **Interposer** installs **no** filter; it still rewrites paths in-process, but
  the loader's full filter traps every path syscall and the supervisor
  idempotently no-ops the already-rewritten path. This reproduces today's
  behavior for measurement.

### Stacking soundness (restated)

Seccomp filters are stacked; the kernel takes the most-restrictive action per
syscall. Therefore in `PCGATE=1` the loader **must not** trace path syscalls,
or the interposer's later `RET_ALLOW` is overridden. In `PCGATE=0` the
interposer installs nothing, so the loader's filter is authoritative.

### Trampoline (interposer)

Introduce one function — e.g. a small naked aarch64 stub
`alr_tramp_syscall(long nr, long a0..a5)` — that contains the **only** `svc #0`
the interposer uses for real syscalls. Export its mapped range as
`[alr_tramp_lo, alr_tramp_hi)` by one of:

- a dedicated linker section with start/stop symbols, or
- `&alr_tramp_syscall` plus a known byte size, or
- reading `/proc/self/maps` for the interposer's executable segment at
  constructor time.

After rewriting the path, the interposer's path wrappers **must** emit the
underlying syscall through this trampoline (not via `dlsym(RTLD_NEXT, ...)`
libc), so the trusted PC is unique and only reached post-rewrite. Non-path
passthrough may still use `RTLD_NEXT`.

### Path mediation via the trampoline

- **open/openat family:** prefer
  `openat2(rootfs_dirfd, relpath, {resolve: RESOLVE_IN_ROOT})`, where
  `rootfs_dirfd` is an `O_PATH | O_DIRECTORY` fd of `ALR_ROOTFS` opened once at
  constructor time. `RESOLVE_IN_ROOT` gives kernel-enforced, symlink-escape-safe
  confinement. Detect `openat2` availability (kernel ≥ 5.6 / API 31+); on
  `ENOSYS`, fall back to the existing `<ROOTFS>+path` string rewrite + plain
  `openat`.
- **other path syscalls** (`newfstatat`, `statx`, `faccessat`, `faccessat2`,
  `readlinkat`, `mkdirat`, `unlinkat`, `renameat2`): use a rootfs-prefixed
  absolute path through the trampoline (`RESOLVE_IN_ROOT` has no variant for all
  of these). Keep the rewrite idempotent and pass `/proc`, `/sys`, `/dev`
  through unchanged.

### seccomp_data offsets (aarch64 UAPI)

```text
nr@0 (u32)   arch@4 (u32)   instruction_pointer@8 (u64)   args@16 (u64[6])
```

In the BPF program, load the 64-bit IP as two 32-bit words (lo @ off 8,
hi @ off 12) and do a `[lo, hi)` range compare. The arch guard is
`AUDIT_ARCH_AARCH64` and must precede the nr/IP inspection (so a foreign-arch
syscall returns `RET_ALLOW` before the gate runs).

### Filter instruction layout (performance, decision-invariant)

The kernel evaluates this stacked filter on **every** syscall the guest makes,
so the per-instruction cost shows up on syscall-heavy hot paths even when the
gate intercepts nothing (`traps == 0`). A device microbench of a raw
`syscall(SYS_getpid)` storm measured ~24 ns/op of this evaluation overhead.

The filter therefore **classifies `nr` first** and only runs the (expensive,
two-word) IP-range test for the rare 9 path syscalls. A non-path syscall is
`RET_ALLOW` regardless of IP, so reordering nr-before-PC is decision-identical
to PC-before-nr while making the common case far cheaper:

```text
arch != AUDIT_ARCH_AARCH64                     -> ALLOW        (insn 0-2)
nr not in the 9 path syscalls                  -> ALLOW        (<= 7 insns)
nr in the 9  AND  IP in [lo,hi)                -> ALLOW        (PC gate)
nr in the 9  AND  IP outside [lo,hi)           -> TRACE        (PC gate)
```

`nr` is matched by a small **bracketed** tree (`nr>79` split, then a `(80,291]`
band) rather than a 9-way linear scan, chosen so the hottest harmless syscalls
land on a 7-instruction `RET_ALLOW`: `read`(63), `write`(64), `futex`(98),
`clock_gettime`(113), `clock_nanosleep`(115), `rt_sigprocmask`(135),
`getpid`(172), `gettid`(178), plus `mmap`/`mprotect`/`execve`/`execveat`/
`exit_group`. The 9 path syscalls {34,35,48,56,78,79,291,437,439} still route to
the PC gate. Total program length 29 (≤ `BPF_MAXINSNS` 4096), forward-jumps only.

This is purely an instruction reordering: the security invariants are unchanged.
A non-path syscall was always `ALLOW`; a path syscall still `RET_TRACE`s unless
emitted from the trampoline PC (so the supervisor backstop and the S1–S5
soundness theorems in `tests/test_pcgate_bpf_logic.py` hold verbatim). W^X is not
this filter's concern: `mmap`/`mprotect` PROT_EXEC is gated by the Android
platform's own SELinux/seccomp policy beneath this stacked filter.

### File ownership

No file is edited by two agents:

- `libalr_interpose.c` — interposer agent only.
- `runtime_report.cpp` — loader agent only.
- New files under `scripts/`, `docs/`, `tests/` — build/test agent only.

### Reporting

The supervisor already counts `path_traps` and `path_rewrites`; keep them
surfaced so an A/B run shows the trap count collapse. The report's
`seccomp_trace_events` / `path_traps` is the primary A/B metric.

### Deferred (explicitly out of scope now)

- `SECCOMP_USER_NOTIF` (would remove ptrace entirely but is a larger change).
- An in-interposer stat cache (memoizing translated paths / negative lookups).

Both are design-noted here and intentionally not implemented in this pass.

---

## 4. A/B procedure (host-measurable design intent, device-confirmed)

Goal: show that `PCGATE=1` collapses `path_traps` toward zero relative to the
`PCGATE=0` baseline, with no loss of path-mediation correctness.

1. Build the interposer: `scripts/build-interpose.sh`, capture the printed
   sha256, and pack the `.so` into the rootfs tar at
   `./usr/lib/androlinux/libalr_interpose.so`.
2. Build the APK with the NDK (off-host) and deploy with the repacked rootfs.
3. Run the same file-heavy guest twice, changing only the host env var:
   - Baseline: `ALR_PCGATE=0` → loader traces all nine path syscalls.
   - Fast path: `ALR_PCGATE=1` → interposer PC-gates; loader traces only
     `execve`/`execveat`.
4. Compare from the runtime report:
   - `path_traps` (a.k.a. `seccomp_trace_events`): expect a large drop in
     `PCGATE=1` (ideally to ~0 once the interposer covers the libc entry points
     the guest uses).
   - `path_rewrites`: in `PCGATE=0` this tracks the supervisor rewrites; in
     `PCGATE=1` supervisor rewrites should be near-zero (the interposer did them
     in-process). Path resolution must still resolve to the rootfs in both runs.
5. Cross-check `ALR_DISABLE_INTERPOSE=1` still behaves (A/B with the interposer
   entirely off remains intact).

The host unit test `tests/test_pcgate_bpf_logic.py` models exactly this A/B at
the decision level: `test_ab_pcgate1_traces_strictly_fewer_path_syscalls_than_baseline`
and `test_ab_fully_gated_workload_collapses_traces_to_zero` assert that the
trampoline-origin path syscalls are spared while the baseline traces them all.

---

## 5. On-device verification plan

1. **Build** the APK with the Android NDK (arm64-v8a) — cannot be done on the
   current host (no NDK; zig + cmake + clang only).
2. **Repack** the 252 MB rootfs tar to include the freshly built
   `libalr_interpose.so` at `./usr/lib/androlinux/libalr_interpose.so` (sha256
   from `build-interpose.sh` recorded in the build log for auditing).
3. **Functional gate:** launch the existing `fileio-test`-style guest and a
   dynamic glibc guest; confirm rootfs reads resolve (e.g. guest `/etc/...`
   reads hit `<ALR_ROOTFS>/etc/...`) under both `PCGATE` modes.
4. **Performance gate (the payoff):** launch a file-heavy GUI guest (GIMP
   startup is the canonical stress case — thousands of font/brush/data/config
   opens). Record:
   - `path_traps` for `PCGATE=0` vs `PCGATE=1` (expect a steep collapse).
   - Wall-clock to first window / to interactive, for both modes.
5. **Soundness gate:** force a stale trampoline window (e.g. via an `execve`
   re-entry path) and confirm path syscalls degrade to `RET_TRACE` (supervisor
   still mediates; no missed rewrite, no escape). This is the device analogue of
   the `S3` host test.
6. **Constraint checks:** glibc/`ld.so` byte-for-byte unmodified; public Android
   APIs only; arm64-v8a; W^X (no anon `PROT_EXEC`); the 236 host tests still
   pass; `ALR_DISABLE_INTERPOSE` A/B intact.
