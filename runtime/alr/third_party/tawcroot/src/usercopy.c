/* See include/usercopy.h. */

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#include "alr_mode.h"
#include "errno_neg.h"
#include "usercopy.h"
#include "raw_sys.h"
#include "sysnr.h"

int tawc_usercopy_works = 0;

/* The process_vm_{read,write}v helpers need the *current* task id to
 * address our own VM. We can't cache it: after `fork(2)` the
 * child inherits parent globals but lives in a different process,
 * with a fresh tid that doesn't match the parent's. Same with
 * thread creation. Recompute on every call — gettid is a cheap
 * vDSO-eligible syscall (~50ns through our raw stub) and the
 * SIGSYS handler is already on the slow path. (Discovered when
 * bash's fork+execve dance returned -EFAULT through our exec
 * handler — child's process_vm_readv was reading parent VM.) */
#ifdef TAWCROOT_ALR
/* alr-lane: the tgid of the process that owns this memory, without a
 * syscall -- cached in a page that fork(2) zero-fills (MADV_WIPEONFORK),
 * which is exactly the event that invalidates it.  A CLONE_VM child (vfork,
 * posix_spawn) keeps the page and so its parent's tgid; comparing that
 * with getpid() is how alr_fast.c and alr_fault.c tell such a child from
 * the memory's owner.  supervisor.c fills it at image start; after a
 * fork(2) the first trap refills it.  (A fork child whose very first trap
 * comes from a vfork child of its own would cache that child's pid.) */
static long *g_tgid_cache;

static long cached_task_id(void)
{
	long t = g_tgid_cache ? __atomic_load_n(g_tgid_cache, __ATOMIC_RELAXED) : 0;
	if (t > 0) return t;
	t = TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
	if (t > 0 && g_tgid_cache)
		__atomic_store_n(g_tgid_cache, t, __ATOMIC_RELAXED);
	return t > 0 ? t : 0;
}

long tawc_self_tgid(void)
{
	return cached_task_id();
}
#endif

static long current_task_id(void)
{
	long t = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	return t > 0 ? t : 0;
}

/* process_vm_{read,write}v against ourselves.  alr-lane copies directly
 * once its fault handlers are in (alr_fault.c), so this is the cold
 * path there and addresses the thread itself: a tgid cached before a
 * vfork could name a pid that has since gone to another process. */
static long vm_self(long nr, struct iovec *liov, struct iovec *riov)
{
	return TAWC_RAW(nr, current_task_id(), (long)liov, 1, (long)riov, 1, 0);
}

long tawc_usercopy_init(void)
{
	/* Probe: copy 1 byte from a known-good local address into a local
	 * buffer through process_vm_readv. If the kernel honors it, mark
	 * the helpers usable. */
	char src = 'x';
	char dst = 0;
	struct iovec liov = { .iov_base = &dst, .iov_len = 1 };
	struct iovec riov = { .iov_base = &src, .iov_len = 1 };
	long rv = TAWC_RAW(TAWC_SYS_process_vm_readv, current_task_id(),
			   (long)&liov, 1, (long)&riov, 1, 0);
	if (rv == 1 && dst == 'x') {
		tawc_usercopy_works = 1;
#ifdef TAWCROOT_ALR
		long pg = TAWC_RAW(TAWC_SYS_mmap, 0, 4096, 3 /*RW*/,
				   0x22 /*PRIVATE|ANONYMOUS*/, -1, 0);
		if (pg > 0) {
			if (TAWC_RAW(TAWC_SYS_madvise, pg, 4096,
				     18 /*MADV_WIPEONFORK*/, 0, 0, 0) == 0)
				g_tgid_cache = (long *)pg;
			else
				TAWC_RAW(TAWC_SYS_munmap, pg, 4096, 0, 0, 0, 0);
		}
#endif
		return 0;
	}
	return rv < 0 ? rv : TAWC_ENOSYS;
}

