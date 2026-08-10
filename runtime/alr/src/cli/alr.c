/* alr — Termux-native Ubuntu ARM64 glibc runtime.
 *
 *   alr install <distro>      provision a rootfs
 *   alr run <cmd> [args...]   run one guest command
 *   alr shell                 interactive guest shell
 *   alr doctor                (separate binary for now: src/cli/doctor.c)
 *
 * The load-bearing part is launch(): a stock Ubuntu binary CANNOT be execve'd
 * directly, because the kernel resolves its PT_INTERP ("/lib/ld-linux-aarch64
 * .so.1") against the real host root before userspace runs.  We therefore
 * invoke the GUEST loader explicitly and hand it the program.  (ADR 0002)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "alr_path_rule.h"
#include "alr_version.h"

#include <dirent.h>
#include "alr_exec_rule.h"
#include "alr_elf.h"
#include "alr_supervisor.h"
#include "alr_resolv_proto.h"

const char *alr_resolvd_start(const char *dir);
void alr_resolvd_stop(void);

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ALR_MAX_ARGV 4096
#define ALR_MAX_ENVP 512

static int g_log;
static const char *g_resolv_sock;
static const char *g_with;
static int g_force;
/* Our own path, captured at startup.  A subshell's /proc/self/exe is the
 * SHELL's, not ours -- reading it lazily from inside sh -c silently invoked
 * /bin/sh with our arguments and reported "Illegal option -d". */
static char g_self[ALR_PBUF];
/* Guest view of the cwd we chdir'd to; handed to build_env as $PWD. */
static char g_guest_cwd[ALR_PBUF];

static void die(const char *reason, const char *detail)
{
    fprintf(stderr, "alr: %s\n  reason=%s\n", detail ? detail : reason, reason);
    exit(125);
}

/* ── layout ──────────────────────────────────────────────────────────── */

static const char *prefix(void)
{
    const char *p = getenv("PREFIX");
    return (p && *p) ? p : "/data/data/com.termux/files/usr";
}

/* A distro name becomes a PATH COMPONENT and is interpolated into a shell
 * command (guest_run builds `... -d '%s' run ...`).  Nothing validated it.
 * `alr remove` makes that dangerous rather than merely untidy: the name is
 * joined onto $ALR_ROOT_DIR and the result is deleted recursively, so ".." or
 * an absolute path would delete something else entirely.
 *
 * Deliberately strict -- [A-Za-z0-9._-], no leading dot, no empty.  Every
 * distro this project ships or plans (ubuntu-24.04, ubuntu-26.04, debian-*)
 * fits, and a name that does not is far more likely to be a mistake than a
 * requirement. */
static int distro_name_ok(const char *d)
{
    size_t i;
    if (!d || !*d || d[0] == '.') return 0;
    for (i = 0; d[i]; i++) {
        char c = d[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return i <= 64;
}


/* ── configuration file (docs/06-cli-spec.md §2) ─────────────────────────
 *
 * $PREFIX/etc/alr/config.toml (global), then $HOME/.alr/config.toml (user),
 * with the user file winning.  Everything here sits BELOW the environment and
 * below flags: flag > env > user config > global config > built-in.
 *
 * THE SCHEMA IS SMALLER THAN §2 SKETCHED, and deliberately.  Two of the seven
 * keys were checked against what the code actually does and dropped:
 *
 *   [env] passthrough  is already true of everything.  alr copies the whole
 *                      host environ into the guest minus a blocklist, so an
 *                      allow-list of names to forward would only be able to
 *                      RESTRICT what already passes -- the opposite of what
 *                      the key means.  A key that cannot change any outcome
 *                      is worse than no key: it reads as a control.
 *   runtime.supervisor whose own documentation is "false 로 두지 말 것 --
 *                      부팅이 안 된다".  A persistent setting whose only
 *                      non-default value breaks every subsequent run is a
 *                      footgun; `--no-supervisor` already exists per-run and
 *                      announces itself with reason=no-supervisor-requested.
 *
 * The parser handles exactly this schema -- string, bool, integer, table
 * headers, # comments.  It is not a TOML library and does not pretend to be:
 * an unknown key is reported, not silently ignored, because a typo'd setting
 * that appears to be accepted is the failure mode config files are famous for.
 */
struct alr_config {
    char distro[128];      int has_distro;
    int  fakeroot;         int has_fakeroot;
    int  log;              int has_log;
    char root[ALR_PBUF];   int has_root;
    char mirror[512];      int has_mirror;
    int  bad;              /* unknown keys seen while loading */
};
static struct alr_config g_cfg;
static int g_cfg_loaded;

static void cfg_path(char *out, size_t n, int user)
{
    const char *h = getenv("HOME");
    if (user) snprintf(out, n, "%s/.alr/config.toml", (h && *h) ? h : ".");
    else      snprintf(out, n, "%s/etc/alr/config.toml", prefix());
}

/* "$PREFIX/var/lib/alr" -> the expansion.  Only a LEADING $PREFIX, which is
 * the only form §2 shows and the only one worth supporting: a general variable
 * expander in a config path is a way to read the environment twice. */
static void cfg_expand(const char *in, char *out, size_t n)
{
    if (!strncmp(in, "$PREFIX", 7)) snprintf(out, n, "%s%s", prefix(), in + 7);
    else                            snprintf(out, n, "%s", in);
}

static char *cfg_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* Strip an unquoted #comment.  Quoted because a mirror URL may contain '#'. */
static void cfg_decomment(char *s)
{
    int q = 0;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (*s == '#' && !q) { *s = '\0'; return; }
    }
}

static void cfg_unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') { memmove(s, s + 1, n - 2); s[n-2] = '\0'; }
}

static int cfg_bool(const char *v, int *out)
{
    if (!strcmp(v, "true"))  { *out = 1; return 0; }
    if (!strcmp(v, "false")) { *out = 0; return 0; }
    return -1;
}

/* Apply one dotted key.  Returns 0 if the key is known and the value parsed. */
static int cfg_apply(struct alr_config *c, const char *key, char *val)
{
    cfg_unquote(val);
    if (!strcmp(key, "default_distro")) {
        snprintf(c->distro, sizeof c->distro, "%s", val); c->has_distro = 1; return 0;
    }
    if (!strcmp(key, "runtime.fakeroot")) {
        if (cfg_bool(val, &c->fakeroot) != 0) return -1;
        c->has_fakeroot = 1; return 0;
    }
    if (!strcmp(key, "runtime.log")) {
        char *end; long v = strtol(val, &end, 10);
        if (end == val || *end) return -1;
        c->log = (int)v; c->has_log = 1; return 0;
    }
    if (!strcmp(key, "paths.root")) {
        cfg_expand(val, c->root, sizeof c->root); c->has_root = 1; return 0;
    }
    if (!strcmp(key, "network.mirror")) {
        snprintf(c->mirror, sizeof c->mirror, "%s", val);
        c->has_mirror = *val != '\0'; return 0;
    }
    return -1;
}

static void cfg_read(struct alr_config *c, const char *path)
{
    char line[1024], table[64] = "";
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        char *s, *eq, key[160];
        cfg_decomment(line);
        s = cfg_trim(line);
        if (!*s) continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) continue;
            *close = '\0';
            snprintf(table, sizeof table, "%s", cfg_trim(s + 1));
            continue;
        }
        if (!(eq = strchr(s, '='))) continue;
        *eq = '\0';
        if (*table) snprintf(key, sizeof key, "%s.%s", table, cfg_trim(s));
        else        snprintf(key, sizeof key, "%s", cfg_trim(s));
        if (cfg_apply(c, key, cfg_trim(eq + 1)) != 0) {
            fprintf(stderr, "alr: %s: unknown or invalid setting `%s`\n", path, key);
            c->bad++;
        }
    }
    fclose(fp);
}

static const struct alr_config *cfg(void)
{
    char p[ALR_PBUF];
    if (g_cfg_loaded) return &g_cfg;
    g_cfg_loaded = 1;
    cfg_path(p, sizeof p, 0); cfg_read(&g_cfg, p);
    cfg_path(p, sizeof p, 1); cfg_read(&g_cfg, p);   /* user wins */
    return &g_cfg;
}

/* The directory that CONTAINS the rootfses, in host form -- what
 * ALR_ROOT_DIR would have to be for `alr -d <distro> run` to find this one.
 * Filled by distro_root() so the wrappers bake in the same answer the CLI
 * itself resolved, rather than re-deriving it from an environment the guest
 * does not have. */
static char g_root_dir_for_shims[ALR_PBUF];

