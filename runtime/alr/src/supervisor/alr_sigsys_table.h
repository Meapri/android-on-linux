/* alr — SIGSYS emulation table.
 *
 * GENERATED FROM A REAL DEVICE by `alr doctor`, not from documentation.
 *   device   Samsung SM-X236N  (MediaTek MT8775 / mt6878)
 *   android  16 (SDK 36), SELinux Enforcing
 *   kernel   6.1.145-android14-11 aarch64
 *   context  uid=10297  u:r:untrusted_app_27:s0  Seccomp=2
 *   date     2026-08-02
 *   result   239 of 468 syscalls blocked (SIGSYS / SYS_SECCOMP)
 *
 * CONFIRMED ON A SECOND DEVICE, 2026-08-03 (docs/evidence/2026-08-03-m19-*):
 *   device   Samsung SM-S937N  (Snapdragon 8 Elite SM8750 / "sun")
 *   kernel   6.6.98-android15-8 aarch64      <- different vendor AND major.minor
 *   context  uid=10447  u:r:untrusted_app_27:s0  Seccomp=2
 *   result   239 of 468 blocked -- and the SETS ARE IDENTICAL.  Not merely the
 *            same count: a full 239-vs-239 diff of the two sweeps comes back
 *            with zero on either side.
 *              scripts/diff-sweep.sh docs/evidence/sweeps/mediatek-*.txt \
 *                                    docs/evidence/sweeps/snapdragon-*.txt
 *
 * The negative claims below hold there too: 147/148/150 are not blocked on
 * either device.  That is the load-bearing detail -- a coarse range filter
 * would have swept those up with their neighbours 143-152, so their survival
 * on both devices is a fingerprint match rather than a coincidence of totals.
 *
 * SCOPE: this table is a CONSTANT for Android 16, which is the only release
 * alr supports (docs/adr/0007-android-16-only.md).  Two devices differing in
 * SoC vendor and kernel (6.1-android14/MediaTek vs 6.6-android15/Qualcomm)
 * produce identical sets, so it ships as a default rather than being
 * regenerated per phone.
 *
 * One axis is still unvaried and it is NOT the release: both reference
 * devices are SAMSUNG.  The allowlist comes from the OEM's platform build of
 * bionic, not from the SoC vendor, so a different OEM on Android 16 is
 * unmeasured -- and there is no way to obtain such a device, so it stays that
 * way.  That is a permanent limit of the evidence, not a pending task.
 *
 * It differs from the Android 12-15 case in kind.  The release axis is KNOWN
 * to vary (365 -> 392 allowlist lines), so extrapolating across it would be
 * unjustified and those releases are out of scope.  The OEM axis has no
 * evidence of variation -- SECCOMP_ALLOWLIST_*.TXT ships in AOSP bionic and
 * no case of an OEM patching it is known here -- but not knowing is not
 * measuring.  So this table is neither claimed nor denied for other OEMs;
 * `alr doctor`'s sweep is what tells a user on one.
 *
 * On ANY OTHER Android release this table is an unvalidated guess.  The
 * allowlist grows with each release (android12 365 lines -> android16 392),
 * and that axis was deliberately not measured -- it is out of scope, not
 * pending.  `alr doctor` prints the Android release and says UNSUPPORTED
 * when it is not 16; its P2 sweep is the only thing that would reveal a
 * mismatch, and scripts/diff-sweep.sh compares it against
 * docs/evidence/sweeps/.
 *
 * Raw sweeps are checked in verbatim under docs/evidence/sweeps/ so the next
 * comparison is a one-liner.  Before that they lived only in the `#if 0` block
 * at the end of this file, which is easy to miss -- the diff above was nearly
 * abandoned as impossible because a grep for the table rows found only the
 * ~30 exceptions and the full set looked lost.
 *
 * Regenerate with:  alr doctor            (see docs/06-cli-spec.md §3)
 * Rationale for the return values:  docs/03-supervisor-spec.md §5
 *
 * Only syscalls whose emulated return is NOT the default belong here.
 * The supervisor returns -ENOSYS for every other blocked syscall, because
 * glibc's "try the new syscall, fall back on ENOSYS" pattern keys on it.
 */
