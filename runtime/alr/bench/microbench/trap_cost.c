/* What does an in-process SECCOMP_RET_TRAP round trip cost?
 *
 * Spike S2 of docs/adr/0004-unified-direction.md (D6).  notif_cost.c measured
 * the out-of-thread lane (USER_NOTIF, 154 us) and ADR 0006 rejected it.  This
 * measures the lane ADR 0006 did not evaluate: the kernel turns the syscall
 * into a SIGSYS on the SAME thread, the handler does the work, and anything
 * that must really reach the kernel is re-issued from a trampoline whose PC
 * the filter allows.  That is the shape of tawcroot
 * (third_party/tawcroot/src/filter.c), stripped of its path translation so
 * the number is the mechanism and nothing else.
 *
 * Same syscall as notif_cost.c (getppid: no arguments, no side effects), same
 * process, one filter:
 *
 *   nr != getppid                 -> ALLOW
 *   PC == trampoline return label -> ALLOW
 *   otherwise                     -> TRAP
 *
 * Four numbers:
 *
 *   baseline      plain svc, no filter of ours
 *   tramp ALLOW   svc from the trampoline after the filter is installed
 *                 (filter evaluation incl. the IP compare, no signal)
 *   TRAP emulate  svc from elsewhere; the handler writes the result into the
 *                 saved x0/rax and returns (signal delivery + sigreturn)
 *   TRAP reissue  same, but the handler re-issues getppid from the
 *                 trampoline (the tawcroot shape; this is the D6 number)
 *
 * The ADR 0004 D6 table is applied to (TRAP reissue - baseline).
 *
 * Only a process forked from the Termux app is a valid context: adb shell and
 * run-as carry no zygote filter (scripts/dev-bootstrap.md).  The context is
 * printed so the output is self-describing, and the verdict is withheld when
 * the context is wrong.
 *
 * usage: trap_cost [cpu]     pin to that CPU first (big/LITTLE matters)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#define BENCH_NR __NR_getppid
#define ITERS    20000
#define ROUNDS   9

/* long tramp_syscall0(long nr): issue syscall nr with no arguments.  The
 * kernel reports the PC *after* the syscall instruction in
 * seccomp_data.instruction_pointer, so the label the filter allows is
 * tramp_ret, not the svc itself (same contract as tawcroot's
 * tawcroot_raw_syscall_ret). */
#if defined(__aarch64__)
__asm__(".text\n"
        ".globl tramp_syscall0\n"
        ".type tramp_syscall0, %function\n"
        "tramp_syscall0:\n"
        "    mov x8, x0\n"
        "    svc #0\n"
        ".globl tramp_ret\n"
        "tramp_ret:\n"
        "    ret\n"
        ".size tramp_syscall0, .-tramp_syscall0\n");
#define AUDIT_ARCH_SELF AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
__asm__(".text\n"
        ".globl tramp_syscall0\n"
        ".type tramp_syscall0, @function\n"
        "tramp_syscall0:\n"
        "    mov %rdi, %rax\n"
        "    syscall\n"
        ".globl tramp_ret\n"
        "tramp_ret:\n"
        "    ret\n"
        ".size tramp_syscall0, .-tramp_syscall0\n");
#define AUDIT_ARCH_SELF AUDIT_ARCH_X86_64
#else
#error "aarch64 or x86_64 only"
#endif

long tramp_syscall0(long nr);
extern char tramp_ret[];

/* The measured loop calls this, not syscall(3): libc's wrapper is a PC the
 * filter does not allow, which is exactly the "guest issued it" case. */
static inline long plain_getppid(void)
{
#if defined(__aarch64__)
    register long x8 __asm__("x8") = BENCH_NR;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
#else
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"((long)BENCH_NR)
                     : "rcx", "r11", "memory");
    return ret;
#endif
}

static volatile int reissue;
static long cached_ppid;
static volatile long traps;

static void on_sigsys(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    long ret;
    (void)sig;
    if (si->si_syscall != BENCH_NR) return;
    ret = reissue ? tramp_syscall0(BENCH_NR) : cached_ppid;
#if defined(__aarch64__)
    uc->uc_mcontext.regs[0] = (unsigned long long)ret;
#else
    uc->uc_mcontext.gregs[REG_RAX] = ret;
#endif
    traps++;
}

