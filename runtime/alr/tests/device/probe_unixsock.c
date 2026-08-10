/* AF_UNIX socket addresses carry a path, and it must be rewritten -- but only
 * when it IS a path.
 *
 * Until 2026-08-03 bind() and connect() were not interposed at all, so a guest
 * binding "/tmp/x.sock" bound the ANDROID path.  tmux was the visible victim:
 * it failed to create its socket even with the directory present and confirmed
 * visible to the guest, because the directory was rewritten and the address
 * was not.
 *
 * Two cases, and the second is the one that keeps the fix honest:
 *
 *   pathname  bind("/tmp/alr-unix-probe.sock") must land under the rootfs, so
 *             a connect() to the same guest path finds it.
 *   abstract  sun_path[0] == '\0' is the Linux abstract namespace and has NO
 *             filesystem presence.  Prefixing it would invent a different
 *             address -- two processes that agreed on "\0alr-probe" would stop
 *             finding each other.  It must pass through untouched.
 *
 * Prints one line: "unix-path-ok abstract-ok" when both hold.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int bind_and_connect(const char *name, int abstract)
{
    struct sockaddr_un a;
    socklen_t len;
    int s, c, ok = 0;

    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (abstract) {
        a.sun_path[0] = '\0';
        strncpy(a.sun_path + 1, name, sizeof a.sun_path - 2);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    } else {
        strncpy(a.sun_path, name, sizeof a.sun_path - 1);
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(name) + 1);
        unlink(name);
    }

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return 0;
    if (bind(s, (struct sockaddr *)&a, len) != 0) { close(s); return 0; }
    if (listen(s, 1) != 0) { close(s); return 0; }

    /* Connect AND ACCEPT.  connect() alone completes against the listen
     * backlog without the server ever accepting, so an earlier version of this
     * probe passed while accept(2) was completely broken -- syscall 202 is
     * blocked by the zygote filter and was emulated as ENOSYS, which is what
     * killed the tmux server.  The check has to reach the thing that failed. */
    if ((c = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0) {
        if (connect(c, (struct sockaddr *)&a, len) == 0) {
            int as = accept(s, NULL, NULL);
            if (as >= 0) { ok = 1; close(as); }
        }
        close(c);
    }
    close(s);
    if (!abstract) unlink(name);
    return ok;
}

int main(void)
{
    int p = bind_and_connect("/tmp/alr-unix-probe.sock", 0);
    int b = bind_and_connect("alr-abstract-probe", 1);
    printf("%s %s\n", p ? "unix-path-ok" : "unix-path-FAILED",
                      b ? "abstract-ok"  : "abstract-FAILED");
    return (p && b) ? 0 : 1;
}
