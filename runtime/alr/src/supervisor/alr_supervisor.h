/* alr_supervisor.h — signal-only ptrace supervisor.
 *
 * Exists for exactly one reason: Android's zygote seccomp filter blocks
 * set_robust_list(99) with SECCOMP_RET_TRAP, and glibc's __tls_init_tp() calls
 * it from inside ld.so BEFORE any DSO constructor runs.  No in-process handler
 * can be installed in time, and a stacked filter cannot relax a TRAP.  So the
 * rescue has to come from the parent.  (docs/adr/0001)
 *
 * ABSOLUTE RULES — violating any of these turns this into PRoot:
 *   1. never call PTRACE_SYSCALL
 *   2. never install a seccomp filter of our own
 *   3. never read or write guest memory (path mediation is the preload's job)
 *   4. handle only signal-delivery-stops and ptrace event-stops
 */
#ifndef ALR_SUPERVISOR_H
#define ALR_SUPERVISOR_H

#include <stddef.h>
#include <sys/types.h>

struct alr_sup_stats {
    unsigned long sigsys_seen;        /* SIGSYS deliveries observed        */
    unsigned long sigsys_emulated;    /* ...of which we emulated           */
    unsigned long sigsys_passthrough; /* ...not SYS_SECCOMP: given to guest */
    unsigned long signals_forwarded;
    unsigned long tracees_seen;
    unsigned long group_stops;
    /* Invariants: both MUST stay 0.  Non-zero means someone introduced
     * PTRACE_SYSCALL and the entire performance claim is void. */
    unsigned long syscall_stops;
    unsigned long path_traps;
    /* docs/03-supervisor-spec.md §6 names both of these in the stats line and
     * neither existed.  passthrough_signals is the §4.3 rule made visible: a
     * signal we did NOT originate is delivered unchanged.  elapsed_ms is the
     * supervised wall time -- the denominator for every other counter here,
     * and without it "sigsys=22" has no scale. */
    unsigned long passthrough_signals;
    unsigned long elapsed_ms;
};

struct alr_sup_opts {
    const char *path;          /* host path to execve                    */
    char *const *argv;
    char *const *envp;
    int   log_level;           /* 0 quiet, 1 warn, 2 trace               */
    int   log_fd;              /* diagnostics fd; -1 -> stderr           */
    unsigned long runaway_cap; /* 0 -> default 1<<20                     */
};

/* Fork, attach, exec, and supervise until the leader exits.
 * Returns the leader's exit status in wait(2) encoding via *status, and 0 on
 * success / -1 on setup failure. */
int alr_supervise(const struct alr_sup_opts *o, int *status,
                  struct alr_sup_stats *st);

/* wait(2) status -> shell-style exit code (128+sig when signalled). */
int alr_exit_code(int status);

/* Emulated return value for a blocked syscall; ALR_SIGSYS_DEFAULT_RET when the
 * number is not in the measured table.  Exposed for unit testing. */
long alr_sigsys_emulate(long nr);

#endif /* ALR_SUPERVISOR_H */
