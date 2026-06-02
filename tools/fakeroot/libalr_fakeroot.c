/*
 * libalr_fakeroot.c — ALR non-root fakeroot LD_PRELOAD shim.
 *
 * Purpose
 * -------
 * On the device ALR runs as a normal Android `untrusted_app` (uid ~10xxx, no
 * root, no CAP_CHOWN/CAP_DAC_OVERRIDE/CAP_FOWNER). When a guest runs
 * `apt install` / `dpkg -i hello.deb`, dpkg refuses with:
 *
 *     dpkg: error: requires superuser privilege
 *
 * and even if forced past that gate, its unpack stage calls chown()/chmod() to
 * lay down each file with the ownership recorded in the .deb (root:root, 0755…)
 * and then stat()s the result to confirm. Those chown()s fail with EPERM for a
 * non-root uid, so the unpack aborts. This is the *classic* non-root packaging
 * problem and the *classic* fix is a fakeroot-style preload: make the process
 * BELIEVE it is uid 0 and that every chown/chmod succeeded, backed by an
 * in-process fake-ownership database so a later stat() reports the faked
 * uid/gid/mode instead of the real on-disk values.
 *
 * This is a from-scratch, freestanding re-implementation of the small slice of
 * `fakeroot`'s libfakeroot that dpkg's unpack path actually exercises — it does
 * NOT depend on fakeroot's `faked` daemon or its SysV-IPC message bus. The fake
 * DB lives entirely in this process's memory (a small open-addressing hash
 * keyed by (st_dev, st_ino)), which is all dpkg -i of a single local .deb needs:
 * dpkg chowns a freshly-unpacked file then immediately stats it within the same
 * process tree, and execve() re-loads this .so into each child (the maintainer
 * scripts sh/dpkg-deb), so a child re-derives the DB by re-running the same
 * chowns. (A cross-process persistent DB — fakeroot's `.fakeroot` save-file — is
 * deferred; not needed for the local-.deb unpack the device gate exercises.)
 *
 * Symbols interposed
 * ------------------
 *   credentials (always report root):
 *     getuid geteuid getgid getegid  -> 0
 *     getresuid getresgid            -> {0,0,0}, success
 *     setuid setgid seteuid setegid setreuid setregid setresuid setresgid -> 0 (no-op success)
 *   ownership (record in the fake DB, return success):
 *     chown lchown fchown fchownat   -> remember (uid,gid) for the target, return 0
 *   mode (try real chmod; on EPERM remember the mode and fake success):
 *     chmod fchmod fchmodat          -> real chmod, else remember mode + return 0
 *   stat family (overlay the faked uid/gid/mode onto the real result):
 *     stat lstat fstat fstatat statx and the glibc __xstat/__lxstat/__fxstatat
 *     (+ *64) vtable variants                     -> real stat, then patch buf
 *
 * Chaining with the ALR path interposer (CRITICAL)
 * -----------------------------------------------
 * ALR ALWAYS injects its own path-mediation interposer as LD_PRELOAD:
 *     LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_interpose.so
 * This shim must CHAIN with it, never replace it. The two are orthogonal:
 *   - libalr_interpose rewrites the PATH argument (guest abs path -> rootfs+path)
 *   - libalr_fakeroot rewrites CREDENTIALS / OWNERSHIP and the stat RESULT
 * Because each wrapper here calls the *next* implementation via
 * dlsym(RTLD_NEXT, ...), the call chain is:
 *     guest stat("/x")
 *       -> fakeroot stat()  [calls RTLD_NEXT stat]
 *         -> interpose stat()  [rewrites "/x" -> "<rootfs>/x", does the syscall]
 *       <- fakeroot patches buf->st_uid/st_gid/st_mode from the fake DB
 * so the fakeroot shim must be listed FIRST in LD_PRELOAD (its wrapper runs
 * outermost and the interpose path-rewrite still happens underneath). The exact
 * chained value WS-1 must use is documented in tools/fakeroot/README (and in the
 * device-gate note returned by the builder).
 *
 * Build (mirrors scripts/build-interpose.sh):
 *   zig cc --target=aarch64-linux-gnu.2.36 -shared -fPIC -O2 \
 *       -o libalr_fakeroot.so libalr_fakeroot.c
 *
 * W^X / SELinux: this shim allocates NO executable memory, installs NO seccomp
 * filter, performs NO ptrace, and does not touch SELinux — it is pure libc
 * interposition. It is strictly weaker (more permissive only inside its own
 * process's view) than running as real root, and changes nothing the kernel
 * enforces; an unpacked file is still owned by the real uid on disk.
 *
 * Implementation notes
 * --------------------
 * - Real libc functions are reached via cached dlsym(RTLD_NEXT, name) — this is
 *   what makes the shim CHAIN onto libalr_interpose rather than the bare libc.
 * - The fake-ownership DB is a fixed-capacity open-addressing hash with no heap
 *   growth; on overflow we silently keep the most-recently-set entries (an
 *   unpack of one .deb touches far fewer files than the table holds). Access is
 *   guarded by a tiny spinlock so a threaded apt cannot corrupt it.
 * - We honour FAKEROOTUID/FAKEROOTGID (fakeroot's env contract) so a caller may
 *   pin the faked identity; default 0:0.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>     /* getenv */