static void distro_root(char *out, size_t n, const char *distro)
{
    const char *root = getenv("ALR_ROOT_DIR");
    if (!root || !*root) root = cfg()->has_root ? cfg()->root : NULL;
    if (!distro || !*distro) distro = "ubuntu-24.04";
    if (!distro_name_ok(distro))
        die("bad-distro-name",
            "distro names may contain only [A-Za-z0-9._-] and may not start "
            "with '.'");
    if (root && *root) {
        snprintf(out, n, "%s/%s", root, distro);
        snprintf(g_root_dir_for_shims, sizeof g_root_dir_for_shims, "%s", root);
    } else {
        snprintf(out, n, "%s/var/lib/alr/distros/%s", prefix(), distro);
        snprintf(g_root_dir_for_shims, sizeof g_root_dir_for_shims,
                 "%s/var/lib/alr/distros", prefix());
    }
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int run_cmd(char *const argv[])
{
    pid_t p = fork();
    int st;
    if (p < 0) return -1;
    if (p == 0) { execvp(argv[0], argv); _exit(127); }
    if (waitpid(p, &st, 0) < 0) return -1;
    return alr_exit_code(st);
}

/* ── install ─────────────────────────────────────────────────────────── */

/* ldconfig is STATICALLY linked (no PT_INTERP), so LD_PRELOAD cannot reach it:
 * it walks the ANDROID host root, finds no /lib/aarch64-linux-gnu, and then
 * fails writing /etc/ld.so.cache~ because Android's /etc is a symlink to the
 * read-only /system/etc.  Replacing it with a no-op is CORRECT here rather
 * than a workaround: this runtime always invokes the loader with an explicit
 * --library-path and --inhibit-cache (ADR 0002), so ld.so.cache has no
 * consumer.  We are removing work whose only output is never read. */
static const char LDCONFIG_SHIM[] =
    "#!/bin/sh\n"
    "# alr: no-op. ld.so is always invoked with --inhibit-cache, so the cache\n"
    "# this would build is never consulted. See docs/adr/0002.\n"
    "exit 0\n";

static int write_shim(const char *R)
{
    char path[ALR_PBUF];
    FILE *fp;
    /* Ubuntu 24.04 is a merged-/usr system: /sbin is a symlink to usr/sbin and
     * libc-bin's file list names /usr/sbin/ldconfig.  Diverting /sbin/ldconfig
     * registers a path dpkg never touches, so the diversion silently does
     * nothing and the next libc-bin upgrade reinstalls the real wrapper. */
    snprintf(path, sizeof path, "%s/usr/sbin/ldconfig", R);
    unlink(path);
    if (!(fp = fopen(path, "w"))) return -1;
    fputs(LDCONFIG_SHIM, fp);
    fclose(fp);
    return chmod(path, 0755);
}

/* docs/05-provisioning-spec.md §3.2: add the Termux uid/gid to the guest's
 * user databases -- "게스트의 `ls -l`, `getpwuid()`가 해석되게".
 *
 * MEASURED 2026-08-03 before this existed:
 *     $ alr run ls -ld /root
 *     drwx------ 6 10297 10297 3452 Aug  3 10:25 /root
 *     $ alr run --no-fakeroot whoami
 *     whoami: cannot find name for user ID 10297
 * Every file in the rootfs is owned by the Termux uid on disk, and no entry in
 * the image describes it, so every ownership display in the guest degrades to
 * a bare number and getpwuid() returns NULL.  Note this is true in BOTH
 * fakeroot modes: fakeroot lies about the PROCESS credentials (getuid), not
 * about st_uid, which is deliberate (§2694 of the preload) and unaffected here.
 *
 * APPEND, never overwrite -- the spec is explicit, and for good reason: the
 * image's root/daemon/nobody entries and every uid apt creates for a package
 * live in these files.  Truncating them would break far more than it fixed.
 *
 * REWRITTEN rather than appended-if-absent, because the uid is not immutable:
 * restoring a Termux backup onto a fresh install keeps the data directory and
 * gets a NEW uid, at which point a leftover line maps a uid that no longer
 * exists.  Dropping our own line and re-adding it makes the operation
 * idempotent and self-healing, which is why `alr update-components` runs it
 * too. */
static int rewrite_userdb(const char *R, const char *rel, const char *line)
{
    char path[ALR_PBUF], tmp[ALR_PBUF], buf[1024];
    FILE *in, *out;
    /* Track line starts properly: fgets returns a PARTIAL line for anything
     * over the buffer, and treating each chunk as a line would let a long
     * entry beginning "alr:" mid-stream drop the wrong bytes. */
    int at_bol = 1, drop = 0, ended_nl = 1;

    snprintf(path, sizeof path, "%s%s", R, rel);
    snprintf(tmp, sizeof tmp, "%s.alr-tmp", path);
    if (!(out = fopen(tmp, "w"))) return -1;
    if ((in = fopen(path, "r"))) {
        while (fgets(buf, sizeof buf, in)) {
            if (at_bol) drop = !strncmp(buf, "alr:", 4);
            at_bol = strchr(buf, '\n') != NULL;
            if (!drop) { fputs(buf, out); ended_nl = at_bol; }
        }
        fclose(in);
    }
    /* A stock file ends in a newline, but a hand-edited one may not, and
     * gluing our entry onto the tail of theirs would corrupt both. */
    if (!ended_nl) fputc('\n', out);
    fputs(line, out);
    if (fclose(out) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void sync_userdb(const char *R)
{
    char pw[256], gr[128];

    snprintf(pw, sizeof pw, "alr:x:%lu:%lu:alr:/root:/bin/bash\n",
             (unsigned long)getuid(), (unsigned long)getgid());
    /* Four fields, not seven.  A group entry is name:passwd:gid:members --
     * docs/05 §3.2 showed the passwd line for both files, which would have put
     * a malformed record in front of every getgrgid() in the guest. */
    snprintf(gr, sizeof gr, "alr:x:%lu:\n", (unsigned long)getgid());

    if (rewrite_userdb(R, "/etc/passwd", pw) != 0 ||
        rewrite_userdb(R, "/etc/group", gr) != 0)
        fprintf(stderr, "alr: could not add the Termux uid to the guest user "
                        "databases; ownership will display as raw numbers\n");
}

/* Files that ship as ZERO BYTES in ubuntu-base and must be written, plus the
 * two config files that make apt survive a non-root, seccomp-restricted host.
 * Everything else in /etc is already correct -- notably ubuntu.sources, which
 * already points at ports.ubuntu.com for arm64 and MUST NOT be rewritten. */
static void repair(const char *R)
{
    struct { const char *rel; const char *body; } f[] = {
      { "/etc/resolv.conf", "nameserver 8.8.8.8\nnameserver 8.8.4.4\n" },
      { "/etc/hosts",
        "127.0.0.1\tlocalhost localhost.localdomain\n"
        "::1\tlocalhost localhost.localdomain ip6-localhost ip6-loopback\n" },
      /* Without this apt dies in setgroups() on its very first run: the
       * setuid family is blocked by the Android app seccomp filter, and apt
       * drops to the _apt user before it checks anything. */
      { "/etc/apt/apt.conf.d/99-alr-no-sandbox", "APT::Sandbox::User \"root\";\n" },
      /* fsync per file is brutal on Android flash. */
      { "/etc/dpkg/dpkg.cfg.d/99-alr", "force-unsafe-io\n" },
      /* See LDCONFIG_SHIM above.  This is the fallback copy; divert_ldconfig()
       * makes it survive libc-bin upgrades. */
      { NULL, NULL }
    };
    int i;
    for (i = 0; f[i].rel; i++) {
        char path[ALR_PBUF];
        FILE *fp;
        snprintf(path, sizeof path, "%s%s", R, f[i].rel);
        unlink(path);                 /* a later image may ship a symlink */
        fp = fopen(path, "w");
        if (!fp) { fprintf(stderr, "alr: cannot write %s: %s\n",
                           path, strerror(errno)); continue; }
        fputs(f[i].body, fp);
        fclose(fp);
    }
    write_shim(R);
    sync_userdb(R);
    {   /* directories the tarball ships empty or not at all.
         * The apt ones matter: apt does not create them and fails with a bare
         * ENOENT from mkstemp/statvfs if they are missing. */
        const char *d[] = { "/dev", "/proc", "/sys", "/run", "/tmp",
                            "/var/cache/apt/archives",
                            "/var/cache/apt/archives/partial",
                            "/var/lib/apt/lists",
                            "/var/lib/apt/lists/partial",
                            "/var/log/apt", "/usr/local/bin", "/opt", NULL };
        for (i = 0; d[i]; i++) {
            char path[ALR_PBUF];
            snprintf(path, sizeof path, "%s%s", R, d[i]);
            mkdir(path, strcmp(d[i], "/tmp") ? 0755 : 01777);
        }
    }
}

static int copy_file(const char *from, const char *to)
{
    char buf[65536];
    int in, out;
    ssize_t n;
    struct stat st;

    in = open(from, O_RDONLY);
    if (in < 0) return -1;
    if (fstat(in, &st) != 0) { close(in); return -1; }
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

/* ubuntu-base DOES contain hardlink members (perl5.38.2 -> perl,
 * uncompress -> gunzip, and more).  link(2) fails with EACCES on Android
 * app-private storage (docs/01-platform-facts.md §B6, measured by doctor P6),
 * so tar skips them and the files simply do not exist -- a silently broken
 * rootfs.  Re-create each one, preferring a real hardlink where the kernel
 * allows it and falling back to a copy.
 *
 * A copy is NOT equivalent: st_nlink stays 1 and edits do not propagate.  For
 * a freshly extracted, read-mostly rootfs that is acceptable; the general fix
 * for guest-created hardlinks is the preload's link2symlink layer (ADR 0004). */
static int fix_hardlinks(const char *tarball, const char *root,
                         long *members_out, long *special_out)
{
    char cmd[ALR_PBUF * 2];
    char line[ALR_PBUF * 2];
    FILE *fp;
    int made = 0, copied = 0, failed = 0;
    long members = 0, special = 0;

    snprintf(cmd, sizeof cmd, "tar -tvzf '%s' 2>/dev/null", tarball);
    fp = popen(cmd, "r");
    if (!fp) return -1;

    while (fgets(line, sizeof line, fp)) {
        char src[ALR_PBUF], dst[ALR_PBUF];
        char *sep = strstr(line, " link to ");
        char *name;
        size_t k;

        members++;
        /* Members tar cannot create here and we do not want: device nodes and
         * fifos.  Counted from the listing we are already reading rather than
         * from a second decompression pass. */
        if (line[0] == 'c' || line[0] == 'b' || line[0] == 'p') special++;

        if (!sep || line[0] != 'h') continue;    /* 'h' = hardlink member */
        *sep = '\0';
        /* the member name is the last field before " link to " */
        name = sep;
        while (name > line && name[-1] != ' ') name--;
        k = strlen(sep + 9);
        while (k && (sep[9 + k - 1] == '\n' || sep[9 + k - 1] == '\r')) k--;
        sep[9 + k] = '\0';

        /* Both halves come from the archive, so both are attacker-controlled
         * under `alr install --url`.  An absolute or climbing name would make
         * link() operate outside the extraction root -- and unlike the symlink
         * case, a hardlink to a file outside is indistinguishable from the
         * real thing afterwards. */
        if (name[0] == '/' || (sep + 9)[0] == '/' ||
            strstr(name, "../") || strstr(sep + 9, "../")) {
            fprintf(stderr, "alr: REJECTING hardlink member outside the "
                            "rootfs: %s -> %s\n", name, sep + 9);
            failed++;
            continue;
        }
        snprintf(dst, sizeof dst, "%s/%s", root, name);
        snprintf(src, sizeof src, "%s/%s", root, sep + 9);

        if (access(dst, F_OK) == 0) continue;    /* tar managed it */
        if (access(src, F_OK) != 0) { failed++; continue; }

        if (link(src, dst) == 0)      made++;
        else if (copy_file(src, dst) == 0) copied++;
        else                          failed++;
    }
    pclose(fp);
    if (members_out) *members_out = members;
    if (special_out) *special_out = special;

    if (made || copied || failed)
        printf("alr: hardlink members: %d linked, %d copied, %d failed\n",
               made, copied, failed);
    return failed ? -1 : 0;
}

/* Create a symlink INSIDE the rootfs.  It MUST be relative: the kernel
 * resolves symlink targets against the REAL host root and the preload cannot
 * intervene in kernel symlink resolution, so an absolute "/opt/node/bin/node"
 * silently points at a host path that does not exist.  Ubuntu itself uses
 * relative links throughout the rootfs for exactly this reason.
 *
 * `link` and `target` are GUEST absolute paths; the depth is computed, because
 * getting it wrong is silent: from /usr/local/bin the root is ../../../, and
 * ../../ lands in /usr/... which merely looks plausible. */
static int guest_symlink(const char *R, const char *link, const char *target)
{
    char host[ALR_PBUF], rel[ALR_PBUF];
    size_t o = 0;
    const char *p;
    int depth = 0;

    for (p = link + 1; *p; p++) if (*p == '/') depth++;   /* dirs above the leaf */
    while (depth-- > 0 && o + 3 < sizeof rel) { memcpy(rel + o, "../", 3); o += 3; }
    snprintf(rel + o, sizeof rel - o, "%s", target + 1);  /* drop leading '/' */

    snprintf(host, sizeof host, "%s%s", R, link);
    unlink(host);
    return symlink(rel, host);
}

/* tar restores symlink targets verbatim, so every ABSOLUTE target shipped in
 * the image points at ANDROID's root rather than the rootfs.  The preload
 * relativises links the GUEST creates (04-preload-spec.md §6.16), but these are
 * created by tar, outside it.
 *
 * ubuntu-base ships 17 of them and they were broken from the first boot -- most
 * damagingly /usr/bin/awk -> /etc/alternatives/awk, which is why ucf, locales,
 * mercurial and php8.3-common all died with a bare "awk: not found" and left
 * five packages unconfigured in the breadth run.
 *
 * `depth` is the directory's depth below the rootfs root, which is exactly the
 * number of "../" needed to climb back to it. */
/* Does a symlink target climb above the extraction root?
 *
 * docs/05 §2 requires rejecting link targets that escape, and GNU tar does not
 * check this -- it creates `x -> ../../../etc/passwd` exactly as written.  The
 * sha256-verified ubuntu-base has none, but `alr install --url` accepts any
 * archive the user names, and this is the one §2 rule the shell-out did not
 * already satisfy (ADR 0009).
 *
 * Purely lexical, which is the right test here: `depth` is how far the
 * containing directory sits below the root, and every `..` in the target
 * climbs one.  Going negative at any point means the link reaches outside,
 * whether or not the intermediate directories happen to exist right now --
 * a target that escapes and comes back would still resolve outside the root
 * for as long as the intermediate component is itself a symlink. */
static int link_escapes(const char *tgt, int depth)
{
    const char *p = tgt;
    int lvl = depth;

    if (*p == '/') return 0;            /* absolute: handled by the caller */
    while (*p) {
        const char *s = p;
        size_t n;
        while (*p && *p != '/') p++;
        n = (size_t)(p - s);
        while (*p == '/') p++;
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (--lvl < 0) return 1;
        } else if (*p) {
            /* Only non-final components descend; the last one is the target
             * name itself and cannot be stepped into. */
            lvl++;
        }
    }
    return 0;
}

static void relativize_tree(const char *dir, int depth, int *fixed, int *failed,
                            int *escaped)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char path[ALR_PBUF];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path)
            continue;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            relativize_tree(path, depth + 1, fixed, failed, escaped); continue;
        }
        if (!S_ISLNK(st.st_mode)) continue;
        {
            char tgt[ALR_PBUF], rel[ALR_PBUF];
            ssize_t r = readlink(path, tgt, sizeof tgt - 1);
            size_t o = 0, n;
            int k;
            if (r <= 0) continue;
            tgt[r] = '\0';
            if (tgt[0] != '/') {                    /* already relative */
                if (link_escapes(tgt, depth)) {
                    fprintf(stderr, "alr: REJECTING symlink that escapes the "
                                    "rootfs: %s -> %s\n", path, tgt);
                    unlink(path);
                    (*escaped)++;
                }
                continue;
            }
            /* An ABSOLUTE target with a `..` in it escapes just as surely, and
             * the first version of this check missed the whole branch: the
             * escape test sat only on the relative side, and `/../../etc/passwd`
             * takes the other one.  Worse, alr_is_sysdir("/proc/../../etc")
             * matches on the /proc prefix and would have left it verbatim.
             *
             * Rejected before relativization rather than after, because
             * relativization is what turns it into the form the other branch
             * would have caught -- checking only afterwards would mean the
             * dangerous case is the one that got rewritten first. */
            if (strstr(tgt, "/../") || !strcmp(tgt, "/..") ||
                (strlen(tgt) > 3 && !strcmp(tgt + strlen(tgt) - 3, "/.."))) {
                fprintf(stderr, "alr: REJECTING symlink that escapes the "
                                "rootfs: %s -> %s\n", path, tgt);
                unlink(path);
                (*escaped)++;
                continue;
            }
            /* /proc, /sys and /dev are NOT rewritten at runtime either, so an
             * absolute target into them is already correct. */
            if (alr_is_sysdir(tgt)) continue;
            n = strlen(tgt + 1);
            if ((size_t)depth * 3 + n + 1 > sizeof rel) { (*failed)++; continue; }
            for (k = 0; k < depth; k++) { memcpy(rel + o, "../", 3); o += 3; }
            memcpy(rel + o, tgt + 1, n + 1);
            if (unlink(path) == 0 && symlink(rel, path) == 0) (*fixed)++;
            else (*failed)++;
        }
    }
    closedir(d);
}

static int guest_run(const char *distro, const char *const *argv, int fakeroot)
{
    char buf[ALR_PBUF * 2];
    char *sh[4];
    size_t o = 0;
    int i;
    o += (size_t)snprintf(buf + o, sizeof buf - o,
                          "%sALR_ROOT_DIR='%s' '%s' -d '%s' run",
                          fakeroot ? "ALR_FAKEROOT=1 " : "",
                          getenv("ALR_ROOT_DIR") ? getenv("ALR_ROOT_DIR") : "",
                          g_self, distro);
    for (i = 0; argv[i]; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, " '%s'", argv[i]);
    sh[0] = (char *)"sh"; sh[1] = (char *)"-c"; sh[2] = buf; sh[3] = NULL;
    return run_cmd(sh);
}

/* Fetch a URL to a path, verifying nothing (callers that need integrity pass a
 * checksum step of their own). */
static int fetch(const char *url, const char *out)
{
    char *dl[8];
    dl[0] = (char *)"curl"; dl[1] = (char *)"-fsSL"; dl[2] = (char *)"--retry";
    dl[3] = (char *)"3"; dl[4] = (char *)"-o"; dl[5] = (char *)out;
    dl[6] = (char *)url; dl[7] = NULL;
    return run_cmd(dl);
}

/* --with node : nodejs.org LTS tarball.  Deliberately NOT apt's nodejs, which
 * is 18.19 (EOL).  A current Node also exercises the io_uring SIGSYS rescue,
 * which apt's libuv 1.44 never triggers. */
/* --with node : nodejs.org LTS tarball.  Deliberately NOT apt's nodejs, which
 * is 18.19 (EOL).  A current Node also exercises the io_uring SIGSYS rescue,
 * which apt's libuv 1.44 never triggers. */
static int with_node(const char *R, const char *cache)
{
    char cmd[ALR_PBUF * 3], ver[64], tarball[ALR_PBUF];
    char *sh[4];
    FILE *fp;

    ver[0] = '\0';
    fp = popen("curl -fsSL --max-time 30 https://nodejs.org/dist/index.json"
               " 2>/dev/null | tr '{' '\\n' | grep -m1 '\"lts\":\"'"
               " | sed -n 's/.*\"version\":\"\\([^\"]*\\)\".*/\\1/p'", "r");
    if (fp) {
        if (fgets(ver, sizeof ver, fp)) ver[strcspn(ver, "\r\n")] = '\0';
        pclose(fp);
    }
    if (ver[0] != 'v') snprintf(ver, sizeof ver, "v22.20.0");   /* offline pin */
    printf("alr: node %s\n", ver);

    snprintf(tarball, sizeof tarball, "%s/node-%s-linux-arm64.tar.xz", cache, ver);
    if (access(tarball, R_OK) != 0) {
        char url[ALR_PBUF];
        snprintf(url, sizeof url,
                 "https://nodejs.org/dist/%s/node-%s-linux-arm64.tar.xz", ver, ver);
        if (fetch(url, tarball) != 0) return -1;
    }
    snprintf(cmd, sizeof cmd,
        "set -e\n"
        "curl -fsSL --max-time 60 -o '%s/SHASUMS256.txt'"
        " https://nodejs.org/dist/%s/SHASUMS256.txt\n"
        "cd '%s'\n"
        "grep ' node-%s-linux-arm64.tar.xz$' SHASUMS256.txt | sha256sum -c - >/dev/null\n"
        "rm -rf '%s/opt/node'\n"
        "tar -xJf '%s' -C '%s/opt'\n"
        "mv '%s/opt/node-%s-linux-arm64' '%s/opt/node'\n",
        cache, ver, cache, ver, R, tarball, R, R, ver, R);
    sh[0] = (char *)"sh"; sh[1] = (char *)"-c"; sh[2] = cmd; sh[3] = NULL;
    if (run_cmd(sh) != 0) return -1;

    {
        static const char *const b[] = { "node", "npm", "npx", "corepack", NULL };
        int i;
        for (i = 0; b[i]; i++) {
            char link[ALR_PBUF], tgt[ALR_PBUF];
            snprintf(link, sizeof link, "/usr/local/bin/%s", b[i]);
            snprintf(tgt,  sizeof tgt,  "/opt/node/bin/%s", b[i]);
            guest_symlink(R, link, tgt);
        }
    }
    return 0;
}

/* --with codex : the Rust binary release.  NOT the npm package, which drags in
 * a ~129 MB platform tgz plus a Node runtime purely to exec the same binary. */