/* Read up to `len` bytes from a guest address. Returns bytes read or
 * -errno.
 *
 * If process_vm_readv was unreachable at init (probe failed) we return
 * -EFAULT loudly rather than fall back to a bare memcpy. The probe must
 * succeed on every target we ship to (Linux 3.2+ has the syscall; SELinux
 * lets a process readv itself); a probe failure is a deployment problem,
 * not a runtime fallback condition. (Review finding B7.) */
static long readv_guest(void *dst, size_t len, const void *src)
{
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	struct iovec liov = { .iov_base = dst,        .iov_len = len };
	struct iovec riov = { .iov_base = (void *)src, .iov_len = len };
	return vm_self(TAWC_SYS_process_vm_readv, &liov, &riov);
}

long tawc_copy_string_from_guest(char *dst, size_t cap,
				 const char *guest_src)
{
	if (!dst || cap == 0) return TAWC_EFAULT;
	if (!guest_src) return TAWC_EFAULT;
#ifdef TAWCROOT_ALR
	if (tawcroot_alr_fault_armed) {
		long n = tawcroot_alr_copy_str(dst, cap, guest_src);
		if (n < 0) return TAWC_EFAULT;
		return (size_t)n < cap ? n : TAWC_ENAMETOOLONG;
	}
#endif

	/* Read in chunks, scanning for NUL. Chunk size is a compromise
	 * between syscall overhead and over-reading near a page boundary
	 * (which can EFAULT past a valid string that ends mid-page). The
	 * kernel handles partial reads cleanly: process_vm_readv returns
	 * the number of bytes that actually copied. */
	size_t off = 0;
	while (off < cap) {
		size_t want = cap - off;
		if (want > 256) want = 256;
		long got = readv_guest(dst + off, want, guest_src + off);
		if (got < 0) return TAWC_EFAULT;
		if (got == 0) return TAWC_EFAULT;
		for (long i = 0; i < got; i++) {
			if (dst[off + i] == 0) {
				return (long)(off + i);  /* len excl NUL */
			}
		}
		off += (size_t)got;
		if ((size_t)got < want) {
			/* Partial read suggests a page boundary truncation;
			 * try again from the new offset. */
		}
	}
	return TAWC_ENAMETOOLONG;
}

long tawc_copy_from_guest(void *dst, size_t n, const void *guest_src)
{
	if (!dst || !guest_src) return TAWC_EFAULT;
#ifdef TAWCROOT_ALR
	if (tawcroot_alr_fault_armed)
		return tawcroot_alr_copy(dst, guest_src, n) < 0 ? TAWC_EFAULT : 0;
#endif
	long got = readv_guest(dst, n, guest_src);
	if (got < 0) return TAWC_EFAULT;
	if ((size_t)got != n) return TAWC_EFAULT;
	return 0;
}

long tawc_copy_to_guest(void *guest_dst, const void *src, size_t n)
{
	/* Even under the shared-VM model (guest manual-loaded into our own
	 * address space), a bare deref is unsafe: the guest may pass a NULL
	 * or unmapped output pointer and the kernel's contract is to return
	 * -EFAULT, not deliver SIGSEGV synchronously into our SIGSYS handler.
	 * Route through process_vm_writev against our own task id — the kernel
	 * validates the destination and reports -EFAULT cleanly. (Reviewed
	 * findings B1+B6.) */
	if (!guest_dst || !src) return TAWC_EFAULT;
	if (n == 0) return 0;
#ifdef TAWCROOT_ALR
	if (tawcroot_alr_fault_armed)
		return tawcroot_alr_copy(guest_dst, src, n) < 0 ? TAWC_EFAULT : 0;
#endif
	if (!tawc_usercopy_works) return TAWC_EFAULT;
	struct iovec liov = { .iov_base = (void *)src,    .iov_len = n };
	struct iovec riov = { .iov_base = guest_dst,      .iov_len = n };
	long rv = vm_self(TAWC_SYS_process_vm_writev, &liov, &riov);
	if (rv < 0)        return TAWC_EFAULT;
	if ((size_t)rv != n) return TAWC_EFAULT;
	return 0;
}
