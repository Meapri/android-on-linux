/* Host-side test for src/common/alr_path_rule.h.
 *
 * Runs on macOS and Linux with a plain compiler — no device, no Linux headers.
 * That is the whole point of keeping the rule in a dependency-free header
 * (docs/08-milestones.md M1).
 *
 * Reads tests/cases/paths.tsv, the shared case table.  Emits the acceptance
 * string ALR PATH RULE HOST TESTS: PASS|FAIL.
 */
#include "alr_path_rule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;

static void report(int ok, const char *root, const char *in,
                   const char *want, const char *got, const char *why)
{
    if (ok) { pass++; return; }
    fail++;
    fprintf(stderr,
            "  FAIL %s\n"
            "       root=%s\n"
            "       in  =%s\n"
            "       want=%s\n"
            "       got =%s\n",
            why, root && *root ? root : "(empty)", in, want,
            got ? got : "(NULL)");
}

/* One tsv row. */
static void run_case(const char *root_in, const char *in, const char *want)
{
    char rootbuf[ALR_PBUF];
    char buf[ALR_PBUF];
    size_t rlen;
    const char *got = NULL;
    int err = 0;

    /* The tsv line buffer is larger than ALR_PBUF, so gcc is right that this
     * can truncate (-Wformat-truncation).  A root that does not fit is bad
     * test input, not something to silently shorten -- a truncated root would
     * make every case in the row assert against a prefix that alr_rw() would
     * never produce.  Fail loudly instead. */
    if (strlen(root_in) >= sizeof rootbuf) {
        fprintf(stderr, "FAIL  root longer than ALR_PBUF (%zu >= %zu): %.64s...\n",
                strlen(root_in), sizeof rootbuf, root_in);
        fail++;
        return;
    }
    memcpy(rootbuf, root_in, strlen(root_in) + 1);
    rlen = *rootbuf ? alr_trim_root(rootbuf) : 0;

    memset(buf, 0xAA, sizeof buf);      /* poison: catch reads of stale buf */
    got = alr_rw(in, rootbuf, rlen, buf, sizeof buf, &err);

    if (!strcmp(want, "!TOOLONG")) {
        report(got == NULL && err == ALR_E_TOOLONG, root_in, in, want,
               got, "expected ENAMETOOLONG");
        return;
    }
    if (got == NULL) {
        report(0, root_in, in, want, NULL, "unexpected NULL");
        return;
    }
    if (!strcmp(want, "=")) {
        /* Identity is part of the contract: returning a copy would put a
         * memcpy on every no-rewrite call, which is most of git status. */
        report(got == in, root_in, in, "(input pointer, unchanged)", got,
               got == in ? "" : "returned a copy instead of the input pointer");
        return;
    }
    report(!strcmp(got, want), root_in, in, want, got, "wrong result");
}

