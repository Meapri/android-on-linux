/* alr_gpu_ring_c.h — a PURE C port of the producer half of the SPSC ring.
 *
 * >>> MUST MATCH alr_gpu_ring.hpp's RingHeader LAYOUT BYTE-FOR-BYTE <<<
 * The host (C++) creates the ring with alr::gpu::RingHeader / ring_init() and
 * consumes with RingConsumer. The guest shim (this file) is the PRODUCER. The
 * struct below is a C11 translation of RingHeader; the offsets are identical:
 *
 *   off  type                     field        (C++ std::atomic<T> == C _Atomic T)
 *   ---  -----------------------  -----------
 *   0    uint32_t                 magic
 *   4    uint32_t                 version
 *   8    uint32_t                 ring_bytes   (power of two)
 *   12   uint32_t                 pad
 *   16   _Atomic uint64_t         head         (producer write cursor — WE advance)
 *   24   _Atomic uint64_t         tail         (consumer read cursor — host advances)
 *   32   _Atomic uint32_t         reply_seq    (host bumps on SYNC/swap reply)
 *   36   _Atomic uint32_t         req_seq      (WE bump when we flush a sync request)
 *   40   _Atomic uint32_t         closed       (1 = producer gone / teardown)
 *   44   _Atomic uint32_t         pad2
 *   ---  the first 48 bytes are the original hot-path layout.
 *   48   _Atomic uint32_t         identity_ready (host sets 1 after the strings are filled)
 *   52   uint32_t                 identity_pad
 *   56   char[64]                 renderer       (host glGetString(GL_RENDERER))
 *   120  char[64]                 vendor         (host glGetString(GL_VENDOR))
 *   184  char[64]                 gl_version     (host glGetString(GL_VERSION))
 *   ---  total 248 bytes; the data ring follows immediately after the header.
 *
 * The identity block (off 48) is an APPEND-ONLY extension carrying the host's REAL
 * glGetString values so the guest shim's glGetString(GL_RENDERER/VENDOR/VERSION)
 * reports the actual host GPU (e.g. "Mali-G615") instead of a synthetic placeholder.
 * The host writes it once at startup (ring_set_identity in alr_gpu_ring.hpp) AFTER a
 * GL context is current; the guest only reads it when identity_ready != 0. An old host
 * that never sets it leaves identity_ready == 0 and the guest keeps its fallback.
 *
 * Cursors are MONOTONIC absolute byte counts (never wrapped); the ring index is
 * cursor % ring_bytes. The producer logic below (append/free_bytes/flush_and_wait/
 * close) is a faithful C translation of alr::gpu::RingProducer with the SAME
 * memory orders (release on head publish, acquire on tail/reply_seq reads), so
 * the host RingConsumer sees a correct stream.
 *
 * On aarch64, _Atomic uint64_t / uint32_t are lock-free and have the same size,
 * alignment and representation as the C++ std::atomic counterparts in the host
 * header — the shared mapping interops. (A static_assert on sizeof guards this.)
 *
 * Pure C11 (needs <stdatomic.h>); glibc-guest-buildable with zig cc / gcc.
 */
#ifndef ALR_GPU_RING_C_H
#define ALR_GPU_RING_C_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#error "alr_gpu_ring_c.h is the C producer port; include alr_gpu_ring.hpp from C++."
#endif

#define ALR_RING_MAGIC   0x47524C41u  /* 'ALRG' little-endian — matches RingHeader::kMagic */
#define ALR_RING_VERSION 1u           /* matches RingHeader::kVersion */

/* Per-string identity capacity (incl. NUL). MUST equal alr_gpu_ring.hpp's kRingIdentMax. */
#define ALR_RING_IDENT_MAX 64u

