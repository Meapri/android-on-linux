#include "alr_elf.h"

#include <string.h>

int alr_parse_shebang(const char *line, size_t len, struct alr_shebang *sb)
{
    size_t i = 0, j;

    memset(sb, 0, sizeof *sb);

    /* The kernel truncates the whole line at BINPRM_BUF_SIZE-2 and cuts at the
     * first newline; anything past that simply does not exist. */
    for (j = 0; j < len; j++)
        if (line[j] == '\n' || line[j] == '\0') { len = j; break; }
    if (len > ALR_SHEBANG_MAX) len = ALR_SHEBANG_MAX;

    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return 0;

    j = 0;
    while (i < len && line[i] != ' ' && line[i] != '\t')
        sb->interp[j++] = line[i++];
    sb->interp[j] = '\0';
    if (j == 0) return 0;

    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) return 1;

    /* ONE argument, unsplit: "#!/usr/bin/env -S foo bar" gives argv[1] =
     * "-S foo bar", not three arguments.  Trailing blanks are stripped. */
    j = len;
    while (j > i && (line[j - 1] == ' ' || line[j - 1] == '\t')) j--;
    memcpy(sb->arg, line + i, j - i);
    sb->arg[j - i] = '\0';
    sb->has_arg = (j > i);
    return 1;
}

static int find_interp(alr_pread_fn pread, void *ctx,
                       const struct alr_ehdr64 *eh,
                       char *out, size_t outsz, int *found)
{
    uint16_t i;
    *found = 0;

    if (eh->e_phentsize != sizeof(struct alr_phdr64)) return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > 512) return 0;

    for (i = 0; i < eh->e_phnum; i++) {
        struct alr_phdr64 ph;
        uint64_t off = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
        if (!pread(ctx, &ph, sizeof ph, off)) return 0;
        if (ph.p_type != ALR_PT_INTERP) continue;
        if (ph.p_filesz == 0 || ph.p_filesz > outsz) return 0;
        if (!pread(ctx, out, (size_t)ph.p_filesz, ph.p_offset)) return 0;
        out[ph.p_filesz - 1] = '\0';   /* defensive: PT_INTERP is NUL-terminated */
        *found = 1;
        return 1;
    }
    return 1;
}

enum alr_exe_kind alr_classify(const unsigned char *head, size_t n,
                               alr_pread_fn pread, void *ctx,
                               struct alr_shebang *sb,
                               char *interp_out, size_t interp_sz)
{
    struct alr_ehdr64 eh;

    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        if (!sb) return ALR_EXE_UNSUPPORTED;
        if (!alr_parse_shebang((const char *)head + 2, n - 2, sb))
            return ALR_EXE_UNSUPPORTED;
        return ALR_EXE_SHEBANG;
    }

    if (n < sizeof eh) return ALR_EXE_UNSUPPORTED;
    if (memcmp(head, "\177ELF", 4) != 0) return ALR_EXE_UNSUPPORTED;

    memcpy(&eh, head, sizeof eh);
    if (eh.e_ident[4] != 2)  return ALR_EXE_UNSUPPORTED;   /* ELFCLASS64   */
    if (eh.e_ident[5] != 1)  return ALR_EXE_UNSUPPORTED;   /* ELFDATA2LSB  */
    if (eh.e_machine != ALR_EM_AARCH64) return ALR_EXE_UNSUPPORTED;
    if (eh.e_type != ALR_ET_EXEC && eh.e_type != ALR_ET_DYN)
        return ALR_EXE_UNSUPPORTED;

    if (pread && interp_out && interp_sz) {
        int found = 0;
        interp_out[0] = '\0';
        if (find_interp(pread, ctx, &eh, interp_out, interp_sz, &found) && found)
            return ALR_EXE_ELF_DYNAMIC;
    }
    return ALR_EXE_ELF_STATIC;
}
