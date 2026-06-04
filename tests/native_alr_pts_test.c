/*
 * native_alr_pts_test.c — host unit tests for the syscall-free PTY-emulation
 * core (app/src/main/cpp/alr_interpose/alr_pts.c).
 *
 * Exercises path classification, /dev/pts/N parsing, the termios/winsize
 * defaults, and the full ioctl dispatch (TIOCGPTN/TIOCSPTLCK/TIOCGPTPEER,
 * TIOC[GS]WINSZ, TCGETS/TCSETS, TCGETS2/TCSETS2, TIOC[GS]PGRP, TIOCGSID,
 * TIOCSCTTY, TIOCNOTTY) against an in-memory struct alr_pty — NO syscalls,
 * NO sockets. The kernel ioctl request VALUES are spelled out here (the same
 * stable Linux/aarch64 encodings alr_pts.c switches on) so the test is
 * independent of the host's <sys/ioctl.h> (which differs on macOS).
 *
 * Compiled + run by tests/test_alr_pts_emulation.py with a host cc; mirrors the
 * tests/native_alr_doh_wire_test.c convention. Prints "all tests passed" and
 * exits 0 on success.
 */
#include "alr_pts.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>       /* read/write/close — for the socketpair data-flow test */
#include <sys/socket.h>   /* socketpair — proves the master/slave channel works */

/* Kernel TTY ioctl request numbers (aarch64/asm-generic; identical across the
 * Linux arches that matter here). Must match alr_pts.c's switch. */
#define R_TCGETS     0x5401UL
#define R_TCSETS     0x5402UL
#define R_TCSETSW    0x5403UL
#define R_TCSETSF    0x5404UL
#define R_TIOCSCTTY  0x540EUL
#define R_TIOCGPGRP  0x540FUL
#define R_TIOCSPGRP  0x5410UL
#define R_TIOCGWINSZ 0x5413UL
#define R_TIOCSWINSZ 0x5414UL
#define R_TIOCNOTTY  0x5422UL
#define R_TIOCGSID   0x5429UL
#define R_TIOCGPTN   0x80045430UL
#define R_TIOCSPTLCK 0x40045431UL
#define R_TIOCGPTPEER 0x5441UL
#define R_FIONREAD   0x541BUL   /* an ioctl we DON'T emulate → PASS */

/* kernel termios flag bits we assert on (ABI-stable) */
#define F_ISIG   0000001
#define F_ICANON 0000002
#define F_ECHO   0000010

static int g_fail = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { printf("FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
                   g_fail = 1; }                                           \
} while (0)

static struct alr_pty make_pty(int ptn) {
    struct alr_pty p;
    memset(&p, 0, sizeof p);
    p.used = 1;
    p.ptn = ptn;
    p.master_fd = 100 + ptn;     /* arbitrary; the pure core never touches fds */
    p.slave_fd  = 200 + ptn;
    p.locked = 1;
    alr_pty_init_termios(&p.tio);
    alr_pty_init_winsize(&p.win);
    return p;
}

static void test_path_classification(void) {
    CHECK(alr_pts_is_ptmx_path("/dev/ptmx") == 1, "ptmx");
    CHECK(alr_pts_is_ptmx_path("/dev/pts/ptmx") == 1, "pts/ptmx");
    CHECK(alr_pts_is_ptmx_path("/dev/pts/0") == 0, "pts/0 not ptmx");
    CHECK(alr_pts_is_ptmx_path("/dev/tty") == 0, "tty not ptmx");
    CHECK(alr_pts_is_ptmx_path(NULL) == 0, "NULL not ptmx");

    CHECK(alr_pts_is_ptsdir_path("/dev/pts") == 1, "ptsdir");
    CHECK(alr_pts_is_ptsdir_path("/dev/pts/") == 1, "ptsdir slash");
    CHECK(alr_pts_is_ptsdir_path("/dev/pts/3") == 0, "pts/3 not dir");

    CHECK(alr_pts_is_tty_path("/dev/tty") == 1, "tty");
    CHECK(alr_pts_is_tty_path("/dev/tty0") == 0, "tty0 not tty");
    CHECK(alr_pts_is_tty_path("/dev/ptmx") == 0, "ptmx not tty");
}

