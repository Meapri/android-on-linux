/*
 * libalr_interpose.c — ALR in-process LD_PRELOAD path-mediation interposer.
 *
 * Purpose
 * -------
 * The ALR loader runs dynamic glibc guests (e.g. GIMP) in-process and mediates
 * every file syscall in the parent via a stacked SECCOMP_RET_TRACE filter:
 *   guest open()  -> seccomp RET_TRACE -> PTRACE_EVENT_SECCOMP
 *               -> parent reads x1 from /proc/<tid>/mem -> translate -> write back.
 * That ptrace round-trip is microseconds *per file syscall*. GIMP opens
 * thousands of files at startup (fonts/brushes/data/config), so the round-trip
 * is too slow to reach the main window within the watchdog.
 *
 * This .so is LD_PRELOAD'd into the guest. It interposes the glibc *wrappers*
 * (open/openat/stat/access/...) and rewrites absolute guest paths to
 * <ALR_ROOTFS>+path BEFORE the underlying syscall, entirely in-process
 * (nanoseconds). The seccomp-trace filter then sees an already-rootfs path and,
 * thanks to its idempotency guard (a path already under ALR_ROOTFS is left
 * alone), does nothing. The ptrace path remains the safety net for everything
 * the interposer cannot reach (static binaries, raw inline syscalls, ld.so's
 * own loads).
 *
 * Rewrite policy — MUST match the supervisor (runtime_report.cpp, the
 * PTRACE_EVENT_SECCOMP block, and alr_runtime/alr_path.cpp::translate_rootfs_path):
 *   For a pathname p:
 *     - p == NULL or p[0] != '/'  (relative)            -> unchanged
 *     - p under /proc, /sys, or /dev (kernel virtual fs) -> unchanged
 *     - p already under $ALR_ROOTFS (idempotent)         -> unchanged
 *     - otherwise                                        -> $ALR_ROOTFS + p
 *   "under(p, d)" is a boundary-aware prefix test: p starts with d AND the next
 *   char of p is '/' or '\0' (so "/proc" and "/proc/x" match, "/procx" does not)
 *   — identical to the supervisor's `under` lambda.
 *   ALR_ROOTFS's trailing slashes are trimmed (matching trim_trailing_slashes),
 *   except a lone "/" is kept. Path normalization (".." clamping) is NOT done
 *   here: a plain prefix is sufficient because the seccomp net still clamps
 *   anything we miss.
 *
 * The loader is expected to set, in the guest environment:
 *     ALR_ROOTFS=<rootfs host dir>        (absolute, no required trailing slash)
 *     LD_PRELOAD=/usr/lib/libalr_interpose.so   (guest path to this .so)
 *
 * Build:
 *   zig cc --target=aarch64-linux-gnu.2.36 -shared -fPIC -O2 \
 *       -o libalr_interpose.so libalr_interpose.c
 *
 * Implementation notes
 * --------------------
 * - We call the real libc function via a cached dlsym(RTLD_NEXT, "name")
 *   pointer, resolved lazily on first use. We never call the libc wrapper
 *   by name internally (that would recurse into ourselves).
 * - The path is rewritten into a caller-supplied stack buffer (no malloc), so
 *   the hot path performs zero heap allocation and is reentrancy-safe.
 * - Everything is implemented with hand-rolled byte ops (no strlen/strcpy)
 *   to avoid any chance of recursing through an interposed libc string routine
 *   and to stay allocation-free.
 *
 * PCGATE fast path (ALR_PCGATE, default "1")
 * ------------------------------------------
 * In the default (PCGATE=1) mode the loader installs an ALLOW-all seccomp
 * filter (it does NOT trace the 9 path syscalls). This constructor then
 * installs a SECOND, PC-GATED filter, stacked on top, that:
 *     - if seccomp_data.instruction_pointer is inside the interposer's single
 *       'svc #0' trampoline  [alr_tramp_lo, alr_tramp_hi)   -> RET_ALLOW
 *     - else if nr is one of the 9 traced path syscalls      -> RET_TRACE
 *     - else                                                 -> RET_ALLOW
 * Every path wrapper below, AFTER rewriting the path, emits its underlying
 * syscall through that ONE trampoline (alr_tramp_syscall), so the trusted PC
 * is unique and only reachable post-rewrite. The supervisor's ptrace path
 * thus stops trapping interposer-mediated I/O (path_traps collapses), while
 * any path syscall that escapes the interposer (raw syscalls, ld.so loads,
 * compositional wrappers like realpath/scandir that call libc internally)
 * still traps at a non-trampoline PC -> RET_TRACE -> the supervisor backstop.
 * (M2, ADR-001 §4: opendir was such a compositional wrapper -- its internal
 * directory open is now emitted through the trampoline so it no longer traps;
 * see the opendir wrapper. The non-path credential getters getuid/geteuid/
 * getgid/getegid are memoized to drop their seccomp-dispatch cost to zero.)
 *
 * FAIL-SAFE: if the trampoline range is ever stale/wrong (e.g. after a guest
 * execve re-maps this .so at a fresh ASLR base), the PC gate simply misses and
 * path syscalls fall through to RET_TRACE -- slower, NEVER silently allowed.
 * Completeness can only degrade toward MORE tracing.
 *
 * Stacked-filter precedence (kernel: signed-min of the masked action wins, so
 * TRACE < ALLOW => TRACE beats ALLOW): the interposer's in-range ALLOW only
 * governs because the loader's filter is ALSO ALLOW for those syscalls, AND
 * the always-present Android zygote app filter ALLOWs the 7 path syscalls it
 * knows. For any syscall the zygote filter TRAPs (notably openat2=437 and
 * faccessat2=439, which are NOT in bionic's allowlist), no later filter can
 * rescue it -> SIGSYS. That is why the openat2/faccessat2 fast paths below are
 * gated behind a constructor probe that catches SIGSYS (not just ENOSYS) and
 * disables them on any device whose app filter lacks those syscalls.
 *
 * PCGATE=0 (A/B baseline): the constructor installs NO filter and every
 * wrapper reverts to its exact pre-PCGATE behavior (real libc via RTLD_NEXT
 * after string rewrite); the loader's full 9-syscall TRACE filter + the
 * supervisor's idempotent rewrite reproduce today's measured behavior.
 *
 * DEFERRED (design notes, NOT implemented here):
 *   - SECCOMP_USER_NOTIF instead of RET_TRACE: incompatible with the in-process
 *     model without a dedicated out-of-process supervisor (the triggering
 *     thread would block in-kernel awaiting a response no in-process thread can
 *     deliver). The existing cross-process ptrace supervisor already provides
 *     that supervision; the PC gate just removes its per-syscall round-trip for
 *     trampoline-emitted I/O. Revisit if exec-chain filter stacking (each guest
 *     execve adds another immutable, cumulative stale filter layer) makes the
 *     RET_TRACE backstop's cost matter -- USER_NOTIF sidesteps the stacked-
 *     filter trap.
 *   - stat/readlink result cache: not implemented.
 *   - kernel-enforced confinement (RESOLVE_IN_ROOT) for the non-open path
 *     syscalls (statx, faccessat, faccessat2, readlinkat, mkdirat, unlinkat):
 *     no UAPI variant exists, so they keep the string-prefix rewrite. Migrate
 *     if such resolve flags are ever extended to them.
 *   - renameat2/linkat kernel confinement via tracing: deferred; they are not
 *     in the traced 9, so their two-/single-path string rewrite is the sole
 *     (and unchanged) mediation.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>     /* getenv */
#include <fcntl.h>
#include <time.h>       /* struct timespec (utimensat) */
#include <sys/types.h>
#include <sys/stat.h>   /* struct stat[64], struct statx, statx flags */

/* PCGATE additions: raw UAPI seccomp/BPF + openat2, plus the SIGSYS-catching
 * availability probe. We deliberately use raw __NR_/SECCOMP_RET_ numbers (not
 * glibc's seccomp helpers) to keep the BPF byte-identical in intent to the
 * loader's and to minimize the dependency surface. */
