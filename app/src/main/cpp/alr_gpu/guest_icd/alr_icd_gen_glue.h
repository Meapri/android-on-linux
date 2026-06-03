/* alr_icd_gen_glue.h — HAND-WRITTEN ICD-side runtime the GENERATED entrypoints need.
 *
 * The codegen output (alr_gpu/generated/alr_gpu_vk_gen_icd.inc + _icd_runtime.inc) is
 * mechanical: per-entrypoint marshalling + reply scanners. It depends on two small pieces
 * of hand-written guest runtime that are NOT per-entrypoint and so are not generated:
 *
 *   1. AlrVkReader — a tiny little-endian byte reader (u8/u16/u32/u64/i32) the generated
 *      reply scanners walk the reply stream with. The hand-written ICD has its own ad-hoc
 *      scanner (alr_icd_scan_reply) but no reusable reader; this provides one.
 *
 *   2. The same-process ARENA glue (the zero-copy vkMapMemory keystone): attach to the
 *      inherited arena memfd (ALR_VK_ARENA_FD) MAP_SHARED, resolve an arena offset the host
 *      returned to a guest pointer (alr_icd_arena_ptr), and record each device-memory
 *      virtual id's arena offset (alr_icd_mem_record) so a later vkMapMemory is a local
 *      lookup. Reuses the C++-free POSIX layer of alr_gpu_vk_arena.hpp's design, ported to
 *      pure C here (the ICD is C11).
 *
 * Pure C11, glibc-guest-buildable (zig cc), header-only. Included by alr_icd_vulkan.c
 * BEFORE the generated .inc files.
 */
#ifndef ALR_ICD_GEN_GLUE_H
#define ALR_ICD_GEN_GLUE_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "alr_icd_env.h"

/* ---- AlrVkReader: bounds-checked little-endian cursor (the generated scanners' reader). */
typedef struct AlrVkReader {
    const uint8_t *p;
    uint32_t n;
    uint32_t pos;
} AlrVkReader;

static inline void alr_vk_reader_init(AlrVkReader *r, const uint8_t *p, uint32_t n) {
    r->p = p; r->n = n; r->pos = 0;
}
static inline int alr_vk_reader_take(AlrVkReader *r, void *out, uint32_t bytes) {
    if (r->pos + bytes > r->n) return 0;
    memcpy(out, r->p + r->pos, bytes);
    r->pos += bytes;
    return 1;
}
static inline int alr_vk_reader_u8(AlrVkReader *r, uint8_t *v)  { return alr_vk_reader_take(r, v, 1); }
static inline int alr_vk_reader_u16(AlrVkReader *r, uint16_t *v){ return alr_vk_reader_take(r, v, 2); }
static inline int alr_vk_reader_u32(AlrVkReader *r, uint32_t *v){ return alr_vk_reader_take(r, v, 4); }
static inline int alr_vk_reader_u64(AlrVkReader *r, uint64_t *v){ return alr_vk_reader_take(r, v, 8); }
static inline int alr_vk_reader_i32(AlrVkReader *r, int32_t *v) { return alr_vk_reader_take(r, v, 4); }

/* ---- The same-process arena (guest side): map the inherited memfd MAP_SHARED. ---- */
typedef struct AlrIcdArena {
    int      ok;        /* 1 if mapped */
    void    *base;      /* MAP_SHARED mapping of the arena memfd */
    uint64_t size;      /* arena size in bytes */
} AlrIcdArena;

static AlrIcdArena     g_icd_arena;
static pthread_once_t  g_icd_arena_once = PTHREAD_ONCE_INIT;

static long alr_icd_arena_env_long(const char *key, long dflt) {
    const char *v = getenv(key);
    char *end = NULL;
    long n;
    if (!v || !*v) return dflt;
    n = strtol(v, &end, 10);
    return (end == v) ? dflt : n;
}

static void alr_icd_arena_init_once(void) {
    long fd, bytes;
    void *p;
    memset(&g_icd_arena, 0, sizeof(g_icd_arena));
    fd = alr_icd_arena_env_long(ALR_VK_ENV_ARENA_FD, -1);
    bytes = alr_icd_arena_env_long(ALR_VK_ENV_ARENA_BYTES, 0);
    if (fd < 0 || bytes <= 0) return;  /* no arena -> map_memory degrades (see header) */
    p = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (p == MAP_FAILED) return;
    g_icd_arena.base = p;
    g_icd_arena.size = (uint64_t)bytes;
    g_icd_arena.ok = 1;
}

static AlrIcdArena *alr_icd_arena(void) {
    pthread_once(&g_icd_arena_once, alr_icd_arena_init_once);
    return &g_icd_arena;
}

/* Resolve an arena offset the host returned to THIS process's pointer. Returns NULL for the
 * no-offset sentinel ((uint64_t)-1), an out-of-range offset, or if the arena isn't mapped. */
static void *alr_icd_arena_ptr(uint64_t off) {
    AlrIcdArena *a = alr_icd_arena();
    if (!a->ok || off == (uint64_t)-1 || off >= a->size) return NULL;
    return (uint8_t *)a->base + off;
}

/* Record a device-memory virtual id's arena offset + size (so a later vkMapMemory could
 * resolve it without re-asking the host; the generated vkMapMemory currently round-trips
 * each time, which is correct + simple — this table is a cache hook for a future rung). */
#define ALR_ICD_MEM_TABLE 256
typedef struct AlrIcdMemEntry { uint32_t vmem; uint64_t arena_off; uint64_t size; } AlrIcdMemEntry;
static AlrIcdMemEntry  g_icd_mem[ALR_ICD_MEM_TABLE];
static pthread_mutex_t g_icd_mem_lock = PTHREAD_MUTEX_INITIALIZER;

static void alr_icd_mem_record(uint32_t vmem, uint64_t arena_off, uint64_t size) {
    int i, free_slot = -1;
    pthread_mutex_lock(&g_icd_mem_lock);
    for (i = 0; i < ALR_ICD_MEM_TABLE; ++i) {
        if (g_icd_mem[i].vmem == vmem) { free_slot = i; break; }
        if (free_slot < 0 && g_icd_mem[i].vmem == 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_icd_mem[free_slot].vmem = vmem;
        g_icd_mem[free_slot].arena_off = arena_off;
        g_icd_mem[free_slot].size = size;
    }
    pthread_mutex_unlock(&g_icd_mem_lock);
}

#endif /* ALR_ICD_GEN_GLUE_H */
