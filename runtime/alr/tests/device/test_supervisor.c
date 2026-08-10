/* M2 acceptance suite for the signal-only supervisor.
 *
 * Self-hosting: run with no arguments it acts as the supervisor and re-execs
 * itself as `test_supervisor child <n>` for each case.  Must run inside a real
 * Termux context (uid>=10000, Seccomp==2) or the SIGSYS cases are vacuous —
 * it refuses to run otherwise.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "alr_supervisor.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NR_set_robust_list 99
#define NR_rseq            293
#define NR_io_uring_setup  425
#define NR_mount           40

static int pass, fail;
static char self[4096];

static void ck(int ok, const char *name, const char *detail)
{
    printf("%-40s %s", name, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf("  (%s)", detail);
    printf("\n");
    ok ? pass++ : fail++;
}

/* ── the child side ──────────────────────────────────────────────────── */

static int child_main(int n)
{
    switch (n) {
    case 1:   /* set_robust_list must be emulated as SUCCESS (0).
               * This is the syscall the whole project exists for. */
        return syscall(NR_set_robust_list, NULL, 0) == 0 ? 0 : 1;

    case 2: { /* an unlisted blocked syscall must come back -ENOSYS so glibc's
               * fallback chains engage */
        long r = syscall(NR_rseq, NULL, 0, 0, 0);
        return (r == -1 && errno == ENOSYS) ? 0 : 1;
    }
    case 3: { /* io_uring_setup: Node>=20/libuv>=1.45 dies here without us */
        long r = syscall(NR_io_uring_setup, 1, NULL);
        return (r == -1 && errno == ENOSYS) ? 0 : 1;
    }
    case 4: { /* privileged op maps to EPERM, not ENOSYS: glibc has no
               * ENOSYS-keyed fallback for these and EPERM is what an
               * unprivileged process would really see */
        long r = syscall(NR_mount, "a", "b", "c", 0UL, NULL);
        return (r == -1 && errno == EPERM) ? 0 : 1;
    }
    case 5:   /* RESTART-LOOP GUARD.  arg0 == -513 (-ERESTARTNOINTR) is the
               * value that makes the kernel rewind pc to the svc.  Without
               * regs[PC] = si_call_addr this hangs forever. */
        return syscall(NR_set_robust_list, (void *)-513L, (size_t)-513L) == 0 ? 0 : 1;

    case 6: { /* a guest's OWN SIGSYS (si_code != SYS_SECCOMP) must reach it */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGSYS, &sa, NULL);
        raise(SIGSYS);              /* si_code = SI_TKILL, not SYS_SECCOMP */
        return 0;                   /* survived => it was passed through */
    }
    case 7: { /* descendants must be tracked and rescued too */
        pid_t p = fork();
        int st;
        if (p == 0) _exit(syscall(NR_set_robust_list, NULL, 0) == 0 ? 0 : 1);
        if (waitpid(p, &st, 0) < 0) return 1;
        return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
    }
    case 8: { /* grandchild through execve: filter survives exec, so must we */
        pid_t p = fork();
        int st;
        if (p == 0) {
            char *av[] = { self, (char *)"child", (char *)"1", NULL };
            execv(self, av);
            _exit(127);
        }
        if (waitpid(p, &st, 0) < 0) return 1;
        return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
    }
    case 9:   /* exit code propagation */
        return 42;

    case 10:  /* threads: each new thread hits set_robust_list at creation.
               * Also the case that group-stops the guest if the startup
               * SIGSTOP is re-injected. */
        {
            pid_t p;
            int i, st, bad = 0;
            for (i = 0; i < 8; i++) {
                p = fork();
                if (p == 0) _exit(syscall(NR_set_robust_list, NULL, 0) == 0 ? 0 : 1);
                if (waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st))
                    bad = 1;
            }
            return bad;
        }
    default:
        return 99;
    }
}

/* ── the supervisor side ─────────────────────────────────────────────── */

static struct alr_sup_stats g_total;

static void accum(const struct alr_sup_stats *s)
{
    g_total.sigsys_seen        += s->sigsys_seen;
    g_total.sigsys_emulated    += s->sigsys_emulated;
    g_total.sigsys_passthrough += s->sigsys_passthrough;
    g_total.signals_forwarded  += s->signals_forwarded;
    g_total.tracees_seen       += s->tracees_seen;
    g_total.group_stops        += s->group_stops;
    g_total.syscall_stops      += s->syscall_stops;
    g_total.path_traps         += s->path_traps;
}