#include <stdint.h>
#include <signal.h>          /* SIGSYS, struct sigaction (openat2/faccessat2 probe) */
#include <setjmp.h>          /* sigsetjmp/siglongjmp around the SIGSYS probe */
#include <errno.h>           /* errno, ENOSYS, EISDIR, EINTR */
#include <linux/audit.h>     /* AUDIT_ARCH_AARCH64 */
#include <linux/filter.h>    /* struct sock_filter, struct sock_fprog, BPF_* */
#include <linux/seccomp.h>   /* SECCOMP_SET_MODE_FILTER, SECCOMP_RET_* */
#include <sys/prctl.h>       /* PR_SET_NO_NEW_PRIVS, PR_SET_SECCOMP (fallback) */
#include <sys/syscall.h>     /* __NR_* */

/* linux/openat2.h (struct open_how, RESOLVE_IN_ROOT) is present on the
 * aarch64-linux-gnu.2.36 zig sysroot, but guard it so the file still builds if
 * a future/older sysroot drops it. */
#if defined(__has_include)
#  if __has_include(<linux/openat2.h>)
#    include <linux/openat2.h>
#  endif
#endif
#ifndef RESOLVE_IN_ROOT
#define RESOLVE_IN_ROOT 0x10
struct open_how { uint64_t flags; uint64_t mode; uint64_t resolve; };
#endif

/* arm64 syscall numbers we may need that an older sysroot might lack. All are
 * stable in asm-generic/unistd.h; these fallbacks just keep the file portable. */
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
#ifndef __NR_faccessat2
#define __NR_faccessat2 439
#endif

/* SECCOMP_FILTER_FLAG_TSYNC: apply the PC gate to every guest thread that may
 * already exist when the constructor runs (defensive; ld.so init normally runs
 * single-threaded). Fall back to 0 (current thread only) if undefined. */
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC (1UL << 0)
#endif

/* glibc marks the path arg of many of these wrappers __nonnull, so the
 * compiler "knows" path != NULL and warns that our defensive `path &&` guard
 * is always true. We keep the guard (a guest can still pass NULL via a raw
 * call into our wrapper) and silence just that diagnostic. */
#pragma GCC diagnostic ignored "-Wpointer-bool-conversion"
#pragma GCC diagnostic ignored "-Wtautological-pointer-compare"

/* ----- tiny self-contained byte helpers (no libc string calls) ----- */

static size_t a_len(const char *s) {
    size_t n = 0;
    if (s) { while (s[n]) ++n; }
    return n;
}

/* boundary-aware prefix: returns 1 iff p starts with d and the char right after
 * the prefix in p is '/' or '\0'. Mirrors the supervisor's `under` lambda. */
static int a_under(const char *p, const char *d) {
    size_t i = 0;
    while (d[i]) {
        if (p[i] != d[i]) return 0;
        ++i;
    }
    return p[i] == '/' || p[i] == '\0';
}

/* ----- ALR_ROOTFS, captured once at init ----- */

/* Holds the trimmed rootfs prefix (trailing '/'s removed, lone "/" kept).
 * If ALR_ROOTFS is unset/empty, g_rootfs_len == 0 and rw() is a pure
 * pass-through (defensive: the interposer then does nothing and the ptrace
 * net still mediates). Sized generously for any plausible Android data path. */
static char   g_rootfs[1024];
static size_t g_rootfs_len = 0;
static int    g_inited = 0;

static void alr_init(void) {
    if (g_inited) return;
    g_inited = 1;

    /* getenv is safe to resolve normally; it is not interposed here. */
    const char *r = getenv("ALR_ROOTFS");
    if (!r || r[0] != '/') {        /* must be an absolute host path */
        g_rootfs_len = 0;
        return;
    }
    size_t n = a_len(r);
    /* trim trailing slashes, but keep a lone "/" (matches trim_trailing_slashes) */
    while (n > 1 && r[n - 1] == '/') --n;
    if (n >= sizeof(g_rootfs)) n = sizeof(g_rootfs) - 1;   /* clamp, never overflow */
    for (size_t i = 0; i < n; ++i) g_rootfs[i] = r[i];
    g_rootfs[n] = '\0';
    /* A rootfs of exactly "/" is a no-op prefix; treat as disabled. */
    g_rootfs_len = (n == 1 && g_rootfs[0] == '/') ? 0 : n;
}

/*
 * rw(): the single rewrite helper.
 * Returns p unchanged, or a pointer into buf holding "<rootfs><p>".
 * buf must hold at least g_rootfs_len + strlen(p) + 1 bytes; if it is too
 * small we fail safe by returning p unchanged (the seccomp net catches it).
 */
static const char *rw(const char *p, char *buf, size_t buflen) {
    if (!p || p[0] != '/') return p;            /* NULL or relative -> passthrough */
    if (!g_inited) alr_init();
    if (g_rootfs_len == 0) return p;            /* interposer disabled */

    /* /proc, /sys, /dev are kernel virtual filesystems -> never rewritten. */
    if (a_under(p, "/proc") || a_under(p, "/sys") || a_under(p, "/dev"))
        return p;

    /* Idempotency: a path already under the rootfs is left as-is (matches the
     * supervisor's already_host guard; prevents <rootfs><rootfs>/...). */
    if (a_under(p, g_rootfs)) return p;

    size_t plen = a_len(p);
    if (g_rootfs_len + plen + 1 > buflen) return p;   /* too long -> fail safe */

    size_t i = 0;
    for (; i < g_rootfs_len; ++i) buf[i] = g_rootfs[i];
    for (size_t j = 0; j <= plen; ++j) buf[i + j] = p[j];   /* copies the NUL too */
    return buf;
}

/* Per-call scratch sized for rootfs prefix + a long guest path. */
#define ALR_PBUF 2304

/* =================================================================== */
/* PCGATE: single-svc trampoline + PC-gated seccomp filter             */
/* =================================================================== */

/* Module-level PCGATE state. All static, set once in the constructor and only
 * read afterward (read-only post-ctor => no locking needed, matching the benign
 * race on the cached ALR_REAL slots). Zero heap. */
static int      g_pcgate        = 1;    /* ALR_PCGATE, default "1" (read in ctor) */
static uint64_t alr_tramp_lo    = 0;    /* [lo,hi): the only PCs the BPF trusts  */
static uint64_t alr_tramp_hi    = 0;
static int      g_rootfs_fd     = -1;   /* O_PATH|O_DIRECTORY anchor, lifetime-leaked */
static int      g_openat2_ok    = 0;    /* 1 => openat2(RESOLVE_IN_ROOT) usable   */
static int      g_faccessat2_ok = 0;    /* 1 => faccessat2 usable (else faccessat) */

/*
 * alr_tramp_syscall — the ONE real-syscall site of the interposer.
 *
 * AAPCS64 in: x0=nr, x1..x6 = a0..a5 (7 ints fit in x0..x7, no stack args).
 * Linux arm64 syscall ABI: x8=nr, x0..x5=args, svc #0, result in x0.
 * We shuffle x1..x6 -> x0..x5, move nr -> x8, svc, return the raw kernel value.
 *
 * It lives in its own section "alr_tramp" so [__start_alr_tramp,
 * __stop_alr_tramp) is the exact, optimizer-independent mapped extent of the
 * trusted PC window (the linker auto-emits __start_/__stop_ for a
 * C-identifier section name). 'naked' forbids any compiler-emitted
 * prologue/epilogue or register spills, so the section contains ONLY these
 * instructions -> the range is tight and the stub uses no stack (leaf,
 * reentrancy-safe). 'used' keeps it even though it has internal linkage and is
 * only called indirectly. W^X-clean: ordinary file-backed r-x .text, no
 * anonymous PROT_EXEC, no runtime codegen.
 *
 * The kernel reports seccomp_data.instruction_pointer as the PC of the 'svc'
 * insn, which lies inside this section, so a half-open [start,stop) compare is
 * correct.
 */