static int with_codex(const char *R, const char *cache)
{
    char cmd[ALR_PBUF * 3], tag[64];
    char *sh[4];
    FILE *fp;

    tag[0] = '\0';
    fp = popen("curl -fsSL --max-time 30"
               " https://api.github.com/repos/openai/codex/releases/latest"
               " 2>/dev/null | sed -n 's/.*\"tag_name\": *\"\\([^\"]*\\)\".*/\\1/p'"
               " | head -1", "r");
    if (fp) {
        if (fgets(tag, sizeof tag, fp)) tag[strcspn(tag, "\r\n")] = '\0';
        pclose(fp);
    }
    if (!tag[0]) snprintf(tag, sizeof tag, "rust-v0.146.0");    /* offline pin */
    printf("alr: codex %s\n", tag);

    /* NOTE the `-musl` in the asset name: this is a STATICALLY linked build.
     * LD_PRELOAD cannot reach it, so codex runs with NO path virtualization and
     * sees Android's filesystem rather than the rootfs -- see docs/RISKS.md and
     * docs/evidence/2026-08-03-m12-spawn-resolver.md §8.  `codex --version`
     * working does not mean codex operates inside the guest.
     *
     * Checksum: node is verified against SHASUMS256.txt (see with_node); codex
     * had NO verification at all.  Try the release's own checksum asset under
     * the names GitHub projects commonly use, and if none exists say so out
     * loud rather than staying silent about an unverified 100+ MB download. */
    snprintf(cmd, sizeof cmd,
        "set -e\n"
        "A=codex-aarch64-unknown-linux-musl.tar.gz\n"
        "[ -f '%s'/$A ] || curl -fsSL --max-time 900 -o '%s'/$A"
        " https://github.com/openai/codex/releases/download/%s/$A\n"
        "rm -rf '%s'/.codex-x\n"
        "mkdir -p '%s'/.codex-x\n"
        "tar -xzf '%s'/$A -C '%s'/.codex-x\n"
        "B=$(find '%s'/.codex-x -type f -perm -u+x | head -1)\n"
        "install -m755 \"$B\" '%s/usr/local/bin/codex'\n"
        "rm -rf '%s'/.codex-x\n",
        cache, cache, tag, cache, cache, cache, cache, cache, R, cache);
    sh[0] = (char *)"sh"; sh[1] = (char *)"-c"; sh[2] = cmd; sh[3] = NULL;
    if (run_cmd(sh) != 0) return -1;

    {   /* Verify AFTER the tarball is on disk, before it is trusted. */
        char vc[ALR_PBUF * 2];
        char *vs[4];
        snprintf(vc, sizeof vc,
            "A=codex-aarch64-unknown-linux-musl.tar.gz\n"
            "for C in checksums.txt SHA256SUMS sha256sums.txt; do\n"
            "  curl -fsSL --max-time 60 -o '%s'/$C"
            "   https://github.com/openai/codex/releases/download/%s/$C 2>/dev/null || continue\n"
            "  W=$(grep -m1 \"$A\" '%s'/$C | tr -d '*' | awk '{print $1}')\n"
            "  [ -n \"$W\" ] || continue\n"
            "  G=$(sha256sum '%s'/$A | awk '{print $1}')\n"
            "  [ \"$W\" = \"$G\" ] || { echo \"alr: codex sha256 MISMATCH\" >&2; exit 1; }\n"
            "  echo 'alr: codex sha256 ok'; exit 0\n"
            "done\n"
            "echo 'alr: WARNING codex tarball is UNVERIFIED - the release publishes"
            " no checksum asset under any name we know' >&2\n"
            "exit 0\n",
            cache, tag, cache, cache);
        vs[0] = (char *)"sh"; vs[1] = (char *)"-c"; vs[2] = vc; vs[3] = NULL;
        if (run_cmd(vs) != 0) {
            fprintf(stderr, "alr: refusing the codex tarball\n");
            return -1;
        }
    }

    {   /* Landlock and bubblewrap cannot work inside an Android app process, so
         * Codex's own sandbox must be off.  alr is NOT a security boundary --
         * say so where the user will actually see it. */
        char path[ALR_PBUF];
        FILE *f;
        snprintf(path, sizeof path, "%s/root/.codex", R);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/root/.codex/config.toml", R);
        f = fopen(path, "w");
        if (f) {
            fputs("# alr: Codex's Linux sandbox relies on Landlock and bubblewrap,\n"
                  "# neither of which functions inside an Android app process.\n"
                  "# Confirm the exact mode name with `codex --help` for your version.\n"
                  "sandbox_mode = \"danger-full-access\"\n", f);
            fclose(f);
        }
        /* Do NOT claim the sandbox is off -- we do not know that it is.
         * MEASURED (M18): breaking this file three ways (invalid TOML, deleted,
         * bogus mode value) changes nothing about how codex starts.  That is
         * not proof it is unread -- `codex --version` may not parse config at
         * all -- but codex is a static musl binary that LD_PRELOAD cannot
         * reach (M12 §8), so it resolves ~/.codex against ANDROID's root, and
         * Android has no /root.  Saying "sandbox disabled" would assert more
         * than the evidence supports. */
        /* MEASURED 2026-08-03, and it settles RISKS R8: codex CANNOT read the
         * file written above.  With the default HOME=/root it prints
         *   WARNING: ... could not create PATH aliases: Read-only file system
         * because it resolves /root against ANDROID, which has no /root.  Give
         * it an Android-visible HOME and the warning disappears and it creates
         * ~/.codex there:
         *   alr run -e HOME=$HOME/codexhome /usr/local/bin/codex ...
         * So the rootfs copy is written where codex will never look.  It is
         * kept because it costs nothing and documents intent, but the NOTE now
         * says plainly that it is not the file codex reads. */
        printf(
          "alr: NOTE codex is installed, but it is NOT a guest program.\n"
          "     It is statically linked, so path virtualization cannot reach it\n"
          "     (ADR 0008) -- it resolves every path against ANDROID, not the\n"
          "     rootfs.  MEASURED: the binary produces identical output run\n"
          "     directly from Termux with no alr involved, so `alr run codex`\n"
          "     adds nothing.\n"
          "     Consequences you will actually hit:\n"
          "       - the config alr wrote to %s/root/.codex/config.toml is NOT\n"
          "         the file codex reads; with HOME=/root it cannot even create\n"
          "         ~/.codex, because Android has no /root\n"
          "       - shell commands it spawns see Android's toybox: /bin/sh and\n"
          "         /bin/ls exist, /bin/bash and /usr/bin/env do not\n"
          "     Run it against Termux paths instead:\n"
          "       PATH=$PREFIX/bin:/system/bin %s/usr/local/bin/codex\n"
          "     Treat the guest as UNSANDBOXED regardless -- alr is not a\n"
          "     security boundary.\n", R, R);
    }
    return 0;
}

/* Put libalr_preload.so inside the rootfs.  WITHOUT THIS the guest has no path
 * virtualization at all: `alr run /path/to/x` still works because alr resolves
 * that path itself, but anything the guest looks up (apt reading
 * /etc/apt/apt.conf.d, env searching PATH for node) resolves against the
 * ANDROID host and fails in ways that look like a broken rootfs.
 *
 * Searched next to the alr binary first, then $PREFIX/share/alr -- the release
 * layout.  Returns 0 if installed, -1 if the .so could not be found. */
/* Rewriting the shim after each `alr` operation is not enough: a libc-bin
 * upgrade restores the genuine wrapper, which runs the static ldconfig.real,
 * which fails -- and that leaves libc-bin half-configured so every LATER apt
 * install dies too.  It surfaced as 96 packages "unavailable" in the breadth
 * run, and anything that runs apt without going through `alr` (a shell inside
 * the guest) reintroduces it.
 *
 * Register a dpkg diversion instead: dpkg then writes future versions to
 * /sbin/ldconfig.distrib and leaves our no-op in place, permanently. */
static void divert_ldconfig(const char *distro, const char *R)
{
    static const char *const dv[] = { "/usr/bin/dpkg-divert", "--local",
                                      "--rename", "--add", "/usr/sbin/ldconfig",
                                      NULL };
    if (guest_run(distro, dv, 1) != 0) {
        fprintf(stderr, "alr: WARNING dpkg-divert failed; the ldconfig no-op "
                        "will not survive a libc-bin upgrade\n");
        return;
    }
    /* --rename moved our shim to .distrib; put it back at the real name. */
    if (write_shim(R) != 0)
        fprintf(stderr, "alr: WARNING could not reinstall the ldconfig no-op\n");
}

static int install_preload(const char *R)
{
    char src[ALR_PBUF], dst[ALR_PBUF], dir[ALR_PBUF];
    const char *cands[3];
    char selfdir[ALR_PBUF];
    char *slash;
    int i;

    snprintf(selfdir, sizeof selfdir, "%s", g_self);
    slash = strrchr(selfdir, '/');
    if (slash) *slash = '\0'; else snprintf(selfdir, sizeof selfdir, ".");

    snprintf(src, sizeof src, "%s/libalr_preload.so", selfdir);
    cands[0] = src;
    { static char c1[ALR_PBUF];
      snprintf(c1, sizeof c1, "%s/share/alr/libalr_preload.so", prefix());
      cands[1] = c1; }
    { static char c2[ALR_PBUF];
      snprintf(c2, sizeof c2, "%s/build/libalr_preload.so", selfdir);
      cands[2] = c2; }

    /* Ship the manifest beside the .so (docs/05-provisioning-spec.md §3.4).
     * Three separate incidents in this project's evidence were "the deployed
     * binary was not what I thought"; a self-describing rootfs is the cheap
     * structural fix. */
    snprintf(dir, sizeof dir, "%s/usr/lib/alr", R);
    { char *mk[4]; mk[0]=(char*)"mkdir"; mk[1]=(char*)"-p"; mk[2]=dir; mk[3]=NULL;
      run_cmd(mk); }
    snprintf(dst, sizeof dst, "%s/usr/lib/alr/libalr_preload.so", R);

    for (i = 0; i < 3; i++) {
        if (access(cands[i], R_OK) != 0) continue;
        if (copy_file(cands[i], dst) == 0) {
            char msrc[ALR_PBUF], mdst[ALR_PBUF];
            char *dot;
            chmod(dst, 0755);
            /* The manifest lives beside whichever candidate we took, under
             * ONE OF TWO NAMES.  The build tree writes
             * libalr_preload.manifest.json; the release tarball renames it to
             * share/alr/manifest.json on purpose (that directory holds exactly
             * one .so, so repeating its name is noise).  Deriving only the
             * first name meant EVERY install from a published tarball printed
             * "NOTE no manifest beside ..." and left the rootfs unable to say
             * which build it carries -- on the single most common install
             * path there is. */
            snprintf(mdst, sizeof mdst, "%s/usr/lib/alr/libalr_preload.manifest.json", R);
            snprintf(msrc, sizeof msrc, "%s", cands[i]);
            if ((dot = strstr(msrc, ".so")) && dot[3] == '\0')
                snprintf(dot, sizeof msrc - (size_t)(dot - msrc), ".manifest.json");
            if (access(msrc, R_OK) != 0) {
                char dir[ALR_PBUF], *slash;
                snprintf(dir, sizeof dir, "%s", cands[i]);
                if ((slash = strrchr(dir, '/'))) *slash = '\0';
                else snprintf(dir, sizeof dir, ".");
                snprintf(msrc, sizeof msrc, "%s/manifest.json", dir);
            }
            if (access(msrc, R_OK) == 0 && copy_file(msrc, mdst) == 0)
                chmod(mdst, 0644);
            else
                fprintf(stderr, "alr: NOTE no manifest beside %s; the rootfs will "
                                "not record which build it carries\n", cands[i]);
            printf("alr: preload installed from %s\n", cands[i]);
            return 0;
        }
    }
    fprintf(stderr,
        "alr: WARNING libalr_preload.so not found next to the binary or in\n"
        "     %s/share/alr/.  The guest will run WITHOUT path virtualization:\n"
        "     it will see the Android filesystem, not the rootfs.\n", prefix());
    return -1;
}

/* docs/05-provisioning-spec.md §1.1.  There is no `latest` symlink and
 * ubuntu-base-24.04-base-arm64.tar.gz is a 404 -- the images are named by point
 * release.  Read SHA256SUMS, take the highest point release, and keep its hash
 * so the download can actually be verified rather than merely fetched.
 *
 * Parsed in C rather than an awk one-liner because the '*' binary marker is
 * optional in SHA256SUMS and silently ending up with "*ubuntu-base-..." in the
 * URL would 404 in a way that reads like a network problem. */
#define ALR_UBUNTU_CDIMAGE "https://cdimage.ubuntu.com/ubuntu-base/releases"
#define ALR_UBUNTU_PIN     "ubuntu-base-24.04.4-base-arm64.tar.gz"
#define ALR_SUFFIX         "-base-arm64.tar.gz"

/* `rel` is the release as it appears in the URL and the file names: "24.04",
 * "26.04".  The two are NOT named alike -- 24.04 carries a point release
 * (ubuntu-base-24.04.4-base-arm64.tar.gz) and 26.04, freshly out, does not
 * (ubuntu-base-26.04-base-arm64.tar.gz).  Match both, and prefer the highest
 * point release when several exist; a bare name counts as point 0 so it wins
 * only when nothing else matches. */
static int discover_ubuntu(const char *rel, char *url, size_t urlsz,
                           char *sha, size_t shasz)
{
    char cmd[512], line[512], pfx[64];
    char bestname[256], besthash[80];
    FILE *fp;
    size_t plen;
    long best = -1;

    bestname[0] = besthash[0] = '\0';
    plen = (size_t)snprintf(pfx, sizeof pfx, "ubuntu-base-%s", rel);
    if (plen >= sizeof pfx) return -1;

    snprintf(cmd, sizeof cmd,
             "curl -fsSL --max-time 60 --retry 3 '%s/%s/release/SHA256SUMS' 2>/dev/null",
             cfg()->has_mirror ? cfg()->mirror : ALR_UBUNTU_CDIMAGE, rel);
    if (!(fp = popen(cmd, "r"))) return -1;
    while (fgets(line, sizeof line, fp)) {
        char h[80], name[256];
        const char *n, *rest;
        long pt;
        if (sscanf(line, "%79s %255s", h, name) != 2) continue;
        if (strlen(h) != 64) continue;
        n = name;
        if (*n == '*') n++;                       /* binary marker */
        if (strncmp(n, pfx, plen) != 0) continue;
        rest = n + plen;
        if (*rest == '.') {                       /* ...-24.04.4-base-arm64... */
            char *end;
            pt = strtol(rest + 1, &end, 10);
            if (end == rest + 1 || strcmp(end, ALR_SUFFIX) != 0) continue;
        } else if (strcmp(rest, ALR_SUFFIX) == 0) {
            pt = 0;                               /* ...-26.04-base-arm64...   */
        } else {
            continue;
        }
        if (pt > best) {
            best = pt;
            snprintf(bestname, sizeof bestname, "%s", n);
            snprintf(besthash, sizeof besthash, "%s", h);
        }
    }
    pclose(fp);
    if (best < 0) return -1;
    snprintf(url, urlsz, "%s/%s/release/%s",
             cfg()->has_mirror ? cfg()->mirror : ALR_UBUNTU_CDIMAGE, rel, bestname);
    snprintf(sha, shasz, "%s", besthash);
    return 0;
}

static int verify_sha256(const char *file, const char *want)
{
    char cmd[ALR_PBUF + 64], got[80];
    FILE *fp;
    int n;
    got[0] = '\0';
    snprintf(cmd, sizeof cmd, "sha256sum '%s' 2>/dev/null", file);
    if (!(fp = popen(cmd, "r"))) return -1;
    n = fscanf(fp, "%79s", got);
    pclose(fp);
    if (n != 1) return -1;
    return strcmp(got, want) == 0 ? 0 : -1;
}

/* Report enough to make a bug report actionable: three separate incidents in
 * this project's evidence docs were "the deployed binary was not what I
 * thought", so print the preload's hash rather than merely its path. */
/* sha256 of a file, or "" if unreadable.  popen'd because there is no hash in
 * libc and pulling one in for a version banner is not worth the code. */
static void file_sha256(const char *path, char *out, size_t outsz)
{
    char cmd[ALR_PBUF + 64];
    FILE *fp;
    out[0] = '\0';
    if (!path || !*path || access(path, R_OK) != 0) return;
    snprintf(cmd, sizeof cmd, "sha256sum '%s' 2>/dev/null", path);
    if ((fp = popen(cmd, "r"))) {
        if (fscanf(fp, "%79s", out) != 1) out[0] = '\0';
        pclose(fp);
    }
    (void)outsz;
}

static int cmd_version(const char *distro)
{
    char path[ALR_PBUF], cmd[ALR_PBUF + 64], sha[80];
    FILE *fp;

    char selfdir[ALR_PBUF], *slash;
    const char *cands[3];
    int i;

    printf("alr %s\n", ALR_VERSION);

    snprintf(selfdir, sizeof selfdir, "%s", g_self);
    slash = strrchr(selfdir, '/');
    if (slash) *slash = '\0'; else snprintf(selfdir, sizeof selfdir, ".");
    { static char c0[ALR_PBUF], c1[ALR_PBUF], c2[ALR_PBUF];
      snprintf(c0, sizeof c0, "%s/libalr_preload.so", selfdir);
      snprintf(c1, sizeof c1, "%s/share/alr/libalr_preload.so", prefix());
      snprintf(c2, sizeof c2, "%s/build/libalr_preload.so", selfdir);
      cands[0] = c0; cands[1] = c1; cands[2] = c2; }

    path[0] = '\0';
    for (i = 0; i < 3; i++)
        if (access(cands[i], R_OK) == 0) { snprintf(path, sizeof path, "%s", cands[i]); break; }

    if (*path) {
        sha[0] = '\0';
        snprintf(cmd, sizeof cmd, "sha256sum '%s' 2>/dev/null", path);
        if ((fp = popen(cmd, "r"))) {
            if (fscanf(fp, "%79s", sha) != 1) sha[0] = '\0';
            pclose(fp);
        }
        printf("preload (host)  %s\n", path);
        if (*sha) printf("  sha256       %s\n", sha);
    } else {
        printf("preload (host)  (not found)\n");
    }

    /* THE ONE THAT MATTERS: the copy the guest actually loads.
     *
     * install_preload() copies the .so into <R>/usr/lib/alr once, and
     * cmd_install returns early when the rootfs already exists -- so upgrading
     * alr (untar over $PREFIX, docs/INSTALL.md) leaves the OLD preload in the
     * rootfs while this command reported the NEW one's hash.  MEASURED on
     * 2026-08-03: guest 48efc48b..., reported 16167c4e...  The whole point of
     * this command is preload identity, and it was reporting an identity that
     * was not in use.
     *
     * INSTALL.md's check could not catch it either: it compares
     * $PREFIX/share/alr/manifest.json against this output, and both sides read
     * $PREFIX.  That is a tautology.  Compare against the GUEST copy. */
    { char r[ALR_PBUF], gp[ALR_PBUF], gsha[80];
      /* The distro main() resolved -- -d, then ALR_DISTRO, then the config's
       * default_distro.  Reading the environment directly here made the
       * preload-staleness check inspect a DIFFERENT rootfs than the one the
       * user named, and report a mismatch or a clean bill about the wrong one. */
      distro_root(r, sizeof r, distro);
      printf("rootfs  %s%s\n", r, is_dir(r) ? "" : "  (not installed)");
      if (is_dir(r)) {
          snprintf(gp, sizeof gp, "%s/usr/lib/alr/libalr_preload.so", r);
          file_sha256(gp, gsha, sizeof gsha);
          if (*gsha) {
              printf("preload (guest) %s\n", gp);
              printf("  sha256       %s\n", gsha);
              if (*sha && strcmp(sha, gsha) != 0) {
                  printf("\n"
                     "  !! MISMATCH: the guest loads a DIFFERENT build than this\n"
                     "     alr shipped.  Everything you run is using the guest\n"
                     "     copy, not the one above.\n"
                     "     reason=preload-stale\n"
                     "     Fix:  alr update-components\n");
                  return 1;
              }
          } else {
              printf("preload (guest) (missing -- run `alr update-components`)\n"
                     "  reason=preload-missing-in-rootfs\n");
              return 1;
          }
      }
    }
    return 0;
}

