/* alr_path_rule.h — guest <-> host path translation.
 *
 * THIS IS THE SINGLE SOURCE OF TRUTH.  The preload interposer and the CLI both
 * include this header; neither is allowed to reimplement any of it (playbook
 * invariant I3).  Two rewriters that drift produce "files randomly missing",
 * which is close to undebuggable.
 *
 * MUST NOT include a Linux-specific header.  This compiles and runs on macOS so
 * the whole rule set is testable without a device (docs/08-milestones.md M1).
 *
 * Hot-path budget: <=100 ns for an absolute-path rewrite, <=20 ns for the
 * relative-path miss (docs/adr/0003 — git status makes 12-15k of these calls).
 * No allocation, no locks, no syscalls, no cache.
 */
#ifndef ALR_PATH_RULE_H
#define ALR_PATH_RULE_H

#include <stddef.h>
#include <string.h>

/* Buffer size for a rewritten path.  MUST equal PATH_MAX: the guard below then
 * rejects exactly what the kernel would reject and nothing more.  A smaller
 * value silently fails legal deep paths (pnpm stores, dpkg unpack trees) and
 * the symptom looks like "file not found", not like a size error. */
#define ALR_PBUF 4096

/* Maximum path components we will normalize through.  A path deeper than this
 * is rejected rather than silently mis-normalized. */
#define ALR_MAX_DEPTH 128

enum {
    ALR_OK = 0,
    ALR_E_TOOLONG = 1,   /* caller should report ENAMETOOLONG */
    ALR_E_DEPTH   = 2    /* caller should report ENAMETOOLONG as well */
};

/* ── component-boundary prefix match ─────────────────────────────────────
 * "/proctology" is NOT under "/proc".  `prefix` must not end in '/'. */
static inline int alr_path_under(const char *p, const char *prefix, size_t plen)
{
    if (plen == 0) return 0;
    if (memcmp(p, prefix, plen) != 0) return 0;
    return p[plen] == '\0' || p[plen] == '/';
}

/* Kernel virtual filesystems: never redirected into the rootfs. */
static inline int alr_is_sysdir(const char *p)
{
    return alr_path_under(p, "/proc", 5)
        || alr_path_under(p, "/sys",  4)
        || alr_path_under(p, "/dev",  4);
}

/* ── cheap pre-scan ──────────────────────────────────────────────────────
 * Most paths are already normal.  One linear pass (same cost class as the
 * strlen we need anyway) lets the common case skip the normalizer. */
static inline int alr_needs_normalize(const char *p)
{
    const char *s;
    for (s = p; *s; s++) {
        if (s[0] != '/') continue;
        if (s[1] == '/') return 1;                       /* //          */
        if (s[1] == '\0') return s != p;                 /* trailing /  */
        if (s[1] == '.') {
            if (s[2] == '\0' || s[2] == '/') return 1;   /* /. or /./   */
            if (s[2] == '.' && (s[3] == '\0' || s[3] == '/')) return 1;  /* /.. */
        }
    }
    return 0;
}

/* ── normalize an ABSOLUTE guest path ────────────────────────────────────
 * Collapses '.', '//' and trailing '/'; ".." pops one component and is a
 * NO-OP at the root.  That clamp is what makes rootfs escape structurally
 * impossible (docs/02-architecture.md §7.1) — it is not a convenience.
 * Returns 1 on success, 0 on overflow (out is then undefined). */
static inline int alr_normalize_guest_abs(const char *p, char *out, size_t outsz)
{
    size_t stack[ALR_MAX_DEPTH];
    int depth = 0;
    size_t o = 0;
    const char *s = p;

    if (outsz < 2) return 0;
    out[o++] = '/';

    while (*s) {
        const char *comp;
        size_t len;

        while (*s == '/') s++;
        if (!*s) break;

        comp = s;
        while (*s && *s != '/') s++;
        len = (size_t)(s - comp);

        if (len == 1 && comp[0] == '.') continue;
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (depth > 0) { depth--; o = stack[depth]; }
            continue;                       /* clamp: no-op at root */
        }
        if (depth >= ALR_MAX_DEPTH) return 0;

        stack[depth++] = o;                 /* rewind point INCLUDES the '/' */
        if (o > 1) {
            if (o + 1 >= outsz) return 0;
            out[o++] = '/';
        }
        if (o + len >= outsz) return 0;
        memcpy(out + o, comp, len);
        o += len;
    }
    if (o == 0) out[o++] = '/';
    out[o] = '\0';
    return 1;
}

