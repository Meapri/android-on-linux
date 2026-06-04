# In-process re-map: NON-PIE ET_EXEC fix + apt gpgv fd-3 survival fix

Date: 2026-06-04. Branch: see commit trailer. Two device-proven loader bugs in the
in-process exec re-entry, both addressed in `app/src/main/cpp/alr_inproc_reexec.c`
(FIX 1 + the trampoline half of FIX 2) and `app/src/main/cpp/runtime_report.cpp` (the
launch-child half of FIX 2). Host-implemented; 4-ABI native build green; host pytest
green. DEVICE-VERIFY checklist below (the device is owned by a concurrent agent).

## FIX 1 — NON-PIE (ET_EXEC) in-process re-map, execve-replacement aware

**Symptom (device).** `ALR-INPROC: prog map fail` when re-mapping a NON-PIE ELF
(`base=0x0 seg vaddr=0x400000`). Root cause of the **verdict-readback** bug:
`dpkg-query` (and `dpkg`, `dpkg-deb`, `dpkg-split`) are built **non-PIE** on Ubuntu
noble — ET_EXEC with ABSOLUTE vaddrs at the canonical 0x400000 text base, no
relocations to move them. The mapper must place the image AT 0x400000.

**The real wall.** The in-process re-map runs INSIDE the live process and EMULATES
execve (no kernel exec). When `dpkg` (non-PIE @ 0x400000) `fork()`+`exec`s
`dpkg-query` (ALSO non-PIE @ 0x400000), the forked child's address space STILL holds
`dpkg` mapped at 0x400000. The prior code reserved the fixed range with
`MAP_FIXED_NOREPLACE` and **bailed** (`EXEC_FIXED_OCCUPIED`) on the collision → the
target never mapped → `prog map fail` → `_exit(73)` → empty `dpkg-query` stdout → every
`dpkg --status` verdict read FALSE even when installs succeeded. (The same wall hit
`gnome-calculator`'s non-PIE appstream maintscript helper → exit 73 → 51-pkg configure
cascade.)

**Fix** (`map_elf_image`, ET_EXEC branch). A real execve UNCONDITIONALLY replaces the
whole address space, so the stale prior image MUST be evicted:
1. Try `MAP_FIXED_NOREPLACE` at the fixed `min_v` first — when the range is FREE (the
   common case; Android maps the app/bionic/loader at HIGH randomized addresses, so
   0x400000 is normally free) this claims it with zero clobber.
2. If it is OCCUPIED (EEXIST) — the only legitimate occupant of an ET_EXEC's fixed low
   vaddr is a PREVIOUS non-PIE guest we mapped — evict it with `MAP_FIXED` (atomic
   unmap+map), exactly as execve would replace it. New diag: `EXEC_FIXED_REPLACE@…`.
3. **Guard:** before replacing, read the live `SP` (`mov x,sp`) and check `&base` /
   `&map_elf_image`; if the fixed range overlaps our live stack or trampoline `.text`,
   BAIL (`EXEC_FIXED_HITS_LIVE@…`) — a clean, diagnosable refusal, never corruption. In
   practice SP/text are at high addresses, far from 0x400000, so the guard never fires;
   it makes the replace provably safe.

The old hard `EXEC_FIXED_OCCUPIED` bail is removed. ET_DYN (PIE programs, ld.so) is
unchanged — the kernel picks the base, no collision.

## FIX 2 — apt gpgv `--status-fd N` survival through the deep re-map

**Symptom (device).** At every rung of the `apt → apt-key(sh) → gpgv` re-map the fd-3
audit showed `status-fd3=CLOSED` + `inheritable_fds>=3=0x0` (should be `OPEN-keep` /
`0x1`). gpgv's `--status-fd N` write then EBADFs → apt never sees `GOODSIG` → "the
repository is not signed".

**Two compounding root causes, both fixed:**

(a) **Hard-wired fd 3 + leaked supervisor fds (number perturbation).** apt does NOT use
a fixed fd 3 for the status pipe — it uses the pipe's own fd number and passes
`--status-fd N`. The launch child (`runtime_report.cpp`) leaked its capture pipes
(`out_pipe[1]`, `diag_pipe[1]`) into the guest as **non-CLOEXEC** fds. Those (i) shift
the guest's low fd numbers so apt's status pipe no longer lands on the native fd, and
(ii) being non-CLOEXEC, survive EVERY re-map sweep → permanently inflate the survivor
count (a stray status-pipe-like write end → the pipe never EOFs).
- Fix: in the launch child, **close the redundant `out_pipe[1]`** right after dup2'ing
  it onto stdout/stderr (the dups carry the output), and mark **`diag_pipe[1]` (dg)
  CLOEXEC** so the execve-emulating sweep drops it at the guest's first child exec. The
  guest now sees the SAME clean fd table a real kernel-exec'd process would (stdio + the
  deliberately-inherited GPU/VK ring fds only).

