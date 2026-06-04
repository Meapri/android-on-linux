/*
 * alr_pts.c — pure (syscall-free) core of the in-process PTY emulation.
 *
 * See alr_pts.h for the design rationale. This translation unit contains ONLY
 * logic that touches a `struct alr_pty` plus the caller's ioctl argument
 * buffer: path classification, ptn parsing, termios/winsize defaults, and the
 * ioctl dispatch. It performs NO syscalls and resolves NO symbols, so it links
 * cleanly into both libalr_interpose.so (aarch64, via build-interpose.sh) and a
 * host unit-test binary (native, via the C toolchain). The fd-minting
 * (socketpair) and the libc wrappers that own the PTY table live in
 * libalr_interpose.c.
 *
 * All termios work is done in the KERNEL wire layout (struct alr_ktermios /
 * struct alr_ktermios2 from alr_pts.h), never glibc's larger struct termios —
 * because that is exactly what the kernel sees at the ioctl boundary after
 * glibc's tcgetattr/tcsetattr has translated.
 */
#include "alr_pts.h"

#include <errno.h>
#include <string.h>   /* memcpy/memset — compiler builtins; no syscall */

/* ---- KERNEL termios flag/index constants (explicit, layout-independent) ----
 * The flag BIT values are identical across glibc and the kernel on Linux (same
 * ABI); only the struct LAYOUT differs (which is why we keep our own structs).
 * We spell them out here so this file needs no <termios.h> and the wire format
 * is unambiguous. Values verified against the aarch64 sysroot. */
#define K_ISIG    0000001
#define K_ICANON  0000002
#define K_ECHO    0000010
#define K_ECHOE   0000020
#define K_ECHOK   0000040
#define K_ECHOCTL 0001000
#define K_IEXTEN  0100000
/* c_iflag */
#define K_BRKINT  0000002
#define K_ICRNL   0000400
#define K_IXON    0002000
#define K_IMAXBEL 0020000
/* c_oflag */
#define K_OPOST   0000001
#define K_ONLCR   0000004
/* c_cflag */
#define K_CS8     0000060
#define K_CREAD   0000200
#define K_HUPCL   0002000
#define K_B38400  0000017   /* kernel CBAUD-encoded 38400 (octal 17) */
/* c_cc indices (kernel ordering; matches glibc symbolic V* on Linux) */
#define K_VINTR    0
#define K_VQUIT    1
#define K_VERASE   2
#define K_VKILL    3
#define K_VEOF     4
#define K_VTIME    5
#define K_VMIN     6
#define K_VSTART   8
#define K_VSTOP    9
#define K_VSUSP   10
#define K_VEOL    11
#define K_VREPRINT 12
#define K_VWERASE 14
#define K_VLNEXT  15
#define K_VEOL2   16
/* CTRL(): control-char value. <sys/ttydefaults.h> (pulled in via <sys/ioctl.h>)
 * already defines an identical CTRL(x) = (x & 037); guard to avoid redefining
 * it under -Wmacro-redefined while still working if that header is absent. */
#ifndef CTRL
#define CTRL(c) ((c) & 0x1f)
#endif

/* PTY ioctl request numbers — explicit so this file is self-contained and the
 * dispatch is a constant-value switch (TCGETS2/TCSETS2 cannot use the macros;
 * see alr_pts.h). These are the stable aarch64/asm-generic encodings. */
#define K_TCGETS     0x5401UL
#define K_TCSETS     0x5402UL
#define K_TCSETSW    0x5403UL
#define K_TCSETSF    0x5404UL
#define K_TIOCSCTTY  0x540EUL
#define K_TIOCGPGRP  0x540FUL
#define K_TIOCSPGRP  0x5410UL
#define K_TIOCGWINSZ 0x5413UL
#define K_TIOCSWINSZ 0x5414UL
#define K_TIOCNOTTY  0x5422UL
#define K_TIOCGSID   0x5429UL
#define K_TIOCGPTN   0x80045430UL  /* _IOR('T',0x30,unsigned int) */
#define K_TIOCSPTLCK 0x40045431UL  /* _IOW('T',0x31,int) */
#define K_TIOCGPTPEER 0x5441UL     /* _IO('T',0x41) */
/* TCGETS2/TCSETS2 family come from alr_pts.h: ALR_TCGETS2 / ALR_TCSETS2 / … */