__attribute__((naked, used, section("alr_tramp")))
static long alr_tramp_syscall(long nr, long a0, long a1,
                              long a2, long a3, long a4, long a5) {
    __asm__ volatile(
        "mov x8, x0\n"      /* syscall number            */
        "mov x0, x1\n"      /* a0                        */
        "mov x1, x2\n"      /* a1                        */
        "mov x2, x3\n"      /* a2                        */
        "mov x3, x4\n"      /* a3                        */
        "mov x4, x5\n"      /* a4                        */
        "mov x5, x6\n"      /* a5                        */
        "svc #0\n"
        "ret\n"
    );
}

/* Linker-provided bounds of the `alr_tramp` section. */
extern char __start_alr_tramp[];
extern char __stop_alr_tramp[];

/* Raw kernel return -> libc convention (-1 + errno on error). The kernel
 * returns -errno in [-4095,-1]; everything else is a valid result. Setting the
 * (TLS) errno calls no interposed function. */
static long alr_ret(long r) {
    if (r < 0 && r >= -4095) { errno = (int)(-r); return -1; }
    return r;
}

/* ----- openat2 / faccessat2 SIGSYS-catching availability probe -----
 *
 * On Android the guest's filter stacks on the zygote app filter whose default
 * action is SECCOMP_RET_TRAP -> SIGSYS for any syscall outside bionic's
 * allowlist. openat2 (437) and faccessat2 (439) are NOT in that allowlist, so a
 * blind emission would deliver SIGSYS and kill the process BEFORE any errno
 * (ENOSYS) check could run. We therefore probe each once at constructor time
 * with a temporary SIGSYS handler + siglongjmp; if the probe traps (or returns
 * an error), the corresponding fast path stays disabled and we fall back to the
 * always-allowed openat/faccessat. */
static sigjmp_buf  g_sigsys_jb;
static volatile sig_atomic_t g_sigsys_armed = 0;

static void alr_sigsys_handler(int sig) {
    (void)sig;
    if (g_sigsys_armed) {
        g_sigsys_armed = 0;
        siglongjmp(g_sigsys_jb, 1);
    }
    /* Not our probe: nothing sane to do from here; let it return (the kernel
     * re-raises on the same insn, but we only ever arm around our own probe
     * syscalls, so this branch is effectively unreached). */
}

/* Run `fn` (a single probe syscall via the trampoline) under a temporary SIGSYS
 * trap. Returns the syscall's raw return on normal completion, or -ENOSYS if it
 * raised SIGSYS (treated identically to "syscall absent"). Restores the prior
 * SIGSYS disposition. Reentrancy/threads: the probe runs once in the ctor
 * before guest threads/app code, so the process-wide handler swap is safe. */
typedef long (*alr_probe_fn)(void);
static long alr_probe_guarded(alr_probe_fn fn) {
    struct sigaction sa, old;
    long rc;

    /* hand-rolled zero of sa (no memset -> no interposed libc) */
    for (unsigned i = 0; i < sizeof sa; ++i) ((char *)&sa)[i] = 0;
    sa.sa_handler = alr_sigsys_handler;
    /* sigemptyset(&sa.sa_mask) is just an all-zero mask here. */
    sa.sa_flags = 0;

    if (alr_tramp_syscall(__NR_rt_sigaction, SIGSYS, (long)&sa, (long)&old,
                          (long)(sizeof(unsigned long)), 0, 0) != 0) {
        /* Could not install the handler: probe conservatively as unavailable. */
        return -ENOSYS;
    }

    if (sigsetjmp(g_sigsys_jb, 1) == 0) {
        g_sigsys_armed = 1;
        rc = fn();              /* may SIGSYS -> longjmp back with rc unset */
        g_sigsys_armed = 0;
    } else {
        rc = -ENOSYS;           /* trapped: treat as absent */
    }

    /* restore previous SIGSYS disposition */
    alr_tramp_syscall(__NR_rt_sigaction, SIGSYS, (long)&old, 0,
                      (long)(sizeof(unsigned long)), 0, 0);
    return rc;
}

/* Probe bodies: each issues exactly one syscall through the trampoline. */
static long alr_probe_openat2_body(void) {
    struct open_how how;
    how.flags   = (uint64_t)(O_PATH | O_DIRECTORY | O_CLOEXEC);
    how.mode    = 0;
    how.resolve = RESOLVE_IN_ROOT;
    return alr_tramp_syscall(__NR_openat2, g_rootfs_fd, (long)".",
                             (long)&how, (long)sizeof how, 0, 0);
}
static long alr_probe_faccessat2_body(void) {
    /* faccessat2(AT_FDCWD, "/", F_OK=0, 0): harmless existence check. */
    return alr_tramp_syscall(__NR_faccessat2, AT_FDCWD, (long)"/", 0, 0, 0, 0);
}

/* Open the O_PATH|O_DIRECTORY rootfs anchor (once) and probe openat2 against
 * it. Both go through the trampoline so the ctor never depends on a wrapped
 * libc symbol. The anchor fd is intentionally leaked for process lifetime. */
static void alr_open_rootfs_fd(void) {
    long fd = alr_tramp_syscall(__NR_openat, AT_FDCWD, (long)g_rootfs,
                                (long)(O_PATH | O_DIRECTORY | O_CLOEXEC), 0, 0, 0);
    if (fd >= 0) {
        g_rootfs_fd = (int)fd;
        long t = alr_probe_guarded(alr_probe_openat2_body);
        if (t >= 0) {
            alr_tramp_syscall(__NR_close, t, 0, 0, 0, 0, 0);  /* close probe fd */
            g_openat2_ok = 1;
        }
        /* t < 0 (ENOSYS, SIGSYS-trapped, or any error) -> string fallback */
    }
}

static void alr_probe_faccessat2(void) {
    long r = alr_probe_guarded(alr_probe_faccessat2_body);
    /* present iff it neither trapped nor returned ENOSYS */
    g_faccessat2_ok = (r != -ENOSYS);
}

/*
 * Install the PC-gated path filter (PCGATE=1 only). ALLOW iff the trap PC is
 * inside the trampoline [lo,hi); else RET_TRACE for the 9 path syscalls,
 * RET_ALLOW for everything else. 64-bit IP is compared as two 32-bit words
 * (lo @ off 8, hi @ off 12) for a half-open [lo,hi) unsigned range test.
 *
 * DECISION (unchanged contract, see docs/design/pcgate-seccomp.md):
 *     arch != AUDIT_ARCH_AARCH64                     -> ALLOW
 *     nr NOT in the 9 path syscalls                  -> ALLOW   (the hot case)
 *     nr in the 9 AND IP in [lo,hi)                  -> ALLOW   (trampoline)
 *     nr in the 9 AND IP outside [lo,hi)             -> TRACE   (supervisor)
 *
 * PERFORMANCE LAYOUT (the slimming): the kernel evaluates this filter on EVERY
 * syscall the guest makes, and on-device a getpid/futex/clock_gettime storm pays
 * for it (~24 ns/syscall measured before this change). The OLD layout ran the
 * full two-word PC-range test FIRST, then a 9-way linear nr scan, so a harmless
 * syscall walked ~20-24 instructions for an outcome (ALLOW) that never depended
 * on the PC at all. This layout instead CLASSIFIES nr FIRST with a small
 * bracketed decision tree and ALLOWs every non-path syscall in 7 instructions
 * flat — the PC-range test is computed ONLY for the (rare) 9 path syscalls,
 * where its result actually matters. Decision-identical to the old filter; the
 * reorder is observably-equivalent because a non-path syscall is ALLOW under
 * both the old PC-then-nr and the new nr-then-PC ordering.
 *
 * nr bracketing rationale (aarch64 NRs): the 9 path syscalls are
 *   {34,35,48,56,78,79, 291, 437,439}. The hot harmless syscalls
 *   (futex=98, clock_gettime=113, clock_nanosleep=115, rt_sigprocmask=135,
 *    getpid=172, gettid=178) all sit in the gap (79,291); read=63/write=64 sit
 *   in (56,78). A split at nr>79, then a (80,291] band, lands every one of those
 *   on a 7-instruction ALLOW. The path NRs are matched by exact bracketed
 *   compares so each still routes to the PC gate.
 *
 * The jt/jf/k offsets below were laid out by instruction index and VALIDATED
 * on-host (tests/test_pcgate_bpf_logic.py decision model + an exhaustive cBPF
 * simulator over all 9 path NRs, the hot harmless NRs, foreign arch, and IP
 * boundaries incl. lo/hi-word edges and a 4GB-straddling window: 0 mismatches
 * vs the contract). Offsets are relative to the NEXT instruction. If you change
 * the layout, re-run that host check.
 */
