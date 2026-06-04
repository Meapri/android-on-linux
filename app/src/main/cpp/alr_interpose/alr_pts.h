/*
 * alr_pts.h — in-process PTY (pseudo-terminal) emulation for the ALR path
 * interposer.
 *
 * WHY THIS EXISTS
 * ---------------
 * A non-root Android app cannot get a working /dev/pts: there is no devpts
 * instance the app may mount, and /dev/ptmx either does not exist or, when it
 * does, opening it does not allocate a usable kernel PTY for an unprivileged
 * app in the app sandbox. That breaks the entire TERMINAL app class — a guest
 * terminal emulator (sakura, xterm) opens /dev/ptmx to get a master, forks a
 * shell on the slave /dev/pts/N, and pumps bytes between them. With no devpts
 * the openat("/dev/ptmx") fails and the terminal never starts.
 *
 * THE EMULATION
 * -------------
 * We give the guest a *virtual* PTY pair backed by a socketpair(AF_UNIX,
 * SOCK_STREAM): fd[0] is the "master", fd[1] is the "slave". A UNIX stream
 * socketpair is bidirectional, ordered and reliable — exactly the byte channel
 * a PTY master/slave pair provides. Both ends live in the same process; the
 * terminal keeps the master, the forked child dup2()s the slave onto 0/1/2 and
 * execs the shell. Because the data path is a real kernel object, read()/
 * write()/poll()/close() on either end work through libc UNCHANGED — the
 * interposer does NOT wrap them. We only need to:
 *
 *   1. Intercept open()/openat() of "/dev/ptmx" (and "/dev/pts/ptmx") → mint a
 *      pair, remember it under a virtual index N, and return the master fd.
 *   2. Intercept open()/openat() of "/dev/pts/N" → return a dup of that pair's
 *      slave fd. "/dev/tty" → the calling process's controlling-tty slave.
 *   3. Emulate the PTY/termios ioctls on those fds (TIOCGPTN, TIOCSPTLCK,
 *      TIOCGPTPEER, TIOC[GS]WINSZ, TCGETS/TCSETS*, TCGETS2/TCSETS2, TIOCSCTTY,
 *      TIOC[GS]PGRP, TIOCGSID, TIOCNOTTY, …). A raw socket answers these with
 *      ENOTTY, which would make isatty() false and abort the terminal; the
 *      emulation answers them from a per-PTY struct termios + winsize instead.
 *   4. Make isatty()/tcgetattr()/tcsetattr()/tcgetpgrp()/tcsetpgrp()/ptsname_r()
 *      report TRUE/valid for both ends.
 *
 * LINE DISCIPLINE (the one honest limitation)
 * -------------------------------------------
 * A socketpair has no kernel line discipline: no canonical line buffering and
 * no kernel ECHO. We TRACK the termios bits faithfully (so tcgetattr/tcsetattr
 * round-trip and apps that *query* the mode see what they set) but we do NOT
 * synthesize canonical-mode kernel echo. In practice this is sufficient for the
 * real terminal workload: an interactive shell (bash/readline) and full-screen
 * TUIs (htop, vim, less) put the tty into RAW mode and do their OWN echo and
 * line editing, so they render and respond correctly. A program that relies on
 * the *kernel* to echo in canonical mode (e.g. a bare `cat` with no prompt)
 * will run but not see typed characters echoed. This is documented, not
 * silently wrong, and is the standard ceiling of a userspace PTY shim.
 *
 * TESTABILITY
 * -----------
 * The fd-minting (socketpair) and the libc wrappers live in
 * libalr_interpose.c. The PURE logic — path classification, ptn parsing,
 * termios/winsize defaults, and the ioctl dispatch that only touches a
 * struct alr_pty + the caller's argument buffer — lives in alr_pts.c and is
 * callable from a host unit test with no syscalls (see
 * tests/native_alr_pts_test.c).
 */
#ifndef ALR_PTS_H
#define ALR_PTS_H

#include <stddef.h>
#include <sys/types.h>   /* pid_t */
#include <sys/ioctl.h>   /* TIOC / TC request macros, struct winsize */

