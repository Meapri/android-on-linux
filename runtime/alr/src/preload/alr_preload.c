/* libalr_preload.so — guest-side glibc interposer.
 *
 * Makes the guest see <ALR_ROOT> as "/".  This is where the low-overhead claim
 * actually lives: a string prefix rewrite in the calling process, zero extra
 * context switches, versus PRoot's ptrace round-trip per path syscall.
 *
 * RULES (docs/04-preload-spec.md §2) — each of these has cost a real project
 * real days:
 *   R1  no malloc/free anywhere on the rewrite path (the guest allocator may
 *       not be initialised yet, and the guest may have hooked it)
 *   R2  never CALL the stat family, only DEFINE it (the .2.17 target enforces
 *       this at link time: stat/fstatat are GLIBC_2.33+)
 *   R3  never call realpath (glibc's internal lstat walk recurses into us)
 *   R4  no locks on the rewrite path
 *   R5  no syscalls on the rewrite path -- string ops only
 *   R6  the path rule comes from src/common/alr_path_rule.h and nowhere else
 *   R7  diagnostics go to ALR_LOG_FD, never stderr
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "alr_path_rule.h"
#include "alr_elf.h"
#include "alr_resolv_proto.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <ftw.h>
#include <glob.h>
#include <sys/inotify.h>
#include <sys/xattr.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <utime.h>
#include <unistd.h>

/* ── state ──────────────────────────────────────────────────────────── */

static char   g_root[ALR_PBUF];
static size_t g_root_len;
static const char *g_guest_exe;
static const char *g_ldso;
static const char *g_libpath;
static const char *g_preload;
static const char *g_resolv_sock;
static int g_fakeroot;
static int g_count;
static int    g_log_fd = -1;
static int    g_log;

/* Thread-local suppression: glibc's realpath() walks the path with internal
 * lstat calls that land back in our wrappers.  Rewriting there would double
 * the prefix.  (R3) */
static __thread int g_suppress;

/* Rewrite counters.  This is how the 13,500-calls-per-git-status figure in
 * docs/adr/0003 stops being MODELED: counting inside rw() is both cheaper and
 * more precise than strace, because it counts exactly the calls this layer
 * actually handles -- relative-path misses included, which strace cannot
 * distinguish from any other openat. */
static unsigned long g_n_total, g_n_rewritten, g_n_rel, g_n_sysdir, g_n_underroot;

#define RW(p, buf) alr_rw((p), g_root, g_root_len, (buf), sizeof (buf), NULL)

static void lg(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    int n;
    if (g_log < 2 || g_log_fd < 0) return;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) { ssize_t w = write(g_log_fd, b, (size_t)n); (void)w; }
}

static void alr_init(void);
static int g_ready;
static __thread int g_in_init;

/* Initialise on first use, not in the constructor: a preload's ctor runs AFTER
 * every other DSO's (see alr_init below), so wrappers are live long before it. */
static inline void ensure_init(void)
{
    if (__builtin_expect(g_ready, 1)) return;
    if (g_in_init) return;          /* dlsym re-entered us: stay in passthrough */
    alr_init();
}

/* Rewrite honouring the suppression flag. */
static inline const char *rw(const char *p, char *buf, size_t bufsz)
{
    const char *r;
    ensure_init();
    if (g_suppress) return p;
    r = alr_rw(p, g_root, g_root_len, buf, bufsz, NULL);
    if (g_count) {
        /* The classification must be TOTAL -- every counted call lands in
         * exactly one bucket.  It used to be an if/else-if chain with no
         * else, so an absolute path that was neither rewritten nor a sysdir
         * (i.e. already under the root, or NULL) incremented g_n_total and
         * nothing else.  MEASURED on a 10k git status: total=10067 against
         * 28+10008+1=10037, a silent 30-call hole.
         *
         * That matters because these counts are the call-mix half of the
         * rewrite total-cost budget (docs/04-preload-spec.md §13).  A bucket
         * with no counter is a bucket the cost model cannot price, so it
         * would be costed at zero -- an unbounded understatement in
         * principle, however small it happens to be today.
         *
         * rw_cost.c already measures this case ("already-under-root"), so the
         * per-op figure to pair with it exists; only the count was missing.
         *
         * The increments are ATOMIC.  Plain ++ lost updates under git, whose
         * index refresh is multi-threaded: the buckets summed to 119 less
         * than total on a 10k status, about 1.2%, while single-threaded id
         * and cat were exact.  Relaxed ordering is enough -- these are
         * independent counters read only after every thread has joined, and
         * nothing else is ordered against them.  The cost is irrelevant
         * because this whole block is behind g_count, which is off unless
         * ALR_COUNT=1 is set for a measurement run.
         */
#define CNT(x) __atomic_fetch_add(&(x), 1UL, __ATOMIC_RELAXED)
        CNT(g_n_total);
        if (r == buf)                       CNT(g_n_rewritten);
        else if (p && p[0] != '/')          CNT(g_n_rel);
        else if (p && alr_is_sysdir(p))     CNT(g_n_sysdir);
        else                                CNT(g_n_underroot);
#undef CNT
    }
    return r;
}

/* ── real symbol resolution ─────────────────────────────────────────── */

#define DECL(ret, name, ...) static ret (*real_##name)(__VA_ARGS__)

DECL(int,    open,       const char *, int, ...);
DECL(int,    openat,     int, const char *, int, ...);
DECL(FILE *, fopen,      const char *, const char *);
DECL(FILE *, setmntent,  const char *, const char *);
DECL(FILE *, freopen,    const char *, const char *, FILE *);
DECL(int,    stat,       const char *, struct stat *);
DECL(int,    fstat,      int, struct stat *);
DECL(int,    scandir,    const char *, struct dirent ***,
             int (*)(const struct dirent *),
             int (*)(const struct dirent **, const struct dirent **));
DECL(int,    scandir64_, const char *, struct dirent64 ***,
             int (*)(const struct dirent64 *),
             int (*)(const struct dirent64 **, const struct dirent64 **));
DECL(int,    ttyname_r,  int, char *, size_t);
DECL(int,    mkfifoat,   int, const char *, mode_t);
DECL(int,    name_to_handle_at, int, const char *, struct file_handle *, int *, int);
DECL(void *, dlmopen,    long, const char *, int);
DECL(int,    lstat,      const char *, struct stat *);
DECL(int,    fstatat,    int, const char *, struct stat *, int);
DECL(int,    statx,      int, const char *, int, unsigned int, void *);
DECL(int,    stat64_,    const char *, struct stat64 *);
DECL(int,    fstat64_,   int, struct stat64 *);
DECL(int,    lstat64_,   const char *, struct stat64 *);
DECL(int,    fstatat64_, int, const char *, struct stat64 *, int);
DECL(int,    access,     const char *, int);
DECL(int,    faccessat,  int, const char *, int, int);
DECL(ssize_t, readlink,  const char *, char *, size_t);
DECL(ssize_t, readlinkat, int, const char *, char *, size_t);
DECL(DIR *,  opendir,    const char *);
DECL(int,    mkdir,      const char *, mode_t);
DECL(int,    mkdirat,    int, const char *, mode_t);
DECL(int,    rmdir,      const char *);
DECL(int,    unlink,     const char *);
DECL(int,    unlinkat,   int, const char *, int);
DECL(int,    rename,     const char *, const char *);
DECL(int,    renameat,   int, const char *, int, const char *);
DECL(int,    renameat2,  int, const char *, int, const char *, unsigned int);
DECL(int,    chdir,      const char *);
DECL(char *, getcwd,     char *, size_t);
DECL(char *, realpath,   const char *, char *);
DECL(ssize_t, getxattr,     const char *, const char *, void *, size_t);
DECL(ssize_t, lgetxattr,    const char *, const char *, void *, size_t);
DECL(int,     setxattr,     const char *, const char *, const void *, size_t, int);
DECL(int,     lsetxattr,    const char *, const char *, const void *, size_t, int);
DECL(ssize_t, listxattr,    const char *, char *, size_t);
DECL(ssize_t, llistxattr,   const char *, char *, size_t);
DECL(int,     removexattr,  const char *, const char *);
DECL(int,     lremovexattr, const char *, const char *);
DECL(int,     inotify_add_watch, int, const char *, uint32_t);
DECL(long,    pathconf,     const char *, int);
DECL(int,     glob,         const char *, int, int (*)(const char *, int), glob_t *);
DECL(int,     ftw,          const char *, int (*)(const char *, const struct stat *, int), int);
DECL(int,     nftw,         const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);
DECL(int,    chmod,      const char *, mode_t);
DECL(int,    fchmodat,   int, const char *, mode_t, int);
DECL(int,    chown,      const char *, uid_t, gid_t);
DECL(int,    lchown,     const char *, uid_t, gid_t);
DECL(int,    fchownat,   int, const char *, uid_t, gid_t, int);
DECL(int,    utimensat,  int, const char *, const struct timespec *, int);
DECL(int,    symlink,    const char *, const char *);
DECL(int,    symlinkat,  const char *, int, const char *);
DECL(int,    link,       const char *, const char *);
DECL(int,    linkat,     int, const char *, int, const char *, int);
DECL(int,    execve,     const char *, char *const[], char *const[]);
DECL(int,    execvp,     const char *, char *const[]);
DECL(long,   syscall,    long, ...);
DECL(int,    mkstemp,    char *);
DECL(int,    mkstemps,   char *, int);
DECL(int,    mkostemp,   char *, int);
DECL(int,    mkostemps,  char *, int, int);
DECL(char *, mkdtemp,    char *);
DECL(int,    statvfs,    const char *, struct statvfs *);
DECL(int,    statvfs64_, const char *, struct statvfs64 *);
DECL(int,    statfs,     const char *, struct statfs *);
DECL(int,    statfs64_,  const char *, struct statfs64 *);
DECL(void *, dlopen,     const char *, int);
DECL(int,    utimes,     const char *, const struct timeval *);
DECL(int,    lutimes,    const char *, const struct timeval *);
DECL(int,    utime,      const char *, const struct utimbuf *);
DECL(int,    futimesat,  int, const char *, const struct timeval *);
DECL(int,    truncate,   const char *, off_t);
DECL(int,    mknod,      const char *, mode_t, dev_t);
DECL(int,    mknodat,    int, const char *, mode_t, dev_t);
DECL(int,    mkfifo,     const char *, mode_t);

static char g_missing[512];
static void note_missing(const char *n)
{
    size_t l = strlen(g_missing), k = strlen(n);
    if (l + k + 2 < sizeof g_missing) {
        if (l) g_missing[l++] = ' ';
        memcpy(g_missing + l, n, k + 1);
    }
}
/* dlsym returning NULL is not hypothetical: symbols come and go across glibc
 * versions (the __xstat family vanished in 2.33, stat/fstatat only appeared
 * then).  Calling a NULL pointer segfaults the guest with no message at all,
 * which is close to undebuggable -- record it and let the wrapper fall back. */
#define BIND(n) do { real_##n = dlsym(RTLD_NEXT, #n); \
                     if (!real_##n) note_missing(#n); } while (0)

/* ── initialisation ──────────────────────────────────────────────────────
 * ROOT CAUSE OF THE SIGSEGV (docs/RISKS.md R14, confirmed):
 * a preload's constructor does NOT run first.  glibc's _dl_init() walks
 * l_initfini DESCENDING, and the preload sits at index 1 (right after the main
 * map), so it initialises AFTER libc and after every other DSO.  Meanwhile all
 * 73 wrappers are already interposing.  Any other library's constructor that
 * touches a wrapped symbol -- libselinux's init_lib() does fopen("/proc/
 * filesystems"), libgcrypt's self-check opens /proc/self/maps -- jumped through
 * a still-NULL real_* pointer and died before main(), with no output.
 *
 * That is exactly why `cat`, `true` and `readlink` worked while `ls`, `find`
 * and `apt-get` did not: the first group links only libc, whose own init uses
 * hidden internal aliases (__openat_nocancel, __fstatat64_time64) that are
 * bound inside libc and are not interposable.
 *
 * __attribute__((constructor(101))) does NOT help: constructor priority only
 * orders .init_array entries WITHIN one object, and this .so has exactly one.
 * docs/04-preload-spec.md §3 claimed otherwise and was wrong.
 *
 * The fix is to stop depending on constructor order: initialise on first use.
 * The ctor is kept purely as an optimisation so the common case pays nothing. */
__attribute__((constructor(101)))
static void alr_ctor(void) { ensure_init(); }

/* Per-process totals, one line per process so a pipeline shows each stage. */
__attribute__((destructor))
static void alr_dtor(void)
{
    char b[192];
    int n;
    if (!g_count || g_log_fd < 0) return;
    n = snprintf(b, sizeof b,
                 "alr rw: total=%lu rewritten=%lu relative=%lu sysdir=%lu "
                 "underroot=%lu\n",
                 g_n_total, g_n_rewritten, g_n_rel, g_n_sysdir, g_n_underroot);
    if (n > 0) { ssize_t w = write(g_log_fd, b, (size_t)n); (void)w; }
}

static void alr_init(void)
{
    g_in_init = 1;
    const char *r = getenv("ALR_ROOT");
    const char *l;

    /* Resolve everything up front: a lazy dlsym on the first wrapper call can
     * fire during libc initialisation, and dlsym itself allocates. */
    BIND(open); BIND(openat); BIND(fopen); BIND(freopen); BIND(setmntent);
    BIND(stat); BIND(lstat); BIND(fstatat); BIND(statx);
    BIND(fstat); BIND(scandir); BIND(ttyname_r); BIND(mkfifoat);
    BIND(name_to_handle_at); BIND(dlmopen);
    real_scandir64_ = dlsym(RTLD_NEXT, "scandir64");
    real_fstat64_   = dlsym(RTLD_NEXT, "fstat64");
    real_stat64_    = dlsym(RTLD_NEXT, "stat64");
    real_lstat64_   = dlsym(RTLD_NEXT, "lstat64");
    real_fstatat64_ = dlsym(RTLD_NEXT, "fstatat64");
    if (!real_stat64_)    note_missing("stat64");
    if (!real_lstat64_)   note_missing("lstat64");
    if (!real_fstatat64_) note_missing("fstatat64");
    BIND(access); BIND(faccessat);
    BIND(readlink); BIND(readlinkat); BIND(opendir);
    BIND(mkdir); BIND(mkdirat); BIND(rmdir); BIND(unlink); BIND(unlinkat);
    BIND(rename); BIND(renameat); BIND(renameat2); BIND(chdir); BIND(getcwd); BIND(realpath);
    BIND(getxattr); BIND(lgetxattr); BIND(setxattr); BIND(lsetxattr);
    BIND(listxattr); BIND(llistxattr); BIND(removexattr); BIND(lremovexattr);
    BIND(inotify_add_watch); BIND(pathconf); BIND(glob);
    BIND(ftw); BIND(nftw);
    BIND(chmod); BIND(fchmodat); BIND(chown); BIND(lchown); BIND(fchownat);
    BIND(utimensat); BIND(symlink); BIND(symlinkat); BIND(link); BIND(linkat);
    BIND(execve); BIND(execvp); BIND(syscall);
    BIND(mkstemp); BIND(mkstemps); BIND(mkostemp); BIND(mkostemps);
    BIND(mkdtemp);
    BIND(statvfs); BIND(statfs); BIND(dlopen);
    BIND(utimes); BIND(lutimes); BIND(utime); BIND(futimesat);
    BIND(truncate); BIND(mknod); BIND(mknodat); BIND(mkfifo);
    real_statvfs64_ = dlsym(RTLD_NEXT, "statvfs64");
    real_statfs64_  = dlsym(RTLD_NEXT, "statfs64");
    if (!real_statvfs64_) note_missing("statvfs64");
    if (!real_statfs64_)  note_missing("statfs64");

    l = getenv("ALR_LOG");        g_log = l ? atoi(l) : 0;
    l = getenv("ALR_LOG_FD");     g_log_fd = l ? atoi(l) : -1;

    /* Move the log fd out of the guest program's reach.
     *
     * MEASURED 2026-08-03: `ALR_COUNT=1 alr run /bin/echo hi` printed no
     * "alr rw:" line at all, 0 times out of 10, while /bin/true printed one
     * 10 times out of 10.  Not a race -- deterministic per binary.  The cause
     * is that alr passes ALR_LOG_FD=2, and GNU coreutils registers gnulib's
     * close_stdout with atexit, which closes stdout AND STDERR.  glibc runs
     * atexit handlers before _dl_fini, so by the time this library's
     * destructor writes its totals, fd 2 is gone.  The write fails, the
     * return value was already being discarded, and the line vanishes.
     *
     * That silence is the dangerous part: a consumer summing these lines
     * would have quietly dropped every coreutils process in a pipeline and
     * reported a smaller total than the truth.  Proven by re-running the same
     * command with the log on fd 9, where /bin/echo does emit.
     *
     * So: duplicate onto a high fd nothing in a normal program touches.
     * CLOEXEC is correct rather than a leak -- each exec'd process re-runs
     * this init and re-duplicates from the inherited original.
     */
    if (g_log_fd >= 0) {
        int hi = fcntl(g_log_fd, F_DUPFD_CLOEXEC, 900);
        if (hi >= 0) g_log_fd = hi;
        /* On failure keep the original fd: degraded, not broken. */
    }
    g_guest_exe = getenv("ALR_GUEST_EXE");
    /* Every guest process is exec'd as the LOADER (ADR 0002), so the kernel
     * records "ld-linux-aarch64" as the task name and it surfaces in
     * /proc/self/status Name:, /proc/self/comm, and every ps/top/htop listing.
     * Programs that read their own name back get the loader instead of
     * themselves.  Correct it at the source rather than synthesising the files:
     * one prctl fixes all three views at once.  Best-effort -- if PR_SET_NAME
     * is denied we simply keep the old behaviour. */
    if (g_guest_exe && *g_guest_exe) {
        const char *b = strrchr(g_guest_exe, '/');
        char nm[16];
        size_t n;
        b = b ? b + 1 : g_guest_exe;
        n = strlen(b);
        if (n > sizeof nm - 1) n = sizeof nm - 1;   /* kernel caps at 15+NUL */
        memcpy(nm, b, n); nm[n] = '\0';
        (void)syscall(SYS_prctl, 15 /*PR_SET_NAME*/, (unsigned long)(uintptr_t)nm,
                      0UL, 0UL, 0UL);
    }
    g_ldso      = getenv("ALR_LDSO");
    g_libpath   = getenv("ALR_LIBPATH");
    g_preload   = getenv("ALR_PRELOAD");
    g_resolv_sock = getenv(ALR_RESOLV_ENV);
    { const char *f = getenv("ALR_FAKEROOT"); g_fakeroot = f && *f == '1'; }
    { const char *c = getenv("ALR_COUNT");    g_count    = c && *c == '1'; }

    if (r && r[0] == '/') {
        size_t n = strlen(r);
        if (n < sizeof g_root) {
            memcpy(g_root, r, n + 1);
            g_root_len = alr_trim_root(g_root);
        }
    }
    /* No ALR_ROOT (or a bad one) => passthrough mode.  Silently mangling
     * paths would be far worse than doing nothing. */
    lg("alr preload: root=%s len=%zu\n", g_root_len ? g_root : "(passthrough)",
       g_root_len);
    if (g_missing[0]) lg("alr preload: UNBOUND: %s\n", g_missing);
    g_ready = 1;
    g_in_init = 0;
}

/* ── /proc virtualization ───────────────────────────────────────────── */

/* Node's process.execPath is uv_exepath() is readlink("/proc/self/exe").
 * Uncorrected it returns the LOADER path and npm/npx, which respawn through
 * process.execPath, break in a way that looks like a Node bug. */
static int is_self_exe(const char *p)
{
    if (!p) return 0;
    if (!strcmp(p, "/proc/self/exe")) return 1;
    if (!strcmp(p, "/proc/thread-self/exe")) return 1;
    if (!strncmp(p, "/proc/", 6)) {
        const char *q = p + 6;
        while (*q >= '0' && *q <= '9') q++;
        if (q != p + 6 && !strcmp(q, "/exe")) return 1;
    }
    return 0;
}

static void lid_patch(struct stat *st);   /* defined with the link fallback */
/* On aarch64 struct stat and struct stat64 have identical layout, so the same
 * patch serves both.  The *64 names are what a program built with
 * _FILE_OFFSET_BITS=64 actually binds -- git does, which is why patching only
 * the non-64 names left `git clone --local` still failing. */
#define LID_PATCH64(p) lid_patch((struct stat *)(void *)(p))

/* ── wrappers ───────────────────────────────────────────────────────── */

/* alr_rw() returns NULL for TWO different reasons: the input was NULL (legal
 * for utimensat/faccessat, where it means "operate on the dirfd itself"), or
 * the rewrite overflowed.  Only the second is an error -- conflating them made
 * futimens-style utimensat(fd, NULL, ...) fail, which is how dpkg sets
 * timestamps, and it surfaced as an unexplained
 * "error setting timestamps of '/usr/bin/perl.dpkg-new'". */
#define P(name) char _b[ALR_PBUF]; const char *_p = rw(name, _b, sizeof _b); \
                if (!_p && (name)) { errno = ENAMETOOLONG; return -1; }
#define PN(name) char _b[ALR_PBUF]; const char *_p = rw(name, _b, sizeof _b); \
                 if (!_p && (name)) { errno = ENAMETOOLONG; return NULL; }
/* Never call through a NULL real_ pointer: fail the call cleanly instead of
 * killing the guest with an unexplained SIGSEGV. */
#define NEED(f)  do { ensure_init(); if (!real_##f) { errno = ENOSYS; return -1; } } while (0)
#define NEEDN(f) do { ensure_init(); if (!real_##f) { errno = ENOSYS; return NULL; } } while (0)

/* ---- synthetic /proc files (docs/04-preload-spec.md §7) -----------------
 *
 * /proc is a sysdir, so rw() passes it through untouched and the guest reads
 * ANDROID's mount table: /apex, /vendor, the erofs system image, and the host
 * path the rootfs is unpacked into. That is an information leak, and it is
 * also simply wrong -- df, mount, findmnt and container detectors parse it and
 * see a filesystem layout the guest does not live in.
 *
 * Answer with a table describing the GUEST's view. §7 requires that no host
 * path appear in it; synth_leaks() enforces that at runtime so a later edit
 * cannot quietly reintroduce one. */
static const char SYNTH_MOUNTS[] =
    "/dev/root / ext4 rw,relatime 0 0\n"
    "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
    "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
    "devtmpfs /dev devtmpfs rw,nosuid,relatime 0 0\n"
    "devpts /dev/pts devpts rw,nosuid,noexec,relatime 0 0\n"
    "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n";

static const char SYNTH_MOUNTINFO[] =
    "15 0 0:1 / / rw,relatime - ext4 /dev/root rw\n"
    "16 15 0:2 / /proc rw,nosuid,nodev,noexec,relatime - proc proc rw\n"
    "17 15 0:3 / /sys rw,nosuid,nodev,noexec,relatime - sysfs sysfs rw\n"
    "18 15 0:4 / /dev rw,nosuid,relatime - devtmpfs devtmpfs rw\n"
    "19 18 0:5 / /dev/pts rw,nosuid,noexec,relatime - devpts devpts rw\n"
    "20 15 0:6 / /tmp rw,nosuid,nodev - tmpfs tmpfs rw\n";

static int synth_leaks(const char *s)
{
    static const char *const bad[] = { "/data/", "/system", "/vendor", "/apex",
                                       "/storage", "/mnt/", NULL };
    int i;
    for (i = 0; bad[i]; i++) if (strstr(s, bad[i])) return 1;
    return (g_root_len && strstr(s, g_root)) ? 1 : 0;
}

/* Android denies /proc/stat to app UIDs (measured: EACCES), so libuv's
 * uv_cpu_info() finds no `cpuN` lines and Node's os.cpus() returns [] -- code
 * that sizes a worker pool from os.cpus().length gets zero.
 *
 * The COUNT is genuinely available (sched_getaffinity works: nproc and
 * getconf _NPROCESSORS_ONLN both report 8), so reporting it is real
 * information, not invention.  The per-CPU TIME fields are not available at
 * all and are emitted as zeros.
 *
 * >>> Anything computing CPU UTILISATION from these deltas will read a
 * >>> constant 0%.  That is a documented limitation (docs/RISKS.md), not a
 * >>> measurement.  /proc/cpuinfo is left alone -- it IS readable, and
 * >>> replacing real SoC data with a fabrication would regress lscpu. */
#define SYNTH_STAT_MAXCPU 128
static char g_statbuf[SYNTH_STAT_MAXCPU * 48 + 256];

static const char *synth_stat(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    int i, o = 0;
    if (n < 1) n = 1;
    if (n > SYNTH_STAT_MAXCPU) n = SYNTH_STAT_MAXCPU;
    o += snprintf(g_statbuf + o, sizeof g_statbuf - (size_t)o,
                  "cpu  0 0 0 0 0 0 0 0 0 0\n");
    for (i = 0; i < n; i++)
        o += snprintf(g_statbuf + o, sizeof g_statbuf - (size_t)o,
                      "cpu%d 0 0 0 0 0 0 0 0 0 0\n", i);
    snprintf(g_statbuf + o, sizeof g_statbuf - (size_t)o,
             "intr 0\nctxt 0\nbtime 0\nprocesses 0\n"
             "procs_running 1\nprocs_blocked 0\nsoftirq 0\n");
    return g_statbuf;
}

/* /proc/self/cmdline exposes the ENTIRE loader invocation:
 *
 *   .../ld-linux-aarch64.so.1\0--library-path\0/data/data/com.termux/...\0
 *   --inhibit-cache\0--argv0\0/bin/echo\0--preload\0...\0<R>/bin/echo\0
 *
 * docs/04-preload-spec.md §7 requires synthesising it and it never was.  Two
 * consequences, one of them a hard boot failure:
 *
 *  - Ubuntu 26.04 ships uutils coreutils as a multicall binary that resolves
 *    its own name from here, not from argv[0].  It read the loader and died
 *    with "coreutils: unknown program 'ld-linux-aarch64.so'" -- Rust's
 *    file_stem() having eaten the ".1".  EVERY coreutils tool was unusable.
 *  - Every host path in the invocation leaks to anything reading cmdline.
 *
 * Rebuild the guest's own argv by walking off the loader options we emit.  No
 * new environment variable is needed: --argv0 already carries argv[0], and the
 * guest's arguments are whatever follows the program path.  A process not
 * started through our loader (a static binary, or the CLI itself) has a
 * correct cmdline already and is passed through untouched. */
static char g_cmdbuf[8192];
static size_t g_cmdlen;      /* NUL-separated: strlen() would lie about it */

static int synth_is_ldso(const char *a)
{
    const char *b = a ? strrchr(a, '/') : NULL;
    b = b ? b + 1 : a;
    return b && strncmp(b, "ld-linux", 8) == 0;
}

static const char *synth_cmdline(void)
{
    char raw[8192];
    const char *argv[256];
    const char *argv0 = NULL;
    ssize_t n;
    size_t i, o = 0;
    int fd, k, argc = 0, prog = -1;

    if (!real_open) return NULL;
    if ((fd = real_open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC)) < 0) return NULL;
    n = read(fd, raw, sizeof raw - 1);
    close(fd);
    if (n <= 0) return NULL;
    raw[n] = '\0';

    /* Split on NUL.  The kernel NUL-terminates every entry including the last. */
    for (i = 0; (ssize_t)i < n && argc < (int)(sizeof argv / sizeof *argv); ) {
        argv[argc++] = raw + i;
        i += strlen(raw + i) + 1;
    }
    if (argc == 0 || !synth_is_ldso(argv[0])) return NULL;   /* not ours */

    for (k = 1; k < argc; ) {
        if (!strcmp(argv[k], "--library-path") || !strcmp(argv[k], "--preload")) k += 2;
        else if (!strcmp(argv[k], "--argv0")) { if (k + 1 < argc) argv0 = argv[k + 1]; k += 2; }
        else if (!strcmp(argv[k], "--inhibit-cache")) k += 1;
        else { prog = k; break; }
    }
    if (prog < 0) return NULL;
    if (!argv0) argv0 = g_guest_exe ? g_guest_exe : argv[prog];

    /* argv0, then everything AFTER the program path, NUL separated. */
    { size_t l = strlen(argv0);
      if (l + 1 > sizeof g_cmdbuf) return NULL;
      memcpy(g_cmdbuf, argv0, l + 1); o = l + 1; }
    for (k = prog + 1; k < argc; k++) {
        size_t l = strlen(argv[k]);
        if (o + l + 1 > sizeof g_cmdbuf) break;
        memcpy(g_cmdbuf + o, argv[k], l + 1);
        o += l + 1;
    }
    g_cmdlen = o;
    return g_cmdbuf;
}

