// alr_reentry.c — ALR static re-entry stub (ADR-003-v2 Option S, PoC).
//
// WHAT THIS IS
// ============
// A freestanding, statically-linked aarch64 ELF with NO PT_INTERP and NO libc
// dependency. It is the in-process ALR loader, repackaged as a *guest-executable
// bootstrap* that the kernel CAN execve directly (a static ELF needs no
// interpreter the Android kernel must resolve).
//
// WHY IT EXISTS (the device finding this fixes)
// =============================================
// ALR runs a glibc-aarch64 guest by mapping the guest ld.so + the ELF IN-PROCESS
// (not via a kernel execve). When the guest later calls execve(rootfs_binary),
// the supervisor traps it at the seccomp pre-exec stage and rewrites the path
// into the rootfs (B-1, device-proven). But across EVERY exec, exec_events=0:
// PTRACE_EVENT_EXEC never fires, because the kernel's execve of a glibc ELF needs
// its PT_INTERP (the guest /lib/ld-linux-aarch64.so.1), which the Android kernel
// CANNOT resolve -> the kernel execve fails, no new image enters execution. So
// dpkg/apt/GIMP-plugin/chromium-multiprocess chains all stall at the first exec.
// ADR-003 (v1) assumed "path-rewrite + seccomp/SEIZE inheritance is enough" — the
// device DISPROVES that. The real fix: the LOADER must RE-MAP the new ELF
// in-process on exec.
//
// HOW THE STUB FIXES IT (Option S)
// ================================
// The supervisor rewrites the guest's execve(target, argv, envp) into
//   execve(<rootfs>/usr/lib/androlinux/alr-reentry, [<target>, argv...], envp)
// i.e. it splices THIS stub's host path into x0 and shifts the original target
// in as argv[1] (the stub's own argv[0] is the stub path). The kernel CAN load
// this stub (static, no PT_INTERP), and crucially the kernel PRESERVES across that
// real execve: (i) the stacked seccomp filter (seccomp.2: filters survive execve),
// (ii) the PTRACE_SEIZE trace + PTRACE_O_TRACEEXEC (ptrace.2: SEIZE survives
// execve, EVENT_EXEC fires), and (iii) the envp the supervisor passed (so
// LD_PRELOAD=<abs rootfs interpose .so> and ALR_ROOTFS reach the guest glibc).
// The stub then maps the guest ld.so + the *real* target ELF in-process — exactly
// like ALR's in-process loader does at first launch — and jumps to ld.so's entry
// with a correct SysV initial SP/auxv. glibc comes up under the inherited filter
// + trace, so path syscalls keep trapping to the supervisor and re-exec works.
//
// SELF-CONTAINED, ON PURPOSE
// ==========================
// Per the R8-A constraints this PoC must NOT touch the working loader
// (runtime_report.cpp / alr_loader*.cpp). So the ELF-mapper + auxv/SP builder
// below are a DELIBERATE ~200-line DUPLICATE of
// runtime_report.cpp:map_elf_image_into_execmem + the initial-stack build around
// alr_enter_guest. TODO(unify-with-loader): once the device proves re-entry, fold
// this and the loader's copy into one shared mapper compiled for both the host
// (.so, via NDK) and the guest (static ELF, via zig) targets.
//
// FREESTANDING NOTES
// ==================
// No libc: _start is the ELF entry, syscalls are inline `svc #0`, and there is no
// crt0/TLS setup. We deliberately keep TLS untouched: glibc's own ld.so sets up
// TPIDR_EL0 once it runs, and this stub never calls anything that reads the TCB.
// Page size comes from AT_PAGESZ in our own auxv (we are a fresh kernel-loaded
// image, so the kernel handed us a real auxv) with a 4096 fallback.

#include <stddef.h>
#include <stdint.h>

// ---- ELF types (freestanding: we declare exactly what we touch) -------------
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word   p_type;
    Elf64_Word   p_flags;
    Elf64_Off    p_offset;
    Elf64_Addr   p_vaddr;
    Elf64_Addr   p_paddr;
    Elf64_Xword  p_filesz;
    Elf64_Xword  p_memsz;
    Elf64_Xword  p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