static void test_slave_parse(void) {
    CHECK(alr_pts_parse_slave_path("/dev/pts/0") == 0, "pts/0");
    CHECK(alr_pts_parse_slave_path("/dev/pts/7") == 7, "pts/7");
    CHECK(alr_pts_parse_slave_path("/dev/pts/42") == 42, "pts/42");
    CHECK(alr_pts_parse_slave_path("/dev/pts/123") == 123, "pts/123");
    CHECK(alr_pts_parse_slave_path("/dev/pts/ptmx") == -1, "ptmx not slave");
    CHECK(alr_pts_parse_slave_path("/dev/pts/") == -1, "empty not slave");
    CHECK(alr_pts_parse_slave_path("/dev/pts") == -1, "dir not slave");
    CHECK(alr_pts_parse_slave_path("/dev/pts/1a") == -1, "trailing junk");
    CHECK(alr_pts_parse_slave_path("/dev/pts/-1") == -1, "negative");
    CHECK(alr_pts_parse_slave_path("/dev/tty") == -1, "tty not slave");
    CHECK(alr_pts_parse_slave_path(NULL) == -1, "NULL not slave");
    CHECK(alr_pts_parse_slave_path("/dev/pts/99999999") == -1, "overflow rejected");
}

static void test_termios_defaults(void) {
    struct alr_ktermios2 t;
    alr_pty_init_termios(&t);
    CHECK((t.c_lflag & F_ICANON) != 0, "default ICANON on");
    CHECK((t.c_lflag & F_ECHO)   != 0, "default ECHO on");
    CHECK((t.c_lflag & F_ISIG)   != 0, "default ISIG on");
    CHECK(t.c_cc[4]  == ('D' & 0x1f), "VEOF=^D");   /* VEOF index 4 */
    CHECK(t.c_cc[6]  == 1,            "VMIN=1");     /* VMIN index 6 */
    CHECK(t.c_ispeed == 38400 && t.c_ospeed == 38400, "baud 38400");
}

static void test_winsize_defaults(void) {
    struct winsize w;
    alr_pty_init_winsize(&w);
    CHECK(w.ws_col == 80 && w.ws_row == 24, "default 80x24");
}

static void test_ioctl_ptmx_control(void) {
    struct alr_pty p = make_pty(5);

    /* TIOCGPTN on master → N=5 */
    unsigned int n = 0xffffffffu;
    long r = alr_pts_emulate_ioctl(&p, /*is_master*/1, R_TIOCGPTN, &n);
    CHECK(r == 0 && n == 5, "TIOCGPTN master returns ptn");

    /* TIOCGPTN on slave → EINVAL (master-only) */
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCGPTN, &n);
    CHECK(r == -EINVAL, "TIOCGPTN slave EINVAL");

    /* TIOCSPTLCK(0) = unlockpt → locked cleared */
    int lock = 0;
    r = alr_pts_emulate_ioctl(&p, 1, R_TIOCSPTLCK, &lock);
    CHECK(r == 0 && p.locked == 0, "unlockpt clears lock");
    lock = 1;
    r = alr_pts_emulate_ioctl(&p, 1, R_TIOCSPTLCK, &lock);
    CHECK(r == 0 && p.locked == 1, "lockpt sets lock");

    /* TIOCGPTPEER → sentinel (caller dups the slave) */
    r = alr_pts_emulate_ioctl(&p, 1, R_TIOCGPTPEER, (void *)(long)0);
    CHECK(r == ALR_PTS_IOCTL_GPTPEER, "TIOCGPTPEER sentinel");
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCGPTPEER, (void *)(long)0);
    CHECK(r == -EINVAL, "TIOCGPTPEER slave EINVAL");
}

static void test_ioctl_winsize(void) {
    struct alr_pty p = make_pty(0);
    struct winsize set = { .ws_row = 50, .ws_col = 132, .ws_xpixel = 1, .ws_ypixel = 2 };
    long r = alr_pts_emulate_ioctl(&p, 0, R_TIOCSWINSZ, &set);
    CHECK(r == 0, "TIOCSWINSZ ok");
    struct winsize got;
    memset(&got, 0, sizeof got);
    r = alr_pts_emulate_ioctl(&p, 1, R_TIOCGWINSZ, &got);
    CHECK(r == 0 && got.ws_row == 50 && got.ws_col == 132 &&
          got.ws_xpixel == 1 && got.ws_ypixel == 2, "TIOCGWINSZ round-trips");
}