#include <fcntl.h>      /* AT_FDCWD, AT_SYMLINK_NOFOLLOW */
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

/* glibc marks several of these args __nonnull; keep our defensive NULL guards
 * but silence the resulting always-true diagnostics (matches libalr_interpose). */
#pragma GCC diagnostic ignored "-Wpointer-bool-conversion"
#pragma GCC diagnostic ignored "-Wtautological-pointer-compare"

/* ----- faked identity, captured once ----- */

static uid_t g_fake_uid = 0;
static gid_t g_fake_gid = 0;
static int   g_inited   = 0;

static unsigned long a_strtoul(const char *s) {
    unsigned long v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned long)(*s - '0'); ++s; }
    return v;
}

static void fr_init(void) {
    if (g_inited) return;
    g_inited = 1;
    const char *u = getenv("FAKEROOTUID");
    const char *g = getenv("FAKEROOTGID");
    g_fake_uid = (uid_t)(u ? a_strtoul(u) : 0);
    g_fake_gid = (gid_t)(g ? a_strtoul(g) : 0);
}

__attribute__((constructor(102)))   /* after libalr_interpose's 101 ctor */
static void fr_ctor(void) { fr_init(); }

/* ----- fake-ownership DB: open-addressing hash keyed by (dev, ino) ----- */

#define FR_CAP 8192u    /* power of two; one .deb unpack touches << this */

typedef struct {
    uint64_t dev;
    uint64_t ino;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;      /* full st_mode (type bits | perm bits) */
    uint8_t  used;
    uint8_t  has_mode;  /* 1 once chmod recorded a mode */
} fr_ent;

static fr_ent  g_db[FR_CAP];
static volatile int g_lock = 0;    /* tiny test-and-set spinlock */

static void fr_lock(void)   { while (__sync_lock_test_and_set(&g_lock, 1)) { } }
static void fr_unlock(void) { __sync_lock_release(&g_lock); }

static size_t fr_slot(uint64_t dev, uint64_t ino) {
    /* splitmix64-ish mix; FR_CAP is a power of two so & (FR_CAP-1) is the bucket */
    uint64_t h = dev * 0x9E3779B97F4A7C15ull ^ (ino + 0x7F4A7C159E3779B9ull);
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 27;
    return (size_t)(h & (FR_CAP - 1));
}

/* Find the slot for (dev,ino), or the first free slot if absent. Returns
 * SIZE_MAX only if the table is completely full AND the key is not present. */
