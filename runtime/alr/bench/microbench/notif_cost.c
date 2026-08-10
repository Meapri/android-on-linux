/* What does a seccomp user-notification round trip actually cost?
 *
 * This is the number that decides whether raw-syscall binaries (uutils, Go)
 * can be supported without giving up the property the project exists for.
 * docs/00-product.md §5 declares them a non-goal on the reasoning that only a
 * syscall-stopping tracer can see them -- and a probe just showed that
 * reasoning was wrong: seccomp user notification installs fine once
 * no_new_privs is set (it is 0 on an Android app process, which is why an
 * earlier attempt read EPERM and was misfiled as "impossible").
 *
 * Three numbers, same syscall, same process:
 *
 *   baseline    no filter at all
 *   ALLOW       a filter that returns SECCOMP_RET_ALLOW  (filter eval only)
 *   USER_NOTIF  a filter that notifies a supervisor thread, which answers
 *               with SECCOMP_USER_NOTIF_FLAG_CONTINUE
 *
 * The supervisor is a THREAD here, not a process, which makes the measured
 * cost a lower bound -- a real implementation needs a separate process so the
 * filter cannot deadlock against its own supervisor.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
#ifndef SECCOMP_IOCTL_NOTIF_RECV
#define SECCOMP_IOCTL_NOTIF_RECV  SECCOMP_IOWR(0, struct seccomp_notif)
#define SECCOMP_IOCTL_NOTIF_SEND  SECCOMP_IOWR(1, struct seccomp_notif_resp)
#endif

/* The syscall under test: getppid has no arguments and no side effects, so the
 * measurement is the interception cost and nothing else. */
#define BENCH_NR __NR_getppid
#define ITERS    20000

static int listener = -1;
static volatile int stop_sup;

static long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000000L + t.tv_nsec;
}

/* Blocking RECV means every notification pays a wake-from-idle.  On a phone
 * that can dominate: idle-state exit plus a possible big.LITTLE migration.
 * Spin mode removes that at the cost of burning a core, which separates
 * "fundamental round-trip cost" from "this CPU was asleep". */
static int spin_mode;

static void *supervisor(void *arg)
{
    (void)arg;
    if (spin_mode) {
        int fl = fcntl(listener, F_GETFL, 0);
        fcntl(listener, F_SETFL, fl | O_NONBLOCK);
    }
    for (;;) {
        struct seccomp_notif req;
        struct seccomp_notif_resp resp;
        memset(&req, 0, sizeof req);
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) {
            if (stop_sup) break;
            if (errno == EINTR) continue;
            if (spin_mode && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            break;
        }
        memset(&resp, 0, sizeof resp);
        resp.id = req.id;
        resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) != 0 && errno != ENOENT)
            break;
    }
    return NULL;
}

static int install(unsigned action, int want_listener)
{
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),                  /* nr */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, BENCH_NR, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, action),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { .len = 4, .filter = f };
    return (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                        want_listener ? SECCOMP_FILTER_FLAG_NEW_LISTENER : 0,
                        &prog);
}

static long measure(const char *label)
{
    long t0, t1;
    int i;
    for (i = 0; i < 1000; i++) syscall(BENCH_NR);      /* warm */
    t0 = now_ns();
    for (i = 0; i < ITERS; i++) syscall(BENCH_NR);
    t1 = now_ns();
    printf("  %-22s %6.0f ns/call\n", label, (double)(t1 - t0) / ITERS);
    return (t1 - t0) / ITERS;
}

int main(void)
{
    pthread_t th;
    long base, allow, notif;

    /* Unbuffered: the process ends with _exit() so the filter cannot be hit by
     * atexit machinery, and _exit does not flush stdio -- an earlier run lost
     * every line to that. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        printf("no_new_privs: %s\n", strerror(errno));
        return 1;
    }

    base = measure("baseline (no filter)");

    if (install(SECCOMP_RET_ALLOW, 0) != 0) {
        printf("  ALLOW filter: %s\n", strerror(errno));
        return 1;
    }
    allow = measure("+ RET_ALLOW filter");

    listener = install(SECCOMP_RET_USER_NOTIF, 1);
    if (listener < 0) {
        printf("  listener: %s\n", strerror(errno));
        return 1;
    }
    if (pthread_create(&th, NULL, supervisor, NULL) != 0) {
        printf("  supervisor thread failed\n");
        return 1;
    }
    notif = measure("+ USER_NOTIF blocking");
    stop_sup = 1;
    /* No join: the supervisor is parked in a blocking RECV and nothing will
     * wake it now that the measured loop has stopped.  An earlier version
     * joined here and hung the whole run.  The process is about to _exit. */

    printf("\n  filter eval overhead   %+ld ns\n", allow - base);
    printf("  notification overhead  %+ld ns  (%.1fx baseline)\n",
           notif - base, base ? (double)notif / (double)base : 0.0);
    printf("\n  NOTE the supervisor is a thread in the same process; a real one\n");
    printf("  must be a separate process, so treat this as a LOWER BOUND.\n");
    _exit(0);
}
