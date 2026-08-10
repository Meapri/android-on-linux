/* Does a DIRECT syscall(2) get its path rewritten?
 *
 * docs/04-preload-spec.md §10 says it must: a program that bypasses the libc
 * wrapper and calls syscall(SYS_openat, ...) itself still goes through ours,
 * because we interpose `syscall` too and path_arg_mask() knows which argument
 * of which number is a path.
 *
 * WHY THIS IS A C PROBE AND NOT A ONE-LINE PYTHON CHECK.  The first version of
 * this test used ctypes:
 *
 *     libc = ctypes.CDLL("libc.so.6"); libc.syscall(56, -100, b"/etc/...", ...)
 *
 * and it failed with ENOENT while the C equivalent returned a valid fd.  That
 * is not a bug in the interposer -- CDLL does dlopen+dlsym on libc's OWN
 * handle, so it resolves libc's `syscall` directly and never consults the
 * global scope where LD_PRELOAD lives.  The check was measuring dlsym
 * semantics, not path rewriting.
 *
 * Prints "syscall-rewrite-ok" on success.  /etc/os-release exists only in the
 * guest; Android has no such file, so opening it by that path can only work if
 * the rewrite happened.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    long fd = syscall(SYS_openat, AT_FDCWD, "/etc/os-release", O_RDONLY, 0);
    if (fd < 0) { printf("syscall-rewrite-FAILED\n"); return 1; }
    close((int)fd);
    printf("syscall-rewrite-ok\n");
    return 0;
}