#ifdef ALR_TRACE_AUXV
#include <sys/auxv.h>
#include <link.h>

/* dl_iterate_phdr is the last untested channel: uutils imports it, and its
 * NEEDED list carries ld-linux-aarch64.so.1 itself, so the loader appears in
 * the link map as an ordinary object. */
static int trace_cb(struct dl_phdr_info *i, size_t sz, void *d)
{
    (void)sz;
    lg("alr trace phdr: name=%s\n", (i->dlpi_name && *i->dlpi_name)
       ? i->dlpi_name : "(main/empty)");
    return ((int (*)(struct dl_phdr_info *, size_t, void *))
            ((void **)d)[0])(i, sz, ((void **)d)[1]);
}

int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    static int (*real_dip)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    void *pair[2];
    if (!real_dip) real_dip = dlsym(RTLD_NEXT, "dl_iterate_phdr");
    if (!real_dip) { errno = ENOSYS; return -1; }
    lg("alr trace phdr: dl_iterate_phdr called\n");
    pair[0] = (void *)cb; pair[1] = data;
    return real_dip(trace_cb, pair);
}

/* What argv does the failing process ACTUALLY receive?  Everything else has
 * been excluded by measurement (no /proc access, auxv only HWCAP), so this is
 * the remaining variable. */
__attribute__((constructor))
static void alr_trace_argv(int argc, char **argv, char **envp)
{
    (void)envp;
    lg("alr trace argv: argc=%d argv[0]=%s argv[1]=%s\n", argc,
       (argc > 0 && argv[0]) ? argv[0] : "(none)",
       (argc > 1 && argv[1]) ? argv[1] : "(none)");
}
unsigned long getauxval(unsigned long t)
{
    static unsigned long (*real_gav)(unsigned long);
    unsigned long v;
    if (!real_gav) real_gav = dlsym(RTLD_NEXT, "getauxval");
    if (!real_gav) return 0;
    v = real_gav(t);
    if (t == AT_EXECFN) lg("alr trace auxv: AT_EXECFN=%s\n", v ? (const char *)v : "(0)");
    else                lg("alr trace auxv: type=%lu val=%lu\n", t, v);
    return v;
}
#endif

static const char *synth_for(const char *p)
{
    if (!p || strncmp(p, "/proc/", 6) != 0) return NULL;
    p += 6;
    if      (strncmp(p, "self/", 5) == 0)        p += 5;
    else if (strncmp(p, "thread-self/", 12) == 0) p += 12;
    if (strcmp(p, "mounts") == 0)    return SYNTH_MOUNTS;
    if (strcmp(p, "mountinfo") == 0) return SYNTH_MOUNTINFO;
    if (strcmp(p, "cmdline") == 0)   return synth_cmdline();
    /* Only synthesise /proc/stat when the real one is genuinely unreachable;
     * on a device that allows it, the real numbers are strictly better. */
    if (strcmp(p, "stat") == 0 && real_access &&
        real_access("/proc/stat", R_OK) != 0) return synth_stat();
    return NULL;
}

/* Materialise into an unlinked file in the guest's own /tmp. A memfd would be
 * tidier but memfd_create's fate under the app seccomp filter is not
 * established, and this path degrades gracefully: on any failure we return -1
 * and the caller falls through to the real (leaky) /proc file rather than
 * failing the open. */
static int synth_fd(const char *content)
{
    char tmpl[64], b[ALR_PBUF];
    const char *p;
    size_t n = (content == g_cmdbuf) ? g_cmdlen : strlen(content);
    int fd;

    if (synth_leaks(content)) { lg("alr synth: refused, host path in table\n"); return -1; }
    if (!real_open) return -1;
    snprintf(tmpl, sizeof tmpl, "/tmp/.alr-proc-%d", (int)getpid());
    if (!(p = rw(tmpl, b, sizeof b))) return -1;
    if ((fd = real_open(p, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0) return -1;
    if (real_unlink) real_unlink(p);
    if (write(fd, content, n) != (ssize_t)n) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

int open(const char *path, int flags, ...)
{
    mode_t m = 0;
    const char *s;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); m = (mode_t)va_arg(ap, int); va_end(ap); }
    if ((flags & O_ACCMODE) == O_RDONLY && (s = synth_for(path))) {
        int fd; ensure_init();
        if ((fd = synth_fd(s)) >= 0) return fd;
    }
#ifdef ALR_TRACE_AUXV
    if (path && strncmp(path, "/proc/", 6) == 0) lg("alr trace open: %s\n", path);
#endif
    { P(path); NEED(open); return real_open(_p, flags, m); }
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));

int openat(int dfd, const char *path, int flags, ...)
{
    mode_t m = 0;
    const char *s;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); m = (mode_t)va_arg(ap, int); va_end(ap); }
    if ((flags & O_ACCMODE) == O_RDONLY && (s = synth_for(path))) {
        int fd; ensure_init();
        if ((fd = synth_fd(s)) >= 0) return fd;
    }
#ifdef ALR_TRACE_AUXV
    if (path && strncmp(path, "/proc/", 6) == 0) lg("alr trace openat: %s\n", path);
#endif
    { P(path); NEED(openat); return real_openat(dfd, _p, flags, m); }
}
int openat64(int dfd, const char *path, int flags, ...) __attribute__((alias("openat")));

int __open_2(const char *path, int flags) { P(path); NEED(open); return real_open(_p, flags); }
int __open64_2(const char *path, int flags) __attribute__((alias("__open_2")));
int __openat_2(int dfd, const char *path, int flags)
{ P(path); NEED(openat); return real_openat(dfd, _p, flags); }
int __openat64_2(int dfd, const char *path, int flags)
    __attribute__((alias("__openat_2")));

int creat(const char *path, mode_t m) { P(path); NEED(open); return real_open(_p, O_CREAT|O_WRONLY|O_TRUNC, m); }
int creat64(const char *path, mode_t m) __attribute__((alias("creat")));

static FILE *synth_stream(const char *path, const char *mode)
{
    const char *s;
    int fd;
    if (!mode || mode[0] != 'r') return NULL;
    if (!(s = synth_for(path))) return NULL;
    ensure_init();
    if ((fd = synth_fd(s)) < 0) return NULL;
    { FILE *f = fdopen(fd, "r"); if (f) return f; close(fd); return NULL; }
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *f = synth_stream(path, mode);
    if (f) return f;
    { PN(path); NEEDN(fopen); return real_fopen(_p, mode); }
}

/* df and getmntent() reach /proc/mounts through setmntent(), and glibc's
 * setmntent calls fopen through an INTERNAL alias -- our fopen above never
 * sees it. setmntent itself is public, so take it over too. */
FILE *setmntent(const char *path, const char *mode)
{
    FILE *f = synth_stream(path, mode);
    if (f) return f;
    { PN(path); NEEDN(setmntent); return real_setmntent(_p, mode); }
}
FILE *fopen64(const char *path, const char *mode) __attribute__((alias("fopen")));
FILE *freopen(const char *path, const char *mode, FILE *f)
{ PN(path); NEEDN(freopen); return real_freopen(_p, mode, f); }
FILE *freopen64(const char *p, const char *m, FILE *f) __attribute__((alias("freopen")));

int stat(const char *path, struct stat *st)
{ P(path); NEED(stat);
  { int r = real_stat(_p, st); if (r == 0) lid_patch(st); return r; } }
int stat64(const char *path, struct stat64 *st)
{ P(path); NEED(stat64_);
  { int r = real_stat64_(_p, st); if (r == 0) LID_PATCH64(st); return r; } }
int lstat(const char *path, struct stat *st)
{ P(path); NEED(lstat);
  { int r = real_lstat(_p, st); if (r == 0) lid_patch(st); return r; } }
int lstat64(const char *path, struct stat64 *st)
{ P(path); NEED(lstat64_);
  { int r = real_lstat64_(_p, st); if (r == 0) LID_PATCH64(st); return r; } }
int fstatat(int dfd, const char *path, struct stat *st, int f)
{ P(path); NEED(fstatat);
  { int r = real_fstatat(dfd, _p, st, f); if (r == 0) lid_patch(st); return r; } }
int fstatat64(int d, const char *path, struct stat64 *st, int f)
{ P(path); NEED(fstatat64_);
  { int r = real_fstatat64_(d, _p, st, f); if (r == 0) LID_PATCH64(st); return r; } }
/* No path argument, so nothing to rewrite -- but MANDATORY for the link
 * identity table (docs/04-preload-spec.md §6.2/§8.2).  Without them
 * stat(path) reports st_nlink=2 for a copy-fallback "hardlink" while
 * fstat(fd) on the same file reports 1, and dpkg's integrity check breaks on
 * the disagreement.  Ubuntu 24.04 (glibc 2.39) binaries call fstat, not
 * __fxstat, so the pre-2.33 names alone do not catch them. */
#ifndef ALR_NO_FSTAT   /* bisect control: see docs/evidence M13 */
int fstat(int fd, struct stat *st)
{ ensure_init(); if (!real_fstat) { errno = ENOSYS; return -1; }
  { int r = real_fstat(fd, st); if (r == 0) lid_patch(st); return r; } }
int fstat64(int fd, struct stat64 *st)
{ ensure_init(); if (!real_fstat64_) { errno = ENOSYS; return -1; }
  { int r = real_fstat64_(fd, st); if (r == 0) LID_PATCH64(st); return r; } }

/* pre-2.33 vtable forms.  Defining them costs nothing at the 2.17 floor and
 * catches guest binaries built against older distros. */
int __fxstat(int ver, int fd, struct stat *st)
{ (void)ver; return fstat(fd, st); }
int __fxstat64(int ver, int fd, struct stat64 *st)
{ (void)ver; return fstat64(fd, st); }

/* newfstatat is the raw kernel name; some binaries bind it directly. */
int newfstatat(int dfd, const char *path, struct stat *st, int f)
{ P(path); NEED(fstatat);
  { int r = real_fstatat(dfd, _p, st, f); if (r == 0) lid_patch(st); return r; } }
#endif /* ALR_NO_FSTAT */

int statx(int dfd, const char *path, int f, unsigned int m, void *b)
{ P(path); NEED(statx); return real_statx(dfd, _p, f, m, b); }

/* pre-2.33 names: guests built on older distros bind these instead. */
int __xstat(int v, const char *path, struct stat *st)
{ (void)v; P(path); NEED(stat);
  { int r = real_stat(_p, st); if (r == 0) lid_patch(st); return r; } }
int __lxstat(int v, const char *path, struct stat *st)
{ (void)v; P(path); NEED(lstat);
  { int r = real_lstat(_p, st); if (r == 0) lid_patch(st); return r; } }
int __fxstatat(int v, int dfd, const char *path, struct stat *st, int f)
{ (void)v; P(path); NEED(fstatat); return real_fstatat(dfd, _p, st, f); }
int __xstat64(int v, const char *path, struct stat64 *st)
{ (void)v; P(path); NEED(stat64_); return real_stat64_(_p, st); }
int __lxstat64(int v, const char *path, struct stat64 *st)
{ (void)v; P(path); NEED(lstat64_); return real_lstat64_(_p, st); }
int __fxstatat64(int v, int d, const char *path, struct stat64 *st, int f)
{ (void)v; P(path); NEED(fstatat64_); return real_fstatat64_(d, _p, st, f); }

