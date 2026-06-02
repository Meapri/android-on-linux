# Device Evidence — round-10 step 2 (v142/v143): in-process re-map maps+jumps the target — NO execve

Build `0.4.143-r10-v143` (and v142). Device SM-X236N / Mali-G615 MC2 / Android 16 / untrusted_app, targetSdk 35. Cold-start drains. ADR-003-v3 step 2: the real map+jump trampoline (`alr_inproc_reexec.c`, reuses `alr_reentry.c`'s freestanding mapper) replaces the step-1 probe. The supervisor cancels the guest execve and PC-redirects into the trampoline with x19=host target / x20=argv / x21=envp / x22=rootfs.

## ★ Supervisor wiring + trampoline map+jump — device-proven ✓
```
ALR-INPROC: worker target=/data/.../rootfs/debian-arm64/bin/sh
ALR-INPROC: static target (no PT_INTERP) — direct map+jump
ALR-INPROC PROG IREL=0x0
ALR-INPROC: mapped
ALR-INPROC: mapped, jumping entry=0x400640
ALR-INPROC sp@0x78d761f760
alr exec ... inproc=on inproc_redirected=2
```
The supervisor passed the correct rootfs-absolute target + rootfs; the worker opened `/bin/sh`, parsed it (this rootfs ships a **static** 1.9 MB `/bin/sh`, not the usual dash symlink), mapped its PT_LOADs, built a fresh SysV stack (argv/envp/auxv mined from `/proc/self/auxv` + raw uid/gid), and **jumped to its entry — with NO kernel execve**. The static-ELF path (added this round: no PT_INTERP → AT_BASE=0, jump straight to the program entry) reaches the jump. So the full chain — cancel execve → PC-redirect → resident trampoline → map target in-process → jump — runs end-to-end on device.

## Remaining: the re-mapped guest does not execute cleanly yet ⚠️
- The static `/bin/sh`, once jumped to, **SIGILLs** when actually driven (foot's interactive shell: `terminal.c:1770: slave exited with signal 4 (Illegal instruction)`). So the map+jump REACHES the binary's entry but the static glibc binary faults during its own startup — a static-execution correctness bug to chase next (candidates: IRELATIVE/IFUNC resolution for static binaries, BSS-tail zeroing on partial pages, TLS/TPIDR setup, or an auxv field the static `__libc_start_main` needs).
- A guest exec'd **`/proc/self/exe`** (interp `/system/bin/linker64`) — this is **Chromium's zygote** re-execing the *Android* app binary, which correctly cannot be mediated into the Debian rootfs (`interp open/read fail`). A `/proc/self/exe` exec needs a distinct handling path (or a pass-through) — separate from the rootfs-binary re-map.
- `apt install` still `unpacked=false`, but a key driver is now visible and **independent of exec-re-entry**: `dpkg: error: requires superuser privilege` (non-root dpkg refuses to unpack). So the apt grail also needs a fakeroot/root-emulation path, not only re-entry.

## No regression ✓
`ALR GPU LIVE INTEGRATION: PASS`, `ALR VK RENDER MARSHAL: PASS`, `foot/netsurf/qt6 rendered=true`, `glmark2 Score 1080`, Chromium 147 runs. The many dynamic guests launched directly (dpkg-query, apt, Xwayland, chromium) run fine. `ALR_REEXEC_INPROC` is gated **default-OFF** (opt-in `=1`) since the re-mapped guest SIGILLs — main behavior is unchanged from v141.

## Verdict
The hard conceptual + mechanical milestones are **device-proven**: exec-re-entry WITHOUT kernel execve, including mapping the target ELF in-process and jumping to it (static + dynamic paths wired). The remaining work is execution-correctness debugging of the re-mapped guest (the SIGILL) + the `/proc/self/exe` and non-root-dpkg edges — focused iterations on a proven foundation, not a conceptual unknown.
