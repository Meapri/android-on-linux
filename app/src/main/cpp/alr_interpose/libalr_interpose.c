/*
 * libalr_interpose.c — ALR in-process LD_PRELOAD path-mediation interposer.
 *
 * Purpose
 * -------
 * The ALR loader runs dynamic glibc guests (e.g. GIMP) in-process and mediates
 * every file syscall in the parent via a stacked SECCOMP_RET_TRACE filter:
 *   guest open()  -> seccomp RET_TRACE -> PTRACE_EVENT_SECCOMP
 *               -> parent reads x1 from /proc/<tid>/mem -> translate -> write back.
 * That ptrace round-trip is microseconds *per file syscall*. GIMP opens
 * thousands of files at startup (fonts/brushes/data/config), so the round-trip
 * is too slow to reach the main window within the watchdog.
 *
 * This .so is LD_PRELOAD'd into the guest. It interposes the glibc *wrappers*
 * (open/openat/stat/access/...) and rewrites absolute guest paths to
 * <ALR_ROOTFS>+path BEFORE the underlying syscall, entirely in-process
 * (nanoseconds). The seccomp-trace filter then sees an already-rootfs path and,
 * thanks to its idempotency guard (a path already under ALR_ROOTFS is left
 * alone), does nothing. The ptrace path remains the safety net for everything
 * the interposer cannot reach (static binaries, raw inline syscalls, ld.so's
 * own loads).
 *
 * Rewrite policy — MUST match the supervisor (runtime_report.cpp, the
 * PTRACE_EVENT_SECCOMP block, and alr_runtime/alr_path.cpp::translate_rootfs_path):
 *   For a pathname p:
 *     - p == NULL or p[0] != '/'  (relative)            -> unchanged
 *     - p under /proc, /sys, or /dev (kernel virtual fs) -> unchanged
 *     - p already under $ALR_ROOTFS (idempotent)         -> unchanged
 *     - otherwise                                        -> $ALR_ROOTFS + p
 *   "under(p, d)" is a boundary-aware prefix test: p starts with d AND the next
 *   char of p is '/' or '\0' (so "/proc" and "/proc/x" match, "/procx" does not)
 *   — identical to the supervisor's `under` lambda.
 *   ALR_ROOTFS's trailing slashes are trimmed (matching trim_trailing_slashes),
 *   except a lone "/" is kept. Path normalization (".." clamping) is NOT done
 *   here: a plain prefix is sufficient because the seccomp net still clamps
 *   anything we miss.
 *
 * The loader is expected to set, in the guest environment:
 *     ALR_ROOTFS=<rootfs host dir>        (absolute, no required trailing slash)
 *     LD_PRELOAD=/usr/lib/libalr_interpose.so   (guest path to this .so)
 *
 * Build:
 *   zig cc --target=aarch64-linux-gnu.2.36 -shared -fPIC -O2 \
 *       -o libalr_interpose.so libalr_interpose.c
 *
 * Implementation notes
 * --------------------
 * - We call the real libc function via a cached dlsym(RTLD_NEXT, "name")
 *   pointer, resolved lazily on first use. We never call the libc wrapper
 *   by name internally (that would recurse into ourselves).
 * - The path is rewritten into a caller-supplied stack buffer (no malloc), so
 *   the hot path performs zero heap allocation and is reentrancy-safe.
 * - Everything is implemented with hand-rolled byte ops (no strlen/strcpy)
 *   to avoid any chance of recursing through an interposed libc string routine
 *   and to stay allocation-free.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>     /* getenv */
#include <fcntl.h>
#include <time.h>       /* struct timespec (utimensat) */
#include <sys/types.h>
#include <sys/stat.h>   /* struct stat[64], struct statx, statx flags */

/* glibc marks the path arg of many of these wrappers __nonnull, so the
 * compiler "knows" path != NULL and warns that our defensive `path &&` guard
 * is always true. We keep the guard (a guest can still pass NULL via a raw
 * call into our wrapper) and silence just that diagnostic. */
#pragma GCC diagnostic ignored "-Wpointer-bool-conversion"
#pragma GCC diagnostic ignored "-Wtautological-pointer-compare"

/* ----- tiny self-contained byte helpers (no libc string calls) ----- */

static size_t a_len(const char *s) {
    size_t n = 0;
    if (s) { while (s[n]) ++n; }
    return n;
}

/* boundary-aware prefix: returns 1 iff p starts with d and the char right after
 * the prefix in p is '/' or '\0'. Mirrors the supervisor's `under` lambda. */
static int a_under(const char *p, const char *d) {
    size_t i = 0;
    while (d[i]) {
        if (p[i] != d[i]) return 0;
        ++i;
    }
    return p[i] == '/' || p[i] == '\0';
}