#define ET_EXEC 2
#define ET_DYN  3
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
// ARM ABI (aaelf64): 1027=R_AARCH64_RELATIVE (base-relative: *slot += base),
// 1032=R_AARCH64_IRELATIVE (ifunc resolver). The old `IRELATIVE 1027` was the
// RELATIVE value — it ran every RELATIVE entry through the ifunc path (calling a
// data address -> SIGILL) and skipped the real IRELATIVEs. Match runtime_report.cpp:36.
#define R_AARCH64_RELATIVE  1027
#define R_AARCH64_IRELATIVE 1032
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)

typedef struct { Elf64_Addr r_offset; Elf64_Xword r_info; Elf64_Sxword r_addend; } Elf64_Rela;

// ---- AT_* auxv tags ---------------------------------------------------------
#define AT_NULL          0
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_BASE          7
#define AT_FLAGS         8
#define AT_ENTRY         9
#define AT_UID           11
#define AT_EUID          12
#define AT_GID           13
#define AT_EGID          14
#define AT_HWCAP         16
#define AT_CLKTCK        17
#define AT_SECURE        23
#define AT_RANDOM        25
#define AT_HWCAP2        26
#define AT_EXECFN        31
#define AT_SYSINFO_EHDR  33

// ---- raw aarch64 syscall numbers (asm-generic, the aarch64 ABI) -------------
#define SYS_openat      56
#define SYS_close       57
#define SYS_read        63
#define SYS_write       64
#define SYS_brk         214
#define SYS_mmap        222
#define SYS_mprotect    226
#define SYS_exit        93
#define SYS_exit_group  94

#define O_RDONLY 0
#define AT_FDCWD (-100)
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)

// ---- inline aarch64 syscalls ------------------------------------------------
static inline long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}
#define sys0(n)            sys6((n),0,0,0,0,0,0)
#define sys1(n,a)          sys6((n),(long)(a),0,0,0,0,0)
#define sys2(n,a,b)        sys6((n),(long)(a),(long)(b),0,0,0,0)
#define sys3(n,a,b,c)      sys6((n),(long)(a),(long)(b),(long)(c),0,0,0)
#define sys4(n,a,b,c,d)    sys6((n),(long)(a),(long)(b),(long)(c),(long)(d),0,0)
#define sys6_(n,a,b,c,d,e,f) sys6((n),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))

static void sys_exit(int code) {
    sys1(SYS_exit_group, code);
    for (;;) { sys1(SYS_exit, code); }
}

// ---- minimal freestanding helpers (no libc) ---------------------------------
static size_t s_len(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static void s_cpy(char* d, const char* s) { while ((*d++ = *s++)) ; }
static void m_cpy(void* d, const void* s, size_t n) {
    char* dd = (char*)d; const char* ss = (const char*)s;
    for (size_t i = 0; i < n; ++i) dd[i] = ss[i];
}
static void m_set(void* d, int c, size_t n) {
    char* dd = (char*)d; for (size_t i = 0; i < n; ++i) dd[i] = (char)c;
}
static int has_prefix(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return 0; } return 1;
}

// Freestanding aarch64 I-cache sync after writing executable code. Under
// -nostdlib the compiler's __builtin___clear_cache lowers to a libgcc __clear_cache
// call we cannot link, so we issue the architectural maintenance sequence inline:
// clean D-cache to PoU (dc cvau), DSB, invalidate I-cache to PoU (ic ivau), DSB,
// ISB. Line sizes come from CTR_EL0 (DminLine/IminLine, log2 words). This is the
// same work libc's __clear_cache does; doing it by hand keeps the stub libc-free.
static void flush_icache(uintptr_t start, uintptr_t end) {
    unsigned long ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    unsigned long dline = 4UL << ((ctr >> 16) & 0xf);  // DminLine: words -> bytes
    unsigned long iline = 4UL << (ctr & 0xf);          // IminLine: words -> bytes
    for (uintptr_t a = start & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t a = start & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}

// One-line diagnostic to fd 2 (stderr) with a tag; helps the integration session
// read what the stub did from logcat without a debugger. Pure write(), no libc.
static void diag(const char* msg) { sys3(SYS_write, 2, msg, s_len(msg)); }
static void diag_hex(const char* tag, unsigned long v) {
    char buf[2 + 16 + 1];
    char* p = buf;
    *p++ = '0'; *p++ = 'x';
    static const char hx[] = "0123456789abcdef";
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int nib = (int)((v >> i) & 0xf);
        if (nib || started || i == 0) { *p++ = hx[nib]; started = 1; }
    }
    *p = 0;
    diag(tag); diag(buf); diag("\n");
}

