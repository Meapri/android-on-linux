/* Path-taking libc calls that nobody thought to interpose.
 *
 * bind()/connect() were missed for the whole life of the project and took out
 * every guest server (docs/01 §A6).  That was not a one-off: it was a class.
 * scripts/check-path-coverage.sh now diffs the preload's exports against an
 * inventory of path-taking libc entry points, and this probe is the device
 * half -- the gate proves a symbol is EXPORTED, only the device proves the
 * rewrite actually lands.
 *
 * Each check names a real guest workflow, because "an obscure API is wrong"
 * and "cp -a loses every extended attribute" are the same bug:
 *
 *   xattr        ls -l printed "drwx------?" -- coreutils prints '?' when the
 *                ACL probe ERRORS, and getxattr on an unrewritten path is a
 *                bare ENOENT.  Same call underneath cp -a, tar --xattrs,
 *                rsync -X, getfacl.
 *   inotify      every file watcher: node's fs.watch/chokidar (so every JS dev
 *                server), inotifywait, entr, watchman.  Watching the wrong
 *                path does not error -- it silently never fires.
 *   pathconf     configure scripts, and glibc's own readdir sizing.
 *   getsockname  the MIRROR of the bind fix.  Having rewritten sun_path on the
 *                way in, the kernel hands it back on the way out, so a guest
 *                that asks where it is bound gets the ANDROID path -- exactly
 *                the leak getcwd() already corrects for cwd.
 *
 * And one that must NOT be rewritten:
 *
 *   glob         glob() is built on opendir/lstat, which we already rewrite,
 *                and it composes its results from the caller's pattern.  So it
 *                works untouched AND returns guest paths.  Interposing it
 *                would return <R>-prefixed paths and break every shell glob.
 *                Asserted here so a later "completeness" pass does not add it.
 *
 * Prints one "name=ok" / "name=FAILED" per check; exits non-zero if any failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <ftw.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <mntent.h>
#include <stdlib.h>
#include <sys/xattr.h>
#include <unistd.h>

/* EVERY check must use a path that does NOT exist on Android, or it proves
 * nothing.  The first version of this probe watched "/etc" and pathconf'd
 * "/etc" -- both of which Android HAS (a symlink to /system/etc) -- so both
 * reported ok while resolving the wrong directory entirely.  That is the
 * second probe in two days to pass while measuring the host by accident; the
 * first stopped at connect() and missed accept(2) being blocked. */
#define GUEST_FILE  "/etc/os-release"
/* A DIRECTORY that Android does not have.  /etc exists on Android (a symlink
 * to /system/etc), which is the trap the first version of this probe fell in;
 * /usr does not exist there at all. */
#define GUEST_DIR   "/usr/share/base-files"
#define GUEST_FSTAB "/etc/fstab"

static int check_xattr(void)
{
    char buf[4096];
    ssize_t n = listxattr(GUEST_FILE, buf, sizeof buf);
    /* An empty attribute list is a fine answer -- ubuntu-base ships no xattrs
     * (docs/01 §A6: zero SCHILY.xattr.* pax headers).  ENOENT is the failure:
     * it means the path never resolved. */
    return n >= 0 || errno == ENOTSUP || errno == EOPNOTSUPP;
}

static int check_inotify(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC), w;
    if (fd < 0) return 0;
    w = inotify_add_watch(fd, GUEST_FILE, IN_MODIFY);
    if (w >= 0) inotify_rm_watch(fd, w);
    close(fd);
    return w >= 0;
}

static int check_pathconf(void)
{
    errno = 0;
    /* -1 with errno untouched means "no limit", which is a valid answer. */
    return pathconf(GUEST_FILE, _PC_NAME_MAX) >= 0 || errno == 0;
}

static int check_getsockname(void)
{
    struct sockaddr_un a, got;
    socklen_t len, glen = sizeof got;
    int s, ok;
    const char *p = "/tmp/alr-pathcov.sock";

    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, p, sizeof a.sun_path - 1);
    len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(p) + 1);
    unlink(p);
    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return 0;
    if (bind(s, (struct sockaddr *)&a, len) != 0) { close(s); return 0; }
    memset(&got, 0, sizeof got);
    ok = getsockname(s, (struct sockaddr *)&got, &glen) == 0
         && !strcmp(got.sun_path, p);
    if (!ok) fprintf(stderr, "  getsockname returned \"%s\", wanted \"%s\"\n",
                     got.sun_path, p);
    close(s);
    unlink(p);
    return ok;
}

