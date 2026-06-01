# Device Evidence — SM-X236N, APK v62 (native ELF loader / anon-mmap-loader)

Decisive proof for whether ALR can natively execute a guest ELF in-process, W^X-safe, with no PRoot/ptrace. Same device (SM-X236N, MediaTek MT6878, Mali-G615, Android 16 / API 36, arm64). APK `0.4.62-android-native-loader-selftest-v62`, SHA-256 `979da53f98d042b819022e1f248070f8e87a7613089e8b9a264069a90266526c`.

## Result: native-exec MECHANISM proven (PASS)

`ALR NATIVE LOADER MECHANISM (freestanding): PASS`. A 132-byte freestanding PIE ELF is assembled at runtime — its entry is just `mov x0,#42; mov x8,#93(exit); svc #0`. The userspace loader maps its `PT_LOAD` into an anonymous page, flips it to `PROT_EXEC` (the v57-proven execmem path), builds a minimal stack, and `br`s to the entry via a file-asm trampoline. The forked child **exits with code 42** (`signal=0`).

That is end-to-end native execution of a guest ELF: parse → map into anonymous execmem → jump to entry → guest code runs → guest exits. No PRoot, no ptrace, no writable-file exec, no SELinux denial. Combined with v57 (`execmem` PASS) and v59 (GPU boundary VIABLE), the two foundational unknowns are both answered positively.

## glibc-static binary: not yet (known coexistence layer)

`ALR NATIVE LOADER GUEST EXEC (glibc static): FAIL`. Loading the rootfs `/bin/hello` (a 537664-byte **ET_EXEC static glibc** binary, entry 0x400680) parses and maps cleanly (`MAP: PASS`, `reached=jumped-to-entry`) but the guest `SIGSEGV`s (signal 11) during glibc startup before printing — even with `AT_PHDR` corrected for ET_EXEC and `GLIBC_TUNABLES=glibc.pthread.rseq=0` passed in the guest env.

This is the expected harder layer: a full glibc runtime started **in-process inside a bionic app thread** collides on thread-local state (the thread already has bionic's TLS/TPIDR_EL0, signal handlers, etc.). PRoot sidesteps this by running the guest as a separate ptraced process; a real ALR would run the guest on a dedicated thread with cleared TLS or provide a glibc-compatible entry. The `mechanism` is proven; making a stock glibc-static binary start in-process is follow-on work (next step: capture the fault PC with a child SIGSEGV handler to pinpoint the glibc startup site).

## Bottom line for the two project pillars

- **Native CPU exec (1):** foundation PASS — `execmem` works and the userspace ELF loader executes guest code in-process W^X-safely. Real glibc binaries need a runtime-coexistence layer.
- **GPU passthrough (2):** foundation VIABLE — host GPU hardware render proven (Mali, 0 drops) and the boundary is cheap (shared-ring ~105K cmds/frame, in-process ~1.85M).

Both pillars have a proven foundation on a real Android 16 device, so the approach is not blocked at the fundamental level. What remains is engineering on top (glibc runtime coexistence; the GPU command-marshalling layer over a shared ring) plus multi-vendor (Adreno etc.) coverage.

## v63/v64 follow-up: glibc-static crash is deep

A child SIGSEGV/SIGBUS/SIGILL handler on an alternate stack was added (v63/v64) to capture the exact fault PC of the glibc-static crash. The handler **does not fire** — the guest dies with `signal 11` and no `FAULT;pc=...` line, even with `SA_ONSTACK`. A crash that bypasses an installed alternate-stack handler indicates the failure is in early glibc runtime state (TLS/TCB/signal state shared with bionic), not a simple bad-pointer dereference. This confirms glibc-static-in-process is an architectural coexistence problem, not a quick patch: the realistic path is to run the guest with its own clean thread/TLS (or a glibc-compatible entry), which is follow-on R&D. The freestanding mechanism proof (exit 42) stands as the decisive native-exec result.
