/* alr_shim_internal.h — shared runtime state + helpers for the two ALR shims.
 *
 * libEGL.so.1 and libGLESv2.so.2 share ONE process-global runtime:
 *   - the inherited GPU command ring (producer side),
 *   - a scratch buffer for encoding a single op before append,
 *   - the client-side virtual-ID allocators (THE CRUX, DESIGN §2.3),
 *   - the per-program uniform-NAME table backing glGetUniformLocation,
 *   - the shim-tracked GL/EGL error.
 *
 * The runtime is defined ONCE in alr_shim_runtime.c, which is compiled into BOTH
 * .so's; on first use (pthread_once) it attaches to the inherited ring described
 * by the env contract (see alr_shim_env.h). Because libEGL.so.1 lists
 * libGLESv2.so.2 as NEEDED (see build-shim.sh), there is a single copy of these
 * symbols in the process and both shims share the same ring + virtual-ID space.
 *
 * Pure C; not part of the exported ABI (hidden visibility recommended via the
 * build script's -fvisibility=hidden + explicit exports of the gl/egl entry names).
 */
#ifndef ALR_SHIM_INTERNAL_H
#define ALR_SHIM_INTERNAL_H

#include <stdint.h>
#include "alr_gpu_ring_c.h"
#include "alr_gles_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest single encoded op the cube emits before an append is the one-time
 * texture upload (OP_TEX_IMAGE_2D with a 64x64 RGBA = 16 KiB payload) or the VBO
 * (OP_BUFFER_DATA, ~1.2 KiB). Scratch is sized well above that; uploads larger
 * than scratch are rejected (set error) — the cube never approaches this. */
#define ALR_SHIM_SCRATCH_BYTES (256 * 1024)

/* Per-program uniform-name table: glGetUniformLocation returns a small virtual
 * handle = an index into this name list; glUniform* looks the name back up and
 * encodes it as a blob (the host resolves the real location by name). The cube
 * uses ~2 uniforms per program, so these caps are generous. */
#define ALR_SHIM_MAX_PROGRAMS        64
#define ALR_SHIM_MAX_UNIFORMS_PER_PROG 32
#define ALR_SHIM_MAX_UNIFORM_NAME    64

typedef struct AlrUniformName {
    char name[ALR_SHIM_MAX_UNIFORM_NAME];
} AlrUniformName;

typedef struct AlrProgramUniforms {
    uint32_t       vprog;                                  /* virtual program id (0 = free slot) */
    int            count;                                  /* names registered */
    AlrUniformName names[ALR_SHIM_MAX_UNIFORMS_PER_PROG];
} AlrProgramUniforms;

/* The process-global shim runtime (single instance; see alr_shim_runtime.c). */
typedef struct AlrShimState {
    int             ring_ok;        /* 1 if the ring attached successfully */
    AlrRingProducer ring;           /* producer over the inherited shared region */
    void           *ring_region;    /* mmap base (kept for munmap on teardown) */
    size_t          ring_region_sz; /* bytes mapped */
    int             doorbell_fd;    /* eventfd to signal the host on flush, or -1 */

    /* virtual-ID allocators — monotonic per type, START AT 1 (0 reserved). */
    uint32_t        next_shader;
    uint32_t        next_program;
    uint32_t        next_buffer;
    uint32_t        next_texture;
    uint32_t        next_framebuffer;
    uint32_t        next_renderbuffer;

    /* per-program uniform-name tables (the glGetUniformLocation backing store) */
    AlrProgramUniforms progs[ALR_SHIM_MAX_PROGRAMS];

    /* per-program attribute-name tables (the glGetAttribLocation backing store —
     * same client-handle-by-name scheme as uniforms; reuses AlrProgramUniforms as a
     * generic per-program name table). */
    AlrProgramUniforms attribs[ALR_SHIM_MAX_PROGRAMS];

    /* shim-tracked sticky errors (the cube reads these via glGetError/eglGetError). */
    uint32_t        gl_error;       /* GL_NO_ERROR by default */
    int32_t         egl_error;      /* EGL_SUCCESS by default */

    /* scratch for encoding one op, then ring append. */
    uint8_t         scratch[ALR_SHIM_SCRATCH_BYTES];
} AlrShimState;

/* Lazily-initialized accessor (pthread_once inside). Never NULL. If the ring
 * could not be attached, the returned state has ring_ok==0 and emit_* become
 * no-ops (so a shim linked without a host ring degrades quietly rather than
 * crashing — useful for a smoke `--version` run). */
AlrShimState *alr_shim(void);

/* Encode one op into the shared scratch via `build`, then append it to the ring.
 * `build` receives an AlrEncoder already init'd over the scratch buffer. If the
 * ring is full, this flushes+waits once and retries (back-pressure); if it still
 * doesn't fit (op larger than the whole ring), it sets GL_OUT_OF_MEMORY-ish error
 * via gl_error and drops the op. Thread-compatible with the single-producer model
 * (the cube is single-threaded; a mutex guards scratch reuse if ever multithreaded). */
void alr_shim_emit(void (*build)(AlrEncoder *e, void *ctx), void *ctx);

/* Flush + block until the host posts the reply for the latest request (the
 * eglSwapBuffers / glFinish sync point). Writes the doorbell eventfd first if
 * present so the host wakes immediately, then spins on reply_seq. */
void alr_shim_flush_and_wait(void);

/* Flush without blocking (glFlush): write the doorbell so the host drains soon. */
void alr_shim_flush(void);

/* Uniform-name table ops (used by glGetUniformLocation / glLinkProgram). */
int  alr_shim_uniform_intern(uint32_t vprog, const char *name); /* -> handle (index), or -1 */
const char *alr_shim_uniform_name(uint32_t vprog, int handle);  /* handle -> name, or NULL */
void alr_shim_program_reset(uint32_t vprog);                    /* clear a program's uniform+attrib names */

/* Attribute-name table ops (used by glGetAttribLocation). Same scheme as uniforms. */
int  alr_shim_attrib_intern(uint32_t vprog, const char *name);  /* -> handle (index), or -1 */
const char *alr_shim_attrib_name(uint32_t vprog, int handle);   /* handle -> name, or NULL */

#ifdef __cplusplus
}
#endif

#endif /* ALR_SHIM_INTERNAL_H */
