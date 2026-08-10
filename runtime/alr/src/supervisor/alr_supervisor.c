#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "alr_supervisor.h"
#include "alr_sigsys_table.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>          /* uintptr_t: bionic pulls this in transitively,
                              * glibc does not -- a cross-build fails without it */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef __WNOTHREAD
#define __WNOTHREAD 0x20000000
#endif
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK   0x00000002
#define PTRACE_O_TRACEVFORK  0x00000004
#define PTRACE_O_TRACECLONE  0x00000008
#define PTRACE_O_TRACEEXEC   0x00000010
#define PTRACE_O_TRACEEXIT   0x00000040
#endif
#ifndef PTRACE_O_EXITKILL
#define PTRACE_O_EXITKILL    0x00100000
#endif
#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK    1
#define PTRACE_EVENT_VFORK   2
#define PTRACE_EVENT_CLONE   3
#define PTRACE_EVENT_EXEC    4
#define PTRACE_EVENT_EXIT    6
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef AUDIT_ARCH_AARCH64
#define AUDIT_ARCH_AARCH64 0xc00000b7u
#endif
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

/* aarch64 NT_PRSTATUS layout (struct user_pt_regs):
 *   [0..30] x0..x30, [31] sp, [32] pc, [33] pstate */
#define R_RET    0
#define R_NR     8
#define R_PC     32
#define ALR_NREGS 34

#define ALR_MAX_TRACEES 512
#define ALR_DEFAULT_RUNAWAY (1UL << 20)

/* ── tid registry ────────────────────────────────────────────────────────
 * Two paths can teach us about a tid and they race: the parent's
 * PTRACE_EVENT_{FORK,CLONE} and the child's own first stop.  State NEW means
 * "startup SIGSTOP not yet consumed". */
enum tstate { T_FREE = 0, T_NEW, T_RUNNING };

struct sup {
    pid_t tid[ALR_MAX_TRACEES];
    unsigned char st[ALR_MAX_TRACEES];
    int n;
    int log_level, log_fd;
    unsigned long runaway_cap;
    struct alr_sup_stats *stats;
};

static void slog(struct sup *s, int lvl, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    if (lvl > s->log_level) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t w = write(s->log_fd >= 0 ? s->log_fd : 2, buf, (size_t)n);
        (void)w;
    }
}

static int find(struct sup *s, pid_t t)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->tid[i] == t && s->st[i] != T_FREE) return i;
    return -1;
}

static int add(struct sup *s, pid_t t, enum tstate state)
{
    int i = find(s, t);
    if (i >= 0) return i;
    for (i = 0; i < s->n; i++)
        if (s->st[i] == T_FREE) goto put;
    if (s->n >= ALR_MAX_TRACEES) return -1;
    i = s->n++;
put:
    s->tid[i] = t;
    s->st[i] = (unsigned char)state;
    if (s->stats) s->stats->tracees_seen++;
    return i;
}

static void drop(struct sup *s, pid_t t)
{
    int i = find(s, t);
    if (i >= 0) s->st[i] = T_FREE;
}

static int live(struct sup *s)
{
    int i, n = 0;
    for (i = 0; i < s->n; i++) if (s->st[i] != T_FREE) n++;
    return n;
}

/* ── emulation table lookup ──────────────────────────────────────────── */

long alr_sigsys_emulate(long nr)
{
    size_t i;
    for (i = 0; i < ALR_SIGSYS_TAB_LEN; i++)
        if (alr_sigsys_tab[i].nr == nr) return alr_sigsys_tab[i].ret;
    /* Everything else gets -ENOSYS, because glibc's "try the new syscall,
     * fall back on ENOSYS" pattern keys on exactly that value.  EPERM or
     * EINVAL here would stop the fallback from engaging. */
    return ALR_SIGSYS_DEFAULT_RET;
}

static const char *sigsys_name(long nr)
{
    size_t i;
    for (i = 0; i < ALR_SIGSYS_TAB_LEN; i++)
        if (alr_sigsys_tab[i].nr == nr) return alr_sigsys_tab[i].name;
    return "?";
}

/* ── SIGSYS handling ─────────────────────────────────────────────────── */

