#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *root_prefix = "";
static const char *self_path = "/tmp/tawcroot-perf/tawcroot-perf";
static long iterations = 10000;
static int nfiles = 256;
static int child_mode = 0;

static void die(const char *msg)
{
	fprintf(stderr, "tawcroot-perf: %s: %s\n", msg, strerror(errno));
	exit(111);
}

static int streq(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

static void join_path(char *out, size_t cap, const char *a, const char *b)
{
	size_t an = strlen(a);
	size_t bn = strlen(b);
	if (an + bn + 2 > cap) {
		fprintf(stderr, "tawcroot-perf: path too long: %s%s\n", a, b);
		exit(112);
	}
	memcpy(out, a, an);
	memcpy(out + an, b, bn + 1);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) die("clock_gettime");
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void result(const char *name, uint64_t elapsed, long ops)
{
	double per = ops > 0 ? (double)elapsed / (double)ops : 0.0;
	printf("RESULT\t%s\t%llu\t%ld\t%.2f\n",
	       name, (unsigned long long)elapsed, ops, per);
}

static void mkdir_p(const char *path)
{
	char tmp[4096];
	size_t n = strlen(path);
	if (n >= sizeof tmp) {
		fprintf(stderr, "tawcroot-perf: mkdir path too long: %s\n", path);
		exit(112);
	}
	memcpy(tmp, path, n + 1);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = 0;
		if (mkdir(tmp, 0777) != 0 && errno != EEXIST) die("mkdir");
		*p = '/';
	}
	if (mkdir(tmp, 0777) != 0 && errno != EEXIST) die("mkdir");
}

static void chmod_best_effort(const char *path, mode_t mode, const char *what)
{
	if (chmod(path, mode) == 0 || errno == EPERM) return;
	die(what);
}

static void file_path(char *out, size_t cap, int i)
{
	char rel[128];
	snprintf(rel, sizeof rel, "/tmp/tawcroot-perf/work/file-%04d.dat", i);
	join_path(out, cap, root_prefix, rel);
}

static void setup_files(void)
{
	char dir[4096];
	join_path(dir, sizeof dir, root_prefix, "/tmp/tawcroot-perf/work");
	mkdir_p(dir);
	chmod_best_effort(dir, 0777, "chmod work dir");

	for (int i = 0; i < nfiles; i++) {
		char path[4096];
		file_path(path, sizeof path, i);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
		if (fd < 0) die("open setup file");
		char buf[64];
		int n = snprintf(buf, sizeof buf, "file %d\n", i);
		if (write(fd, buf, (size_t)n) != n) die("write setup file");
		if (close(fd) != 0) die("close setup file");
	}
}

static void bench_getpid(void)
{
	uint64_t start = now_ns();
	volatile pid_t sink = 0;
	for (long i = 0; i < iterations; i++) sink ^= getpid();
	uint64_t end = now_ns();
	if (sink == (pid_t)-1) fprintf(stderr, "sink=%d\n", sink);
	result("getpid", end - start, iterations);
}

static void bench_stat(void)
{
	struct stat st;
	uint64_t start = now_ns();
	for (long i = 0; i < iterations; i++) {
		char path[4096];
		file_path(path, sizeof path, (int)(i % nfiles));
		if (stat(path, &st) != 0) die("stat");
	}
	uint64_t end = now_ns();
	result("stat", end - start, iterations);
}

static void bench_open_close(void)
{
	char byte;
	uint64_t start = now_ns();
	for (long i = 0; i < iterations; i++) {
		char path[4096];
		file_path(path, sizeof path, (int)(i % nfiles));
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) die("open");
		if (read(fd, &byte, 1) < 0) die("read");
		if (close(fd) != 0) die("close");
	}
	uint64_t end = now_ns();
	result("open_read_close", end - start, iterations);
}