int access(const char *path, int m)   { P(path); NEED(access); return real_access(_p, m); }
int euidaccess(const char *p, int m)  __attribute__((alias("access")));
int eaccess(const char *p, int m)     __attribute__((alias("access")));
int faccessat(int dfd, const char *path, int m, int f)
{ P(path); NEED(faccessat); return real_faccessat(dfd, _p, m, f); }

ssize_t readlink(const char *path, char *buf, size_t sz)
{
#ifdef ALR_TRACE_AUXV
    if (path && strncmp(path, "/proc/", 6) == 0) lg("alr trace readlink: %s\n", path);
#endif
    if (is_self_exe(path) && g_guest_exe) {
        size_t n = strlen(g_guest_exe);
        if (n > sz) n = sz;
        memcpy(buf, g_guest_exe, n);
        return (ssize_t)n;
    }
    { P(path);
      if (!real_readlink) { errno = ENOSYS; return -1; }
      { ssize_t r = real_readlink(_p, buf, sz);
        /* A rootfs-internal absolute symlink target must come back as a guest
         * path, or the guest re-prefixes it into nonsense on the next open. */
        if (r > 0 && (size_t)r < sz && (size_t)r < ALR_PBUF && buf[0] == '/') {
            char g[ALR_PBUF], tmp[ALR_PBUF];
            const char *c;
            memcpy(tmp, buf, (size_t)r); tmp[r] = '\0';
            c = alr_guest_canon(tmp, g_root, g_root_len, g, sizeof g);
            if (c != tmp) { size_t n = strlen(c); if (n <= sz) { memcpy(buf, c, n); return (ssize_t)n; } }
        }
        return r; } }
}
ssize_t readlinkat(int dfd, const char *path, char *buf, size_t sz)
{
    if (is_self_exe(path) && g_guest_exe) return readlink(path, buf, sz);
    { P(path); NEED(readlinkat); return real_readlinkat(dfd, _p, buf, sz); }
}
ssize_t __readlink_chk(const char *p, char *b, size_t sz, size_t blen)
{ if (sz > blen) { errno = EINVAL; return -1; } return readlink(p, b, sz); }

DIR *opendir(const char *path) { PN(path); NEEDN(opendir); return real_opendir(_p); }

int mkdir(const char *path, mode_t m) { P(path); NEED(mkdir); return real_mkdir(_p, m); }
int mkdirat(int d, const char *path, mode_t m) { P(path); NEED(mkdirat); return real_mkdirat(d, _p, m); }
int rmdir(const char *path)   { P(path); NEED(rmdir); return real_rmdir(_p); }
int unlink(const char *path)  { P(path); NEED(unlink); return real_unlink(_p); }
int unlinkat(int d, const char *path, int f) { P(path); NEED(unlinkat); return real_unlinkat(d, _p, f); }
int remove(const char *path)
{ struct stat st; P(path); NEED(lstat); NEED(unlink); NEED(rmdir);
  if (real_lstat(_p, &st) == 0 && S_ISDIR(st.st_mode)) return real_rmdir(_p);
  return real_unlink(_p); }

/* ── the rest of the path-taking surface ─────────────────────────────────
 *
 * Everything below was missing until 2026-08-03 and was found by
 * scripts/check-path-coverage.sh, which diffs an inventory of path-taking libc
 * entry points against what this file exports.  The gate exists because
 * bind()/connect() had been missing for the life of the project (docs/01 §A6)
 * and nothing would ever have pointed at them; "we interposed the ones we
 * thought of" is not a coverage argument.
 *
 * MEASURED before, by tests/device/probe_pathcov.c, all against paths that do
 * not exist on Android:
 *     xattr=FAILED inotify=FAILED pathconf=FAILED getsockname=FAILED
 *
 * xattr:    `ls -l` printed "drwx------?" in the guest.  coreutils prints '?'
 *           when the ACL probe ERRORS rather than when there is no ACL, and
 *           getxattr on an unrewritten path is a bare ENOENT.  The same calls
 *           sit under cp -a, tar --xattrs, rsync -X, getfacl and install -Z.
 * inotify:  every file watcher -- node's fs.watch and chokidar (so every JS
 *           dev server), inotifywait, entr, watchman.  Watching the wrong path
 *           does not error; it silently never fires, which is worse.
 * pathconf: configure scripts, and glibc's own readdir buffer sizing. */
ssize_t getxattr(const char *path, const char *n, void *v, size_t s)
{ P(path); NEED(getxattr); return real_getxattr(_p, n, v, s); }
ssize_t lgetxattr(const char *path, const char *n, void *v, size_t s)
{ P(path); NEED(lgetxattr); return real_lgetxattr(_p, n, v, s); }
int setxattr(const char *path, const char *n, const void *v, size_t s, int f)
{ P(path); NEED(setxattr); return real_setxattr(_p, n, v, s, f); }
int lsetxattr(const char *path, const char *n, const void *v, size_t s, int f)
{ P(path); NEED(lsetxattr); return real_lsetxattr(_p, n, v, s, f); }
ssize_t listxattr(const char *path, char *l, size_t s)
{ P(path); NEED(listxattr); return real_listxattr(_p, l, s); }
ssize_t llistxattr(const char *path, char *l, size_t s)
{ P(path); NEED(llistxattr); return real_llistxattr(_p, l, s); }
int removexattr(const char *path, const char *n)
{ P(path); NEED(removexattr); return real_removexattr(_p, n); }
int lremovexattr(const char *path, const char *n)
{ P(path); NEED(lremovexattr); return real_lremovexattr(_p, n); }

/* glob(3) is the one entry point here that has to be corrected on BOTH sides.
 *
 * MEASURED 2026-08-03: glob("/etc/os-relea*") returned GLOB_NOMATCH in the
 * guest.  Not because the pattern is exotic -- because glibc walks it with its
 * OWN internal opendir/lstat, called directly rather than through the PLT, so
 * LD_PRELOAD never sees them.  It searched Android's /etc.  This is the one
 * structural hole in the interposer model, and the fix is always the same:
 * catch the public entry point, since that one IS a PLT call.
 *
 * Rewriting the pattern alone is not enough.  glob() composes each result from
 * the literal prefix of the pattern plus the directory entry, so rewriting the
 * pattern makes it return <R>-prefixed paths -- correct matches at addresses
 * the guest cannot name.  Every result therefore goes back through
 * alr_guest_canon, which also covers GLOB_NOCHECK (returns the pattern itself
 * when nothing matched) and GLOB_APPEND (already-guest entries are left alone,
 * so re-running over them is idempotent).
 *
 * GLOB_ALTDIRFUNC is passed through untouched: the caller supplied its own
 * opendir/lstat, so the paths glibc builds go to THEIR code, not the kernel,
 * and rewriting would hand them a host path they never asked for.
 *
 * KNOWN LIMIT: GLOB_TILDE.  glibc expands `~` internally after we have already
 * rewritten, then walks the expansion with the same internal calls.  A
 * `~/...` pattern therefore still resolves against Android.  Not worth chasing
 * -- the shells that use tilde expansion expand it themselves before calling. */
static void glob_unrewrite(glob_t *g, int flags)
{
    char **v, b[ALR_PBUF];
    size_t skip = (flags & GLOB_DOOFFS) ? g->gl_offs : 0;

    if (!g->gl_pathv) return;
    for (v = g->gl_pathv + skip; *v; v++) {
        const char *c = alr_guest_canon(*v, g_root, g_root_len, b, sizeof b);
        /* guest_canon only strips the prefix, so this never grows the string
         * and the allocation globfree() will free is unchanged. */
        if (c != *v) memmove(*v, c, strlen(c) + 1);
    }
}

int glob(const char *pat, int flags, int (*errfunc)(const char *, int),
         glob_t *pg)
{
    char b[ALR_PBUF];
    const char *p;
    int r;

    ensure_init();
    if (!real_glob) { errno = ENOSYS; return GLOB_ABORTED; }
    if (flags & GLOB_ALTDIRFUNC) return real_glob(pat, flags, errfunc, pg);
    p = rw(pat, b, sizeof b);
    if (!p && pat) { errno = ENAMETOOLONG; return GLOB_ABORTED; }
    r = real_glob(p, flags, errfunc, pg);
    /* Only on success: on GLOB_NOMATCH without GLOB_APPEND glibc does not
     * promise gl_pathv is initialised, and walking it would be a read of
     * whatever the caller's stack held. */
    if (r == 0) glob_unrewrite(pg, flags);
    return r;
}

/* Same function on LP64 -- glob64_t and glob_t are layout-identical and glibc
 * itself makes one a strong alias of the other.  Defined so a guest binary
 * that imports glob64@GLIBC_2.17 (anything built with _FILE_OFFSET_BITS=64,
 * which is most of Ubuntu) does not slip past to libc's copy. */
int glob64(const char *pat, int flags, int (*errfunc)(const char *, int),
           glob64_t *pg)
{ return glob(pat, flags, errfunc, (glob_t *)pg); }

/* ftw/nftw are glob's problem again -- glibc walks with its own internal
 * opendir/lstat -- with one extra twist: the CALLBACK is handed each path, so
 * rewriting the root without correcting the callback argument would hand the
 * guest a stream of <R>-prefixed paths.
 *
 * MEASURED 2026-08-03: nftw("/usr/lib/os-release", ...) returned -1 in the
 * guest before this existed.
 *
 * Note this is NOT how coreutils walks.  du, rm -r, chmod -R, cp -r and find
 * carry gnulib's fts, which calls openat/opendir through the PLT and has
 * therefore always worked -- MEASURED: `du -sh /etc` reports the guest's 2.2M,
 * and a mkdir -p / chmod -R / cp -r / rm -rf round trip is clean.  So the
 * blast radius here is smaller than it looks; it is the third-party users of
 * glibc's own walker.
 *
 * The saved callback is thread-local and saved/restored around the real call,
 * which makes a callback that itself calls nftw() work: the inner call's
 * restore hands the outer one its own callback back. */
static __thread int (*g_ftw_cb)(const char *, const struct stat *, int);
static __thread int (*g_nftw_cb)(const char *, const struct stat *, int,
                                 struct FTW *);

static int ftw_tramp(const char *f, const struct stat *s, int tf)
{
    char b[ALR_PBUF];
    return g_ftw_cb(alr_guest_canon(f, g_root, g_root_len, b, sizeof b), s, tf);
}

static int nftw_tramp(const char *f, const struct stat *s, int tf,
                      struct FTW *w)
{
    char b[ALR_PBUF];
    const char *c = alr_guest_canon(f, g_root, g_root_len, b, sizeof b);
    struct FTW w2 = *w;
    if (c != f) {
        /* FTW.base is a byte offset OF THE FILENAME WITHIN fpath, so stripping
         * the prefix has to move it by the same amount or the callback indexes
         * into the middle of a component. */
        w2.base -= (int)(strlen(f) - strlen(c));
        if (w2.base < 0) w2.base = 0;
    }
    return g_nftw_cb(c, s, tf, &w2);
}

int ftw(const char *dir, int (*fn)(const char *, const struct stat *, int),
        int nfd)
{
    char b[ALR_PBUF];
    const char *p;
    int r, (*save)(const char *, const struct stat *, int) = g_ftw_cb;

    ensure_init();
    if (!real_ftw) { errno = ENOSYS; return -1; }
    p = rw(dir, b, sizeof b);
    if (!p && dir) { errno = ENAMETOOLONG; return -1; }
    /* Unrewritten (a sysdir, or already host-form): the callback would see the
     * right paths anyway, so skip the trampoline entirely. */
    if (p == dir) return real_ftw(dir, fn, nfd);
    g_ftw_cb = fn;
    r = real_ftw(p, ftw_tramp, nfd);
    g_ftw_cb = save;
    return r;
}

int nftw(const char *dir,
         int (*fn)(const char *, const struct stat *, int, struct FTW *),
         int nfd, int flags)
{
    char b[ALR_PBUF];
    const char *p;
    int r, (*save)(const char *, const struct stat *, int, struct FTW *) = g_nftw_cb;

    ensure_init();
    if (!real_nftw) { errno = ENOSYS; return -1; }
    p = rw(dir, b, sizeof b);
    if (!p && dir) { errno = ENAMETOOLONG; return -1; }
    if (p == dir) return real_nftw(dir, fn, nfd, flags);
    g_nftw_cb = fn;
    r = real_nftw(p, nftw_tramp, nfd, flags);
    g_nftw_cb = save;
    return r;
}

/* struct stat64 == struct stat on LP64, and glibc aliases these itself. */
int ftw64(const char *dir, int (*fn)(const char *, const struct stat64 *, int),
          int nfd)
{ return ftw(dir, (int (*)(const char *, const struct stat *, int))fn, nfd); }

int nftw64(const char *dir,
           int (*fn)(const char *, const struct stat64 *, int, struct FTW *),
           int nfd, int flags)
{ return nftw(dir,
              (int (*)(const char *, const struct stat *, int, struct FTW *))fn,
              nfd, flags); }

int inotify_add_watch(int fd, const char *path, uint32_t mask)
{ P(path); NEED(inotify_add_watch); return real_inotify_add_watch(fd, _p, mask); }

long pathconf(const char *path, int name)
{
    char _b[ALR_PBUF];
    const char *_p = rw(path, _b, sizeof _b);
    if (!_p && path) { errno = ENAMETOOLONG; return -1; }
    ensure_init();
    if (!real_pathconf) { errno = ENOSYS; return -1; }
    return real_pathconf(_p, name);
}

int chmod(const char *path, mode_t m) { P(path); NEED(chmod); return real_chmod(_p, m); }
int fchmodat(int d, const char *path, mode_t m, int f)
{ P(path); NEED(fchmodat); return real_fchmodat(d, _p, m, f); }
int chown(const char *path, uid_t u, gid_t g)
{ if (g_fakeroot) return 0; { P(path); NEED(chown); return real_chown(_p, u, g); } }
int lchown(const char *path, uid_t u, gid_t g)
{ if (g_fakeroot) return 0; { P(path); NEED(lchown); return real_lchown(_p, u, g); } }
int fchownat(int d, const char *path, uid_t u, gid_t g, int f)
{ if (g_fakeroot) return 0; { P(path); NEED(fchownat); return real_fchownat(d, _p, u, g, f); } }
int utimensat(int d, const char *path, const struct timespec t[2], int f)
{ P(path); NEED(utimensat); return real_utimensat(d, _p, t, f); }

/* Two-path calls need two buffers; both sides must be rewritten or dpkg's
 * atomic status->status-old replacement lands outside the rootfs. */
int rename(const char *a, const char *b)
{
    char ba[ALR_PBUF], bb[ALR_PBUF];
    const char *pa = rw(a, ba, sizeof ba), *pb = rw(b, bb, sizeof bb);
    if ((!pa && a) || (!pb && b)) { errno = ENAMETOOLONG; return -1; }
    NEED(rename);
    return real_rename(pa, pb);
}
int renameat(int da, const char *a, int db, const char *b)
{
    char ba[ALR_PBUF], bb[ALR_PBUF];
    const char *pa = rw(a, ba, sizeof ba), *pb = rw(b, bb, sizeof bb);
    if ((!pa && a) || (!pb && b)) { errno = ENAMETOOLONG; return -1; }
    NEED(renameat);
    return real_renameat(da, pa, db, pb);
}
/* Android app-private storage refuses link(2) with EACCES (SELinux; measured
 * by alr doctor P6).  dpkg needs it for the atomic status -> status-old
 * backup, git clone --local for object dedup, pnpm for its whole store.
 *
 * We fall back to a COPY.  That is not equivalent -- st_nlink stays 1 and
 * later edits do not propagate -- but the full shadow-file scheme (rename the
 * original aside, replace both names with symlinks, track a link count) needs
 * coherent interposition of lstat/fstat/readlink/readdir/scandir/unlink/rename
 * as well, all to preserve nlink semantics that dpkg, git and pnpm do not
 * actually depend on: they use hardlinks for atomicity and disk savings, not
 * for shared mutation.  The complexity is not worth the fidelity here.
 * Recorded as a deliberate limitation, not an oversight (docs/adr/0004). */
static int copy_at(int dfrom, const char *from, int dto, const char *to)
{
    char buf[65536];
    int in, out;
    ssize_t n;
    struct stat st;

    if (!real_openat) { errno = ENOSYS; return -1; }
    in = real_openat(dfrom, from, O_RDONLY);
    if (in < 0) return -1;
    if (fstat(in, &st) != 0) { close(in); return -1; }
    out = real_openat(dto, to, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in); close(out);
    return n < 0 ? -1 : 0;
}

static int copy_path(const char *from, const char *to)
{ return copy_at(AT_FDCWD, from, AT_FDCWD, to); }

/* ── link-identity table ─────────────────────────────────────────────────
 * A copy is not a hardlink, and some callers CHECK.  git clone --local links
 * every object then verifies with lstat that source and destination share an
 * inode; our copy has a fresh one, so git aborts with
 *   fatal: hardlink different from source at '.../objects/58/587be6b4...'
 *
 * Rather than the full shadow-file scheme (rename aside, symlink both names,
 * emulate nlink across lstat/readlink/readdir/unlink/rename), record just the
 * identity: destinations created by the copy fallback report the SOURCE's
 * st_dev/st_ino and st_nlink=2.  That is the entire property callers test for,
 * at a fraction of the surface.
 *
 * Deliberately per-process and bounded: git does the link and the check in one
 * process, which is the case that matters.  A cross-process check would see
 * the truth -- documented limitation, not an oversight. */
#define ALR_LID_MAX 1024   /* two entries per link -- see lid_record() */
static struct { dev_t d, sd; ino_t i, si; } g_lid[ALR_LID_MAX];
static int g_lid_n;

static void lid_record(const char *src, const char *dst)
{
    struct stat a, b;
    if (g_lid_n + 2 > ALR_LID_MAX) return;
    if (!real_stat) return;
    if (real_stat(src, &a) != 0 || real_stat(dst, &b) != 0) return;
    g_lid[g_lid_n].d  = b.st_dev; g_lid[g_lid_n].i  = b.st_ino;
    g_lid[g_lid_n].sd = a.st_dev; g_lid[g_lid_n].si = a.st_ino;
    g_lid_n++;
    /* git stats the DESTINATION, but shadow's do_lock_file() links
     * /etc/group.<pid> -> /etc/group.lock and then checks st_nlink of the
     * SOURCE. With only the entry above, groupadd fails with "cannot lock
     * /etc/group; try again later." and every postinst that adds a system
     * group dies (openssh-client was the first to hit it). Register the source
     * against its own identity so its nlink reads 2 too -- dev/ino unchanged,
     * only the count is completed to match the success we just reported. */
    g_lid[g_lid_n].d  = a.st_dev; g_lid[g_lid_n].i  = a.st_ino;
    g_lid[g_lid_n].sd = a.st_dev; g_lid[g_lid_n].si = a.st_ino;
    g_lid_n++;
}

/* Rewrite a stat buffer so a copied destination looks like a hardlink. */
static void lid_patch(struct stat *st)
{
    int k;
    for (k = 0; k < g_lid_n; k++)
        if (g_lid[k].d == st->st_dev && g_lid[k].i == st->st_ino) {
            st->st_dev = g_lid[k].sd;
            st->st_ino = g_lid[k].si;
            if (st->st_nlink < 2) st->st_nlink = 2;
            return;
        }
}

/* coreutils `mv` uses renameat2(), not renameat().  It is listed in
 * docs/04-preload-spec.md §6.6 but was not implemented, so ca-certificates'
 * postinst failed at
 *   mv: cannot move '/etc/ssl/certs/ca-certificates.crt.new' to '...'
 * -- the absolute source went to the ANDROID root.  One unwrapped rename
 * variant left dpkg in a half-configured state that blocked every later apt
 * transaction, which is why this surfaced as an unrelated staging error. */