typedef struct AlrRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t ring_bytes;
    uint32_t pad;
    _Atomic uint64_t head;       /* producer write cursor (guest advances) */
    _Atomic uint64_t tail;       /* consumer read cursor (host advances) */
    _Atomic uint32_t reply_seq;  /* host bumps when a SYNC/swap reply is ready */
    _Atomic uint32_t req_seq;    /* producer bumps when it flushes a sync request */
    _Atomic uint32_t closed;     /* 1 = producer gone / teardown */
    _Atomic uint32_t pad2;
    /* ---- identity block (off 48): host->guest glGetString passthrough ---- */
    _Atomic uint32_t identity_ready;        /* host sets 1 AFTER the strings are filled */
    uint32_t         identity_pad;          /* keeps the char arrays 8-byte aligned */
    char             renderer[ALR_RING_IDENT_MAX];   /* host glGetString(GL_RENDERER) */
    char             vendor[ALR_RING_IDENT_MAX];     /* host glGetString(GL_VENDOR) */
    char             gl_version[ALR_RING_IDENT_MAX]; /* host glGetString(GL_VERSION) */
} AlrRingHeader;

/* Compile-time guard: the hot-path fields keep their original offsets and the identity
 * block sits where the C++ RingHeader puts it, or it does not interop. (aarch64-LE.) */
_Static_assert(offsetof(AlrRingHeader, magic)     == 0,  "magic@0");
_Static_assert(offsetof(AlrRingHeader, version)   == 4,  "version@4");
_Static_assert(offsetof(AlrRingHeader, ring_bytes)== 8,  "ring_bytes@8");
_Static_assert(offsetof(AlrRingHeader, head)      == 16, "head@16");
_Static_assert(offsetof(AlrRingHeader, tail)      == 24, "tail@24");
_Static_assert(offsetof(AlrRingHeader, reply_seq) == 32, "reply_seq@32");
_Static_assert(offsetof(AlrRingHeader, req_seq)   == 36, "req_seq@36");
_Static_assert(offsetof(AlrRingHeader, closed)    == 40, "closed@40");
_Static_assert(offsetof(AlrRingHeader, pad2)      == 44, "pad2@44");
_Static_assert(offsetof(AlrRingHeader, identity_ready) == 48, "identity_ready@48");
_Static_assert(offsetof(AlrRingHeader, renderer)   == 56, "renderer@56");
_Static_assert(offsetof(AlrRingHeader, vendor)     == 56 + ALR_RING_IDENT_MAX, "vendor follows renderer");
_Static_assert(offsetof(AlrRingHeader, gl_version) == 56 + 2u * ALR_RING_IDENT_MAX, "gl_version follows vendor");
_Static_assert(sizeof(AlrRingHeader) == 56 + 3u * ALR_RING_IDENT_MAX, "AlrRingHeader must be 248 bytes (matches RingHeader)");

/* The producer view over a mapped shared region (header + data ring). */
typedef struct AlrRingProducer {
    AlrRingHeader *h;
    uint8_t       *data;   /* region + sizeof(header) */
    uint32_t       mask;   /* ring_bytes - 1 */
} AlrRingProducer;

/* Bytes a caller must have mapped for a ring of `ring_bytes` data. */
static inline size_t alr_ring_region_size(uint32_t ring_bytes) {
    return sizeof(AlrRingHeader) + (size_t)ring_bytes;
}

/* Validate a region that the HOST already ring_init()'d (the guest never inits
 * the ring — the parent does before fork; the guest only attaches). */
static inline int alr_ring_valid(const void *region) {
    const AlrRingHeader *h;
    if (!region) return 0;
    h = (const AlrRingHeader *)region;
    return h->magic == ALR_RING_MAGIC && h->version == ALR_RING_VERSION &&
           h->ring_bytes != 0 && (h->ring_bytes & (h->ring_bytes - 1)) == 0;
}

/* Attach the producer to an already-initialized, mapped region. Returns 0 on
 * success, -1 if the region is not a valid ring. */
static inline int alr_ring_producer_attach(AlrRingProducer *p, void *region) {
    if (!alr_ring_valid(region)) { p->h = NULL; p->data = NULL; p->mask = 0; return -1; }
    p->h = (AlrRingHeader *)region;
    p->data = (uint8_t *)region + sizeof(AlrRingHeader);
    p->mask = p->h->ring_bytes - 1;
    return 0;
}

static inline int alr_ring_producer_valid(const AlrRingProducer *p) {
    return p && p->h && alr_ring_valid(p->h);
}

/* Identity field selector for alr_ring_identity(). */
typedef enum AlrRingIdent {
    ALR_IDENT_RENDERER = 0,
    ALR_IDENT_VENDOR   = 1,
    ALR_IDENT_VERSION  = 2,
} AlrRingIdent;

