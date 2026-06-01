/* alr_shim_runtime.c — the shared runtime for the ALR EGL/GLES shims.
 *
 * Compiled into BOTH libEGL.so.1 and libGLESv2.so.2. Because libEGL lists
 * libGLESv2 as NEEDED and these symbols have default visibility, the dynamic
 * linker collapses them to ONE definition at load — so both shims share a single
 * ring + virtual-ID space (verified by the README's integration notes).
 *
 * Lazy init (pthread_once): on first alr_shim() call, read ALR_GPU_RING_FD /
 * ALR_GPU_RING_BYTES (see alr_shim_env.h), mmap MAP_SHARED, attach the producer.
 * If the env is absent or the region isn't a valid ring, ring_ok stays 0 and all
 * emits become no-ops (a shim with no host degrades quietly — useful for a smoke
 * `--version` invocation off-device).
 *
 * Pure C / glibc. Uses pthread_once + a mutex (the cube is single-threaded, but
 * the mutex makes scratch reuse safe if a guest ever issues GL from two threads).
 */
#include "alr_shim_internal.h"
#include "alr_shim_env.h"
#include "alr_khr_gles2.h"  /* for GL_NO_ERROR */
#include "alr_khr_egl.h"    /* for EGL_SUCCESS */

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

static AlrShimState   g_state;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static long env_long(const char *key, long dflt) {
    const char *v = getenv(key);
    if (!v || !*v) return dflt;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return dflt;
    return n;
}

static void shim_init_once(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.next_shader  = 1;
    g_state.next_program = 1;
    g_state.next_buffer  = 1;
    g_state.next_texture = 1;
    g_state.gl_error     = GL_NO_ERROR;
    g_state.egl_error    = EGL_SUCCESS;
    g_state.doorbell_fd  = -1;
    g_state.ring_ok      = 0;

    long ring_fd    = env_long(ALR_ENV_RING_FD, -1);
    long ring_bytes = env_long(ALR_ENV_RING_BYTES, 0);
    long bell_fd    = env_long(ALR_ENV_RING_DOORBELL_FD, -1);

    if (ring_fd >= 0 && ring_bytes > 0) {
        size_t map_sz = alr_ring_region_size((uint32_t)ring_bytes);
        void *region = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
                            (int)ring_fd, 0);
        if (region != MAP_FAILED) {
            if (alr_ring_producer_attach(&g_state.ring, region) == 0) {
                g_state.ring_region    = region;
                g_state.ring_region_sz = map_sz;
                g_state.ring_ok        = 1;
            } else {
                munmap(region, map_sz);
                fprintf(stderr, "[alr-shim] ring fd=%ld present but region is not a "
                                "valid ALRG ring (magic/version/size mismatch)\n", ring_fd);
            }
        } else {
            fprintf(stderr, "[alr-shim] mmap(ring fd=%ld, %zu bytes) failed\n",
                    ring_fd, map_sz);
        }
    } else {
        /* No ring in the env: smoke/offline mode. Quietly no-op emits. */
        fprintf(stderr, "[alr-shim] no %s/%s in env — running ring-less (no GPU)\n",
                ALR_ENV_RING_FD, ALR_ENV_RING_BYTES);
    }

    if (bell_fd >= 0) g_state.doorbell_fd = (int)bell_fd;
}

AlrShimState *alr_shim(void) {
    pthread_once(&g_once, shim_init_once);
    return &g_state;
}

static void ring_doorbell(AlrShimState *s) {
    if (s->doorbell_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(s->doorbell_fd, &one, sizeof(one));
        (void)w; /* best-effort wakeup; the host also polls */
    }
}

void alr_shim_emit(void (*build)(AlrEncoder *e, void *ctx), void *ctx) {
    AlrShimState *s = alr_shim();
    if (!s->ring_ok) return;  /* ring-less: drop */

    pthread_mutex_lock(&g_lock);
    AlrEncoder e;
    alr_enc_init(&e, s->scratch, sizeof(s->scratch));
    build(&e, ctx);
    if (e.overflow) {
        /* Op + payload exceeds scratch — only possible for an oversized upload.
         * Flag it (GL_OUT_OF_MEMORY = 0x0505) and drop; the cube never hits this. */
        s->gl_error = 0x0505;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* Try to append; on a full ring, flush+wait once for the host to drain, retry. */
    if (!alr_ring_append(&s->ring, e.buf, (uint32_t)e.len)) {
        ring_doorbell(s);
        (void)alr_ring_flush_and_wait(&s->ring, 0);
        if (!alr_ring_append(&s->ring, e.buf, (uint32_t)e.len)) {
            /* Still doesn't fit: the op is larger than the whole ring. Flag it. */
            s->gl_error = 0x0505;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void alr_shim_flush_and_wait(void) {
    AlrShimState *s = alr_shim();
    if (!s->ring_ok) return;
    ring_doorbell(s);
    (void)alr_ring_flush_and_wait(&s->ring, 0);
}

void alr_shim_flush(void) {
    AlrShimState *s = alr_shim();
    if (!s->ring_ok) return;
    ring_doorbell(s);
    /* no block */
}

/* ---- uniform-name table backing glGetUniformLocation ---- */

static AlrProgramUniforms *find_prog(AlrShimState *s, uint32_t vprog, int create) {
    int free_slot = -1;
    for (int i = 0; i < ALR_SHIM_MAX_PROGRAMS; ++i) {
        if (s->progs[i].vprog == vprog && vprog != 0) return &s->progs[i];
        if (s->progs[i].vprog == 0 && free_slot < 0) free_slot = i;
    }
    if (create && free_slot >= 0) {
        s->progs[free_slot].vprog = vprog;
        s->progs[free_slot].count = 0;
        return &s->progs[free_slot];
    }
    return NULL;
}

int alr_shim_uniform_intern(uint32_t vprog, const char *name) {
    AlrShimState *s = alr_shim();
    if (!name) return -1;
    pthread_mutex_lock(&g_lock);
    AlrProgramUniforms *p = find_prog(s, vprog, 1);
    if (!p) { pthread_mutex_unlock(&g_lock); return -1; }
    /* dedupe: same name -> same handle */
    for (int i = 0; i < p->count; ++i) {
        if (strncmp(p->names[i].name, name, ALR_SHIM_MAX_UNIFORM_NAME) == 0) {
            pthread_mutex_unlock(&g_lock);
            return i;
        }
    }
    if (p->count >= ALR_SHIM_MAX_UNIFORMS_PER_PROG) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int idx = p->count++;
    strncpy(p->names[idx].name, name, ALR_SHIM_MAX_UNIFORM_NAME - 1);
    p->names[idx].name[ALR_SHIM_MAX_UNIFORM_NAME - 1] = '\0';
    pthread_mutex_unlock(&g_lock);
    return idx;
}

const char *alr_shim_uniform_name(uint32_t vprog, int handle) {
    AlrShimState *s = alr_shim();
    if (handle < 0) return NULL;
    pthread_mutex_lock(&g_lock);
    AlrProgramUniforms *p = find_prog(s, vprog, 0);
    const char *out = NULL;
    if (p && handle < p->count) out = p->names[handle].name;
    pthread_mutex_unlock(&g_lock);
    return out;
}

void alr_shim_program_reset(uint32_t vprog) {
    AlrShimState *s = alr_shim();
    pthread_mutex_lock(&g_lock);
    AlrProgramUniforms *p = find_prog(s, vprog, 0);
    if (p) p->count = 0;
    pthread_mutex_unlock(&g_lock);
}
