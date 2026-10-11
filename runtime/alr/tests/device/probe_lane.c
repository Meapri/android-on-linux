/* Lane semantics probe (ADR 0010).  Built static inside the guest, so alr
 * runs it in the tawcroot lane.  Each check prints "LANE <NAME> PASS" or
 * "LANE <NAME> FAIL detail", which acceptance.sh emits as they are.
 *
 * The fixture directory (argv[1], default /tmp/alrlp) holds `abs`, a
 * symlink whose target is the ABSOLUTE guest path /etc/hostname, made from
 * the host side so neither lane relativized it.  Followed by the kernel it
 * would name Android's /etc/hostname; in the lane it must name the
 * rootfs's.
 *
 * What it guards: the lane's fast paths (kernel lookups of one name
 * against a guest dirfd or the cwd, FOLLOW stats that look with NOFOLLOW
 * first, the leaf-symlink cache) and the lane's own fault handling (guest
 * memory read directly, a bad pointer still EFAULT, the guest's SIGSEGV
 * and SIGBUS handlers still the guest's). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void res(const char *name, int ok, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void res(const char *name, int ok, const char *fmt, ...)
{
	if (ok) { printf("%s PASS\n", name); return; }
	fails++;
	printf("%s FAIL ", name);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static ino_t ino_of(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? st.st_ino : 0;
}

static void slurp(int fd, char *buf, size_t cap)
{
	ssize_t n = fd >= 0 ? read(fd, buf, cap - 1) : -1;
	buf[n > 0 ? n : 0] = 0;
	if (fd >= 0) close(fd);
}

static sigjmp_buf jb;
static volatile int caught_sig, caught_code;
static void on_fault(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	caught_sig = sig;
	caught_code = si->si_code;
	siglongjmp(jb, 1);
}

static int install(int sig, int flags)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | flags;
	sigemptyset(&sa.sa_mask);
	return sigaction(sig, &sa, NULL);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/tmp/alrlp";
	char want[256], got[256], path[512];
	struct stat st, st2;

	setvbuf(stdout, NULL, _IOLBF, 0);
	int h = open("/etc/hostname", O_RDONLY);
	slurp(h, want, sizeof want);
	ino_t host_ino = ino_of("/etc/hostname");

	/* --- one name against a dirfd or the cwd -------------------------- */
	int dfd = open(dir, O_RDONLY | O_DIRECTORY);
	res("LANE DIRFD ABS SYMLINK STAT", dfd >= 0 && fstatat(dfd, "abs", &st, 0) == 0 &&
	    st.st_ino == host_ino, "ino %lu want %lu", (unsigned long)st.st_ino,
	    (unsigned long)host_ino);
	slurp(openat(dfd, "abs", O_RDONLY), got, sizeof got);
	res("LANE DIRFD ABS SYMLINK OPEN", strcmp(got, want) == 0, "read '%s'", got);
	res("LANE DIRFD NOFOLLOW SYMLINK", openat(dfd, "abs", O_RDONLY | O_NOFOLLOW) < 0 &&
	    errno == ELOOP, "errno %d", errno);
	res("LANE DIRFD LSTAT IS LINK", fstatat(dfd, "abs", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
	    S_ISLNK(st.st_mode), "mode %o", st.st_mode);

	if (chdir(dir) != 0) res("LANE CWD CHDIR", 0, "%s", strerror(errno));
	res("LANE CWD ABS SYMLINK STAT", stat("abs", &st) == 0 && st.st_ino == host_ino,
	    "ino %lu", (unsigned long)st.st_ino);
	slurp(open("abs", O_RDONLY), got, sizeof got);
	res("LANE CWD ABS SYMLINK OPEN", strcmp(got, want) == 0, "read '%s'", got);
	unlink("made");
	int cf = open("made", O_WRONLY | O_CREAT | O_EXCL, 0644);
	res("LANE CWD CREATE", cf >= 0 && close(cf) == 0 && stat("made", &st) == 0 &&
	    S_ISREG(st.st_mode), "%s", strerror(errno));
	snprintf(path, sizeof path, "%s/made", dir);
	res("LANE CWD CREATE WHERE", ino_of(path) == st.st_ino, "absolute ino differs");

	/* A vfork child's chdir must not move the parent's idea of its cwd. */
	pid_t vp = vfork();
	if (vp == 0) { if (chdir("/") != 0) _exit(2); _exit(0); }
	int ws;
	waitpid(vp, &ws, 0);
	res("LANE CWD AFTER VFORK CHDIR", stat("abs", &st) == 0 && st.st_ino == host_ino,
	    "%s", strerror(errno));

	/* At the root, a bind's name is the bind, not the rootfs's own dir. */
	struct stat proc_abs;
	stat("/proc", &proc_abs);
	if (chdir("/") != 0) res("LANE ROOT CHDIR", 0, "%s", strerror(errno));
	res("LANE ROOT CWD BIND NAME", lstat("proc", &st) == 0 &&
	    st.st_dev == proc_abs.st_dev && st.st_ino == proc_abs.st_ino,
	    "dev %lu want %lu", (unsigned long)st.st_dev, (unsigned long)proc_abs.st_dev);
	int rfd = open("/", O_RDONLY | O_DIRECTORY);
	res("LANE ROOT DIRFD BIND NAME", fstatat(rfd, "proc", &st, 0) == 0 &&
	    st.st_dev == proc_abs.st_dev, "dev %lu", (unsigned long)st.st_dev);
	res("LANE ROOT DIRFD DOTDOT", fstatat(rfd, "..", &st, 0) == 0 &&
	    st.st_ino == ino_of("/"), "ino %lu", (unsigned long)st.st_ino);
	int pfd = open("/proc/self", O_RDONLY | O_DIRECTORY);
	int prt = openat(pfd, "root", O_PATH | O_DIRECTORY);
	res("LANE PROC DIRFD ROOT LINK", prt >= 0 && fstat(prt, &st) == 0 &&
	    st.st_ino == ino_of("/"), "fd %d", prt);

	/* --- the lane's own fds ------------------------------------------ */
	int seen = 0;
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (atoi(e->d_name) >= 1000) seen++;
	if (d) closedir(d);
	res("LANE OWN FDS HIDDEN", d && seen == 0, "%d fd(s) >= 1000 listed", seen);
	int bad = 0;
	for (int fd = 1000; fd < 1010; fd++)
		if (fstat(fd, &st) == 0 || errno != EBADF) bad++;
	res("LANE OWN FDS EBADF", bad == 0, "%d of fds 1000-1009 answered fstat", bad);

	/* --- FOLLOW stat of a symlink another process retargets ----------- */
	snprintf(path, sizeof path, "%s/flip", dir);
	unlink(path);
	if (symlink("/etc/hostname", path) != 0)
		res("LANE FOLLOW STAT FIXTURE", 0, "%s", strerror(errno));
	ino_t a = ino_of(path);
	pid_t fp = fork();
	if (fp == 0) {
		char tmp[600];
		snprintf(tmp, sizeof tmp, "%s.new", path);
		unlink(tmp);
		if (symlink("/etc/os-release", tmp) != 0 || rename(tmp, path) != 0)
			_exit(1);
		_exit(0);
	}
	waitpid(fp, &ws, 0);
	ino_t b = ino_of(path);
	res("LANE FOLLOW STAT RETARGETED LINK", a == host_ino && b == ino_of("/etc/os-release"),
	    "before %lu after %lu", (unsigned long)a, (unsigned long)b);

	/* --- bad pointers ---------------------------------------------- */
	errno = 0;
	res("LANE EFAULT STAT PATH", stat((const char *)8, &st) == -1 && errno == EFAULT,
	    "errno %d", errno);
	errno = 0;
	res("LANE EFAULT STAT BUF", stat("/etc/hostname", (struct stat *)8) == -1 &&
	    errno == EFAULT, "errno %d", errno);
	errno = 0;
	res("LANE EFAULT OPEN PATH", open((const char *)8, O_RDONLY) == -1 && errno == EFAULT,
	    "errno %d", errno);
	errno = 0;
	res("LANE EFAULT DIRFD PATH", fstatat(dfd, (const char *)8, &st, 0) == -1 &&
	    errno == EFAULT, "errno %d", errno);
	errno = 0;
	snprintf(path, sizeof path, "%s/abs", dir);
	res("LANE EFAULT READLINK BUF", syscall(SYS_readlinkat, AT_FDCWD, path, (char *)8, 64) == -1 &&
	    errno == EFAULT, "errno %d", errno);
	char *unmapped = (char *)syscall(SYS_mmap, 0, 8192, 3, 0x22, -1, 0);
	memcpy(unmapped + 4096 - 4, "/etc", 4);    /* no NUL: runs off the page */
	syscall(SYS_munmap, unmapped + 4096, 4096);
	errno = 0;
	res("LANE EFAULT PATH CROSSES PAGE", stat(unmapped + 4096 - 4, &st) == -1 &&
	    errno == EFAULT, "errno %d", errno);

	/* --- the guest's own fault handlers -------------------------------- */
	install(SIGSEGV, 0);
	struct sigaction old;
	sigaction(SIGSEGV, NULL, &old);
	res("LANE SEGV HANDLER READBACK", old.sa_sigaction == on_fault &&
	    (old.sa_flags & SA_SIGINFO), "handler %p", (void *)old.sa_sigaction);
	caught_sig = 0;
	if (sigsetjmp(jb, 1) == 0) *(volatile int *)16 = 1;
	res("LANE SEGV HANDLER RUNS", caught_sig == SIGSEGV && caught_code > 0,
	    "sig %d code %d", caught_sig, caught_code);
	caught_sig = 0;
	if (sigsetjmp(jb, 1) == 0) raise(SIGSEGV);
	res("LANE SEGV SENT HANDLER RUNS", caught_sig == SIGSEGV && caught_code <= 0,
	    "sig %d code %d", caught_sig, caught_code);

	/* A vfork child resetting SIGSEGV (posix_spawn, Go's forkExec do)
	 * must not take the parent's handler away. */
	vp = vfork();
	if (vp == 0) { signal(SIGSEGV, SIG_DFL); _exit(0); }
	waitpid(vp, &ws, 0);
	caught_sig = 0;
	if (sigsetjmp(jb, 1) == 0) *(volatile int *)16 = 1;
	res("LANE SEGV HANDLER AFTER VFORK RESET", caught_sig == SIGSEGV, "sig %d", caught_sig);

	/* The same from a fork child whose first trapped call is its vfork
	 * child's: the vfork child must still be told apart from the memory's
	 * owner. */
	pid_t fc = fork();
	if (fc == 0) {
		pid_t v = vfork();
		if (v == 0) { signal(SIGSEGV, SIG_DFL); _exit(0); }
		int vs;
		waitpid(v, &vs, 0);
		caught_sig = 0;
		if (sigsetjmp(jb, 1) == 0) *(volatile int *)16 = 1;
		_exit(caught_sig == SIGSEGV ? 0 : 1);
	}
	waitpid(fc, &ws, 0);
	res("LANE SEGV HANDLER AFTER FORK THEN VFORK RESET",
	    WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "child status 0x%x", ws);

	install(SIGSEGV, SA_RESETHAND);
	caught_sig = 0;
	if (sigsetjmp(jb, 1) == 0) *(volatile int *)16 = 1;
	sigaction(SIGSEGV, NULL, &old);
	res("LANE SEGV RESETHAND", caught_sig == SIGSEGV && old.sa_handler == SIG_DFL,
	    "sig %d handler %p", caught_sig, (void *)old.sa_handler);

	install(SIGBUS, 0);
	caught_sig = 0;
	char tmpl[] = "/tmp/alrlp-busXXXXXX";
	int bf = mkstemp(tmpl);
	unlink(tmpl);
	char *m = bf >= 0 ? (char *)syscall(SYS_mmap, 0, 4096, 1, 1, bf, 0) : NULL;
	if (sigsetjmp(jb, 1) == 0 && m) (void)*(volatile char *)m;
	res("LANE BUS HANDLER RUNS", caught_sig == SIGBUS, "sig %d", caught_sig);

	signal(SIGSEGV, SIG_DFL);
	pid_t kp = fork();
	if (kp == 0) { *(volatile int *)16 = 1; _exit(0); }
	waitpid(kp, &ws, 0);
	res("LANE SEGV DEFAULT KILLS", WIFSIGNALED(ws) && WTERMSIG(ws) == SIGSEGV,
	    "status %#x", ws);
	signal(SIGSEGV, SIG_IGN);
	kp = fork();
	if (kp == 0) { *(volatile int *)16 = 1; _exit(0); }
	waitpid(kp, &ws, 0);
	res("LANE SEGV IGNORED STILL KILLS", WIFSIGNALED(ws) && WTERMSIG(ws) == SIGSEGV,
	    "status %#x", ws);
	kp = fork();
	if (kp == 0) { raise(SIGSEGV); _exit(7); }
	waitpid(kp, &ws, 0);
	res("LANE SEGV IGNORED SENT", WIFEXITED(ws) && WEXITSTATUS(ws) == 7, "status %#x", ws);

	return fails ? 1 : 0;
}