/* ── host-side wrappers for static guests (docs/adr/0008) ─────────────────
 *
 * A static binary cannot exec a guest program: the guest's binaries name
 * /lib/ld-linux-aarch64.so.1 in PT_INTERP and Android has no such file, so the
 * exec fails with 127 (the ADR0002 BARE EXECVE FAILS acceptance check is that
 * exact fact).  And it cannot be taught otherwise -- LD_PRELOAD is not loaded
 * for it at all.
 *
 * What it CAN exec is an Android binary.  So each wrapper here is a script
 * whose interpreter is Termux's own bash -- a real Android ELF, reachable from
 * a static binary's view of the filesystem -- which re-enters `alr run` and
 * gets the full loader + preload + supervisor launch for the guest tool.
 *
 * MEASURED 2026-08-04: with these on PATH, codex 0.146.0 (static musl, 269 MB)
 * runs the guest's ripgrep -- `codex doctor` reports
 *     search  ripgrep 14.1.0 (system, `rg`)
 * and its own log shows it invoking `git --version` and `git --exec-path`.
 *
 * THE ROOTFS IS BAKED IN, not inherited.  The first version read ALR_ROOT_DIR
 * from the environment and silently resolved against the DEFAULT rootfs when
 * the caller had none -- which is every child of a static binary, since alr
 * does not put ALR_ROOT_DIR in the guest environment.
 *
 * NO RECURSION: `alr run` builds its own PATH for a dynamic guest (guest-form,
 * without this directory), so a tool launched through a wrapper sees the
 * ordinary guest PATH and cannot re-enter the wrapper.
 *
 * The list is curated, not a sweep of the rootfs: these are the tools a coding
 * agent actually reaches for, and a wrapper per binary in /usr/bin would be
 * thousands of files re-written on every update. */
static const char *HOSTBIN_TOOLS[] = {
    "sh", "bash", "env", "git", "rg", "grep", "sed", "awk", "find", "ls",
    "cat", "head", "tail", "wc", "sort", "uniq", "diff", "patch", "make",
    "node", "npm", "npx", "python3", "pip3", "curl", "tar", "gzip", "which",
    NULL
};

static int install_hostbin(const char *distro, const char *R)
{
    char dir[ALR_PBUF], path[ALR_PBUF];
    int i, bad = 0;

    /* distro is NULL for `alr update-components` with no argument, and the
     * name is BAKED INTO every wrapper -- writing "(null)" there would produce
     * 28 files that all fail at the first use. */
    if (!distro || !*distro) distro = "ubuntu-24.04";

    snprintf(dir, sizeof dir, "%s/usr/lib/alr/hostbin", R);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "alr: cannot create %s: %s\n", dir, strerror(errno));
        return -1;
    }
    for (i = 0; HOSTBIN_TOOLS[i]; i++) {
        FILE *fp;
        snprintf(path, sizeof path, "%s/%s", dir, HOSTBIN_TOOLS[i]);
        unlink(path);
        if (!(fp = fopen(path, "w"))) { bad++; continue; }
        fprintf(fp,
            "#!%s/bin/bash\n"
            "# generated by alr -- host-side wrapper for a static guest.\n"
            "# docs/adr/0008-static-guest-binaries-non-goal.md\n"
            "export ALR_ROOT_DIR=%s\n"
            "exec %s -d %s run /usr/bin/env %s \"$@\"\n",
            prefix(), g_root_dir_for_shims, g_self, distro, HOSTBIN_TOOLS[i]);
        if (fclose(fp) != 0 || chmod(path, 0755) != 0) bad++;
    }
    if (bad) fprintf(stderr, "alr: %d host wrapper(s) could not be written\n", bad);
    return bad ? -1 : 0;
}

/* `alr update-components [<distro>]` -- docs/05-provisioning-spec.md.
 *
 * It exists because the .so the guest loads is a COPY made at install time,
 * and the documented upgrade path (untar a new release over $PREFIX) does not
 * touch it.  Without this there is no way to refresh it short of deleting the
 * rootfs -- which the spec names as exactly the reason this command matters:
 * "`.so`를 고칠 때마다 rootfs를 다시 깔 이유가 없다". */
static int cmd_update_components(const char *distro)
{
    char R[ALR_PBUF], gp[ALR_PBUF], before[80], after[80];

    distro_root(R, sizeof R, distro);
    if (!is_dir(R))
        die("rootfs-missing", "rootfs not installed; run `alr install`");

    snprintf(gp, sizeof gp, "%s/usr/lib/alr/libalr_preload.so", R);
    file_sha256(gp, before, sizeof before);

    if (install_preload(R) != 0)
        die("preload-install-failed", "could not refresh the guest preload");

    /* Refresh the host wrappers too: they bake in this binary's path and the
     * rootfs location, so an alr that moved leaves them pointing at nothing. */
    install_hostbin(distro, R);

    /* Also re-sync the uid lines.  This is the command for "refresh what alr
     * owns inside an existing rootfs", and the uid is exactly the thing that
     * can go stale under a rootfs that outlives the app install it was made
     * under. */
    sync_userdb(R);

    file_sha256(gp, after, sizeof after);
    if (*before && *after && !strcmp(before, after))
        printf("alr: guest preload already current (%s)\n", after);
    else
        printf("alr: guest preload updated\n  was %s\n  now %s\n",
               *before ? before : "(absent)", *after ? after : "(absent)");
    return 0;
}

/* docs/05-provisioning-spec.md §4: install must not declare success on a
 * rootfs it has not checked.
 *
 * tar's exit code cannot carry this.  A HEALTHY extraction exits non-zero here
 * -- hardlink members fail under Android's SELinux policy and that is expected
 * (see the "continuing" branch above) -- so the code has always ignored it.
 * Nor does comparing the tar listing against the disk work: a TRUNCATED
 * archive lists fewer members and extracts exactly those, so listing and disk
 * agree perfectly while the rootfs is useless.
 *
 * What actually separates the two cases is whether the essentials are there.
 * MEASURED 2026-08-03 by tests/device/install_gate.sh: `alr install` from a
 * tarball truncated to 2 MB reported SUCCESS. */
static int verify_rootfs(const char *R)
{
    /* Each of these is load-bearing, and each fails differently later if it is
     * absent -- ld.so with "rootfs looks corrupt", /bin/sh with every shebang
     * script dying, os-release with silent misidentification. */
    static const char *req[] = {
        "lib/ld-linux-aarch64.so.1",
        "bin/sh",
        "usr/bin/env",
        "etc/os-release",
        NULL
    };
    char p[ALR_PBUF];
    int i, missing = 0;

    for (i = 0; req[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", R, req[i]);
        if (access(p, F_OK) != 0) {
            fprintf(stderr, "alr: rootfs is missing %s\n", req[i]);
            missing++;
        }
    }
    return missing;
}

/* `alr list` -- docs/06-cli-spec.md §1, docs/05-provisioning-spec.md §6. */
static int cmd_list(void)
{
    /* Same resolution order as distro_root(): env, then paths.root from the
     * config, then the built-in.  `alr list` read only the environment, so
     * after `alr config set paths.root X` every other subcommand used X and
     * this one reported "no rootfs installed" about a different directory. */
    const char *root = getenv("ALR_ROOT_DIR");
    char dir[ALR_PBUF], p[ALR_PBUF];
    DIR *d;
    struct dirent *e;
    int n = 0;

    if (!root || !*root) root = cfg()->has_root ? cfg()->root : NULL;
    if (root && *root) snprintf(dir, sizeof dir, "%s", root);
    else snprintf(dir, sizeof dir, "%s/var/lib/alr/distros", prefix());

    if (!(d = opendir(dir))) {
        printf("no rootfs installed (%s does not exist)\n", dir);
        return 0;
    }
    printf("%-20s %-10s %s\n", "DISTRO", "PRELOAD", "PATH");
    while ((e = readdir(d))) {
        char so[ALR_PBUF];
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!is_dir(p)) continue;
        /* A directory here without a loader is a failed or partial install,
         * and saying so is more useful than listing it as if it worked. */
        snprintf(so, sizeof so, "%s/lib/ld-linux-aarch64.so.1", p);
        if (access(so, F_OK) != 0) {
            printf("%-20s %-10s %s\n", e->d_name, "-", "(no ld.so: incomplete)");
            n++; continue;
        }
        snprintf(so, sizeof so, "%s/usr/lib/alr/libalr_preload.so", p);
        printf("%-20s %-10s %s\n", e->d_name,
               access(so, R_OK) == 0 ? "yes" : "MISSING", p);
        n++;
    }
    closedir(d);
    if (!n) printf("(none)\n");
    return 0;
}

/* `alr remove <distro> [--force]`.
 *
 * The only subcommand that destroys user data, and the path it destroys is an
 * environment variable joined to a name -- see distro_name_ok().  Everything
 * here is about not deleting the wrong thing. */
static int cmd_remove(const char *distro, int force)
{
    char R[ALR_PBUF], so[ALR_PBUF];
    /* A distro name may be up to the filesystem's limit; 16 bytes here meant
     * fgets() truncated at 15 and strcmp() could never match a longer name, so
     * such a rootfs was undeletable without --force.  Size it to the same
     * bound the name itself has. */
    char ans[ALR_PBUF];

    if (!distro || !*distro) die("bad-distro-name", "remove needs a distro name");
    distro_root(R, sizeof R, distro);          /* validates the name */

    if (!is_dir(R)) die("rootfs-missing", "no such rootfs");

    /* Refuse anything that is not recognisably one of ours.  A user who points
     * ALR_ROOT_DIR at their home directory should get a refusal, not an empty
     * home directory. */
    snprintf(so, sizeof so, "%s/lib/ld-linux-aarch64.so.1", R);
    if (access(so, F_OK) != 0) {
        snprintf(so, sizeof so, "%s/usr/lib/alr", R);
        if (!is_dir(so))
            die("not-a-rootfs",
                "that directory has neither a guest ld.so nor usr/lib/alr; "
                "refusing to delete it");
    }

    if (!force) {
        fprintf(stderr, "alr: about to DELETE %s\n", R);
        fprintf(stderr, "     type the distro name to confirm: ");
        if (!fgets(ans, sizeof ans, stdin)) { fprintf(stderr, "\naborted\n"); return 1; }
        ans[strcspn(ans, "\n")] = '\0';
        if (strcmp(ans, distro) != 0) {
            fprintf(stderr, "alr: name did not match; nothing was deleted\n");
            return 1;
        }
    }

    {   char *rm[4];
        rm[0] = (char *)"rm"; rm[1] = (char *)"-rf"; rm[2] = R; rm[3] = NULL;
        if (run_cmd(rm) != 0) die("remove-failed", "rm -rf failed");
    }
    if (is_dir(R)) die("remove-failed", "the rootfs is still there after rm -rf");
    printf("alr: removed %s\n", R);
    return 0;
}


/* ── first-boot verification report (docs/05-provisioning-spec.md §4) ─────
 *
 * `alr install` used to check four files for existence and call it done.  That
 * catches a TRUNCATED tarball (measured: install reported success from a 2 MB
 * one) but nothing about whether the rootfs actually RUNS.  A rootfs can
 * extract perfectly and still be unusable -- a loader for the wrong arch, a
 * loader too old for the options ADR 0002 requires, a preload that fails to
 * load -- and every one of those looks like a clean install right up until the
 * user's first command fails with something unrelated-looking.
 *
 * So the last thing install does is boot the thing it just built, and say so
 * in nine lines.  Per §4, a failure LEAVES the rootfs in place and reports
 * clearly rather than deleting it: the user may want to look at it, and this
 * runs after the rename, so deleting would be the one code path that removes a
 * directory the user did not ask to remove.
 */
/* Did repair() actually produce the files it is responsible for?  Reporting
 * REPAIR: PASS because the function returned void would be reporting that it
 * was CALLED, which is not the same claim. */
static int access_ok_all(const char *R)
{
    static const char *req[] = {
        "/etc/resolv.conf", "/etc/hosts",
        "/etc/apt/apt.conf.d/99-alr-no-sandbox",
        "/etc/dpkg/dpkg.cfg.d/99-alr",
        "/usr/sbin/ldconfig",
        "/etc/passwd", "/etc/group",
        NULL
    };
    char p[ALR_PBUF];
    int i;
    for (i = 0; req[i]; i++) {
        snprintf(p, sizeof p, "%s%s", R, req[i]);
        if (access(p, F_OK) != 0) return 0;
    }
    return 1;
}

struct install_report {
    int  fetched;          /* 1 downloaded, 0 served from cache */
    int  sha;              /* 1 verified, 0 mismatch, -1 no digest to check */
    long members, special, setuid;
    int  repaired;
};

/* First line of a command's output, newline-stripped.  "" on failure. */
static void read_line_cmd(const char *cmd, char *out, size_t sz)
{
    FILE *fp = popen(cmd, "r");
    size_t n;
    out[0] = '\0';
    if (!fp) return;
    if (fgets(out, (int)sz, fp)) {
        n = strlen(out);
        while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    }
    pclose(fp);
}