void alr_pty_init_termios(struct alr_ktermios2 *t)
{
    if (!t) return;
    memset(t, 0, sizeof *t);
    /* The kernel pty driver's default "cooked" line discipline (drivers/tty/
     * pty.c → tty_init_termios → tty_std_termios). */
    t->c_iflag = K_ICRNL | K_IXON | K_BRKINT | K_IMAXBEL;
    t->c_oflag = K_OPOST | K_ONLCR;
    t->c_cflag = K_CS8 | K_CREAD | K_HUPCL | K_B38400;
    t->c_lflag = K_ISIG | K_ICANON | K_ECHO | K_ECHOE | K_ECHOK | K_ECHOCTL | K_IEXTEN;
    t->c_cc[K_VINTR]    = CTRL('C');
    t->c_cc[K_VQUIT]    = CTRL('\\');
    t->c_cc[K_VERASE]   = 0177;        /* DEL */
    t->c_cc[K_VKILL]    = CTRL('U');
    t->c_cc[K_VEOF]     = CTRL('D');
    t->c_cc[K_VTIME]    = 0;
    t->c_cc[K_VMIN]     = 1;
    t->c_cc[K_VSTART]   = CTRL('Q');
    t->c_cc[K_VSTOP]    = CTRL('S');
    t->c_cc[K_VSUSP]    = CTRL('Z');
    t->c_cc[K_VEOL]     = 0;
    t->c_cc[K_VREPRINT] = CTRL('R');
    t->c_cc[K_VWERASE]  = CTRL('W');
    t->c_cc[K_VLNEXT]   = CTRL('V');
    t->c_cc[K_VEOL2]    = 0;
    t->c_ispeed = 38400;
    t->c_ospeed = 38400;
}

void alr_pty_init_winsize(struct winsize *w)
{
    if (!w) return;
    w->ws_row    = 24;
    w->ws_col    = 80;
    w->ws_xpixel = 0;
    w->ws_ypixel = 0;
}

/* ---- path classification ---- */

int alr_pts_is_ptmx_path(const char *path)
{
    if (!path) return 0;
    return strcmp(path, "/dev/ptmx") == 0 ||
           strcmp(path, "/dev/pts/ptmx") == 0;
}

int alr_pts_is_ptsdir_path(const char *path)
{
    if (!path) return 0;
    return strcmp(path, "/dev/pts") == 0 ||
           strcmp(path, "/dev/pts/") == 0;
}

int alr_pts_is_tty_path(const char *path)
{
    if (!path) return 0;
    return strcmp(path, "/dev/tty") == 0;
}

int alr_pts_parse_slave_path(const char *path)
{
    if (!path) return -1;
    static const char pfx[] = "/dev/pts/";
    size_t i = 0;
    for (; pfx[i]; ++i) {
        if (path[i] != pfx[i]) return -1;
    }
    const char *d = path + i;          /* the part after "/dev/pts/" */
    if (d[0] < '0' || d[0] > '9') return -1;   /* "ptmx", "", non-digit → no */
    long n = 0;
    for (; *d; ++d) {
        if (*d < '0' || *d > '9') return -1;    /* trailing junk → not a slave */
        n = n * 10 + (*d - '0');
        if (n > 1000000) return -1;             /* absurd → reject (overflow guard) */
    }
    return (int)n;
}

size_t alr_pts_slave_env_name(int ptn, char *buf, size_t buflen)
{
    if (!buf || ptn < 0) return 0;
    static const char pfx[] = "ALR_PTY_SLAVE_";
    size_t plen = sizeof pfx - 1;                 /* strlen, compile-time */
    /* decimal digits of ptn (ptn >= 0 here) */
    char dig[16]; int di = 0;
    unsigned int u = (unsigned int)ptn;
    if (u == 0) dig[di++] = '0';
    while (u && di < (int)sizeof dig) { dig[di++] = (char)('0' + u % 10); u /= 10; }
    /* need prefix + digits + NUL */
    if (buflen < plen + (size_t)di + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < plen; ++i) buf[o++] = pfx[i];
    while (di > 0) buf[o++] = dig[--di];
    buf[o] = '\0';
    return o;
}

/* ---- ioctl dispatch ----
 *
 * Operates on the kernel wire layout. `arg` is the third ioctl() argument (a
 * user pointer for the struct-carrying ioctls, or an int-by-value reinterpreted
 * as a pointer's bits for TIOCSCTTY/TCSBRK — but the only by-value one we honor
 * is TIOCSCTTY where the arg is ignored). We never dereference NULL: a struct
 * ioctl with arg==NULL returns -EINVAL (the kernel returns -EFAULT, but EINVAL
 * is safer to surface and apps treat both as "ioctl failed"). */
