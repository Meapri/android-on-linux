/* Host-side tests for alr_elf.{h,c} and alr_exec_rule.{h,c}.
 * Emits ALR ELF CLASSIFY and ALR EXEC RULE TESTS acceptance strings. */
#include "alr_elf.h"
#include "alr_exec_rule.h"

#include <stdio.h>
#include <string.h>

static int pass, fail;

static void ck(int ok, const char *what, const char *got, const char *want)
{
    if (ok) { pass++; return; }
    fail++;
    fprintf(stderr, "  FAIL %s\n       got =%s\n       want=%s\n",
            what, got ? got : "(null)", want ? want : "(null)");
}

/* ── shebang parsing (Linux binfmt_script semantics) ───────────────────── */

static void t_shebang(void)
{
    struct alr_shebang sb;

    ck(alr_parse_shebang("/bin/sh", 7, &sb) && !strcmp(sb.interp, "/bin/sh")
           && !sb.has_arg,
       "shebang plain", sb.interp, "/bin/sh");

    ck(alr_parse_shebang("  /bin/sh  ", 10, &sb) && !strcmp(sb.interp, "/bin/sh")
           && !sb.has_arg,
       "shebang leading/trailing blanks", sb.interp, "/bin/sh");

    /* THE Linux rule people get wrong: the argument is ONE unsplit string. */
    ck(alr_parse_shebang("/usr/bin/env -S foo bar", 23, &sb)
           && !strcmp(sb.interp, "/usr/bin/env")
           && sb.has_arg && !strcmp(sb.arg, "-S foo bar"),
       "shebang single unsplit arg", sb.arg, "-S foo bar");

    ck(alr_parse_shebang("/bin/sh\t-e\t", 11, &sb)
           && !strcmp(sb.interp, "/bin/sh") && !strcmp(sb.arg, "-e"),
       "shebang tab separated", sb.arg, "-e");

    ck(alr_parse_shebang("/bin/sh -x\nrest", 15, &sb)
           && !strcmp(sb.interp, "/bin/sh") && !strcmp(sb.arg, "-x"),
       "shebang stops at newline", sb.arg, "-x");

    ck(!alr_parse_shebang("   ", 3, &sb), "shebang blank line rejected", "", "reject");
    ck(!alr_parse_shebang("", 0, &sb), "shebang empty rejected", "", "reject");

    {   /* Over-long lines are truncated, not overflowed. */
        char big[600];
        memset(big, 'x', sizeof big);
        big[0] = '/';
        ck(alr_parse_shebang(big, sizeof big, &sb)
               && strlen(sb.interp) <= ALR_SHEBANG_MAX,
           "shebang truncates at 255", "(len)", "<=255");
    }
}

/* ── ELF classification ───────────────────────────────────────────────── */

static unsigned char elf_hdr[256];
static struct alr_phdr64 fake_ph;
static const char *fake_interp = "/lib/ld-linux-aarch64.so.1";

static int mem_pread(void *ctx, void *dst, size_t len, uint64_t off)
{
    (void)ctx;
    if (off == 0x40 && len == sizeof fake_ph) { memcpy(dst, &fake_ph, len); return 1; }
    if (off == 0x1000) {
        size_t n = strlen(fake_interp) + 1;
        if (len > n) return 0;
        memcpy(dst, fake_interp, len);
        return 1;
    }
    return 0;
}

static void build_elf(int etype, int with_interp)
{
    struct alr_ehdr64 eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, "\177ELF", 4);
    eh.e_ident[4] = 2;  /* ELFCLASS64  */
    eh.e_ident[5] = 1;  /* ELFDATA2LSB */
    eh.e_type = (uint16_t)etype;
    eh.e_machine = ALR_EM_AARCH64;
    eh.e_phoff = 0x40;
    eh.e_phentsize = sizeof(struct alr_phdr64);
    eh.e_phnum = 1;
    memset(elf_hdr, 0, sizeof elf_hdr);
    memcpy(elf_hdr, &eh, sizeof eh);

    memset(&fake_ph, 0, sizeof fake_ph);
    fake_ph.p_type = with_interp ? ALR_PT_INTERP : ALR_PT_LOAD;
    fake_ph.p_offset = 0x1000;
    fake_ph.p_filesz = strlen(fake_interp) + 1;
}