static long read_long_cmd(const char *cmd, long dflt)
{
    char b[64];
    read_line_cmd(cmd, b, sizeof b);
    return *b ? strtol(b, NULL, 10) : dflt;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* One report line, and a running failure count. */
static void rline(int *bad, const char *name, int ok, const char *fmt, ...)
{
    va_list ap;
    printf("%-24s %s", name, ok ? "PASS" : "FAIL");
    if (fmt && *fmt) {
        printf("  ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    putchar('\n');
    if (!ok) (*bad)++;
}

static int install_verify(const char *distro, const char *R,
                          const struct install_report *rep)
{
    char cmd[ALR_PBUF * 2], line[ALR_PBUF], ld[ALR_PBUF];
    int bad = 0;
    long t0, ms;

    printf("\n");
    rline(&bad, "INSTALL DOWNLOAD:", 1, "%s",
          rep->fetched ? "fetched" : "cache hit");
    /* An unverified download is not a pass.  ubuntu-base is fetched over
     * HTTPS, but the fallback path when SHA256SUMS is unreachable has no
     * digest at all, and the report must not launder that into PASS. */
    if (rep->sha < 0)
        printf("%-24s %s  %s\n", "INSTALL VERIFY SHA256:", "SKIP",
               "no digest available (--url, or SHA256SUMS unreachable)");
    else
        rline(&bad, "INSTALL VERIFY SHA256:", rep->sha, "");
    rline(&bad, "INSTALL EXTRACT:", rep->members > 0,
          "files=%ld skipped_special=%ld setuid_masked=%ld",
          rep->members, rep->special, rep->setuid);
    rline(&bad, "INSTALL REPAIR:", rep->repaired, "");

    snprintf(ld, sizeof ld, "%s/lib/ld-linux-aarch64.so.1", R);
    rline(&bad, "INSTALL LDSO PRESENT:", access(ld, X_OK) == 0, "%s", ld);

    /* ADR 0002: we invoke the loader explicitly, so these four options are
     * load-bearing.  --argv0 arrived in glibc 2.33; without it argv[0] leaks
     * the host path and multicall binaries (busybox, uutils) misbehave. */
    {
        int argv0, preload, libpath, inhibit;
        snprintf(cmd, sizeof cmd, "'%s' --help 2>&1", ld);
        {
            FILE *fp = popen(cmd, "r");
            argv0 = preload = libpath = inhibit = 0;
            if (fp) {
                while (fgets(line, sizeof line, fp)) {
                    if (strstr(line, "--argv0"))         argv0   = 1;
                    if (strstr(line, "--preload"))       preload = 1;
                    if (strstr(line, "--library-path"))  libpath = 1;
                    if (strstr(line, "--inhibit-cache")) inhibit = 1;
                }
                pclose(fp);
            }
        }
        rline(&bad, "INSTALL LDSO OPTIONS:",
              argv0 && preload && libpath && inhibit,
              "argv0=%s preload=%s library-path=%s inhibit-cache=%s",
              argv0 ? "yes" : "NO", preload ? "yes" : "NO",
              libpath ? "yes" : "NO", inhibit ? "yes" : "NO");
        if (!argv0)
            fprintf(stderr, "alr: WARNING this loader has no --argv0 "
                            "(glibc < 2.33); argv0_leaks=true "
                            "(docs/01-platform-facts.md §C2)\n");
    }

    /* The actual boot.  Goes through `alr run` -- the same path a user takes,
     * preload and supervisor included -- because a check that bypassed them
     * would pass on a rootfs nobody can actually use. */
    snprintf(cmd, sizeof cmd,
             "ALR_ROOT_DIR='%s' '%s' -d '%s' run /bin/true >/dev/null 2>&1; echo $?",
             getenv("ALR_ROOT_DIR") ? getenv("ALR_ROOT_DIR") : "", g_self, distro);
    t0 = now_ms();
    {
        long code = read_long_cmd(cmd, -1);
        ms = now_ms() - t0;
        rline(&bad, "INSTALL BOOT /bin/true:", code == 0,
              "exit=%ld elapsed_ms=%ld", code, ms);
    }

    snprintf(cmd, sizeof cmd,
             "ALR_ROOT_DIR='%s' '%s' -d '%s' run /bin/echo alr 2>/dev/null",
             getenv("ALR_ROOT_DIR") ? getenv("ALR_ROOT_DIR") : "", g_self, distro);
    read_line_cmd(cmd, line, sizeof line);
    rline(&bad, "INSTALL BOOT /bin/echo:", strcmp(line, "alr") == 0,
          "stdout=\"%s\"", line);

    /* Not a pass/fail -- a recorded fact.  The guest's glibc version decides
     * which wrapper names exist (the __xstat family vanished in 2.33) and it
     * belongs in any bug report about this rootfs. */
    snprintf(cmd, sizeof cmd,
             "'%s' --version 2>&1 | sed -n '1s/.*version \\([0-9][0-9.]*[0-9]\\).*/\\1/p'", ld);
    read_line_cmd(cmd, line, sizeof line);
    printf("%-24s %s\n", "INSTALL GLIBC VERSION:", *line ? line : "(unknown)");

    return bad;
}

/* `--with git,node,codex`, for both a fresh rootfs and an existing one.
 *
 * Returns the number of components that failed.  It used to print a line per
 * failure and return nothing, so `alr install --with git` reported success
 * having installed no git -- and `&&` reads the exit status, not the line. */
static int with_components(const char *distro, const char *R, const char *cache)
{
    int bad = 0;

    /* apt must run before the HTTPS fetches: ubuntu-base ships no
     * ca-certificates, and nodejs.org / GitHub are both HTTPS. */
    if (strstr(g_with, "git") || strstr(g_with, "node") || strstr(g_with, "codex")) {
        static const char *upd[] = { "/usr/bin/apt-get", "update", NULL };
        static const char *ins[] = { "/usr/bin/apt-get", "install", "-y",
                                     "--no-install-recommends",
                                     "ca-certificates", "xz-utils", NULL };
        guest_run(distro, upd, 1);
        guest_run(distro, ins, 1);
    }
    if (strstr(g_with, "git")) {
        static const char *g[] = { "/usr/bin/apt-get", "install", "-y",
                                   "--no-install-recommends", "git", NULL };
        if (guest_run(distro, g, 1) != 0) {
            fprintf(stderr, "alr: git install failed\n"); bad++;
        }
    }
    if (strstr(g_with, "node")  && with_node(R, cache) != 0) {
        fprintf(stderr, "alr: node install failed\n"); bad++;
    }
    if (strstr(g_with, "codex") && with_codex(R, cache) != 0) {
        fprintf(stderr, "alr: codex install failed\n"); bad++;
    }
    return bad;
}

/* Remove the staging directory and then fail.
 *
 * `<R>.part` used to be left behind by every failure after extraction --
 * traversal reject, missing preload, rootfs-incomplete, a failed rename.  It
 * is not inert: the next `alr install` mkdir -p's the same path and untars
 * ONTO it, so files from a REJECTED archive -- including one rejected for
 * carrying symlinks out of the rootfs -- survive into a subsequent install
 * that then reports every check green.  A staging directory that outlives the
 * attempt it staged is not staging.
 *
 * Best-effort by design: if the removal fails there is nothing better to do
 * than say so and still fail the install, which is what the caller does. */
static void die_staged(const char *part, const char *reason, const char *msg)
{
    char *rm[4];
    rm[0] = (char *)"rm"; rm[1] = (char *)"-rf";
    rm[2] = (char *)part; rm[3] = NULL;
    if (run_cmd(rm) != 0)
        fprintf(stderr, "alr: WARNING could not remove the staging directory "
                        "%s; remove it before installing again\n", part);
    die(reason, msg);
}

static int cmd_install(const char *distro, const char *url_override)
{
    char R[ALR_PBUF], part[ALR_PBUF], tarball[ALR_PBUF], cache[ALR_PBUF];
    char durl[512], dsha[80];
    char *dl[8], *ex[12], *mk[4];
    const char *url = url_override;
    struct install_report rep;
    int rc;

    memset(&rep, 0, sizeof rep);
    rep.sha = -1;                       /* nothing to check against, until there is */
    durl[0] = dsha[0] = '\0';
    distro_root(R, sizeof R, distro);
    snprintf(part,  sizeof part,  "%s.part", R);
    snprintf(cache, sizeof cache, "%s/var/lib/alr/cache", prefix());
    snprintf(tarball, sizeof tarball, "%s/%s.tar.gz", cache, distro);

    if (is_dir(R) && g_force) {
        /* --force means "provision this again", so remove first rather than
         * bailing out.  Goes through cmd_remove so it inherits the same
         * refusals -- a --force that skipped them would be the one path that
         * can delete an unrecognised directory. */
        int rc2 = cmd_remove(distro, 1);
        if (rc2 != 0) return rc2;
    }
    if (is_dir(R)) {
        /* Used to return 0 and silently drop --with, so
         * `alr install -d x --with node && alr run node` failed confusingly.
         * Say what was ignored and what to do instead. */
        fprintf(stderr, "alr: %s already installed\n", R);
        fprintf(stderr,
            "  (to refresh the guest preload after upgrading alr:"
            " `alr update-components`)\n");
        if (!g_with) return 0;
        /* --with on an existing rootfs used to print a NOTE and exit 0.  Exit 0
         * is what `&&` reads, so
         *     alr install --with git && alr run git status
         * ran the second command against a rootfs with no git and failed with
         * boot-enoent -- the NOTE having scrolled past.  The user asked for
         * components; either deliver them or fail. */
        fprintf(stderr,
            "alr: installing --with %s into the existing rootfs\n", g_with);
        rc = with_components(distro, R, cache);
        if (rc != 0)
            die("with-failed",
                "the rootfs is intact but the requested components did not "
                "all install");
        return 0;
    }

    mk[0] = (char *)"mkdir"; mk[1] = (char *)"-p"; mk[2] = cache; mk[3] = NULL;
    run_cmd(mk);
    mk[2] = part; run_cmd(mk);

    if (!url) {
        if (strncmp(distro, "ubuntu-", 7) != 0)
            die("unsupported-distro",
                "no discovery path for this distro yet; pass --url "
                "(docs/05-provisioning-spec.md §1.1)");
        /* v1 targets 24.04 only (docs/00-product.md §2).  26.04 installs and
         * boots, but its coreutils is the uutils multicall binary, which
         * resolves its own name in a way a correct argv[0] does not satisfy
         * under an explicit loader (ADR 0002) -- every coreutils tool is
         * unusable there.  Say so at install time rather than letting the
         * rootfs look fine until the first `ls`. */
        if (strcmp(distro, "ubuntu-24.04") != 0)
            fprintf(stderr,
                "alr: WARNING %s is not a v1 target.\n"
                "     Ubuntu 26.04 replaced GNU coreutils with uutils, a Rust\n"
                "     multicall binary that issues raw syscalls inline instead of\n"
                "     calling libc.  LD_PRELOAD cannot see those, so ls/cat/echo\n"
                "     and the rest of coreutils CANNOT work here -- this is the\n"
                "     same limit documented for Go binaries, not a bug to fix.\n"
                "     Everything else (bash, apt, dpkg, grep, sed, awk, tar) does\n"
                "     work.  See docs/RISKS.md.\n", distro);
        printf("alr: resolving the current ubuntu-base image for %s\n", distro + 7);
        if (discover_ubuntu(distro + 7, durl, sizeof durl, dsha, sizeof dsha) != 0) {
            /* Offline / air-gapped fallback.  Say plainly that the download is
             * unverified rather than implying the pin is as good as a hash. */
            snprintf(durl, sizeof durl, "%s/24.04/release/%s",
                     cfg()->has_mirror ? cfg()->mirror : ALR_UBUNTU_CDIMAGE,
                     ALR_UBUNTU_PIN);
            dsha[0] = '\0';
            fprintf(stderr, "alr: WARNING SHA256SUMS unreachable; falling back to "
                            "the pinned %s WITHOUT hash verification\n",
                    ALR_UBUNTU_PIN);
        }
        url = durl;
        printf("alr: %s\n", url);
    }

    {   int attempt;
        for (attempt = 0; attempt < 2; attempt++) {
            if (access(tarball, R_OK) != 0) {
                printf("alr: fetching %s\n", url);
                dl[0] = (char *)"curl"; dl[1] = (char *)"-fSL"; dl[2] = (char *)"--retry";
                dl[3] = (char *)"3"; dl[4] = (char *)"-o"; dl[5] = tarball;
                dl[6] = (char *)url; dl[7] = NULL;
                if (run_cmd(dl) != 0) die("download-network", "download failed");
                rep.fetched = 1;
            }
            if (!*dsha) break;                     /* --url: nothing to check against */
            if (verify_sha256(tarball, dsha) == 0) {
                printf("alr: sha256 ok\n"); rep.sha = 1; break;
            }
            rep.sha = 0;
            /* A cached tarball from an older point release is the common case,
             * so retry once with a fresh download before calling it corrupt. */
            fprintf(stderr, "alr: cached tarball does not match SHA256SUMS; refetching\n");
            unlink(tarball);
            if (attempt) die("download-corrupt",
                             "SHA256 mismatch after refetch; the tarball was removed");
        }
    }

    /* Safe extraction (docs/05-provisioning-spec.md §2).  As a non-root user
     * GNU tar already refuses to chown and cannot create device nodes; we add
     * the traversal and setuid guards explicitly rather than relying on that.
     * A hardened in-process untar is still owed -- tracked in the spec. */
    printf("alr: extracting into %s\n", part);
    ex[0]  = (char *)"tar";
    ex[1]  = (char *)"-xzf";        ex[2] = tarball;
    ex[3]  = (char *)"-C";          ex[4] = part;
    ex[5]  = (char *)"--no-same-owner";
    /* NOT --no-same-permissions: that makes tar apply the caller's umask, and
     * Termux's is 077, so every extracted file lands as 0700.  Programs that
     * compute executability themselves rather than calling access() then
     * decide nothing is runnable -- dpkg reports
     *   "'sh' not found in PATH or not executable"
     * for files that plainly exist and are executable by their owner.
     * We set umask 022 around the extraction instead (see below). */
    ex[6]  = (char *)"--no-overwrite-dir";
    ex[7]  = (char *)"--exclude=dev/*";
    ex[8]  = (char *)"--warning=no-unknown-keyword";
    ex[9]  = NULL;
    {
        mode_t old = umask(022);
        rc = run_cmd(ex);
        umask(old);
    }
    if (rc != 0) fprintf(stderr, "alr: tar exited %d (continuing; some members "
                                 "are expected to be skipped)\n", rc);

    if (fix_hardlinks(tarball, part, &rep.members, &rep.special) < 0)
        fprintf(stderr, "alr: hardlink fixup incomplete\n");

    {   int fixed = 0, failed = 0, escaped = 0;
        relativize_tree(part, 0, &fixed, &failed, &escaped);
        printf("alr: absolute symlinks: %d relativized, %d failed\n", fixed, failed);
        if (failed) fprintf(stderr, "alr: WARNING %d symlink(s) left absolute; "
                                    "they will not resolve inside the guest\n", failed);
        if (escaped)
            die_staged(part, "extract-traversal-reject",
                "the archive contains symlinks pointing outside the rootfs; "
                "nothing was installed and the staging directory was removed");
    }

    {   /* setuid/setgid bits are inert on /data (nosuid) and only produce
         * confusing warnings later -- mask them off.  Counted first: the
         * install report states how many were masked, and "0" from a find that
         * matched nothing looks the same as "0" from a find that failed. */
        char cmdbuf[ALR_PBUF * 2];
        char *sh[4];
        snprintf(cmdbuf, sizeof cmdbuf,
                 "find '%s' -type f -perm /6000 2>/dev/null | wc -l", part);
        rep.setuid = read_long_cmd(cmdbuf, -1);
        snprintf(cmdbuf, sizeof cmdbuf,
                 "find '%s' -type f -perm /6000 -exec chmod a-s {} + 2>/dev/null; exit 0",
                 part);
        sh[0] = (char *)"sh"; sh[1] = (char *)"-c"; sh[2] = cmdbuf; sh[3] = NULL;
        run_cmd(sh);
    }

    repair(part);
    rep.repaired = access_ok_all(part);
    /* NOT ignorable.  This used to discard the return value, so a rootfs with
     * NO path virtualization was reported as a successful install: the warning
     * scrolled past, cmd_install returned 0, and `alr run` then booted it
     * anyway because prepare() treats a missing preload as "omit --preload"
     * rather than as a failure.  The guest would see the ANDROID filesystem
     * while every message said the install worked.  reason=preload-install-failed
     * already exists (cmd_update_components emits it). */
    /* Host-side wrappers for static guests.  Written before the rename, like
     * everything else that belongs to the rootfs. */
    install_hostbin(distro, part);

    if (install_preload(part) != 0)
        die_staged(part, "preload-install-failed",
            "the rootfs would run WITHOUT path virtualization");

    /* Check BEFORE the rename: a rootfs that fails this must never appear at
     * the real path, where the next `alr run` would pick it up. */
    if (verify_rootfs(part) != 0)
        die_staged(part, "rootfs-incomplete",
            "extraction did not produce a usable rootfs (truncated or "
            "corrupt tarball?); nothing was installed");

    if (rename(part, R) != 0)
        die_staged(part, "extract-permission", "rename into place failed");
    printf("alr: installed %s\n", R);

    divert_ldconfig(distro, R);

    if (install_verify(distro, R, &rep) != 0)
        die("rootfs-unbootable",
            "the rootfs extracted but does not boot; it was LEFT IN PLACE for "
            "inspection (docs/05-provisioning-spec.md §4)");

    if (g_with && *g_with && with_components(distro, R, cache) != 0)
        die("with-failed",
            "the rootfs installed but the requested components did not all "
            "install");
    return 0;
}


/* ── `alr config` (docs/06-cli-spec.md §1, §2) ───────────────────────────
 *
 *   alr config            every setting, its effective value, and WHERE it
 *                         came from
 *   alr config get <key>  one effective value, bare, for scripts
 *   alr config set <k> <v>  write it to the user file
 *
 * The source column is the part that earns its keep.  "fakeroot is true" is
 * not actionable; "fakeroot is true, from the environment" tells you which of
 * four places to edit, and it is the question anyone reading a config dump is
 * actually asking.
 */
static const char *CFG_KEYS[] = {
    "default_distro", "runtime.fakeroot", "runtime.log",
    "paths.root", "network.mirror", NULL
};

/* Effective value of one key plus its provenance.  Mirrors exactly what the
 * runtime does -- if these two ever disagree, the dump is a lie, so each arm
 * below reads the same environment variable the runtime reads. */
static void cfg_effective(const char *key, char *val, size_t vn,
                          const char **src)
{
    const struct alr_config *c = cfg();
    const char *e;

    *src = "default";
    if (!strcmp(key, "default_distro")) {
        if ((e = getenv("ALR_DISTRO")) && *e) { snprintf(val, vn, "%s", e); *src = "env ALR_DISTRO"; return; }
        if (c->has_distro) { snprintf(val, vn, "%s", c->distro); *src = "config"; return; }
        snprintf(val, vn, "ubuntu-24.04"); return;
    }
    if (!strcmp(key, "runtime.fakeroot")) {
        if ((e = getenv("ALR_FAKEROOT")) && *e) { snprintf(val, vn, "%s", *e == '1' ? "true" : "false"); *src = "env ALR_FAKEROOT"; return; }
        if (c->has_fakeroot) { snprintf(val, vn, "%s", c->fakeroot ? "true" : "false"); *src = "config"; return; }
        snprintf(val, vn, "false"); return;
    }
    if (!strcmp(key, "runtime.log")) {
        if ((e = getenv("ALR_LOG")) && *e) { snprintf(val, vn, "%s", e); *src = "env ALR_LOG"; return; }
        if (c->has_log) { snprintf(val, vn, "%d", c->log); *src = "config"; return; }
        snprintf(val, vn, "0"); return;
    }
    if (!strcmp(key, "paths.root")) {
        if ((e = getenv("ALR_ROOT_DIR")) && *e) { snprintf(val, vn, "%s", e); *src = "env ALR_ROOT_DIR"; return; }
        if (c->has_root) { snprintf(val, vn, "%s", c->root); *src = "config"; return; }
        snprintf(val, vn, "%s/var/lib/alr/distros", prefix()); return;
    }
    if (!strcmp(key, "network.mirror")) {
        if (c->has_mirror) { snprintf(val, vn, "%s", c->mirror); *src = "config"; return; }
        snprintf(val, vn, "%s", ALR_UBUNTU_CDIMAGE); return;
    }
    snprintf(val, vn, "");
}

static int cfg_known(const char *key)
{
    int i;
    for (i = 0; CFG_KEYS[i]; i++) if (!strcmp(CFG_KEYS[i], key)) return 1;
    return 0;
}

/* Rewrite the user file from the parsed struct.
 *
 * This does NOT preserve comments in that file -- the schema is closed and
 * round-tripping comments means keeping a full document model for five keys.
 * Said plainly here and in docs/06 §2 rather than discovered by someone whose
 * annotations vanished. */
static int cfg_write_user(const struct alr_config *c)
{
    char path[ALR_PBUF], dir[ALR_PBUF], tmp[ALR_PBUF];
    char *slash;
    FILE *fp;

    cfg_path(path, sizeof path, 1);
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) { *slash = '\0'; mkdir(dir, 0755); }
    snprintf(tmp, sizeof tmp, "%s.alr-tmp", path);
    if (!(fp = fopen(tmp, "w"))) return -1;
    fputs("# written by `alr config set` -- comments are not preserved\n", fp);
    if (c->has_distro) fprintf(fp, "default_distro = \"%s\"\n", c->distro);
    if (c->has_fakeroot || c->has_log) {
        fputs("\n[runtime]\n", fp);
        if (c->has_fakeroot) fprintf(fp, "fakeroot = %s\n", c->fakeroot ? "true" : "false");
        if (c->has_log)      fprintf(fp, "log = %d\n", c->log);
    }
    if (c->has_root)   fprintf(fp, "\n[paths]\nroot = \"%s\"\n", c->root);
    if (c->has_mirror) fprintf(fp, "\n[network]\nmirror = \"%s\"\n", c->mirror);
    if (fclose(fp) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    printf("alr: wrote %s\n", path);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    char val[ALR_PBUF];
    const char *src;
    int i;

    if (argc == 0) {
        char gp[ALR_PBUF], up[ALR_PBUF];
        cfg_path(gp, sizeof gp, 0);
        cfg_path(up, sizeof up, 1);
        printf("  global  %s%s\n", gp, access(gp, R_OK) == 0 ? "" : "   (absent)");
        printf("  user    %s%s\n\n", up, access(up, R_OK) == 0 ? "" : "   (absent)");
        for (i = 0; CFG_KEYS[i]; i++) {
            cfg_effective(CFG_KEYS[i], val, sizeof val, &src);
            printf("  %-18s %-44s %s\n", CFG_KEYS[i], val, src);
        }
        if (cfg()->bad)
            fprintf(stderr, "\nalr: %d unknown setting(s) were ignored\n", cfg()->bad);
        return cfg()->bad ? 1 : 0;
    }

    if (!strcmp(argv[0], "get")) {
        if (argc < 2) die("bad-option", "usage: alr config get <key>");
        if (!cfg_known(argv[1])) die("config-unknown-key", "no such setting");
        cfg_effective(argv[1], val, sizeof val, &src);
        printf("%s\n", val);
        return 0;
    }

    if (!strcmp(argv[0], "set")) {
        struct alr_config u;
        char path[ALR_PBUF], v[ALR_PBUF];
        if (argc < 3) die("bad-option", "usage: alr config set <key> <value>");
        if (!cfg_known(argv[1])) die("config-unknown-key", "no such setting");
        /* Edit the USER file only -- never the global one, which may be
         * shared and is not this command's to rewrite.  Re-read it fresh
         * rather than reusing cfg(), which is the two files MERGED: writing
         * that back would silently copy every global setting into the user
         * file and freeze it there. */
        memset(&u, 0, sizeof u);
        cfg_path(path, sizeof path, 1);
        cfg_read(&u, path);
        snprintf(v, sizeof v, "%s", argv[2]);
        if (cfg_apply(&u, argv[1], v) != 0)
            die("config-bad-value", "the value is not valid for this setting");
        if (cfg_write_user(&u) != 0)
            die("config-write-failed", "could not write the user config file");
        /* cfg() memoises, and main() already called it to resolve the default
         * distro -- so without this the confirmation line reports the value
         * from BEFORE the write.  It printed "runtime.fakeroot = false" one
         * line after writing true. */
        memset(&g_cfg, 0, sizeof g_cfg);
        g_cfg_loaded = 0;
        cfg_effective(argv[1], val, sizeof val, &src);
        printf("%s = %s   (%s)\n", argv[1], val, src);
        /* Only an ENVIRONMENT variable can outrank what we just wrote.  The
         * built-in default cannot -- saying so would be false, and it is what
         * the first version of this said whenever the written value happened
         * to equal the default. */
        if (strncmp(src, "env ", 4) == 0)
            fprintf(stderr, "alr: NOTE the effective value still comes from "
                            "%s, which outranks the config file\n", src);
        return 0;
    }

    die("bad-option", "usage: alr config [get <key> | set <key> <value>]");
    return 1;
}

/* ── launch ──────────────────────────────────────────────────────────── */

struct launch {
    char root[ALR_PBUF];
    size_t root_len;
    char ldso[ALR_PBUF];
    char libpath[ALR_PBUF * 2];
    char preload[ALR_PBUF];
    int  have_preload;
};

static int prepare(struct launch *L, const char *distro)
{
    static const char *libdirs[] = {
        "/lib/aarch64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
        "/lib", "/usr/lib", "/usr/local/lib/aarch64-linux-gnu", NULL
    };
    size_t o = 0;
    int i;

    distro_root(L->root, sizeof L->root, distro);
    L->root_len = alr_trim_root(L->root);
    if (!is_dir(L->root)) { errno = ENOENT; return -1; }

    snprintf(L->ldso, sizeof L->ldso, "%s/lib/ld-linux-aarch64.so.1", L->root);
    if (access(L->ldso, X_OK) != 0) return -2;

    /* ld.so's own library search is the one uninterposable component (it runs
     * before any preload), so we solve it declaratively instead of hooking it. */
    for (i = 0; libdirs[i]; i++)
        o += (size_t)snprintf(L->libpath + o, sizeof L->libpath - o, "%s%s%s",
                              o ? ":" : "", L->root, libdirs[i]);

    snprintf(L->preload, sizeof L->preload,
             "%s/usr/lib/alr/libalr_preload.so", L->root);
    L->have_preload = (access(L->preload, R_OK) == 0);
    return 0;
}

/* Resolve a guest command to a host path, searching the guest PATH when the
 * command has no slash. */
static int resolve(const struct launch *L, const char *cmd, char *out, size_t n)
{
    static const char *dflt =
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const char *path = getenv("ALR_GUEST_PATH");
    const char *p;

    if (strchr(cmd, '/')) {
        char buf[ALR_PBUF];
        const char *h = alr_rw(cmd, L->root, L->root_len, buf, sizeof buf, NULL);
        if (!h) return -1;
        snprintf(out, n, "%s", h);
        return access(out, X_OK) == 0 ? 0 : -1;
    }
    if (!path || !*path) path = dflt;
    for (p = path; *p; ) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) {
            snprintf(out, n, "%.*s%.*s/%s", (int)L->root_len, L->root,
                     (int)len, p, cmd);
            if (access(out, X_OK) == 0) return 0;
        }
        if (!e) break;
        p = e + 1;
    }
    return -1;
}

/* ── run/shell/exec common options (docs/06-cli-spec.md §1.1) ─────────
 *
 * These were specified from the start and never implemented; the parser
 * accepted only -d/-v/--url.  Two of them are load-bearing elsewhere: -e is
 * named in docs/02-architecture §6.3 as part of the guest env contract, and -w
 * is what makes §1.2's cwd mapping controllable.
 */
#define ALR_MAX_USER_ENV 32

struct runopts {
    const char *workdir;                    /* -w: guest path, NULL = auto  */
    const char *envs[ALR_MAX_USER_ENV];     /* -e KEY=VAL                   */
    int         nenv;
    int         fakeroot;                   /* -1 unset, 0/1 forced         */
    int         no_supervisor;
};

static void runopts_init(struct runopts *ro)
{
    memset(ro, 0, sizeof *ro);
    ro->fakeroot = -1;
}

/* Keys alr owns.  A user -e must not silently replace one: the loader
 * invocation, the resolver bridge and the preload all read these, and a
 * shadowed ALR_ROOT produces a guest that half-works in ways that look like
 * our bug.  Refuse loudly instead. */
static int env_is_reserved(const char *kv)
{
    return !strncmp(kv, "ALR_", 4)
        || !strncmp(kv, "LD_PRELOAD=", 11)
        || !strncmp(kv, "LD_LIBRARY_PATH=", 16)
        /* Also load-bearing: LOCPATH is how the guest finds its only generated
         * locale (without it tmux refuses to start), and GLIBC_TUNABLES is how
         * rseq is disabled before ld.so would trip the zygote filter. */
        || !strncmp(kv, "LOCPATH=", 8)
        || !strncmp(kv, "GLIBC_TUNABLES=", 15);
}

/* Parse the common options in place.  Returns the index of the first
 * non-option argument.  `stop_at_ddash` is for `exec`, where -- ends options
 * unambiguously (§1: "run과 동일하나 옵션 파싱 모호성 없음"). */
static int parse_runopts(int argc, char **argv, struct runopts *ro,
                         const char **distro, int stop_at_ddash)
{
    int i = 0;
    while (i < argc) {
        const char *a = argv[i];
        if (stop_at_ddash && !strcmp(a, "--")) return i + 1;
        if (a[0] != '-' || a[1] == '\0') break;
        if ((!strcmp(a, "-d") || !strcmp(a, "--distro")) && i + 1 < argc) {
            *distro = argv[++i]; i++;
        } else if ((!strcmp(a, "-w") || !strcmp(a, "--workdir")) && i + 1 < argc) {
            ro->workdir = argv[++i]; i++;
        } else if ((!strcmp(a, "-e") || !strcmp(a, "--env")) && i + 1 < argc) {
            const char *kv = argv[++i]; i++;
            if (!strchr(kv, '='))
                die("bad-env", "-e needs KEY=VAL");
            if (env_is_reserved(kv))
                die("env-reserved",
                    "-e cannot set ALR_*, LD_PRELOAD, LD_LIBRARY_PATH, LOCPATH "
                    "or GLIBC_TUNABLES; alr owns those");
            if (ro->nenv >= ALR_MAX_USER_ENV)
                die("too-many-env", "too many -e options");
            ro->envs[ro->nenv++] = kv;
        } else if (!strcmp(a, "--fakeroot"))      { ro->fakeroot = 1; i++; }
        else if (!strcmp(a, "--no-fakeroot"))     { ro->fakeroot = 0; i++; }
        else if (!strcmp(a, "--no-supervisor"))   { ro->no_supervisor = 1; i++; }
        else if (!strcmp(a, "--log") && i + 1 < argc) { g_log = atoi(argv[++i]); i++; }
        else if (!strcmp(a, "-v"))                { g_log++; i++; }
        else die("bad-option", a);
    }
    return i;
}

static char **build_env(const struct launch *L, const char *guest_exe,
                        int target_static,
                        const char *guest_argv0, const struct runopts *ro,
                        const char *guest_cwd)
{
    static char *env[ALR_MAX_ENVP];
    static char bufs[24][ALR_PBUF];
    int n = 0, b = 0, i;
    extern char **environ;

    /* Carry through only what is safe; drop the Android/Termux host set. */
    for (i = 0; environ[i] && n < ALR_MAX_ENVP - 24; i++) {
        const char *val;
        size_t klen = alr_split_env(environ[i], &val);
        if (alr_env_is_blocked(environ[i])) continue;
        /* LD_PRELOAD / LD_LIBRARY_PATH are replaced, never inherited: Termux's
         * value points at a BIONIC .so which a glibc ld.so cannot load, and an
         * empty string is a real bug on the bionic linker, not a no-op. */
        if (klen == 10 && !memcmp(environ[i], "LD_PRELOAD", 10)) continue;
        if (klen == 15 && !memcmp(environ[i], "LD_LIBRARY_PATH", 15)) continue;
        if (klen == 14 && !memcmp(environ[i], "GLIBC_TUNABLES", 14)) continue;
        /* Anything we are about to PUT() below must be dropped here first.
         * envp allows duplicate keys and getenv(3) returns the FIRST match, so
         * an inherited PATH would shadow the guest one for every C program --
         * while bash still showed the right value, because bash builds its own
         * table where the last assignment wins.  That divergence is what made
         * dpkg report "'sh' not found in PATH or not executable" for files that
         * plainly existed: it was searching Termux's $PREFIX/bin. */
        if (klen ==  4 && !memcmp(environ[i], "PATH", 4)) continue;
        if (klen ==  4 && !memcmp(environ[i], "HOME", 4)) continue;
        if (klen ==  6 && !memcmp(environ[i], "TMPDIR", 6)) continue;
        /* The host PWD is a Termux path and is stale the moment we chdir into
         * the rootfs.  It is not cosmetic: POSIX pwd and many shells trust
         * $PWD when it names the current directory, so leaving the host value
         * in place makes the guest disagree with itself. */
        if (klen ==  3 && !memcmp(environ[i], "PWD", 3)) continue;
        /* alr_split_env returns the KEY length: 6 for "OLDPWD=...".  This
         * read `klen == 7` and so never fired, and the neighbouring PWD line
         * being correct is what made it invisible.  A leaked OLDPWD is a
         * Termux absolute path, so `cd -` in the guest goes to
         * <R>/data/data/... and fails with a bare ENOENT that reads as a
         * corrupt rootfs. */
        if (klen ==  6 && !memcmp(environ[i], "OLDPWD", 6)) continue;
        if (klen ==  8 && !memcmp(environ[i], "ALR_ROOT", 8)) continue;
        /* The guest rootfs has only C.UTF-8 generated; an inherited
         * LANG=en_US.UTF-8 makes every perl/dpkg invocation emit a locale
         * warning block that buries the real output. */
        if (klen ==  4 && !memcmp(environ[i], "LANG", 4)) continue;
        if (klen >=  3 && !memcmp(environ[i], "LC_", 3)) continue;
        /* A user -e must WIN over the inherited value.  envp allows duplicate
         * keys and getenv(3) returns the FIRST match, and this loop runs before
         * the PUT()s below -- so an inherited key would shadow the -e one.  The
         * same trap already cost this project a day over PATH (see above). */
        if (ro) {
            int u, shadowed = 0;
            for (u = 0; u < ro->nenv; u++) {
                const char *uv; size_t ulen = alr_split_env(ro->envs[u], &uv);
                if (ulen == klen && !memcmp(environ[i], ro->envs[u], klen)) {
                    shadowed = 1; break;
                }
            }
            if (shadowed) continue;
        }
        env[n++] = environ[i];
    }

/* True when a user -e already provides this key. */
#define USER_SET(entry) ({                                                    \
        int _s = 0;                                                           \
        if (ro) { const char *_v; size_t _k = alr_split_env((entry), &_v);     \
            int _u; for (_u = 0; _u < ro->nenv; _u++) {                       \
                const char *_uv; size_t _ul = alr_split_env(ro->envs[_u], &_uv); \
                if (_ul == _k && !memcmp((entry), ro->envs[_u], _k)) { _s = 1; break; } } } \
        _s; })

