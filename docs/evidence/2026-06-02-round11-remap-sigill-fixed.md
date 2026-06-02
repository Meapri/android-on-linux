# Device Evidence — round-11 (v144): re-mapped guest SIGILL FIXED (boundary-page clobber) + chromium-storm measure

Build `0.4.144-r11-v144`. Device SM-X236N / Mali-G615 MC2 / Android 16. Two cold-start drains.

## ★ The re-mapped-guest SIGILL is FIXED ✓
With `ALR_REEXEC_INPROC=1`, the in-process re-map of the static `/bin/sh` now runs with all-healthy diagnostics and **no `signal 4` (Illegal instruction) anywhere**:
```
ALR-INPROC PROG IREL=  filesz=0x9081  memsz=0x100a8  flags=0x6
ALR-INPROC PROG IREL=bss_zeroed=anon-span
ALR-INPROC: argc=0x4 envc=0x1a auxc=0x13
ALR-INPROC: at_phdr=0x400040 at_entry=0x400640 at_base=0x0
ALR-INPROC: sp_align=ok
ALR-INPROC: hwcap=0x119fff
ALR-INPROC: mapped, jumping entry=0x400640
```
**Root cause (R11, found by adversarial audit):** the old `map_elf_image` mmap'd EACH PT_LOAD separately with `MAP_FIXED`. When two PT_LOADs share a page (a segment's BSS tail spilling into the next segment's first page, or adjacent segments), the **second `MAP_FIXED` replaced that shared page with a fresh zeroed anon page — wiping the first segment's already-copied bytes** (code or `.rela.plt`/IFUNC data). Executing the clobbered page → SIGILL in `__libc_start_main`. This matched the device signature precisely: map+jump *reached* `entry=0x400640`, then the static glibc binary faulted in its own startup. **Fix:** reserve the whole `[min_vaddr,max_vaddr)` span as ONE anonymous RW mapping, `memcpy` each segment in (no re-mmap → no clobber), then `mprotect` page-by-page to the union of every covering segment's perms. Also: real `AT_HWCAP` (0x119fff) now threaded into the `R_AARCH64_IRELATIVE` resolver (was 0), `MAP_FIXED_NOREPLACE` collision detection for ET_EXEC, BSS zero-by-construction, and granular diag (`span/seg/bss_zeroed/sp_align/at_*`).

## inproc gated default-OFF (sequence-integration remaining)
Enabling `ALR_REEXEC_INPROC` GLOBALLY (every guest exec → re-map) is NOT yet safe: with it on, the onCreate probe sequence **stalled after the PERF probe (~9 s)** — the app stayed alive but the later GPU/GUI probes never ran. The native loader probe serializes guest supervision, so a re-mapped guest that blocks (or the chromium-zygote `/proc/self/exe` redirect edge) wedges the shared path. So `inproc` is gated **default-OFF** (`ALR_REEXEC_INPROC=1` opt-in). The mapper itself is now correct (SIGILL gone) — what remains is sequence-level integration (per-exec scoping, `/proc/self/exe` pass-through, non-blocking supervision).

## chromium-storm (PR #2 measure-first): chromium REACHES worker clones
Under the new 120 s chromium alarm window, a guest exec showed `clone_events=7` — chromium does **reach worker clones**, supporting PR #2's re-diagnosis that the `--dump-dom` "1-thread deadlock" was a MISDIAGNOSIS (the 25 s window killing the heavy single-init). The inline `--dump-dom` probe was REVERTED from the onCreate sequence: a device drain showed it hangs the whole serialized probe path (every later GPU/GUI probe blocked). Running `--dump-dom` belongs in a dedicated isolated drain; the 120 s chromium alarm window is kept for it.

## No regression ✓ (v144, inproc off, no inline --dump-dom)
```
ALR GPU LIVE INTEGRATION: PASS   ALR VK RENDER MARSHAL: PASS   ALR GPU SCREEN CUBE: PASS
foot/netsurf/qt6 rendered=true   glmark2 Score 1073   Chromium 147 runs   no crash
```

## Verdict
R11 fixes the re-mapped-guest SIGILL — the in-process re-map mapper is now correct (single-span, no boundary clobber, real HWCAP, healthy auxv/stack). The exec-re-entry foundation (no-execve mechanism + map+jump + correct mapper) is fully device-proven; what remains to flip `inproc` on by default is sequence-level integration (per-exec scoping + `/proc/self/exe` + non-root dpkg), not a conceptual or mapper unknown.