/* ── the hot path ────────────────────────────────────────────────────────
 * Returns `p` itself when no rewrite is needed (callers may rely on pointer
 * identity — returning a copy would add a memcpy to every call), `buf` when a
 * rewrite or normalization happened, or NULL on error with *err set.
 *
 * `root` must be absolute with NO trailing slash; `root_len` its length.
 * root_len == 0 means passthrough mode (ALR_ROOT unset/invalid).
 *
 * ORDER IS LOAD-BEARING: normalize BEFORE the sysdir and already-under-root
 * checks.  Both of those are component-boundary prefix matches, so they fire
 * on unnormalized input and return early — with the order reversed,
 * "/proc/../etc/passwd" passes through unrewritten and the guest silently
 * reads ANDROID's /etc/passwd instead of the rootfs's. */
static inline const char *alr_rw(const char *p, const char *root, size_t root_len,
                                 char *buf, size_t bufsz, int *err)
{
    const char *q;
    size_t n;

    if (err) *err = ALR_OK;
    if (!p)            return p;
    if (p[0] != '/')   return p;      /* relative — never rewritten */
    if (!root_len)     return p;      /* passthrough mode */

    q = p;
    if (alr_needs_normalize(p)) {
        if (!alr_normalize_guest_abs(p, buf, bufsz)) {
            if (err) *err = ALR_E_TOOLONG;
            return NULL;
        }
        q = buf;
    }

    if (alr_is_sysdir(q))                    return (q == buf) ? buf : p;
    if (alr_path_under(q, root, root_len))   return (q == buf) ? buf : p;

    /* guest "/" maps to the rootfs itself, with no trailing slash. */
    if (q[0] == '/' && q[1] == '\0') {
        if (root_len + 1 > bufsz) { if (err) *err = ALR_E_TOOLONG; return NULL; }
        memcpy(buf, root, root_len);
        buf[root_len] = '\0';
        return buf;
    }

    n = strlen(q);
    if (root_len + n + 1 > bufsz) { if (err) *err = ALR_E_TOOLONG; return NULL; }
    memmove(buf + root_len, q, n + 1);   /* q may alias buf after normalizing */
    memcpy(buf, root, root_len);
    return buf;
}

/* ── reverse: host -> guest ──────────────────────────────────────────────
 * Applied to anything that RETURNS a path (getcwd, realpath, readlink of a
 * rootfs-internal absolute symlink, ttyname_r).  Without it the guest sees raw
 * host paths and the next absolute open re-prefixes them into nonsense.
 * Returns `h` unchanged when it is not under root. */
static inline const char *alr_guest_canon(const char *h, const char *root,
                                          size_t root_len, char *buf, size_t bufsz)
{
    size_t n;
    if (!h || !root_len) return h;
    if (!alr_path_under(h, root, root_len)) return h;

    n = strlen(h) - root_len;
    if (n == 0) {                        /* exactly the rootfs -> "/" */
        if (bufsz < 2) return h;
        buf[0] = '/'; buf[1] = '\0';
        return buf;
    }
    if (n + 1 > bufsz) return h;
    memcpy(buf, h + root_len, n + 1);
    return buf;
}

/* Trim trailing slashes from a root path in place; returns the new length.
 * Never trims below "/" -> length 1 (a root of "/" means passthrough-ish and
 * is rejected by the caller, but the function must not produce ""). */
static inline size_t alr_trim_root(char *root)
{
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') root[--n] = '\0';
    return n;
}

#endif /* ALR_PATH_RULE_H */