/* Skip anything the user overrode with -e.
 *
 * Emitting both is not harmless: envp allows duplicate keys, getenv(3) takes
 * the FIRST and bash's import loop takes the LAST, so `-e PATH=/x` gave C
 * programs /x and shells the default -- MEASURED, two PATH entries in envp and
 * `sh -c 'echo $PATH'` printing alr's value.  That is verbatim the divergence
 * the comment above memorializes as having cost a day on dpkg. */
#define PUT(fmt, ...) do { \
        snprintf(bufs[b], ALR_PBUF, fmt, __VA_ARGS__); \
        if (!USER_SET(bufs[b])) env[n++] = bufs[b++]; \
    } while (0)

    /* User -e first: these are plain pointers into argv, no buffer needed, and
     * putting them ahead of everything makes getenv(3) find them first. */
    if (ro) { int u; for (u = 0; u < ro->nenv && n < ALR_MAX_ENVP - 24; u++)
                  env[n++] = (char *)ro->envs[u]; }

    PUT("ALR_ROOT=%s", L->root);
    /* The preload needs these to re-dispatch the guest's own exec* calls
     * through the same loader invocation we used for the first program. */
    PUT("ALR_LDSO=%s", L->ldso);
    PUT("ALR_LIBPATH=%s", L->libpath);
    /* IN ADDITION to --library-path, not instead of it.  --library-path
     * populates the loader's search list for the INITIAL link; glibc's NSS
     * loads libnss_dns.so.2 later via __libc_dlopen_mode, and without
     * LD_LIBRARY_PATH that dlopen searches the default dirs against the REAL
     * host root, finds nothing, and every hostname lookup fails with
     * "Temporary failure in name resolution" while raw IP sockets still work.
     * Measured 2026-08-02: nss_dns load count was 0 before this line.
     * The value must be HOST paths -- the loader's own opens happen before the
     * preload can rewrite anything. */
    if (!target_static) PUT("LD_LIBRARY_PATH=%s", L->libpath);
    if (L->have_preload) PUT("ALR_PRELOAD=%s", L->preload);
    PUT("ALR_GUEST_EXE=%s", guest_exe);
    PUT("ALR_GUEST_ARGV0=%s", guest_argv0);
    /* rseq is blocked by the Android filter and glibc registers it at startup;
     * this tunable removes the syscall entirely, one less SIGSYS per process.
     * It is only a partial fix -- set_robust_list has no equivalent knob, which
     * is exactly why the supervisor exists. */
    PUT("GLIBC_TUNABLES=%s", "glibc.pthread.rseq=0");
    /* ── static targets get a HOST-form environment ──────────────────────
     *
     * A static binary has no PT_INTERP, so LD_PRELOAD is never loaded and NONE
     * of its paths are rewritten (ADR 0008).  Handing it the guest's view is
     * therefore not merely useless, it is wrong twice over:
     *
     *   - its own paths resolve against Android.  MEASURED: codex with
     *     HOME=/root reported "could not create PATH aliases: Read-only file
     *     system" -- it was writing to ANDROID's /root.  With HOME set to the
     *     host path of the same directory the warning disappears and its
     *     config, auth and state land in the rootfs, where `codex doctor`
     *     reports "config loaded / state inspectable".
     *
     *   - LD_PRELOAD and LD_LIBRARY_PATH are INHERITED BY EVERY CHILD IT
     *     SPAWNS.  Those children are Android binaries (a static agent can
     *     only reach the guest through host-side wrappers), and bionic's
     *     linker refuses a glibc object:
     *         CANNOT LINK EXECUTABLE "$PREFIX/bin/bash":
     *           library "libc.so.6" not found: needed by libalr_preload.so
     *     so every process it tried to launch died before main().  MEASURED:
     *     with these two removed, codex found and ran the guest's ripgrep --
     *     "search  ripgrep 14.1.0 (system, `rg`)".
     *
     * So the rule is the exact dual of the dynamic case: guest paths for a
     * binary we can rewrite, host paths for one we cannot. */
    if (target_static) {
        PUT("HOME=%s/root", L->root);
        PUT("TMPDIR=%s/tmp", L->root);
        /* hostbin first: host-side wrappers that re-enter `alr run`, which is
         * the only way a static binary can reach a guest program at all --
         * exec'ing one directly fails, its PT_INTERP names a loader Android
         * does not have.  $PREFIX/bin after it so Termux's own tools remain
         * reachable. */
        PUT("PATH=%s/usr/lib/alr/hostbin:%s/bin", L->root, prefix());
        if (guest_cwd) {
            char hc[ALR_PBUF];
            if (!strcmp(guest_cwd, "/")) snprintf(hc, sizeof hc, "%s", L->root);
            else snprintf(hc, sizeof hc, "%s%s", L->root, guest_cwd);
            PUT("PWD=%s", hc);
        }
    } else {
    PUT("HOME=%s", "/root");
    /* The guest view of the working directory (docs/06-cli-spec.md §1.2).
     *
     * Needed beyond the chdir because coreutils' pwd does NOT call our
     * getcwd: gnulib reimplements it by walking ".." with openat/readdir, and
     * that walk runs on the real tree and reconstructs the HOST path.
     * MEASURED: `alr run /bin/pwd` printed <R>/usr/lib while python3's
     * os.getcwd() and bash's `pwd -P` -- both of which do call getcwd --
     * printed /usr/lib.  POSIX pwd prefers $PWD when it names the current
     * directory, so setting it correctly is what makes those agree. */
    if (guest_cwd) PUT("PWD=%s", guest_cwd);
    PUT("TMPDIR=%s", "/tmp");
    PUT("PATH=%s", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    }
    /* ubuntu-base DOES ship /usr/lib/locale/C.utf8 -- the earlier note that it
     * ships no locales was wrong.  glibc simply never found it: _nl_find_locale
     * opens the LITERAL "/usr/lib/locale" through an internal call we cannot
     * interpose, and the HOST has no such directory at all, so the guest was
     * left with only C and POSIX.  LOCPATH is glibc's own escape hatch and it
     * points the lookup at the rootfs's copy.
     *
     * Without this, tmux (and anything else that insists on UTF-8) refuses to
     * start: "need UTF-8 locale (LC_CTYPE) but have ANSI_X3.4-1968".
     *
     * Caveat: when LOCPATH is set glibc skips locale-archive entirely, so
     * locales added later must be generated as directories --
     * `locale-gen --no-archive`.  See docs/RISKS.md. */
    PUT("LOCPATH=%s/usr/lib/locale", L->root);
    PUT("LANG=%s", "C.UTF-8");
    PUT("LC_ALL=%s", "C.UTF-8");
    /* See the static-target block above: this is the line whose INHERITANCE
     * killed every child a static binary spawned. */
    if (L->have_preload && !target_static) PUT("LD_PRELOAD=%s", L->preload);
    /* --fakeroot / --no-fakeroot override the environment (§1.1: "설정값"). */
    if (ro && ro->fakeroot >= 0)      PUT("ALR_FAKEROOT=%d", ro->fakeroot);
    else if (getenv("ALR_FAKEROOT"))  PUT("ALR_FAKEROOT=%s", getenv("ALR_FAKEROOT"));
    else if (cfg()->has_fakeroot)     PUT("ALR_FAKEROOT=%d", cfg()->fakeroot);
    /* ALR_LOG_FD is honoured from the caller when set, defaulting to 2.
     *
     * It used to be hardcoded to 2 here, and a harness that asked for fd 9 got
     * it only by accident: the environ copy loop above runs first, so the
     * inherited ALR_LOG_FD=9 landed earlier in envp and getenv(3) returned it
     * before reaching this hardcoded 2.  Working by ordering is not working.
     *
     * The fd matters because stderr is not a safe channel for it: GNU
     * coreutils closes stderr from an atexit handler, before the destructor
     * that prints the counters runs, so the line silently disappears
     * (MEASURED: /bin/echo emitted nothing 10 runs out of 10). */
    if (getenv("ALR_COUNT")) {
        const char *lf = getenv("ALR_LOG_FD");
        PUT("ALR_COUNT=%s", getenv("ALR_COUNT"));
        PUT("ALR_LOG_FD=%s", lf && *lf ? lf : "2");
    }
    if (g_resolv_sock) PUT(ALR_RESOLV_ENV "=%s", g_resolv_sock);
#undef PUT

    env[n] = NULL;
    return env;
}