/* Returns 1 if we emulated (caller continues with sig 0), 0 to pass through. */
/* Syscalls that carry a PATH, used to split syscall stops into the two numbers
 * docs/03-supervisor-spec.md §6 specifies.
 *
 * NOTE WHAT THIS IS NOT.  A first version counted these on the SIGSYS path,
 * reasoning that a path syscall reaching the supervisor meant the preload had
 * missed it.  That is wrong per the spec and wrong in fact: §6 says both
 * counters mean "someone introduced PTRACE_SYSCALL", and a SIGSYS on e.g.
 * chroot(2) is the KERNEL refusing a call whose path the preload rewrote
 * perfectly well.  Counting it would have made the number actively
 * misleading -- worse than the zero it replaced.
 *
 * aarch64 numbers. */
static int is_path_syscall(long nr)
{
    switch (nr) {
    case 34:  /* mkdirat      */ case 35:  /* unlinkat   */
    case 36:  /* symlinkat    */ case 37:  /* linkat     */
    case 38:  /* renameat     */ case 43:  /* statfs     */
    case 44:  /* fstatfs      */ case 45:  /* truncate   */
    case 48:  /* faccessat    */ case 49:  /* chdir      */
    case 50:  /* chroot(path) */ case 51:  /* fchmod-at  */
    case 53:  /* fchmodat     */ case 54:  /* fchownat   */
    case 56:  /* openat       */ case 78:  /* readlinkat */
    case 79:  /* newfstatat   */ case 259: /* renameat2  */
    case 437: /* openat2      */ case 439: /* faccessat2 */
        return 1;
    default:
        return 0;
    }
}

static int handle_sigsys(struct sup *s, pid_t t)
{
    siginfo_t si;
    unsigned long long regs[ALR_NREGS];
    struct iovec iov = { regs, sizeof regs };
    long nr, ret;

    if (s->stats) s->stats->sigsys_seen++;

    if (ptrace(PTRACE_GETSIGINFO, t, 0, &si) < 0) return 0;

    /* Only seccomp-generated SIGSYS is ours.  A guest that installs its own
     * seccomp sandbox (Codex does on normal Linux) must keep its signals. */
    if (si.si_code != SYS_SECCOMP) {
        if (s->stats) s->stats->sigsys_passthrough++;
        slog(s, 2, "alr sigsys: tid=%d si_code=%d not SYS_SECCOMP, passthrough\n",
             (int)t, si.si_code);
        return 0;
    }
#ifdef si_arch
    if (si.si_arch != AUDIT_ARCH_AARCH64) {
        if (s->stats) s->stats->sigsys_passthrough++;
        return 0;
    }
#endif

    if (ptrace(PTRACE_GETREGSET, t, NT_PRSTATUS, &iov) < 0) return 0;

#ifdef si_syscall
    nr = si.si_syscall;
#else
    nr = (long)regs[R_NR];
#endif
    ret = alr_sigsys_emulate(nr);

    /* MANDATORY: the kernel ran syscall_rollback() before raising the signal,
     * which on arm64 restores regs[0] = orig_x0.  On entry regs[0] therefore
     * holds the syscall's FIRST ARGUMENT, not a return value.  Returning
     * without writing it hands the caller its own argument back. */
    regs[R_RET] = (unsigned long long)ret;

    /* DEFENSIVE, and not optional: arch/arm64/kernel/signal.c runs the
     * syscall-restart check on regs[0] BEFORE delivering the signal, and after
     * the rollback that value is arg0.  If arg0 happens to be -512..-516 the
     * kernel rewinds pc to the svc; for -ERESTARTNOINTR(-513) there is no
     * revert branch, so we would re-trap forever.  si_call_addr is captured at
     * seccomp time, before any rewind, so it is the only trustworthy resume
     * address.  One store makes that whole class of hang impossible. */
#ifdef si_call_addr
    regs[R_PC] = (unsigned long long)(uintptr_t)si.si_call_addr;
#endif

    if (ptrace(PTRACE_SETREGSET, t, NT_PRSTATUS, &iov) < 0) return 0;

    if (s->stats) {
        s->stats->sigsys_emulated++;
        if (s->stats->sigsys_emulated > s->runaway_cap) {
            slog(s, 0, "alr: runaway SIGSYS (>%lu) nr=%ld, killing guest\n",
                 s->runaway_cap, nr);
            kill(t, SIGKILL);
            return 1;
        }
    }
    slog(s, 2, "alr sigsys: tid=%d nr=%ld name=%s ret=%ld pc=%llx\n",
         (int)t, nr, sigsys_name(nr), ret, regs[R_PC]);
    return 1;
}

/* Group-stop is NOT a SEIZE-only concept: not using SEIZE removes our ability
 * to IDENTIFY it, not the stop itself.  do_jobctl_trap() reports it as a plain
 * WIFSTOPPED with the stopping signal and (st>>16)==0, indistinguishable from
 * a signal-delivery-stop -- except that PTRACE_GETSIGINFO fails with EINVAL. */