static int run_case(int n, struct alr_sup_stats *st)
{
    char num[16];
    char *argv[4];
    struct alr_sup_opts o;
    int status = 0;

    snprintf(num, sizeof num, "%d", n);
    argv[0] = self; argv[1] = (char *)"child"; argv[2] = num; argv[3] = NULL;

    memset(&o, 0, sizeof o);
    o.path = self;
    o.argv = argv;
    o.envp = environ;
    o.log_level = getenv("ALR_LOG") ? atoi(getenv("ALR_LOG")) : 0;
    o.log_fd = -1;

    if (alr_supervise(&o, &status, st) < 0) return -1;
    accum(st);
    return alr_exit_code(status);
}

static int context_ok(void)
{
    FILE *f;
    char line[256];
    int mode = -1;
    if (getuid() < 10000) {
        fprintf(stderr, "REFUSING: uid=%u < 10000 — not an app process.\n", getuid());
        return 0;
    }
    f = fopen("/proc/self/status", "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "Seccomp:", 8)) { mode = atoi(line + 8); break; }
        fclose(f);
    }
    if (mode != 2) {
        fprintf(stderr, "REFUSING: Seccomp=%d, expected 2. Results would be "
                        "false PASSes (see scripts/dev-bootstrap.md).\n", mode);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct alr_sup_stats st;
    ssize_t n;

    if (argc >= 3 && !strcmp(argv[1], "child")) {
        n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) self[n] = '\0';
        return child_main(atoi(argv[2]));
    }

    n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) { perror("readlink"); return 2; }
    self[n] = '\0';

    if (!context_ok()) return 2;
    printf("supervisor acceptance — uid=%u Seccomp=2\n\n", getuid());

    ck(run_case(1, &st) == 0, "SUPERVISOR SIGSYS SET_ROBUST_LIST:", "must return 0");
    ck(st.sigsys_emulated > 0, "SUPERVISOR TRACEME HANDSHAKE:", "no SIGSYS seen at all");
    ck(run_case(2, &st) == 0, "SUPERVISOR SIGSYS ENOSYS DEFAULT:", "want ENOSYS");
    ck(run_case(3, &st) == 0, "SUPERVISOR SIGSYS IO_URING:", "want ENOSYS");
    ck(run_case(4, &st) == 0, "SUPERVISOR SIGSYS EPERM MAPPING:", "want EPERM");
    ck(run_case(5, &st) == 0, "SUPERVISOR SIGSYS RESTART LOOP GUARD:", "hung or wrong");
    ck(run_case(6, &st) == 0 && st.sigsys_passthrough > 0,
       "SUPERVISOR SIGSYS PASSTHROUGH:", "guest SIGSYS was swallowed");
    ck(run_case(7, &st) == 0, "SUPERVISOR CHILD TRACKING:", "fork child not rescued");
    ck(run_case(8, &st) == 0, "SUPERVISOR EXEC TRACKING:", "exec grandchild not rescued");
    ck(run_case(9, &st) == 42, "SUPERVISOR EXIT CODE:", "want 42");
    ck(run_case(10, &st) == 0, "SUPERVISOR MULTI CHILD:", "some child not rescued");

    {   /* The invariants that separate this from PRoot. Checked against the
         * ACCUMULATED totals, not just the last case. */
        int inv = (g_total.syscall_stops == 0 && g_total.path_traps == 0);
        ck(inv, "SUPERVISOR NO SYSCALL STOPS:", "PTRACE_SYSCALL was used");
        printf("\n  totals over all cases:\n"
               "    sigsys_seen=%lu emulated=%lu passthrough=%lu\n"
               "    tracees=%lu group_stops=%lu\n"
               "    syscall_stops=%lu path_traps=%lu   <- both MUST be 0\n",
               g_total.sigsys_seen, g_total.sigsys_emulated,
               g_total.sigsys_passthrough, g_total.tracees_seen,
               g_total.group_stops, g_total.syscall_stops, g_total.path_traps);
    }

    printf("\n  %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
