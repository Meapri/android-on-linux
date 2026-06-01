# Device Evidence — SM-X236N, v79: dynamically-linked glibc runs via an in-process ld.so

A **dynamically-linked** glibc ARM64 program runs natively in-process by loading the guest's own `ld-linux-aarch64.so.1` and handing control to it — the interpreter links `libc.so.6` from the rootfs and runs the program. This unlocks the rootfs's real dynamic userland (dash, coreutils, dpkg, apt), all on non-root Android, W^X-safe, no PRoot.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, arm64-v8a, untrusted_app). APK `0.4.79-android-dynloader-v79`, SHA-256 `1db380dc775905eac80d8f23cf2d16640ffb2b0b9a99a22805ce4de45415c329` (the build that adds the real-Debian-program probes below; the dynhello-only build was `4215068b…`).

## Capstone: REAL Debian programs run (not our test binaries)

```
ALR REAL DEBIAN PROGRAM (/usr/bin/env, dynamic coreutils):       PASS
ALR REAL DEBIAN PROGRAM (/usr/bin/id, libselinux+rootfs /etc):   PASS

/usr/bin/id  -> LINK MODE DYNAMIC, INTERP MAP PASS, jumped-to-entry, exit 0, no fault
   stdout: uid=10326 gid=10326 groups=10326,3003,9997,20326,50326
/usr/bin/env -> LINK MODE DYNAMIC, INTERP MAP PASS, jumped-to-entry, exit 0, no fault
   stdout: the guest environment (GLIBC_TUNABLES, LD_LIBRARY_PATH, …)
```

These are the actual Debian bookworm coreutils binaries shipped in the rootfs — `/usr/bin/id` (dynamically linked against `libc.so.6` + `libselinux.so.1`, reads `/etc/passwd`/`/etc/group` via the path-mediated rootfs) and `/usr/bin/env` — running natively in-process. `id` printed the live uid/gid/groups (the app's own uid `10326`, since fakeroot/uid-0 mapping is a later feature); it executed `getuid`/`getgid`/`getgroups` and the NSS path, exit 0. This is the end-to-end demonstration: a real Linux userland program, unmodified, runs on non-root Android through the ALR loader.

## Result

```
ALR NATIVE LOADER GUEST EXEC (glibc static):                 PASS  (/bin/hello — no regression)
ALR NATIVE LOADER GUEST EXEC (glibc threads+fork):           PASS  (/bin/mt-test — no regression)
ALR LOADER PATH-MEDIATION REAL FILE READ:                    PASS  (/bin/fileio-test — no regression)
ALR NATIVE LOADER GUEST EXEC (glibc DYNAMIC via ld.so):      PASS  (/bin/dynhello)

dynhello:
  LINK MODE: DYNAMIC(interp-handoff)
  INTERP MAP: PASS
  reached=jumped-to-entry
  child exit=0  signal=0  fault signo=0       (no fault anywhere)
  diag: DYN; INTERP_READ=0x31cc0 (203968 B) PROG:IREL=0 INTERP:IREL=0 MAPPED; SIGRESET; …JUMPING
  stdout: alr-dyn-ok
```

`/bin/dynhello` is a dynamically-linked PIE (`ET_DYN`, `PT_INTERP=/lib/ld-linux-aarch64.so.1`, `NEEDED libc.so.6`), built with `zig cc --target=aarch64-linux-gnu.2.36 -fPIE -pie`. It prints `alr-dyn-ok` and exits 0 — proof the in-process `ld.so` linked it against the rootfs glibc and ran it.

## How the dynamic path works

The loader now branches on `PT_INTERP` (static path unchanged). For a dynamic executable it does what the kernel's `binfmt_elf` does, in userspace:

1. **Read `PT_INTERP`** from the program → open `rootfs_dir + "/lib/ld-linux-aarch64.so.1"` (a real file in the rootfs, not a symlink) and read it (`INTERP_READ=0x31cc0`).
2. **Map both images** via the shared, refactored `map_elf_image_into_execmem` helper (anonymous RW→memcpy→RX, the W^X-safe path) — the program and the interpreter (`MAPPED;`, `INTERP MAP: PASS`). Each image's IRELATIVE is applied; `ld.so`'s own `R_AARCH64_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/TLS relocs are left to `ld.so` (we do not pre-apply them).
3. **Build the auxv to describe the PROGRAM to the interpreter**: `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_ENTRY` are the program's; **`AT_BASE` = the interpreter's load base** (the one change that tells `ld.so` its own bias); `AT_EXECFN` added. Push `LD_LIBRARY_PATH=<rootfs>/lib/aarch64-linux-gnu:<rootfs>/lib:…` (absolute Android paths) so `ld.so` resolves `libc.so.6` from the extracted rootfs.
4. **Jump to the INTERPRETER's entry** (not the program's). `ld.so` self-relocates, maps `libc.so.6`, links the program, and jumps to `AT_ENTRY`.

## The W^X question, answered

The principal risk for the dynamic path was that `ld.so` maps `libc.so.6` with **file-backed `mmap(PROT_READ|PROT_EXEC)`** from the app-data filesystem, which SELinux `untrusted_app` *might* deny (`execute`/`execmod` on `app_data_file`). **It does not** — the program ran with no fault (`fault signo=0`), so `untrusted_app` permits file-backed `PROT_EXEC` of a rootfs `.so` under `filesDir`. This is the decisive unknown for the whole dynamic userland, and it holds on this device. (Anonymous `memfd`-exec remains EACCES — that is a different SELinux check; file-backed library mapping is allowed.)

## Significance

Combined with the prior milestones, the full mechanical stack to run a **real Debian userland** on non-root Android is now demonstrated end-to-end on a real device:
- Native static glibc exec (v73) + threads/fork (v76).
- Rootfs filesystem view via low-overhead selective seccomp path mediation (v78).
- **Dynamic linking via an in-process `ld.so` (v79, here)** — the rootfs's real dynamic programs can run.
- GPU marshalling → real Mali render (v69).

The static path is byte-for-byte unaffected (the map loop was refactored into the shared helper; hello/mt-test/fileio-test all still PASS). Verification: pytest 235 passed, native-core PASS, validate-host PASS, Gradle BUILD SUCCESSFUL.

## Next
- Run `/bin/dash -c 'echo ...'` and a coreutils binary (`/bin/ls`) — real multi-library dynamic programs — through the same path; these exercise more of `ld.so`'s `DT_NEEDED` closure and file syscalls (now path-mediated).
- Procfs synthesis (`/proc/self/exe` → guest exe) via the interposer.
- Remaining loader-audit hardening (supervisor `si_code==SYS_SECCOMP` guard, runaway kill-group, 16 KiB page size).
- For libraries `ld.so` can't file-exec in stricter domains, interpose its library `mmap` into the same anon-RW→memcpy→RX path.