static int is_group_stop(pid_t t, int sig)
{
    siginfo_t si;
    if (sig != SIGSTOP && sig != SIGTSTP && sig != SIGTTIN && sig != SIGTTOU)
        return 0;
    return ptrace(PTRACE_GETSIGINFO, t, 0, &si) < 0 && errno == EINVAL;
}

/* ── signal forwarding ───────────────────────────────────────────────── */

static volatile sig_atomic_t g_pending[NSIG];
static void relay(int sig) { if (sig > 0 && sig < NSIG) g_pending[sig] = 1; }

static void install_relays(void)
{
    static const int fwd[] = { SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGWINCH,
                               SIGUSR1, SIGUSR2, 0 };
    struct sigaction sa;
    int i;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = relay;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: we want waitpid to return EINTR so the loop can forward
     * promptly instead of after the next tracee event. */
    for (i = 0; fwd[i]; i++) sigaction(fwd[i], &sa, NULL);
}

static void drain_pending(struct sup *s, pid_t leader)
{
    int sig;
    for (sig = 1; sig < NSIG; sig++) {
        if (!g_pending[sig]) continue;
        g_pending[sig] = 0;
        kill(-leader, sig);            /* whole guest process group */
        if (s->stats) s->stats->signals_forwarded++;
        slog(s, 2, "alr: forwarded signal %d to guest pgrp\n", sig);
    }
}

/* ── main ────────────────────────────────────────────────────────────── */

int alr_exit_code(int status)
{
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 125;
}

int alr_supervise(const struct alr_sup_opts *o, int *status_out,
                  struct alr_sup_stats *st)
{
    struct sup s;
    pid_t leader;
    int st0, leader_status = 0;
    struct timespec t_start;
    /* CLOCK_MONOTONIC, not wall time: a clock step mid-run would otherwise
     * produce a negative or absurd duration, and this number's job is to be
     * the denominator for the counters beside it. */
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    long opts = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE
              | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT | PTRACE_O_EXITKILL;

    memset(&s, 0, sizeof s);
    s.log_level  = o->log_level;
    s.log_fd     = o->log_fd;
    s.runaway_cap = o->runaway_cap ? o->runaway_cap : ALR_DEFAULT_RUNAWAY;
    s.stats      = st;
    if (st) memset(st, 0, sizeof *st);

    leader = fork();
    if (leader < 0) return -1;

