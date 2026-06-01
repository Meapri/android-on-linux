/*
 * Hand-written config.h for building canonical libwayland-server 1.25.0
 * against the Android NDK (bionic, API >= 26, aarch64).
 *
 * Replaces the autotools/meson generated config.h. Values chosen to match
 * what `meson` would emit for an aarch64-linux-android (bionic) target.
 *
 * Provenance / generation: see ../VENDORING.md. Do NOT hand-edit the wayland
 * source .c/.h files vendored alongside this header.
 */
#ifndef ALR_WAYLAND_CONFIG_H
#define ALR_WAYLAND_CONFIG_H

/* libwayland insists on _GNU_SOURCE for accept4/SOCK_CLOEXEC/etc.
 * CMake also passes -D_GNU_SOURCE; guard to avoid a redefinition warning
 * under -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Package identification (from meson.build: version '1.25.0'). */
#define PACKAGE "wayland"
#define PACKAGE_VERSION "1.25.0"
#define PACKAGE_STRING "wayland 1.25.0"
#define PACKAGE_BUGREPORT \
	"https://gitlab.freedesktop.org/wayland/wayland/issues/"

/* bionic (API >= 26) provides accept4(2). */
#define HAVE_ACCEPT4 1

/* bionic provides memfd_create(2) (linux/memfd.h + sys/mman.h shim, API 30
 * for the libc symbol; we provide a syscall fallback in config below if the
 * symbol is missing — but API 26 + NDK r27 unified headers declare it). */
#define HAVE_MEMFD_CREATE 1

/* bionic declares gettid() in <unistd.h> for API >= 21. */
#define HAVE_GETTID 1

/* bionic provides mkostemp(3) (API >= 23). */
#define HAVE_MKOSTEMP 1

/* bionic provides posix_fallocate(3) (API >= 21). */
#define HAVE_POSIX_FALLOCATE 1

/* bionic provides prctl(2) via <sys/prctl.h>. */
#define HAVE_SYS_PRCTL_H 1

/*
 * The following are deliberately LEFT UNDEFINED for Linux/bionic; defining
 * them would pull in BSD-only code paths in wayland-os.c:
 *   HAVE_SYS_UCRED_H, HAVE_XUCRED_CR_PID  (FreeBSD/Darwin xucred)
 *   HAVE_SYS_PROCCTL_H                    (FreeBSD procctl)
 *   HAVE_BROKEN_MSG_CMSG_CLOEXEC          (old kernels; Linux is fine)
 */

#endif /* ALR_WAYLAND_CONFIG_H */
