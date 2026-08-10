/* link() -> stat() -> fstat() in ONE process, which is the only case the link
 * identity table covers (docs/04-preload-spec.md §8.2: it is deliberately
 * per-process, because git does the link and the check in one process).
 *
 * A shell test cannot exercise this: `ln` and the check land in different
 * processes and the table is empty by the time anything looks.
 *
 * §6.2 states the failure mode this guards: without fstat/fstat64 interposed,
 * stat(path) reports st_nlink=2 for a copy-fallback "hardlink" while fstat(fd)
 * on the SAME file reports 1, and dpkg's integrity check breaks on the
 * disagreement.  Prints NLINKOK only when both views agree.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    const char *a = "/tmp/alrnl_a", *b = "/tmp/alrnl_b";
    struct stat sa, sb, sf;
    int fd;

    unlink(a); unlink(b);
    if ((fd = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0644)) < 0) return 1;
    if (write(fd, "x\n", 2) != 2) return 2;
    close(fd);

    if (link(a, b) != 0) { printf("link failed\n"); return 3; }
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 4;
    if ((fd = open(b, O_RDONLY)) < 0) return 5;
    if (fstat(fd, &sf) != 0) return 6;
    close(fd);

    printf("stat(a)  nlink=%lu ino=%llu\n",
           (unsigned long)sa.st_nlink, (unsigned long long)sa.st_ino);
    printf("stat(b)  nlink=%lu ino=%llu\n",
           (unsigned long)sb.st_nlink, (unsigned long long)sb.st_ino);
    printf("fstat(b) nlink=%lu ino=%llu\n",
           (unsigned long)sf.st_nlink, (unsigned long long)sf.st_ino);

    if (sb.st_nlink == sf.st_nlink && sb.st_ino == sf.st_ino &&
        sa.st_ino == sb.st_ino && sb.st_nlink >= 2)
        printf("NLINKOK\n");
    else
        printf("MISMATCH: stat and fstat disagree, or the link identity was "
               "not recorded\n");
    unlink(a); unlink(b);
    return 0;
}