    if (leader == 0) {
        /* PTRACE_TRACEME does NOT stop us -- ptrace_traceme() only sets
         * PT_PTRACED.  Without an explicit stop we would run straight into
         * execve and die on set_robust_list before the parent could attach
         * options, and PTRACE_SETOPTIONS would fail ESRCH on a running tracee
         * anyway.  raise(SIGSTOP) creates the stop that satisfies both. */
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) _exit(126);
        setpgid(0, 0);              /* own process group, so kill(-pgid) works */
        raise(SIGSTOP);
        execve(o->path, o->argv, o->envp);
        _exit(127);
    }

    setpgid(leader, leader);        /* race-free: both sides set it */

    if (waitpid(leader, &st0, __WALL) < 0) return -1;
    if (!WIFSTOPPED(st0)) { if (status_out) *status_out = st0; return 0; }

    if (ptrace(PTRACE_SETOPTIONS, leader, 0, (void *)opts) < 0) {
        slog(&s, 0, "alr: PTRACE_SETOPTIONS failed: %s\n", strerror(errno));
        kill(leader, SIGKILL);
        waitpid(leader, &st0, __WALL);
        return -1;
    }
    add(&s, leader, T_RUNNING);
    install_relays();
    ptrace(PTRACE_CONT, leader, 0, 0);   /* sig 0 swallows our own SIGSTOP */

    for (;;) {
        int status, sig, event, idx;
        pid_t t = waitpid(-1, &status, __WALL | __WNOTHREAD);

        if (t < 0) {
            if (errno == EINTR) { drain_pending(&s, leader); continue; }
            if (errno == ECHILD) break;
            return -1;
        }
        drain_pending(&s, leader);

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            drop(&s, t);
            if (t == leader) leader_status = status;
            if (live(&s) == 0) break;
            continue;
        }
        if (!WIFSTOPPED(status)) continue;

        sig   = WSTOPSIG(status);
        event = (status >> 16) & 0xff;

        /* THE INVARIANT THIS WHOLE DESIGN RESTS ON, actually counted.
         *
         * docs/07-acceptance.md §3 makes `syscall_stops == 0` the hardest gate
         * in the repo -- "if that becomes non-zero somebody introduced
         * PTRACE_SYSCALL and at that moment this product IS PRoot".  Until
         * 2026-08-03 NOTHING INCREMENTED IT.  The field was declared, printed
         * every run, and read by bench/regression_gate.py, and it was
         * structurally zero: `grep -rn syscall_stops src/` found the struct
         * member and the printf and no writer at all.  The gate that existed
         * to make PTRACE_SYSCALL impossible to merge quietly could not see it.
         *
         * A syscall-enter/exit stop reaches us in one of two shapes:
         *   SIGTRAP|0x80  when PTRACE_O_TRACESYSGOOD is set
         *   bare SIGTRAP, event 0, from a tracee we already know
         * We set neither TRACESYSGOOD nor PTRACE_SYSCALL, and the only bare
         * SIGTRAPs we legitimately expect carry an event (fork/exec/exit,
         * handled above) or are the startup SIGSTOP (SIGSTOP, not SIGTRAP).
         * So either shape means someone put this process into syscall-stop
         * mode.  Count it and say so loudly -- the number is the evidence for
         * a claim the whole product is sold on. */
        if (sig == (SIGTRAP | 0x80)
            || (sig == SIGTRAP && !event && find(&s, t) >= 0
                && s.st[find(&s, t)] != T_NEW)) {
            unsigned long long r[ALR_NREGS];
            struct iovec iv = { r, sizeof r };
            long snr = -1;
            if (st) st->syscall_stops++;
            /* path_traps is the subset whose syscall carries a path (§6). */
            if (ptrace(PTRACE_GETREGSET, t, NT_PRSTATUS, &iv) == 0) {
                snr = (long)r[R_NR];
                if (is_path_syscall(snr) && st) st->path_traps++;
            }
            slog(&s, 0, "alr: SYSCALL-STOP tid=%d sig=%d nr=%ld -- PTRACE_SYSCALL "
                        "is in play; this is PRoot's model, not ours "
                        "(docs/adr/0001)\n", (int)t, sig, snr);
        }

        if (event) {
            if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK
                || event == PTRACE_EVENT_CLONE) {
                unsigned long msg = 0;
                if (ptrace(PTRACE_GETEVENTMSG, t, 0, &msg) == 0)
                    add(&s, (pid_t)msg, T_NEW);
            }
            /* EXEC and EXIT need no bookkeeping: options are inherited and
             * the tracee is already registered. */
            ptrace(PTRACE_CONT, t, 0, 0);
            continue;
        }

        /* A tid we have never seen: its own stop beat the parent's event.
         * This first stop IS the kernel-injected startup SIGSTOP. */
        idx = find(&s, t);
        if (idx < 0) {
            idx = add(&s, t, T_RUNNING);
            ptrace(PTRACE_CONT, t, 0, 0);
            continue;
        }

        /* Known but still NEW: swallow the startup SIGSTOP.  Re-injecting it
         * makes do_signal_stop() group-stop the ENTIRE guest thread group, so
         * every pthread_create() inside the guest would freeze it. */
        if (s.st[idx] == T_NEW && sig == SIGSTOP) {
            s.st[idx] = T_RUNNING;
            ptrace(PTRACE_CONT, t, 0, 0);
            continue;
        }

        if (sig == SIGSYS && handle_sigsys(&s, t)) {
            ptrace(PTRACE_CONT, t, 0, 0);
            continue;
        }

        if (is_group_stop(t, sig)) {
            /* Never re-inject the stopping signal here: the kernel discards it
             * and re-injection is what un-suspends the guest. */
            if (st) st->group_stops++;
            slog(&s, 2, "alr: group-stop tid=%d sig=%d\n", (int)t, sig);
            ptrace(PTRACE_CONT, t, 0, 0);
            continue;
        }

        /* §4.3: anything we did not originate goes through untouched.  Counted
         * so the rule is visible rather than asserted -- a regression that
         * started swallowing signals would otherwise look like nothing. */
        if (st) st->passthrough_signals++;
        ptrace(PTRACE_CONT, t, 0, sig);   /* pass through unchanged */
    }

    if (st) {
        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        st->elapsed_ms = (unsigned long)
            ((t_end.tv_sec - t_start.tv_sec) * 1000
             + (t_end.tv_nsec - t_start.tv_nsec) / 1000000);
    }
    if (status_out) *status_out = leader_status;
    return 0;
}