int renameat2(int da, const char *a, int db, const char *b, unsigned int flags)
{
    char ba[ALR_PBUF], bb[ALR_PBUF];
    const char *pa = rw(a, ba, sizeof ba), *pb = rw(b, bb, sizeof bb);
    if ((!pa && a) || (!pb && b)) { errno = ENAMETOOLONG; return -1; }
    NEED(renameat2);
    return real_renameat2(da, pa, db, pb, flags);
}

/* glibc's lckpwdf() opens the LITERAL "/etc/.pwd.lock" through an internal
 * __open64_nocancel -- below any public symbol we can interpose -- so it lands
 * on Android's /etc (a symlink to the read-only /system/etc) and fails EROFS.
 * shadow's commonio_lock() calls lckpwdf() FIRST and gives up when it fails,
 * which is why groupadd never even created /etc/group.<pid>:
 *     groupadd: cannot lock /etc/group; try again later.
 * Same shape as the mkstemp/__open case in docs/04-preload-spec.md: the public
 * entry point is interposable even though the internal one is not. Take over
 * lckpwdf/ulckpwdf and do the open ourselves, through rw(). */
static int g_pwdlock_fd = -1;

int lckpwdf(void)
{
    char b[ALR_PBUF];
    const char *p;
    struct flock fl;
    int fd, i;

    if (g_pwdlock_fd >= 0) return -1;          /* glibc: one lock per process */
    ensure_init();
    p = rw("/etc/.pwd.lock", b, sizeof b);
    if (!p) { errno = ENAMETOOLONG; return -1; }
    if (!real_open) { errno = ENOSYS; return -1; }
    fd = real_open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;

    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    /* glibc bounds the wait at 15s with alarm()+SIGALRM. A preload must not
     * install signal handlers behind the guest's back, so bound it with
     * retries instead -- same ceiling, no handler. */
    for (i = 0; i < 15; i++) {
        if (fcntl(fd, F_SETLK, &fl) == 0) { g_pwdlock_fd = fd; return 0; }
        if (errno != EACCES && errno != EAGAIN) break;
        sleep(1);
    }
    { int s = errno; close(fd); errno = s; return -1; }
}

int ulckpwdf(void)
{
    if (g_pwdlock_fd < 0) return -1;
    close(g_pwdlock_fd);
    g_pwdlock_fd = -1;
    return 0;
}

int link(const char *a, const char *b)
{
    char ba[ALR_PBUF], bb[ALR_PBUF];
    const char *pa = rw(a, ba, sizeof ba), *pb = rw(b, bb, sizeof bb);
    int rc;
    if ((!pa && a) || (!pb && b)) { errno = ENAMETOOLONG; return -1; }
    NEED(link);
    rc = real_link(pa, pb);
    /* EACCES is the SELinux refusal; EPERM/EXDEV are the classic ones other
     * filesystems give.  Catch all three -- code that only handles EXDEV is
     * exactly why this fails silently elsewhere. */
    if (rc != 0 && (errno == EACCES || errno == EPERM || errno == EXDEV)) {
        int saved = errno;
        if (copy_path(pa, pb) == 0) { lid_record(pa, pb);
            lg("alr link->copy: %s -> %s\n", a, b); return 0; }
        errno = saved;
    }
    return rc;
}
int linkat(int da, const char *a, int db, const char *b, int f)
{
    char ba[ALR_PBUF], bb[ALR_PBUF];
    const char *pa = rw(a, ba, sizeof ba), *pb = rw(b, bb, sizeof bb);
    int rc;
    if ((!pa && a) || (!pb && b)) { errno = ENAMETOOLONG; return -1; }
    NEED(linkat);
    rc = real_linkat(da, pa, db, pb, f);
    /* coreutils `ln` and apt both call linkat() with REAL directory fds, not
     * AT_FDCWD -- an earlier version of this fallback only handled AT_FDCWD and
     * so left `ln` failing with EACCES, which is how apt's
     * /tmp/apt-dpkg-install-XXXXXX staging directory ended up empty.
     * Copy through openat() so the dirfds are honoured either way. */
    if (rc != 0 && (errno == EACCES || errno == EPERM || errno == EXDEV)) {
        int saved = errno;
        if (copy_at(da, pa, db, pb) == 0) {
            if (da == AT_FDCWD && db == AT_FDCWD) lid_record(pa, pb);
            lg("alr linkat->copy: %s\n", a); return 0; }
        errno = saved;
    }
    return rc;
}
/* ASYMMETRIC: `target` is the symlink's CONTENT, a guest-namespace string.
 * Rewriting it to a host path would bake the rootfs location into the image.
 *
 * But leaving an ABSOLUTE target alone is equally broken: the kernel resolves
 * symlink contents against the REAL host root, so a guest link holding
 * "/etc/os-release" points at Android's /etc, not the rootfs's. We cannot
 * intercept that resolution -- it happens inside the kernel, below any libc
 * hook. The only representation that survives is a RELATIVE target, which the
 * kernel resolves against the link's own directory -- already inside the
 * rootfs. So: rewrite absolute targets to equivalent relative ones.
 *
 * `linkguest` is the link's own path in GUEST namespace; its depth below / is
 * how many "../" are needed to climb back to the rootfs root. */
static const char *relativize(const char *target, const char *linkguest,
                              char *buf, size_t bufsz)
{
    size_t o = 0, n;
    const char *p;
    int depth = 0;

    if (!target || target[0] != '/')       return target;  /* already relative */
    if (!linkguest || linkguest[0] != '/') return target;  /* depth unknowable */
    if (!g_root_len)                       return target;  /* passthrough mode */
    /* /proc, /sys, /dev are NOT rewritten by rw(), so an absolute target into
     * them is already correct -- relativizing would break it. */
    if (alr_is_sysdir(target))             return target;

    for (p = linkguest + 1; *p; p++) if (*p == '/') depth++;
    if ((size_t)depth * 3 + strlen(target) + 1 > bufsz) return target;

    while (depth-- > 0) { memcpy(buf + o, "../", 3); o += 3; }
    n = strlen(target + 1);                 /* skip the leading '/' */
    memcpy(buf + o, target + 1, n + 1);
    if (o == 0 && n == 0) { buf[0] = '.'; buf[1] = '\0'; }  /* link at /, "/" */
    return buf;
}

int symlink(const char *target, const char *linkpath)
{
    char rb[ALR_PBUF];
    const char *t = relativize(target, linkpath, rb, sizeof rb);
    if (t != target) lg("alr symlink: %s -> %s (link %s)\n", target, t, linkpath);
    { P(linkpath); NEED(symlink); return real_symlink(t, _p); }
}

int symlinkat(const char *target, int dfd, const char *linkpath)
{
    char rb[ALR_PBUF], gb[ALR_PBUF], fdb[64], hb[ALR_PBUF];
    const char *t = target, *lg_path = linkpath;

    /* Relative linkpath + a real dirfd: recover the guest path via
     * /proc/self/fd/N so the depth is computable. If anything fails we leave
     * the target untouched -- a wrong depth is worse than an absolute link. */
    if (linkpath && linkpath[0] != '/' && dfd != AT_FDCWD) {
        ssize_t r;
        int k = snprintf(fdb, sizeof fdb, "/proc/self/fd/%d", dfd);
        if (k > 0 && (size_t)k < sizeof fdb &&
            (r = readlink(fdb, hb, sizeof hb - 1)) > 0) {
            const char *c;
            hb[r] = '\0';
            c = alr_guest_canon(hb, g_root, g_root_len, gb, sizeof gb);
            if (c && c[0] == '/') {
                size_t dl = strlen(c);
                if (dl + 1 + strlen(linkpath) + 1 <= sizeof gb) {
                    if (c != gb) { memcpy(gb, c, dl + 1); }
                    if (dl && gb[dl - 1] != '/') gb[dl++] = '/';
                    memcpy(gb + dl, linkpath, strlen(linkpath) + 1);
                    lg_path = gb;
                }
            }
        }
    }
    t = relativize(target, lg_path, rb, sizeof rb);
    if (t != target)
        lg("alr symlinkat: %s -> %s (link %s dfd=%d)\n", target, t, lg_path, dfd);
    { P(linkpath); NEED(symlinkat); return real_symlinkat(t, dfd, _p); }
}

int chdir(const char *path) { P(path); NEED(chdir); return real_chdir(_p); }

char *getcwd(char *buf, size_t sz)
{
    char *r;
    /* Every other wrapper calls this; getcwd was the one that did not, so a
     * getcwd() reaching us before the constructor ran saw real_getcwd == NULL
     * and returned ENOSYS -- and worse, g_root/g_root_len unset, which is the
     * pre-main window (R14) the whole init dance exists to close. */
    ensure_init();
    if (!real_getcwd) { errno = ENOSYS; return NULL; }
    r = real_getcwd(buf, sz);
    char g[ALR_PBUF];
    const char *c;
    if (!r) return r;
    c = alr_guest_canon(r, g_root, g_root_len, g, sizeof g);
    if (c != r) {
        /* Capacity, not the caller's `sz`.  glibc supports getcwd(NULL, 0),
         * which allocates a buffer sized to the path -- and `sz` is then 0, so
         * a `n < sz` guard skipped the copy-back entirely and the caller got
         * the raw HOST path.
         *
         * MEASURED: `alr run /bin/pwd` printed <R>/usr/lib while python3's
         * os.getcwd() and bash's `pwd -P` printed /usr/lib.  coreutils reaches
         * getcwd through xgetcwd(), which uses exactly the allocating form;
         * the others pass a real buffer.  It looked like coreutils bypassing
         * the interposer -- it imports getcwd@GLIBC_2.17 like everyone else.
         *
         * Copying in place is safe because guest_canon only ever STRIPS the
         * root prefix, so the result is never longer than what glibc wrote. */
        size_t n = strlen(c);
        size_t cap = sz ? sz : strlen(r) + 1;
        if (n < cap) memcpy(r, c, n + 1);
    }
    return r;
}
char *get_current_dir_name(void)
{ char *b = malloc(ALR_PBUF); return b ? getcwd(b, ALR_PBUF) : NULL; }

char *realpath(const char *path, char *out)
{
    char b[ALR_PBUF], g[ALR_PBUF];
    const char *p = rw(path, b, sizeof b);
    char *r;
    const char *c;
    if (!p) { errno = ENAMETOOLONG; return NULL; }
    /* Rewrite the INPUT once, then suppress so glibc's internal lstat walk
     * does not re-prefix each component, then canonicalise the RESULT back to
     * guest space -- otherwise apt-key's keyring path and gpgv's disagree. */
    if (!real_realpath) { errno = ENOSYS; return NULL; }
    g_suppress++;
    r = real_realpath(p, out);
    g_suppress--;
    if (!r) return r;
    c = alr_guest_canon(r, g_root, g_root_len, g, sizeof g);
    if (c != r) { size_t n = strlen(c); memcpy(r, c, n + 1); }
    return r;
}
char *canonicalize_file_name(const char *p) { return realpath(p, NULL); }

/* ── SIGSYS-death guards ────────────────────────────────────────────── */
/* These are seccomp RET_TRAP on Android: calling them kills the process with
 * "Bad system call".  Guest tooling must get a clean errno instead. */
int mount(const char *a, const char *b, const char *c, unsigned long d, const void *e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; errno = EPERM; return -1; }
int umount(const char *a)            { (void)a; errno = EPERM; return -1; }
int umount2(const char *a, int b)    { (void)a;(void)b; errno = EPERM; return -1; }
int chroot(const char *a)            { (void)a; errno = EPERM; return -1; }
int pivot_root(const char *a, const char *b)
{ (void)a;(void)b; errno = EPERM; return -1; }

/* ── raw syscall interposition ──────────────────────────────────────── */
/* libuv issues path syscalls directly, so every Node fs.stat would otherwise
 * reach the kernel with an unrewritten guest path and ENOENT.  This is a
 * correctness fix, not an optimisation. */

/* bit i set => argument i is a path.  symlinkat is the ONLY entry whose dirfd
 * is not arg 0, so its path is arg 2 and arg 0 (the target) must not move. */
static unsigned path_arg_mask(long nr)
{
    switch (nr) {
    case 56: case 79: case 291: case 48: case 439: case 78:
    case 34: case 35: case 53: case 54: case 88: case 33: case 437:
        return 1u << 1;
    case 36:                       return 1u << 2;   /* symlinkat: linkpath */
    case 37: case 38: case 276:    return (1u << 1) | (1u << 3);
    default:                       return 0;
    }
}

long syscall(long nr, ...)
{
    long a[6];
    unsigned m = path_arg_mask(nr);
    va_list ap;
    int i;
    char b0[ALR_PBUF], b1[ALR_PBUF];
    char *bufs[2] = { b0, b1 };
    int used = 0;

    va_start(ap, nr);
    for (i = 0; i < 6; i++) a[i] = va_arg(ap, long);
    va_end(ap);

    for (i = 0; m && i < 6; i++) {
        if (!(m & (1u << i)) || !a[i]) continue;
        {
            const char *n = rw((const char *)a[i], bufs[used], ALR_PBUF);
            if (!n) { errno = ENAMETOOLONG; return -1; }
            if (n != (const char *)a[i]) { a[i] = (long)n; used++; }
        }
        if (used >= 2) break;
    }
#ifdef ALR_TRACE_AUXV
    if (m) lg("alr trace syscall: nr=%ld path=%s\n", nr,
              a[0] ? (const char *)a[0] : "(0)");
    else   lg("alr trace syscall: nr=%ld\n", nr);
#endif
    if (!real_syscall) { errno = ENOSYS; return -1; }
    return real_syscall(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}


/* ── exec re-dispatch ────────────────────────────────────────────────────
 * A stock guest binary cannot be execve'd: the kernel resolves its PT_INTERP
 * ("/lib/ld-linux-aarch64.so.1") against the REAL host root, which does not
 * have it, so exec fails with ENOENT (ADR 0002, measured).  Every exec the
 * guest makes must therefore be rewritten into an explicit loader invocation,
 * exactly like the one `alr` used for the first program.
 *
 * Miss ONE of the exec* variants and the child dies with a bare ENOENT that
 * is very hard to trace back to here. */

struct pctx { int fd; };
static int pctx_read(void *c, void *dst, size_t len, uint64_t off)
{
    struct pctx *p = c;
    ssize_t r = pread(p->fd, dst, len, (off_t)off);
    return r == (ssize_t)len;
}

#define ALR_EXEC_MAXARG 4096

/* ALR_GUEST_EXE is what /proc/self/exe answers with, and it also names the
 * process for prctl(PR_SET_NAME).  Re-dispatching an exec WITHOUT updating it
 * makes every child report its parent's identity:
 *
 *     alr run /bin/bash -c 'readlink /proc/self/exe'   ->  /bin/bash
 *
 * which is exactly the failure 04-preload-spec.md §7 calls a hard requirement,
 * since Node's process.execPath is that readlink and npm re-spawns from it
 * through shell wrappers.
 *
 * Rebuild the vector on the stack rather than mutating environ: callers may
 * pass an envp of their own, and execve must honour it.  Bounded and
 * allocation-free (R1); an env larger than the cap is passed through unchanged
 * rather than truncated, with a log line. */
#define ALR_ENV_MAX 256

static char *const *env_set_exe(char *const envp[], const char *guest,
                                char *kv, size_t kvsz, const char **out)
{
    size_t n = 0;
    int i, seen = 0;

    if (!envp) return envp;
    if ((size_t)snprintf(kv, kvsz, "ALR_GUEST_EXE=%s", guest) >= kvsz) return envp;
    for (i = 0; envp[i]; i++) {
        if (n >= ALR_ENV_MAX - 2) { lg("alr exec: env > %d, exe identity stale\n",
                                       ALR_ENV_MAX); return envp; }
        if (strncmp(envp[i], "ALR_GUEST_EXE=", 14) == 0) { out[n++] = kv; seen = 1; }
        else out[n++] = envp[i];
    }
    if (!seen) out[n++] = kv;
    out[n] = NULL;
    return (char *const *)out;
}

/* Build the loader argv for `host` and exec it.  Returns only on failure. */
/* Resolve `guest` -- following shebang chains -- into the (file, argv) pair a
 * caller can hand to execve() OR posix_spawn().  Split out of exec_dispatch()
 * because glibc's posix_spawn reaches the kernel through the internal
 * __spawnix, never our execve, so without this every posix_spawn child is
 * exec'd by the kernel with its PT_INTERP resolved against the ANDROID root.
 *
 * That is not theoretical: GNU make 4.3+ spawns every recipe line that way, and
 * `make` failed with "echo: No such file or directory" (Error 127) while the
 * identical command through a shell worked.  Same internal-alias family as
 * __nss_files_fopen (§6.17) and __gen_tempname -> __open (§6.14).
 *
 * The shebang chain is walked ITERATIVELY rather than by recursion so the
 * resolved vector outlives the call: each level's interpreter string lives in
 * the caller's ibuf[level].  Linux semantics -- exactly one optional argument
 * per #! line, never split further.
 *
 * Returns 1 (run *file_out with av[]), or -1 with errno set.  *ident_out is the
 * program's own guest path, for ALR_GUEST_EXE.  All storage is caller-provided:
 * R1 (no malloc) holds. */
#define ALR_SHEBANG_IBUF 256

static int exec_build(const char *guest, char *const argv[],
                      const char **av, int avmax,
                      char *hb, size_t hbsz,
                      char ibuf[][ALR_SHEBANG_IBUF], int maxdepth,
                      const char **file_out, const char **ident_out)
{
    struct { const char *arg, *file; } lvl[ALR_SHEBANG_MAX_DEPTH];
    unsigned char head[ALR_PROBE_BYTES];
    struct alr_shebang sb;
    struct pctx ctx;
    enum alr_exe_kind kind = ALR_EXE_UNSUPPORTED;
    const char *cur = guest, *host = NULL;
    int nl = 0, argc = 0, i, j;

    ensure_init();
    if (!real_open) { errno = ENOSYS; return -1; }

    for (;;) {
        ssize_t n;
        int fd;
        host = rw(cur, hb, hbsz);
        if (!host) { errno = ENAMETOOLONG; return -1; }
        fd = real_open(host, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        n = read(fd, head, sizeof head);
        if (n < 0) { close(fd); return -1; }
        ctx.fd = fd;
        kind = alr_classify(head, (size_t)n, pctx_read, &ctx,
                            &sb, ibuf[nl], ALR_SHEBANG_IBUF);
        close(fd);
        if (kind != ALR_EXE_SHEBANG) break;
        if (nl >= maxdepth - 1) { errno = ELOOP; return -1; }
        lvl[nl].arg  = sb.has_arg ? sb.arg : NULL;
        lvl[nl].file = cur;
        cur = sb.interp;
        nl++;
    }

    if (kind == ALR_EXE_UNSUPPORTED) { errno = ENOEXEC; return -1; }
    *ident_out = cur;

    if (kind == ALR_EXE_ELF_STATIC || !g_ldso) {
        /* Static binaries cannot be LD_PRELOADed at all; they run unhooked and
         * will not see the rootfs.  Report it rather than pretend. */
        lg("alr exec: static/unhooked %s\n", cur);
        *file_out = host;
        av[argc++] = argv[0] ? argv[0] : cur;
        for (j = nl - 1; j >= 0 && argc < avmax - 2; j--) {
            if (lvl[j].arg) av[argc++] = lvl[j].arg;
            av[argc++] = lvl[j].file;
        }
        for (i = 1; argv[i] && argc < avmax - 2; i++) av[argc++] = argv[i];
        av[argc] = NULL;
        return 1;
    }

    *file_out = g_ldso;
    av[argc++] = g_ldso;
    if (g_libpath) { av[argc++] = "--library-path"; av[argc++] = g_libpath; }
    av[argc++] = "--inhibit-cache";
    av[argc++] = "--argv0";
    /* Below the first level argv[0] belongs to the SCRIPT, not the interpreter
     * the loader is about to run. */
    av[argc++] = nl ? cur : (argv[0] ? argv[0] : guest);
    if (g_preload) { av[argc++] = "--preload"; av[argc++] = g_preload; }
    av[argc++] = host;
    for (j = nl - 1; j >= 0 && argc < avmax - 2; j--) {
        if (lvl[j].arg) av[argc++] = lvl[j].arg;
        av[argc++] = lvl[j].file;
    }
    for (i = 1; argv[i] && argc < avmax - 2; i++) av[argc++] = argv[i];
    av[argc] = NULL;

    lg("alr exec: %s -> loader%s\n", cur, nl ? " (shebang)" : "");
    return 1;
}

static int exec_dispatch(const char *guest, char *const argv[],
                         char *const envp[], int depth)
{
    char hb[ALR_PBUF], ibuf[ALR_SHEBANG_MAX_DEPTH][ALR_SHEBANG_IBUF];
    char kv[ALR_PBUF + 16];
    const char *av[ALR_EXEC_MAXARG];
    const char *ne[ALR_ENV_MAX + 1];
    const char *file = NULL, *ident = NULL;
    char *const *ep;

    (void)depth;                       /* depth now lives inside exec_build */
    if (!real_execve) { errno = ENOSYS; return -1; }
    if (exec_build(guest, argv, av, ALR_EXEC_MAXARG, hb, sizeof hb,
                   ibuf, ALR_SHEBANG_MAX_DEPTH, &file, &ident) < 0)
        return -1;
    ep = env_set_exe(envp, ident, kv, sizeof kv, ne);
    return real_execve(file, (char *const *)av, ep);
}

int execve(const char *path, char *const argv[], char *const envp[])
{ return exec_dispatch(path, argv, envp, 0); }

int execv(const char *path, char *const argv[])
{ return exec_dispatch(path, argv, environ, 0); }

/* PATH search must happen in the GUEST namespace: the host has no /usr/bin. */
static int exec_path_search(const char *file, char *const argv[],
                            char *const envp[])
{
    const char *path;
    char cand[ALR_PBUF], hb[ALR_PBUF];
    const char *p;
    int saved = ENOENT;

    if (strchr(file, '/')) return exec_dispatch(file, argv, envp, 0);

    path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    for (p = path; *p; ) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) {
            const char *h;
            snprintf(cand, sizeof cand, "%.*s/%s", (int)len, p, file);
            h = rw(cand, hb, sizeof hb);
            if (h && real_access && real_access(h, X_OK) == 0) {
                exec_dispatch(cand, argv, envp, 0);
                saved = errno;          /* only reached on failure */
            }
        }
        if (!e) break;
        p = e + 1;
    }
    errno = saved;
    return -1;
}