static void alr_install_pcgated_filter(void) {
    /* zygote already set NO_NEW_PRIVS process-wide; a re-arm is harmless and
     * keeps us correct if ever loaded somewhere it was not pre-set. */
    alr_tramp_syscall(__NR_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0);

    const uint32_t lo_hi = (uint32_t)(alr_tramp_lo >> 32);
    const uint32_t lo_lo = (uint32_t)(alr_tramp_lo & 0xffffffffu);
    const uint32_t hi_hi = (uint32_t)(alr_tramp_hi >> 32);
    const uint32_t hi_lo = (uint32_t)(alr_tramp_hi & 0xffffffffu);

    /* struct seccomp_data offsets (UAPI-stable; hardcoded to be independent of
     * the local struct definition): nr@0, arch@4, ip@8 (lo@8, hi@12), args@16. */
    enum { OFF_NR = 0, OFF_ARCH = 4, OFF_IP_LO = 8, OFF_IP_HI = 12 };

    struct sock_filter f[] = {
        /* 0  */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARCH),
        /* 1  */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0), /* aarch64 -> nr classify(3); else ALLOW(2) */
        /* 2  */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),                  /* foreign arch -> allow */

        /* ---- nr classify FIRST: non-path -> ALLOW in <=7 insns ---- */
        /* 3  */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),                    /* A = nr */
        /* 4  */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 79, 7, 0),                 /* nr>79 -> UPPER(12); else LOWER(5) */

        /* LOWER (nr<=79): path subset {34,35,48,56,78,79} */
        /* 5  */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 77, 0, 1),                 /* {78,79} -> L7879(6); nr<=77 -> LMID(7) */
        /* 6  */ BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 78, 10, 20),               /* L7879: nr>=78 -> PATH(17); else ALLOW(27) */
        /* 7  */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 56, 19, 0),                /* LMID: 57..77 (read/write) -> ALLOW(27); nr<=56 -> LLOW(8) */
        /* 8  */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat,    8, 0),     /* 56  -> PATH(17) */
        /* 9  */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat, 7, 0),     /* 48  -> PATH(17) */
        /* 10 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_unlinkat,  6, 0),     /* 35  -> PATH(17) */
        /* 11 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mkdirat,   5, 15),    /* 34  -> PATH(17); else ALLOW(27) */

        /* UPPER (nr>79): path subset {291,437,439} */
        /* 12 */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 291, 1, 0),                /* nr>291 -> U1(14); 80..291 -> HOT(13) */
        /* 13 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_statx,     3, 13),    /* HOT: 291 -> PATH(17); else (incl all hot NRs) ALLOW(27) */
        /* 14 */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 439, 12, 0),               /* U1: nr>439 -> ALLOW(27); 292..439 -> U2(15) */
        /* 15 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat2, 1, 0),    /* 439 -> PATH(17) */
        /* 16 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat2,    0, 10),   /* 437 -> PATH(17); else ALLOW(27) */

        /* ---- PC gate (reached ONLY for the 9 path NRs): ALLOW iff lo<=IP<hi ---- */
        /* 17 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_IP_HI),                 /* PATH: A = IP.hi */
        /* 18 */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, lo_hi, 3, 0),              /* hi>lo_hi: lower ok -> UPCHK(22) */
        /* 19 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, lo_hi, 0, 8),              /* hi==lo_hi: test lo(20); hi<lo_hi: below -> TRACE(28) */
        /* 20 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_IP_LO),                 /* A = IP.lo */
        /* 21 */ BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, lo_lo, 0, 6),              /* lo>=lo_lo: UPCHK(22); else TRACE(28) */
        /* 22 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_IP_HI),                 /* UPCHK: A = IP.hi (upper bound) */
        /* 23 */ BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, hi_hi, 4, 0),              /* hi>hi_hi: IP>=hi -> TRACE(28) */
        /* 24 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, hi_hi, 0, 2),              /* hi==hi_hi: test lo(25); hi<hi_hi -> ALLOW(27) */
        /* 25 */ BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_IP_LO),                 /* A = IP.lo */
        /* 26 */ BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, hi_lo, 1, 0),              /* lo>=hi_lo: NOT trusted -> TRACE(28); else ALLOW(27) */

        /* 27 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),                  /* ALLOW */
        /* 28 */ BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),                  /* path syscall, untrusted PC -> trace */
    };

    struct sock_fprog prog = {
        (unsigned short)(sizeof(f) / sizeof(f[0])), f
    };
    /* Prefer the seccomp() syscall with TSYNC (covers any pre-existing guest
     * thread). If TSYNC is rejected (single-threaded path, or kernel without
     * it), retry without flags; finally fall back to prctl(PR_SET_SECCOMP). */
    long fr = alr_tramp_syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                                (long)SECCOMP_FILTER_FLAG_TSYNC, (long)&prog, 0, 0, 0);
    if (fr != 0) {
        fr = alr_tramp_syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0,
                               (long)&prog, 0, 0, 0);
    }
    if (fr != 0) {
        alr_tramp_syscall(__NR_prctl, PR_SET_SECCOMP, SECCOMP_MODE_FILTER,
                          (long)&prog, 0, 0, 0);
    }
    /* No diag fd here; success is observable via the supervisor's path_traps
     * collapsing in a PCGATE 0-vs-1 A/B run. Install failure leaves the loader's
     * ALLOW-all filter governing -> paths run unmediated by seccomp in this
     * (practically unreachable on a NO_NEW_PRIVS arm64 process) case; we never
     * abort the guest. */
}

/*
 * Constructor. ORDERING IS LOAD-BEARING (see CONTRACT "WHY THE ORDERING IS
 * SOUND"): it runs during ld.so init, AFTER all libraries are mapped via the
 * loader's injected absolute LD_LIBRARY_PATH, so no path syscall before this
 * point needs mediation. Every syscall the ctor itself issues goes through the
 * trampoline (in-range PC); the filter is installed LAST, so the probes run
 * before it exists and are unconditionally allowed by the loader's ALLOW-all
 * filter. From here on, all path I/O routed through the trampoline is gated.
 *
 * Priority 101 sorts this ahead of any default-priority (65535) library
 * constructor that might open data files (it cannot order ahead of an
 * earlier-listed LD_PRELOAD, but we ship only one).
 */
__attribute__((constructor(101)))
static void alr_ctor(void) {
    alr_init();                         /* sets g_rootfs / g_rootfs_len (idempotent) */

    /* Trampoline range FIRST: needed by the BPF and by every emit/probe. */
    alr_tramp_lo = (uint64_t)(uintptr_t)__start_alr_tramp;
    alr_tramp_hi = (uint64_t)(uintptr_t)__stop_alr_tramp;

    /* PCGATE gate: read from the guest env (the loader propagates ALR_PCGATE).
     * DEFAULT "1"; only an explicit "0" disables. */
    const char *g = getenv("ALR_PCGATE");
    g_pcgate = (g != NULL && g[0] == '0') ? 0 : 1;

    /* rootfs anchor + openat2 probe, then the faccessat2 probe. Both use the
     * trampoline and tolerate the rootfs being unset (self-disable). These run
     * regardless of g_pcgate so the trampoline emit paths (used in PCGATE=1)
     * have a valid anchor; in PCGATE=0 they are simply unused. */
    if (g_rootfs_len > 0) alr_open_rootfs_fd();
    alr_probe_faccessat2();

    if (!g_pcgate) return;              /* PCGATE=0: install NO filter (loader does) */

    /* Fail-safe: a degenerate/empty trampoline range would make the gate match
     * nothing; rather than install a filter that TRACEs everything (which would
     * also trap our own trampoline emits and deadlock against a tracer the
     * loader is not running for paths), install none. The section is never
     * empty in practice, so this is unreachable. */
    if (alr_tramp_hi <= alr_tramp_lo) return;

    alr_install_pcgated_filter();
}