/* Cases that cannot be spelled out in a tsv. */
static void run_generated(void)
{
    static char deep[ALR_PBUF * 2];
    char buf[ALR_PBUF];
    const char *root = "/data/alr/ubuntu";
    size_t rlen = strlen(root);
    const char *got = NULL;
    int err = 0;
    size_t i;

    /* GEN_TOOLONG: a guest path that cannot fit once prefixed. */
    deep[0] = '/';
    for (i = 1; i < ALR_PBUF; i++) deep[i] = 'a';
    deep[ALR_PBUF] = '\0';
    err = 0;
    got = alr_rw(deep, root, rlen, buf, sizeof buf, &err);
    report(got == NULL && err == ALR_E_TOOLONG, root, "(4096 'a's)",
           "!TOOLONG", got, "long path must be rejected, not truncated");

    /* GEN_FITS: just under the limit must SUCCEED.  An undersized buffer
     * would reject paths the kernel accepts, and the symptom looks like
     * "file not found" (docs/adr/0003). */
    {
        size_t room = ALR_PBUF - rlen - 2;
        deep[0] = '/';
        for (i = 1; i <= room; i++) deep[i] = 'a';
        deep[room + 1] = '\0';
        err = 0;
        got = alr_rw(deep, root, rlen, buf, sizeof buf, &err);
        report(got != NULL && err == ALR_OK
                   && !strncmp(got, root, rlen)
                   && strlen(got) == rlen + room + 1,
               root, "(longest legal path)", "(rewritten)", got,
               "a path that fits must not be rejected");
    }

    /* GEN_DEPTH: more components than ALR_MAX_DEPTH must be rejected, not
     * silently mis-normalized. */
    {
        char *w = deep;
        for (i = 0; i < ALR_MAX_DEPTH + 8; i++) { *w++ = '/'; *w++ = 'd'; }
        *w = '\0';
        err = 0;
        got = alr_rw(deep, root, rlen, buf, sizeof buf, &err);
        /* No '.' or '..' present, so needs_normalize() is false and this is a
         * plain prefix concat — depth only matters when normalizing. */
        report(got != NULL, root, "(deep, no dots)", "(rewritten)", got,
               "deep path without dots needs no normalization");

        w = deep;
        for (i = 0; i < ALR_MAX_DEPTH + 8; i++) { memcpy(w, "/d/.", 4); w += 4; }
        *w = '\0';
        err = 0;
        got = alr_rw(deep, root, rlen, buf, sizeof buf, &err);
        report(got == NULL && err == ALR_E_TOOLONG, root, "(deep, with dots)",
               "!TOOLONG", got, "over-deep normalize must be rejected");
    }

    /* GEN_NUL: embedded NUL truncates at the C level; assert we do not read
     * past it and do not escape. */
    got = alr_rw("/etc\0/passwd", root, rlen, buf, sizeof buf, &err);
    report(got != NULL && !strcmp(got, "/data/alr/ubuntu/etc"), root,
           "/etc\\0/passwd", "/data/alr/ubuntu/etc", got,
           "embedded NUL must terminate the path");

    /* GEN_CANON: reverse mapping round-trips. */
    {
        char cbuf[ALR_PBUF];
        const char *g = alr_guest_canon("/data/alr/ubuntu/usr/bin", root, rlen,
                                        cbuf, sizeof cbuf);
        report(!strcmp(g, "/usr/bin"), root, "canon(/data/alr/ubuntu/usr/bin)",
               "/usr/bin", g, "guest_canon");

        g = alr_guest_canon("/data/alr/ubuntu", root, rlen, cbuf, sizeof cbuf);
        report(!strcmp(g, "/"), root, "canon(root itself)", "/", g,
               "guest_canon of the root is /");

        g = alr_guest_canon("/data/alr/other/x", root, rlen, cbuf, sizeof cbuf);
        report(!strcmp(g, "/data/alr/other/x"), root, "canon(outside root)",
               "(unchanged)", g, "guest_canon must not touch outside paths");

        /* prefix-sharing sibling must NOT be canonicalized */
        g = alr_guest_canon("/data/alr/ubuntu-old/x", root, rlen, cbuf, sizeof cbuf);
        report(!strcmp(g, "/data/alr/ubuntu-old/x"), root,
               "canon(/data/alr/ubuntu-old/x)", "(unchanged)", g,
               "component boundary in guest_canon");
    }

    /* GEN_NULL: NULL in, NULL out, no crash. */
    report(alr_rw(NULL, root, rlen, buf, sizeof buf, &err) == NULL,
           root, "(NULL)", "(NULL)", "?", "NULL input");
}

static char *trim_nl(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
    return s;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/cases/paths.tsv";
    char line[8192];
    FILE *f = fopen(path, "r");
    int rows = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

    while (fgets(line, sizeof line, f)) {
        char *root, *in, *want, *t1, *t2;
        trim_nl(line);
        if (!*line || line[0] == '#') continue;

        t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = '\0';

        root = line; in = t1 + 1; want = t2 + 1;
        if (!*in) continue;
        run_case(root, in, want);
        rows++;
    }
    fclose(f);

    run_generated();

    printf("  tsv cases: %d   total assertions: %d\n", rows, pass + fail);
    if (rows < 60) {
        printf("ALR PATH RULE HOST TESTS: FAIL (only %d tsv cases, need >= 60)\n",
               rows);
        return 1;
    }
    printf("ALR PATH RULE HOST TESTS: %s (%d passed, %d failed)\n",
           fail ? "FAIL" : "PASS", pass, fail);
    return fail ? 1 : 0;
}