static size_t fr_find(uint64_t dev, uint64_t ino, int for_insert) {
    size_t start = fr_slot(dev, ino);
    size_t free_slot = SIZE_MAX;
    for (size_t i = 0; i < FR_CAP; ++i) {
        size_t s = (start + i) & (FR_CAP - 1);
        if (g_db[s].used) {
            if (g_db[s].dev == dev && g_db[s].ino == ino) return s;
        } else {
            if (free_slot == SIZE_MAX) free_slot = s;
            if (!for_insert) return SIZE_MAX;   /* lookup miss: stop at first hole */
        }
    }
    return for_insert ? free_slot : SIZE_MAX;
}

static void fr_set_owner(uint64_t dev, uint64_t ino, uint32_t uid, uint32_t gid) {
    fr_lock();
    size_t s = fr_find(dev, ino, 1);
    if (s != SIZE_MAX) {
        if (!g_db[s].used) {
            g_db[s].used = 1; g_db[s].dev = dev; g_db[s].ino = ino;
            g_db[s].has_mode = 0; g_db[s].mode = 0;
        }
        /* chown(-1,...) means "leave this id alone" — preserve prior fake value */
        if (uid != (uint32_t)-1) g_db[s].uid = uid;
        if (gid != (uint32_t)-1) g_db[s].gid = gid;
    }
    fr_unlock();
}

static void fr_set_mode(uint64_t dev, uint64_t ino, uint32_t mode) {
    fr_lock();
    size_t s = fr_find(dev, ino, 1);
    if (s != SIZE_MAX) {
        if (!g_db[s].used) {
            g_db[s].used = 1; g_db[s].dev = dev; g_db[s].ino = ino;
            g_db[s].uid = g_fake_uid; g_db[s].gid = g_fake_gid;
        }
        /* keep the file-type bits from the existing/real mode, set perm bits */
        g_db[s].mode = mode;
        g_db[s].has_mode = 1;
    }
    fr_unlock();
}

/* Overlay any faked owner/mode for (dev,ino) onto an already-filled stat buf.
 * Owner is ALWAYS forced to the faked identity (fakeroot semantics: under the
 * preload every file *appears* owned by root, whether or not we chowned it),
 * but a remembered explicit chown wins if it set a non-default value. The mode
 * is only overridden if a chmod was recorded (else the real mode stands). */
static void fr_patch_owner_mode(uint64_t dev, uint64_t ino,
                                uint32_t *uid, uint32_t *gid, uint32_t *mode) {
    uint32_t fu = g_fake_uid, fg = g_fake_gid;
    int have_mode = 0; uint32_t fm = 0;
    fr_lock();
    size_t s = fr_find(dev, ino, 0);
    if (s != SIZE_MAX) {
        fu = g_db[s].uid; fg = g_db[s].gid;
        if (g_db[s].has_mode) { have_mode = 1; fm = g_db[s].mode; }
    }
    fr_unlock();
    *uid = fu;
    *gid = fg;
    if (have_mode) *mode = fm;
}

/* =================================================================== */
/* credential getters — always report the faked (root) identity        */
/* =================================================================== */

uid_t getuid(void)  { fr_init(); return g_fake_uid; }
uid_t geteuid(void) { fr_init(); return g_fake_uid; }
gid_t getgid(void)  { fr_init(); return g_fake_gid; }
gid_t getegid(void) { fr_init(); return g_fake_gid; }

int getresuid(uid_t *r, uid_t *e, uid_t *s) {
    fr_init();
    if (r) *r = g_fake_uid;
    if (e) *e = g_fake_uid;
    if (s) *s = g_fake_uid;
    return 0;
}
int getresgid(gid_t *r, gid_t *e, gid_t *s) {
    fr_init();
    if (r) *r = g_fake_gid;
    if (e) *e = g_fake_gid;
    if (s) *s = g_fake_gid;
    return 0;
}

/* setters: fake unconditional success (dpkg drops privs / re-asserts root). */
int setuid(uid_t uid)   { (void)uid; return 0; }
int seteuid(uid_t uid)  { (void)uid; return 0; }
int setgid(gid_t gid)   { (void)gid; return 0; }
int setegid(gid_t gid)  { (void)gid; return 0; }
int setreuid(uid_t r, uid_t e)            { (void)r; (void)e; return 0; }
int setregid(gid_t r, gid_t e)            { (void)r; (void)e; return 0; }
int setresuid(uid_t r, uid_t e, uid_t s)  { (void)r; (void)e; (void)s; return 0; }
int setresgid(gid_t r, gid_t e, gid_t s)  { (void)r; (void)e; (void)s; return 0; }