/* ----- RTLD_NEXT resolution, cached per wrapper ----- */

/* Resolve & cache the real libc symbol "sym" into the static slot `slot`.
 * The store of a pointer is atomic on aarch64; a benign race just resolves
 * twice to the same value. */
#define ALR_REAL(slot, type, sym)                                   \
    do {                                                            \
        if (!(slot)) (slot) = (type)dlsym(RTLD_NEXT, sym);         \
    } while (0)

/* =================================================================== */
/* open family (variadic: extract mode only when creating)             */
/* =================================================================== */

static mode_t alr_va_mode(int flags, va_list ap) {
    /* mode is only consumed by the kernel when O_CREAT or O_TMPFILE is set. */
#ifdef O_TMPFILE
    if (flags & (O_CREAT | O_TMPFILE))
#else
    if (flags & O_CREAT)
#endif
        return (mode_t)va_arg(ap, int /* mode_t promotes to int */);
    return 0;
}

/*
 * alr_open_emit — PCGATE=1 open emission for the whole open/creat family.
 *
 * Fast path: an ABSOLUTE guest path that is NOT a passthrough class, with
 * openat2(RESOLVE_IN_ROOT) available and the rootfs anchor open. RESOLVE_IN_ROOT
 * treats g_rootfs_fd as '/', giving kernel-enforced, symlink-escape-safe
 * confinement with NO string concatenation (closing the ".."-normalization gap
 * the string rewrite cannot). We pass the GUEST path made relative to the
 * anchor (drop the leading '/'; "/" itself becomes ".").
 *
 * Fallback (no openat2, or a passthrough/relative path): the existing
 * <ROOTFS>+path string rewrite + a plain openat through the trampoline. For a
 * relative path the original dirfd and unmodified path are forwarded, exactly
 * preserving today's "we do not mediate dirfd-relative paths" behavior.
 *
 * Either way the underlying syscall is emitted through alr_tramp_syscall so the
 * trusted PC is the trampoline.
 */
static int alr_open_emit(int dirfd, const char *path, int flags, mode_t mode) {
    if (g_openat2_ok && g_rootfs_fd >= 0 &&
        path && path[0] == '/' &&
        !a_under(path, "/proc") && !a_under(path, "/sys") && !a_under(path, "/dev") &&
        !a_under(path, g_rootfs)) {
        struct open_how how;
        how.flags = (uint64_t)(unsigned)flags;
#ifdef O_TMPFILE
        how.mode  = (flags & (O_CREAT | O_TMPFILE)) ? (uint64_t)mode : 0;
#else
        how.mode  = (flags & O_CREAT) ? (uint64_t)mode : 0;
#endif
        how.resolve = RESOLVE_IN_ROOT;
        const char *rel = path + 1;          /* drop leading '/'; "" => rootfs root */
        if (rel[0] == '\0') rel = ".";
        long r = alr_tramp_syscall(__NR_openat2, g_rootfs_fd, (long)rel,
                                   (long)&how, (long)sizeof how, 0, 0);
        return (int)alr_ret(r);
    }
    /* Fallback: string-prefix rewrite (absolute only) + plain openat. */
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r = alr_tramp_syscall(__NR_openat, dirfd, (long)p, flags, mode, 0, 0);
    return (int)alr_ret(r);
}

int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    if (g_pcgate) return alr_open_emit(AT_FDCWD, path, flags, m);
    /* PCGATE=0 baseline: original RTLD_NEXT behavior, unchanged. */
    static int (*real)(const char *, int, ...);
    ALR_REAL(real, int (*)(const char *, int, ...), "open");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), flags, m);
}

int open64(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    if (g_pcgate) return alr_open_emit(AT_FDCWD, path, flags, m);
    static int (*real)(const char *, int, ...);
    ALR_REAL(real, int (*)(const char *, int, ...), "open64");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), flags, m);
}

/* FORTIFY (_FORTIFY_SOURCE) variants glib/GIMP may emit. No mode (O_CREAT is
 * not expressible through __open_2), so pass mode=0. */
int __open_2(const char *path, int flags) {
    if (g_pcgate) return alr_open_emit(AT_FDCWD, path, flags, 0);
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "__open_2");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), flags);
}

int __open64_2(const char *path, int flags) {
    if (g_pcgate) return alr_open_emit(AT_FDCWD, path, flags, 0);
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "__open64_2");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), flags);
}

static int alr_openat(int (*real)(int, const char *, int, ...),
                      int dirfd, const char *path, int flags, mode_t m) {
    char b[ALR_PBUF];
    /* *at: only rewrite ABSOLUTE paths. dirfd-relative paths resolve against
     * the guest cwd, which we do not mediate here. (AT_FDCWD + absolute path
     * still gets rewritten because the path itself is absolute.) */
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags, m);
}

int openat(int dirfd, const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    if (g_pcgate) return alr_open_emit(dirfd, path, flags, m);
    static int (*real)(int, const char *, int, ...);
    ALR_REAL(real, int (*)(int, const char *, int, ...), "openat");
    return alr_openat(real, dirfd, path, flags, m);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t m = alr_va_mode(flags, ap);
    va_end(ap);
    if (g_pcgate) return alr_open_emit(dirfd, path, flags, m);
    static int (*real)(int, const char *, int, ...);
    ALR_REAL(real, int (*)(int, const char *, int, ...), "openat64");
    return alr_openat(real, dirfd, path, flags, m);
}

int __openat_2(int dirfd, const char *path, int flags) {
    if (g_pcgate) return alr_open_emit(dirfd, path, flags, 0);
    static int (*real)(int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int), "__openat_2");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags);
}

int __openat64_2(int dirfd, const char *path, int flags) {
    if (g_pcgate) return alr_open_emit(dirfd, path, flags, 0);
    static int (*real)(int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int), "__openat64_2");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags);
}

int creat(const char *path, mode_t mode) {
    if (g_pcgate) return alr_open_emit(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "creat");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}

int creat64(const char *path, mode_t mode) {
    if (g_pcgate) return alr_open_emit(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "creat64");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}

/* =================================================================== */
/* stat family                                                         */
/* =================================================================== */

/*
 * stat-family reduction. arm64 glibc implements stat/lstat/stat64/lstat64 and
 * all the __xstat / fstatat vtable forms on top of the newfstatat syscall. To
 * keep the trusted-PC syscall unique we re-express each as a newfstatat through
 * the trampoline (PCGATE=1). The kernel writes a 'struct stat' in the kernel
 * layout, which on arm64 IS glibc's userspace struct stat / struct stat64 (no
 * translation, LFS default) -- the same ABI the supervisor already relies on
 * when it traps newfstatat for these. So writing directly into the caller's buf
 * is correct and matches the prior real() forward.
 *
 * alr_stat_emit rewrites an ABSOLUTE path (relative paths and AT_FDCWD-relative
 * forms pass through unchanged via dirfd) and emits newfstatat through the
 * trampoline. `flags` carries AT_SYMLINK_NOFOLLOW for the l* variants.
 */
static int alr_stat_emit(int dirfd, const char *path, void *buf, int flags) {
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r = alr_tramp_syscall(__NR_newfstatat, dirfd, (long)p, (long)buf, flags, 0, 0);
    return (int)alr_ret(r);
}
static int alr_statx_emit(int dirfd, const char *path, int flags,
                          unsigned int mask, void *buf) {
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r = alr_tramp_syscall(__NR_statx, dirfd, (long)p, flags,
                               (long)mask, (long)buf, 0);
    return (int)alr_ret(r);
}

/* stat/lstat/stat64/lstat64: AT_FDCWD + newfstatat (l* => AT_SYMLINK_NOFOLLOW).
 * PCGATE=0 keeps the original RTLD_NEXT path verbatim. */
