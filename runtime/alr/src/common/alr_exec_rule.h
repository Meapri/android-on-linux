/* alr_exec_rule.h — pure decision kernels for the exec path.
 *
 * No filesystem access, no allocation beyond caller-supplied buffers, no Linux
 * headers.  These are the parts of exec handling that are pure functions of
 * (env, paths), so they can be tested exhaustively on a dev machine while the
 * messy I/O lives in the preload.
 */
#ifndef ALR_EXEC_RULE_H
#define ALR_EXEC_RULE_H

#include <stddef.h>

/* Split "KEY=VALUE" at the first '='.  A bare "KEY" yields an empty value.
 * Returns the key length; *val points into `entry` (or to its NUL). */
size_t alr_split_env(const char *entry, const char **val);

/* Is `needle` present as a whole colon-separated element of `list`?
 * "a:bc" contains "bc" but not "b". */
int alr_colon_contains(const char *list, const char *needle);

/* Build the LD_PRELOAD value the guest must see.
 *
 * Order is load-bearing (docs/02-architecture.md §4.4): fakeroot FIRST so it
 * wins the binding for the credential/metadata symbols, then the interposer,
 * then whatever the guest already had (de-duplicated, order preserved).
 *
 * `cur` may be NULL or "".  Writes into `out`; returns 1 on success, 0 if the
 * result would not fit.  Sets *changed to 0 when `cur` already satisfies the
 * requirement — callers use that to stay idempotent across nested execs.
 */
int alr_build_ld_preload(const char *cur,
                         const char *interpose_so,   /* required, absolute */
                         const char *fakeroot_so,    /* NULL when disabled */
                         char *out, size_t outsz, int *changed);

/* Should this exec target be treated as crossing back to the bionic side?
 * A guest calling /system/bin/am or $PREFIX/bin/termux-open must NOT inherit
 * the glibc LD_PRELOAD — the bionic linker cannot load a glibc .so and the
 * exec fails with a confusing linker error (docs/01-platform-facts.md §B7). */
int alr_is_bionic_target(const char *host_path, const char *termux_prefix);

/* Environment variables that must never reach the guest. Returns 1 if `entry`
 * (a "KEY=VALUE" string) should be dropped. */
int alr_env_is_blocked(const char *entry);

#endif /* ALR_EXEC_RULE_H */