static void bench_readdir(void)
{
	char dirpath[4096];
	join_path(dirpath, sizeof dirpath, root_prefix, "/tmp/tawcroot-perf/work");

	long seen = 0;
	uint64_t start = now_ns();
	for (long i = 0; i < iterations / 32 + 1; i++) {
		DIR *d = opendir(dirpath);
		if (!d) die("opendir");
		for (;;) {
			errno = 0;
			struct dirent *de = readdir(d);
			if (!de) {
				if (errno) die("readdir");
				break;
			}
			if (de->d_name[0] != '.') seen++;
		}
		if (closedir(d) != 0) die("closedir");
	}
	uint64_t end = now_ns();
	result("readdir_entries", end - start, seen);
}

static void bench_create_unlink(void)
{
	char dir[4096];
	join_path(dir, sizeof dir, root_prefix, "/tmp/tawcroot-perf/create");
	mkdir_p(dir);
	chmod_best_effort(dir, 0777, "chmod create dir");

	uint64_t start = now_ns();
	for (long i = 0; i < iterations / 8 + 1; i++) {
		char path[4096];
		char rel[160];
		snprintf(rel, sizeof rel, "/tmp/tawcroot-perf/create/tmp-%ld", i);
		join_path(path, sizeof path, root_prefix, rel);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
		if (fd < 0) die("create");
		if (write(fd, "x", 1) != 1) die("write create");
		if (close(fd) != 0) die("close create");
		if (unlink(path) != 0) die("unlink");
	}
	uint64_t end = now_ns();
	result("create_write_unlink", end - start, iterations / 8 + 1);
}

static void bench_fork_exec(void)
{
	long n = iterations / 200;
	if (n < 1) n = 1;
	if (n > 200) n = 200;

	uint64_t start = now_ns();
	for (long i = 0; i < n; i++) {
		pid_t pid = fork();
		if (pid < 0) die("fork");
		if (pid == 0) {
			char *const argv[] = { (char *)self_path, (char *)"--child", NULL };
			execv(self_path, argv);
			_exit(127);
		}
		int status = 0;
		if (waitpid(pid, &status, 0) < 0) die("waitpid");
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "tawcroot-perf: child failed: status=%d\n", status);
			exit(113);
		}
	}
	uint64_t end = now_ns();
	result("fork_exec_wait", end - start, n);
}

static void usage(FILE *f)
{
	fprintf(f,
	        "usage: tawcroot-perf [--root-prefix PATH] [--self PATH]\n"
	        "                     [--iterations N] [--files N]\n"
	        "                     [--setup] [--child]\n");
}

int main(int argc, char **argv)
{
	int do_setup = 0;
	for (int i = 1; i < argc; i++) {
		if (streq(argv[i], "--root-prefix")) {
			if (++i >= argc) { usage(stderr); return 2; }
			root_prefix = argv[i];
		} else if (streq(argv[i], "--self")) {
			if (++i >= argc) { usage(stderr); return 2; }
			self_path = argv[i];
		} else if (streq(argv[i], "--iterations")) {
			if (++i >= argc) { usage(stderr); return 2; }
			iterations = strtol(argv[i], NULL, 10);
			if (iterations < 1) iterations = 1;
		} else if (streq(argv[i], "--files")) {
			if (++i >= argc) { usage(stderr); return 2; }
			nfiles = (int)strtol(argv[i], NULL, 10);
			if (nfiles < 1) nfiles = 1;
		} else if (streq(argv[i], "--setup")) {
			do_setup = 1;
		} else if (streq(argv[i], "--child")) {
			child_mode = 1;
		} else if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
			usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "tawcroot-perf: unknown arg: %s\n", argv[i]);
			usage(stderr);
			return 2;
		}
	}

	if (child_mode) return 0;
	if (do_setup) setup_files();

	bench_getpid();
	bench_stat();
	bench_open_close();
	bench_readdir();
	bench_create_unlink();
	bench_fork_exec();
	return 0;
}