int execvp(const char *file, char *const argv[])
{ return exec_path_search(file, argv, environ); }
/* ---- posix_spawn family, system(), popen() ------------------------------
 *
 * docs/04-preload-spec.md §9.4.  These are the six exec entry points that were
 * declared mandatory and never implemented.  Measured failure before this
 * landed (guest-compiled probe, real glibc entry points -- not a CLI proxy):
 *
 *   posix_spawn  rc=0, child: CANNOT LINK EXECUTABLE "echo"
 *   system       rc=256   (sh could not link)
 *   make         Error 127
 *   python3 -c os.system(...)   rc=256
 *
 * posix_spawn is implemented by LOADERIZING the (path, argv) pair and handing
 * it to the real posix_spawn -- file_actions and attrp then keep working
 * untouched, which reimplementing the spawn would have put at risk.
 * system()/popen() are built on top, because glibc's own versions reach the
 * kernel through the same internal path. */
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>

static int (*real_posix_spawn)(pid_t *, const char *,
                               const posix_spawn_file_actions_t *,
                               const posix_spawnattr_t *,
                               char *const [], char *const []);

/* Resolve a PATH-searched name to a guest path, mirroring exec_path_search()
 * but returning the candidate instead of exec'ing it. */
static const char *spawn_search(const char *file, char *buf, size_t bufsz)
{
    const char *path, *p;
    char hb[ALR_PBUF];

    if (!file || strchr(file, '/')) return file;
    path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (p = path; *p; ) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) {
            const char *h;
            snprintf(buf, bufsz, "%.*s/%s", (int)len, p, file);
            h = rw(buf, hb, sizeof hb);
            if (h && real_access && real_access(h, X_OK) == 0) return buf;
        }
        if (!e) break;
        p = e + 1;
    }
    return file;                        /* let the spawn fail with ENOENT */
}

/* posix_spawn reports the error as its RETURN value and leaves errno alone. */
static int spawn_common(pid_t *pid, const char *path,
                        const posix_spawn_file_actions_t *fa,
                        const posix_spawnattr_t *attr,
                        char *const argv[], char *const envp[])
{
    char hb[ALR_PBUF], ibuf[ALR_SHEBANG_MAX_DEPTH][ALR_SHEBANG_IBUF];
    char kv[ALR_PBUF + 16];
    const char *av[ALR_EXEC_MAXARG];
    const char *ne[ALR_ENV_MAX + 1];
    const char *file = NULL, *ident = NULL;
    char *const *ep;

    ensure_init();
    if (!real_posix_spawn)
        real_posix_spawn = dlsym(RTLD_NEXT, "posix_spawn");
    if (!real_posix_spawn) return ENOSYS;
    if (exec_build(path, argv, av, ALR_EXEC_MAXARG, hb, sizeof hb,
                   ibuf, ALR_SHEBANG_MAX_DEPTH, &file, &ident) < 0)
        return errno ? errno : ENOEXEC;
    ep = env_set_exe(envp, ident, kv, sizeof kv, ne);
    return real_posix_spawn(pid, file, fa, attr, (char *const *)av, ep);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[])
{ return spawn_common(pid, path, fa, attr, argv, envp); }

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[])
{
    char cand[ALR_PBUF];
    ensure_init();
    return spawn_common(pid, spawn_search(file, cand, sizeof cand),
                        fa, attr, argv, envp);
}

int system(const char *cmd)
{
    char *const av[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
    struct sigaction ign, oint, oquit;
    sigset_t chld, omask;
    pid_t pid;
    int rc, st = -1;

    ensure_init();
    if (!cmd) return 1;                 /* "is a shell available" -- yes */

    /* POSIX: the caller ignores SIGINT/SIGQUIT and blocks SIGCHLD for the
     * duration.  make and configure scripts depend on this. */
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    sigaction(SIGINT,  &ign, &oint);
    sigaction(SIGQUIT, &ign, &oquit);
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, &omask);

    rc = spawn_common(&pid, "/bin/sh", NULL, NULL, av, environ);
    if (rc == 0) {
        while (waitpid(pid, &st, 0) < 0) {
            if (errno != EINTR) { st = -1; break; }
        }
    }

    sigaction(SIGINT,  &oint,  NULL);
    sigaction(SIGQUIT, &oquit, NULL);
    sigprocmask(SIG_SETMASK, &omask, NULL);

    if (rc != 0) { errno = rc; return -1; }
    return st;
}

/* Bounded, allocation-free (R1).  A process with more than this many live
 * popen streams falls back to reporting failure rather than losing the pid. */
#define ALR_POPEN_MAX 64
static struct { FILE *fp; pid_t pid; } g_popen[ALR_POPEN_MAX];

