/* alr_elf.h — minimal ELF64/aarch64 classifier.
 *
 * Answers the one question the exec path needs: given the first N bytes of a
 * file (plus a way to read its program headers), is this
 *   - a dynamic ELF   -> must be launched via the guest ld.so   (ADR 0002)
 *   - a static ELF    -> execve directly, but it runs UNHOOKED
 *   - a #! script     -> we must emulate binfmt_script ourselves
 *   - something else  -> ENOEXEC
 *
 * Deliberately does NOT map or load anything.  No Linux headers: the struct
 * definitions are bundled so this compiles on macOS for host tests.
 */
#ifndef ALR_ELF_H
#define ALR_ELF_H

#include <stddef.h>
#include <stdint.h>

/* Kernel's limit for the whole "#!" line (fs/binfmt_script.c). */
#define ALR_SHEBANG_MAX 255
/* Kernel's BINPRM_MAX_RECURSION. */
#define ALR_SHEBANG_MAX_DEPTH 4
/* Bytes we need to classify. */
#define ALR_PROBE_BYTES 256

enum alr_exe_kind {
    ALR_EXE_UNSUPPORTED = 0,
    ALR_EXE_ELF_DYNAMIC,        /* has PT_INTERP  -> needs guest ld.so */
    ALR_EXE_ELF_STATIC,         /* no PT_INTERP   -> unhookable        */
    ALR_EXE_SHEBANG
};

struct alr_shebang {
    char interp[ALR_SHEBANG_MAX + 1];  /* e.g. "/usr/bin/env"        */
    char arg[ALR_SHEBANG_MAX + 1];     /* single unsplit arg, or ""  */
    int  has_arg;
};

/* --- bundled ELF64 definitions (no <elf.h> dependency) ------------------ */
#define ALR_EI_NIDENT 16
#define ALR_ET_EXEC   2
#define ALR_ET_DYN    3
#define ALR_EM_AARCH64 183
#define ALR_PT_LOAD   1
#define ALR_PT_INTERP 3

struct alr_ehdr64 {
    unsigned char e_ident[ALR_EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct alr_phdr64 {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

/* Reader callback so this stays free of any I/O dependency: must read
 * exactly `len` bytes at `off` into `dst`, returning 1 on success. */
typedef int (*alr_pread_fn)(void *ctx, void *dst, size_t len, uint64_t off);

/* Classify from the first `n` bytes of the file (n may be < ALR_PROBE_BYTES).
 * For ELF, `pread`/`ctx` are used to walk program headers looking for
 * PT_INTERP; pass NULL to skip that (result is then ELF_STATIC vs DYNAMIC
 * undetermined and reported as ALR_EXE_ELF_STATIC).
 * On ALR_EXE_SHEBANG, *sb is filled in. */
enum alr_exe_kind alr_classify(const unsigned char *head, size_t n,
                               alr_pread_fn pread, void *ctx,
                               struct alr_shebang *sb,
                               char *interp_out, size_t interp_sz);

/* Parse a "#!" line exactly the way fs/binfmt_script.c does.
 * `line` is the content after "#!", NUL- or newline-terminated.
 * Linux semantics: leading blanks skipped, then the interpreter runs to the
 * next blank, then ONE argument which is NOT split further and keeps its
 * internal blanks.  Returns 1 on success, 0 if no interpreter. */
int alr_parse_shebang(const char *line, size_t len, struct alr_shebang *sb);

#endif /* ALR_ELF_H */
