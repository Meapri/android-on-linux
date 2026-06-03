/* alr_icd_runtime.h — the ALR Vulkan ICD's ring transport runtime (guest side).
 *
 * Header-only, pure C11 (glibc-guest-buildable with zig cc, NEEDED-clean). This is
 * the Vulkan twin of alr_gpu/guest_shim/alr_shim_runtime.c's ring glue, with the
 * ONE structural addition the Vulkan ENUM rung needs: a REPLY ring the ICD CONSUMES
 * (the GLES shim has no reply stream — see alr_icd_env.h). It provides exactly one
 * primitive to the ICD entry points:
 *
 *     alr_icd_roundtrip(req_bytes, req_len, reply_out, reply_cap) -> reply_len
 *
 * which appends a request op batch to the request ring, signals the host (req_seq
 * bump + optional doorbell), blocks until the host posts the reply, drains the reply
 * ring into `reply_out`, and returns the reply length. The ICD encodes the request
 * with AlrVkEncoder (alr_gpu_vk_proto.hpp) and decodes the reply with the same wire
 * the host self-test uses — so the bytes are identical to the DEVICE-PROVEN
 * run_vk_marshal_mali_probe path; only the producer is now a real guest ICD.
 *
 * Lazy init (pthread_once): on first use, read ALR_VK_RING_FD/BYTES (request) and
 * ALR_VK_REPLY_FD/BYTES (reply) from the env, mmap both MAP_SHARED, attach. If the
 * env is absent or a region is not a valid ring, ring_ok stays 0 and the ICD reports
 * ZERO devices (the conformant ring-less answer).
 *
 * Reuses alr_gpu_ring_c.h's AlrRingHeader + AlrRingProducer (the request side) and
 * adds a tiny C consumer over the SAME header layout for the reply side (the C
 * header shipped only the producer; the host C++ owns RingConsumer, so the guest
 * needs its own consumer here).
 */
#ifndef ALR_ICD_RUNTIME_H
#define ALR_ICD_RUNTIME_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "alr_gpu_ring_c.h"  /* AlrRingHeader, AlrRingProducer, alr_ring_* */
#include "alr_icd_env.h"

/* ---- Reply-ring CONSUMER over the same AlrRingHeader layout (the C header only
 * ported the producer; the host C++ uses RingConsumer for the request side, so the
 * guest needs its own consumer for the reply side). Mirrors RingConsumer's logic:
 * available()/snapshot()/advance(), acquire on head, release on tail. ---- */
typedef struct AlrRingConsumer {
    AlrRingHeader *h;
    uint8_t       *data;   /* region + sizeof(header) */
    uint32_t       mask;   /* ring_bytes - 1 */
} AlrRingConsumer;

static inline int alr_ring_consumer_attach(AlrRingConsumer *c, void *region) {
    if (!alr_ring_valid(region)) { c->h = NULL; c->data = NULL; c->mask = 0; return -1; }
    c->h = (AlrRingHeader *)region;
    c->data = (uint8_t *)region + sizeof(AlrRingHeader);
    c->mask = c->h->ring_bytes - 1u;
    return 0;
}
static inline uint64_t alr_ring_consumer_available(const AlrRingConsumer *c) {
    uint64_t head = atomic_load_explicit(&c->h->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&c->h->tail, memory_order_relaxed);
    return head - tail;
}
/* Copy up to out_cap available bytes into out (linearizing a wrap), WITHOUT advancing
 * tail. Returns the count copied. */
static inline uint32_t alr_ring_consumer_snapshot(const AlrRingConsumer *c,
                                                  uint8_t *out, uint32_t out_cap) {
    uint64_t avail = alr_ring_consumer_available(c);
    uint32_t n = (uint32_t)(avail < out_cap ? avail : out_cap);
    uint64_t tail;
    uint32_t off, first;
    if (n == 0) return 0;
    tail = atomic_load_explicit(&c->h->tail, memory_order_relaxed);
    off = (uint32_t)(tail & c->mask);
    first = (off + n <= c->h->ring_bytes) ? n : (c->h->ring_bytes - off);
    memcpy(out, c->data + off, first);
    if (first < n) memcpy(out + first, c->data, n - first);
    return n;
}
static inline void alr_ring_consumer_advance(AlrRingConsumer *c, uint32_t n) {
    uint64_t tail = atomic_load_explicit(&c->h->tail, memory_order_relaxed);
    atomic_store_explicit(&c->h->tail, tail + n, memory_order_release);
}

/* ---- ICD ring state (process-global; lazily attached). ---- */
typedef struct AlrIcdState {
    int             ring_ok;       /* 1 if BOTH rings attached */
    AlrRingProducer req;           /* request producer (guest -> host) */
    AlrRingConsumer rep;           /* reply consumer (host -> guest) */
    void           *req_region;    /* mmap base of the request ring (for munmap) */
    size_t          req_region_sz;
    void           *rep_region;    /* mmap base of the reply ring */
    size_t          rep_region_sz;
    int             doorbell_fd;   /* eventfd to signal the host on submit, or -1 */
} AlrIcdState;

static AlrIcdState     g_icd_state;
static pthread_once_t  g_icd_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_icd_lock = PTHREAD_MUTEX_INITIALIZER;

static long alr_icd_env_long(const char *key, long dflt) {
    const char *v = getenv(key);
    char *end = NULL;
    long n;
    if (!v || !*v) return dflt;
    n = strtol(v, &end, 10);
    return (end == v) ? dflt : n;
}

