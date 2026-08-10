/* PTY-slave ioctl census, compiled against the GUEST's glibc.
 *
 * docs/04-preload-spec.md §11 specifies an ioctl translation table on the
 * claim (docs/01-platform-facts.md §B5) that Android whitelists 13 slave-side
 * ioctls and denies the rest with EACCES via SELinux ioctlcmd filters.  That
 * table was never implemented, and what actually breaks without it was never
 * measured -- this probe is the measurement.
 *
 * The pair is created HERE, in the guest process, via /dev/ptmx -- the same
 * way script(1), tmux and expect get theirs.  A denial on this fd is SELinux
 * policy (errno, not SIGSYS), so results print as name=errno.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* asm ioctls.h defines TCGETS2 as _IOR('T',0x2A,struct termios2) and termios2
 * is not exposed through <termios.h>, so sizeof explodes -- use the resolved
 * aarch64 number directly. */
#define ALR_TCGETS2 0x802c542aUL
#ifndef TIOCGSID
#define TIOCGSID 0x5429
#endif
#ifndef TIOCGETD
#define TIOCGETD 0x5424
#endif

static void try1(const char *name, int fd, unsigned long req, void *arg)
{
    errno = 0;
    if (ioctl(fd, req, arg) == 0) printf("%-12s ok\n", name);
    else printf("%-12s %s\n", name, strerror(errno));
}

int main(void)
{
    int m, s;
    struct termios t;
    struct winsize w = { 24, 80, 0, 0 };
    pid_t pg;
    int n, ld;
    char buf[64];

    if (openpty(&m, &s, NULL, NULL, NULL) != 0) {
        printf("openpty: %s\n", strerror(errno));
        return 1;
    }
    printf("openpty ok (master=%d slave=%d)\n", m, s);
    printf("── slave ──\n");
    try1("TCGETS",     s, TCGETS,     &t);
    try1("TCSETS",     s, TCSETS,     &t);
    try1("TCGETS2",    s, ALR_TCGETS2, buf);
    try1("TIOCGWINSZ", s, TIOCGWINSZ, &w);
    try1("TIOCSWINSZ", s, TIOCSWINSZ, &w);
    try1("TIOCGPGRP",  s, TIOCGPGRP,  &pg);
    try1("FIONREAD",   s, FIONREAD,   &n);
    try1("TIOCOUTQ",   s, TIOCOUTQ,   &n);
    try1("TIOCGSID",   s, TIOCGSID,   &pg);
    try1("TIOCGETD",   s, TIOCGETD,   &ld);
    try1("TIOCEXCL",   s, TIOCEXCL,   NULL);
    try1("TIOCSTI",    s, TIOCSTI,    "x");
    printf("── master ──\n");
    try1("FIONREAD",   m, FIONREAD,   &n);
    try1("TIOCGWINSZ", m, TIOCGWINSZ, &w);

    /* FIONREAD with pending data -- the readline/ncurses use case. */
    if (write(m, "abc", 3) == 3) {
        usleep(50000);
        errno = 0;
        n = -1;
        if (ioctl(s, FIONREAD, &n) == 0) printf("FIONREAD(slave,3 pending) ok n=%d\n", n);
        else printf("FIONREAD(slave,3 pending) %s\n", strerror(errno));
    }
    return 0;
}
