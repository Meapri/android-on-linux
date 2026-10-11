/* alr-lane build (TAWCROOT_ALR): local changes that make the lane share a
 * rootfs with alr's LD_PRELOAD lane.  See VENDORING.md "Local changes" and
 * runtime/alr/docs/adr/0010-tawcroot-lane.md.
 *
 *   - identity is the real uid/gid unless the lane was started with
 *     --fakeroot (alr's ALR_FAKEROOT=1); stat is not decorated root-owned;
 *   - absolute symlink targets are stored relative, as the preload does;
 *   - a refused hardlink is a copy, as the preload does; no link store;
 *   - per-process caches that take syscalls off the trap path;
 *   - guest memory read directly, with SIGSEGV/SIGBUS owned to turn a bad
 *     pointer into EFAULT. */

#pragma once

#ifdef TAWCROOT_ALR
extern int tawcroot_alr_fakeroot;

/* Opened /proc/<pid>/fd directories are moved to fds at or above this,
 * below TAWCROOT_RESERVED_FD_BASE, and getdents64 traps only there
 * (filter_build.c, alr_fast.c). */
#define TAWCROOT_ALR_PROCFD_BAND 900
long tawcroot_alr_procfd_place(long fd, int flags);

/* Forget every "this directory prefix is not a symlink" fact.  Called
 * after any trapped syscall that can turn a directory into something
 * else (rename, unlink/rmdir, symlink, chroot). */
void tawcroot_dircache_invalidate(void);

/* 1 when the one-component relative `name` may be looked up by the
 * kernel against `dirfd` (or the cwd) without a NOFOLLOW lookup of the
 * name differing from the view's (alr_fast.c).  Callers that follow the
 * name must look first with NOFOLLOW and take the general route on a
 * symlink. */
int tawcroot_alr_fast_name(int dirfd, const char *name);
/* The cwd's guest path, as tawcroot_cwd_to_guest_abs, from a
 * per-process cache that chdir/fchdir/chroot clear. */
long tawcroot_alr_cwd_guest(char *out, size_t cap);
/* chdir, fchdir and chroot change the answers above. */
void tawcroot_alr_fast_cwd_changed(void);
void tawcroot_alr_fast_root_changed(void);

/* Leaf-symlink cache for FOLLOW stats (alr_fast.c): `key` is the link's
 * rootfs suffix, (ino, ctime) its NOFOLLOW stat, the value the rootfs
 * suffix the general route resolved it to.  get copies the value into
 * `out` (cap >= 192) and returns 1 on a hit. */
#include <stdint.h>
#include <stddef.h>
int  tawcroot_alr_link_get(const char *key, uint64_t ino, int64_t cs,
			   int64_t cns, char *out, size_t cap);
void tawcroot_alr_link_put(const char *key, uint64_t ino, int64_t cs,
			   int64_t cns, const char *val);
void tawcroot_alr_fast_links_changed(void);

/* Direct guest-memory copies, made safe by owning SIGSEGV/SIGBUS
 * (alr_fault.c).  Armed by tawcroot_alr_fault_init once the handlers are
 * in; until then usercopy.c keeps process_vm_readv.  The copies return
 * -1 on a fault: copy_str the length without the NUL (cap if none),
 * copy 0. */
extern int tawcroot_alr_fault_armed;
void tawcroot_alr_fault_init(void);
long tawcroot_alr_fault_sigaction(int sig, const void *act, void *oldact,
				  size_t sigsetsize);
void tawcroot_alr_fault_before_exec(void);
void tawcroot_alr_fault_after_failed_exec(void);
long tawcroot_alr_copy_str(char *dst, size_t cap, const char *src);
long tawcroot_alr_copy(void *dst, const void *src, size_t n);
#endif