static void *alr_icd_map_ring(long fd, long bytes, size_t *out_sz) {
    size_t map_sz;
    void *region;
    if (fd < 0 || bytes <= 0) return NULL;
    map_sz = alr_ring_region_size((uint32_t)bytes);
    region = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (region == MAP_FAILED) return NULL;
    if (!alr_ring_valid(region)) { munmap(region, map_sz); return NULL; }
    *out_sz = map_sz;
    return region;
}

static void alr_icd_init_once(void) {
    long req_fd, req_bytes, rep_fd, rep_bytes, bell_fd;
    void *req_region, *rep_region;
    size_t req_sz = 0, rep_sz = 0;

    memset(&g_icd_state, 0, sizeof(g_icd_state));
    g_icd_state.ring_ok = 0;
    g_icd_state.doorbell_fd = -1;

    req_fd    = alr_icd_env_long(ALR_VK_ENV_RING_FD, -1);
    req_bytes = alr_icd_env_long(ALR_VK_ENV_RING_BYTES, 0);
    rep_fd    = alr_icd_env_long(ALR_VK_ENV_REPLY_FD, -1);
    rep_bytes = alr_icd_env_long(ALR_VK_ENV_REPLY_BYTES, 0);
    bell_fd   = alr_icd_env_long(ALR_VK_ENV_RING_DOORBELL_FD, -1);

    req_region = alr_icd_map_ring(req_fd, req_bytes, &req_sz);
    rep_region = alr_icd_map_ring(rep_fd, rep_bytes, &rep_sz);

    if (req_region && rep_region &&
        alr_ring_producer_attach(&g_icd_state.req, req_region) == 0 &&
        alr_ring_consumer_attach(&g_icd_state.rep, rep_region) == 0) {
        g_icd_state.req_region = req_region;
        g_icd_state.req_region_sz = req_sz;
        g_icd_state.rep_region = rep_region;
        g_icd_state.rep_region_sz = rep_sz;
        g_icd_state.ring_ok = 1;
        if (bell_fd >= 0) g_icd_state.doorbell_fd = (int)bell_fd;
    } else {
        if (req_region) munmap(req_region, req_sz);
        if (rep_region) munmap(rep_region, rep_sz);
        fprintf(stderr,
                "[alr-icd] no/invalid %s + %s in env — Vulkan ICD reports 0 devices "
                "(ring-less)\n",
                ALR_VK_ENV_RING_FD, ALR_VK_ENV_REPLY_FD);
    }
}

static AlrIcdState *alr_icd(void) {
    pthread_once(&g_icd_once, alr_icd_init_once);
    return &g_icd_state;
}

static void alr_icd_doorbell(AlrIcdState *s) {
    if (s->doorbell_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(s->doorbell_fd, &one, sizeof(one));
        (void)w; /* best-effort wake; host also polls head/req_seq */
    }
}

/* Append a request op batch, signal the host, block for the reply, drain it into
 * reply_out. Returns the reply byte length (0 if ring-less or on failure). Thread-safe
 * (a mutex serializes the whole round trip so two threads can't interleave request /
 * reply on the single SPSC ring pair — a real multi-threaded Vulkan app may submit
 * from several threads; the ENUM rung is light, so one global lock is correct here). */
static uint32_t alr_icd_roundtrip(const uint8_t *req_bytes, uint32_t req_len,
                                  uint8_t *reply_out, uint32_t reply_cap) {
    AlrIcdState *s = alr_icd();
    uint32_t want, observed, got = 0;
    unsigned spin;
    if (!s->ring_ok || req_len == 0) return 0;

    pthread_mutex_lock(&g_icd_lock);

    /* 1) Push the request. If it doesn't fit, signal + wait once for the host to
     *    drain, then retry. (The ENUM batch is tiny — tens of bytes — so this is
     *    effectively always a single append.) */
    if (!alr_ring_append(&s->req, req_bytes, req_len)) {
        alr_icd_doorbell(s);
        (void)alr_ring_flush_and_wait(&s->req, 0);
        if (!alr_ring_append(&s->req, req_bytes, req_len)) {
            pthread_mutex_unlock(&g_icd_lock);
            return 0;  /* request larger than the whole ring — caller treats as 0 devices */
        }
    }

    /* 2) Signal the host that a request batch is pending (bump req_seq on the REQUEST
     *    ring + ring the doorbell) and remember the seqno we expect the host to ack. */
    alr_icd_doorbell(s);
    want = atomic_fetch_add_explicit(&s->req.h->req_seq, 1u, memory_order_acq_rel) + 1u;

    /* 3) Block until the host has serviced this request. The host posts completion by
     *    bumping the REQUEST ring's reply_seq to >= want (after it has appended the
     *    reply op stream to the reply ring), exactly like the GLES per-frame handshake.
     *    Spin fallback (correct, just busier); the doorbell makes the host prompt. */
    spin = (1u << 24);
    for (unsigned i = 0; i < spin; ++i) {
        observed = atomic_load_explicit(&s->req.h->reply_seq, memory_order_acquire);
        if (observed >= want) break;
        if (atomic_load_explicit(&s->req.h->closed, memory_order_acquire)) break;
    }

    /* 4) Drain the reply ring into reply_out (the host wrote the AlrVkReply stream
     *    there before bumping reply_seq, so it is fully visible now). */
    got = alr_ring_consumer_snapshot(&s->rep, reply_out, reply_cap);
    if (got) alr_ring_consumer_advance(&s->rep, got);

    pthread_mutex_unlock(&g_icd_lock);
    return got;
}

/* True if the ICD has a live host ring (so vkEnumeratePhysicalDevices can marshal).
 * When false the ICD reports 0 devices — the conformant ring-less answer. */
static int alr_icd_ring_ok(void) { return alr_icd()->ring_ok; }

#endif /* ALR_ICD_RUNTIME_H */