/* ----- ALR_ROOTFS, captured once at init ----- */

/* Holds the trimmed rootfs prefix (trailing '/'s removed, lone "/" kept).
 * If ALR_ROOTFS is unset/empty, g_rootfs_len == 0 and rw() is a pure
 * pass-through (defensive: the interposer then does nothing and the ptrace
 * net still mediates). Sized generously for any plausible Android data path. */
static char   g_rootfs[1024];
static size_t g_rootfs_len = 0;
static int    g_inited = 0;

static void alr_init(void) {
    if (g_inited) return;
    g_inited = 1;

    /* getenv is safe to resolve normally; it is not interposed here. */
    const char *r = getenv("ALR_ROOTFS");
    if (!r || r[0] != '/') {        /* must be an absolute host path */
        g_rootfs_len = 0;
        return;
    }
    size_t n = a_len(r);
    /* trim trailing slashes, but keep a lone "/" (matches trim_trailing_slashes) */
    while (n > 1 && r[n - 1] == '/') --n;
    if (n >= sizeof(g_rootfs)) n = sizeof(g_rootfs) - 1;   /* clamp, never overflow */
    for (size_t i = 0; i < n; ++i) g_rootfs[i] = r[i];
    g_rootfs[n] = '\0';
    /* A rootfs of exactly "/" is a no-op prefix; treat as disabled. */
    g_rootfs_len = (n == 1 && g_rootfs[0] == '/') ? 0 : n;
}

/*
 * rw(): the single rewrite helper.
 * Returns p unchanged, or a pointer into buf holding "<rootfs><p>".
 * buf must hold at least g_rootfs_len + strlen(p) + 1 bytes; if it is too
 * small we fail safe by returning p unchanged (the seccomp net catches it).
 */
static const char *rw(const char *p, char *buf, size_t buflen) {
    if (!p || p[0] != '/') return p;            /* NULL or relative -> passthrough */
    if (!g_inited) alr_init();
    if (g_rootfs_len == 0) return p;            /* interposer disabled */

    /* /proc, /sys, /dev are kernel virtual filesystems -> never rewritten. */
    if (a_under(p, "/proc") || a_under(p, "/sys") || a_under(p, "/dev"))
        return p;

    /* Idempotency: a path already under the rootfs is left as-is (matches the
     * supervisor's already_host guard; prevents <rootfs><rootfs>/...). */
    if (a_under(p, g_rootfs)) return p;

    size_t plen = a_len(p);
    if (g_rootfs_len + plen + 1 > buflen) return p;   /* too long -> fail safe */

    size_t i = 0;
    for (; i < g_rootfs_len; ++i) buf[i] = g_rootfs[i];
    for (size_t j = 0; j <= plen; ++j) buf[i + j] = p[j];   /* copies the NUL too */
    return buf;
}

/* Per-call scratch sized for rootfs prefix + a long guest path. */
#define ALR_PBUF 2304

/* ----- RTLD_NEXT resolution, cached per wrapper ----- */

/* Resolve & cache the real libc symbol "sym" into the static slot `slot`.
 * The store of a pointer is atomic on aarch64; a benign race just resolves
 * twice to the same value. */
#define ALR_REAL(slot, type, sym)                                   \
    do {                                                            \
        if (!(slot)) (slot) = (type)dlsym(RTLD_NEXT, sym);         \
    } while (0)

/* =================================================================== */
/* open family (variadic: extract mode only when creating)             */
/* =================================================================== */

static mode_t alr_va_mode(int flags, va_list ap) {
    /* mode is only consumed by the kernel when O_CREAT or O_TMPFILE is set. */
#ifdef O_TMPFILE
    if (flags & (O_CREAT | O_TMPFILE))
#else
    if (flags & O_CREAT)
#endif
        return (mode_t)va_arg(ap, int /* mode_t promotes to int */);
    return 0;
}

int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    ALR_REAL(real, int (*)(const char *, int, ...), "open");
    char b[ALR_PBUF];
    const char *p = rw(path, b, sizeof b);
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    return real(p, flags, m);
}

int open64(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    ALR_REAL(real, int (*)(const char *, int, ...), "open64");
    char b[ALR_PBUF];
    const char *p = rw(path, b, sizeof b);
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    return real(p, flags, m);
}

/* FORTIFY (_FORTIFY_SOURCE) variants glib/GIMP may emit. */
int __open_2(const char *path, int flags) {
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "__open_2");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), flags);
}

int __open64_2(const char *path, int flags) {
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "__open64_2");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), flags);
}

static int alr_openat(int (*real)(int, const char *, int, ...),
                      int dirfd, const char *path, int flags, mode_t m) {
    char b[ALR_PBUF];
    /* *at: only rewrite ABSOLUTE paths. dirfd-relative paths resolve against
     * the guest cwd, which we do not mediate here. (AT_FDCWD + absolute path
     * still gets rewritten because the path itself is absolute.) */
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags, m);
}