// Read a whole file into a fixed buffer (caller provides). Returns bytes read, or
// -1 on error. The stub maps modest ELFs (ld.so ~200KB, the target's own header +
// PT_LOADs); a few MB buffer is plenty for the bootstrap target. (A production
// stub would mmap the file; the PoC keeps the read path simple and self-contained.)
static long read_file(const char* path, char* buf, size_t cap) {
    long fd = sys4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t total = 0;
    for (;;) {
        long n = sys3(SYS_read, fd, buf + total, cap - total);
        if (n < 0) { sys1(SYS_close, fd); return -1; }
        if (n == 0) break;
        total += (size_t)n;
        if (total >= cap) { sys1(SYS_close, fd); return -1; }  // too big for PoC buf
    }
    sys1(SYS_close, fd);
    return (long)total;
}

// ---- mapped-image result (mirror of loader's MappedImage) -------------------
typedef struct {
    uintptr_t base;   // load bias: runtime_addr = base + p_vaddr
    uintptr_t entry;  // base + e_entry
    uintptr_t phdr;   // runtime address of THIS image's program headers
    uint16_t  phent;
    uint16_t  phnum;
    int       ok;
} MappedImage;

// Page size: this stub is a fresh kernel-loaded image, so the kernel handed us a
// real auxv; we read AT_PAGESZ from it (set by _start). 4096 fallback.
static unsigned long g_pagesz = 4096;
static inline uintptr_t page_down(uintptr_t v) { return v & ~(uintptr_t)(g_pagesz - 1); }
static inline uintptr_t page_up(uintptr_t v)   { return (v + g_pagesz - 1) & ~(uintptr_t)(g_pagesz - 1); }