FILE *popen(const char *cmd, const char *mode)
{
    char *const av[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
    posix_spawn_file_actions_t fa;
    int pfd[2], rc, reading, k, slot = -1;
    pid_t pid;
    FILE *fp;

    ensure_init();
    if (!cmd || !mode) { errno = EINVAL; return NULL; }
    reading = (mode[0] == 'r');
    if (!reading && mode[0] != 'w') { errno = EINVAL; return NULL; }

    for (k = 0; k < ALR_POPEN_MAX; k++) if (!g_popen[k].fp) { slot = k; break; }
    if (slot < 0) { errno = EMFILE; return NULL; }
    if (pipe(pfd) != 0) return NULL;

    posix_spawn_file_actions_init(&fa);
    if (reading) {
        posix_spawn_file_actions_addclose(&fa, pfd[0]);
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, pfd[1]);
    } else {
        posix_spawn_file_actions_addclose(&fa, pfd[1]);
        posix_spawn_file_actions_adddup2(&fa, pfd[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&fa, pfd[0]);
    }
    rc = spawn_common(&pid, "/bin/sh", &fa, NULL, av, environ);
    posix_spawn_file_actions_destroy(&fa);

    close(reading ? pfd[1] : pfd[0]);
    if (rc != 0) { close(reading ? pfd[0] : pfd[1]); errno = rc; return NULL; }

    fp = fdopen(reading ? pfd[0] : pfd[1], reading ? "r" : "w");
    if (!fp) { close(reading ? pfd[0] : pfd[1]); return NULL; }
    g_popen[slot].fp = fp;
    g_popen[slot].pid = pid;
    return fp;
}

int pclose(FILE *fp)
{
    int k, st = -1;
    pid_t pid = -1;
    for (k = 0; k < ALR_POPEN_MAX; k++)
        if (g_popen[k].fp == fp) { pid = g_popen[k].pid;
                                   g_popen[k].fp = NULL; break; }
    fclose(fp);
    if (pid < 0) { errno = ECHILD; return -1; }
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
    return st;
}

/* Both reach the kernel without passing our execve, same as the spawn family.
 * /proc/self/fd/N is a sysdir so rw() leaves it alone and it names the real
 * file -- exec_build then classifies and loaderizes it normally. */
int fexecve(int fd, char *const argv[], char *const envp[])
{
    char p[64];
    if (snprintf(p, sizeof p, "/proc/self/fd/%d", fd) >= (int)sizeof p)
        { errno = EINVAL; return -1; }
    return exec_dispatch(p, argv, envp, 0);
}

int execveat(int dfd, const char *path, char *const argv[],
             char *const envp[], int flags)
{
    char p[ALR_PBUF];
    if (path && path[0] == '/') return exec_dispatch(path, argv, envp, 0);
    if (!path || !*path) return fexecve(dfd, argv, envp);   /* AT_EMPTY_PATH */
    if (dfd == AT_FDCWD) return exec_dispatch(path, argv, envp, 0);
    (void)flags;
    if (snprintf(p, sizeof p, "/proc/self/fd/%d/%s", dfd, path) >= (int)sizeof p)
        { errno = ENAMETOOLONG; return -1; }
    return exec_dispatch(p, argv, envp, 0);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{ return exec_path_search(file, argv, envp); }

static int build_l_argv(const char *a0, va_list ap, const char **av, int max,
                        char *const **envp_out)
{
    int n = 0;
    av[n++] = a0;
    while (n < max - 1) {
        const char *s = va_arg(ap, const char *);
        if (!s) break;
        av[n++] = s;
    }
    av[n] = NULL;
    if (envp_out) *envp_out = va_arg(ap, char *const *);
    return n;
}

int execl(const char *path, const char *a0, ...)
{
    const char *av[ALR_EXEC_MAXARG];
    va_list ap;
    va_start(ap, a0); build_l_argv(a0, ap, av, ALR_EXEC_MAXARG, NULL); va_end(ap);
    return exec_dispatch(path, (char *const *)av, environ, 0);
}
int execlp(const char *file, const char *a0, ...)
{
    const char *av[ALR_EXEC_MAXARG];
    va_list ap;
    va_start(ap, a0); build_l_argv(a0, ap, av, ALR_EXEC_MAXARG, NULL); va_end(ap);
    return exec_path_search(file, (char *const *)av, environ);
}
int execle(const char *path, const char *a0, ...)
{
    const char *av[ALR_EXEC_MAXARG];
    char *const *envp = environ;
    va_list ap;
    va_start(ap, a0); build_l_argv(a0, ap, av, ALR_EXEC_MAXARG, &envp); va_end(ap);
    return exec_dispatch(path, (char *const *)av, envp, 0);
}


/* ── resolver bridge (docs/RISKS.md R15) ─────────────────────────────────
 * glibc talks DNS straight to /etc/resolv.conf over port 53.  On a device with
 * Private DNS (DoT) or a VPN that path is silently dead -- measured: a raw
 * query to 8.8.8.8:53 from the guest times out while TCP to the same host:port
 * connects.  Android expects resolution to go through netd, which bionic does.
 * So hand the question across the ABI boundary to the bionic side.
 *
 * This is the one place the no-malloc rule (R1) cannot hold: getaddrinfo's
 * contract is to return a malloc'd list that the caller frees with
 * freeaddrinfo.  It is not a hot path -- one round trip per name lookup -- so
 * the exception is bounded and deliberate. */

/* ---- NSS: the `files` backend for passwd/group -------------------------
 *
 * glibc 2.34+ builds nss_files into libc, and it opens the LITERAL
 * "/etc/passwd" / "/etc/group" through __nss_files_fopen -> an internal fopen
 * alias that no LD_PRELOAD can reach. Inside the guest those literals resolve
 * against Android's /etc -> /system/etc, so every name lookup is answered from
 * the phone's 89-byte AID table instead of the rootfs:
 *
 *     getent group root      ->  (nothing)   [rootfs has root:x:0:]
 *     chgrp: invalid group: '_ssh'           [openssh-client postinst]
 *
 * Same shape as lckpwdf() and mkstemp(): the internal entry point is out of
 * reach, the public one is not. getpwnam/getgrnam and friends ARE interposable,
 * so implement the files backend over the rewritten paths.
 *
 * `files` is the only source that can work here anyway -- nss_systemd and
 * nss_ldap both need a daemon the guest does not run. */
#ifndef ALR_NO_NSS
#include <pwd.h>
#include <grp.h>

#define NSS_LINE 4096
#define NSS_MEM  64     /* group members per line; longer lists are truncated */

static FILE *nss_open(const char *guest)
{
    char b[ALR_PBUF];
    const char *p;
    ensure_init();
    if (!real_fopen) return NULL;
    p = rw(guest, b, sizeof b);
    if (!p) return NULL;
    return real_fopen(p, "re");
}

/* Split one field out of `*s` in place. Returns NULL when exhausted. */
static char *nss_field(char **s, char sep)
{
    char *b = *s, *e;
    if (!b) return NULL;
    e = strchr(b, sep);
    if (e) { *e = '\0'; *s = e + 1; } else { *s = NULL; }
    return b;
}

static int pw_parse(char *line, struct passwd *pw)
{
    char *s = line, *f;
    if (!(pw->pw_name   = nss_field(&s, ':')) || !s) return -1;
    if (!(pw->pw_passwd = nss_field(&s, ':')) || !s) return -1;
    if (!(f = nss_field(&s, ':')) || !s) return -1;
    pw->pw_uid = (uid_t)strtoul(f, NULL, 10);
    if (!(f = nss_field(&s, ':')) || !s) return -1;
    pw->pw_gid = (gid_t)strtoul(f, NULL, 10);
    if (!(pw->pw_gecos = nss_field(&s, ':')) || !s) return -1;
    if (!(pw->pw_dir   = nss_field(&s, ':'))) return -1;
    pw->pw_shell = s ? s : (char *)"";
    return 0;
}

static int gr_parse(char *line, struct group *gr, char **memv, int maxmem)
{
    char *s = line, *f;
    int n = 0;
    if (!(gr->gr_name   = nss_field(&s, ':')) || !s) return -1;
    if (!(gr->gr_passwd = nss_field(&s, ':')) || !s) return -1;
    if (!(f = nss_field(&s, ':'))) return -1;
    gr->gr_gid = (gid_t)strtoul(f, NULL, 10);
    while (s && *s && n < maxmem - 1) {
        char *m = nss_field(&s, ',');
        if (!m || !*m) break;
        memv[n++] = m;
    }
    if (s && *s) lg("alr nss: group %s member list truncated at %d\n", gr->gr_name, n);
    memv[n] = NULL;
    gr->gr_mem = memv;
    return 0;
}

static void nss_chomp(char *b)
{
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = '\0';
}

/* Carve an aligned gr_mem array out of the tail of the caller's buffer and
 * report how much is left for the line itself. */
static char **gr_carve(char *buf, size_t sz, size_t *linesz)
{
    size_t need = (NSS_MEM + 1) * sizeof(char *);
    uintptr_t end;
    if (sz < need + 128) return NULL;
    end = ((uintptr_t)buf + sz - need) & ~(uintptr_t)(sizeof(char *) - 1);
    *linesz = (size_t)(end - (uintptr_t)buf);
    return (char **)end;
}

/* byname: 1 = match name, 0 = match id. */
static int pw_scan(const char *name, uid_t uid, int byname,
                   struct passwd *pw, char *buf, size_t sz)
{
    FILE *f = nss_open("/etc/passwd");
    if (!f) return -1;
    while (fgets(buf, (int)sz, f)) {
        nss_chomp(buf);
        if (!buf[0] || buf[0] == '#') continue;
        if (pw_parse(buf, pw) != 0) continue;
        if (byname ? (name && strcmp(pw->pw_name, name) == 0) : (pw->pw_uid == uid))
            { fclose(f); return 0; }
    }
    fclose(f);
    return -1;
}

static int gr_scan(const char *name, gid_t gid, int byname,
                   struct group *gr, char *buf, size_t sz)
{
    size_t linesz;
    char **memv = gr_carve(buf, sz, &linesz);
    FILE *f;
    if (!memv) { errno = ERANGE; return -2; }
    if (!(f = nss_open("/etc/group"))) return -1;
    while (fgets(buf, (int)linesz, f)) {
        nss_chomp(buf);
        if (!buf[0] || buf[0] == '#') continue;
        if (gr_parse(buf, gr, memv, NSS_MEM) != 0) continue;
        if (byname ? (name && strcmp(gr->gr_name, name) == 0) : (gr->gr_gid == gid))
            { fclose(f); return 0; }
    }
    fclose(f);
    return -1;
}

/* glibc contract for the _r forms: return 0 with *res == NULL when the entry
 * simply is not there; only a real error gets a non-zero return. */
int getpwnam_r(const char *n, struct passwd *pw, char *b, size_t sz, struct passwd **res)
{ *res = NULL; if (pw_scan(n, 0, 1, pw, b, sz) == 0) *res = pw; return 0; }
int getpwuid_r(uid_t u, struct passwd *pw, char *b, size_t sz, struct passwd **res)
{ *res = NULL; if (pw_scan(NULL, u, 0, pw, b, sz) == 0) *res = pw; return 0; }
int getgrnam_r(const char *n, struct group *gr, char *b, size_t sz, struct group **res)
{ int r; *res = NULL; r = gr_scan(n, 0, 1, gr, b, sz);
  if (r == -2) return ERANGE; if (r == 0) *res = gr; return 0; }
int getgrgid_r(gid_t g, struct group *gr, char *b, size_t sz, struct group **res)
{ int r; *res = NULL; r = gr_scan(NULL, g, 0, gr, b, sz);
  if (r == -2) return ERANGE; if (r == 0) *res = gr; return 0; }

static struct passwd g_pw;  static char g_pwb[NSS_LINE];
static struct group  g_gr;  static char g_grb[NSS_LINE];

struct passwd *getpwnam(const char *n)
{ return pw_scan(n, 0, 1, &g_pw, g_pwb, sizeof g_pwb) == 0 ? &g_pw : NULL; }
struct passwd *getpwuid(uid_t u)
{ return pw_scan(NULL, u, 0, &g_pw, g_pwb, sizeof g_pwb) == 0 ? &g_pw : NULL; }
struct group *getgrnam(const char *n)
{ return gr_scan(n, 0, 1, &g_gr, g_grb, sizeof g_grb) == 0 ? &g_gr : NULL; }
struct group *getgrgid(gid_t g)
{ return gr_scan(NULL, g, 0, &g_gr, g_grb, sizeof g_grb) == 0 ? &g_gr : NULL; }

/* Enumeration -- `getent passwd` with no key, adduser's uid search, etc. */
static FILE *g_pwent, *g_grent;
static char *g_grmem[NSS_MEM + 1];

void setpwent(void) { if (g_pwent) fclose(g_pwent); g_pwent = nss_open("/etc/passwd"); }
void endpwent(void) { if (g_pwent) { fclose(g_pwent); g_pwent = NULL; } }
struct passwd *getpwent(void)
{
    if (!g_pwent && !(g_pwent = nss_open("/etc/passwd"))) return NULL;
    while (fgets(g_pwb, sizeof g_pwb, g_pwent)) {
        nss_chomp(g_pwb);
        if (!g_pwb[0] || g_pwb[0] == '#') continue;
        if (pw_parse(g_pwb, &g_pw) == 0) return &g_pw;
    }
    return NULL;
}
void setgrent(void) { if (g_grent) fclose(g_grent); g_grent = nss_open("/etc/group"); }
void endgrent(void) { if (g_grent) { fclose(g_grent); g_grent = NULL; } }
struct group *getgrent(void)
{
    if (!g_grent && !(g_grent = nss_open("/etc/group"))) return NULL;
    while (fgets(g_grb, sizeof g_grb, g_grent)) {
        nss_chomp(g_grb);
        if (!g_grb[0] || g_grb[0] == '#') continue;
        if (gr_parse(g_grb, &g_gr, g_grmem, NSS_MEM) == 0) return &g_gr;
    }
    return NULL;
}

/* `id -G`, and shadow's group bookkeeping. Read-only, so it is safe to answer
 * truthfully from the rootfs files. */
int getgrouplist(const char *user, gid_t base, gid_t *groups, int *ngroups)
{
    char line[NSS_LINE];
    char *memv[NSS_MEM + 1];
    struct group gr;
    FILE *f;
    int n = 0, room = *ngroups, k;

    if (room > 0) groups[0] = base;
    n = 1;
    if ((f = nss_open("/etc/group"))) {
        while (fgets(line, sizeof line, f)) {
            nss_chomp(line);
            if (!line[0] || line[0] == '#') continue;
            if (gr_parse(line, &gr, memv, NSS_MEM) != 0) continue;
            if (gr.gr_gid == base) continue;
            for (k = 0; gr.gr_mem[k]; k++) {
                if (user && strcmp(gr.gr_mem[k], user) == 0) {
                    if (n < room) groups[n] = gr.gr_gid;
                    n++;
                    break;
                }
            }
        }
        fclose(f);
    }
    k = (n > room) ? -1 : n;
    *ngroups = n;
    return k;
}
#endif /* ALR_NO_NSS -- bisect switch, see docs/evidence */

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>

/* shadow's audit_help_open() aborts useradd/groupadd outright ("Cannot open
 * audit interface - aborting.") unless audit_open() fails with one of
 * EINVAL/EPROTONOSUPPORT/EAFNOSUPPORT, which it reads as "kernel built without
 * audit". Android's SELinux policy denies NETLINK_AUDIT to app UIDs with
 * EACCES/EPERM instead, so every postinst that creates a system group dies --
 * openssh-client was the first to hit it. Report the unsupported errno so the
 * script takes the no-audit path it already has. */
#ifndef NETLINK_AUDIT
#define NETLINK_AUDIT 9
#endif
static int (*real_socket)(int, int, int);

int socket(int domain, int type, int protocol)
{
    if (domain == AF_NETLINK && protocol == NETLINK_AUDIT) {
        lg("alr socket: NETLINK_AUDIT -> EPROTONOSUPPORT\n");
        errno = EPROTONOSUPPORT;
        return -1;
    }
    if (!real_socket) real_socket = dlsym(RTLD_NEXT, "socket");
    if (!real_socket) { errno = ENOSYS; return -1; }
    return real_socket(domain, type, protocol);
}

/* ── AF_UNIX socket paths (docs/04-preload-spec.md §5) ───────────────────
 *
 * A unix socket address carries a PATH in sun_path, and until 2026-08-03
 * nothing rewrote it: bind() and connect() were not interposed at all.  So a
 * guest binding "/tmp/tmux-10297/default" bound the ANDROID path, which does
 * not exist.  MEASURED: tmux failed with
 *     error creating /tmp/tmux-10297/default (No such file or directory)
 * even after the directory was created and confirmed visible to the guest --
 * the directory was rewritten, the socket address was not.
 *
 * The class is much wider than tmux: dbus, gpg-agent and ssh-agent, X11, and
 * every database that listens on /var/run/<db>/.s.* are all in it.
 *
 * TWO THINGS THIS MUST NOT DO:
 *   - rewrite an ABSTRACT socket.  sun_path[0] == '\0' means the Linux
 *     abstract namespace, which has no filesystem presence at all; prefixing
 *     it would invent a different address.
 *   - truncate.  sun_path is 108 bytes and the root prefix is ~57 of them, so
 *     a long guest path genuinely does not fit.  A silently truncated address
 *     is a socket at the WRONG path that then half-works; fail with
 *     ENAMETOOLONG instead.
 */
static int (*real_bind)(int, const struct sockaddr *, socklen_t);
static int (*real_connect)(int, const struct sockaddr *, socklen_t);

static int un_rewrite(const struct sockaddr *addr, socklen_t len,
                      struct sockaddr_un *out, socklen_t *outlen)
{
    const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
    char buf[ALR_PBUF];
    const char *r;
    size_t n;

    if (!addr || addr->sa_family != AF_UNIX) return 0;
    if (len < (socklen_t)(sizeof(sa_family_t) + 1))   return 0;  /* unnamed */
    if (un->sun_path[0] == '\0')                      return 0;  /* abstract */
    if (un->sun_path[0] != '/')                       return 0;  /* relative */

    r = rw(un->sun_path, buf, sizeof buf);
    if (!r) { errno = ENAMETOOLONG; return -1; }
    if (r == un->sun_path) return 0;                             /* unchanged */

    n = strlen(r);
    if (n >= sizeof out->sun_path) {
        lg("alr unix: rewritten socket path too long (%zu >= %zu): %s\n",
           n, sizeof out->sun_path, r);
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->sun_family = AF_UNIX;
    memcpy(out->sun_path, r, n + 1);
    /* Length must cover the NUL for a pathname socket. */
    *outlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    return 1;
}

/* accept(2) is BLOCKED by the zygote filter; accept4(2) is not.
 *
 * MEASURED 2026-08-03: syscall 202 (accept) is in the 239-entry blocked set on
 * both reference devices, while 242 (accept4) is allowed -- as are socket,
 * bind, listen and connect.  So the supervisor emulated accept as -ENOSYS (the
 * table default) and EVERY GUEST PROGRAM THAT ACCEPTS CONNECTIONS FAILED.
 * tmux is how it surfaced: the server started, bound its socket, and then
 * died with "server exited unexpectedly" the moment a client connected.
 * Nothing tested it, so the whole class -- servers of any kind, unix or TCP --
 * was quietly broken.
 *
 * accept(fd, addr, len) is exactly accept4(fd, addr, len, 0); the flags
 * argument is the only difference.  Doing it here rather than in the
 * supervisor is both simpler and free: this is a plain libc call, whereas
 * redirecting one syscall to another from the tracer means rewriting registers
 * and re-entering, for no gain. */
static int (*real_accept4)(int, struct sockaddr *, socklen_t *, int);

/* Calls OUR accept4 below, not libc's, so the peer address gets the same
 * un-rewrite. Within this translation unit the name binds to the definition
 * here; that is the point. */
int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return accept4(fd, addr, len, 0);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct sockaddr_un un;
    socklen_t ul = 0;
    int k;
    ensure_init();
    k = un_rewrite(addr, len, &un, &ul);
    if (k < 0) return -1;
    if (!real_bind) real_bind = dlsym(RTLD_NEXT, "bind");
    if (!real_bind) { errno = ENOSYS; return -1; }
    return k ? real_bind(fd, (const struct sockaddr *)&un, ul)
             : real_bind(fd, addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct sockaddr_un un;
    socklen_t ul = 0;
    int k;
    ensure_init();
    k = un_rewrite(addr, len, &un, &ul);
    if (k < 0) return -1;
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    if (!real_connect) { errno = ENOSYS; return -1; }
    return k ? real_connect(fd, (const struct sockaddr *)&un, ul)
             : real_connect(fd, addr, len);
}

/* ...and the mirror.  Having rewritten sun_path on the way IN, the kernel
 * hands the rewritten path back on the way OUT, so a guest that asks where it
 * is bound gets the ANDROID path:
 *
 *   MEASURED 2026-08-03, before this existed:
 *     getsockname returned "/data/data/com.termux/.../ubuntu-24.04/tmp/x.sock"
 *                  wanted "/tmp/x.sock"
 *
 * This is the same leak getcwd() already corrects for the working directory,
 * and it matters for the same reason: programs print these paths, write them
 * into lock files and pid files, and hand them to other processes. tmux's
 * `display-message '#{socket_path}'` and every "server already running at ..."
 * message are the visible half; a lock file naming a path the guest cannot
 * open is the ugly half.
 *
 * accept()/accept4() fill in the PEER address, which for a unix stream socket
 * is normally unnamed -- but a client that bound a path before connecting has
 * one, so they go through the same correction. */
static void un_unrewrite(struct sockaddr *addr, socklen_t *len, socklen_t cap)
{
    struct sockaddr_un *un = (struct sockaddr_un *)addr;
    size_t room, avail;
    char g[ALR_PBUF];
    const char *c;
    size_t n;

    if (!addr || !len || addr->sa_family != AF_UNIX) return;
    if (*len <= (socklen_t)offsetof(struct sockaddr_un, sun_path)) return;

    /* CAP is what the caller actually allocated, captured BEFORE the call.
     * *len afterwards is what the kernel WOULD have written and can exceed it,
     * and sizeof(sun_path) is 108 regardless of either -- so scanning either
     * of those reads past the caller's object.  The idiom
     *     struct sockaddr sa; socklen_t l = sizeof sa;   // 16 bytes
     *     getsockname(fd, &sa, &l);
     * is common, and reading 108 bytes into it walks 92 bytes off the end. */
    room = (size_t)cap > offsetof(struct sockaddr_un, sun_path)
         ? (size_t)cap - offsetof(struct sockaddr_un, sun_path) : 0;
    avail = (size_t)*len > offsetof(struct sockaddr_un, sun_path)
          ? (size_t)*len - offsetof(struct sockaddr_un, sun_path) : 0;
    if (avail < room) room = avail;
    if (room > sizeof un->sun_path) room = sizeof un->sun_path;
    if (room == 0) return;
    if (un->sun_path[0] == '\0') return;               /* abstract or unnamed */

    /* Truncated by the caller's buffer: not a NUL-terminated path, leave it. */
    if (!memchr(un->sun_path, '\0', room)) return;

    c = alr_guest_canon(un->sun_path, g_root, g_root_len, g, sizeof g);
    if (c == un->sun_path) return;                     /* not under the root */

    /* guest_canon only ever strips the prefix, so this never grows. */
    n = strlen(c);
    if (n + 1 > room) return;                          /* would not fit back */
    memmove(un->sun_path, c, n + 1);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
}

static int (*real_getsockname)(int, struct sockaddr *, socklen_t *);
static int (*real_getpeername)(int, struct sockaddr *, socklen_t *);

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    socklen_t cap = len ? *len : 0;     /* the caller's real buffer size */
    int r;
    ensure_init();
    if (!real_getsockname) real_getsockname = dlsym(RTLD_NEXT, "getsockname");
    if (!real_getsockname) { errno = ENOSYS; return -1; }
    r = real_getsockname(fd, addr, len);
    if (r == 0) un_unrewrite(addr, len, cap);
    return r;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    socklen_t cap = len ? *len : 0;     /* the caller's real buffer size */
    int r;
    ensure_init();
    if (!real_getpeername) real_getpeername = dlsym(RTLD_NEXT, "getpeername");
    if (!real_getpeername) { errno = ENOSYS; return -1; }
    r = real_getpeername(fd, addr, len);
    if (r == 0) un_unrewrite(addr, len, cap);
    return r;
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    socklen_t cap = len ? *len : 0;
    int r;
    ensure_init();
    if (!real_accept4) real_accept4 = dlsym(RTLD_NEXT, "accept4");
    if (!real_accept4) { errno = ENOSYS; return -1; }
    r = real_accept4(fd, addr, len, flags);
    if (r >= 0) un_unrewrite(addr, len, cap);
    return r;
}

static int rb_connect(void)
{
    struct sockaddr_un sa;
    int fd;
    size_t n;
    if (!g_resolv_sock || !*g_resolv_sock) return -1;
    n = strlen(g_resolv_sock);
    if (n >= sizeof sa.sun_path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, g_resolv_sock, n + 1);
    /* real_connect, NOT our connect().
     *
     * g_resolv_sock is a HOST path -- alr_resolvd puts its socket under
     * Termux's $TMPDIR, outside the rootfs by construction.  Our connect()
     * rewrites absolute paths into the rootfs, so once bind/connect were
     * interposed this address became <R>$PREFIX/tmp/... which does not exist,
     * and every DNS lookup silently lost its bridge.
     *
     * MEASURED: `dig -v` aborted with "free(): invalid pointer" -- the failure
     * did not even look like a name-resolution problem.  The acceptance suite
     * could not see it: RESOLV HOSTS/AHOSTS/REVERSE all use the files backend
     * and never open this socket, and RESOLV LEGACY DNS is SKIPped for want of
     * a network.  It took the breadth sweep, which runs `dig -v`.
     *
     * The general rule this is an instance of: alr's OWN paths must never go
     * through rw().  The guest cannot name them and has no business reaching
     * them; only we do, and we already hold them in host form. */
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    if (!real_connect) { close(fd); return -1; }
    if (real_connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd); return -1;
    }
    return fd;
}

static int rb_write(int fd, const void *b, size_t n)
{
    const unsigned char *p = b;
    while (n) { ssize_t w = write(fd, p, n);
                if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
                p += w; n -= (size_t)w; }
    return 0;
}
static int rb_read(int fd, void *b, size_t n)
{
    unsigned char *p = b;
    while (n) { ssize_t r = read(fd, p, n);
                if (r == 0) return -1;
                if (r < 0) { if (errno == EINTR) continue; return -1; }
                p += r; n -= (size_t)r; }
    return 0;
}

/* ---- hosts: files ------------------------------------------------------
 *
 * The resolver bridge answers from BIONIC, so it reads Android's
 * /system/etc/hosts -- the rootfs's own /etc/hosts had no consumer on either
 * path.  Measured before this landed: an entry added to the guest's
 * /etc/hosts was invisible to `getent hosts`, `getent ahosts` and Python's
 * gethostbyname_ex alike.
 *
 * Implement the `files` half of `hosts: files dns` ourselves, exactly as §6.17
 * does for passwd/group, and let the bridge keep the `dns` half. */
static int hosts_lookup(const char *name, int af, void *addr4, void *addr6,
                        char *cname, size_t cnamesz)
{
    char line[NSS_LINE];
    FILE *f;
    int found = 0;

    if (!name || !*name) return 0;
    if (!(f = nss_open("/etc/hosts"))) return 0;
    while (fgets(line, sizeof line, f)) {
        char *h, *tok, *sp;
        nss_chomp(line);
        if ((h = strchr(line, '#'))) *h = '\0';
        tok = line;
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!*tok) continue;
        /* addr <tab/space> name [aliases...] */
        sp = tok;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp) continue;
        *sp++ = '\0';
        for (;;) {
            char *nm;
            while (*sp == ' ' || *sp == '\t') sp++;
            if (!*sp) break;
            nm = sp;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (*sp) *sp++ = '\0';
            if (strcasecmp(nm, name) != 0) continue;
            if ((af == AF_INET || af == AF_UNSPEC) &&
                inet_pton(AF_INET, tok, addr4) == 1) {
                snprintf(cname, cnamesz, "%s", nm); found = AF_INET; goto done;
            }
            if ((af == AF_INET6 || af == AF_UNSPEC) &&
                inet_pton(AF_INET6, tok, addr6) == 1) {
                snprintf(cname, cnamesz, "%s", nm); found = AF_INET6; goto done;
            }
        }
    }
done:
    fclose(f);
    return found;
}

/* Build a single-entry addrinfo list from a /etc/hosts hit.  Allocated with
 * malloc because the caller frees it with freeaddrinfo() -- R1 forbids malloc
 * on the PATH-REWRITE hot path, not here. */
/* getaddrinfo()'s second argument, resolved to a port.
 *
 * hosts_addrinfo() hardcoded 0 and was never even PASSED the service, so every
 * guest client that resolved a name out of /etc/hosts got port 0 and an
 * immediate ECONNREFUSED: `curl localhost:8080`, psql -h localhost, redis-cli,
 * python's socket.create_connection, every dev server anyone runs in the
 * guest.  The BRIDGE path carries the service correctly, which is exactly why
 * this hid -- it only bites the names answered from /etc/hosts, and localhost
 * is the most common one there is.
 *
 * Numeric first, which covers the overwhelming majority and is the only form
 * allowed under AI_NUMERICSERV.  Named services fall back to libc's
 * getservbyname; that reads a copy of the IANA list, and Android ships the
 * same one, so the answer is right even though the file is not the guest's. */
static unsigned short serv_port(const char *service,
                                const struct addrinfo *hints)
{
    char *end;
    unsigned long v;
    struct servent *se;
    const char *proto;
    int st;

    if (!service || !*service) return 0;
    v = strtoul(service, &end, 10);
    if (end != service && *end == '\0' && v <= 65535) return (unsigned short)v;
    if (hints && (hints->ai_flags & AI_NUMERICSERV)) return 0;

    st = hints ? hints->ai_socktype : 0;
    proto = st == SOCK_DGRAM ? "udp" : st == SOCK_STREAM ? "tcp" : NULL;

    /* Parse the GUEST's /etc/services ourselves, through nss_open -- the same
     * reason /etc/hosts and /etc/passwd are parsed here (§6.17): glibc's
     * getservbyname reaches the file through an internal call LD_PRELOAD
     * cannot see, so it would read ANDROID's copy.  MEASURED: falling back to
     * libc returned port 0 for "http". */
    {
        char line[NSS_LINE];
        FILE *f = nss_open("/etc/services");
        unsigned short port = 0;
        if (f) {
            while (fgets(line, sizeof line, f)) {
                char *h, *nm, *sp, *slash;
                unsigned long v;
                nss_chomp(line);
                if ((h = strchr(line, '#'))) *h = '\0';
                nm = line;
                while (*nm == ' ' || *nm == '\t') nm++;
                if (!*nm) continue;
                sp = nm;
                while (*sp && *sp != ' ' && *sp != '\t') sp++;
                if (!*sp) continue;
                *sp++ = '\0';
                while (*sp == ' ' || *sp == '\t') sp++;
                /* "<port>/<proto>", then optional aliases we do not need. */
                if (!(slash = strchr(sp, '/'))) continue;
                *slash++ = '\0';
                {   char *e = slash;
                    while (*e && *e != ' ' && *e != '\t') e++;
                    *e = '\0';
                }
                if (strcmp(nm, service) != 0) continue;
                if (proto && strcmp(slash, proto) != 0) continue;
                v = strtoul(sp, NULL, 10);
                if (v && v <= 65535) { port = (unsigned short)v; break; }
            }
            fclose(f);
        }
        if (port) return port;
    }

    /* Last resort.  Reads Android's copy of the same IANA list, which is
     * usually right, and is still better than returning 0. */
    se = getservbyname(service, proto);
    return se ? (unsigned short)ntohs((unsigned short)se->s_port) : 0;
}

static int hosts_addrinfo(int af, const void *a4, const void *a6,
                          const char *cname, const char *service,
                          const struct addrinfo *hints,
                          struct addrinfo **out)
{
    struct addrinfo *ai;
    int want_cname = hints && (hints->ai_flags & AI_CANONNAME);
    unsigned short port = serv_port(service, hints);