static int install_filter(void)
{
    uint64_t ip = (uint64_t)(uintptr_t)tramp_ret;
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_SELF, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, BENCH_NR, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)ip, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, instruction_pointer) + 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)(ip >> 32), 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
    };
    struct sock_fprog prog = { .len = sizeof f / sizeof f[0], .filter = f };
    return (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

static long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000000L + t.tv_nsec;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

/* Median of ROUNDS rounds of ITERS calls; one call per op so the per-op
 * number is directly comparable with notif_cost.c. */
static long measure(const char *label, int via_tramp)
{
    long r[ROUNDS];
    int k, i;
    for (i = 0; i < 1000; i++)
        via_tramp ? tramp_syscall0(BENCH_NR) : plain_getppid();
    for (k = 0; k < ROUNDS; k++) {
        long t0 = now_ns();
        if (via_tramp)
            for (i = 0; i < ITERS; i++) tramp_syscall0(BENCH_NR);
        else
            for (i = 0; i < ITERS; i++) plain_getppid();
        r[k] = (now_ns() - t0) / ITERS;
    }
    qsort(r, ROUNDS, sizeof r[0], cmp_long);
    printf("  %-24s %7ld ns/call  (median of %d; min %ld, max %ld)\n",
           label, r[ROUNDS / 2], ROUNDS, r[0], r[ROUNDS - 1]);
    return r[ROUNDS / 2];
}

/* "Seccomp:" and "Seccomp_filters:" from /proc/self/status, -1 if absent. */
static void read_seccomp(int *mode, int *filters)
{
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    *mode = *filters = -1;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Seccomp:", 8)) *mode = atoi(line + 8);
        else if (!strncmp(line, "Seccomp_filters:", 16)) *filters = atoi(line + 16);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    char ctx[256] = "?";
    int mode, filters, valid;
    long base, allow, emul, reiss, over;
    FILE *f;

    /* Unbuffered and _exit at the end, as in notif_cost.c: nothing after the
     * measurement should run through the filter. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(atoi(argv[1]), &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) {
            printf("pin cpu %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
    }

    if ((f = fopen("/proc/self/attr/current", "r"))) {
        if (!fgets(ctx, sizeof ctx, f)) strcpy(ctx, "?");
        ctx[strcspn(ctx, "\n")] = 0;
        fclose(f);
    }
    read_seccomp(&mode, &filters);
    valid = getuid() >= 10000 && mode == 2;
    printf("context: uid=%u ctx=%s Seccomp=%d Seccomp_filters=%d cpu=%s %s\n",
           (unsigned)getuid(), ctx, mode, filters, argc > 1 ? argv[1] : "any",
           valid ? "" : "(NOT an app context: verdict withheld)");

    cached_ppid = getppid();
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, NULL) != 0) {
        printf("sigaction: %s\n", strerror(errno));
        return 1;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        printf("no_new_privs: %s\n", strerror(errno));
        return 1;
    }

    base = measure("baseline", 0);

    if (install_filter() != 0) {
        printf("  TRAP filter: %s\n", strerror(errno));
        return 1;
    }
    allow = measure("+ filter, tramp ALLOW", 1);

    traps = 0;
    reissue = 0;
    emul = measure("+ TRAP, emulate", 0);
    if (traps == 0) {
        printf("  FAIL: no SIGSYS delivered; the filter did not trap\n");
        _exit(1);
    }

    traps = 0;
    reissue = 1;
    reiss = measure("+ TRAP, reissue", 0);
    if (traps == 0) {
        printf("  FAIL: no SIGSYS delivered; the filter did not trap\n");
        _exit(1);
    }
    if (tramp_syscall0(BENCH_NR) != cached_ppid || plain_getppid() != cached_ppid) {
        printf("  FAIL: trapped getppid returned the wrong value\n");
        _exit(1);
    }

    over = reiss - base;
    printf("\n  filter eval overhead    %+ld ns\n", allow - base);
    printf("  signal round trip       %+ld ns  (emulate - baseline)\n", emul - base);
    printf("  D6 round-trip cost      %+ld ns  (reissue - baseline)\n", over);
    if (!valid)
        printf("  D6 verdict              withheld (not an app context)\n");
    else
        printf("  D6 verdict              %s\n",
               over <= 5000  ? "PROCEED (<= 5 us)" :
               over <= 31000 ? "OPTIONAL (5-31 us)" : "DROP (> 31 us)");
    _exit(0);
}
