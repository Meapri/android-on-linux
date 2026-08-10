/* A libdl.so.2 that exists only to satisfy our own DT_NEEDED.
 *
 * The interposer targets glibc 2.17, where dlsym/dlopen live in libdl.so.2, so
 * it carries libdl.so.2 in DT_NEEDED and references dlsym@GLIBC_2.17. Every
 * stock Debian/Ubuntu rootfs ships the stub that satisfies that -- glibc 2.34+
 * moved the functions into libc and left libdl as a placeholder exporting
 * nothing but a version node.
 *
 * An image trimmed by closure analysis over its OWN binaries can drop it,
 * because the interposer is injected afterwards and was never in that closure.
 * When that happens ld.so fails EVERY dynamic guest program and names the
 * program, not us:
 *     /usr/bin/dpkg: error while loading shared libraries: libdl.so.2
 *
 * MEASURED against Ubuntu 24.04's own libdl.so.2: 67,440 bytes exporting
 * exactly one symbol, __libdl_version_placeholder@GLIBC_2.17, plus the
 * GLIBC_2.17 version node. There is no code in it. So this file reproduces it
 * rather than shipping a distro binary -- same effect, no vendored artifact,
 * no licence question.
 *
 * The real dlsym/dlopen still resolve from the guest's libc, which keeps the
 * GLIBC_2.17 aliases for compatibility. This provides the NAME and the VERSION
 * NODE, nothing else, which is precisely what the stub it replaces provides.
 */
void __libdl_version_placeholder(void);
void __libdl_version_placeholder(void) { }