static void test_ioctl_termios(void) {
    struct alr_pty p = make_pty(1);

    /* TCGETS → 36-byte kernel termios reflecting defaults */
    struct alr_ktermios k;
    memset(&k, 0xab, sizeof k);
    long r = alr_pts_emulate_ioctl(&p, 0, R_TCGETS, &k);
    CHECK(r == 0 && (k.c_lflag & F_ICANON) && (k.c_lflag & F_ECHO),
          "TCGETS reflects default lflag");

    /* Flip to raw (clear ICANON|ECHO|ISIG) via TCSETS, read back */
    k.c_lflag &= ~(unsigned)(F_ICANON | F_ECHO | F_ISIG);
    r = alr_pts_emulate_ioctl(&p, 0, R_TCSETS, &k);
    CHECK(r == 0, "TCSETS raw ok");
    struct alr_ktermios k2;
    memset(&k2, 0, sizeof k2);
    r = alr_pts_emulate_ioctl(&p, 0, R_TCGETS, &k2);
    CHECK(r == 0 && !(k2.c_lflag & F_ICANON) && !(k2.c_lflag & F_ECHO) &&
          !(k2.c_lflag & F_ISIG), "TCGETS sees raw mode after TCSETS");

    /* TCSETSW / TCSETSF behave identically (no drain on a socket) */
    k2.c_lflag |= F_ECHO;
    r = alr_pts_emulate_ioctl(&p, 0, R_TCSETSW, &k2);
    CHECK(r == 0, "TCSETSW ok");
    r = alr_pts_emulate_ioctl(&p, 0, R_TCSETSF, &k2);
    CHECK(r == 0, "TCSETSF ok");

    /* TCGETS2/TCSETS2 (44-byte termios2) round-trip incl. c_ispeed */
    struct alr_ktermios2 t2;
    memset(&t2, 0, sizeof t2);
    r = alr_pts_emulate_ioctl(&p, 0, ALR_TCGETS2, &t2);
    CHECK(r == 0 && t2.c_ispeed == 38400, "TCGETS2 carries c_ispeed");
    t2.c_ispeed = 115200; t2.c_ospeed = 115200; t2.c_lflag |= F_ICANON;
    r = alr_pts_emulate_ioctl(&p, 0, ALR_TCSETS2, &t2);
    CHECK(r == 0, "TCSETS2 ok");
    struct alr_ktermios2 t2b;
    memset(&t2b, 0, sizeof t2b);
    r = alr_pts_emulate_ioctl(&p, 0, ALR_TCGETS2, &t2b);
    CHECK(r == 0 && t2b.c_ispeed == 115200 && (t2b.c_lflag & F_ICANON),
          "TCGETS2 round-trips c_ispeed + lflag");
}

static void test_ioctl_jobcontrol(void) {
    struct alr_pty p = make_pty(2);

    /* TIOCSCTTY / TIOCNOTTY succeed (no session table to update) */
    long r = alr_pts_emulate_ioctl(&p, 0, R_TIOCSCTTY, (void *)(long)0);
    CHECK(r == 0, "TIOCSCTTY ok");
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCNOTTY, (void *)(long)0);
    CHECK(r == 0, "TIOCNOTTY ok");

    /* TIOC[GS]PGRP round-trip */
    pid_t pg = 4321;
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCSPGRP, &pg);
    CHECK(r == 0 && p.pgrp == 4321, "TIOCSPGRP stores pgrp");
    pid_t got = 0;
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCGPGRP, &got);
    CHECK(r == 0 && got == 4321, "TIOCGPGRP returns pgrp");

    /* TIOCGSID */
    p.sid = 999;
    pid_t sid = 0;
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCGSID, &sid);
    CHECK(r == 0 && sid == 999, "TIOCGSID returns sid");
}