// Map every PT_LOAD of `img` (a full ELF file already in memory) into anonymous
// memory with the W^X-safe RW->memcpy->RX MAP_FIXED path, then apply
// R_AARCH64_IRELATIVE. This is a SELF-CONTAINED duplicate of
// runtime_report.cpp:map_elf_image_into_execmem (TODO unify with loader). It uses
// the runtime page size (sysconf(_SC_PAGESIZE) equivalent = AT_PAGESZ) rather than
// the loader's hard-coded 0xfff mask, so a 16K-page device is handled correctly.
static MappedImage map_elf_image(const char* img, size_t len, const char* tag) {
    MappedImage R; m_set(&R, 0, sizeof(R));
    if (len < sizeof(Elf64_Ehdr)) { diag(tag); diag("SHORT\n"); return R; }
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)img;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(img + eh->e_phoff);

    uintptr_t min_v = ~(uintptr_t)0, max_v = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        uintptr_t s = page_down((uintptr_t)ph[i].p_vaddr);
        uintptr_t e = (uintptr_t)(ph[i].p_vaddr + ph[i].p_memsz);
        if (s < min_v) min_v = s;
        if (e > max_v) max_v = e;
    }
    if (min_v == ~(uintptr_t)0) { diag(tag); diag("NO_LOAD\n"); return R; }

    size_t span = page_up(max_v - min_v);
    uintptr_t base = 0;
    if (eh->e_type == ET_DYN) {
        // PROT_NONE reservation for a contiguous base (PIE / ld.so).
        void* reserve = (void*)sys6_(SYS_mmap, 0, span, PROT_NONE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserve == MAP_FAILED) { diag(tag); diag("RESERVE_FAIL\n"); return R; }
        base = (uintptr_t)reserve - min_v;
    }

    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        uintptr_t seg_start = base + page_down((uintptr_t)ph[i].p_vaddr);
        uintptr_t v_off     = (uintptr_t)ph[i].p_vaddr - page_down((uintptr_t)ph[i].p_vaddr);
        size_t    map_len   = page_up(v_off + ph[i].p_memsz);
        void* m = (void*)sys6_(SYS_mmap, seg_start, map_len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED) { diag(tag); diag("SEG_MAP_FAIL\n"); return R; }
        m_cpy((void*)(base + ph[i].p_vaddr), img + ph[i].p_offset, ph[i].p_filesz);
        int prot = 0;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        if (sys3(SYS_mprotect, seg_start, map_len, prot) != 0) {
            diag(tag); diag("SEG_PROT_FAIL\n"); return R;
        }
        if (prot & PROT_EXEC) {
            flush_icache(seg_start, seg_start + map_len);
        }
    }

    // Apply relocations via PT_DYNAMIC's DT_RELA (required for ld.so, which is
    // section-header-stripped). Handle BOTH R_AARCH64_RELATIVE (base-relative:
    // *(base+offset) = base + addend, the bulk of a static-PIE/ET_DYN image's
    // startup fixups) and R_AARCH64_IRELATIVE (ifunc resolver). The old code only
    // matched "IRELATIVE 1027" — actually the RELATIVE value — so it called every
    // RELATIVE slot as an ifunc (data-as-code -> SIGILL) and skipped the true
    // IRELATIVEs (1032). NB: ifunc resolvers may read HWCAP; we pass 0 for the PoC
    // (the loader threads real AT_HWCAP — a TODO to wire here too).
    const Elf64_Phdr* dynph = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) { dynph = &ph[i]; break; }
    }
    int irel = 0;
    if (dynph) {
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)(base + dynph->p_vaddr);
        uintptr_t rela = 0; size_t relasz = 0, relaent = sizeof(Elf64_Rela);
        for (; dyn->d_tag != DT_NULL; ++dyn) {
            if (dyn->d_tag == DT_RELA)    rela    = base + dyn->d_un.d_ptr;
            if (dyn->d_tag == DT_RELASZ)  relasz  = (size_t)dyn->d_un.d_val;
            if (dyn->d_tag == DT_RELAENT) relaent = (size_t)dyn->d_un.d_val;
        }
        if (rela && relaent) {
            for (size_t off = 0; off < relasz; off += relaent) {
                const Elf64_Rela* r = (const Elf64_Rela*)(rela + off);
                unsigned long rtype = ELF64_R_TYPE(r->r_info);
                if (rtype == R_AARCH64_RELATIVE) {
                    *(uintptr_t*)(base + r->r_offset) = base + (uintptr_t)r->r_addend;
                } else if (rtype == R_AARCH64_IRELATIVE) {
                    typedef unsigned long (*Resolver)(unsigned long, const void*);
                    Resolver res = (Resolver)(base + r->r_addend);
                    *(uint64_t*)(base + r->r_offset) = res(0, 0);  // TODO: thread HWCAP
                    ++irel;
                }
            }
        }
    }
    diag_hex(tag, (unsigned long)irel);

    // AT_PHDR: prefer PT_PHDR; else the PT_LOAD covering e_phoff; else base+e_phoff.
    uintptr_t at_phdr = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) { at_phdr = base + ph[i].p_vaddr; break; }
    }
    if (!at_phdr) {
        for (int i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type == PT_LOAD && eh->e_phoff >= ph[i].p_offset &&
                eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                at_phdr = base + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }
    }
    if (!at_phdr) at_phdr = base + eh->e_phoff;

    R.base  = base;
    R.entry = base + eh->e_entry;
    R.phdr  = at_phdr;
    R.phent = eh->e_phentsize;
    R.phnum = eh->e_phnum;
    R.ok    = 1;
    return R;
}

// Transfer control to the guest entry with a freshly built SysV stack. Mirrors the
// device-proven loader's alr_enter_guest(sp, entry, tcb) (runtime_report.cpp
// L1160-1169). Even as a fresh kernel-execve'd static ELF, TPIDR_EL0 starts at 0,
// and glibc's pre-TLS-init startup (_dl_aux_init/__tunables_init) is built with
// -fstack-protector and reads the canary via THREAD_SELF (=TPIDR_EL0) BEFORE
// __libc_setup_tls installs glibc's own TLS — a TP=0 load faults near address 0
// (SIGSEGV). Point TP at a fresh ZEROED 16 KiB region (caller passes its center) so
// those reads land in mapped-zero until ld.so/__libc_setup_tls takes over. Naked:
// never returns.
__attribute__((noreturn)) static void enter_guest(void* sp, void* entry, void* tcb) {
    __asm__ volatile(
        "msr tpidr_el0, %2\n"    // clean zeroed TCB: canary/THREAD_SELF reads land in mapped-zero
        "mov sp, %0\n"
        "mov x0, #0\n"
        "br  %1\n"
        :: "r"(sp), "r"(entry), "r"(tcb) : "memory");
    __builtin_unreachable();
}

