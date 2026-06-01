/*
 * Hand-written fficonfig.h for libffi 3.5.2 targeting aarch64-linux-android
 * (bionic, NDK r27, API >= 26).
 *
 * Replaces the autotools-generated fficonfig.h. Values mirror what
 * `./configure --host=aarch64-linux-android` would emit, per
 * libffi/configure.host (TARGET=AARCH64) and libffi/configure.ac
 * (the `*-linux-android*` branch sets FFI_MMAP_EXEC_WRIT=1, NOT
 * FFI_EXEC_TRAMPOLINE_TABLE).
 *
 * Provenance / regeneration: see ../../VENDORING.md.
 */
#ifndef ALR_LIBFFI_FFICONFIG_H
#define ALR_LIBFFI_FFICONFIG_H

/* Define this if you are using Purify and want to suppress spurious messages. */
/* #undef USING_PURIFY */

/* The target architecture (matches `#define AARCH64` from ffi.h). */
/* TARGET=AARCH64 (configure.host) — handled via ffi.h's @TARGET@ expansion. */

/* Cannot use malloc for closures on this target (Android): revert to mmap
 * of an exec-writable temp file. configure.ac sets this for *-linux-android*. */
#define FFI_MMAP_EXEC_WRIT 1

/* We do NOT use the iOS/macOS-style executable trampoline table on Android. */
/* #undef FFI_EXEC_TRAMPOLINE_TABLE */

/* Use the static executable trampoline (single page of code shared by all
 * closures, mapped PROT_EXEC once). Supported on aarch64 + linux/bionic. */
#define FFI_EXEC_STATIC_TRAMP 1

/* SELinux exec-mem detection path is x86/glibc-only in closures.c and is
 * explicitly compiled out for __ANDROID__; leave undefined. */
/* #undef FFI_MMAP_EXEC_SELINUX */
/* #undef FFI_MMAP_EXEC_EMUTRAMP_PAX */

/* Cannot use PROT_EXEC on read-only mapped regions? (not relevant here) */
/* #undef FFI_MMAP_PAX */

/* Define if your assembler supports .cfi_* pseudo-ops. Clang does. */
#define HAVE_AS_CFI_PSEUDO_OP 1

/* Define if your assembler and linker support unwind section type. */
#define HAVE_AS_X86_64_UNWIND_SECTION_TYPE 0

/* Cannot use the .ascii pseudo-op? No, we can. */
/* #undef HAVE_AS_ASCII_PSEUDO_OP -> available */
#define HAVE_AS_ASCII_PSEUDO_OP 1
#define HAVE_AS_STRING_PSEUDO_OP 1

/* bionic libc feature probes (API >= 26, NDK r27 unified headers). */
#define HAVE_ALLOCA 1
#define HAVE_ALLOCA_H 1
#define HAVE_MEMCPY 1
#define HAVE_MMAP 1
#define HAVE_MMAP_ANON 1
#define HAVE_MMAP_DEV_ZERO 1
#define HAVE_MMAP_FILE 1
#define HAVE_GETPAGESIZE 1
#define HAVE_MKOSTEMP 1
/*
 * NOTE: bionic only DECLARES memfd_create() in <sys/mman.h> for API >= 30, but
 * this app targets API 26. Leaving HAVE_MEMFD_CREATE undefined makes libffi's
 * closures.c fall back to open_temp_exec_file_mkostemp() (mkostemp is API >=
 * 23), which is correct and avoids an implicit-function-declaration error.
 * (The compositor never executes libffi closures from a memfd anyway; libffi
 * is used by wayland-server only for dispatch-time ffi_call of request handlers,
 * not for creating exec closures in the hot path.)
 */
/* #undef HAVE_MEMFD_CREATE */
#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* closures.c includes <sys/memfd.h> only under HAVE_MEMFD_CREATE (now off). */
/* #undef HAVE_SYS_MEMFD_H */

/* glibc-only headers — absent on bionic. */
/* #undef HAVE_MNTENT_H */
/* #undef HAVE_MNTENT */
/* #undef HAVE_USR_INCLUDE_MALLOC_H */
/* #undef HAVE_MORECORE */
/* #undef HAVE_MREMAP */

/* Sizes for the raw-API path (LP64). */
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 16
#define SIZEOF_SIZE_T 8

/* Define if compiler supports pointer authentication (we don't enable PAC). */
/* #undef HAVE_PTRAUTH */

/* Define to 1 if GNU symver attribute is supported. */
#define HAVE_AS_GNU_SYMVER 1

/* Allow long-double to differ from double (aarch64 has 128-bit long double). */
#define HAVE_LONG_DOUBLE 1
#define HAVE_LONG_DOUBLE_VARIANT 0

/* Name of the package + version. */
#define PACKAGE "libffi"
#define PACKAGE_NAME "libffi"
#define PACKAGE_VERSION "3.5.2"
#define PACKAGE_STRING "libffi 3.5.2"
#define PACKAGE_TARNAME "libffi"
#define PACKAGE_BUGREPORT "http://github.com/libffi/libffi/issues"
#define PACKAGE_URL ""
#define VERSION "3.5.2"

/* Symbols are NOT prefixed with an underscore on ELF/aarch64. */
/* #undef SYMBOL_UNDERSCORE */

/* The ELF section flags for the .eh_frame section on Linux. */
#ifdef HAVE_AS_CFI_PSEUDO_OP
#define EH_FRAME_FLAGS "a"
#else
#define EH_FRAME_FLAGS "aw"
#endif

/* Define this if you want extra debugging. */
/* #undef FFI_DEBUG */

/* Cannot use PROT_EXEC: NO (we can, via mmap-exec-writ temp file). */

/* No Go closures needed. */
/* #undef FFI_GO_CLOSURES */

#ifdef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name) .hidden name
#else
#define FFI_HIDDEN __attribute__ ((visibility ("hidden")))
#endif
#else
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#endif
#endif

/* clang on aarch64 supports the hidden visibility attribute. */
#define HAVE_HIDDEN_VISIBILITY_ATTRIBUTE 1

#endif /* ALR_LIBFFI_FFICONFIG_H */