(b) **Status fd dropped by the CLOEXEC sweep.** If any process in the deep chain leaves
the status fd CLOEXEC (or the perturbation above moves it), the sweep wrongly closes it.
- Fix (trampoline): **parse `--status-fd N` / `--status-fd=N` from the guest argv**
  (`find_status_fd`) and **clear CLOEXEC on fd N BEFORE the sweep** (`protect_status_fd`,
  via `fcntl(F_SETFD)`) so the sweep KEEPS it — replicating apt's own pre-exec clear for
  our no-execve re-map. Only an OPEN, guest-named fd is touched (no blind whitelist). The
  audit is now parameterized by the parsed N (`audit_status_fd(status_fd)`) so a
  perturbed number is reported, not a falsely-CLOSED hard-wired 3. New diags:
  `status-fd-arg=`, `status-fd-uncloexec=yes/no`, `status-fd-num=`, and (non-3 case)
  `status-fdN=OPEN-keep`. The `status-fd3=` token is retained for the fd-3 case (device
  grep + host source-invariant contract).

## Build + host verification (this change)

* 4-ABI compile of `alr_inproc_reexec.c` (NDK 27.2 clang, `-std=c11 -Wall -Wextra
  -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2 -O2`, also -O0): aarch64 /
  armv7a / x86_64 / i686 → all OK. New helpers file-local (`t`), trampoline exported.
* CMake `alr_loader` SHARED link for ALL 4 ABIs (arm64-v8a / armeabi-v7a / x86 / x86_64)
  → all OK (links both modified TUs). `runtime_report.cpp` arm64 `-fsyntax-only` clean.
* interpose `libalr_interpose.so` via `scripts/build-interpose.sh` → OK (unchanged).
* host pytest: **2078 passed, 8 skipped** (was 2007; +21 new model/source tests for
  both fixes in `tests/test_elf_remap_model.py` + `tools/elf_remap_model.py`).

## DEVICE-VERIFY CHECKLIST — for the device-owning agent

### A. NON-PIE re-map (FIX 1) — fast, no network

Run a NON-PIE guest directly (the loader's native-loader probe / a guest shell):
`dpkg-query --version` (or `dpkg --status bash`). dpkg-query is non-PIE ET_EXEC.

In `logcat -s alr_loader alr_cr_out`:
- [ ] `ALR-INPROC: worker target=…/usr/bin/dpkg-query`
- [ ] `ALR-INPROC PROG IREL=…base=0x0` then `seg vaddr=0x400000` (the ET_EXEC mapped at
      its fixed base) — and **NO** `prog map fail`, **NO** `EXEC_FIXED_OCCUPIED`.
- [ ] For the deep case (`dpkg` → `dpkg-query`, both non-PIE @ 0x400000), expect
      `ALR-INPROC PROG IREL=…EXEC_FIXED_REPLACE@0x400000` at the SECOND one (the prior
      `dpkg` image is evicted, execve-style) — still no `prog map fail`.
- [ ] **Verdict readback works:** `dpkg-query`/`dpkg --status <pkg>` prints a real status
      stanza to stdout (non-empty), so install verdicts read TRUE after a successful
      install. (Was: empty stdout → FALSE verdicts.)
- [ ] You should never see `EXEC_FIXED_HITS_LIVE` (would mean 0x400000 overlapped live
      state — not expected on Android; if it appears, capture the SP/text addresses).

### B. Authenticated apt gpgv fd-3 survival (FIX 2)

Prereqs (unchanged from the prior doc): `build_apt_mirror_overlay.py --authenticated`
+ `build_apt_dpkg_overlay.py --self-contained`; app sets `ALR_REEXEC_INPROC=1` +
`ALR_FAKEROOT=1`. Run `apt-get update` (authenticated). In `logcat -s alr_loader
alr_cr_out`:
- [ ] `ALR-INPROC: worker target=…/usr/lib/apt/methods/gpgv`, then `shebang interp=/bin/sh`
      … `worker target=…/bin/dash` (apt-key `#!`), then `worker target=…/usr/bin/gpgv`.
- [ ] at the **gpgv** re-map: `ALR-INPROC: status-fd-arg=0x3` (or the actual N apt used),
      `ALR-INPROC: status-fd-uncloexec=…`, then `status-fd3=OPEN-keep` (or `status-fdN=
      OPEN-keep` if N≠3) **and** `inheritable_fds>=3=0x1` (exactly the status fd).
- [ ] supervisor: `alr CR4 blocked-syscall nr=144 -> OK(0)` (and 146) — cred-drop fired.
- [ ] gpgv emits `[GNUPG:] GOODSIG … 871920D1991BC93C` / `VALIDSIG …C93C` on the status fd.
- [ ] apt does **NOT** print `E: The repository … is not signed`; `apt-get update` exits 0.

Failure localization (new audit):
- `status-fd-arg=` absent at the gpgv rung → gpgv argv carried no `--status-fd` (a
  different apt path; cross-check the gpgv argv).
- `status-fd-uncloexec=yes` means the named fd WAS CLOEXEC and we cleared it (the fix
  fired); `=no` means it was already clean (healthy). Either is fine if the next line is
  `OPEN-keep`.
- `status-fd{3,N}=CLOSED` at the gpgv rung → the named fd is genuinely closed upstream
  (apt did not leave it open at all). `inheritable_fds>=3=0x2`+ → a stray inheritable fd
  still leaks beside the status fd; the depth where the count first exceeds 1 names the
  offending re-map.