static void t_elf(void)
{
    struct alr_shebang sb;
    char interp[256];
    enum alr_exe_kind k;

    build_elf(ALR_ET_DYN, 1);
    k = alr_classify(elf_hdr, sizeof elf_hdr, mem_pread, NULL, &sb,
                     interp, sizeof interp);
    ck(k == ALR_EXE_ELF_DYNAMIC && !strcmp(interp, fake_interp),
       "ELF dynamic detected with PT_INTERP", interp, fake_interp);

    build_elf(ALR_ET_EXEC, 0);
    k = alr_classify(elf_hdr, sizeof elf_hdr, mem_pread, NULL, &sb,
                     interp, sizeof interp);
    ck(k == ALR_EXE_ELF_STATIC, "ELF static (no PT_INTERP)",
       k == ALR_EXE_ELF_STATIC ? "static" : "other", "static");

    {   /* wrong machine -> unsupported (we are aarch64 only) */
        struct alr_ehdr64 eh;
        build_elf(ALR_ET_DYN, 1);
        memcpy(&eh, elf_hdr, sizeof eh);
        eh.e_machine = 62;             /* x86-64 */
        memcpy(elf_hdr, &eh, sizeof eh);
        k = alr_classify(elf_hdr, sizeof elf_hdr, mem_pread, NULL, &sb,
                         interp, sizeof interp);
        ck(k == ALR_EXE_UNSUPPORTED, "non-aarch64 ELF rejected", "", "unsupported");
    }

    {   /* 32-bit ELF -> unsupported */
        struct alr_ehdr64 eh;
        build_elf(ALR_ET_DYN, 1);
        memcpy(&eh, elf_hdr, sizeof eh);
        eh.e_ident[4] = 1;             /* ELFCLASS32 */
        memcpy(elf_hdr, &eh, sizeof eh);
        k = alr_classify(elf_hdr, sizeof elf_hdr, mem_pread, NULL, &sb,
                         interp, sizeof interp);
        ck(k == ALR_EXE_UNSUPPORTED, "ELFCLASS32 rejected", "", "unsupported");
    }

    {   /* shebang wins over everything */
        const unsigned char s[] = "#!/bin/bash\nexit 0\n";
        k = alr_classify(s, sizeof s - 1, NULL, NULL, &sb, NULL, 0);
        ck(k == ALR_EXE_SHEBANG && !strcmp(sb.interp, "/bin/bash"),
           "shebang classified", sb.interp, "/bin/bash");
    }
    {   /* random data */
        const unsigned char s[] = "not an executable at all";
        k = alr_classify(s, sizeof s - 1, NULL, NULL, &sb, NULL, 0);
        ck(k == ALR_EXE_UNSUPPORTED, "garbage rejected", "", "unsupported");
    }
}

/* ── LD_PRELOAD construction ──────────────────────────────────────────── */

#define IP "/r/usr/lib/alr/libalr_preload.so"
#define FR "/r/usr/lib/alr/libalr_fakeroot.so"

static void t_preload(void)
{
    char out[1024];
    int changed = 0;               /* see t_env: gcc -Wuninitialized, clang not */

    ck(alr_build_ld_preload(NULL, IP, NULL, out, sizeof out, &changed)
           && !strcmp(out, IP) && changed,
       "preload from empty", out, IP);

    ck(alr_build_ld_preload("", IP, FR, out, sizeof out, &changed)
           && !strcmp(out, FR ":" IP) && changed,
       "fakeroot comes FIRST", out, FR ":" IP);

    ck(alr_build_ld_preload("/guest/x.so", IP, NULL, out, sizeof out, &changed)
           && !strcmp(out, IP ":/guest/x.so") && changed,
       "guest entries preserved after ours", out, IP ":/guest/x.so");

    /* Idempotency: a nested exec must not churn the value. */
    ck(alr_build_ld_preload(IP, IP, NULL, out, sizeof out, &changed)
           && !strcmp(out, IP) && !changed,
       "idempotent when already correct", out, IP "(unchanged)");

    ck(alr_build_ld_preload(FR ":" IP ":/g.so", IP, FR, out, sizeof out, &changed)
           && !changed,
       "idempotent with fakeroot + guest entries", out, "(unchanged)");

    /* Missing fakeroot must be added even if the interposer is present. */
    ck(alr_build_ld_preload(IP, IP, FR, out, sizeof out, &changed)
           && !strcmp(out, FR ":" IP) && changed,
       "fakeroot inserted when absent", out, FR ":" IP);

    /* No duplication of our own entries. */
    ck(alr_build_ld_preload(IP ":" IP, IP, NULL, out, sizeof out, &changed)
           && !changed,
       "already-contains check is element-wise", out, "(unchanged)");

    /* Substring must NOT count as containment. */
    ck(!alr_colon_contains("/r/usr/lib/alr/libalr_preload.so.bak", IP),
       "colon_contains is element-wise, not substring", "", "no match");
    ck(alr_colon_contains("a:" IP ":b", IP), "colon_contains finds middle", "", "match");
    ck(alr_colon_contains(IP, IP), "colon_contains finds sole", "", "match");
    ck(!alr_colon_contains("", IP), "colon_contains empty list", "", "no match");

    /* Overflow must fail cleanly, not truncate into a wrong path. */
    ck(!alr_build_ld_preload("x", IP, FR, out, 8, &changed),
       "overflow rejected", "", "fail");
}