#ifndef ALR_SIGSYS_TABLE_H
#define ALR_SIGSYS_TABLE_H

#include <errno.h>

struct alr_sigsys_ent { long nr; long ret; const char *name; };

/* Default for anything blocked but absent from this table. */
#define ALR_SIGSYS_DEFAULT_RET (-ENOSYS)

static const struct alr_sigsys_ent alr_sigsys_tab[] = {
    /* --- credential drop: must SUCCEED, not fail ----------------------
     * A child lowering privilege via setgid(getgid()) before execve dies
     * here otherwise.  This is where apt's _apt sandbox and gpgv
     * grandchildren disappear.  MEASURED: 147 setresuid and 148/150
     * getres[ug]id are NOT blocked on this device, so they are absent. */
    {  99, 0,       "set_robust_list" },
    { 143, 0,       "setregid" },
    { 144, 0,       "setgid" },
    { 145, 0,       "setreuid" },
    { 146, 0,       "setuid" },
    { 149, 0,       "setresgid" },
    { 151, 0,       "setfsuid" },
    { 152, 0,       "setfsgid" },
    { 159, 0,       "setgroups" },

    /* --- privileged ops with no ENOSYS-keyed glibc fallback ----------- */
    {  39, -EPERM,  "umount2" },
    {  40, -EPERM,  "mount" },
    {  51, -EPERM,  "chroot" },
    { 112, -EPERM,  "clock_settime" },
    { 161, -EPERM,  "sethostname" },
    { 162, -EPERM,  "setdomainname" },
    { 170, -EPERM,  "settimeofday" },
    { 171, -EPERM,  "adjtimex" },
    { 266, -EPERM,  "clock_adjtime" },

    /* --- explicitly named -ENOSYS (same as the default; listed because
     *     these are the ones that actually fire in practice) ---------- */
    {  89, -ENOSYS, "acct" },
    { 100, -ENOSYS, "get_robust_list" },
    { 105, -ENOSYS, "init_module" },
    { 106, -ENOSYS, "delete_module" },
    { 116, -ENOSYS, "syslog" },
    { 224, -ENOSYS, "swapon" },
    { 225, -ENOSYS, "swapoff" },
    { 293, -ENOSYS, "rseq" },
    { 425, -ENOSYS, "io_uring_setup" },
    { 426, -ENOSYS, "io_uring_enter" },
    { 427, -ENOSYS, "io_uring_register" },
    { 449, -ENOSYS, "futex_waitv" },
};

#define ALR_SIGSYS_TAB_LEN (sizeof alr_sigsys_tab / sizeof alr_sigsys_tab[0])

/* Full measured blocked set (device ground truth, for regression diffing).
 * Entries not named above fall through to ALR_SIGSYS_DEFAULT_RET. */
#if 0
blocked = 18, 39, 40, 42, 51, 58, 89, 99, 100, 104, 105, 106, 112, 116, 143, 144,
145, 146, 149, 151, 152, 159, 161, 162, 170, 171, 180, 181, 182, 183, 184,
185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 202, 217,
218, 219, 224, 225, 234, 235, 236, 237, 238, 239, 244, 245, 246, 247, 248,
249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 262, 263, 264, 265,
266, 272, 273, 288, 289, 290, 292, 293, 294, 295, 296, 297, 298, 299, 300,
301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315,
316, 317, 318, 319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330,
331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345,
346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360,
361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375,
376, 377, 378, 379, 380, 381, 382, 383, 384, 385, 386, 387, 388, 389, 390,
391, 392, 393, 394, 395, 396, 397, 398, 399, 400, 401, 402, 403, 404, 405,
406, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 418, 419, 420,
421, 422, 423, 425, 426, 427, 428, 429, 430, 431, 432, 433, 442, 443, 447,
449, 450, 451, 453, 454, 455, 456, 457, 458, 459, 460, 461, 467
#endif

#endif /* ALR_SIGSYS_TABLE_H */
