/* alr-lane: read and write guest memory directly.
 *
 * Every path trap copies the guest's path in.  Upstream does it with
 * process_vm_readv against its own pid, which the kernel validates: a bad
 * pointer is EFAULT, not a crash in the handler.  That syscall cost as
 * much as the path lookup it served (318-437 ns on the reference device).
 * Here the copy is a plain load loop (arch/aarch64_alr_fault.S) and a bad
 * pointer is caught as the SIGSEGV/SIGBUS it raises: the handler sees the
 * fault inside the copy loop and resumes it at a stub that returns -1.
 *
 * That means owning SIGSEGV and SIGBUS in the kernel while the guest
 * believes it owns them (Go and the JVM depend on SIGSEGV):
 *
 *   - rt_sigaction on either signal is answered from a shadow, and the
 *     kernel gets our entry with the guest's flags and mask, so delivery
 *     (stack, mask, restart) is what the guest asked for;
 *   - a fault outside our code is passed on: the guest's handler is
 *     entered by a tail call with the kernel's arguments untouched; for
 *     SIG_DFL (or SIG_IGN, which a real fault ignores) the kernel action
 *     goes back to SIG_DFL and the instruction faults again;
 *   - SA_RESETHAND is applied to the shadow, since the kernel action has
 *     to stay ours;
 *   - a signal sent with kill() while we are inside the SIGSYS handler is
 *     queued again, blocked until the handler returns, so the guest's
 *     handler never runs inside ours.
 *
 * A vfork child shares our memory but has its own dispositions.
 * posix_spawn and Go's forkExec reset SIGSEGV there before exec; written
 * to the shadow, that would take the parent's handler away.  A process
 * whose pid is not the memory's tgid (usercopy.c) keeps its actions in a
 * small side table instead.
 *
 * x86_64 keeps process_vm_readv: the copy loop exists only for aarch64. */

#ifdef TAWCROOT_ALR

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>

#include "alr_mode.h"
#include "errno_neg.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "usercopy.h"

#ifndef SA_RESTORER
# define SA_RESTORER 0x04000000
#endif
#define K_SIG_DFL 0UL
#define K_SIG_IGN 1UL
#define BIT(s)    (1ULL << ((s) - 1))
#define FAULT_SIGS (BIT(SIGSEGV) | BIT(SIGBUS))
#define SIGSYS_BIT BIT(SIGSYS)

struct ksa {
	uint64_t handler, flags, restorer, mask;
};
_Static_assert(sizeof(struct ksa) == 32, "kernel sigaction at sigsetsize 8");

extern char tawc_safe_begin[], tawc_safe_end[], tawc_safe_fault[];
extern char __ehdr_start[], etext[];
extern void tawc_fault_entry(int, siginfo_t *, void *);
extern void tawcroot_sigreturn_trampoline(void);
extern long tawc_safe_strcpy(char *dst, const char *src, size_t cap);
extern long tawc_safe_memcpy(void *dst, const void *src, size_t n);

int tawcroot_alr_fault_armed;

/* Shadow: [0] SIGSEGV, [1] SIGBUS.  Seqcount; writers block both signals
 * so a reader in the fault path never spins on its own thread. */
static uint32_t   g_seq;
static struct ksa g_main[2];

#define SIDE_SLOTS 8
struct side {
	int32_t    pid;
	uint32_t   have;          /* bit i: act[i] set by this pid */
	struct ksa act[2];
};
static struct side g_side[SIDE_SLOTS];
static uint32_t    g_side_next;

static int sig_index(int sig)
{
	return sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : -1;
}

static long self_pid(void)
{
	return TAWC_RAW(TAWC_SYS_getpid, 0, 0, 0, 0, 0, 0);
}

/* 0 for the process that owns this memory, else our own pid. */
static long clone_vm_child(void)
{
	long pid = self_pid();
	return pid == tawc_self_tgid() ? 0 : pid;
}