long alr_pts_emulate_ioctl(struct alr_pty *pty, int is_master,
                           unsigned long request, void *arg)
{
    if (!pty || !pty->used) return ALR_PTS_IOCTL_PASS;

    switch (request) {

    /* ---- pty-master control (Linux: master-only) ---- */
    case K_TIOCGPTN: {
        if (!is_master) return -EINVAL;
        if (!arg) return -EINVAL;
        unsigned int n = (unsigned int)pty->ptn;
        memcpy(arg, &n, sizeof n);
        return 0;
    }
    case K_TIOCSPTLCK: {
        if (!is_master) return -EINVAL;
        if (!arg) return -EINVAL;
        int lock;
        memcpy(&lock, arg, sizeof lock);
        pty->locked = lock ? 1 : 0;     /* unlockpt() passes 0 */
        return 0;
    }
    case K_TIOCGPTPEER:
        /* Open the slave end of THIS master. The dispatch cannot dup (no
         * syscalls); signal the caller to dup pty->slave_fd. Master-only. */
        if (!is_master) return -EINVAL;
        return ALR_PTS_IOCTL_GPTPEER;

    /* ---- window size (both ends) ---- */
    case K_TIOCGWINSZ:
        if (!arg) return -EINVAL;
        memcpy(arg, &pty->win, sizeof pty->win);
        return 0;
    case K_TIOCSWINSZ:
        if (!arg) return -EINVAL;
        memcpy(&pty->win, arg, sizeof pty->win);
        return 0;

    /* ---- termios get/set (both ends). TCSETS/TCSETSW/TCSETSF differ only in
     * when the kernel would drain/flush a real UART; for a socket-backed pty
     * there is nothing to drain, so all three behave identically. ---- */
    case K_TCGETS: {
        if (!arg) return -EINVAL;
        struct alr_ktermios k;
        memset(&k, 0, sizeof k);
        k.c_iflag = pty->tio.c_iflag;
        k.c_oflag = pty->tio.c_oflag;
        k.c_cflag = pty->tio.c_cflag;
        k.c_lflag = pty->tio.c_lflag;
        k.c_line  = pty->tio.c_line;
        memcpy(k.c_cc, pty->tio.c_cc, sizeof k.c_cc);
        memcpy(arg, &k, sizeof k);
        return 0;
    }
    case K_TCSETS:
    case K_TCSETSW:
    case K_TCSETSF: {
        if (!arg) return -EINVAL;
        struct alr_ktermios k;
        memcpy(&k, arg, sizeof k);
        pty->tio.c_iflag = k.c_iflag;
        pty->tio.c_oflag = k.c_oflag;
        pty->tio.c_cflag = k.c_cflag;
        pty->tio.c_lflag = k.c_lflag;
        pty->tio.c_line  = k.c_line;
        memcpy(pty->tio.c_cc, k.c_cc, sizeof k.c_cc);
        return 0;
    }
    case ALR_TCGETS2: {
        if (!arg) return -EINVAL;
        memcpy(arg, &pty->tio, sizeof pty->tio);    /* 44-byte termios2 */
        return 0;
    }
    case ALR_TCSETS2:
    case ALR_TCSETSW2:
    case ALR_TCSETSF2: {
        if (!arg) return -EINVAL;
        memcpy(&pty->tio, arg, sizeof pty->tio);
        return 0;
    }

    /* ---- session / job control (so bash job control + setsid succeed) ---- */
    case K_TIOCSCTTY:
        /* Make this (slave) the controlling terminal. We don't model the full
         * session table; record nothing extra and report success — the
         * terminal's child has already setsid()'d and just needs this to not
         * error. arg (the "steal" flag) is ignored. */
        return 0;
    case K_TIOCSPGRP: {
        if (!arg) return -EINVAL;
        pid_t pg;
        memcpy(&pg, arg, sizeof pg);
        pty->pgrp = pg;
        return 0;
    }
    case K_TIOCGPGRP: {
        if (!arg) return -EINVAL;
        /* Default to the session/own pgrp if never set, so tcgetpgrp() returns
         * a plausible positive pgid rather than 0. */
        pid_t pg = pty->pgrp;
        memcpy(arg, &pg, sizeof pg);
        return 0;
    }
    case K_TIOCGSID: {
        if (!arg) return -EINVAL;
        pid_t sid = pty->sid;
        memcpy(arg, &sid, sizeof sid);
        return 0;
    }
    case K_TIOCNOTTY:
        /* Drop the controlling terminal. No session table to update; succeed. */
        return 0;

    default:
        /* Not a PTY ioctl we emulate. The caller falls through to the real
         * ioctl on the socket fd — FIONREAD (bytes available), TIOCOUTQ, the
         * non-blocking/async flags, etc. all work on the underlying socket. */
        return ALR_PTS_IOCTL_PASS;
    }
}
