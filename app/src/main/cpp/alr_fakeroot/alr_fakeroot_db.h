/*
 * alr_fakeroot_db.h — shared metadata DB for the ALR fakeroot LD_PRELOAD .so.
 *
 * WHY A SHARED FILE (not libfakeroot's daemon+SysV-msg):
 * ------------------------------------------------------
 * libfakeroot keeps its (ino->uid/gid/mode/dev) map in a `faked` daemon and
 * talks to it over SysV message queues. On non-root Android (untrusted_app):
 *   - SysV IPC (msgget/msgsnd) is NOT in bionic's seccomp allowlist for an app
 *     and there is no privileged `faked` we may spawn/keep.
 *   - But dpkg DOES fork()+exec() maintainer scripts (preinst/postinst/...),
 *     and the fake chown/chmod a script performs MUST be visible to the parent
 *     dpkg's later stat() — i.e. the DB must survive fork+exec.
 * So the DB is a single mmap(MAP_SHARED) file under a per-run dir
 * (ALR_FAKEROOT_DB). Every guest process that loads libalr_fakeroot.so maps the
 * SAME file, so a write in a child is immediately visible to the parent (shared
 * pages, no message round-trip). Entries are keyed by (st_dev, st_ino), which is
 * stable across the rootfs regardless of the path the interposer rewrote to —
 * this is exactly how real libfakeroot keys, and it sidesteps having to agree
 * with the interposer on the *post-rewrite host path string*.
 *
 * CONCURRENCY: a fixed-size open-addressing table in shared memory. A 1-slot
 * write is published with a release store of the key AFTER the value fields, and
 * read with an acquire load of the key; a partially-written slot is simply not
 * yet found (reader falls back to the real stat). dpkg unpacks essentially
 * single-threaded per package; cross-process writers are serialized by a single
 * futex-based spinlock word at the head of the file (FUTEX is allowed on
 * Android). The table never resizes (sized for a full base-system unpack,
 * ~256k inodes default, overridable by ALR_FAKEROOT_DB_SLOTS); on overflow we
 * fail-safe to "no override" (the real EPERM-free chown already happened, so the
 * worst case is the file keeps its real on-disk uid/gid — which for a
 * single-user rootfs the guest itself owns is *already* the value dpkg wants to
 * believe it set).
 *
 * This header is the on-disk/on-mmap layout contract. It is pure data + inline
 * helpers so the host DB-logic test (tests/test_fakeroot_db_logic.py via a tiny
 * C shim, or a Python re-impl) can validate the hashing/overflow/visibility
 * rules WITHOUT a device.
 */
#ifndef ALR_FAKEROOT_DB_H
#define ALR_FAKEROOT_DB_H

#include <stdint.h>

#define ALR_FR_MAGIC   0x414C524600314B46ULL /* "ALRF\0" ish, versioned */
#define ALR_FR_VERSION 1u

/* One metadata override entry. 32 bytes, cache-line friendly. key=(dev,ino);
 * key_ino==0 means EMPTY (ino 0 never names a real file). */
typedef struct {
    uint64_t key_dev;   /* st_dev  */
    uint64_t key_ino;   /* st_ino  (0 == empty slot) */
    uint32_t f_uid;     /* overridden owner   */
    uint32_t f_gid;     /* overridden group   */
    uint32_t f_mode;    /* overridden st_mode (type bits + perm); 0 == "keep real mode, only uid/gid set" */
    uint32_t f_rdev_lo; /* mknod device, low 32  (for char/block special) */
    uint32_t f_rdev_hi; /* mknod device, high 32 */
    uint32_t f_flags;   /* ALR_FR_F_* */
} alr_fr_entry;

enum {
    ALR_FR_F_HAVE_UIDGID = 1u << 0, /* f_uid/f_gid valid (a chown happened)   */
    ALR_FR_F_HAVE_MODE   = 1u << 1, /* f_mode valid (a chmod/mknod happened)  */
    ALR_FR_F_HAVE_RDEV   = 1u << 2, /* f_rdev valid (mknod special file)      */
    ALR_FR_F_DELETED     = 1u << 3, /* tombstone: file unlink'd; ignore real  */
};

/* File header. lock is a futex word (0=free, 1=held). */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t lock;        /* futex: 0 free / 1 held */
    uint64_t n_slots;     /* table capacity (power of two) */
    uint64_t n_used;      /* approximate; advisory only   */
    uint64_t reserved[4];
    /* alr_fr_entry slots[n_slots] follow, page-aligned. */
} alr_fr_header;

#define ALR_FR_SLOTS_OFF 4096u /* slots start on the second page */

/* FNV-1a over (dev,ino) -> slot base; open addressing probes linearly. */
static inline uint64_t alr_fr_hash(uint64_t dev, uint64_t ino) {
    uint64_t h = 1469598103934665603ULL;
    uint64_t k = dev ^ (ino * 1099511628211ULL);
    for (int i = 0; i < 8; ++i) { h ^= (k & 0xff); h *= 1099511628211ULL; k >>= 8; }
    return h;
}

#endif /* ALR_FAKEROOT_DB_H */
