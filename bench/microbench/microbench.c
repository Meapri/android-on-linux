/*
 * microbench.c — M1 same-binary CPU-overhead probe (ALR WS-5).
 *
 * One small, portable, static-linkable C99 binary that is run TWICE:
 *   (1) native on the device via `adb shell`, and
 *   (2) through the ALR native loader (the APK's loader path),
 * so the two `ns_per_op` numbers can be diffed into a CPU-overhead percentage
 * by `python -m bench overhead`.
 *
 * Two workloads, selected by argv[1]:
 *   "compute" (default) — a volatile arithmetic busy-loop that issues NO
 *       syscalls. Pure userspace CPU work, so ALR's ptrace/seccomp mediation
 *       never fires and the overhead should approach 0% (§0 gated < 5%).
 *   "syscall"           — a loop issuing a real syscall (SYS_getpid) every
 *       iteration. A syscall storm that stresses ALR's ptrace round-trip /
 *       seccomp mediation; expected to be high (reported, not gated — the
 *       known L1 ptrace wall).
 *
 * Timing uses clock_gettime(CLOCK_MONOTONIC) around the loop only.
 * Exactly one result line is printed:
 *   MICROBENCH mode=%s iters=%ld ns=%lld ns_per_op=%.2f
 *
 * No external libraries; only the listed standard headers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define DEFAULT_COMPUTE_ITERS 50000000L
#define DEFAULT_SYSCALL_ITERS 1000000L

static long long elapsed_ns(const struct timespec *start, const struct timespec *end) {
    long long s = (long long)(end->tv_sec - start->tv_sec);
    long long n = (long long)(end->tv_nsec - start->tv_nsec);
    return s * 1000000000LL + n;
}

int main(int argc, char **argv) {
    const char *mode = "compute";
    if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') {
        mode = argv[1];
    }

    int is_syscall = (strcmp(mode, "syscall") == 0);

    long iters;
    if (argc > 2 && argv[2] != NULL && argv[2][0] != '\0') {
        iters = strtol(argv[2], NULL, 10);
        if (iters <= 0) {
            iters = is_syscall ? DEFAULT_SYSCALL_ITERS : DEFAULT_COMPUTE_ITERS;
        }
    } else {
        iters = is_syscall ? DEFAULT_SYSCALL_ITERS : DEFAULT_COMPUTE_ITERS;
    }

    struct timespec start, end;
    /* volatile sink so the optimizer cannot elide the busy-loop. */
    volatile unsigned long long sink = 0;

    clock_gettime(CLOCK_MONOTONIC, &start);
    if (is_syscall) {
        for (long i = 0; i < iters; i++) {
            /* a real syscall each iteration — the ptrace/seccomp storm. */
            sink += (unsigned long long)syscall(SYS_getpid);
        }
    } else {
        for (long i = 0; i < iters; i++) {
            /* pure userspace arithmetic — no syscalls at all. */
            sink += (unsigned long long)i * 2654435761ULL + 0x9E3779B9ULL;
            sink ^= sink >> 13;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long long ns = elapsed_ns(&start, &end);
    double ns_per_op = iters > 0 ? (double)ns / (double)iters : 0.0;

    /* Keep `sink` observable so the loop is not dead-code-eliminated. */
    if (sink == 0xFFFFFFFFFFFFFFFFULL) {
        fprintf(stderr, "sink=%llu\n", sink);
    }

    printf("MICROBENCH mode=%s iters=%ld ns=%lld ns_per_op=%.2f\n",
           mode, iters, ns, ns_per_op);
    return 0;
}
