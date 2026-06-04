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
#include <stdlib.h>       /* setenv/getenv/unsetenv — cross-fork env handoff test */
#include <string.h>
#include <errno.h>
#include <fcntl.h>        /* fcntl(F_DUPFD) — mirrors the interposer's slave dup */
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

/* The pure env-name formatter the cross-fork handoff is built on. The minter
 * sets ALR_PTY_SLAVE_<ptn>=<slave_fd>; the forked child (empty COW pty table)
 * reads the same name to resolve open("/dev/pts/N"). Both sides format the name
 * with alr_pts_slave_env_name, so it must be stable, exact, and bounds-safe. */
static void test_slave_env_name(void) {
    char b[32];
    size_t n = alr_pts_slave_env_name(0, b, sizeof b);
    CHECK(n == 15 && strcmp(b, "ALR_PTY_SLAVE_0") == 0, "env name ptn=0");
    n = alr_pts_slave_env_name(7, b, sizeof b);
    CHECK(n == 15 && strcmp(b, "ALR_PTY_SLAVE_7") == 0, "env name ptn=7");
    n = alr_pts_slave_env_name(42, b, sizeof b);
    CHECK(n == 16 && strcmp(b, "ALR_PTY_SLAVE_42") == 0, "env name ptn=42");
    n = alr_pts_slave_env_name(123, b, sizeof b);
    CHECK(n == 17 && strcmp(b, "ALR_PTY_SLAVE_123") == 0, "env name ptn=123");

    /* bad args / overflow → 0, buffer not relied upon */
    CHECK(alr_pts_slave_env_name(-1, b, sizeof b) == 0, "negative ptn → 0");
    CHECK(alr_pts_slave_env_name(0, NULL, sizeof b) == 0, "NULL buf → 0");
    /* "ALR_PTY_SLAVE_0" needs 15 + NUL = 16; a 15-byte buffer must refuse */
    char small[15];
    CHECK(alr_pts_slave_env_name(0, small, sizeof small) == 0, "too-small buf → 0");
    /* exact-fit 16-byte buffer for ptn=0 succeeds and NUL-terminates */
    char exact[16];
    memset(exact, 0x55, sizeof exact);
    CHECK(alr_pts_slave_env_name(0, exact, sizeof exact) == 15 &&
          exact[15] == '\0' && strcmp(exact, "ALR_PTY_SLAVE_0") == 0,
          "exact-fit buf ok");
}

/* Cross-fork inherited-slave resolution, host-simulated end to end.
 *
 * This reproduces, with no Android and no interposer .so, the exact sequence the
 * device path runs in foot's forked child: the table lookup MISSES (empty COW
 * copy), so the child rebuilds the env-var name, reads the inherited slave fd
 * number the parent published, dup()s it, and that dup is a live slave channel
 * back to the master. We model the "empty table" as a plain struct alr_pty with
 * used=0 (so alr_pts_by_fd-style logic would miss) and prove the env round-trip
 * + dup yields a working byte path — the precise behavior of
 * alr_pts_adopt_inherited_slave() minus the (untestable here) g_pts[] write. */
static void test_cross_fork_inherited_slave(void) {
    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("FAIL: socketpair: %s\n", strerror(errno));
        g_fail = 1;
        return;
    }
    const int ptn = 3;
    int master = sv[0], slave = sv[1];

    /* PARENT (minter): publish the slave fd under ALR_PTY_SLAVE_<ptn>, exactly as
     * alr_pts_publish_slave_env() does. */
    char name[32];
    size_t nn = alr_pts_slave_env_name(ptn, name, sizeof name);
    CHECK(nn > 0, "xfork: env name formatted");
    char val[24];
    snprintf(val, sizeof val, "%d", slave);
    CHECK(setenv(name, val, 1) == 0, "xfork: publish slave env");

    /* CHILD (forked): the table lookup misses (modeled: a fresh/empty entry).
     * Rebuild the SAME name, read the inherited fd number, dup it. */
    struct alr_pty empty;
    memset(&empty, 0, sizeof empty);            /* used==0 → would-miss table */
    CHECK(empty.used == 0, "xfork: child table is empty");

    char name2[32];
    CHECK(alr_pts_slave_env_name(ptn, name2, sizeof name2) == nn &&
          strcmp(name, name2) == 0, "xfork: child rebuilds identical env name");
    const char *got = getenv(name2);
    CHECK(got != NULL && got[0], "xfork: child reads published env");
    int inh = -1;
    if (got) {
        inh = 0; int ok = 1;
        for (const char *q = got; *q; ++q) {
            if (*q < '0' || *q > '9') { ok = 0; break; }
            inh = inh * 10 + (*q - '0');
        }
        CHECK(ok && inh == slave, "xfork: parsed inherited fd matches slave");
    }
    int dfd = fcntl(inh, F_DUPFD, 0);            /* the interposer's dup of the slave */
    CHECK(dfd >= 0 && dfd != slave, "xfork: dup of inherited slave");

    /* The dup is a working slave: master→dup and dup→master both carry bytes —
     * i.e. open("/dev/pts/3") in the child resolved to a live terminal channel. */
    const char *prompt = "$ ";
    ssize_t w = write(master, prompt, 2);
    CHECK(w == 2, "xfork: master writes to child slave");
    char buf[8]; memset(buf, 0, sizeof buf);
    ssize_t rd = read(dfd, buf, sizeof buf);
    CHECK(rd == 2 && memcmp(buf, prompt, 2) == 0, "xfork: dup slave reads master bytes");

    const char *cmd = "ls\n";
    w = write(dfd, cmd, 3);
    CHECK(w == 3, "xfork: child slave writes to master");
    memset(buf, 0, sizeof buf);
    rd = read(master, buf, sizeof buf);
    CHECK(rd == 3 && memcmp(buf, cmd, 3) == 0, "xfork: master reads child slave bytes");

    /* A bad/never-published index yields no env → the resolver would fall back to
     * ENXIO (here: getenv miss). Proves we don't mis-resolve unrelated slaves. */
    char miss[32];
    alr_pts_slave_env_name(58, miss, sizeof miss);
    CHECK(getenv(miss) == NULL, "xfork: unpublished ptn has no env (→ ENXIO)");

    unsetenv(name);
    if (dfd >= 0) close(dfd);
    close(master);
    close(slave);
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
    test_slave_env_name();
    test_cross_fork_inherited_slave();

    if (g_fail) { printf("SOME TESTS FAILED\n"); return 1; }
    printf("all tests passed\n");
    return 0;
}
