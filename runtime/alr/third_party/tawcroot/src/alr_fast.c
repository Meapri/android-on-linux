/* alr-lane: kernel-resolved fast path for one-component relative names.
 *
 * The general route for a relative path lifts the dirfd (or the cwd) to
 * a guest-absolute path through /proc/self/fd (or getcwd), joins, folds
 * and walks the result.  That costs two readlinks and a getcwd-sized
 * string walk per call, eleven times a native fstatat(dirfd, name) on
 * the reference device.  find, fts, rm -r, du and git all issue exactly
 * that call.
 *
 * A single name with no ".." can be handed to the kernel against the
 * guest's own dirfd when nothing about that one lookup differs from the
 * view:
 *
 *   - the directory lies on the rootfs's filesystem, so it is not in a
 *     bind (/proc, /dev, /sys) whose magic links and shadows the full
 *     translator handles;
 *   - when the directory is the rootfs root, the name is not a bind
 *     destination (the rootfs's own empty /proc is not the view's);
 *   - the name itself is not followed when it is a symlink.  Callers
 *     look with NOFOLLOW first and take the general route on a symlink.
 *
 * A directory on the rootfs's filesystem but outside the rootfs (an fd
 * inherited from Termux) already resolves through the kernel on the
 * general route, so the fast path changes nothing there.
 *
 * Enabled only with the real ids, no link store, no read-only view and
 * binds that are one component deep (alr's /proc, /dev, /sys).  Each
 * condition is something the general route would act on. */

#ifdef TAWCROOT_ALR

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "alr_mode.h"
#include "dirent_filter.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#ifndef AT_FDCWD
# define AT_FDCWD -100
#endif

/* -1 unknown, 0 off, 1 on.  Recomputed after chroot. */
static int g_enabled = -1;
static unsigned long g_root_dev, g_root_ino;

static int compute_enabled(void)
{
	if (tawcroot_alr_fakeroot || tawcroot_store_link_fd >= 0 ||
	    tawcroot_identity_euid() == 0 || tawcroot_view_has_ro())
		return 0;
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		if (!b->active) continue;
		if (b->dst_len == 0) return 0;
		for (size_t k = 0; k < b->dst_len; k++)
			if (b->dst[k] == '/') return 0;
	}
	struct stat st;
	if (TAWC_RAW(TAWC_SYS_fstat, tawcroot_rootfs_fd, (long)&st,
		     0, 0, 0, 0) != 0)
		return 0;
	g_root_dev = (unsigned long)st.st_dev;
	g_root_ino = (unsigned long)st.st_ino;
	return 1;
}

static int enabled(void)
{
	int e = __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
	if (e < 0) {
		e = compute_enabled();
		__atomic_store_n(&g_enabled, e, __ATOMIC_RELEASE);
	}
	return e;
}

/* The cwd, cached per process: its guest path and its place.  One word
 * holds a generation, the tgid that computed it and the kind; chdir,
 * fchdir and chroot bump the generation.  The tgid keeps a vfork child
 * (same memory, own cwd) from trusting the parent's answer after its own
 * chdir, and the parent from trusting the child's.  The path is guarded
 * by a seqcount; a torn read is a miss. */
enum { CWD_UNKNOWN = 0, CWD_INSIDE = 1, CWD_ROOT = 2, CWD_ELSEWHERE = 3 };
static uint64_t g_cwd;
static uint32_t g_cwd_seq;            /* odd while written */
static long     g_cwd_len;            /* guest path length, or -errno */
static char     g_cwd_path[4096];

#define CWD_GEN(w)   ((uint32_t)((w) >> 40))
#define CWD_TGID(w)  ((uint32_t)(((w) >> 8) & 0xffffffffu))
#define CWD_KIND(w)  ((int)((w) & 0xff))
#define CWD_PACK(g, t, k) \
	(((uint64_t)(g) << 40) | ((uint64_t)(t) << 8) | (uint64_t)(k))

void tawcroot_alr_fast_cwd_changed(void)
{
	uint64_t w = __atomic_load_n(&g_cwd, __ATOMIC_RELAXED);
	for (;;) {
		uint64_t nw = CWD_PACK((CWD_GEN(w) + 1) & 0xffffff, 0, CWD_UNKNOWN);
		if (__atomic_compare_exchange_n(&g_cwd, &w, nw, 0,
						__ATOMIC_RELEASE,
						__ATOMIC_RELAXED))
			return;
	}
}