    ai = calloc(1, sizeof *ai);
    if (!ai) return EAI_MEMORY;
    ai->ai_family   = af;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    if (af == AF_INET) {
        struct sockaddr_in *sa = calloc(1, sizeof *sa);
        if (!sa) { free(ai); return EAI_MEMORY; }
        sa->sin_family = AF_INET; sa->sin_port = htons(port);
        memcpy(&sa->sin_addr, a4, 4);
        ai->ai_addr = (struct sockaddr *)sa; ai->ai_addrlen = sizeof *sa;
    } else {
        struct sockaddr_in6 *sa = calloc(1, sizeof *sa);
        if (!sa) { free(ai); return EAI_MEMORY; }
        sa->sin6_family = AF_INET6; sa->sin6_port = htons(port);
        memcpy(&sa->sin6_addr, a6, 16);
        ai->ai_addr = (struct sockaddr *)sa; ai->ai_addrlen = sizeof *sa;
    }
    if (want_cname && cname && *cname) ai->ai_canonname = strdup(cname);
    *out = ai;
    return 0;
}

/* Deep-copy an addrinfo chain into the layout freeaddrinfo() below expects:
 * every node, every ai_addr and every ai_canonname its own allocation. */
static int ai_clone(const struct addrinfo *src, struct addrinfo **out)
{
    struct addrinfo *head = NULL, *tail = NULL;
    *out = NULL;
    for (; src; src = src->ai_next) {
        struct addrinfo *n = calloc(1, sizeof *n);
        if (!n) goto oom;
        n->ai_flags = src->ai_flags;
        n->ai_family = src->ai_family;
        n->ai_socktype = src->ai_socktype;
        n->ai_protocol = src->ai_protocol;
        n->ai_addrlen = src->ai_addrlen;
        if (src->ai_addr && src->ai_addrlen) {
            if (!(n->ai_addr = malloc(src->ai_addrlen))) { free(n); goto oom; }
            memcpy(n->ai_addr, src->ai_addr, src->ai_addrlen);
        }
        if (src->ai_canonname) {
            size_t l = strlen(src->ai_canonname) + 1;
            if (!(n->ai_canonname = malloc(l))) {
                free(n->ai_addr); free(n); goto oom;
            }
            memcpy(n->ai_canonname, src->ai_canonname, l);
        }
        if (tail) tail->ai_next = n; else head = n;
        tail = n;
    }
    *out = head;
    return head ? 0 : EAI_NONAME;
oom:
    freeaddrinfo(head);
    return EAI_MEMORY;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **out)
{
    struct alr_resolv_req rq;
    struct alr_resolv_rsp rs;
    struct alr_resolv_ent ents[ALR_RESOLV_MAXRES];
    struct addrinfo *head = NULL, *tail = NULL;
    size_t nl, sl;
    uint32_t i;
    int fd;

    /* files before dns, per the guest's own nsswitch.conf ordering. */
    {   unsigned char a4[4], a6[16];
        char cn[256];
        int af = hosts_lookup(node, hints ? hints->ai_family : AF_UNSPEC,
                              a4, a6, cn, sizeof cn);
        if (af) {
            lg("alr hosts: %s from /etc/hosts\n", node);
            return hosts_addrinfo(af, a4, a6, cn, service, hints, out);
        }
    }

    fd = rb_connect();
    if (fd < 0) {
        /* No bridge (older alr, or it failed to start): fall through to the
         * guest's own resolver, which is correct on devices without Private
         * DNS or a VPN.  Failing hard here would break those too.
         *
         * THE RESULT MUST BE COPIED INTO OUR LAYOUT, not handed back as-is.
         * glibc allocates the whole chain as ONE block -- ai_addr and
         * ai_canonname point INSIDE the same allocation as ai -- while our
         * freeaddrinfo() below frees the three separately, because that is how
         * the bridge path allocates them.  Returning glibc's structure means
         * the caller's freeaddrinfo() calls free() on an interior pointer.
         *
         * MEASURED: `dig -v` aborted with "free(): invalid pointer" whenever
         * the bridge was unavailable.  Not a lookup failure, an abort -- and
         * this is the documented, expected path for any device where alr's
         * resolver daemon does not start, so it was one failed daemon away
         * from crashing every name lookup in the guest.
         *
         * One allocator owns the layout.  Copying costs one pass over a
         * handful of addresses on a path that already did a DNS round trip. */
        static int (*real_gai)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **);
        static void (*real_fai)(struct addrinfo *);
        struct addrinfo *g = NULL;
        int rc;

        if (!real_gai) real_gai = dlsym(RTLD_NEXT, "getaddrinfo");
        if (!real_fai) real_fai = dlsym(RTLD_NEXT, "freeaddrinfo");
        if (!real_gai) return EAI_FAIL;
        rc = real_gai(node, service, hints, &g);
        if (rc != 0) return rc;
        rc = ai_clone(g, out);
        if (real_fai) real_fai(g);
        return rc;
    }

    nl = node ? strlen(node) : 0;
    sl = service ? strlen(service) : 0;
    if (nl > ALR_RESOLV_MAXNAME || sl > ALR_RESOLV_MAXSERV) { close(fd); return EAI_FAIL; }

    memset(&rq, 0, sizeof rq);
    rq.magic    = ALR_RESOLV_MAGIC;
    rq.family   = hints ? hints->ai_family   : 0;
    rq.socktype = hints ? hints->ai_socktype : 0;
    rq.protocol = hints ? hints->ai_protocol : 0;
    rq.flags    = hints ? (hints->ai_flags & (AI_PASSIVE | AI_NUMERICHOST)) : 0;
    rq.node_len = (uint32_t)nl;  rq.has_node = node ? 1 : 0;
    rq.serv_len = (uint32_t)sl;  rq.has_serv = service ? 1 : 0;

    if (rb_write(fd, &rq, sizeof rq) != 0
        || (nl && rb_write(fd, node, nl) != 0)
        || (sl && rb_write(fd, service, sl) != 0)
        || rb_read(fd, &rs, sizeof rs) != 0
        || rs.magic != ALR_RESOLV_MAGIC) { close(fd); return EAI_FAIL; }

    if (rs.rc != 0 || rs.count == 0) { close(fd); return rs.rc ? rs.rc : EAI_NONAME; }
    if (rs.count > ALR_RESOLV_MAXRES) { close(fd); return EAI_FAIL; }
    if (rb_read(fd, ents, rs.count * sizeof ents[0]) != 0) { close(fd); return EAI_FAIL; }
    close(fd);

    for (i = 0; i < rs.count; i++) {
        struct addrinfo *ai = calloc(1, sizeof *ai);
        struct sockaddr *sa;
        if (!ai) break;
        sa = calloc(1, ents[i].addrlen ? ents[i].addrlen : 1);
        if (!sa) { free(ai); break; }
        memcpy(sa, ents[i].addr, ents[i].addrlen);
        ai->ai_family   = ents[i].family;
        ai->ai_socktype = ents[i].socktype;
        ai->ai_protocol = ents[i].protocol;
        ai->ai_addrlen  = ents[i].addrlen;
        ai->ai_addr     = sa;
        ai->ai_canonname = NULL;
        ai->ai_next     = NULL;
        if (tail) tail->ai_next = ai; else head = ai;
        tail = ai;
    }
    if (!head) return EAI_MEMORY;
    *out = head;
    return 0;
}

/* Must match our allocator: the guest frees what WE built. */
void freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *n = ai->ai_next;
        free(ai->ai_addr);
        free(ai->ai_canonname);
        free(ai);
        ai = n;
    }
}


/* ── temp-file family ────────────────────────────────────────────────────
 * These CANNOT be covered by the open() wrapper.  glibc implements them in
 * __gen_tempname, which calls the INTERNAL alias __open -- an intra-libc bind
 * that LD_PRELOAD cannot intercept.  So mkstemp("/tmp/apt.conf.XXXXXX") is
 * issued against the ANDROID host root, not the rootfs.
 *
 * Measured 2026-08-02: coreutils `mktemp` appeared to work, which briefly made
 * this look unnecessary -- but coreutils uses gnulib's own gen_tempname, which
 * calls the PUBLIC open and so is caught by the open() wrapper.  apt uses
 * glibc's mkstemp and failed with ENOENT on /var/cache/apt/... (no such
 * directory on the host).  Testing with `mktemp` measures the wrong thing.
 *
 * The template is IN-OUT: the real call overwrites XXXXXX in the buffer we
 * hand it, so the mutated bytes must be copied back into the caller's array. */

/* Copy the mutated template back.  `h` is what rw() returned: when it is the
 * caller's own pointer no rewrite happened and there is nothing to copy. */
/* ---- the legacy gethostby* family --------------------------------------
 *
 * Only getaddrinfo/freeaddrinfo were bridged, so everything still calling the
 * pre-RFC2553 API resolved through the guest's own glibc resolver -- which is
 * dead here whenever Private DNS or a VPN blocks plaintext port 53, the exact
 * condition the bridge exists for.  Measured before this landed:
 *
 *   python3 -c socket.getaddrinfo("ports.ubuntu.com", 80)   -> ('91.189.92.19', 80)
 *   python3 -c socket.gethostbyname_ex("ports.ubuntu.com")  -> herror 2
 *
 * Note that socket.gethostbyname() is NOT a valid probe: CPython routes it
 * through setipaddr() -> getaddrinfo, i.e. the path that already worked.
 * gethostbyname_ex / gethostbyaddr are the ones that bind the legacy symbols.
 *
 * Implemented over our own getaddrinfo, so both the /etc/hosts files backend
 * and the bionic bridge are inherited for free. */
static int he_fill(const char *name, int af, struct hostent *he,
                   char *buf, size_t bufsz, int *herr)
{
    struct addrinfo hints, *res = NULL, *p;
    char **alist, **aliases, *cur;
    size_t alen, nlen, need;
    uintptr_t pad;
    int rc, n = 0;

    if (af != AF_INET && af != AF_INET6) { *herr = NO_RECOVERY; return -1; }
    alen = (af == AF_INET) ? 4u : 16u;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = af;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(name, NULL, &hints, &res);
    if (rc != 0 || !res) { *herr = HOST_NOT_FOUND; return -1; }
    for (p = res; p; p = p->ai_next) if (p->ai_family == af) n++;
    if (!n) { freeaddrinfo(res); *herr = NO_ADDRESS; return -1; }

    nlen = strlen(name) + 1;
    /* addr_list[n+1] + aliases[1] + n addresses + the name, plus alignment */
    need = (size_t)(n + 2) * sizeof(char *) + (size_t)n * alen + nlen
         + sizeof(char *);
    cur = buf;
    pad = (uintptr_t)cur & (sizeof(char *) - 1);
    if (pad) cur += sizeof(char *) - pad;
    if (bufsz < need + (size_t)(cur - buf)) {
        freeaddrinfo(res);
        *herr = NETDB_INTERNAL;
        return ERANGE;
    }

    alist   = (char **)(void *)cur; cur += (size_t)(n + 1) * sizeof(char *);
    aliases = (char **)(void *)cur; cur += sizeof(char *);
    aliases[0] = NULL;

    n = 0;
    for (p = res; p; p = p->ai_next) {
        const void *src;
        if (p->ai_family != af) continue;
        src = (af == AF_INET)
            ? (const void *)&((struct sockaddr_in  *)(void *)p->ai_addr)->sin_addr
            : (const void *)&((struct sockaddr_in6 *)(void *)p->ai_addr)->sin6_addr;
        memcpy(cur, src, alen);
        alist[n++] = cur;
        cur += alen;
    }
    alist[n] = NULL;
    memcpy(cur, name, nlen);

    he->h_name      = cur;
    he->h_aliases   = aliases;
    he->h_addrtype  = af;
    he->h_length    = (int)alen;
    he->h_addr_list = alist;
    freeaddrinfo(res);
    return 0;
}

static struct hostent g_he;
static char g_hebuf[2048];

int gethostbyname2_r(const char *name, int af, struct hostent *ret, char *buf,
                     size_t buflen, struct hostent **result, int *h_errnop)
{
    int herr = 0, rc;
    if (result) *result = NULL;
    rc = he_fill(name, af, ret, buf, buflen, &herr);
    if (rc == ERANGE) { if (h_errnop) *h_errnop = NETDB_INTERNAL; return ERANGE; }
    /* glibc contract: a plain "not found" is return 0 with *result == NULL. */
    if (rc != 0) { if (h_errnop) *h_errnop = herr; return 0; }
    if (result) *result = ret;
    return 0;
}

int gethostbyname_r(const char *name, struct hostent *ret, char *buf,
                    size_t buflen, struct hostent **result, int *h_errnop)
{ return gethostbyname2_r(name, AF_INET, ret, buf, buflen, result, h_errnop); }

struct hostent *gethostbyname2(const char *name, int af)
{
    int herr = 0;
    ensure_init();
    if (he_fill(name, af, &g_he, g_hebuf, sizeof g_hebuf, &herr) != 0) {
        h_errno = herr ? herr : HOST_NOT_FOUND;
        return NULL;
    }
    return &g_he;
}

struct hostent *gethostbyname(const char *name)
{ return gethostbyname2(name, AF_INET); }

/* Reverse lookup is answered from /etc/hosts only.  There is no reverse path
 * through the bridge, and falling through to the guest resolver would stall on
 * the very port-53 block the bridge exists to route around -- an honest
 * HOST_NOT_FOUND beats a 20-second hang. */
static int hosts_reverse(const void *addr, int af, char *out, size_t outsz)
{
    char line[NSS_LINE], want[INET6_ADDRSTRLEN];
    FILE *f;
    int found = 0;

    if (!inet_ntop(af, addr, want, sizeof want)) return 0;
    if (!(f = nss_open("/etc/hosts"))) return 0;
    while (fgets(line, sizeof line, f)) {
        char *h, *tok, *sp;
        nss_chomp(line);
        if ((h = strchr(line, '#'))) *h = '\0';
        tok = line;
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!*tok) continue;
        sp = tok;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp) continue;
        *sp++ = '\0';
        if (strcmp(tok, want) != 0) continue;
        while (*sp == ' ' || *sp == '\t') sp++;
        if (!*sp) continue;
        { char *e = sp;
          while (*e && *e != ' ' && *e != '\t') e++;
          *e = '\0'; }
        snprintf(out, outsz, "%s", sp);
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

static int he_fill_addr(const void *addr, int af, struct hostent *he,
                        char *buf, size_t bufsz, int *herr)
{
    char nm[256];
    size_t alen = (af == AF_INET) ? 4u : 16u, nlen;
    char *cur = buf, **alist, **aliases;
    uintptr_t pad;

    if (af != AF_INET && af != AF_INET6) { *herr = NO_RECOVERY; return -1; }
    if (!hosts_reverse(addr, af, nm, sizeof nm)) { *herr = HOST_NOT_FOUND; return -1; }
    nlen = strlen(nm) + 1;

    pad = (uintptr_t)cur & (sizeof(char *) - 1);
    if (pad) cur += sizeof(char *) - pad;
    if (bufsz < (size_t)(cur - buf) + 3 * sizeof(char *) + alen + nlen)
        { *herr = NETDB_INTERNAL; return ERANGE; }

    alist = (char **)(void *)cur; cur += 2 * sizeof(char *);
    aliases = (char **)(void *)cur; cur += sizeof(char *);
    aliases[0] = NULL;
    memcpy(cur, addr, alen);
    alist[0] = cur; alist[1] = NULL;
    cur += alen;
    memcpy(cur, nm, nlen);

    he->h_name = cur;
    he->h_aliases = aliases;
    he->h_addrtype = af;
    he->h_length = (int)alen;
    he->h_addr_list = alist;
    return 0;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int af)
{
    int herr = 0;
    size_t alen = (af == AF_INET) ? 4u : 16u;
    ensure_init();
    if ((size_t)len != alen) { h_errno = NO_RECOVERY; return NULL; }
    if (he_fill_addr(addr, af, &g_he, g_hebuf, sizeof g_hebuf, &herr) != 0)
        { h_errno = herr ? herr : HOST_NOT_FOUND; return NULL; }
    return &g_he;
}

/* CPython's socket.gethostbyaddr binds gethostbyaddr_r, not gethostbyaddr --
 * implementing only the non-_r form left it failing with herror 2.  The same
 * trap as verifying mkstemp with mktemp. */
int gethostbyaddr_r(const void *addr, socklen_t len, int af,
                    struct hostent *ret, char *buf, size_t buflen,
                    struct hostent **result, int *h_errnop)
{
    int herr = 0, rc;
    size_t alen = (af == AF_INET) ? 4u : 16u;
    if (result) *result = NULL;
    if ((size_t)len != alen) { if (h_errnop) *h_errnop = NO_RECOVERY; return 0; }
    rc = he_fill_addr(addr, af, ret, buf, buflen, &herr);
    if (rc == ERANGE) { if (h_errnop) *h_errnop = NETDB_INTERNAL; return ERANGE; }
    if (rc != 0) { if (h_errnop) *h_errnop = herr; return 0; }
    if (result) *result = ret;
    return 0;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    const void *ap = NULL;
    int af = sa ? sa->sa_family : AF_UNSPEC;

    ensure_init();
    if (!sa) return EAI_FAMILY;
    if (af == AF_INET  && salen >= (socklen_t)sizeof(struct sockaddr_in))
        ap = &((const struct sockaddr_in *)(const void *)sa)->sin_addr;
    else if (af == AF_INET6 && salen >= (socklen_t)sizeof(struct sockaddr_in6))
        ap = &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
    else return EAI_FAMILY;

    if (host && hostlen) {
        char nm[256];
        if (!(flags & NI_NUMERICHOST) && hosts_reverse(ap, af, nm, sizeof nm))
            snprintf(host, hostlen, "%s", nm);
        else if (flags & NI_NAMEREQD)
            return EAI_NONAME;          /* no reverse path -- say so, do not stall */
        else if (!inet_ntop(af, ap, host, hostlen))
            return EAI_OVERFLOW;
    }
    if (serv && servlen) {
        unsigned short port = (af == AF_INET)
            ? ((const struct sockaddr_in  *)(const void *)sa)->sin_port
            : ((const struct sockaddr_in6 *)(const void *)sa)->sin6_port;
        snprintf(serv, servlen, "%u", (unsigned)ntohs(port));
    }
    return 0;
}

static void tmpl_writeback(char *tmpl, const char *h, const char *buf)
{
    if (h != buf) return;                 /* not rewritten */
    memcpy(tmpl, buf + g_root_len, strlen(tmpl));
}

#define TMPL_PROLOGUE(tmpl, failval) \
    char _tb[ALR_PBUF]; \
    const char *_h = rw((tmpl), _tb, sizeof _tb); \
    if (!_h) { errno = ENAMETOOLONG; return (failval); }

int mkstemp(char *tmpl)
{
    int fd;
    TMPL_PROLOGUE(tmpl, -1);
    NEED(mkstemp);
    fd = real_mkstemp((char *)_h);
    tmpl_writeback(tmpl, _h, _tb);
    return fd;
}
int mkstemps(char *tmpl, int suffixlen)
{
    int fd;
    TMPL_PROLOGUE(tmpl, -1);
    NEED(mkstemps);
    fd = real_mkstemps((char *)_h, suffixlen);
    tmpl_writeback(tmpl, _h, _tb);
    return fd;
}
int mkostemp(char *tmpl, int flags)
{
    int fd;
    TMPL_PROLOGUE(tmpl, -1);
    NEED(mkostemp);
    fd = real_mkostemp((char *)_h, flags);
    tmpl_writeback(tmpl, _h, _tb);
    return fd;
}
int mkostemps(char *tmpl, int suffixlen, int flags)
{
    int fd;
    TMPL_PROLOGUE(tmpl, -1);
    NEED(mkostemps);
    fd = real_mkostemps((char *)_h, suffixlen, flags);
    tmpl_writeback(tmpl, _h, _tb);
    return fd;
}
char *mkdtemp(char *tmpl)
{
    char *r;
    TMPL_PROLOGUE(tmpl, NULL);
    NEEDN(mkdtemp);
    r = real_mkdtemp((char *)_h);
    if (!r) return NULL;
    tmpl_writeback(tmpl, _h, _tb);
    return tmpl;                          /* contract: returns the CALLER's buffer */
}
int mkstemp64(char *t)                 __attribute__((alias("mkstemp")));
int mkstemps64(char *t, int s)         __attribute__((alias("mkstemps")));
int mkostemp64(char *t, int f)         __attribute__((alias("mkostemp")));
int mkostemps64(char *t, int s, int f) __attribute__((alias("mkostemps")));


