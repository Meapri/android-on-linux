/* posix_spawn / popen probe, compiled against the GUEST's glibc.
 *
 * These two have no CLI consumer we can lean on, and this project has been
 * burned repeatedly by verifying one libc entry point through a tool that
 * actually calls a different one (mktemp for mkstemp, touch -d for utimes,
 * mv for rename, a shell for posix_spawn).  So bind the real symbols.
 *
 * Prints PSOK and POPENOK on success; anything else means the child was exec'd
 * without the loader and the guest's PT_INTERP was resolved against Android's
 * root -- which is what "CANNOT LINK EXECUTABLE" looks like from here.
 */
#define _GNU_SOURCE
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

int main(void)
{
    char *av[] = { (char *)"echo", (char *)"PSOK", NULL };
    char buf[128];
    pid_t pid;
    int st = 0, rc;
    FILE *f;

    rc = posix_spawn(&pid, "/bin/echo", NULL, NULL, av, environ);
    if (rc != 0) { printf("posix_spawn failed: %s\n", strerror(rc)); return 1; }
    if (waitpid(pid, &st, 0) < 0) return 2;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("child st=%d\n", st); return 3; }

    f = popen("echo POPENOK", "r");
    if (!f) { printf("popen NULL\n"); return 4; }
    buf[0] = '\0';
    if (!fgets(buf, sizeof buf, f)) { pclose(f); printf("popen empty\n"); return 5; }
    if (pclose(f) != 0) { printf("pclose nonzero\n"); return 6; }
    fputs(buf, stdout);
    return 0;
}