void tawcroot_alr_fast_root_changed(void)
{
	__atomic_store_n(&g_enabled, -1, __ATOMIC_RELEASE);
	tawcroot_alr_fast_cwd_changed();
}

/* Kind of the cached cwd for this process, CWD_UNKNOWN on a miss.  With
 * `out`, also copies the guest path (or returns its -errno in *len). */
static int cwd_lookup(char *out, size_t cap, long *len)
{
	uint32_t s1 = __atomic_load_n(&g_cwd_seq, __ATOMIC_ACQUIRE);
	if (s1 & 1) return CWD_UNKNOWN;
	uint64_t w = __atomic_load_n(&g_cwd, __ATOMIC_ACQUIRE);
	int kind = CWD_KIND(w);
	if (kind == CWD_UNKNOWN || (long)CWD_TGID(w) != tawc_self_tgid())
		return CWD_UNKNOWN;
	if (out) {
		long n = __atomic_load_n(&g_cwd_len, __ATOMIC_RELAXED);
		if (n >= (long)cap) return CWD_UNKNOWN;
		for (long i = 0; i < n; i++)
			out[i] = __atomic_load_n(&g_cwd_path[i], __ATOMIC_RELAXED);
		if (n >= 0) out[n] = 0;
		*len = n;
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (__atomic_load_n(&g_cwd_seq, __ATOMIC_RELAXED) != s1)
		return CWD_UNKNOWN;
	if (__atomic_load_n(&g_cwd, __ATOMIC_RELAXED) != w)
		return CWD_UNKNOWN;
	return kind;
}

/* Compute the cwd's kind and guest path (into `out`, -errno in *len) and
 * publish them unless a chdir ran meanwhile.  The getpid is this
 * process's own id, never a vfork parent's. */
static int cwd_fill(char *out, size_t cap, long *len)
{
	uint64_t w = __atomic_load_n(&g_cwd, __ATOMIC_ACQUIRE);
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *host = scratch->buf[0];
	long r = TAWC_RAW(TAWC_SYS_getcwd, (long)host, TAWCROOT_PATH_SCRATCH_SIZE,
			  0, 0, 0, 0);
	size_t hl = 0;
	int kind = CWD_ELSEWHERE;
	if (r > 0) {
		hl = (size_t)r;
		while (hl > 0 && host[hl - 1] == 0) hl--;
		size_t rl = tawcroot_rootfs_host_path_len;
		if (rl > 0 && hl >= rl &&
		    memcmp(host, tawcroot_rootfs_host_path, rl) == 0) {
			if (hl == rl) kind = CWD_ROOT;
			else if (host[rl] == '/') kind = CWD_INSIDE;
		}
		/* A bind whose source lies inside the rootfs owns its subtree. */
		for (size_t i = 0; kind != CWD_ELSEWHERE && i < tawcroot_n_binds; i++) {
			const struct tawcroot_bind *b = &tawcroot_binds[i];
			if (b->active && b->src_len > 0 && b->src_len <= hl &&
			    memcmp(host, b->src, b->src_len) == 0 &&
			    (hl == b->src_len || host[b->src_len] == '/'))
				kind = CWD_ELSEWHERE;
		}
		*len = tawcroot_host_path_to_guest_abs(host, hl, out, cap);
	} else {
		*len = r < 0 ? r : TAWC_ENOENT;
	}

	long self = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	uint32_t s1 = __atomic_load_n(&g_cwd_seq, __ATOMIC_RELAXED);
	if (!(s1 & 1) && __atomic_compare_exchange_n(&g_cwd_seq, &s1, s1 + 1, 0,
						     __ATOMIC_ACQUIRE,
						     __ATOMIC_RELAXED)) {
		long n = *len;
		if (n < (long)sizeof g_cwd_path) {
			for (long i = 0; i < n; i++)
				__atomic_store_n(&g_cwd_path[i], out[i],
						 __ATOMIC_RELAXED);
			__atomic_store_n(&g_cwd_len, n, __ATOMIC_RELAXED);
			uint64_t nw = CWD_PACK(CWD_GEN(w), (uint32_t)self, kind);
			(void)__atomic_compare_exchange_n(&g_cwd, &w, nw, 0,
							  __ATOMIC_RELEASE,
							  __ATOMIC_RELAXED);
		}
		__atomic_store_n(&g_cwd_seq, s1 + 2, __ATOMIC_RELEASE);
	}
	return kind;
}

static int cwd_kind(void)
{
	int k = cwd_lookup(0, 0, 0);
	if (k != CWD_UNKNOWN) return k;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	long n;
	k = cwd_fill(scratch->buf[0], TAWCROOT_PATH_SCRATCH_SIZE, &n);
	/* A vfork child computes for itself but must not reuse it. */
	return TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0) == tawc_self_tgid()
		? k : CWD_ELSEWHERE;
}

