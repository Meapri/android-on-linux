/* Where does argv[0] differ between main() and an .init_array constructor?
 *
 * Ubuntu 26.04 ships uutils coreutils as a multicall binary that dispatches on
 * its own name, and under alr it reported 'ld-linux-aarch64.so' -- the loader,
 * with Rust's file_stem() having eaten the ".1".  Yet bash reports $0 correctly
 * as /bin/bash, so ld.so's --argv0 clearly works for main().
 *
 * Rust's std captures argc/argv in an .init_array callback rather than from
 * main(), so the question is whether glibc hands the SAME argv to both.  This
 * prints all three views.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>

static const char *ctor_argv0 = "(not captured)";
static int ctor_argc = -1;

__attribute__((constructor))
static void cap(int argc, char **argv, char **envp)
{
    (void)envp;
    ctor_argc = argc;
    if (argc > 0 && argv && argv[0]) ctor_argv0 = argv[0];
}

extern char *program_invocation_name;
extern char *program_invocation_short_name;

int main(int argc, char **argv)
{
    char exe[512];
    ssize_t n;

    printf("main       argc=%d argv[0]=%s\n", argc, argv[0] ? argv[0] : "(null)");
    printf("init_array argc=%d argv[0]=%s\n", ctor_argc, ctor_argv0);
    printf("prog_inv_name       =%s\n", program_invocation_name);
    printf("prog_inv_short_name =%s\n", program_invocation_short_name);
    {   /* AT_EXECFN is the path the KERNEL was handed at execve.  Invoking the
         * loader explicitly (ADR 0002) makes that the loader, and nothing in
         * userspace can change it after the fact -- unlike argv[0] or
         * /proc/self/exe, both of which we already correct. */
        unsigned long fn = getauxval(AT_EXECFN);
        printf("AT_EXECFN           =%s\n", fn ? (const char *)fn : "(unset)");
        printf("AT_BASE set         =%s\n", getauxval(AT_BASE) ? "yes" : "no");
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = '\0'; printf("/proc/self/exe      =%s\n", exe); }
    else        printf("/proc/self/exe      =(readlink failed)\n");
    return 0;
}