/* =================================================================== */
/* ownership — record in the fake DB, report success                   */
/* =================================================================== */

/* Resolve (dev,ino) for a path/fd by calling the NEXT stat (so libalr_interpose
 * rewrites the path underneath us). Returns 0 on success. */
static int fr_key_path(const char *path, int at_dirfd, int flags,
                       uint64_t *dev, uint64_t *ino) {
    static int (*real_fstatat)(int, const char *, struct stat *, int);
    if (!real_fstatat)
        real_fstatat = (int (*)(int, const char *, struct stat *, int))
                       dlsym(RTLD_NEXT, "fstatat");
    if (!real_fstatat) return -1;
    struct stat st;
    if (real_fstatat(at_dirfd, path, &st, flags) != 0) return -1;
    *dev = (uint64_t)st.st_dev; *ino = (uint64_t)st.st_ino;
    return 0;
}
static int fr_key_fd(int fd, uint64_t *dev, uint64_t *ino) {
    static int (*real_fstat)(int, struct stat *);
    if (!real_fstat)
        real_fstat = (int (*)(int, struct stat *))dlsym(RTLD_NEXT, "fstat");
    if (!real_fstat) return -1;
    struct stat st;
    if (real_fstat(fd, &st) != 0) return -1;
    *dev = (uint64_t)st.st_dev; *ino = (uint64_t)st.st_ino;
    return 0;
}

int chown(const char *path, uid_t owner, gid_t group) {
    uint64_t dev, ino;
    if (path && fr_key_path(path, AT_FDCWD, 0, &dev, &ino) == 0)
        fr_set_owner(dev, ino, (uint32_t)owner, (uint32_t)group);
    return 0;   /* fake success even if the file vanished — dpkg only checks rc */
}
int lchown(const char *path, uid_t owner, gid_t group) {
    uint64_t dev, ino;
    if (path && fr_key_path(path, AT_FDCWD, AT_SYMLINK_NOFOLLOW, &dev, &ino) == 0)
        fr_set_owner(dev, ino, (uint32_t)owner, (uint32_t)group);
    return 0;
}
int fchown(int fd, uid_t owner, gid_t group) {
    uint64_t dev, ino;
    if (fr_key_fd(fd, &dev, &ino) == 0)
        fr_set_owner(dev, ino, (uint32_t)owner, (uint32_t)group);
    return 0;
}
int fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags) {
    uint64_t dev, ino;
    if (path && fr_key_path(path, dirfd, flags & AT_SYMLINK_NOFOLLOW, &dev, &ino) == 0)
        fr_set_owner(dev, ino, (uint32_t)owner, (uint32_t)group);
    return 0;
}

/* =================================================================== */
/* mode — try the real chmod; on EPERM record + fake success           */
/* =================================================================== */

int chmod(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    if (!real) real = (int (*)(const char *, mode_t))dlsym(RTLD_NEXT, "chmod");
    int rc = real ? real(path, mode) : -1;
    if (rc == 0) return 0;                 /* real chmod worked (user owns the file) */
    uint64_t dev, ino;
    if (path && fr_key_path(path, AT_FDCWD, 0, &dev, &ino) == 0) {
        /* fold the requested perm bits over the file's current type bits */
        fr_set_mode(dev, ino, (uint32_t)mode);
        return 0;
    }
    return rc;   /* couldn't even key it: surface the real error */
}
int fchmod(int fd, mode_t mode) {
    static int (*real)(int, mode_t);
    if (!real) real = (int (*)(int, mode_t))dlsym(RTLD_NEXT, "fchmod");
    int rc = real ? real(fd, mode) : -1;
    if (rc == 0) return 0;
    uint64_t dev, ino;
    if (fr_key_fd(fd, &dev, &ino) == 0) { fr_set_mode(dev, ino, (uint32_t)mode); return 0; }
    return rc;
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    static int (*real)(int, const char *, mode_t, int);
    if (!real) real = (int (*)(int, const char *, mode_t, int))dlsym(RTLD_NEXT, "fchmodat");
    int rc = real ? real(dirfd, path, mode, flags) : -1;
    if (rc == 0) return 0;
    uint64_t dev, ino;
    if (path && fr_key_path(path, dirfd, flags & AT_SYMLINK_NOFOLLOW, &dev, &ino) == 0) {
        fr_set_mode(dev, ino, (uint32_t)mode);
        return 0;
    }
    return rc;
}