long tawcroot_alr_cwd_guest(char *out, size_t cap)
{
	long n;
	if (cwd_lookup(out, cap, &n) != CWD_UNKNOWN) return n;
	(void)cwd_fill(out, cap, &n);
	return n;
}

static int names_bind(const char *name)
{
	size_t n = tawc_strlen(name);
	for (size_t i = 0; i < tawcroot_n_binds; i++) {
		const struct tawcroot_bind *b = &tawcroot_binds[i];
		if (b->active && b->dst_len == n && memcmp(b->dst, name, n) == 0)
			return 1;
	}
	return 0;
}

int tawcroot_alr_fast_name(int dirfd, const char *name)
{
	if (!enabled()) return 0;
	if (name[0] == 0 || name[0] == '/') return 0;
	for (const char *p = name; *p; p++)
		if (*p == '/') return 0;
	if (name[0] == '.' && name[1] == '.' && name[2] == 0) return 0;

	int at_root;
	if (dirfd == AT_FDCWD) {
		int k = cwd_kind();
		if (k == CWD_ELSEWHERE) return 0;
		at_root = (k == CWD_ROOT);
	} else {
		if (tawcroot_fd_is_reserved(dirfd)) return 0;
		struct stat st;
		if (TAWC_RAW(TAWC_SYS_fstat, dirfd, (long)&st, 0, 0, 0, 0) != 0)
			return 0;
		if (!S_ISDIR(st.st_mode) || (unsigned long)st.st_dev != g_root_dev)
			return 0;
		at_root = ((unsigned long)st.st_ino == g_root_ino);
	}
	if (at_root && names_bind(name)) return 0;
	return 1;
}

/* A /proc/<pid>/fd directory just opened through a bind: move it into the
 * band where getdents64 still traps, so its listing keeps hiding our
 * reserved fds.  Every other directory is read without a trap.  The
 * guest sees a higher fd number than the lowest free one for exactly
 * these opens.  If the band is full the fd stays where it is and its
 * listing shows our fds. */
long tawcroot_alr_procfd_place(long fd, int flags)
{
	if (fd < 0 || (flags & O_PATH) || fd >= TAWCROOT_ALR_PROCFD_BAND)
		return fd;
	struct { long f_type; long rest[15]; } sfs;
	if (TAWC_RAW(TAWC_SYS_fstatfs, fd, (long)&sfs, 0, 0, 0, 0) != 0 ||
	    sfs.f_type != 0x9fa0 /* PROC_SUPER_MAGIC */)
		return fd;
	char self[32], link[64];
	if (tawc_proc_fd_path(self, sizeof self, (int)fd, 0) < 0) return fd;
	long ln = tawc_readlinkat(AT_FDCWD, self, link, sizeof link);
	if (ln <= 0 || ln >= (long)sizeof link ||
	    !tawcroot_dirent_filter_is_proc_fd_link(link, ln))
		return fd;
	long nfd = tawc_fcntl((int)fd, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC
							   : F_DUPFD,
			      TAWCROOT_ALR_PROCFD_BAND);
	if (nfd < 0) return fd;
	if (nfd >= TAWCROOT_RESERVED_FD_BASE) {
		tawc_close((int)nfd);
		return fd;
	}
	tawc_close((int)fd);
	return nfd;
}

/* Leaf-symlink cache for FOLLOW stats.  A FOLLOW stat looks at the leaf
 * with NOFOLLOW first (one fstatat, no readlink for the common
 * non-symlink).  When the leaf is a symlink, this remembers where the
 * general route took it.  The entry is keyed by the link's rootfs suffix
 * and checked against the link's inode and ctime from that first look:
 * a symlink's target never changes in place, so the same inode with the
 * same ctime still says the same thing.  The target's own directories
 * carry the directory cache's trust and its invalidation.  glibc stats
 * /etc/localtime on every localtime() call, so `ls -l` hits this once
 * per file. */