static void shadow_get(int i, long child, struct ksa *out)
{
	for (;;) {
		uint32_t s1 = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
		if (s1 & 1) continue;
		const struct ksa *src = &g_main[i];
		for (int k = 0; child && k < SIDE_SLOTS; k++)
			if (g_side[k].pid == child && (g_side[k].have & (1u << i)))
				src = &g_side[k].act[i];
		struct ksa v;
		v.handler  = __atomic_load_n(&src->handler, __ATOMIC_RELAXED);
		v.flags    = __atomic_load_n(&src->flags, __ATOMIC_RELAXED);
		v.restorer = __atomic_load_n(&src->restorer, __ATOMIC_RELAXED);
		v.mask     = __atomic_load_n(&src->mask, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&g_seq, __ATOMIC_RELAXED) == s1) {
			*out = v;
			return;
		}
	}
}

static void shadow_set(int i, long child, const struct ksa *in)
{
	uint64_t block = FAULT_SIGS, old = 0;
	TAWC_RAW(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/, (long)&block,
		 (long)&old, 8, 0, 0);
	uint32_t s;
	do {
		s = __atomic_load_n(&g_seq, __ATOMIC_RELAXED);
	} while ((s & 1) || !__atomic_compare_exchange_n(&g_seq, &s, s + 1, 0,
							__ATOMIC_ACQUIRE,
							__ATOMIC_RELAXED));
	struct ksa *dst = &g_main[i];
	if (child) {
		int slot = -1;
		for (int k = 0; k < SIDE_SLOTS; k++)
			if (g_side[k].pid == child) slot = k;
		if (slot < 0) {
			slot = (int)(g_side_next++ % SIDE_SLOTS);
			g_side[slot].pid = (int32_t)child;
			g_side[slot].have = 0;
		}
		g_side[slot].have |= 1u << i;
		dst = &g_side[slot].act[i];
	}
	__atomic_store_n(&dst->handler, in->handler, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->flags, in->flags, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->restorer, in->restorer, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->mask, in->mask, __ATOMIC_RELAXED);
	__atomic_store_n(&g_seq, s + 2, __ATOMIC_RELEASE);
	TAWC_RAW(TAWC_SYS_rt_sigprocmask, 2 /*SIG_SETMASK*/, (long)&old, 0, 8,
		 0, 0);
}

/* The kernel action that stands in for the guest's `g`. */
static long install(int sig, const struct ksa *g)
{
	struct ksa k;
	k.handler = (uint64_t)(uintptr_t)tawc_fault_entry;
	if (g->handler == K_SIG_DFL || g->handler == K_SIG_IGN) {
		k.flags    = SA_SIGINFO | SA_ONSTACK | SA_RESTORER;
		k.restorer = (uint64_t)(uintptr_t)tawcroot_sigreturn_trampoline;
		k.mask     = 0;
	} else {
		k.flags    = (g->flags & ~(uint64_t)SA_RESETHAND) | SA_SIGINFO;
		k.restorer = g->restorer;
		k.mask     = g->mask & ~SIGSYS_BIT;
	}
	return TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)&k, 0, 8, 0, 0);
}

static void set_default(int sig)
{
	struct ksa k = { K_SIG_DFL, 0, 0, 0 };
	TAWC_RAW(TAWC_SYS_rt_sigaction, sig, (long)&k, 0, 8, 0, 0);
}

/* Send `info` back to this thread; it stays pending while blocked. */
static void requeue(int sig, siginfo_t *info)
{
	long tid = TAWC_RAW(TAWC_SYS_gettid, 0, 0, 0, 0, 0, 0);
	TAWC_RAW(TAWC_SYS_rt_tgsigqueueinfo, self_pid(), tid, sig, (long)info,
		 0, 0);
}