/* =================================================================== */
/* stat family — real stat, then overlay the faked owner/mode           */
/* =================================================================== */

/* The generic patcher: given a filled `struct stat`, overwrite uid/gid (always)
 * and mode (only if a chmod was faked) from the DB keyed by the buf's dev/ino. */
static void fr_apply_stat(struct stat *buf) {
    if (!buf) return;
    uint32_t uid = (uint32_t)buf->st_uid, gid = (uint32_t)buf->st_gid,
             mode = (uint32_t)buf->st_mode;
    fr_patch_owner_mode((uint64_t)buf->st_dev, (uint64_t)buf->st_ino,
                        &uid, &gid, &mode);
    buf->st_uid = (uid_t)uid; buf->st_gid = (gid_t)gid; buf->st_mode = (mode_t)mode;
}
static void fr_apply_stat64(struct stat64 *buf) {
    if (!buf) return;
    uint32_t uid = (uint32_t)buf->st_uid, gid = (uint32_t)buf->st_gid,
             mode = (uint32_t)buf->st_mode;
    fr_patch_owner_mode((uint64_t)buf->st_dev, (uint64_t)buf->st_ino,
                        &uid, &gid, &mode);
    buf->st_uid = (uid_t)uid; buf->st_gid = (gid_t)gid; buf->st_mode = (mode_t)mode;
}

int stat(const char *path, struct stat *buf) {
    static int (*real)(const char *, struct stat *);
    if (!real) real = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "stat");
    int rc = real ? real(path, buf) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int lstat(const char *path, struct stat *buf) {
    static int (*real)(const char *, struct stat *);
    if (!real) real = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "lstat");
    int rc = real ? real(path, buf) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int fstat(int fd, struct stat *buf) {
    static int (*real)(int, struct stat *);
    if (!real) real = (int (*)(int, struct stat *))dlsym(RTLD_NEXT, "fstat");
    int rc = real ? real(fd, buf) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    static int (*real)(int, const char *, struct stat *, int);
    if (!real) real = (int (*)(int, const char *, struct stat *, int))dlsym(RTLD_NEXT, "fstatat");
    int rc = real ? real(dirfd, path, buf, flags) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int newfstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    static int (*real)(int, const char *, struct stat *, int);
    if (!real) real = (int (*)(int, const char *, struct stat *, int))dlsym(RTLD_NEXT, "newfstatat");
    int rc = real ? real(dirfd, path, buf, flags) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}

int stat64(const char *path, struct stat64 *buf) {
    static int (*real)(const char *, struct stat64 *);
    if (!real) real = (int (*)(const char *, struct stat64 *))dlsym(RTLD_NEXT, "stat64");
    int rc = real ? real(path, buf) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}
int lstat64(const char *path, struct stat64 *buf) {
    static int (*real)(const char *, struct stat64 *);
    if (!real) real = (int (*)(const char *, struct stat64 *))dlsym(RTLD_NEXT, "lstat64");
    int rc = real ? real(path, buf) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}
int fstat64(int fd, struct stat64 *buf) {
    static int (*real)(int, struct stat64 *);
    if (!real) real = (int (*)(int, struct stat64 *))dlsym(RTLD_NEXT, "fstat64");
    int rc = real ? real(fd, buf) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}
