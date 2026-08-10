#include "alr_exec_rule.h"
#include "alr_path_rule.h"

#include <string.h>

size_t alr_split_env(const char *entry, const char **val)
{
    const char *eq = strchr(entry, '=');
    if (!eq) { *val = entry + strlen(entry); return strlen(entry); }
    *val = eq + 1;
    return (size_t)(eq - entry);
}

int alr_colon_contains(const char *list, const char *needle)
{
    size_t nl;
    const char *p;

    if (!list || !*list || !needle || !*needle) return 0;
    nl = strlen(needle);

    for (p = list; *p; ) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == nl && memcmp(p, needle, nl) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Append `s` to out[*o], with a ':' separator if anything is there already. */
static int append_elem(char *out, size_t outsz, size_t *o, const char *s,
                       size_t len)
{
    if (len == 0) return 1;
    if (*o) {
        if (*o + 1 >= outsz) return 0;
        out[(*o)++] = ':';
    }
    if (*o + len >= outsz) return 0;
    memcpy(out + *o, s, len);
    *o += len;
    out[*o] = '\0';
    return 1;
}

int alr_build_ld_preload(const char *cur, const char *interpose_so,
                         const char *fakeroot_so, char *out, size_t outsz,
                         int *changed)
{
    size_t o = 0;
    const char *p;
    int ok = 1;

    if (changed) *changed = 1;
    if (!interpose_so || !*interpose_so || outsz < 2) return 0;
    out[0] = '\0';

    /* Already satisfied?  Then leave `cur` alone entirely — re-emitting a
     * reordered but equivalent value on every nested exec would churn the
     * environment and break idempotency assertions. */
    if (cur && *cur
        && alr_colon_contains(cur, interpose_so)
        && (!fakeroot_so || alr_colon_contains(cur, fakeroot_so))) {
        if (strlen(cur) + 1 > outsz) return 0;
        memcpy(out, cur, strlen(cur) + 1);
        if (changed) *changed = 0;
        return 1;
    }

    if (fakeroot_so && *fakeroot_so)
        ok = ok && append_elem(out, outsz, &o, fakeroot_so, strlen(fakeroot_so));
    ok = ok && append_elem(out, outsz, &o, interpose_so, strlen(interpose_so));
    if (!ok) return 0;

    /* Carry over the guest's own entries, dropping duplicates of ours. */
    for (p = cur; p && *p; ) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) {
            int dup = (len == strlen(interpose_so)
                       && memcmp(p, interpose_so, len) == 0)
                   || (fakeroot_so && len == strlen(fakeroot_so)
                       && memcmp(p, fakeroot_so, len) == 0);
            if (!dup && !append_elem(out, outsz, &o, p, len)) return 0;
        }
        if (!e) break;
        p = e + 1;
    }
    return 1;
}

int alr_is_bionic_target(const char *host_path, const char *termux_prefix)
{
    if (!host_path || host_path[0] != '/') return 0;
    if (alr_path_under(host_path, "/system", 7)) return 1;
    if (alr_path_under(host_path, "/apex", 5))   return 1;
    if (alr_path_under(host_path, "/vendor", 7)) return 1;
    if (termux_prefix && *termux_prefix
        && alr_path_under(host_path, termux_prefix, strlen(termux_prefix)))
        return 1;
    return 0;
}

int alr_env_is_blocked(const char *entry)
{
    /* Android/Termux host variables that confuse or break a glibc guest.
     * LD_PRELOAD and LD_LIBRARY_PATH are handled separately (they are
     * REPLACED, not dropped — see alr_build_ld_preload). */
    static const char *const blocked[] = {
        "ANDROID_ROOT", "ANDROID_DATA", "ANDROID_ART_ROOT",
        "ANDROID_I18N_ROOT", "ANDROID_TZDATA_ROOT", "ANDROID_ASSETS",
        "ANDROID_STORAGE", "BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH",
        "SYSTEMSERVERCLASSPATH", "EXTERNAL_STORAGE", "PREFIX",
        "TERMUX_APK_RELEASE", "TERMUX_APP_PID", "TERMUX_IS_DEBUGGABLE_BUILD",
        "TERMUX_MAIN_PACKAGE_FORMAT", "TERMUX_VERSION", "TERMUX__USER_ID",
        NULL
    };
    const char *val;
    size_t klen = alr_split_env(entry, &val);
    int i;

    for (i = 0; blocked[i]; i++)
        if (strlen(blocked[i]) == klen && memcmp(entry, blocked[i], klen) == 0)
            return 1;

    /* Whole families. */
    if (klen > 15 && memcmp(entry, "ANDROID_SOCKET_", 15) == 0) return 1;
    if (klen >= 7 && memcmp(entry, "TERMUX_", 7) == 0)          return 1;
    return 0;
}