int openat(int dirfd, const char *path, int flags, ...) {
    static int (*real)(int, const char *, int, ...);
    ALR_REAL(real, int (*)(int, const char *, int, ...), "openat");
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    return alr_openat(real, dirfd, path, flags, m);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static int (*real)(int, const char *, int, ...);
    ALR_REAL(real, int (*)(int, const char *, int, ...), "openat64");
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    return alr_openat(real, dirfd, path, flags, m);
}

int __openat_2(int dirfd, const char *path, int flags) {
    static int (*real)(int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int), "__openat_2");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags);
}

int __openat64_2(int dirfd, const char *path, int flags) {
    static int (*real)(int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int), "__openat64_2");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags);
}

int creat(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "creat");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}

int creat64(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "creat64");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}

/* =================================================================== */
/* stat family                                                         */
/* =================================================================== */

#define ALR_WRAP_PATH_RET(rettype, name, argtype)                      \
    rettype name(const char *path, argtype out) {                      \
        static rettype (*real)(const char *, argtype);                 \
        ALR_REAL(real, rettype (*)(const char *, argtype), #name);     \
        char b[ALR_PBUF];                                              \
        return real(rw(path, b, sizeof b), out);                       \
    }

ALR_WRAP_PATH_RET(int, stat,   struct stat *)
ALR_WRAP_PATH_RET(int, lstat,  struct stat *)
ALR_WRAP_PATH_RET(int, stat64,  struct stat64 *)
ALR_WRAP_PATH_RET(int, lstat64, struct stat64 *)

/* glibc <2.33 vtable entry points (__xstat etc.): version int + path + buf. */
int __xstat(int ver, const char *path, struct stat *buf) {
    static int (*real)(int, const char *, struct stat *);
    ALR_REAL(real, int (*)(int, const char *, struct stat *), "__xstat");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}
int __lxstat(int ver, const char *path, struct stat *buf) {
    static int (*real)(int, const char *, struct stat *);
    ALR_REAL(real, int (*)(int, const char *, struct stat *), "__lxstat");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}
int __xstat64(int ver, const char *path, struct stat64 *buf) {
    static int (*real)(int, const char *, struct stat64 *);
    ALR_REAL(real, int (*)(int, const char *, struct stat64 *), "__xstat64");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}
int __lxstat64(int ver, const char *path, struct stat64 *buf) {
    static int (*real)(int, const char *, struct stat64 *);
    ALR_REAL(real, int (*)(int, const char *, struct stat64 *), "__lxstat64");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}

/* fstatat family — *at, rewrite only absolute paths. */
int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    static int (*real)(int, const char *, struct stat *, int);
    ALR_REAL(real, int (*)(int, const char *, struct stat *, int), "fstatat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, flags);
}
int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags) {
    static int (*real)(int, const char *, struct stat64 *, int);
    ALR_REAL(real, int (*)(int, const char *, struct stat64 *, int), "fstatat64");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, flags);
}
int newfstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    static int (*real)(int, const char *, struct stat *, int);
    ALR_REAL(real, int (*)(int, const char *, struct stat *, int), "newfstatat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, flags);
}
int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flags) {
    static int (*real)(int, int, const char *, struct stat *, int);
    ALR_REAL(real, int (*)(int, int, const char *, struct stat *, int), "__fxstatat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(ver, dirfd, p, buf, flags);
}
int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *buf, int flags) {
    static int (*real)(int, int, const char *, struct stat64 *, int);
    ALR_REAL(real, int (*)(int, int, const char *, struct stat64 *, int), "__fxstatat64");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(ver, dirfd, p, buf, flags);
}

/* statx(dirfd, path, flags, mask, buf) — *at-style, rewrite absolute only. */
int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *buf) {
    static int (*real)(int, const char *, int, unsigned int, struct statx *);
    ALR_REAL(real, int (*)(int, const char *, int, unsigned int, struct statx *),
             "statx");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags, mask, buf);
}

/* =================================================================== */
/* access family                                                       */
/* =================================================================== */

int access(const char *path, int mode) {
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "access");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int euidaccess(const char *path, int mode) {
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "euidaccess");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int eaccess(const char *path, int mode) {
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "eaccess");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int faccessat(int dirfd, const char *path, int mode, int flags) {
    static int (*real)(int, const char *, int, int);
    ALR_REAL(real, int (*)(int, const char *, int, int), "faccessat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, flags);
}
int faccessat2(int dirfd, const char *path, int mode, int flags) {
    static int (*real)(int, const char *, int, int);
    ALR_REAL(real, int (*)(int, const char *, int, int), "faccessat2");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, flags);
}

