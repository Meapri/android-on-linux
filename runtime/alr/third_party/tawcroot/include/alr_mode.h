/* alr-lane build (TAWCROOT_ALR): local changes that make the lane share a
 * rootfs with alr's LD_PRELOAD lane.  See VENDORING.md "Local changes" and
 * runtime/alr/docs/adr/0010-tawcroot-lane.md.
 *
 *   - identity is the real uid/gid unless the lane was started with
 *     --fakeroot (alr's ALR_FAKEROOT=1); stat is not decorated root-owned;
 *   - absolute symlink targets are stored relative, as the preload does;
 *   - a refused hardlink is a copy, as the preload does; no link store;
 *   - per-process caches that take syscalls off the trap path. */

#pragma once

#ifdef TAWCROOT_ALR
extern int tawcroot_alr_fakeroot;

/* Forget every "this directory prefix is not a symlink" fact.  Called
 * after any trapped syscall that can turn a directory into something
 * else (rename, unlink/rmdir, symlink, chroot). */
void tawcroot_dircache_invalidate(void);
#endif