// ---- the rootfs prefix (guest ld.so lives at <rootfs><interp>) --------------
// The supervisor passes ALR_ROOTFS in envp; the stub reads it to find the guest
// ld.so on disk. (The interp string itself comes from the target's PT_INTERP.)
static const char* getenv_from(char** envp, const char* key) {
    size_t kl = s_len(key);
    for (char** e = envp; *e; ++e) {
        const char* s = *e;
        if (has_prefix(s, key) && s[kl] == '=') return s + kl + 1;
    }
    return 0;
}

// File buffers: static .bss, no heap. Sized for the target ELF and ld.so. A real
// dpkg/sh/dpkg-deb binary is a few hundred KB statically-described (PT_LOAD file
// images); 8 MiB each is generous headroom for the PoC.
#define FILEBUF_CAP (8u * 1024u * 1024u)
static char g_target_buf[FILEBUF_CAP];
static char g_interp_buf[FILEBUF_CAP];
static char g_interp_path[1024];  // <rootfs> + interp string

// ===========================================================================
//  _start — the freestanding ELF entry. The kernel hands us the SysV stack:
//    [ argc ][ argv0..argvN ][ NULL ][ envp0.. ][ NULL ][ auxv.. ][ AT_NULL ]
//  argv[0] = this stub's own path; argv[1] = the REAL guest target ELF path the
//  supervisor spliced in; argv[2..] = the guest's original argv[1..]; envp =
//  the guest envp (with LD_PRELOAD/ALR_ROOTFS the supervisor preserved/injected).
// ===========================================================================
__attribute__((used, noreturn)) void alr_reentry_main(unsigned long* sp_in) {
    unsigned long argc = sp_in[0];
    char** argv = (char**)(sp_in + 1);
    char** envp = (char**)(sp_in + 1 + argc + 1);

    // Walk to the auxv (just past the envp NULL) and read AT_PAGESZ / AT_* the
    // kernel gave US, so we can re-synthesize a correct auxv for the guest.
    unsigned long* auxp = (unsigned long*)envp;
    while (*auxp) ++auxp;   // skip envp
    ++auxp;                 // skip envp NULL -> first auxv tag
    unsigned long my_hwcap = 0, my_hwcap2 = 0, my_random = 0, my_sysinfo = 0;
    unsigned long my_uid = 0, my_euid = 0, my_gid = 0, my_egid = 0, my_clktck = 100;
    for (unsigned long* a = auxp; a[0] != AT_NULL; a += 2) {
        switch (a[0]) {
            case AT_PAGESZ:       if (a[1]) g_pagesz = a[1]; break;
            case AT_HWCAP:        my_hwcap = a[1]; break;
            case AT_HWCAP2:       my_hwcap2 = a[1]; break;
            case AT_RANDOM:       my_random = a[1]; break;
            case AT_SYSINFO_EHDR: my_sysinfo = a[1]; break;
            case AT_UID:          my_uid = a[1]; break;
            case AT_EUID:         my_euid = a[1]; break;
            case AT_GID:          my_gid = a[1]; break;
            case AT_EGID:         my_egid = a[1]; break;
            case AT_CLKTCK:       my_clktck = a[1]; break;
            default: break;
        }
    }

    diag("ALR-REENTRY: start\n");
    if (argc < 2) { diag("ALR-REENTRY: no target argv[1]\n"); sys_exit(64); }
    const char* target = argv[1];   // the REAL guest ELF the supervisor spliced in
    diag("ALR-REENTRY: target="); diag(target); diag("\n");

    // ---- read the target ELF -------------------------------------------------
    long tlen = read_file(target, g_target_buf, FILEBUF_CAP);
    if (tlen < (long)sizeof(Elf64_Ehdr) ||
        g_target_buf[0] != 0x7f || g_target_buf[1] != 'E' ||
        g_target_buf[2] != 'L'  || g_target_buf[3] != 'F') {
        diag("ALR-REENTRY: target open/ELF fail\n"); sys_exit(72);
    }

    // ---- find PT_INTERP (dynamic) or note static -----------------------------
    const Elf64_Ehdr* teh = (const Elf64_Ehdr*)g_target_buf;
    const Elf64_Phdr* tph = (const Elf64_Phdr*)(g_target_buf + teh->e_phoff);
    const char* interp_str = 0;
    for (int i = 0; i < teh->e_phnum; ++i) {
        if (tph[i].p_type == PT_INTERP) { interp_str = g_target_buf + tph[i].p_offset; break; }
    }
    int dynamic = (interp_str != 0);
    diag(dynamic ? "ALR-REENTRY: DYN\n" : "ALR-REENTRY: STATIC\n");

    // ---- map the target ELF --------------------------------------------------
    MappedImage prog = map_elf_image(g_target_buf, (size_t)tlen, "ALR-REENTRY PROG IREL=");
    if (!prog.ok) { diag("ALR-REENTRY: prog map fail\n"); sys_exit(72); }

    // ---- map the guest ld.so (dynamic only): <rootfs> + interp_str -----------
    MappedImage interp; m_set(&interp, 0, sizeof(interp));
    if (dynamic) {
        const char* rootfs = getenv_from(envp, "ALR_ROOTFS");
        if (!rootfs) { diag("ALR-REENTRY: no ALR_ROOTFS\n"); sys_exit(75); }
        size_t rl = s_len(rootfs);
        if (rl + s_len(interp_str) + 1 >= sizeof(g_interp_path)) {
            diag("ALR-REENTRY: interp path too long\n"); sys_exit(75);
        }
        s_cpy(g_interp_path, rootfs);
        s_cpy(g_interp_path + rl, interp_str);   // interp_str begins with '/'
        diag("ALR-REENTRY: interp="); diag(g_interp_path); diag("\n");
        long ilen = read_file(g_interp_path, g_interp_buf, FILEBUF_CAP);
        if (ilen < (long)sizeof(Elf64_Ehdr) ||
            g_interp_buf[0] != 0x7f || g_interp_buf[1] != 'E') {
            diag("ALR-REENTRY: interp open/ELF fail\n"); sys_exit(75);
        }
        interp = map_elf_image(g_interp_buf, (size_t)ilen, "ALR-REENTRY INTERP IREL=");
        if (!interp.ok) { diag("ALR-REENTRY: interp map fail\n"); sys_exit(77); }
    }
    diag("ALR-REENTRY: mapped\n");

    // ---- build the guest's SysV initial stack --------------------------------
    // Guest argv = the ORIGINAL guest argv: argv[1] (the real target) becomes
    // guest argv[0], and the supervisor-shifted argv[2..] become guest argv[1..].
    // So guest argc = (our argc - 1), guest argv = &argv[1]. envp passes through.
    const unsigned long g_argc = argc - 1;
    char** g_argv = &argv[1];

    // Fresh stack, like the loader: 8 MiB = the Linux default RLIMIT_STACK (was 512 KiB).
    // glibc GUI apps reserve large on-stack frames (e.g. qalculate-gtk load_preferences()
    // ~978 KiB) that overrun a 512 KiB stack into the guard page → SIGSEGV. mmap commits
    // lazily, so the larger VIRTUAL size is cheap. General fix for any large-frame guest.
    const size_t stack_size = 8u * 1024u * 1024u;
    void* stk = (void*)sys6_(SYS_mmap, 0, stack_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) { diag("ALR-REENTRY: stack fail\n"); sys_exit(74); }
    uintptr_t top = (uintptr_t)stk + stack_size;

    // 16-byte AT_RANDOM block (copy ours if the kernel gave one).
    top -= 16;
    if (my_random) m_cpy((void*)top, (void*)my_random, 16);
    else m_set((void*)top, 0x5a, 16);
    uintptr_t at_random = top;

    // Push argv strings (high -> low).
    #define MAX_ARGV 256
    uintptr_t argv_ptrs[MAX_ARGV];
    unsigned long g_argc_clamped = g_argc < MAX_ARGV ? g_argc : MAX_ARGV;
    for (unsigned long i = g_argc_clamped; i-- > 0;) {
        size_t n = s_len(g_argv[i]) + 1;
        top -= n; m_cpy((void*)top, g_argv[i], n); argv_ptrs[i] = top;
    }
    uintptr_t argv0 = g_argc_clamped ? argv_ptrs[0] : 0;

    // Push envp strings (high -> low).
    #define MAX_ENVP 256
    uintptr_t envp_ptrs[MAX_ENVP];
    unsigned long n_env = 0;
    for (char** e = envp; *e && n_env < MAX_ENVP; ++e) ++n_env;
    for (unsigned long i = n_env; i-- > 0;) {
        size_t n = s_len(envp[i]) + 1;
        top -= n; m_cpy((void*)top, envp[i], n); envp_ptrs[i] = top;
    }

    // auxv describing the PROGRAM to ld.so (AT_PHDR/AT_ENTRY = program's; AT_BASE =
    // interpreter load base). Static: AT_BASE 0, entry = program entry.
    uint64_t aux[] = {
        AT_PHDR, prog.phdr,
        AT_PHENT, prog.phent,
        AT_PHNUM, prog.phnum,
        AT_PAGESZ, g_pagesz,
        AT_BASE, dynamic ? interp.base : 0,
        AT_FLAGS, 0,
        AT_ENTRY, prog.entry,
        AT_EXECFN, argv0,
        AT_UID, my_uid, AT_EUID, my_euid, AT_GID, my_gid, AT_EGID, my_egid,
        AT_HWCAP, my_hwcap, AT_HWCAP2, my_hwcap2,
        AT_CLKTCK, my_clktck,
        AT_RANDOM, at_random,
        AT_SECURE, 0,
        AT_SYSINFO_EHDR, my_sysinfo,
        AT_NULL, 0,
    };
    const size_t n_aux = sizeof(aux) / sizeof(aux[0]);
    const size_t n_words = 1 + (g_argc_clamped + 1) + (n_env + 1) + n_aux;
    uintptr_t start = (top - n_words * 8) & ~(uintptr_t)0xf;
    uint64_t* w = (uint64_t*)start;
    size_t k = 0;
    w[k++] = g_argc_clamped;
    for (unsigned long i = 0; i < g_argc_clamped; ++i) w[k++] = argv_ptrs[i];
    w[k++] = 0;
    for (unsigned long i = 0; i < n_env; ++i) w[k++] = envp_ptrs[i];
    w[k++] = 0;
    for (size_t i = 0; i < n_aux; ++i) w[k++] = aux[i];

    uintptr_t jump_entry = dynamic ? interp.entry : prog.entry;
    diag_hex("ALR-REENTRY E@", (unsigned long)jump_entry);
    diag_hex("ALR-REENTRY sp@", (unsigned long)start);

    // brk(0) reads the current program break so glibc's __libc_setup_tls ->
    // _dl_early_allocate -> __sbrk(0) sees a materialized value. As a fresh
    // kernel-execve'd image the kernel already armed mm->brk, so this is mostly
    // belt-and-suspenders (and keeps parity with the in-process trampoline); failure
    // is non-fatal (glibc 2.36+ has an mmap fallback).
    long cur_brk = sys1(SYS_brk, 0);
    diag_hex("ALR-REENTRY: brk0=", (unsigned long)cur_brk);

    // Clean zeroed TCB: glibc's pre-TLS-init csu reads the canary via TPIDR_EL0; a
    // fresh 16 KiB zero region (loader pattern) keeps that read off NULL. center+8192.
    void* tcb_region = (void*)sys6_(SYS_mmap, 0, 16384, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* tcb = (tcb_region == MAP_FAILED) ? (void*)0 : (void*)((char*)tcb_region + 8192);
    diag_hex("ALR-REENTRY: tcb@", (unsigned long)tcb);

    diag("ALR-REENTRY: jumping\n");
    enter_guest((void*)start, (void*)jump_entry, tcb);
    sys_exit(99);  // unreachable
}

// _start: the ELF entry. The kernel placed the SysV stack at SP; pass &SP[0]
// (argc) to the C main. We must NOT use a normal C prologue that touches SP
// before reading argc, so this is a tiny naked trampoline: x0 = SP, tail-call.
__attribute__((naked, used, noreturn)) void _start(void) {
    __asm__ volatile(
        "mov x0, sp\n"          // x0 = &argc (top of the kernel-provided stack)
        "and sp, x0, #~15\n"    // 16-byte align SP for the C ABI before the call
        "bl  alr_reentry_main\n"
        "brk #0\n");            // unreachable: alr_reentry_main is noreturn
}
