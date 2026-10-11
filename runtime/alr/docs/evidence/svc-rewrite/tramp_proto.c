/* Prototype cost of a rewritten svc: branch to a trampoline that saves the
 * caller-visible registers, finds this thread's stack by gettid, switches
 * to it, does the lane fast path's work for fstatat(dirfd, name)
 * (fstat of the dirfd, then the NOFOLLOW lookup), switches back. */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static long raw(long n, long a, long b, long c, long d) {
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d;
	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}
static struct { long tid; char *top; } tab[64];
static char stacks[64][16384] __attribute__((aligned(16)));
long work(long dirfd, long name, long buf, long flags) {
	struct stat d;
	raw(SYS_fstat, dirfd, (long)&d, 0, 0);
	return raw(SYS_newfstatat, dirfd, name, buf, flags | AT_SYMLINK_NOFOLLOW);
}
char *stack_for_thread(void) {
	long t = raw(SYS_gettid, 0, 0, 0, 0);
	unsigned h = (unsigned)t & 63;
	if (tab[h].tid != t) { tab[h].tid = t; tab[h].top = stacks[h] + sizeof stacks[h]; }
	return tab[h].top;
}
/* x0..x3 args; preserves x1..x30 like svc does. */
long tramp(long, long, long, long);
asm(".global tramp\n tramp:\n"
    " stp x29, x30, [sp, #-176]!\n stp x1, x2, [sp, #16]\n stp x3, x4, [sp, #32]\n stp x5, x6, [sp, #48]\n"
    " stp x7, x8, [sp, #64]\n stp x9, x10, [sp, #80]\n stp x11, x12, [sp, #96]\n stp x13, x14, [sp, #112]\n"
    " stp x15, x16, [sp, #128]\n stp x17, x18, [sp, #144]\n stp x19, x0, [sp, #160]\n"
    " bl stack_for_thread\n mov x19, sp\n mov sp, x0\n"
    " ldp x1, x2, [x19, #16]\n ldr x3, [x19, #32]\n ldr x0, [x19, #168]\n"
    " bl work\n mov sp, x19\n"
    " ldp x1, x2, [sp, #16]\n ldp x3, x4, [sp, #32]\n ldp x5, x6, [sp, #48]\n ldp x7, x8, [sp, #64]\n"
    " ldp x9, x10, [sp, #80]\n ldp x11, x12, [sp, #96]\n ldp x13, x14, [sp, #112]\n ldp x15, x16, [sp, #128]\n"
    " ldp x17, x18, [sp, #144]\n ldr x19, [sp, #160]\n ldp x29, x30, [sp], #176\n ret\n");
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
int main(void) {
	int dfd = open("/data/data/com.termux/files/usr/etc", O_RDONLY | O_DIRECTORY);
	struct stat st; int N = 200000; double t;
	for (int k = 0; k < 2; k++) {
	t = now(); for (int i = 0; i < N; i++) raw(SYS_newfstatat, dfd, (long)"bash.bashrc", (long)&st, 0); printf("native fstatat      %4.0f ns\n", (now()-t)/N);
	t = now(); for (int i = 0; i < N; i++) work(dfd, (long)"bash.bashrc", (long)&st, 0); printf("fast-path work only %4.0f ns\n", (now()-t)/N);
	t = now(); for (int i = 0; i < N; i++) tramp(dfd, (long)"bash.bashrc", (long)&st, 0); printf("trampoline + work   %4.0f ns\n", (now()-t)/N);
	}
	return st.st_size == 0;
}