/* ── env filtering / bionic boundary ──────────────────────────────────── */

static void t_env(void)
{
    /* Initialised because gcc cannot see through the && short-circuit and
     * -Wuninitialized fires (clang does not).  "" is also the right poison:
     * if a callee ever fails to set v, the assertion reports an empty value
     * rather than dereferencing garbage or NULL. */
    const char *v = "";
    ck(alr_split_env("KEY=val", &v) == 3 && !strcmp(v, "val"),
       "split_env normal", v, "val");
    ck(alr_split_env("BARE", &v) == 4 && !*v, "split_env bare key", v, "");
    ck(alr_split_env("K=", &v) == 1 && !*v, "split_env empty value", v, "");
    ck(alr_split_env("K=a=b", &v) == 1 && !strcmp(v, "a=b"),
       "split_env splits at FIRST =", v, "a=b");

    ck(alr_env_is_blocked("ANDROID_ROOT=/system"), "block ANDROID_ROOT", "", "blocked");
    ck(alr_env_is_blocked("BOOTCLASSPATH=x"), "block BOOTCLASSPATH", "", "blocked");
    ck(alr_env_is_blocked("ANDROID_SOCKET_zygote=9"), "block ANDROID_SOCKET_*", "", "blocked");
    ck(alr_env_is_blocked("TERMUX_VERSION=0.118.3"), "block TERMUX_*", "", "blocked");
    ck(alr_env_is_blocked("PREFIX=/data/data/com.termux/files/usr"),
       "block PREFIX", "", "blocked");
    ck(!alr_env_is_blocked("PATH=/usr/bin"), "keep PATH", "", "kept");
    ck(!alr_env_is_blocked("TERM=xterm-256color"), "keep TERM", "", "kept");
    ck(!alr_env_is_blocked("HOME=/root"), "keep HOME", "", "kept");
    /* TERM must not be caught by the TERMUX_ family rule. */
    ck(!alr_env_is_blocked("TERM=x"), "TERM is not TERMUX_", "", "kept");

    {
        const char *px = "/data/data/com.termux/files/usr";
        ck(alr_is_bionic_target("/system/bin/am", px), "bionic /system", "", "yes");
        ck(alr_is_bionic_target("/apex/com.android.art/bin/x", px), "bionic /apex", "", "yes");
        ck(alr_is_bionic_target("/data/data/com.termux/files/usr/bin/termux-open", px),
           "bionic $PREFIX", "", "yes");
        ck(!alr_is_bionic_target("/data/alr/ubuntu/usr/bin/git", px),
           "guest path is not bionic", "", "no");
        /* component boundary */
        ck(!alr_is_bionic_target("/systemx/bin", px), "/systemx is not /system", "", "no");
    }
}

int main(void)
{
    t_shebang();
    t_elf();
    printf("ALR ELF CLASSIFY: %s\n", fail ? "FAIL" : "PASS");
    {
        int before = fail;
        t_preload();
        t_env();
        printf("ALR EXEC RULE TESTS: %s\n", fail > before ? "FAIL" : "PASS");
    }
    printf("  assertions: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