/* glibc's ftw/nftw walk with their own internal opendir/lstat, exactly like
 * glob.  Note that coreutils' du/rm -r/chmod -R do NOT: they carry gnulib's
 * fts, which goes through the PLT -- MEASURED, `du -sh /etc` reports the
 * guest's 2.2M and a mkdir/chmod -R/cp -r/rm -rf round trip is clean.  So this
 * check is about glibc's own walkers, which nothing in coreutils uses. */
static int nftw_seen, nftw_bad, nftw_base_bad;

/* Not just "did it walk" -- WHAT PATH DID THE CALLBACK GET.  Rewriting the
 * root without correcting the callback argument would make nftw succeed while
 * handing the guest a stream of /data/data/... paths, and a check that only
 * counted entries would call that a pass.  Same for FTW.base, which is a byte
 * offset INTO fpath and has to move with it. */
static int nftw_cb(const char *f, const struct stat *s, int tf, struct FTW *w)
{
    (void)s; (void)tf;
    nftw_seen++;
    if (strncmp(f, GUEST_DIR, sizeof GUEST_DIR - 1) != 0) {
        if (!nftw_bad) fprintf(stderr, "  nftw callback got \"%s\", wanted a "
                                       GUEST_DIR " path\n", f);
        nftw_bad++;
    }
    if (w->base < 0 || (size_t)w->base > strlen(f) ||
        (w->base > 0 && f[w->base - 1] != '/')) {
        if (!nftw_base_bad)
            fprintf(stderr, "  nftw FTW.base=%d does not point at the filename "
                            "in \"%s\"\n", w->base, f);
        nftw_base_bad++;
    }
    return 0;
}

static int check_nftw(void)
{
    nftw_seen = nftw_bad = nftw_base_bad = 0;
    if (nftw(GUEST_DIR, nftw_cb, 8, FTW_PHYS) != 0) return 0;
    /* A directory, so this recurses and the base offset is exercised on
     * something other than the root itself. */
    return nftw_seen > 1 && !nftw_bad && !nftw_base_bad;
}

/* setmntent() opens its file with an internal fopen -- an alias, so our fopen
 * never sees it -- which is why the preload takes setmntent itself over.  This
 * is a regression guard, not a new finding: it was already correct.  df, mount
 * and every "is this mounted?" check go through it. */
static int check_setmntent(void)
{
    FILE *f = setmntent(GUEST_FSTAB, "r");
    if (!f) return 0;
    endmntent(f);
    return 1;
}

/* The negative control.  If this ever fails, something started rewriting glob
 * results and every `ls *.c` in the guest now prints host paths. */
static int check_glob_untouched(void)
{
    glob_t g;
    int ok = 0;
    int rc = glob("/etc/os-relea*", 0, NULL, &g);
    if (rc == 0) {
        ok = g.gl_pathc > 0 && !strncmp(g.gl_pathv[0], "/etc/", 5);
        if (!ok) fprintf(stderr, "  glob returned \"%s\", wanted a /etc/ path\n",
                         g.gl_pathc ? g.gl_pathv[0] : "(nothing)");
        globfree(&g);
    } else {
        fprintf(stderr, "  glob(\"/etc/os-relea*\") = %d (%s)\n", rc,
                rc == GLOB_NOMATCH ? "GLOB_NOMATCH -- it searched ANDROID's /etc"
                                   : "error");
    }
    return ok;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } c[] = {
        { "xattr",       check_xattr },
        { "inotify",     check_inotify },
        { "pathconf",    check_pathconf },
        { "getsockname", check_getsockname },
        { "nftw",        check_nftw },
        { "setmntent",   check_setmntent },
        { "glob-guest",  check_glob_untouched },
        { NULL, NULL }
    };
    int i, bad = 0;
    for (i = 0; c[i].name; i++) {
        int ok = c[i].fn();
        if (!ok) bad++;
        printf("%s=%s%s", c[i].name, ok ? "ok" : "FAILED", c[i + 1].name ? " " : "\n");
    }
    return bad ? 1 : 0;
}