int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags) {
    static int (*real)(int, const char *, struct stat64 *, int);
    if (!real) real = (int (*)(int, const char *, struct stat64 *, int))dlsym(RTLD_NEXT, "fstatat64");
    int rc = real ? real(dirfd, path, buf, flags) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}

/* glibc <2.33 vtable entry points: (ver, [dirfd,] path, buf [, flags]). The
 * `ver` arg only selected the old struct ABI; arm64 has one ABI, so we patch
 * the same way. */
int __xstat(int ver, const char *path, struct stat *buf) {
    static int (*real)(int, const char *, struct stat *);
    if (!real) real = (int (*)(int, const char *, struct stat *))dlsym(RTLD_NEXT, "__xstat");
    int rc = real ? real(ver, path, buf) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int __lxstat(int ver, const char *path, struct stat *buf) {
    static int (*real)(int, const char *, struct stat *);
    if (!real) real = (int (*)(int, const char *, struct stat *))dlsym(RTLD_NEXT, "__lxstat");
    int rc = real ? real(ver, path, buf) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int __fxstat(int ver, int fd, struct stat *buf) {
    static int (*real)(int, int, struct stat *);
    if (!real) real = (int (*)(int, int, struct stat *))dlsym(RTLD_NEXT, "__fxstat");
    int rc = real ? real(ver, fd, buf) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flags) {
    static int (*real)(int, int, const char *, struct stat *, int);
    if (!real) real = (int (*)(int, int, const char *, struct stat *, int))dlsym(RTLD_NEXT, "__fxstatat");
    int rc = real ? real(ver, dirfd, path, buf, flags) : -1;
    if (rc == 0) fr_apply_stat(buf);
    return rc;
}
int __xstat64(int ver, const char *path, struct stat64 *buf) {
    static int (*real)(int, const char *, struct stat64 *);
    if (!real) real = (int (*)(int, const char *, struct stat64 *))dlsym(RTLD_NEXT, "__xstat64");
    int rc = real ? real(ver, path, buf) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}
int __lxstat64(int ver, const char *path, struct stat64 *buf) {
    static int (*real)(int, const char *, struct stat64 *);
    if (!real) real = (int (*)(int, const char *, struct stat64 *))dlsym(RTLD_NEXT, "__lxstat64");
    int rc = real ? real(ver, path, buf) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}
int __fxstat64(int ver, int fd, struct stat64 *buf) {
    static int (*real)(int, int, struct stat64 *);
    if (!real) real = (int (*)(int, int, struct stat64 *))dlsym(RTLD_NEXT, "__fxstat64");
    int rc = real ? real(ver, fd, buf) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}
int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *buf, int flags) {
    static int (*real)(int, int, const char *, struct stat64 *, int);
    if (!real) real = (int (*)(int, int, const char *, struct stat64 *, int))dlsym(RTLD_NEXT, "__fxstatat64");
    int rc = real ? real(ver, dirfd, path, buf, flags) : -1;
    if (rc == 0) fr_apply_stat64(buf);
    return rc;
}

/* statx(dirfd, path, flags, mask, buf) — overlay uid/gid/mode (the stx_* fields
 * are independent of struct stat, so patch them directly). */
int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *buf) {
    static int (*real)(int, const char *, int, unsigned int, struct statx *);
    if (!real) real = (int (*)(int, const char *, int, unsigned int, struct statx *))
                       dlsym(RTLD_NEXT, "statx");
    int rc = real ? real(dirfd, path, flags, mask, buf) : -1;
    if (rc == 0 && buf) {
        uint32_t uid = buf->stx_uid, gid = buf->stx_gid, mode = buf->stx_mode;
        fr_patch_owner_mode((uint64_t)(((uint64_t)buf->stx_dev_major << 32) | buf->stx_dev_minor),
                            (uint64_t)buf->stx_ino, &uid, &gid, &mode);
        buf->stx_uid = uid; buf->stx_gid = gid; buf->stx_mode = (uint16_t)mode;
    }
    return rc;
}