#define ALR_WRAP_STAT(name, argtype, nofollow)                                  \
    int name(const char *path, argtype out) {                                   \
        if (g_pcgate)                                                           \
            return alr_stat_emit(AT_FDCWD, path, (void *)out, (nofollow));      \
        static int (*real)(const char *, argtype);                             \
        ALR_REAL(real, int (*)(const char *, argtype), #name);                 \
        char b[ALR_PBUF];                                                       \
        return real(rw(path, b, sizeof b), out);                                \
    }

ALR_WRAP_STAT(stat,    struct stat *,   0)
ALR_WRAP_STAT(lstat,   struct stat *,   AT_SYMLINK_NOFOLLOW)
ALR_WRAP_STAT(stat64,  struct stat64 *, 0)
ALR_WRAP_STAT(lstat64, struct stat64 *, AT_SYMLINK_NOFOLLOW)

/* glibc <2.33 vtable entry points (__xstat etc.): version int + path + buf.
 * `ver` only selected the struct ABI in the old vtable scheme; arm64 has a
 * single kernel struct stat ABI, so dropping `ver` and routing to newfstatat is
 * correct. */
int __xstat(int ver, const char *path, struct stat *buf) {
    if (g_pcgate) return alr_stat_emit(AT_FDCWD, path, buf, 0);
    static int (*real)(int, const char *, struct stat *);
    ALR_REAL(real, int (*)(int, const char *, struct stat *), "__xstat");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}
int __lxstat(int ver, const char *path, struct stat *buf) {
    if (g_pcgate) return alr_stat_emit(AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
    static int (*real)(int, const char *, struct stat *);
    ALR_REAL(real, int (*)(int, const char *, struct stat *), "__lxstat");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}
int __xstat64(int ver, const char *path, struct stat64 *buf) {
    if (g_pcgate) return alr_stat_emit(AT_FDCWD, path, buf, 0);
    static int (*real)(int, const char *, struct stat64 *);
    ALR_REAL(real, int (*)(int, const char *, struct stat64 *), "__xstat64");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}
int __lxstat64(int ver, const char *path, struct stat64 *buf) {
    if (g_pcgate) return alr_stat_emit(AT_FDCWD, path, buf, AT_SYMLINK_NOFOLLOW);
    static int (*real)(int, const char *, struct stat64 *);
    ALR_REAL(real, int (*)(int, const char *, struct stat64 *), "__lxstat64");
    char b[ALR_PBUF];
    return real(ver, rw(path, b, sizeof b), buf);
}

/* fstatat family — *at, rewrite only absolute paths; emit newfstatat. */
int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    if (g_pcgate) return alr_stat_emit(dirfd, path, buf, flags);
    static int (*real)(int, const char *, struct stat *, int);
    ALR_REAL(real, int (*)(int, const char *, struct stat *, int), "fstatat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, flags);
}
int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags) {
    if (g_pcgate) return alr_stat_emit(dirfd, path, buf, flags);
    static int (*real)(int, const char *, struct stat64 *, int);
    ALR_REAL(real, int (*)(int, const char *, struct stat64 *, int), "fstatat64");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, flags);
}
int newfstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    if (g_pcgate) return alr_stat_emit(dirfd, path, buf, flags);
    static int (*real)(int, const char *, struct stat *, int);
    ALR_REAL(real, int (*)(int, const char *, struct stat *, int), "newfstatat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, flags);
}
int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flags) {
    if (g_pcgate) return alr_stat_emit(dirfd, path, buf, flags);
    static int (*real)(int, int, const char *, struct stat *, int);
    ALR_REAL(real, int (*)(int, int, const char *, struct stat *, int), "__fxstatat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(ver, dirfd, p, buf, flags);
}
int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *buf, int flags) {
    if (g_pcgate) return alr_stat_emit(dirfd, path, buf, flags);
    static int (*real)(int, int, const char *, struct stat64 *, int);
    ALR_REAL(real, int (*)(int, int, const char *, struct stat64 *, int), "__fxstatat64");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(ver, dirfd, p, buf, flags);
}

/* statx(dirfd, path, flags, mask, buf) — *at-style, rewrite absolute only. */
int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *buf) {
    if (g_pcgate) return alr_statx_emit(dirfd, path, flags, mask, buf);
    static int (*real)(int, const char *, int, unsigned int, struct statx *);
    ALR_REAL(real, int (*)(int, const char *, int, unsigned int, struct statx *),
             "statx");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags, mask, buf);
}

/* =================================================================== */
/* access family                                                       */
/* =================================================================== */

/*
 * access-family emission. Both __NR_faccessat and __NR_faccessat2 are in the
 * traced 9, so either trusted-PC syscall is allowed by the BPF. We prefer
 * faccessat2 (which honors flags incl. AT_EACCESS) when the constructor probe
 * found it usable, exactly as glibc itself does; otherwise plain faccessat
 * (which historically ignored flags) -- identical to glibc's pre-5.8 fallback.
 * If `flags` needs faccessat2 semantics but it is unavailable, we still fall to
 * faccessat(...,0): a slightly looser id/symlink check, matching glibc.
 */
static int alr_access_emit(int dirfd, const char *path, int mode, int flags) {
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r;
    if (flags != 0 && g_faccessat2_ok) {
        r = alr_tramp_syscall(__NR_faccessat2, dirfd, (long)p, mode, flags, 0, 0);
    } else {
        r = alr_tramp_syscall(__NR_faccessat, dirfd, (long)p, mode, 0, 0, 0);
    }
    return (int)alr_ret(r);
}