#ifdef __cplusplus
extern "C" {
#endif

/* KERNEL termios wire layout — NOT glibc's struct termios.
 *
 * glibc's `struct termios` on aarch64 is 60 bytes (NCCS=32 + c_ispeed/c_ospeed);
 * glibc's tcgetattr/tcsetattr TRANSLATE it to/from the kernel layout before the
 * ioctl. At the ioctl `svc` boundary the kernel sees the *kernel* struct:
 *   - TCGETS/TCSETS/TCSETSW/TCSETSF → struct termios (kernel), 36 bytes, NCCS=19
 *   - TCGETS2/TCSETS2/...           → struct termios2 (kernel), 44 bytes, NCCS=19
 *                                     (adds c_ispeed/c_ospeed)
 * Our emulation operates on whatever buffer the caller hands the ioctl, so it
 * must speak the KERNEL layout. We define both here and store the 44-byte
 * superset as the canonical per-PTY state. */
#define ALR_KNCCS 19
typedef unsigned int  alr_tcflag_t;
typedef unsigned char alr_cc_t;
typedef unsigned int  alr_speed_t;

struct alr_ktermios {                 /* TCGETS/TCSETS wire struct (36 bytes) */
    alr_tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    alr_cc_t     c_line;
    alr_cc_t     c_cc[ALR_KNCCS];
};

struct alr_ktermios2 {                /* TCGETS2/TCSETS2 wire struct (44 bytes) */
    alr_tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    alr_cc_t     c_line;
    alr_cc_t     c_cc[ALR_KNCCS];
    alr_speed_t  c_ispeed, c_ospeed;
};

/* Maximum number of concurrently-open virtual PTYs. A terminal opens exactly
 * one; even a tmux-like multiplexer rarely exceeds a handful. 64 is generous
 * and keeps the whole table in a single zero-initialized BSS array. */
#ifndef ALR_PTS_MAX
#define ALR_PTS_MAX 64
#endif

/* TCGETS2/TCSETS2 family — explicit numeric values. The glibc sysroot only
 * forward-declares `struct termios2`, so the TCGETS2/TCSETS2 macros (which
 * expand to sizeof(struct termios2)) cannot be used in a switch. These are the
 * stable asm-generic encodings for a 44-byte struct termios2 on aarch64
 * (verified by computing _IOR/_IOW('T',0x2A..0x2D, 44)). */
#define ALR_TCGETS2  0x802c542aUL
#define ALR_TCSETS2  0x402c542bUL
#define ALR_TCSETSW2 0x402c542cUL
#define ALR_TCSETSF2 0x402c542dUL

/* Per-PTY state. Backed by a socketpair; master_fd/slave_fd are the two ends. */
struct alr_pty {
    int                  used;        /* 0 = free slot */
    int                  ptn;         /* virtual pts index N (== array index) */
    int                  master_fd;   /* socketpair[0] — returned for /dev/ptmx */
    int                  slave_fd;    /* socketpair[1] — returned for /dev/pts/N */
    int                  locked;      /* TIOCSPTLCK: 1 = locked (initial) */
    int                  slave_opened;/* slave end opened at least once */
    pid_t                pgrp;        /* foreground process group (TIOC[GS]PGRP) */
    pid_t                sid;         /* session id (TIOCGSID / TIOCSCTTY) */
    struct alr_ktermios2 tio;         /* line-discipline / termios state (kernel) */
    struct winsize       win;         /* window size (TIOC[GS]WINSZ) */
};

/* ---- pure helpers (no syscalls) — unit-tested host-side ---- */

/* Initialize *t to a sane "cooked" PTY default: ICANON|ECHO|ISIG|... on,
 * sensible c_cc control chars, 38400 baud, 8N1. Mirrors what the kernel's
 * pty driver hands a freshly-opened slave so tcgetattr() on an un-configured
 * pty looks normal. Writes the kernel termios2 layout. */
void alr_pty_init_termios(struct alr_ktermios2 *t);

/* Initialize *w to 80x24 (cols x rows), 0 pixel size — the universal default
 * before a terminal issues its first TIOCSWINSZ. */
void alr_pty_init_winsize(struct winsize *w);

/* Path classification. All take a guest-visible absolute path.
 * Return 1 on match, 0 otherwise. NULL-safe. */
int  alr_pts_is_ptmx_path(const char *path);   /* "/dev/ptmx" | "/dev/pts/ptmx" */
int  alr_pts_is_ptsdir_path(const char *path); /* "/dev/pts" | "/dev/pts/"      */
int  alr_pts_is_tty_path(const char *path);    /* "/dev/tty"                    */

/* Parse "/dev/pts/N" → N (>= 0). Returns -1 if the path is not a
 * "/dev/pts/<decimal>" slave path (rejects "ptmx", empty, non-digits,
 * overflow). NULL-safe. */
int  alr_pts_parse_slave_path(const char *path);

/* Format the per-PTY inherited-slave-fd handoff env var NAME for virtual index
 * N into `buf` (size `buflen`): "ALR_PTY_SLAVE_<N>". This env var is set by the
 * minter (posix_openpt/open(ptmx)/openpty/forkpty) to the slave socketpair fd,
 * which survives fork; the forked child — whose in-process PTY table is an empty
 * COW copy — reads it to resolve open("/dev/pts/N") to the inherited fd. Returns
 * the number of bytes written (excluding the NUL), or 0 on bad args / overflow
 * (NULL buf, buflen too small, negative N). Pure: no syscalls, unit-tested. */
size_t alr_pts_slave_env_name(int ptn, char *buf, size_t buflen);

/* The ioctl dispatch core. Emulates the PTY/termios ioctls against *pty using
 * only the caller's `arg` buffer — NO syscalls. `is_master` distinguishes the
 * two ends (TIOCGPTN/TIOCSPTLCK/TIOCGPTPEER are master-only in Linux).
 *
 * Return convention (so the caller can set errno without a second branch):
 *   >= 0  handled; this is the ioctl return value (usually 0).
 *   < 0   handled with an error; the value is -errno (e.g. -EINVAL).
 *   ALR_PTS_IOCTL_PASS  not a PTY ioctl we emulate → caller should fall through
 *                       to the real ioctl (e.g. FIONREAD on the socket fd).
 *
 * For TIOCGPTPEER the dispatch cannot itself dup an fd (no syscalls), so it
 * returns the sentinel ALR_PTS_IOCTL_GPTPEER and the caller performs the dup of
 * pty->slave_fd. */
#define ALR_PTS_IOCTL_PASS      (0x7fffffffL)
#define ALR_PTS_IOCTL_GPTPEER   (0x7ffffffeL)

long alr_pts_emulate_ioctl(struct alr_pty *pty, int is_master,
                           unsigned long request, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* ALR_PTS_H */