#define LC_SLOTS 64u
#define LC_LEN   192u

struct lc_slot {
	uint32_t seq;      /* odd while written */
	uint32_t gen;
	uint32_t hash;
	uint32_t klen;
	uint64_t ino;
	int64_t  ctime_s;
	int64_t  ctime_ns;
	char     key[LC_LEN];
	char     val[LC_LEN];
};

static struct lc_slot g_lc[LC_SLOTS];
static uint32_t       g_lc_gen = 1;

void tawcroot_alr_fast_links_changed(void)
{
	__atomic_fetch_add(&g_lc_gen, 1, __ATOMIC_RELEASE);
}

static uint32_t lc_hash(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
	return h;
}

int tawcroot_alr_link_get(const char *key, uint64_t ino, int64_t cs,
			  int64_t cns, char *out, size_t cap)
{
	size_t n = tawc_strlen(key);
	if (n == 0 || n >= LC_LEN || cap < LC_LEN) return 0;
	uint32_t h = lc_hash(key, n);
	struct lc_slot *e = &g_lc[h & (LC_SLOTS - 1)];
	uint32_t s1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
	if (s1 & 1) return 0;
	int ok = __atomic_load_n(&e->gen, __ATOMIC_RELAXED)
			 == __atomic_load_n(&g_lc_gen, __ATOMIC_ACQUIRE)
		 && __atomic_load_n(&e->hash, __ATOMIC_RELAXED) == h
		 && __atomic_load_n(&e->klen, __ATOMIC_RELAXED) == n
		 && __atomic_load_n(&e->ino, __ATOMIC_RELAXED) == ino
		 && __atomic_load_n(&e->ctime_s, __ATOMIC_RELAXED) == cs
		 && __atomic_load_n(&e->ctime_ns, __ATOMIC_RELAXED) == cns;
	for (size_t i = 0; ok && i < n; i++)
		if (__atomic_load_n(&e->key[i], __ATOMIC_RELAXED) != key[i]) ok = 0;
	size_t i = 0;
	for (; ok && i < LC_LEN; i++) {
		out[i] = __atomic_load_n(&e->val[i], __ATOMIC_RELAXED);
		if (!out[i]) break;
	}
	if (i == LC_LEN) ok = 0;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return ok && __atomic_load_n(&e->seq, __ATOMIC_RELAXED) == s1;
}

void tawcroot_alr_link_put(const char *key, uint64_t ino, int64_t cs,
			   int64_t cns, const char *val)
{
	size_t n = tawc_strlen(key), vn = tawc_strlen(val);
	if (n == 0 || n >= LC_LEN || vn >= LC_LEN) return;
	uint32_t h = lc_hash(key, n);
	struct lc_slot *e = &g_lc[h & (LC_SLOTS - 1)];
	uint32_t gen = __atomic_load_n(&g_lc_gen, __ATOMIC_ACQUIRE);
	uint32_t s1 = __atomic_load_n(&e->seq, __ATOMIC_RELAXED);
	if ((s1 & 1) || !__atomic_compare_exchange_n(&e->seq, &s1, s1 + 1, 0,
						     __ATOMIC_ACQUIRE,
						     __ATOMIC_RELAXED))
		return;
	__atomic_store_n(&e->gen, gen, __ATOMIC_RELAXED);
	__atomic_store_n(&e->hash, h, __ATOMIC_RELAXED);
	__atomic_store_n(&e->klen, (uint32_t)n, __ATOMIC_RELAXED);
	__atomic_store_n(&e->ino, ino, __ATOMIC_RELAXED);
	__atomic_store_n(&e->ctime_s, cs, __ATOMIC_RELAXED);
	__atomic_store_n(&e->ctime_ns, cns, __ATOMIC_RELAXED);
	for (size_t i = 0; i < n; i++)
		__atomic_store_n(&e->key[i], key[i], __ATOMIC_RELAXED);
	for (size_t i = 0; i <= vn; i++)
		__atomic_store_n(&e->val[i], val[i], __ATOMIC_RELAXED);
	__atomic_store_n(&e->seq, s1 + 2, __ATOMIC_RELEASE);
}

#endif /* TAWCROOT_ALR */