static void test_ioctl_passthrough_and_guards(void) {
    struct alr_pty p = make_pty(3);

    /* An ioctl we don't emulate → PASS sentinel (caller forwards to kernel) */
    int dummy = 0;
    long r = alr_pts_emulate_ioctl(&p, 0, R_FIONREAD, &dummy);
    CHECK(r == ALR_PTS_IOCTL_PASS, "unknown ioctl → PASS");

    /* NULL arg on a struct ioctl → EINVAL, never a deref/crash */
    r = alr_pts_emulate_ioctl(&p, 1, R_TIOCGPTN, NULL);
    CHECK(r == -EINVAL, "TIOCGPTN NULL arg EINVAL");
    r = alr_pts_emulate_ioctl(&p, 0, R_TCGETS, NULL);
    CHECK(r == -EINVAL, "TCGETS NULL arg EINVAL");
    r = alr_pts_emulate_ioctl(&p, 0, R_TIOCSWINSZ, NULL);
    CHECK(r == -EINVAL, "TIOCSWINSZ NULL arg EINVAL");

    /* A free/NULL pty → PASS (the wrapper would forward to the real ioctl) */
    struct alr_pty freep;
    memset(&freep, 0, sizeof freep);   /* used=0 */
    r = alr_pts_emulate_ioctl(&freep, 0, R_TCGETS, &p.tio);
    CHECK(r == ALR_PTS_IOCTL_PASS, "free pty → PASS");
    r = alr_pts_emulate_ioctl(NULL, 0, R_TCGETS, &p.tio);
    CHECK(r == ALR_PTS_IOCTL_PASS, "NULL pty → PASS");
}

/* Integration: a real socketpair (the SAME backing the interposer mints for a
 * virtual PTY) is a working bidirectional byte channel, and the pure core's
 * ioctl state rides alongside it. This is the closest host-runnable proof that
 * "open ptmx → get master, slave is the peer, bytes flow both ways" holds —
 * the device adds only the open()/ioctl() interposition around this exact pair. */
static void test_socketpair_dataflow(void) {
    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("FAIL: socketpair: %s\n", strerror(errno));
        g_fail = 1;
        return;
    }
    /* Model the table entry the interposer would create for this pair. */
    struct alr_pty p;
    memset(&p, 0, sizeof p);
    p.used = 1; p.ptn = 9; p.master_fd = sv[0]; p.slave_fd = sv[1]; p.locked = 1;
    alr_pty_init_termios(&p.tio);
    alr_pty_init_winsize(&p.win);

    /* unlockpt(master) then ptn query — the terminal's grantpt/unlockpt/ptsname */
    int zero = 0;
    long r = alr_pts_emulate_ioctl(&p, 1, R_TIOCSPTLCK, &zero);
    CHECK(r == 0 && p.locked == 0, "dataflow: unlockpt");
    unsigned int n = 0;
    r = alr_pts_emulate_ioctl(&p, 1, R_TIOCGPTN, &n);
    CHECK(r == 0 && n == 9, "dataflow: ptn == 9");

    /* master → slave (what the terminal writes to the shell's stdin) */
    const char in[] = "echo hi\n";
    ssize_t w = write(sv[0], in, sizeof in - 1);
    CHECK(w == (ssize_t)(sizeof in - 1), "dataflow: master write");
    char buf[64]; memset(buf, 0, sizeof buf);
    ssize_t rd = read(sv[1], buf, sizeof buf);
    CHECK(rd == (ssize_t)(sizeof in - 1) && memcmp(buf, in, sizeof in - 1) == 0,
          "dataflow: slave reads what master wrote");

    /* slave → master (what the shell writes to its stdout, the terminal reads) */
    const char out[] = "hi\r\n$ ";
    w = write(sv[1], out, sizeof out - 1);
    CHECK(w == (ssize_t)(sizeof out - 1), "dataflow: slave write");
    memset(buf, 0, sizeof buf);
    rd = read(sv[0], buf, sizeof buf);
    CHECK(rd == (ssize_t)(sizeof out - 1) && memcmp(buf, out, sizeof out - 1) == 0,
          "dataflow: master reads what slave wrote");

    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    test_path_classification();
    test_slave_parse();
    test_termios_defaults();
    test_winsize_defaults();
    test_ioctl_ptmx_control();
    test_ioctl_winsize();
    test_ioctl_termios();
    test_ioctl_jobcontrol();
    test_ioctl_passthrough_and_guards();
    test_socketpair_dataflow();

    if (g_fail) { printf("SOME TESTS FAILED\n"); return 1; }
    printf("all tests passed\n");
    return 0;
}
