/* Is SysV IPC reachable inside the guest?
 *
 * RISKS R9 and docs/02-architecture.md §121 both carried this as
 * PENDING_DEVICE -- "Termux might not be blocked by Android's seccomp, alr
 * doctor P2 will answer".  It is blocked: all three return ENOSYS, and the
 * supervisor log shows three SIGSYS traps for them, so they hit the zygote
 * filter's RET_TRAP and our default -ENOSYS emulation answered.
 *
 * ENOSYS rather than EPERM is the useful detail: it is the standard signal
 * that makes libraries take their fallback path instead of failing outright.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>

int main(void)
{
    int id;

    errno = 0;
    id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    printf("shmget : %s\n", id >= 0 ? "ok" : strerror(errno));
    if (id >= 0) shmctl(id, IPC_RMID, NULL);

    errno = 0;
    id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    printf("semget : %s\n", id >= 0 ? "ok" : strerror(errno));
    if (id >= 0) semctl(id, 0, IPC_RMID);

    errno = 0;
    id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    printf("msgget : %s\n", id >= 0 ? "ok" : strerror(errno));
    if (id >= 0) msgctl(id, IPC_RMID, NULL);

    return 0;
}