struct rdctx { int fd; };
static int rd_pread(void *c, void *dst, size_t len, uint64_t off)
{
    struct rdctx *r = c;
    return pread(r->fd, dst, len, (off_t)off) == (ssize_t)len;
}

/* Is this ELF statically linked (no PT_INTERP)?
 *
 * Same question shebang_resolve() already answers via alr_classify, asked
 * again at the point that has to ACT on it: the environment builder.  Reusing
 * alr_classify keeps one definition of "static" -- and that definition is
 * load-bearing, because passing NULL for the interp buffer makes it report
 * every ELF as static (the comment at its other call site records the day that
 * cost). */
static int exe_is_static(const char *host)
{
    unsigned char head[ALR_PROBE_BYTES];
    char interp_probe[ALR_PBUF];
    struct alr_shebang sb;
    struct rdctx ctx;
    ssize_t n;
    int fd, k;

    if ((fd = open(host, O_RDONLY | O_CLOEXEC)) < 0) return 0;
    n = read(fd, head, sizeof head);
    ctx.fd = fd;
    k = alr_classify(head, n < 0 ? 0 : (size_t)n, rd_pread, &ctx, &sb,
                     interp_probe, sizeof interp_probe);
    close(fd);
    return k == ALR_EXE_ELF_STATIC;
}

/* Classify the target the way the preload does for guest-initiated execs.
 * Without this, `alr run npm ...` hands a #! script straight to ld.so, which
 * reports "file too short" -- npm, npx, and most language-tool entry points
 * are scripts, so this is the common case, not an edge case.
 * Resolves the interpreter in the GUEST namespace and recurses. */
static int shebang_resolve(const struct launch *L, char *host, size_t hostsz,
                           const char **pre, int *npre, int depth)
{
    unsigned char head[ALR_PROBE_BYTES];
    struct alr_shebang sb;
    struct rdctx ctx;
    char interp_host[ALR_PBUF];
    enum alr_exe_kind k;
    ssize_t n;
    int fd;

    if (depth > ALR_SHEBANG_MAX_DEPTH) { errno = ELOOP; return -1; }
    char interp_probe[ALR_PBUF];
    fd = open(host, O_RDONLY);
    if (fd < 0) return -1;
    n = read(fd, head, sizeof head);
    ctx.fd = fd;
    /* The interp buffer is what MAKES the static/dynamic answer real.
     * alr_elf.c gates the PT_INTERP walk on `pread && interp_out && interp_sz`,
     * so passing NULL/0 -- as this did -- skips it and reports EVERY ELF as
     * ALR_EXE_ELF_STATIC.  A first attempt at the unhooked-binary warning below
     * fired on /usr/bin/git because of it. */
    k = alr_classify(head, n < 0 ? 0 : (size_t)n, rd_pread, &ctx, &sb,
                     interp_probe, sizeof interp_probe);
    close(fd);

    /* Say it when the target cannot be hooked.
     *
     * alr_classify already knows -- ALR_EXE_ELF_STATIC is literally commented
     * "no PT_INTERP -> unhookable" -- and until now that knowledge died here at
     * every verbosity.  A static binary runs, so nothing looks wrong, while
     * every path it touches goes to the ANDROID filesystem instead of the
     * rootfs.  This is the one moment we can tell the user, and it is exactly
     * the class ADR 0008 makes a non-goal.
     *
     * Only for the top-level target (depth 0): a shebang interpreter that is
     * static is a different and much rarer thing, and warning inside the chain
     * would fire on every script. */
    if (k == ALR_EXE_ELF_STATIC && depth == 0)
        fprintf(stderr,
            "alr: NOTE %s is statically linked -- path virtualization does NOT\n"
            "     apply to it.  It runs, but its paths resolve against Android,\n"
            "     not the guest rootfs.\n"
            "     reason=unhooked-static-binary  (docs/adr/0008-static-guest-binaries-non-goal.md)\n",
            host);

    if (k != ALR_EXE_SHEBANG) return 0;      /* ELF (or unsupported): as-is */

    if (resolve(L, sb.interp, interp_host, sizeof interp_host) != 0) {
        errno = ENOENT;
        return -1;
    }
    /* argv becomes [interp, (arg,) script, original args...]; the script keeps
     * its GUEST path because that is what the program will see in argv.
     *
     * PREPEND, do not append.  A nested shebang -- /tmp/s2 is "#!/tmp/s1" and
     * /tmp/s1 is "#!/bin/sh" -- must end up as the kernel builds it:
     *     /bin/sh /tmp/s1 /tmp/s2
     * i.e. each level's script goes in FRONT of what deeper levels already
     * contributed, because the outer script is the argument of the inner one.
     * Appending produced "/bin/sh /tmp/s2 /tmp/s1", so sh read s2 as a shell
     * script, treated its "#!" line as a comment, and exited 0 having printed
     * NOTHING.  Silent and successful is the worst possible shape.
     * MEASURED 2026-08-03: native Termux runs the same pair correctly, alr
     * produced empty output with rc=0. */
    {
        int add = (sb.has_arg ? 1 : 0) + 1, i;
        if (*npre + add <= 4) {
            for (i = *npre - 1; i >= 0; i--) pre[i + add] = pre[i];
            i = 0;
            if (sb.has_arg) pre[i++] = strdup(sb.arg);
            { char *g = malloc(ALR_PBUF);
              if (g) snprintf(g, ALR_PBUF, "%s", host + L->root_len);
              pre[i] = g; }
            *npre += add;
        }
    }
    snprintf(host, hostsz, "%s", interp_host);
    return shebang_resolve(L, host, hostsz, pre, npre, depth + 1);
}

/* Are we already being ptraced?
 *
 * A process can have exactly ONE tracer, so an `alr run` launched from inside
 * another `alr run` cannot start a second supervisor -- and does not need to,
 * because the outer one already traces the whole descendant tree and rescues
 * their SIGSYS just the same.
 *
 * This is not hypothetical plumbing.  It is the path a STATIC guest takes to
 * reach a guest tool at all: codex execs a host-side wrapper, the wrapper
 * re-enters `alr run`, and that inner alr is a descendant of the outer
 * supervisor.  MEASURED before this existed: the inner supervisor attached to
 * nothing and the tool died --
 *     alr supervisor: pids=0 sigsys=0 ... elapsed_ms=0
 *     rc=126
 * With the supervisor skipped, the same command returns `ripgrep 14.1.0`,
 * which also proves the outer supervisor is covering the inner guest.
 *
 * Reading TracerPid rather than trying and failing: PTRACE_TRACEME's failure
 * happens in the CHILD, after fork and before exec, where reporting it means
 * inventing a channel back to the parent. */
static int already_traced(void)
{
    char buf[512];
    FILE *f = fopen("/proc/self/status", "r");
    int traced = 0;
    if (!f) return 0;
    while (fgets(buf, sizeof buf, f)) {
        if (strncmp(buf, "TracerPid:", 10) != 0) continue;
        traced = strtol(buf + 10, NULL, 10) != 0;
        break;
    }
    fclose(f);
    return traced;
}

