/* Can we intercept a RAW syscall -- an inline `svc` that never touches libc --
 * without paying PRoot's per-syscall ptrace round trip?
 *
 * uutils coreutils (Ubuntu 26.04's default) and every Go binary issue syscalls
 * that way, and docs/00-product.md §5 lists them as a non-goal on the grounds
 * that only a syscall-stopping tracer can see them.  This probe tests whether
 * that reasoning still holds on this kernel, because two mechanisms would
 * falsify it:
 *
 *   1. PR_SET_SYSCALL_USER_DISPATCH (SUD, Linux 5.11 on x86; arm64 later).
 *      Syscalls issued from outside a designated PC range raise SIGSYS in the
 *      SAME thread -- no tracer, no context switch.  A per-thread byte toggles
 *      it, so our own libc calls stay at full speed.
 *
 *   2. seccomp user notification (SECCOMP_FILTER_FLAG_NEW_LISTENER).
 *      Filters stack and the kernel takes the NUMERICALLY LOWEST action:
 *      USER_NOTIF is 0x7fc00000 and ALLOW is 0x7fff0000, so for any syscall
 *      the zygote filter ALLOWs, ours would win.  Whether we may install a
 *      filter at all is the open question.
 *
 * Prints one line per capability so the result is greppable.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#endif
#ifndef PR_SYS_DISPATCH_OFF
#define PR_SYS_DISPATCH_OFF 0
#define PR_SYS_DISPATCH_ON  1
#endif
#ifndef SYSCALL_DISPATCH_FILTER_ALLOW
#define SYSCALL_DISPATCH_FILTER_ALLOW 0
#define SYSCALL_DISPATCH_FILTER_BLOCK 1
#endif
#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
/* offsetof on struct seccomp_data inside a designated initialiser upsets some
 * gcc versions; the nr field is first, so the offset is 0. */
#define SECCOMP_DATA_NR_OFFSET 0

#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif

static volatile char sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
static volatile int  sud_hits;
static volatile int  sud_last_nr;

/* A raw `svc #0` that libc never sees.  This is what uutils and Go emit. */
static long raw_syscall(long nr, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void sud_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    sud_hits++;
    sud_last_nr = si->si_syscall;
    /* Emulate: report success without re-issuing.  A real implementation
     * would flip the selector to ALLOW, rewrite the path argument, re-issue,
     * and flip back -- all in-thread. */
    {
        ucontext_t *u = (ucontext_t *)uc;
        u->uc_mcontext.regs[0] = 0;
    }
}

int main(void)
{
    struct sigaction sa;
    long r;

    printf("kernel   : ");
    fflush(stdout);
    { char b[128]; FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
      if (f && fgets(b, sizeof b, f)) { b[strcspn(b, "\n")] = 0; printf("%s\n", b); }
      else printf("(unknown)\n");
      if (f) fclose(f); }

    {   /* seccomp() needs CAP_SYS_ADMIN or no_new_privs.  An app process may
         * not have NNP set, and EPERM would then be OUR omission, not policy. */
        char b[256]; FILE *f = fopen("/proc/self/status", "r");
        printf("status   : ");
        while (f && fgets(b, sizeof b, f))
            if (!strncmp(b, "NoNewPrivs:", 11) || !strncmp(b, "Seccomp:", 8)) {
                b[strcspn(b, "\n")] = 0;
                printf("%s  ", b);
            }
        if (f) fclose(f);
        printf("\n");
        errno = 0;
        printf("set NNP  : %s\n",
               prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 ? "ok" : strerror(errno));
    }

    /* ── 1. SUD availability ─────────────────────────────────────────── */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sud_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);

    /* Try a real exemption range as well as the empty one: EINVAL on the
     * empty range would be an argument mistake, not a missing feature. */
    errno = 0;
    r = prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON,
              (unsigned long)&raw_syscall, 4096UL, &sud_selector);
    if (r != 0) {
        int e1 = errno;
        errno = 0;
        r = prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON,
                  0, 0, &sud_selector);
        if (r != 0)
            printf("SUD      : UNAVAILABLE (range=%s empty=%s)\n",
                   strerror(e1), strerror(errno));
    }
    if (r != 0) {
        /* already reported */
    } else {
        printf("SUD      : ACCEPTED\n");
        /* Selector is ALLOW, so nothing should trap yet. */
        sud_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
        sud_hits = 0;
        /* getppid via raw svc -- nr 173 on aarch64 */
        raw_syscall(173, 0, 0, 0);
        sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
        prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_OFF, 0, 0, 0);
        printf("SUD trap : %s (hits=%d nr=%d)\n",
               sud_hits ? "RAW SYSCALL INTERCEPTED" : "no trap", sud_hits, sud_last_nr);
    }

    /* ── 2. seccomp user notification ────────────────────────────────── */
    {
        struct sock_filter f[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SECCOMP_DATA_NR_OFFSET),
            /* notify only on a syscall nothing here uses, so a granted
             * listener cannot wedge this process */
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sync, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = { .len = sizeof f / sizeof f[0], .filter = f };
        errno = 0;
        r = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                    SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
        if (r < 0) printf("NOTIF    : UNAVAILABLE (%s)\n", strerror(errno));
        else       { printf("NOTIF    : LISTENER fd=%ld\n", r); close((int)r); }
    }

    /* ── 3. plain filter install (is seccomp() reachable at all?) ────── */
    {
        struct sock_filter f[] = { BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
        struct sock_fprog prog = { .len = 1, .filter = f };
        errno = 0;
        r = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
        printf("FILTER   : %s\n", r == 0 ? "INSTALLED" : strerror(errno));
    }
    return 0;
}