/* Return a pointer to the host-published identity string for `which`, or NULL if the
 * host has not filled the identity block yet (identity_ready == 0) or that particular
 * string is empty. The pointer aliases the shared mapping (read-only for the guest) and
 * is stable for the ring's lifetime (the host writes it once at startup). Acquire-load
 * of identity_ready pairs with the host's release store, so a non-NULL return implies
 * the bytes are fully visible. Used by the guest shim's glGetString passthrough. */
static inline const char *alr_ring_identity(const AlrRingProducer *p, AlrRingIdent which) {
    const char *s;
    if (!alr_ring_producer_valid(p)) return NULL;
    if (atomic_load_explicit(&p->h->identity_ready, memory_order_acquire) == 0u) return NULL;
    switch (which) {
        case ALR_IDENT_RENDERER: s = p->h->renderer;   break;
        case ALR_IDENT_VENDOR:   s = p->h->vendor;     break;
        case ALR_IDENT_VERSION:  s = p->h->gl_version; break;
        default: return NULL;
    }
    return (s[0] != '\0') ? s : NULL;
}

/* Free space available to the producer (ring_bytes - 1 - in-flight). One byte is
 * intentionally never used so head==tail unambiguously means "empty" — identical
 * to RingProducer::free_bytes(). */
static inline uint64_t alr_ring_free_bytes(const AlrRingProducer *p) {
    uint64_t head = atomic_load_explicit(&p->h->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&p->h->tail, memory_order_acquire);
    uint64_t used = head - tail;
    return ((uint64_t)p->h->ring_bytes - 1u) - used;
}

/* Append `len` bytes. Returns 1 on success, 0 if they don't currently fit (the
 * caller should flush + wait for the consumer to drain, then retry). Does NOT
 * block. Matches RingProducer::append: split-copy on wrap, release-publish head. */
static inline int alr_ring_append(AlrRingProducer *p, const void *src, uint32_t len) {
    uint64_t head;
    uint32_t off, first;
    if (!alr_ring_producer_valid(p)) return 0;
    if (len == 0) return 1;
    if (alr_ring_free_bytes(p) < len) return 0;
    head = atomic_load_explicit(&p->h->head, memory_order_relaxed);
    off = (uint32_t)(head & p->mask);
    first = (off + len <= p->h->ring_bytes) ? len : (p->h->ring_bytes - off);
    memcpy(p->data + off, src, first);
    if (first < len) memcpy(p->data, (const uint8_t *)src + first, len - first);
    atomic_store_explicit(&p->h->head, head + len, memory_order_release);
    return 1;
}

/* Signal a pending sync request (e.g. eglSwapBuffers) and wait until the host
 * bumps reply_seq past our request. Spin-then-(optionally caller-yields). Matches
 * RingProducer::flush_and_wait: req_seq.fetch_add(1, acq_rel), then spin on
 * reply_seq>=want or closed. Returns the observed reply_seq.
 *
 * NOTE: like the host header, this is a SPIN fallback that is correct but burns
 * CPU. The real integration should additionally signal/wait on an eventfd the
 * loader inherits (ALR_GPU_RING_DOORBELL_FD, see README) — the host writes the
 * eventfd when it posts the reply, and the guest can block-read it instead of
 * spinning. The spin remains as the portable fallback. */
static inline uint32_t alr_ring_flush_and_wait(AlrRingProducer *p, unsigned spin) {
    uint32_t want, i;
    if (!alr_ring_producer_valid(p)) return 0;
    want = atomic_fetch_add_explicit(&p->h->req_seq, 1u, memory_order_acq_rel) + 1u;
    if (spin == 0) spin = (1u << 20);
    for (i = 0; i < spin; ++i) {
        if (atomic_load_explicit(&p->h->reply_seq, memory_order_acquire) >= want) break;
        if (atomic_load_explicit(&p->h->closed, memory_order_acquire)) break;
    }
    return atomic_load_explicit(&p->h->reply_seq, memory_order_acquire);
}

static inline void alr_ring_close(AlrRingProducer *p) {
    if (p && p->h) atomic_store_explicit(&p->h->closed, 1u, memory_order_release);
}

#endif /* ALR_GPU_RING_C_H */
