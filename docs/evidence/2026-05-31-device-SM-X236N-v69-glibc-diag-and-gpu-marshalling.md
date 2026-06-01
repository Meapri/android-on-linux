# Device Evidence — SM-X236N, v62–v69 (glibc coexistence diagnosis + GPU marshalling)

Same device (SM-X236N, MediaTek MT6878, Mali-G615 MC2, Android 16 / API 36, arm64). APK `0.4.69-android-gpu-marshalling-v69`, SHA-256 `6ebb636fbb3471dacba059d3f70fedf6bdd967c74d80ca2a4610889ac4aa5a79`.

## 1) glibc-static in-process — precisely diagnosed (still blocked)

The userspace ELF loader is correct (v65 map verification: entry first instruction `nop; mov x29,#0`, `AT_PHDR` → valid `PT_LOAD`, `MAP_FIXED` exact, `argc=1`). The freestanding mechanism passes (v62, exit 42). The real static glibc `/bin/hello` still crashes, now root-caused with `ptrace`:

- Binary is **static** (no INTERP/DYNAMIC) with **5 `R_AARCH64_IRELATIVE` (IFUNC) relocs** and a **`PT_TLS`** segment.
- The parent traced the child and read the fault from outside (immune to the guest's TLS hijack): **`signal 11`, `pc=0x7b9d4a0fe0`, `addr=0x30`**, every run.
- `pc=0x7b9d4a0fe0` resolves (via `/proc/<pid>/maps`) into **`/apex/com.android.runtime/lib64/bionic/libc.so`** text (`7b9d46d000–7b9d50a000`). So the guest glibc deterministically jumps into **bionic libc** and dereferences `NULL+0x30`. The address is constant across launches because zygote preloads bionic libc at a fixed address.
- Fixes attempted and ruled out: applying all 5 IRELATIVE relocs in the loader (v67) — same crash; setting `TPIDR_EL0` to a clean zeroed TCB before entry (v68) — **identical crash**, proving it is *not* a simple TLS-content read.

Conclusion at the time: a deep coexistence problem. **UPDATE (v73): this was SOLVED — it turned out to be a ~20-line loader fix, not multi-week R&D.** The `addr=0x30` crash was NOT the guest glibc branching into bionic; it was a SECONDARY fault in ART's `libsigchain` SIGSEGV handler (inherited process-wide by the child), which ran bionic `pthread_getspecific` (reading bionic `TLS_SLOT_DTV` at TP+0x30) AFTER glibc had seized `TPIDR_EL0` for its own TCB. Fix: (1) reset all signal dispositions to SIG_DFL via the RAW rt_sigaction syscall to bypass libsigchain; (2) a parent-side SIGSYS supervisor that emulates the one zygote-seccomp-blocked syscall glibc uses (`set_robust_list`). See `2026-05-31-device-SM-X236N-v73-glibc-static-runs.md` for the PASS.

## 2) GPU command-marshalling layer — PASS on Mali hardware

Beyond the v59 boundary-cost measurement, the actual command-translation layer now works end-to-end on the device. `build_gpu_marshalling_probe`:

- **Guest side:** encodes a 73-byte GLES command stream — `VIEWPORT(0,0,64,64)`, `CLEARCOLOR(0.1,0.2,0.7,1)`, `CLEAR`, `ENABLE_SCISSOR`, `SCISSOR(24,24,16,16)`, `CLEARCOLOR(0.9,0.3,0.1,1)`, `CLEAR`, `DISABLE_SCISSOR` (8 commands).
- **Host side:** decodes the byte stream and dispatches each as a **real GLES call** in an EGL pbuffer context on the Mali GPU, then `glReadPixels` verifies the result.
- Result: `ALR GPU MARSHALLING DECODE+EXECUTE: PASS`, `ALR GPU MARSHALLING HARDWARE RENDER: PASS`. renderer `Mali-G615 MC2`, software=false, gl error 0. **center pixel = 229,77,26** (the scissored region rendered color B) and **corner pixel = 26,51,178** (outside kept color A) — exactly the commanded output.

This proves the marshalled guest command stream produces correct hardware GPU output — the core of GPU passthrough's translation layer (the work gfxstream/virgl do). Combined with v59 (cheap transport boundary) and the proven host render, the GPU pillar is substantially demonstrated; remaining is broadening the marshalled GLES/Vulkan API surface and wiring it over the shared ring.

## Net

- **GPU passthrough (2):** boundary VIABLE (v59) + command marshalling PASS with pixel verification (v69). Foundation strongly demonstrated.
- **Native CPU exec (1):** mechanism PASS (v62 freestanding); static-glibc in-process precisely diagnosed to bionic-libc runtime coexistence — deep R&D, not a quick fix.