int access(const char *path, int mode) {
    if (g_pcgate) return alr_access_emit(AT_FDCWD, path, mode, 0);
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "access");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int euidaccess(const char *path, int mode) {
    /* effective-id check: use faccessat2(AT_EACCESS) when available. */
    if (g_pcgate) return alr_access_emit(AT_FDCWD, path, mode,
                                         g_faccessat2_ok ? AT_EACCESS : 0);
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "euidaccess");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int eaccess(const char *path, int mode) {
    if (g_pcgate) return alr_access_emit(AT_FDCWD, path, mode,
                                         g_faccessat2_ok ? AT_EACCESS : 0);
    static int (*real)(const char *, int);
    ALR_REAL(real, int (*)(const char *, int), "eaccess");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int faccessat(int dirfd, const char *path, int mode, int flags) {
    if (g_pcgate) return alr_access_emit(dirfd, path, mode, flags);
    static int (*real)(int, const char *, int, int);
    ALR_REAL(real, int (*)(int, const char *, int, int), "faccessat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, flags);
}
int faccessat2(int dirfd, const char *path, int mode, int flags) {
    /* Explicit faccessat2 call: emit faccessat2 directly (it is in the traced
     * 9). If the device lacks it the kernel returns ENOSYS -> normalized errno,
     * matching a direct call's behavior. */
    if (g_pcgate) {
        char b[ALR_PBUF];
        const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
        long r = alr_tramp_syscall(__NR_faccessat2, dirfd, (long)p, mode, flags, 0, 0);
        return (int)alr_ret(r);
    }
    static int (*real)(int, const char *, int, int);
    ALR_REAL(real, int (*)(int, const char *, int, int), "faccessat2");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, flags);
}

/* =================================================================== */
/* readlink family                                                     */
/* =================================================================== */

/* readlink/readlinkat both reduce to __NR_readlinkat (in the traced 9). */
static ssize_t alr_readlink_emit(int dirfd, const char *path,
                                 char *buf, size_t bufsiz) {
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r = alr_tramp_syscall(__NR_readlinkat, dirfd, (long)p,
                               (long)buf, (long)bufsiz, 0, 0);
    return (ssize_t)alr_ret(r);
}
ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    if (g_pcgate) return alr_readlink_emit(AT_FDCWD, path, buf, bufsiz);
    static ssize_t (*real)(const char *, char *, size_t);
    ALR_REAL(real, ssize_t (*)(const char *, char *, size_t), "readlink");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), buf, bufsiz);
}
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz) {
    if (g_pcgate) return alr_readlink_emit(dirfd, path, buf, bufsiz);
    static ssize_t (*real)(int, const char *, char *, size_t);
    ALR_REAL(real, ssize_t (*)(int, const char *, char *, size_t), "readlinkat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, buf, bufsiz);
}

/* =================================================================== */
/* directory enumeration                                               */
/* =================================================================== */

/*
 * opendir — PCGATE=1: open the directory fd THROUGH THE TRAMPOLINE, then hand it
 * to fdopendir. This is the M2 (ADR-001 §4 candidate 4) trap reduction: the
 * stock opendir is a *compositional* wrapper — it calls glibc's internal
 * open from a NON-trampoline PC, so its underlying openat(56) is a path syscall
 * that escapes the PC gate -> RET_TRACE -> a ptrace supervisor round-trip *per
 * directory open*. GTK/GIMP scan many directories at startup (icon themes, font
 * dirs, pixbuf loaders, GIO modules, config dirs), so each is a trap today.
 *
 * By emitting the openat ourselves via alr_open_emit (the same trusted-PC,
 * RESOLVE_IN_ROOT-or-string-rewrite path the open() family already uses), the
 * directory open is ALLOWed without tracing, and the remaining fdopendir work
 * (fstat/fcntl/getdents on the *fd*) issues only NON-path syscalls that never
 * trap. Net: one fewer path_trap per opendir, with byte-identical results.
 *
 * Semantics match glibc's __opendirat: O_RDONLY|O_NONBLOCK|O_DIRECTORY|O_CLOEXEC
 * (LARGEFILE is implicit on arm64). fdopendir takes ownership of the fd on
 * success; on failure it does NOT close it, so we close it ourselves to avoid an
 * fd leak — exactly glibc's own contract. PCGATE=0 keeps the original
 * RTLD_NEXT + string-rewrite behavior verbatim.
 */
void *opendir(const char *path) {
    if (g_pcgate) {
        int fd = alr_open_emit(AT_FDCWD, path,
                               O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC, 0);
        if (fd < 0) return NULL;                 /* errno already set by alr_ret */
        static void *(*real_fdopendir)(int);
        ALR_REAL(real_fdopendir, void *(*)(int), "fdopendir");
        if (!real_fdopendir) {                   /* should never happen on glibc */
            alr_tramp_syscall(__NR_close, fd, 0, 0, 0, 0, 0);
            errno = ENOSYS;
            return NULL;
        }
        void *d = real_fdopendir(fd);
        if (!d) {
            int e = errno;                        /* preserve fdopendir's errno */
            alr_tramp_syscall(__NR_close, fd, 0, 0, 0, 0, 0);
            errno = e;
        }
        return d;
    }
    static void *(*real)(const char *);
    ALR_REAL(real, void *(*)(const char *), "opendir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}

/* scandir/scandir64 stay on the RTLD_NEXT + string-rewrite path. They internally
 * opendir() the directory (a traced openat), so they still trap once per call —
 * but scandir is comparatively rare at GUI startup vs opendir, and re-expressing
 * scandir without its libc body (qsort + dirent alloc/filter callbacks) would be
 * far higher risk than its trap saving warrants. The string rewrite continues to
 * mediate correctly; deferred to USER_NOTIF (ADR-001 §4 candidate 1) if it ever
 * dominates a measured trace count. */
int scandir(const char *path, void *namelist, void *filter, void *compar) {
    static int (*real)(const char *, void *, void *, void *);
    ALR_REAL(real, int (*)(const char *, void *, void *, void *), "scandir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), namelist, filter, compar);
}
int scandir64(const char *path, void *namelist, void *filter, void *compar) {
    static int (*real)(const char *, void *, void *, void *);
    ALR_REAL(real, int (*)(const char *, void *, void *, void *), "scandir64");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), namelist, filter, compar);
}

/* =================================================================== */
/* mutating single-path ops                                            */
/* =================================================================== */

/* mkdir/mkdirat -> __NR_mkdirat (traced 9). */
static int alr_mkdir_emit(int dirfd, const char *path, mode_t mode) {
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r = alr_tramp_syscall(__NR_mkdirat, dirfd, (long)p, mode, 0, 0, 0);
    return (int)alr_ret(r);
}
int mkdir(const char *path, mode_t mode) {
    if (g_pcgate) return alr_mkdir_emit(AT_FDCWD, path, mode);
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "mkdir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int mkdirat(int dirfd, const char *path, mode_t mode) {
    if (g_pcgate) return alr_mkdir_emit(dirfd, path, mode);
    static int (*real)(int, const char *, mode_t);
    ALR_REAL(real, int (*)(int, const char *, mode_t), "mkdirat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode);
}

/* unlink/rmdir/unlinkat/remove -> __NR_unlinkat (traced 9). */
static int alr_unlink_emit(int dirfd, const char *path, int flags) {
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    long r = alr_tramp_syscall(__NR_unlinkat, dirfd, (long)p, flags, 0, 0, 0);
    return (int)alr_ret(r);
}
int rmdir(const char *path) {
    if (g_pcgate) return alr_unlink_emit(AT_FDCWD, path, AT_REMOVEDIR);
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "rmdir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int unlink(const char *path) {
    if (g_pcgate) return alr_unlink_emit(AT_FDCWD, path, 0);
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "unlink");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int unlinkat(int dirfd, const char *path, int flags) {
    if (g_pcgate) return alr_unlink_emit(dirfd, path, flags);
    static int (*real)(int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int), "unlinkat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, flags);
}
int remove(const char *path) {
    /* glibc tries unlink then rmdir; replicate via unlinkat, retrying with
     * AT_REMOVEDIR on EISDIR. Both syscalls are the traced __NR_unlinkat. */
    if (g_pcgate) {
        int r = alr_unlink_emit(AT_FDCWD, path, 0);
        if (r != 0 && errno == EISDIR)
            r = alr_unlink_emit(AT_FDCWD, path, AT_REMOVEDIR);
        return r;
    }
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "remove");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int chdir(const char *path) {
    static int (*real)(const char *);
    ALR_REAL(real, int (*)(const char *), "chdir");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
int chmod(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "chmod");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    static int (*real)(int, const char *, mode_t, int);
    ALR_REAL(real, int (*)(int, const char *, mode_t, int), "fchmodat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, flags);
}

struct utimbuf;  /* opaque; we only forward the pointer */
int utime(const char *path, const struct utimbuf *times) {
    static int (*real)(const char *, const struct utimbuf *);
    ALR_REAL(real, int (*)(const char *, const struct utimbuf *), "utime");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), times);
}
int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
    static int (*real)(int, const char *, const struct timespec[2], int);
    ALR_REAL(real, int (*)(int, const char *, const struct timespec[2], int),
             "utimensat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, times, flags);
}

/* =================================================================== */
/* two-path ops (rewrite BOTH path arguments)                          */
/* =================================================================== */

int rename(const char *oldp, const char *newp) {
    static int (*real)(const char *, const char *);
    ALR_REAL(real, int (*)(const char *, const char *), "rename");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    return real(rw(oldp, b1, sizeof b1), rw(newp, b2, sizeof b2));
}
int renameat(int oldfd, const char *oldp, int newfd, const char *newp) {
    static int (*real)(int, const char *, int, const char *);
    ALR_REAL(real, int (*)(int, const char *, int, const char *), "renameat");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    const char *o = (oldp && oldp[0] == '/') ? rw(oldp, b1, sizeof b1) : oldp;
    const char *n = (newp && newp[0] == '/') ? rw(newp, b2, sizeof b2) : newp;
    return real(oldfd, o, newfd, n);
}
int renameat2(int oldfd, const char *oldp, int newfd, const char *newp,
              unsigned int flags) {
    /* __NR_renameat2 is NOT in the traced 9 (in either mode), so its two-path
     * string rewrite is the sole mediation -- preserved exactly; no trampoline
     * re-route needed (the BPF would ALLOW it at any PC). */
    static int (*real)(int, const char *, int, const char *, unsigned int);
    ALR_REAL(real, int (*)(int, const char *, int, const char *, unsigned int),
             "renameat2");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    const char *o = (oldp && oldp[0] == '/') ? rw(oldp, b1, sizeof b1) : oldp;
    const char *n = (newp && newp[0] == '/') ? rw(newp, b2, sizeof b2) : newp;
    return real(oldfd, o, newfd, n, flags);
}

/* =================================================================== */
/* link / node-creation ops                                            */
/* =================================================================== */
/* These were previously UNWRAPPED -- a pre-existing correctness gap (absolute
 * link/node paths were not rewritten into the rootfs) independent of PCGATE.
 * Their underlying syscalls (linkat/symlinkat/mknodat) are NOT in the traced 9,
 * so they forward via RTLD_NEXT after a string rewrite, matching the
 * rename/renameat pattern. CAUTION: for symlink/symlinkat the FIRST arg is the
 * symlink *contents* (target), stored verbatim by the kernel -- it is NOT a
 * filesystem location to resolve now, so it must NOT be rewritten; only the
 * linkpath (where the symlink is created) is rewritten. */

int link(const char *oldp, const char *newp) {
    static int (*real)(const char *, const char *);
    ALR_REAL(real, int (*)(const char *, const char *), "link");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    /* both args are existing/target filesystem paths -> rewrite both */
    return real(rw(oldp, b1, sizeof b1), rw(newp, b2, sizeof b2));
}
int linkat(int oldfd, const char *oldp, int newfd, const char *newp, int flags) {
    static int (*real)(int, const char *, int, const char *, int);
    ALR_REAL(real, int (*)(int, const char *, int, const char *, int), "linkat");
    char b1[ALR_PBUF], b2[ALR_PBUF];
    const char *o = (oldp && oldp[0] == '/') ? rw(oldp, b1, sizeof b1) : oldp;
    const char *n = (newp && newp[0] == '/') ? rw(newp, b2, sizeof b2) : newp;
    return real(oldfd, o, newfd, n, flags);
}
int symlink(const char *target, const char *linkpath) {
    static int (*real)(const char *, const char *);
    ALR_REAL(real, int (*)(const char *, const char *), "symlink");
    char b[ALR_PBUF];
    /* target is the link CONTENTS -> left untouched; only linkpath rewritten */
    return real(target, rw(linkpath, b, sizeof b));
}
int symlinkat(const char *target, int newfd, const char *linkpath) {
    static int (*real)(const char *, int, const char *);
    ALR_REAL(real, int (*)(const char *, int, const char *), "symlinkat");
    char b[ALR_PBUF];
    const char *lp = (linkpath && linkpath[0] == '/') ? rw(linkpath, b, sizeof b) : linkpath;
    return real(target, newfd, lp);
}
int mknod(const char *path, mode_t mode, dev_t dev) {
    static int (*real)(const char *, mode_t, dev_t);
    ALR_REAL(real, int (*)(const char *, mode_t, dev_t), "mknod");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode, dev);
}
int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
    static int (*real)(int, const char *, mode_t, dev_t);
    ALR_REAL(real, int (*)(int, const char *, mode_t, dev_t), "mknodat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode, dev);
}
int mkfifo(const char *path, mode_t mode) {
    static int (*real)(const char *, mode_t);
    ALR_REAL(real, int (*)(const char *, mode_t), "mkfifo");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), mode);
}
int mkfifoat(int dirfd, const char *path, mode_t mode) {
    static int (*real)(int, const char *, mode_t);
    ALR_REAL(real, int (*)(int, const char *, mode_t), "mkfifoat");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    return real(dirfd, p, mode);
}

