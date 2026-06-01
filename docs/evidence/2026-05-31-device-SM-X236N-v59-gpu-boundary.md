# Device Evidence — SM-X236N, APK v59 (GPU passthrough boundary cost)

Decisive go/no-go measurement for whether the guest→host GPU boundary can be crossed at low overhead. Same device (SM-X236N, MediaTek MT6878, Mali-G615 MC2, Android 16 / API 36, arm64-v8a). APK `0.4.59-android-gpu-boundary-v59`, SHA-256 `716026b2243ac3cd4737db8ba69a91fa918c9602dd7772d0edcfd52e3f258678`.

## What was measured

The host GPU hardware render is already proven (v54: Mali-G615, Vulkan WSI + EGL/GLES, 0 dropped frames). The open question for "good GPU performance" is the **boundary cost** — how expensive it is to get a guest GPU command to the host. `build_gpu_boundary_probe` measures three boundary mechanisms on-device and converts each to commands that fit a 60fps (16.67ms) frame:

| Mechanism | ns / command | commands per 60fps frame | verdict |
| --- | --- | --- | --- |
| in-process function dispatch (ALR loaded into host address space) | 9 | 1,851,851 | essentially free |
| shared-memory batched ring (gfxstream-style, 1 cross-proc sync / 256-cmd batch) | 158 | 105,485 | viable |
| socket per-command round-trip (naive IPC) | 33,276 | 500 | does not scale |

A draw-call-heavy game frame is ~2,000–5,000 calls. The shared-ring path clears that with ~20–50× headroom; the in-process path is effectively unlimited. Naive per-command socket IPC (≈500 calls/frame) is the only mechanism that fails.

## Verdict

`ALR GPU PASSTHROUGH BOUNDARY: VIABLE`. The boundary is **not** the blocker for low-overhead GPU passthrough on this device. Two routes both work:

- **inter-process + shared-ring** (gfxstream-style): 105K cmds/frame — fine even without ALR's in-process loader.
- **in-process** (ALR loads the guest into the host address space, enabled by the v57 `execmem` PASS): ~1.85M cmds/frame — boundary cost vanishes.

The current guest GPU bridge uses TCP-loopback per-command IPC, which matches the slow "socket per-command" row — that transport must be replaced with a shared-memory ring. That is engineering, not a fundamental wall.

## Honest scope

This measures the **transport boundary cost**, not the actual Vulkan/GLES command marshalling (argument serialization, handle translation) that a real passthrough layer must also do. That marshalling is real work, but it is the same work gfxstream/virgl already do and is known-tractable. Combined with the proven host GPU render, the conclusion is: **the GPU side has no fundamental blocker** — the boundary is cheap enough and the hardware render works. The remaining GPU work is implementing the command-translation layer over a shared ring.

This run also confirms the app recovered from the v58 heavy-diagnostic crash (the verbose `dpkg -i` diagnostic was removed): all prior gates render normally — `ALR EXECMEM ANON RX NATIVE EXEC: PASS`, `HOST GPU EGL/GLES EXECUTION: PASS`, Vulkan probe PASS, surface gates PASS after callback.

Next: #1 — prove ALR can natively execute a guest ELF via an anonymous-execmem userspace loader (the `execmem` PASS is the enabler; the loader itself is the remaining work).