/* =================================================================== */
/* readlink family                                                     */
/* =================================================================== */

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*real)(const char *, char *, size_t);
    ALR_REAL(real, ssize_t (*)(const char *, char *, size_t), "readlink");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), buf, bufsiz);
}
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*real)(int, const char *, char *, size_t);
    ALR_REAL(real, ssize_t (*)(int, const char *, char *, size_t), "readlinkat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, bufsiz);
}

/* =================================================================== */
/* directory enumeration                                               */
/* =================================================================== */

/* opendir/scandir take a path string; rewrite it. We forward to the cached
 * real impl with opaque return types to avoid pulling <dirent.h> structs into
 * every call signature. */
void *opendir(const char *path) {
    static void *(*real)(const char *);
    ALR_REAL(real, void *(*)(const char *), "opendir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int scandir(const char *path, void *namelist, void *filter, void *compar) {
    static int (*real)(const char *, void *, void *, void *);
    ALR_REAL(real, int (*)(const char *, void *, void *, void *), "scandir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), namelist, filter, compar);
}
int scandir64(const char *path, void *namelist, void *filter, void *compar) {
    static int (*real)(const char *, void *, void *, void *);
    ALR_REAL(real, int (*)(const char *, void *, void *, void *), "scandir64");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), namelist, filter, compar);
}

/* =================================================================== */
/* mutating single-path ops                                            */
/* =================================================================== */

int mkdir(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "mkdir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int mkdirat(int dirfd, const char *path, mode_t mode) {
    static int (*real)(int, const char *, mode_t);
    ALR_REAL(real, int (*)(int, const char *, mode_t), "mkdirat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode);
}
int rmdir(const char *path) {
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "rmdir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int unlink(const char *path) {
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "unlink");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int unlinkat(int dirfd, const char *path, int flags) {
    static int (*real)(int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int), "unlinkat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags);
}
int remove(const char *path) {
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "remove");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int chdir(const char *path) {
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "chdir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int chmod(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "chmod");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    static int (*real)(int, const char *, mode_t, int);
    ALR_REAL(real, int (*)(int, const char *, mode_t, int), "fchmodat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, flags);
}

struct utimbuf;  /* opaque; we only forward the pointer */
int utime(const char *path, const struct utimbuf *times) {
    static int (*real)(const char *, const struct utimbuf *);
    ALR_REAL(real, int (*)(const char *, const struct utimbuf *), "utime");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), times);
}
int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
    static int (*real)(int, const char *, const struct timespec[2], int);
    ALR_REAL(real, int (*)(int, const char *, const struct timespec[2], int),
             "utimensat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, times, flags);
}

/* =================================================================== */
/* two-path ops (rewrite BOTH path arguments)                          */
/* =================================================================== */

int rename(const char *oldp, const char *newp) {
    static int (*real)(const char *, const char *);
    ALR_REAL(real, int (*)(const char *, const char *), "rename");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    return real(rw(oldp, b1, sizeof b1), rw(newp, b2, sizeof b2));
}
int renameat(int oldfd, const char *oldp, int newfd, const char *newp) {
    static int (*real)(int, const char *, int, const char *);
    ALR_REAL(real, int (*)(int, const char *, int, const char *), "renameat");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    const char *o = (oldp && oldp[0] == '/') ? rw(oldp, b1, sizeof b1) : oldp;
    const char *n = (newp && newp[0] == '/') ? rw(newp, b2, sizeof b2) : newp;
    return real(oldfd, o, newfd, n);
}
int renameat2(int oldfd, const char *oldp, int newfd, const char *newp,
              unsigned int flags) {
    static int (*real)(int, const char *, int, const char *, unsigned int);
    ALR_REAL(real, int (*)(int, const char *, int, const char *, unsigned int),
             "renameat2");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    const char *o = (oldp && oldp[0] == '/') ? rw(oldp, b1, sizeof b1) : oldp;
    const char *n = (newp && newp[0] == '/') ? rw(newp, b2, sizeof b2) : newp;
    return real(oldfd, o, newfd, n, flags);
}

/* =================================================================== */
/* path canonicalization                                               */
/* =================================================================== */

/* realpath(path, resolved): we rewrite the *input* path. The resolved output
 * will then be a rootfs host path, which is correct for a subsequent open()
 * (already-host paths are left alone by both us and the seccomp net). */
char *realpath(const char *path, char *resolved) {
    static char *(*real)(const char *, char *);
    ALR_REAL(real, char *(*)(const char *, char *), "realpath");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), resolved);
}
char *canonicalize_file_name(const char *path) {
    static char *(*real)(const char *);
    ALR_REAL(real, char *(*)(const char *), "canonicalize_file_name");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