uintptr_t tawc_fault_route(int sig, siginfo_t *info, ucontext_t *uc)
{
	uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
	int sent = info->si_code <= 0;
	if (!sent && pc >= (uintptr_t)tawc_safe_begin &&
	    pc < (uintptr_t)tawc_safe_end) {
		uc->uc_mcontext.pc = (uint64_t)(uintptr_t)tawc_safe_fault;
		return 0;
	}
	int ours = pc >= (uintptr_t)__ehdr_start && pc < (uintptr_t)etext;
	if (ours && !sent) {             /* our own bug: die of it */
		set_default(sig);
		return 0;
	}
	if (ours) {
		/* kill() while the SIGSYS handler runs: hold it until the
		 * handler returns to the guest. */
		uint64_t *m = (uint64_t *)&uc->uc_sigmask;
		*m |= BIT(sig);
		uint64_t b = BIT(sig);
		TAWC_RAW(TAWC_SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/, (long)&b, 0,
			 8, 0, 0);
		requeue(sig, info);
		return 0;
	}

	int i = sig_index(sig);
	if (i < 0) { set_default(sig); return 0; }
	long child = clone_vm_child();
	struct ksa g;
	shadow_get(i, child, &g);
	if (g.handler == K_SIG_IGN && sent) return 0;
	if (g.handler == K_SIG_DFL || g.handler == K_SIG_IGN) {
		/* A fault re-executes and dies of SIG_DFL; a sent signal is
		 * queued again and delivered once this handler returns. */
		set_default(sig);
		if (sent) requeue(sig, info);
		return 0;
	}
	if (g.flags & SA_RESETHAND) {
		struct ksa dfl = { K_SIG_DFL, 0, 0, 0 };
		shadow_set(i, child, &dfl);
		(void)install(sig, &dfl);
	}
	return (uintptr_t)g.handler;
}

long tawcroot_alr_fault_sigaction(int sig, const void *act, void *oldact,
				  size_t sigsetsize)
{
	if (sigsetsize != 8) return TAWC_EINVAL;
	int i = sig_index(sig);
	struct ksa in;
	if (act && tawc_copy_from_guest(&in, sizeof in, act) < 0)
		return TAWC_EFAULT;
	long child = clone_vm_child();
	if (oldact) {
		struct ksa cur;
		shadow_get(i, child, &cur);
		if (tawc_copy_to_guest(oldact, &cur, sizeof cur) < 0)
			return TAWC_EFAULT;
	}
	if (act) {
		long r = install(sig, &in);
		if (r < 0) return r;
		shadow_set(i, child, &in);
	}
	return 0;
}

void tawcroot_alr_fault_init(void)
{
	tawcroot_alr_fault_armed = 0;
	g_seq = 0;
	g_side_next = 0;
	for (int k = 0; k < SIDE_SLOTS; k++) g_side[k].pid = 0;
	static const int sigs[2] = { SIGSEGV, SIGBUS };
	for (int i = 0; i < 2; i++) {
		/* What exec left: SIG_IGN survives it, a handler does not. */
		struct ksa cur = { 0, 0, 0, 0 };
		if (TAWC_RAW(TAWC_SYS_rt_sigaction, sigs[i], 0, (long)&cur, 8,
			     0, 0) < 0)
			return;
		struct ksa g = { cur.handler == K_SIG_IGN ? K_SIG_IGN : K_SIG_DFL,
				 0, 0, 0 };
		g_main[i] = g;
		if (install(sigs[i], &g) < 0) return;
	}
	tawcroot_alr_fault_armed = 1;
}

/* Before the guest's execve: give SIG_IGN back to the kernel, so the new
 * image inherits it as exec would have.  Undone if the exec fails. */
void tawcroot_alr_fault_before_exec(void)
{
	if (!tawcroot_alr_fault_armed) return;
	static const int sigs[2] = { SIGSEGV, SIGBUS };
	long child = clone_vm_child();
	for (int i = 0; i < 2; i++) {
		struct ksa g;
		shadow_get(i, child, &g);
		if (g.handler == K_SIG_IGN) {
			struct ksa k = { K_SIG_IGN, 0, 0, 0 };
			TAWC_RAW(TAWC_SYS_rt_sigaction, sigs[i], (long)&k, 0, 8,
				 0, 0);
		}
	}
}

void tawcroot_alr_fault_after_failed_exec(void)
{
	if (!tawcroot_alr_fault_armed) return;
	static const int sigs[2] = { SIGSEGV, SIGBUS };
	long child = clone_vm_child();
	for (int i = 0; i < 2; i++) {
		struct ksa g;
		shadow_get(i, child, &g);
		(void)install(sigs[i], &g);
	}
}

long tawcroot_alr_copy_str(char *dst, size_t cap, const char *src)
{
	return tawc_safe_strcpy(dst, src, cap);
}

long tawcroot_alr_copy(void *dst, const void *src, size_t n)
{
	return tawc_safe_memcpy(dst, src, n);
}

#endif /* TAWCROOT_ALR */
