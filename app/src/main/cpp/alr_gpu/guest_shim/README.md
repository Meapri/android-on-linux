# ALR GPU-native M3 — guest-side GLES marshalling shim

This directory is the **guest half** of the gfxstream-style GLES marshalling layer
(Phase 4, milestone **M3**). It lets an **ordinary, unmodified glibc GLES2 binary**
(`alr-gles-cube`) drive the device's real **Mali GPU** by encoding every GL/EGL
call into a byte stream that the **host** decodes and replays on a real GLES2
context. The guest never sees the real GPU; it talks only to a shared-memory
command ring.

The **host decoder already exists and is committed** to the repo:
`app/src/main/cpp/alr_gpu/alr_gpu_decode.hpp` (`alr::gpu::decode_batch`) and the
SPSC ring `app/src/main/cpp/alr_gpu/alr_gpu_ring.hpp`. This shim's wire format is
**byte-for-byte identical** to that decoder (verified — see "Wire-format
verification" below). **Do not change the opcodes or field order** on either side
without changing both.

## Files

| File | Role |
|------|------|
| `alr_gles_proto.h` | Op enum + little-endian encoder. The wire contract; mirrors `alr_gpu_decode.hpp`'s `enum Op` and `Reader`/`Encoder` byte-for-byte. |
| `alr_gpu_ring_c.h` | Pure-C port of `RingProducer` (append / free_bytes / flush_and_wait). `RingHeader` layout matches `alr_gpu_ring.hpp` (48-byte header, then the data ring). |
| `alr_khr_egl.h`, `alr_khr_gles2.h` | Minimal vendored EGL/GLES2 type + enum + prototype decls (so the shim and cube build with no NDK/Khronos headers on a glibc target). ABI-compatible with the real headers. |
| `alr_shim_env.h` | The host↔guest **env contract** (`ALR_GPU_RING_FD` / `ALR_GPU_RING_BYTES` / `ALR_GPU_RING_DOORBELL_FD`). |
| `alr_shim_internal.h` | Shared runtime state (ring, virtual-ID allocators, uniform-name table, errors) + the emit/flush API. |
| `alr_shim_runtime.c` | The shared runtime: ring open/mmap, producer attach, `alr_shim_emit`, flush, uniform-name interning. Compiled into `libGLESv2.so.2`. |
| `alr_egl_shim.c` | EGL entry points (libEGL.so.1). Mostly local sentinels; `eglSwapBuffers` is the one per-frame sync. |
| `alr_gles_shim.c` | GLES2 entry points (libGLESv2.so.2). Encodes each call as one wire op; **client-side virtual IDs**; uniform-by-name. |
| `alr-gles-cube.c` | An ordinary GLES2 spinning textured cube (not ALR-aware). Links `-lEGL -lGLESv2`. |
| `build-shim.sh` | `zig cc` cross-build to `aarch64-linux-gnu` (glibc). Produces the two `.so`s + the cube. |
| `decode_check.cpp`, `wc_emit.cpp`, `stubinc/` | Host-only wire-format cross-check harness (not shipped to device). |

## Build

```sh
OUT=./out ./build-shim.sh      # needs zig (tested with 0.16.0)
```

Produces in `out/`:

* `libGLESv2.so.2`  — SONAME `libGLESv2.so.2`, contains the GLES2 shim **and** the
  shared runtime. `NEEDED libc.so.6, libpthread.so.0`.
* `libEGL.so.1`     — SONAME `libEGL.so.1`, **`NEEDED libGLESv2.so.2`** (it imports
  the `alr_shim*` runtime symbols from there — see "Single runtime copy" below).
* `alr-gles-cube`   — aarch64 glibc dynamic ELF (interp `/lib/ld-linux-aarch64.so.1`).
* dev symlinks `libEGL.so`, `libGLESv2.so` (link-time only).

## Installing into the guest rootfs

The guest's dynamic linker must find the shims by their SONAME ahead of any real
Mali driver. Put the real files in a private dir and add the SONAME symlinks:

```
/usr/lib/androlinux/libGLESv2.so.2          <- the built libGLESv2.so.2
/usr/lib/androlinux/libEGL.so.1             <- the built libEGL.so.1
/usr/lib/androlinux/libGLESv2.so -> libGLESv2.so.2     (dev/link convenience)
/usr/lib/androlinux/libEGL.so    -> libEGL.so.1
```

and ensure that dir is first on the guest's library search path, e.g. the loader
sets in the guest env:

```
LD_LIBRARY_PATH=/usr/lib/androlinux:<existing rootfs lib dirs>
```

Alternatively drop them straight in `/usr/lib` with the standard
`libEGL.so.1`/`libGLESv2.so.2` names; either way the **SONAME** is what `NEEDED`
records resolve against, so the symlinks `libEGL.so.1 → libEGL.so` etc. only
matter for `-lEGL` link-time resolution, not runtime.

> **Single runtime copy.** The shared runtime (ring + virtual-ID state) lives only
> in `libGLESv2.so.2`. `libEGL.so.1` lists `libGLESv2.so.2` as `NEEDED` and imports
> the `alr_shim*` symbols from it, so there is exactly **one** `g_state` / one ring
> / one virtual-ID space across both libraries. (If you ever rebuild the runtime
> into both `.so`s instead, you must give the runtime symbols default visibility
> and rely on the dynamic linker collapsing them — the current build avoids that by
> putting the runtime in one place.)

## Env contract the loader must set (per `alr_shim_env.h`)

Before `fork()+exec` of the guest, the host creates the ring and passes its fd to
the child by **fd inheritance** (the guest is the `fork()`ed child, so it inherits
open fds). The loader sets, in the guest env (alongside `ALR_ROOTFS` /
`LD_LIBRARY_PATH` / `LD_PRELOAD`):

| Var | Req | Meaning |
|-----|-----|---------|
| `ALR_GPU_RING_FD` | yes | Decimal fd (inherited) of the shared region the host already `ring_init()`'d. The shim `mmap`s it `MAP_SHARED`. **Must not be `FD_CLOEXEC`** (clear it before exec). |
| `ALR_GPU_RING_BYTES` | yes | Decimal **data-region** size (power of two) = the `ring_bytes` passed to `ring_init`. The shim maps `48 + ALR_GPU_RING_BYTES`. |
| `ALR_GPU_RING_DOORBELL_FD` | no | Decimal fd of an inherited `eventfd`. The shim writes an 8-byte `1` on flush so the host wakes immediately instead of spin-polling `head`. If unset, both sides fall back to spin/poll (correct, just busier). Recommended. |

If `ALR_GPU_RING_FD` is absent or the region isn't a valid `ALRG` ring, the shim
runs **ring-less**: every emit is a quiet no-op (so a `--version`/smoke run off
device doesn't crash). No GPU output in that mode.

## Remaining HOST wiring (new code, host side — the only thing left for M4)

The decoder and ring already exist. What's **new** is the host GPU thread that
drives them. Pseudocode (C++, in `runtime_report.cpp` or a sibling TU; uses the
committed `alr::gpu` API):

```cpp
#include "alr_gpu/alr_gpu_ring.hpp"     // ring_init, RingConsumer
#include "alr_gpu/alr_gpu_decode.hpp"   // decode_batch, HostState

// --- before fork ---
uint32_t ring_bytes = 1u << 20;                       // 1 MiB data, power of two
size_t   region_sz  = alr::gpu::ring_region_size(ring_bytes);
int ring_fd = memfd_create("alr_gpu_ring", 0);        // or ashmem
ftruncate(ring_fd, region_sz);
void* region = mmap(nullptr, region_sz, PROT_READ|PROT_WRITE, MAP_SHARED, ring_fd, 0);
alr::gpu::ring_init(region, ring_bytes);              // host initializes the ring
int bell_fd = eventfd(0, EFD_CLOEXEC);                // optional doorbell
fcntl(ring_fd, F_SETFD, fcntl(ring_fd,F_GETFD) & ~FD_CLOEXEC);   // survive exec
fcntl(bell_fd, F_SETFD, fcntl(bell_fd,F_GETFD) & ~FD_CLOEXEC);
// set ALR_GPU_RING_FD=<ring_fd>, ALR_GPU_RING_BYTES=<ring_bytes>,
//     ALR_GPU_RING_DOORBELL_FD=<bell_fd> in the child env, then fork()+exec the
//     guest (alr-gles-cube) under the native loader.

// --- host GPU thread (owns the real Mali EGL/GLES2 context) ---
// 1. Bring up a REAL GLES2 context on an AHB-backed FBO (the present target):
//      eglCreateContext + an AHardwareBuffer wrapped as an EGLImage, bound to a
//      texture, attached to an FBO as COLOR_ATTACHMENT0 (+ a depth renderbuffer).
//      glViewport/scissor to the AHB size. THIS is the draw target decode_batch
//      renders into. (M1's probe already proves decode_batch on a pbuffer; M4 just
//      swaps the pbuffer for the AHB-FBO.)
alr::gpu::RingConsumer rc(region);
alr::gpu::HostState st;                                // virtual->real maps persist across batches
std::vector<uint8_t> snap(ring_bytes);
for (;;) {
    // block on the doorbell (read 8 bytes) or poll rc.available()
    uint64_t tok; read(bell_fd, &tok, sizeof(tok));    // wakes on guest flush
    for (;;) {
        uint32_t n = rc.snapshot(snap.data(), snap.size());   // linearizes a wrap
        if (n == 0) break;
        alr::gpu::decode_batch(snap.data(), n, st);    // REPLAY on the AHB-FBO context
        rc.advance(n);
    }
    // The guest's eglSwapBuffers bumped req_seq (no "swap" byte is sent). On that
    // frame boundary: finish + present + reply.
    if (st_frame_boundary_reached /* req_seq advanced since last reply */) {
        glFinish();
        wayland_presenter.present_external_oes(ahb);   // hand the AHB to the
                                                        // WaylandPresenter external-OES present path
        rc.post_reply();                                // unblocks the guest's swap
        write(bell_fd_back_to_guest, &one, 8);          // if a return doorbell is used
    }
    if (rc.producer_closed()) break;
}
```

Notes:
* `decode_batch` needs a **current EGL context** — bind the AHB-FBO context on this
  thread once at startup. It is transport-agnostic (M1 ran it on a pbuffer).
* `HostState st` must **persist across batches** (it holds the virtual→real GL
  name maps the guest relies on never being reset mid-session).
* The **frame boundary is `req_seq`**, not a stream byte. The guest's
  `eglSwapBuffers`/`glFinish` call `RingProducer::flush_and_wait`, which does
  `req_seq.fetch_add(1)` then blocks on `reply_seq`. The host detects the frame by
  `req_seq` advancing and answers with `RingConsumer::post_reply()` (sets
  `reply_seq = req_seq`). There is intentionally **no SWAP opcode**.
* On the per-frame sync, hand the AHB to `WaylandPresenter`'s external-OES present
  path (the same zero-copy AHB present proven in v114, MEMORY:
  android-visible-probe-wiring / device-evidence). That closes the loop:
  guest GL → ring → host Mali replay into AHB → Wayland present.

## Wire-format verification (done, off-device)

`build-shim.sh` only proves it **compiles**. To prove the bytes match the
committed decoder, `build-wire-check.sh` drives the real shim and decodes its
output with the **actual** `alr_gpu_decode.hpp` — one command, host-native (macOS
or Linux `cc`/`c++`), no device / NDK / GL driver:

```sh
OUT=/tmp/wc_build ./build-wire-check.sh
```

It runs two stages:
* **stage 1 — `wc_emit`**: links the real `alr_gles_shim.c` against a linear-buffer
  stub runtime (`wc_emit.c`), drives the cube GL sequence, and dumps the wire bytes.
* **stage 2 — `wc_decode`**: decodes those bytes with the committed
  `alr_gpu_decode.hpp` plus recording GL stubs (`decode_check.cpp` + `stubinc/`),
  then asserts the decoded calls match the cube sequence.

Result (`ALR GPU WIRE-FORMAT CHECK: PASS`, 14 assertions): 34 ops decode cleanly
(`decode_batch` returns ok), the virtual IDs {shaders 1,2; program 1; buffer 1;
texture 1} all translate through `HostState`, `glBindAttribLocation` carries
aPos→0 / aUV→1, the `uMVP` uniform's 16 floats arrive intact via **name resolution**
(not a location round-trip), `uTex` resolves to sampler unit 0, the texture upload
is the expected 8×8×4 = 256 bytes (with host `UNPACK_ALIGNMENT=1`), the two VBO
attrib offsets {0, 12} are correct, and there is exactly one `glDrawArrays(…, 36)`.

> Note: `decode_check.cpp`, `wc_emit.c`, `stubinc/`, and `alr-gles-cube.c` were
> restored by WS-2 (they were described here but never committed with the M3 shim
> in 1296b29). The exact op COUNT depends on the driver sequence in `wc_emit.c`
> (34 here); the invariants above are what matter.

## Key wire / ABI decisions

* **Client-side virtual IDs (the crux).** `glGenBuffers/glGenTextures/
  glCreateShader/glCreateProgram` allocate a monotonic per-type id (from 1; 0
  reserved), return it **immediately with no round-trip**, and emit a create-op
  carrying that virtual id. The host's `HostState` maps virtual→real.
* **Uniforms carry the NAME, not a location.** `glGetUniformLocation` returns a
  client handle = an index into a per-program name table, packed as
  `(vprog << 16) | index`. The setters (`glUniform1i` / `glUniformMatrix4fv`)
  recover the program + name from that handle and emit the op with the **name as a
  blob**; the host calls the real `glGetUniformLocation` by name at decode time.
  A handle of `-1` is a silent no-op (matches GL). Because `vprog ≥ 1`, a valid
  packed handle is never `0`, and the wire carries the name (never the handle), so
  the host is oblivious to the packing.
* **No SWAP opcode.** The per-frame sync is `req_seq`/`reply_seq` on the ring
  (`flush_and_wait` ↔ `post_reply`), as the committed ring expects.
* **`glVertexAttribPointer` is VBO-offset only.** The `pointer` arg is encoded as a
  `u32` byte offset into the bound `ARRAY_BUFFER` (the decoder reinterpret_casts it
  back). Client vertex arrays are out of scope; the cube binds a VBO.
* **Optimistic queries.** `glGetError → GL_NO_ERROR` (unless the shim set a sticky
  error, e.g. an oversized upload → `GL_OUT_OF_MEMORY`); `glGetShaderiv(COMPILE_
  STATUS)` / `glGetProgramiv(LINK_STATUS) → GL_TRUE`. Real failures surface as
  wrong/blank pixels and are logged host-side.
* **`glTexImage2D` payload size** is computed as `w*h*components*type_size` tight
  (the host forces `UNPACK_ALIGNMENT=1`). `glPixelStorei` is accepted and dropped.
* **Little-endian only.** The target `aarch64-linux-gnu` is LE, matching the host
  `Reader`. A big-endian guest would need byte-swapping in the encoder.
* **Deletes are advisory.** No delete opcodes exist host-side; `glDelete*` are
  no-ops (the cube deletes once at exit). `glDeleteProgram` resets the local
  uniform-name table so stale handles can't be reused.