/* ── filesystem statistics ───────────────────────────────────────────────
 * apt calls statvfs("/var/cache/apt/archives/") to check free space before
 * downloading.  Unrewritten that hits the Android host root, which has no such
 * directory, and apt aborts with
 *   "Couldn't determine free space in /var/cache/apt/archives/ - statvfs (2)"
 * -- after having already resolved the whole dependency set, so the failure
 * looks like a disk problem rather than a path problem. */
int statvfs(const char *path, struct statvfs *b)
{ P(path); NEED(statvfs); return real_statvfs(_p, b); }
int statvfs64(const char *path, struct statvfs64 *b)
{ P(path); if (!real_statvfs64_) { errno = ENOSYS; return -1; }
  return real_statvfs64_(_p, b); }
int statfs(const char *path, struct statfs *b)
{ P(path); NEED(statfs); return real_statfs(_p, b); }
int statfs64(const char *path, struct statfs64 *b)
{ P(path); if (!real_statfs64_) { errno = ENOSYS; return -1; }
  return real_statfs64_(_p, b); }


/* ── dlopen ──────────────────────────────────────────────────────────────
 * An absolute guest path handed to dlopen is NOT covered by --library-path:
 * the request goes straight to the loader, the one component we cannot
 * interpose, so it resolves against the Android host root and fails.
 * Measured: perl's XSLoader dlopen'ing
 *   /usr/lib/aarch64-linux-gnu/perl-base/auto/Cwd/Cwd.so
 * -> "cannot open shared object file", which aborts every maintainer script
 * dpkg runs.  Python C extensions and Node native addons fail the same way. */
void *dlopen(const char *path, int flags)
{
    char b[ALR_PBUF];
    const char *p = path;

    NEEDN(dlopen);
    /* Rewrite ONLY an absolute path with no dynamic-string token.  A bare
     * soname must keep going through the normal search path, and $ORIGIN /
     * $LIB / $PLATFORM are expanded by the loader against the CALLING object's
     * map -- prefixing them would produce a path that cannot exist. */
    if (path && path[0] == '/' && !strchr(path, '$')) {
        const char *n = rw(path, b, sizeof b);
        if (n) p = n;
    }
    return real_dlopen(p, flags);
}

/* ── fakeroot ────────────────────────────────────────────────────────────
 * dpkg refuses to run as non-root ("requested operation requires superuser
 * privilege") and chowns every unpacked file.  Neither is possible here: the
 * app has no CAP_CHOWN and the setuid family is blocked by seccomp.
 *
 * DESIGN NOTE (supersedes docs/02-architecture.md §4.4): this lives INSIDE
 * libalr_preload.so behind ALR_FAKEROOT=1, not in a separate chained .so.
 * The separate-library design existed so upstream libfakeroot could be dropped
 * in; we do not need that, and the RTLD_NEXT chaining contract it required was
 * flagged as a footgun -- get the symbol partition wrong and every chown/chmod
 * reaches the kernel with an unrewritten path.  One library, no chain, no
 * partition to get wrong.
 *
 * This is identity spoofing only.  Per-file uid/gid bookkeeping (a real
 * fakeroot metadata DB) is not implemented: stat() reports the true owner.
 * dpkg does not verify ownership after chown, so this is enough to unpack --
 * but it is NOT enough for anything that audits ownership. */
uid_t getuid(void)  { ensure_init(); return g_fakeroot ? 0 : (uid_t)syscall(SYS_getuid); }
uid_t geteuid(void) { ensure_init(); return g_fakeroot ? 0 : (uid_t)syscall(SYS_geteuid); }
gid_t getgid(void)  { ensure_init(); return g_fakeroot ? 0 : (gid_t)syscall(SYS_getgid); }
gid_t getegid(void) { ensure_init(); return g_fakeroot ? 0 : (gid_t)syscall(SYS_getegid); }

int getresuid(uid_t *r, uid_t *e, uid_t *s)
{ ensure_init(); if (!g_fakeroot) return (int)syscall(SYS_getresuid, r, e, s);
  if (r) *r = 0; if (e) *e = 0; if (s) *s = 0; return 0; }
int getresgid(gid_t *r, gid_t *e, gid_t *s)
{ ensure_init(); if (!g_fakeroot) return (int)syscall(SYS_getresgid, r, e, s);
  if (r) *r = 0; if (e) *e = 0; if (s) *s = 0; return 0; }

int getgroups(int size, gid_t list[])
{ ensure_init(); if (!g_fakeroot) return (int)syscall(SYS_getgroups, size, list);
  if (size > 0 && list) list[0] = 0;
  return size == 0 ? 1 : 1; }

/* The real calls would EPERM.  Report success and drop the change: dpkg only
 * cares that they did not fail. */
int fchown(int fd, uid_t u, gid_t g)
{ ensure_init(); if (g_fakeroot) return 0; return (int)syscall(SYS_fchown, fd, u, g); }


/* ── timestamps and node creation ────────────────────────────────────────
 * dpkg sets file times with utimes(2), NOT utimensat(2).  Without these
 * wrappers the call goes to the ANDROID host path and dpkg aborts with
 *   error setting timestamps of '/usr/bin/perl.dpkg-new': No such file or directory
 * which reads like the file was never created.
 *
 * This was missed twice over because `touch -d` was used to verify coverage --
 * and coreutils touch uses utimensat, which WAS wrapped.  Same class of wrong
 * proxy measurement as the mktemp/gnulib case (see "발견 4"): always exercise
 * the same libc entry point the real consumer uses. */
int utimes(const char *path, const struct timeval t[2])
{ P(path); NEED(utimes); return real_utimes(_p, t); }
int lutimes(const char *path, const struct timeval t[2])
{ P(path); NEED(lutimes); return real_lutimes(_p, t); }
int utime(const char *path, const struct utimbuf *t)
{ P(path); NEED(utime); return real_utime(_p, t); }
int futimesat(int d, const char *path, const struct timeval t[2])
{ P(path); NEED(futimesat); return real_futimesat(d, _p, t); }

int truncate(const char *path, off_t len)
{ P(path); NEED(truncate); return real_truncate(_p, len); }
int truncate64(const char *p, off_t l) __attribute__((alias("truncate")));

/* dpkg unpacks device nodes from some packages.  mknod cannot succeed without
 * CAP_MKNOD, so under fakeroot report success and leave a placeholder -- the
 * guest never uses these nodes, but dpkg aborts if the call fails. */
int mknod(const char *path, mode_t m, dev_t d)
{
    P(path); NEED(mknod);
    if (g_fakeroot && (S_ISCHR(m) || S_ISBLK(m))) {
        int fd = real_open(_p, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd >= 0) { close(fd); return 0; }
        return errno == EEXIST ? 0 : -1;
    }
    return real_mknod(_p, m, d);
}
int mknodat(int dfd, const char *path, mode_t m, dev_t d)
{ P(path); NEED(mknodat); return real_mknodat(dfd, _p, m, d); }
#ifndef ALR_NO_M13
/* ---- the remainder of the docs/04-preload-spec.md §6 tables -------------
 *
 * These were declared mandatory and never implemented.  Nothing flagged it
 * because src/preload/wrappers.def -- the single source of truth §6 names, and
 * the input to the PRELOAD CHK SYMBOLS PRESENT gate -- did not exist either.
 * It does now; this block is what closing that gap surfaced. */

/* §6.5: scandir takes a path.  Missing it meant `ls`-style scans through
 * scandir(3) walked ANDROID's tree. */
#ifndef ALR_NO_SCANDIR
int scandir(const char *path, struct dirent ***nl,
            int (*sel)(const struct dirent *),
            int (*cmp)(const struct dirent **, const struct dirent **))
{ P(path); NEED(scandir); return real_scandir(_p, nl, sel, cmp); }
int scandir64(const char *path, struct dirent64 ***nl,
              int (*sel)(const struct dirent64 *),
              int (*cmp)(const struct dirent64 **, const struct dirent64 **))
{ P(path); NEED(scandir64_); return real_scandir64_(_p, nl, sel, cmp); }

/* §6.3: faccessat2 is the newer syscall-backed form; glibc 2.33+ routes
 * faccessat through it when AT_EACCESS is requested. */
#endif
int faccessat2(int dfd, const char *path, int mode, int flags)
{ P(path); NEED(faccessat); return real_faccessat(dfd, _p, mode, flags); }

/* §6.5: the obsolete getwd and the _chk forms of the cwd family.  getwd has no
 * length argument at all, which is why glibc deprecated it -- bound the copy
 * at PATH_MAX and refuse rather than overflow the caller. */
char *getwd(char *buf)
{
    char tmp[ALR_PBUF];
    if (!buf) { errno = EINVAL; return NULL; }
    if (!getcwd(tmp, sizeof tmp)) return NULL;
    { size_t n = strlen(tmp);
      if (n + 1 > ALR_PBUF) { errno = ENAMETOOLONG; return NULL; }
      memcpy(buf, tmp, n + 1); }
    return buf;
}
char *__getwd_chk(char *buf, size_t buflen)
{
    char tmp[ALR_PBUF];
    if (!getcwd(tmp, sizeof tmp)) return NULL;
    { size_t n = strlen(tmp);
      if (n + 1 > buflen) { errno = ERANGE; return NULL; }
      memcpy(buf, tmp, n + 1); }
    return buf;
}
#ifndef ALR_NO_GETCWD_CHK
char *__getcwd_chk(char *buf, size_t size, size_t buflen)
{ if (size > buflen) { errno = ERANGE; return NULL; } return getcwd(buf, size); }
#endif

/* §6.4/§5.3: the fortified forms.  glibc's own __realpath_chk aborts when the
 * caller's buffer is under PATH_MAX; ours must do the same check and then go
 * through OUR realpath so the result comes back in guest space. */
#ifndef ALR_NO_REALPATH_CHK
char *__realpath_chk(const char *path, char *out, size_t outlen)
{ if (outlen < ALR_PBUF) { errno = ERANGE; return NULL; } return realpath(path, out); }
#endif
ssize_t __readlinkat_chk(int dfd, const char *path, char *buf,
                         size_t sz, size_t buflen)
{ if (sz > buflen) { errno = EINVAL; return -1; } return readlinkat(dfd, path, buf, sz); }

#ifndef ALR_NO_M13B
/* §5.3: ttyname returns a PATH, so it must come back canonicalised into guest
 * space or the guest re-prefixes it into nonsense on the next open. */
char *ttyname(int fd)
{
    static char g_tty[ALR_PBUF];
    if (ttyname_r(fd, g_tty, sizeof g_tty) != 0) return NULL;
    return g_tty;
}
int ttyname_r(int fd, char *buf, size_t buflen)
{
    char host[ALR_PBUF], g[ALR_PBUF];
    const char *c;
    int r;
    ensure_init();
    if (!real_ttyname_r) { errno = ENOSYS; return ENOSYS; }
    r = real_ttyname_r(fd, host, sizeof host);
    if (r != 0) return r;
    c = alr_guest_canon(host, g_root, g_root_len, g, sizeof g);
    { size_t n = strlen(c);
      if (n + 1 > buflen) return ERANGE;
      memcpy(buf, c, n + 1); }
    return 0;
}
int __ttyname_r_chk(int fd, char *buf, size_t buflen, size_t nreal)
{ if (buflen > nreal) { errno = EINVAL; return EINVAL; } return ttyname_r(fd, buf, buflen); }

/* §6.14: the rest of the temp-file surface.  Same reason mkstemp needed
 * interposing -- glibc builds the name against a LITERAL /tmp and opens it
 * through an internal entry point, so the file lands on Android's read-only
 * root instead of the rootfs. */
FILE *tmpfile(void)
{
    char tmpl[ALR_PBUF], b[ALR_PBUF];
    const char *p;
    int fd;
    ensure_init();
    snprintf(tmpl, sizeof tmpl, "/tmp/alrtmp.%d.XXXXXX", (int)getpid());
    if (!(p = rw(tmpl, b, sizeof b))) { errno = ENAMETOOLONG; return NULL; }
    { char work[ALR_PBUF];
      snprintf(work, sizeof work, "%s", p);
      if ((fd = mkstemp(work)) < 0) return NULL;
      unlink(work); }
    { FILE *f = fdopen(fd, "w+"); if (!f) close(fd); return f; }
}
FILE *tmpfile64(void) __attribute__((alias("tmpfile")));

char *tmpnam(char *buf)
{
    static char g_tmpnam[ALR_PBUF];
    char *out = buf ? buf : g_tmpnam;
    static unsigned seq;
    snprintf(out, buf ? L_tmpnam : sizeof g_tmpnam,
             "/tmp/alrnam.%d.%u", (int)getpid(), seq++);
    return out;
}
char *tmpnam_r(char *buf) { return buf ? tmpnam(buf) : NULL; }

char *tempnam(const char *dir, const char *pfx)
{
    static unsigned seq;
    char *out = malloc(ALR_PBUF);
    if (!out) return NULL;
    snprintf(out, ALR_PBUF, "%s/%s%d.%u",
             (dir && *dir) ? dir : "/tmp", pfx ? pfx : "alr",
             (int)getpid(), seq++);
    return out;                          /* caller frees, per the contract */
}

/* §6.6/§6.7: remaining path-argument syscalls. */
int mkfifoat(int dfd, const char *path, mode_t m)
{ P(path); NEED(mkfifoat); return real_mkfifoat(dfd, _p, m); }
int name_to_handle_at(int dfd, const char *path, struct file_handle *h,
                      int *mnt, int flags)
{ P(path); NEED(name_to_handle_at);
  return real_name_to_handle_at(dfd, _p, h, mnt, flags); }

/* §6.13: dlmopen takes the same path argument as dlopen. */
void *dlmopen(long nsid, const char *path, int flags)
{ PN(path); NEEDN(dlmopen); return real_dlmopen(nsid, _p, flags); }
#endif /* ALR_NO_M13B */
#endif /* ALR_NO_M13 */

int mkfifo(const char *path, mode_t m)
{ P(path); NEED(mkfifo); return real_mkfifo(_p, m); }

#ifdef ALR_DEBUG_CHK
/* Diagnostic build only.  glibc's __chk_fail is what prints
 * "*** buffer overflow detected ***" and aborts; it is an exported symbol, so
 * interposing it turns an opaque abort into a backtrace naming the caller.
 * Used to attribute the php-cli abort (docs/evidence M11 §5 left it
 * unattributed).  Never in a shipping build -- it changes abort semantics. */
#include <execinfo.h>
void __chk_fail(void)
{
    void *bt[24];
    int n = backtrace(bt, 24);
    const char *m = "\n=== alr: __chk_fail intercepted, backtrace ===\n";
    ssize_t w = write(2, m, strlen(m)); (void)w;
    backtrace_symbols_fd(bt, n, 2);
    _exit(134);
}
#endif

/* ---- ioctl translation (docs/04-preload-spec.md §11) --------------------
 *
 * Android's SELinux ioctlcmd filter denies most PTY-slave ioctls with EACCES.
 * MEASURED on the reference device with tests/device/probe_ioctl.c, using a
 * pair opened by the guest itself through /dev/ptmx:
 *
 *   allowed : TCGETS TCSETS TIOCGWINSZ TIOCSWINSZ FIONREAD TIOCOUTQ
 *   denied  : TCGETS2 TIOCGSID TIOCGETD TIOCEXCL TIOCSTI
 *
 * That measurement FALSIFIES this spec section's central premise.  §11 called
 * FIONREAD "가장 중요" and required emulating it through the master fd -- which
 * the guest side does not hold, and which is why the whole section went
 * unimplemented.  FIONREAD is simply allowed; it needs nothing.  (It reports 0
 * for "abc" with no newline because the line discipline is canonical -- correct
 * behaviour, not a denial.)
 *
 * What is left is small and answerable locally. */
#ifndef ALR_TCGETS2
#define ALR_TCGETS2  0x802c542aUL
#define ALR_TCSETS2  0x402c542bUL
#define ALR_TCSETSW2 0x402c542cUL
#define ALR_TCSETSF2 0x402c542dUL
#endif
#ifndef TIOCGSID
#define TIOCGSID 0x5429
#endif
#ifndef TIOCGETD
#define TIOCGETD 0x5424
#endif
#ifndef TIOCSETD
#define TIOCSETD 0x5423
#endif
#ifndef N_TTY
#define N_TTY 0
#endif

/* KERNEL layouts, not glibc's.  ioctl talks to the kernel directly, so glibc's
 * struct termios (NCCS=32, with c_ispeed/c_ospeed) is the wrong shape here;
 * the kernel's has NCCS=19 and termios2 is exactly that plus two speeds. */
#define ALR_KNCCS 19
struct alr_ktermios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[ALR_KNCCS];
};
struct alr_ktermios2 {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[ALR_KNCCS];
    unsigned int c_ispeed, c_ospeed;
};

static int (*real_ioctl)(int, unsigned long, ...);

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);

    ensure_init();
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!real_ioctl) { errno = ENOSYS; return -1; }

    switch (req) {
    case ALR_TCGETS2: {
        /* termios2 is termios + {c_ispeed, c_ospeed}; fill the speeds from the
         * CBAUD bits.  A PTY has no real line rate, so this is exact for every
         * caller that only wants the flags -- and BOTHER/arbitrary baud has no
         * meaning on a pty either. */
        struct alr_ktermios t;
        struct alr_ktermios2 *out = arg;
        if (!out) { errno = EFAULT; return -1; }
        if (real_ioctl(fd, TCGETS, &t) != 0) return -1;
        memcpy(out, &t, sizeof t);
        out->c_ispeed = out->c_ospeed = (t.c_cflag & 0x100f);   /* CBAUD */
        lg("alr ioctl: TCGETS2 -> TCGETS\n");
        return 0;
    }
    case ALR_TCSETS2:
    case ALR_TCSETSW2:
    case ALR_TCSETSF2: {
        const struct alr_ktermios2 *in = arg;
        struct alr_ktermios t;
        unsigned long to = (req == ALR_TCSETS2)  ? TCSETS
                         : (req == ALR_TCSETSW2) ? TCSETSW : TCSETSF;
        if (!in) { errno = EFAULT; return -1; }
        memcpy(&t, in, sizeof t);
        lg("alr ioctl: TCSETS*2 -> TCSETS*\n");
        return real_ioctl(fd, to, &t);
    }
    case TIOCGSID: {
        /* Answerable without the kernel's tty layer. */
        pid_t sid = getsid(0);
        if (sid < 0) return -1;
        if (!arg) { errno = EFAULT; return -1; }
        *(pid_t *)arg = sid;
        return 0;
    }
    case TIOCGETD:
        if (!arg) { errno = EFAULT; return -1; }
        *(int *)arg = N_TTY;               /* the only discipline a pty has */
        return 0;
    case TIOCSETD:
        if (arg && *(const int *)arg != N_TTY) { errno = EINVAL; return -1; }
        return 0;                          /* already N_TTY */
    case TIOCEXCL:
    case TIOCNXCL:
    case TIOCNOTTY:
        /* Advisory on a pty we already own exclusively.  Reporting success is
         * honest: the state the caller asked for is the state it gets. */
        return 0;
    case TIOCSTI:
        /* neverallowxperm in Android's policy -- this can never be granted, and
         * pretending otherwise would silently drop injected input. */
        lg("alr ioctl: TIOCSTI denied (neverallowxperm; see spec §11)\n");
        errno = EACCES;
        return -1;
    default:
        return real_ioctl(fd, req, arg);
    }
}