/* =================================================================== */
/* non-path, result-only, side-effect-free getters (process credentials) */
/* =================================================================== */
/*
 * ADR-001 §4 (candidate 4 — PCGATE non-path expansion). These syscalls are NOT
 * in the traced 9, so the PC gate already RET_ALLOWs them: they generate NO
 * path_trap, NO ptrace round-trip. What they DO pay is the ~24 ns/syscall
 * seccomp DISPATCH floor (docs/design/pcgate-seccomp.md §"Filter instruction
 * layout"; ADR-001 §1-B) on EVERY invocation. GTK/GLib/GIO call the credential
 * getters incessantly during startup (GLib's g_get_user_*, theme/permission
 * checks), so memoizing them removes those syscalls — and their dispatch cost —
 * entirely. This is the ONLY way to drive a syscall class below the 24 ns floor
 * (ADR §1-B: seccomp-on => 0% impossible; the only escape is to not issue the
 * syscall), mirroring the vDSO rationale (ADR §3 candidate 3a) for clocks.
 *
 * SAFETY — why these are cacheable WITHOUT side effects or correctness risk:
 *   - Process credentials (uid/euid/gid/egid) are immutable for an untrusted_app
 *     guest: it has no CAP_SETUID and cannot setuid/setgid, and a child of a
 *     fork() inherits IDENTICAL credentials — so a process-wide cache is correct
 *     across any guest fork too (unlike getpid, which a fork would invalidate;
 *     getpid/getppid/gettid are therefore deliberately NOT cached here).
 *   - Result-only: no pointer args, no memory the caller may have changed
 *     (no TOCTOU — the ADR's reason path-rewrite must NOT move to a cache/notif).
 *   - The first call resolves the real value through the trampoline (trusted PC);
 *     every subsequent call returns the cached value with zero syscalls.
 *
 * The cache slots are written once with the kernel's own value and only read
 * afterward; the benign double-resolve race (two threads before first publish)
 * stores the same value, matching the ALR_REAL slot convention. PCGATE=0 reverts
 * each wrapper to the real libc getter via RTLD_NEXT (no cache, no trampoline) so
 * the A/B baseline is unchanged.
 */
#ifndef __NR_getuid
#define __NR_getuid  174
#endif
#ifndef __NR_geteuid
#define __NR_geteuid 175
#endif
#ifndef __NR_getgid
#define __NR_getgid  176
#endif
#ifndef __NR_getegid
#define __NR_getegid 177
#endif

/* -1 sentinel = "not yet resolved" (no real uid/gid is (uid_t)-1 for a guest;
 * even if it were, a re-resolve through the trampoline is harmless). */
#define ALR_ID_UNSET ((uid_t)-1)

static uid_t alr_cached_id(uid_t *slot, long nr) {
    uid_t v = *slot;
    if (v != ALR_ID_UNSET) return v;
    long r = alr_tramp_syscall(nr, 0, 0, 0, 0, 0, 0);   /* never fails (no args) */
    v = (uid_t)r;
    *slot = v;
    return v;
}

uid_t getuid(void) {
    if (g_pcgate) { static uid_t c = ALR_ID_UNSET; return alr_cached_id(&c, __NR_getuid); }
    static uid_t (*real)(void);
    ALR_REAL(real, uid_t (*)(void), "getuid");
    return real();
}
uid_t geteuid(void) {
    if (g_pcgate) { static uid_t c = ALR_ID_UNSET; return alr_cached_id(&c, __NR_geteuid); }
    static uid_t (*real)(void);
    ALR_REAL(real, uid_t (*)(void), "geteuid");
    return real();
}
gid_t getgid(void) {
    if (g_pcgate) { static uid_t c = ALR_ID_UNSET; return (gid_t)alr_cached_id(&c, __NR_getgid); }
    static gid_t (*real)(void);
    ALR_REAL(real, gid_t (*)(void), "getgid");
    return real();
}
gid_t getegid(void) {
    if (g_pcgate) { static uid_t c = ALR_ID_UNSET; return (gid_t)alr_cached_id(&c, __NR_getegid); }
    static gid_t (*real)(void);
    ALR_REAL(real, gid_t (*)(void), "getegid");
    return real();
}

/* =================================================================== */
/* path canonicalization                                               */
/* =================================================================== */

/* realpath(path, resolved): we rewrite the *input* path. The resolved output
 * will then be a rootfs host path, which is correct for a subsequent open()
 * (already-host paths are left alone by both us and the seccomp net). */
char *realpath(const char *path, char *resolved) {
    static char *(*real)(const char *, char *);
    ALR_REAL(real, char *(*)(const char *, char *), "realpath");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b), resolved);
}
char *canonicalize_file_name(const char *path) {
    static char *(*real)(const char *);
    ALR_REAL(real, char *(*)(const char *), "canonicalize_file_name");
    char b[ALR_PBUF];
    return real(rw(path, b, sizeof b));
}