static int cmd_run(const char *distro, int argc, char **argv, int login_shell,
                   const struct runopts *ro)
{
    struct launch L;
    char host[ALR_PBUF];
    char *av[ALR_MAX_ARGV];
    const char *guest_cmd;
    struct alr_sup_opts o;
    struct alr_sup_stats st;
    int n = 0, i, status = 0, rc;
    const char *pre[4];
    int npre = 0;

    rc = prepare(&L, distro);
    if (rc == -1) die("rootfs-missing", "rootfs not installed; run `alr install`");
    if (rc == -2) die("ldso-missing",
                      "guest ld-linux-aarch64.so.1 not found; rootfs looks corrupt");
    /* Booting without the preload is not a degraded mode, it is a different
     * product: every path the guest touches resolves against Android.  It used
     * to happen silently -- prepare() just omits --preload.  Say it every time,
     * at every verbosity; this is not a --log-gated detail. */
    if (!L.have_preload)
        fprintf(stderr,
            "alr: WARNING the guest preload is MISSING from this rootfs.\n"
            "     reason=preload-missing-in-rootfs\n"
            "     Nothing is path-virtualized: the guest sees the ANDROID\n"
            "     filesystem, not %s.\n"
            "     Fix:  alr update-components\n", L.root);

    guest_cmd = login_shell ? "/bin/bash" : argv[0];
    if (resolve(&L, guest_cmd, host, sizeof host) != 0)
        die("boot-enoent", "command not found in the guest");
    if (shebang_resolve(&L, host, sizeof host, pre, &npre, 0) != 0)
        die("boot-enoent", "script interpreter not found in the guest");

    /* ── cwd mapping (docs/06-cli-spec.md §1.2) ──────────────────────
     * -w wins; otherwise, if the host cwd is inside <R> the guest keeps the
     * corresponding directory, and anything else falls back to /root with a
     * warning.  A chdir here is enough: alr_supervise() forks and the child
     * execs without changing directory, so it inherits this one, and the
     * preload's getcwd/realpath report the guest view of it. */
    {
        char cwd[ALR_PBUF], target[ALR_PBUF];
        const char *guest_wd = ro ? ro->workdir : NULL;
        size_t rl = strlen(L.root);

        if (!guest_wd) {
            if (getcwd(cwd, sizeof cwd) && !strncmp(cwd, L.root, rl)
                && (cwd[rl] == '/' || cwd[rl] == '\0')) {
                guest_wd = cwd[rl] ? cwd + rl : "/";
            } else {
                guest_wd = "/root";
                if (g_log >= 1)
                    fprintf(stderr, "alr: cwd is outside the guest rootfs; "
                                    "using /root (docs/06-cli-spec.md §1.2)\n");
            }
        }
        if (guest_wd[0] != '/')
            die("bad-workdir", "-w takes an absolute GUEST path");
        /* Route through alr_rw() -- the ONE rewriter.
         *
         * This used to be snprintf("%s%s", root, guest_wd), a second rewriter
         * in the CLI, which alr_path_rule.h's own header forbids by name.  It
         * skipped alr_normalize_guest_abs()'s clamp, whose whole job is that
         * ".." pops to nothing at the root -- so `-w /../..` chdir'd ABOVE the
         * rootfs.  MEASURED: `alr run -w /../.. /bin/pwd` printed
         * /data/data/com.termux/files/home, and every relative open in that
         * run then resolved outside the guest.
         *
         * alr_rw also leaves /proc,/sys,/dev alone, so `-w /proc` lands on the
         * real one instead of a rootfs directory that does not exist. */
        {
            const char *hw = alr_rw(guest_wd, L.root, L.root_len,
                                    target, sizeof target, NULL);
            const char *cw;
            char cbuf[ALR_PBUF];
            if (!hw) die("bad-workdir", "-w path too long");
            if (hw != target) snprintf(target, sizeof target, "%s", hw);
            /* $PWD must be the CANONICAL guest view, not the string the user
             * typed: otherwise `-w /root/../etc` sets PWD=/root/../etc while
             * getcwd(3) reports /etc and the guest disagrees with itself. */
            cw = alr_guest_canon(target, L.root, L.root_len, cbuf, sizeof cbuf);
            snprintf(g_guest_cwd, sizeof g_guest_cwd, "%s", cw ? cw : guest_wd);
        }
        if (chdir(target) != 0) {
            /* Do not fall back silently: a command that runs in an unexpected
             * directory produces wrong output rather than an error. */
            char d[ALR_PBUF];
            snprintf(d, sizeof d, "guest workdir %s does not exist", guest_wd);
            die("workdir-enoent", d);
        }
    }

    /* ADR 0002: the program argument MUST contain a '/' -- glibc's dl-load.c
     * treats a slash-free name as a LIBRARY name and searches the library path
     * for it, not $PATH.  We always pass an absolute host path. */
    av[n++] = L.ldso;
    av[n++] = (char *)"--library-path";
    av[n++] = L.libpath;
    /* Defensive: there is no /system/etc/ld.so.cache today, but if one ever
     * appeared it would silently poison every library resolution. */
    av[n++] = (char *)"--inhibit-cache";
    av[n++] = (char *)"--argv0";
    av[n++] = (char *)(login_shell ? "-bash" : argv[0]);
    if (L.have_preload) { av[n++] = (char *)"--preload"; av[n++] = L.preload; }
    av[n++] = host;
    for (i = 0; i < npre; i++) av[n++] = (char *)pre[i];   /* shebang arg + script */
    if (login_shell) av[n++] = (char *)"-l";
    else for (i = 1; i < argc && n < ALR_MAX_ARGV - 2; i++) av[n++] = argv[i];
    av[n] = NULL;

    if (g_log >= 2) {
        fprintf(stderr, "alr: exec %s\n", L.ldso);
        for (i = 0; i < n; i++) fprintf(stderr, "  argv[%d]=%s\n", i, av[i]);
    }

    /* Start the resolver bridge before the guest exists.  If it cannot start
     * we continue without it: the guest then uses glibc's own resolver, which
     * is correct on devices that do not have Private DNS or a VPN. */
    g_resolv_sock = alr_resolvd_start(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (g_log >= 1 && !g_resolv_sock)
        fprintf(stderr, "alr: resolver bridge unavailable; guest DNS may fail "
                        "on devices with Private DNS or a VPN (RISKS R15)\n");

    memset(&o, 0, sizeof o);
    o.path = L.ldso;
    o.argv = av;
    o.envp = build_env(&L, guest_cmd, exe_is_static(host), av[5], ro,
                       g_guest_cwd);
    o.log_level = g_log;
    o.log_fd = -1;

    /* Ubuntu's default umask, not Termux's.
     *
     * Termux runs with umask 077, and the guest inherited it, so every
     * directory a guest program created came out 0700.  MEASURED: that alone
     * breaks `dpkg-deb -b` --
     *     dpkg-deb: error: control directory has bad permissions 700
     *                      (must be >=0755 and <=0775)
     * -- and with it every package build, and anything else that creates a
     * directory expecting the 022 every Linux distribution defaults to.
     *
     * cmd_install already does exactly this around extraction, for exactly
     * this reason (the note there records dpkg reporting "'sh' not found in
     * PATH or not executable" for files that plainly were).  The guest is an
     * Ubuntu userspace; it should boot with Ubuntu's default.
     *
     * Nothing is loosened in practice: the whole rootfs lives under the app's
     * private data directory, which Android keeps at 0700, so 077 inside it
     * protects against nobody and costs the tooling. */
    umask(022);

    /* Inherit the outer supervisor rather than failing to start our own. */
    if (!(ro && ro->no_supervisor) && already_traced()) {
        if (g_log >= 1)
            fprintf(stderr, "alr: already traced (TracerPid != 0); inheriting "
                            "the outer supervisor instead of starting one\n");
        alr_resolvd_stop();
        execve(o.path, (char *const *)o.argv, (char *const *)o.envp);
        die("boot-failed", "execve under an inherited supervisor failed");
    }

    if (ro && ro->no_supervisor) {
        /* §1.1 marks this 위험 and says it usually fails to boot -- and it
         * does: ld.so calls set_robust_list before any constructor runs, the
         * zygote filter kills it with SIGSYS, and there is no one to emulate
         * the return.  Kept because a debugger sometimes needs the raw failure,
         * but it announces itself rather than looking like a normal run. */
        fprintf(stderr,
            "alr: WARNING --no-supervisor: nothing will rescue SIGSYS.\n"
            "  reason=no-supervisor-requested\n"
            "  Expect death on set_robust_list before main() (ADR 0001).\n");
        alr_resolvd_stop();
        execve(o.path, (char *const *)o.argv, (char *const *)o.envp);
        die("boot-failed", "execve without supervisor failed");
    }
    if (alr_supervise(&o, &status, &st) < 0) { alr_resolvd_stop(); die("boot-failed", "supervisor failed"); }
    alr_resolvd_stop();

    if (g_log >= 1)
        /* The full line docs/03-supervisor-spec.md §6 specifies.  It was
         * missing passthrough_signals and elapsed_ms, and bench/
         * regression_gate.py's patterns are anchored per-key, so adding
         * fields does not disturb them. */
        fprintf(stderr, "alr supervisor: pids=%lu sigsys=%lu emulated=%lu "
                        "passthrough_signals=%lu path_traps=%lu "
                        "syscall_stops=%lu elapsed_ms=%lu\n",
                st.tracees_seen, st.sigsys_seen, st.sigsys_emulated,
                st.passthrough_signals, st.path_traps, st.syscall_stops,
                st.elapsed_ms);

    return alr_exit_code(status);
}

/* ── main ────────────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "usage: alr [-d distro] [-v] <command>\n"
        "  install [<distro>] [--url <tarball>] [--with git,node,codex]\n"
        "          [--force]                  provision a rootfs\n"
        "  list                               installed rootfs and preload state\n"
        "  remove <distro> [--force]          DELETE a rootfs\n"
        "  run [opts] <cmd> [args...]         run one guest command\n"
        "  exec [opts] -- <cmd> [args...]     run, with -- ending option parsing\n"
        "  shell [opts]                       interactive guest shell\n"
        "  update-components [<distro>]       refresh the guest preload copy\n"
        "  config [get <k> | set <k> <v>]     settings and where they come from\n"
        "  version                            version and preload identity\n"
        "  doctor [probe-dir]                 device capability report\n"
        "\n"
        "options for run/exec/shell (docs/06-cli-spec.md §1.1):\n"
        "  -d, --distro <name>    rootfs to use\n"
        "  -w, --workdir <path>   guest cwd (default: mapped host cwd, else /root)\n"
        "  -e, --env KEY=VAL      add to the guest environment (repeatable)\n"
        "      --fakeroot         spoof uid 0 (needed by apt/dpkg)\n"
        "      --no-fakeroot      do not spoof\n"
        "      --log <0|1|2>      diagnostic verbosity\n"
        "      --no-supervisor    DANGEROUS: debugging only, usually fails to boot\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *distro = getenv("ALR_DISTRO");
    const char *url = NULL;
    int i = 1;

    {   /* Must happen before any fork: see g_self. */
        ssize_t n = readlink("/proc/self/exe", g_self, sizeof g_self - 1);
        if (n > 0) g_self[n] = '\0';
        else snprintf(g_self, sizeof g_self, "%s", argv[0]);
    }
    if (!distro || !*distro) distro = cfg()->has_distro ? cfg()->distro : "ubuntu-24.04";
    if (getenv("ALR_LOG")) g_log = atoi(getenv("ALR_LOG"));
    else if (cfg()->has_log) g_log = cfg()->log;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { distro = argv[++i]; i++; }
        else if (!strcmp(argv[i], "-v")) { g_log++; i++; }
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) { url = argv[++i]; i++; }
        else break;
    }
    if (i >= argc) usage();

    if (!strcmp(argv[i], "install")) {
        /* Options and the positional in any order, and an unknown option is an
         * ERROR.  The old loop scanned for the three it knew and took
         * argv[i+1] as the distro whatever it was, so
         *     alr install -d ubuntu-24.04 --url ...
         * -- putting the global -d after the subcommand, which every other
         * subcommand accepts -- INSTALLED A ROOTFS NAMED "-d", reported
         * success, and printed a nine-line verification report about it.
         * Nothing rejected it: `-d` is a legal name under distro_name_ok().
         * Same treatment doctor already gives unknown options. */
        const char *d = NULL;
        int j;
        for (j = i + 1; j < argc; j++) {
            if (!strcmp(argv[j], "-d") && j + 1 < argc)     d = argv[++j];
            else if (!strcmp(argv[j], "--url")  && j + 1 < argc) url = argv[++j];
            else if (!strcmp(argv[j], "--with") && j + 1 < argc) g_with = argv[++j];
            else if (!strcmp(argv[j], "--force")) g_force = 1;
            else if (argv[j][0] == '-' && argv[j][1] != '\0')
                die("install-unknown-option", "unknown option for `alr install`");
            else if (!d) d = argv[j];
            else die("install-unknown-option",
                     "more than one distro named for `alr install`");
        }
        return cmd_install(d ? d : distro, url);
    }
    if (!strcmp(argv[i], "run") || !strcmp(argv[i], "exec")) {
        /* `exec` is `run` with -- ending option parsing unambiguously (§1). */
        int is_exec = !strcmp(argv[i], "exec");
        struct runopts ro; int j;
        runopts_init(&ro);
        j = i + 1 + parse_runopts(argc - i - 1, argv + i + 1, &ro, &distro, is_exec);
        if (j >= argc) usage();
        return cmd_run(distro, argc - j, argv + j, 0, &ro);
    }
    if (!strcmp(argv[i], "shell")) {
        struct runopts ro;
        runopts_init(&ro);
        (void)parse_runopts(argc - i - 1, argv + i + 1, &ro, &distro, 0);
        return cmd_run(distro, 0, NULL, 1, &ro);
    }
    if (!strcmp(argv[i], "config"))
        return cmd_config(argc - i - 1, argv + i + 1);
    if (!strcmp(argv[i], "version") || !strcmp(argv[i], "--version")) {
        /* `alr version -d <name>` and `alr -d <name> version` both work: the
         * global loop above already consumed the leading form, this catches
         * the trailing one. */
        int j;
        for (j = i + 1; j + 1 < argc; j++)
            if (!strcmp(argv[j], "-d") || !strcmp(argv[j], "--distro"))
                distro = argv[j + 1];
        return cmd_version(distro);
    }
    if (!strcmp(argv[i], "list"))
        return cmd_list();
    if (!strcmp(argv[i], "remove")) {
        int f = 0, j;
        const char *d = NULL;
        for (j = i + 1; j < argc; j++) {
            if (!strcmp(argv[j], "--force")) f = 1;
            else if (!d) d = argv[j];
        }
        return cmd_remove(d ? d : distro, f);
    }
    if (!strcmp(argv[i], "update-components"))
        return cmd_update_components((i + 1 < argc) ? argv[i + 1] : distro);
    if (!strcmp(argv[i], "doctor")) {
        /* docs/06-cli-spec.md §3 lists `alr doctor`.  It is a separate binary
         * because it must run BEFORE any rootfs exists and probes the host, not
         * the guest -- but a user should not have to know that.  Exec the
         * sibling next to g_self, which is where both land in the release
         * layout (bin/alr, bin/alr-doctor). */
        char p[ALR_PBUF], dir[ALR_PBUF], *slash;
        snprintf(dir, sizeof dir, "%s", g_self);
        slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else snprintf(dir, sizeof dir, ".");
        snprintf(p, sizeof p, "%s/alr-doctor", dir);
        if (access(p, X_OK) != 0) {
            fprintf(stderr,
                "alr: alr-doctor not found next to %s\n"
                "  reason=doctor-missing\n"
                "  The release tarball ships bin/alr and bin/alr-doctor together;\n"
                "  install both, or run scripts/dev-push.sh doctor when developing.\n",
                g_self);
            return 125;
        }
        { char *av[8]; int k = 0, j;
          av[k++] = p;
          for (j = i + 1; j < argc && k < 7; j++) av[k++] = argv[j];
          av[k] = NULL;
          execv(p, av);
          fprintf(stderr, "alr: cannot exec %s: %s\n", p, strerror(errno));
          return 125; }
    }

    usage();
    return 2;
}
