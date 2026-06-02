// alr_inproc_reexec.c — resident in-process re-exec trampoline (ADR-003-v3, R10 step2).
//
// WHAT THIS IS
// ============
// A freestanding, raw-syscall (`svc #0`), no-libc/no-TLS aarch64 trampoline that
// is compiled INTO the loader .so (alr_loader). The supervisor, at the guest's
// execve seccomp-trap, CANCELS the syscall (NT_ARM_SYSTEM_CALL=-1) and PC-redirects
// the tracee here (PTRACE_SETREGSET) after seeding x19-x22 (the entry ABI below).
// This trampoline then maps the new ELF (guest ld.so + target) IN-PROCESS via mmap
// PROT_EXEC (which W^X allows for non-app-storage anon maps) and jumps to ld.so's
// entry with a correct SysV initial SP/auxv — NO execve. glibc comes up under the
// inherited seccomp filter + ptrace, so path syscalls keep trapping and the next
// exec re-enters here.
//
// RELATION TO alr_reentry.c
// =========================
// alr_reentry.c is the (now-dead) kernel-execve Option S: a *standalone static ELF*
// whose `_start` reads target/argv/envp from a KERNEL-built stack and then maps
// ld.so+target and jumps. This file REUSES its proven core verbatim in spirit —
// the ELF types, the inline `svc #0` syscalls, `read_file`, `map_elf_image`
// (PT_LOAD page-rounding + W^X RW->memcpy->RX + R_AARCH64_IRELATIVE), the SysV
// stack + auxv builder, and the final `mov sp / br entry`. The DIFFERENCES forced
// by "in-process, no execve":
//   * Entry is NOT `_start` reading a kernel stack: it is
//     `alr_inproc_reexec_trampoline`, a naked asm shim that reads the target,
//     argv, envp and rootfs from x19-x22 (seeded by the supervisor) and calls a C
//     worker. We did NOT execve, so those guest pointers are still valid here.
//   * There is no kernel-provided auxv on our stack to mine, because we are mid-
//     guest, not a fresh kernel image. We read AT_PAGESZ/AT_HWCAP/AT_HWCAP2/
//     AT_RANDOM/AT_SYSINFO_EHDR from /proc/self/auxv, and uid/gid from raw
//     getuid/geteuid/getgid/getegid syscalls.
//   * We MUST move TPIDR_EL0 OFF the live bionic TCB before the jump: leaving it set
//     would let glibc's ld.so trip over a foreign thread pointer. But it must NOT be
//     NULL either — glibc's pre-TLS-init startup reads the stack canary via
//     THREAD_SELF (=TPIDR_EL0), which faults at ~0 if TP=0. So we point TP at a fresh
//     zeroed 16 KiB region (alr_enter_guest in the loader does the same; see
//     enter_guest below). The earlier `msr tpidr_el0, xzr` was the SEGV bug.
//   * The rootfs prefix comes from x22 directly (the supervisor already resolved
//     it), not from getenv(ALR_ROOTFS).
//
// FREESTANDING / STRICT-FLAGS NOTES
// =================================
// This file is built by the NDK toolchain into alr_loader with
// -Wall -Wextra -Werror -fstack-protector-strong -D_FORTIFY_SOURCE=2 and the
// linker's -Wl,--no-undefined -Wl,--fatal-warnings. To survive that AND to be
// safe to run mid-guest (no bionic TCB, no libc):
//   * Every function is __attribute__((no_stack_protector)) — the canary read
//     would dereference TPIDR_EL0 (the bionic TCB) which is invalid in the guest
//     context and which we are about to zero anyway.
//   * We call NO libc functions at all (only inline `svc #0`), so _FORTIFY_SOURCE
//     has nothing to instrument and the linker's --no-undefined is satisfied with
//     zero external symbols. Our own m_cpy/m_set are plain byte loops; we compile
//     them so the optimizer cannot fold them back into a libc memcpy/memset call
//     (which would (a) add an undefined symbol pre-link in a freestanding-style
//     unit and (b) be unsafe to call mid-guest).
//   * The asm entry never touches the stack before the worker; the guest's current
//     SP is valid and the worker uses it for its frame.
//
// Non-aarch64 ABIs: the asm + svc bodies are aarch64-only. Other NDK ABIs get a
// stub `alr_inproc_reexec_trampoline` that __builtin_trap()s — they must COMPILE
// + LINK (the app ships 4 ABIs) but this code only ever RUNS on the arm64 device.

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__)

// Keep stack-protector off the whole unit (belt-and-suspenders with -fno-builtin
// intent): the canary read dereferences the bionic TCB (TPIDR_EL0), invalid in the
// guest context we run in. Libc-call lowering of our copy loops is prevented by the
// `volatile` destination pointers in m_cpy/m_set below (clang will not fold a
// volatile-store loop into a memcpy/memset call), so we need no GCC-only pragma
// (which clang rejects under -Werror=unknown-pragmas anyway).
#define ALR_FREESTANDING __attribute__((no_stack_protector, no_instrument_function))

// ---- ELF types (freestanding: declare exactly what we touch) ----------------
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
// RELATIVE value — it made the reloc loop treat every RELATIVE entry as an ifunc
// (calling a data address -> SIGILL) and skip the real IRELATIVEs. Match the
// device-proven loader (runtime_report.cpp:36).
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
#define SYS_getpid      172
#define SYS_getuid      174
#define SYS_geteuid     175
#define SYS_getgid      176
#define SYS_getegid     177
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
#define MAP_FIXED_NOREPLACE 0x100000  // since Linux 4.17; EEXIST if the range is taken
#define MAP_FAILED ((void*)-1)

// Distinct fatal exit codes so a device drain can localize the failure. The
// supervisor cancelled the syscall and jumped us here; any _exit here ends the
// guest thread/process with one of these. Keep them unique and documented:
#define EX_NO_TARGET   70  // host_target_path NULL/empty (supervisor seeded x19 wrong)
#define EX_OPEN_TARGET 71  // openat/read of host_target_path failed (path/perm)
#define EX_ELF_TARGET  72  // target is not an ELF / too short
#define EX_MAP_TARGET  73  // mapping the target's PT_LOADs failed
#define EX_STACK       74  // mmap of the fresh guest stack failed
#define EX_NO_ROOTFS   75  // rootfs_dir (x22) NULL/empty
#define EX_INTERP_PATH 76  // <rootfs><interp> too long for buffer
#define EX_OPEN_INTERP 77  // openat/read of the guest ld.so failed
#define EX_ELF_INTERP  78  // guest ld.so is not an ELF
#define EX_MAP_INTERP  79  // mapping the guest ld.so failed
#define EX_NO_INTERP   80  // target has no PT_INTERP (static binary — unsupported here)

// ---- inline aarch64 syscalls ------------------------------------------------
ALR_FREESTANDING static inline long sys6(long n, long a, long b, long c,
                                         long d, long e, long f) {
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
#define sys0(n)              sys6((n),0,0,0,0,0,0)
#define sys1(n,a)            sys6((n),(long)(a),0,0,0,0,0)
#define sys3(n,a,b,c)        sys6((n),(long)(a),(long)(b),(long)(c),0,0,0)
#define sys4(n,a,b,c,d)      sys6((n),(long)(a),(long)(b),(long)(c),(long)(d),0,0)
#define sys6_(n,a,b,c,d,e,f) sys6((n),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))

ALR_FREESTANDING __attribute__((noreturn)) static void sys_exit(int code) {
    sys1(SYS_exit_group, code);
    for (;;) { sys1(SYS_exit, code); }
}

// ---- minimal freestanding helpers (no libc) ---------------------------------
ALR_FREESTANDING static size_t s_len(const char* s) {
    size_t n = 0; while (s[n]) ++n; return n;
}
ALR_FREESTANDING static void s_cpy(char* d, const char* s) {
    while ((*d++ = *s++)) ;
}
ALR_FREESTANDING static void m_cpy(void* d, const void* s, size_t n) {
    volatile char* dd = (volatile char*)d; const char* ss = (const char*)s;
    for (size_t i = 0; i < n; ++i) dd[i] = ss[i];
}
ALR_FREESTANDING static void m_set(void* d, int c, size_t n) {
    volatile char* dd = (volatile char*)d; for (size_t i = 0; i < n; ++i) dd[i] = (char)c;
}

// Freestanding aarch64 I-cache sync after writing executable code (libc's
// __clear_cache is unavailable here). clean D to PoU, DSB, invalidate I to PoU,
// DSB, ISB; line sizes from CTR_EL0. Same work as alr_reentry.c:flush_icache.
ALR_FREESTANDING static void flush_icache(uintptr_t start, uintptr_t end) {
    unsigned long ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    unsigned long dline = 4UL << ((ctr >> 16) & 0xf);
    unsigned long iline = 4UL << (ctr & 0xf);
    for (uintptr_t a = start & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t a = start & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}

// One-line diagnostics to fd 2 (raw write, no libc). Lets a device drain trace
// the trampoline. Same shape as alr_reentry.c:diag / diag_hex.
ALR_FREESTANDING static void diag(const char* msg) { sys3(SYS_write, 2, msg, s_len(msg)); }
ALR_FREESTANDING static void diag_hex(const char* tag, unsigned long v) {
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

// Read a whole file into a fixed buffer. Returns bytes read, or -1 on error.
// Same as alr_reentry.c:read_file (the bootstrap target + ld.so fit a few MiB).
ALR_FREESTANDING static long read_file(const char* path, char* buf, size_t cap) {
    long fd = sys4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t total = 0;
    for (;;) {
        long n = sys3(SYS_read, fd, buf + total, cap - total);
        if (n < 0) { sys1(SYS_close, fd); return -1; }
        if (n == 0) break;
        total += (size_t)n;
        if (total >= cap) { sys1(SYS_close, fd); return -1; }  // too big for buf
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

// Page size: in-process we read AT_PAGESZ from /proc/self/auxv (see worker).
// 4096 fallback; device may be 16K.
static unsigned long g_pagesz = 4096;
// Real AT_HWCAP mined from /proc/self/auxv (worker sets this before mapping). Passed
// to R_AARCH64_IRELATIVE ifunc resolvers so they pick the correct CPU variant; a 0 or
// wrong HWCAP can drive a resolver that indexes a feature table to a bad branch
// (SIGILL). 0 until the worker mines it (then the resolvers see generic, still safe).
static unsigned long g_hwcap = 0;
ALR_FREESTANDING static inline uintptr_t page_down(uintptr_t v) {
    return v & ~(uintptr_t)(g_pagesz - 1);
}
ALR_FREESTANDING static inline uintptr_t page_up(uintptr_t v) {
    return (v + g_pagesz - 1) & ~(uintptr_t)(g_pagesz - 1);
}

// Map every PT_LOAD of `img` (full ELF already in memory) into anonymous memory,
// then apply R_AARCH64_IRELATIVE. SELF-CONTAINED duplicate of
// alr_reentry.c:map_elf_image (which itself mirrors
// runtime_report.cpp:map_elf_image_into_execmem). Uses the runtime AT_PAGESZ so a
// 16K-page device is handled.
//
// R11 SIGILL FIX — one-reservation model (was: per-PT_LOAD MAP_FIXED):
//   The old loop mmap'd EACH PT_LOAD separately with MAP_FIXED. Whenever two
//   segments shared a page (the page that holds the tail of segment N and the head
//   of segment N+1 — common when a static glibc binary's RX text and RELRO/data
//   borders are not a full page apart, and ALWAYS for any seg whose p_memsz BSS
//   tail spills into the next seg's first page), the SECOND MAP_FIXED REPLACED that
//   page with a fresh ZEROED anon page, wiping segment N's already-copied bytes. If
//   the clobbered page held code or a const relocation/IFUNC pointer, execution hit
//   zeros -> SIGILL (signal 4) in glibc startup. Device drain showed exactly this:
//   map+jump reached entry=0x400640 (a fixed-address ET_EXEC), then the static
//   /bin/sh SIGILL'd in its own startup.
//   FIX: reserve the WHOLE [min_v,max_v) span as ONE anonymous RW mapping, memcpy
//   each segment's p_filesz into it (no re-mmap -> no clobber; the [filesz,memsz)
//   BSS tail + whole BSS pages stay zero from the fresh anon span), then walk the
//   span PAGE BY PAGE and mprotect maximal runs to the UNION of p_flags of every
//   PT_LOAD covering each page — so a shared boundary page keeps the perms both
//   neighbours need (an executable boundary page never loses X). ET_EXEC reserves
//   at its FIXED min_v with MAP_FIXED_NOREPLACE so a real collision is DETECTED,
//   not silently clobbered.
ALR_FREESTANDING static MappedImage map_elf_image(const char* img, size_t len,
                                                  const char* tag) {
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
    diag(tag); diag("span="); diag_hex("", (unsigned long)span);

    // ONE reservation for the whole image. ET_DYN: kernel picks the base (we map at
    // 0 then bias). ET_EXEC: reserve AT the fixed min_v with MAP_FIXED_NOREPLACE so
    // a pre-existing mapping at the fixed range is reported (EEXIST) rather than
    // silently clobbered. The reservation is RW so we can memcpy; per-segment
    // mprotect below tightens to final perms (W^X-safe: RX/RO set after copy).
    uintptr_t base = 0;
    void* reserve;
    if (eh->e_type == ET_DYN) {
        reserve = (void*)sys6_(SYS_mmap, 0, span, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserve == MAP_FAILED) { diag(tag); diag("RESERVE_FAIL\n"); return R; }
        base = (uintptr_t)reserve - min_v;
    } else {
        // Fixed-address ET_EXEC: ask for exactly min_v, fail loudly on collision.
        reserve = (void*)sys6_(SYS_mmap, min_v, span, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (reserve == MAP_FAILED) {
            // The fixed range is occupied (the loader/guest already mapped 0x400000..).
            // MAP_FIXED would silently clobber it -> corruption; refuse instead.
            diag(tag); diag("EXEC_FIXED_OCCUPIED@"); diag_hex("", (unsigned long)min_v);
            return R;
        }
        if ((uintptr_t)reserve != min_v) {
            // Kernel ignored the hint (no NOREPLACE support); that's not the fixed
            // address the ET_EXEC needs — bail rather than run at the wrong base.
            diag(tag); diag("EXEC_FIXED_MOVED\n"); return R;
        }
        base = 0;  // ET_EXEC vaddrs are absolute
    }
    diag(tag); diag("base="); diag_hex("", (unsigned long)base);

    // Copy each segment's file bytes into the single reservation. The BSS tail
    // ([p_filesz, p_memsz) within the seg, plus any whole BSS pages up to memsz)
    // needs no memset: the reservation is one fresh anonymous region, all-zero, and
    // we never re-mmap any page, so those bytes are already zero.
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        diag(tag); diag("seg vaddr="); diag_hex("", (unsigned long)ph[i].p_vaddr);
        diag(tag); diag("  filesz="); diag_hex("", (unsigned long)ph[i].p_filesz);
        diag(tag); diag("  memsz="); diag_hex("", (unsigned long)ph[i].p_memsz);
        diag(tag); diag("  flags="); diag_hex("", (unsigned long)ph[i].p_flags);
        m_cpy((void*)(base + ph[i].p_vaddr), img + ph[i].p_offset, ph[i].p_filesz);
    }
    diag(tag); diag("bss_zeroed=anon-span\n");

    // Tighten protections per page across the whole reservation. A page can be
    // claimed by more than one PT_LOAD (the boundary page between e.g. RX text and
    // RW data when the linker did not separate them by a full page, or a seg whose
    // BSS tail spills into the next seg's first page). A naive per-segment mprotect
    // would let the LATER segment DROP perms the earlier one needs on that shared
    // page (mprotect SETS, not ORs) -> e.g. an executable boundary page loses X ->
    // SIGILL. So we compute, per page, the UNION of p_flags of every PT_LOAD that
    // covers it, then mprotect maximal runs of equal prot. (Single pass over pages;
    // span is small — a few hundred KiB for /bin/sh, a few MiB for ld.so.) A page
    // claimed by gaps-only stays PROT_NONE (an inter-segment gap, like a real
    // loader). The pathological case of a page that is BOTH PF_W and PF_X would yield
    // RWX, which W^X rejects: mprotect then fails EACCES and we bail with
    // SEG_PROT_FAIL — a clean, diagnosable refusal, never silent corruption. Real
    // static glibc / ld.so never emit a W+X-shared page (code/data are page-separated
    // by `-z separate-code`, the modern default), so this stays a guard, not a path.
    int       prot_run   = -1;                    // prot of the current run (-1 = none yet)
    uintptr_t prot_runva = 0;                     // image-vaddr of the run start
    for (uintptr_t v = min_v; v < page_up(max_v); v += g_pagesz) {
        int prot = 0;
        for (int i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type != PT_LOAD) continue;
            uintptr_t s = page_down((uintptr_t)ph[i].p_vaddr);
            uintptr_t e = page_up((uintptr_t)(ph[i].p_vaddr + ph[i].p_memsz));
            if (v < s || v >= e) continue;        // this page not in seg i
            if (ph[i].p_flags & PF_R) prot |= PROT_READ;
            if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
            if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        }
        if (prot != prot_run) {
            if (prot_run >= 0) {  // flush the just-ended run
                uintptr_t rs = base + prot_runva, re = base + v;
                if (sys3(SYS_mprotect, rs, re - rs, prot_run) != 0) {
                    diag(tag); diag("SEG_PROT_FAIL\n"); return R;
                }
                if (prot_run & PROT_EXEC) flush_icache(rs, re);
            }
            prot_run = prot; prot_runva = v;
        }
    }
    if (prot_run >= 0) {  // flush the final run
        uintptr_t rs = base + prot_runva, re = base + page_up(max_v);
        if (sys3(SYS_mprotect, rs, re - rs, prot_run) != 0) {
            diag(tag); diag("SEG_PROT_FAIL\n"); return R;
        }
        if (prot_run & PROT_EXEC) flush_icache(rs, re);
    }

    // Relocations via PT_DYNAMIC's DT_RELA (ld.so is section-stripped). We apply
    // BOTH R_AARCH64_RELATIVE (base-relative fixups in a static-PIE / ET_DYN image:
    // *(base+offset) += base) AND R_AARCH64_IRELATIVE (ifunc resolver). The old code
    // only handled "IRELATIVE 1027" — which was actually the RELATIVE value, so it
    // (a) ran every RELATIVE entry through the ifunc path (calling a data address ->
    // SIGILL) and (b) skipped the true IRELATIVEs (1032). Mirrors the device-proven
    // loader (runtime_report.cpp ~L1376-1433), extended with RELATIVE for static-PIE.
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
                    // Base-relative: the linker emitted addend = link-time vaddr;
                    // the runtime value is addend + load bias. For ET_EXEC base==0
                    // (no-op); for ET_DYN/static-PIE this is the bulk of startup
                    // relocs that glibc's _dl_relocate_static_pie would otherwise do.
                    *(uintptr_t*)(base + r->r_offset) = base + (uintptr_t)r->r_addend;
                } else if (rtype == R_AARCH64_IRELATIVE) {
                    typedef unsigned long (*Resolver)(unsigned long, const void*);
                    Resolver res = (Resolver)(base + r->r_addend);
                    // Thread the REAL AT_HWCAP (was 0): glibc ifunc resolvers select a
                    // CPU variant from HWCAP; a 0/wrong value can SIGILL a resolver that
                    // indexes a feature table. The 2nd arg (HWCAP2/__ifunc_arg_t*) stays
                    // 0 — aarch64 glibc resolvers that need HWCAP2 take it via a pointer
                    // arg we don't synthesize; passing 0 makes them treat HWCAP2 as
                    // absent (generic), which is correct-but-conservative, never illegal.
                    *(uint64_t*)(base + r->r_offset) = res(g_hwcap, 0);
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

// Transfer control to the guest entry with the freshly built SysV stack. Mirrors
// the device-proven loader's alr_enter_guest(sp, entry, tcb) (runtime_report.cpp
// L1160-1169). In-process we run under a LIVE bionic TCB, so we must move the thread
// pointer OFF it before glibc startup runs. The old code used `msr tpidr_el0, xzr`
// (TP=NULL) — but glibc's pre-TLS-init csu (_dl_aux_init/__tunables_init) is built
// with -fstack-protector and reads the canary via THREAD_SELF (=TPIDR_EL0); with
// TP=NULL that load faults near address 0 (SIGSEGV). The loader instead points TP at
// a fresh ZEROED 16 KiB region (caller mmaps it, passes its center) so those reads
// land in mapped-zero until ld.so installs its own TLS. Naked-ish: never returns.
ALR_FREESTANDING __attribute__((noreturn)) static void enter_guest(void* sp, void* entry, void* tcb) {
    __asm__ volatile(
        "msr tpidr_el0, %2\n"    // clean zeroed TCB (NOT xzr): canary/THREAD_SELF reads land in mapped-zero
        "mov sp, %0\n"
        "mov x0, #0\n"
        "br  %1\n"
        :: "r"(sp), "r"(entry), "r"(tcb) : "memory");
    __builtin_unreachable();
}

// Read selected AT_* tags from /proc/self/auxv (we are mid-guest: no kernel-built
// stack to mine like alr_reentry.c does). Best-effort; leaves *out_* untouched on
// any failure so the caller's defaults stand.
ALR_FREESTANDING static void read_auxv(unsigned long* hwcap, unsigned long* hwcap2,
                                       unsigned long* random_ptr, unsigned long* sysinfo,
                                       unsigned long* clktck) {
    // /proc/self/auxv is a flat array of [unsigned long key, unsigned long val]
    // pairs terminated by AT_NULL. A few hundred bytes; 4096 is ample.
    static char auxbuf[4096];
    long fd = sys4(SYS_openat, AT_FDCWD, "/proc/self/auxv", O_RDONLY, 0);
    if (fd < 0) return;
    size_t total = 0;
    for (;;) {
        long n = sys3(SYS_read, fd, auxbuf + total, sizeof(auxbuf) - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= sizeof(auxbuf)) break;
    }
    sys1(SYS_close, fd);
    const unsigned long* a = (const unsigned long*)auxbuf;
    size_t pairs = total / (2 * sizeof(unsigned long));
    for (size_t i = 0; i < pairs; ++i) {
        unsigned long key = a[2 * i], val = a[2 * i + 1];
        if (key == AT_NULL) break;
        switch (key) {
            case AT_PAGESZ:       if (val) g_pagesz = val; break;
            case AT_HWCAP:        *hwcap = val; break;
            case AT_HWCAP2:       *hwcap2 = val; break;
            case AT_RANDOM:       *random_ptr = val; break;
            case AT_SYSINFO_EHDR: *sysinfo = val; break;
            case AT_CLKTCK:       if (val) *clktck = val; break;
            default: break;
        }
    }
}

// File buffers: static .bss, no heap (we must not call libc malloc mid-guest).
// 8 MiB each matches alr_reentry.c's headroom for ld.so + a typical target's
// PT_LOAD file image.
#define FILEBUF_CAP (8u * 1024u * 1024u)
static char g_target_buf[FILEBUF_CAP];
static char g_interp_buf[FILEBUF_CAP];
static char g_interp_path[1024];

// ===========================================================================
//  alr_inproc_reexec_worker — the C worker. Entered from the naked asm shim with
//  the entry ABI already unpacked into the SysV arg regs:
//    target = host_target_path (x19), argv (x20), envp (x21), rootfs (x22).
//  Never returns: maps + jumps, or sys_exit()s with a distinct EX_* code.
// ===========================================================================
// no_builtin("memcpy"/"memset"): at -O2 clang would otherwise materialize the large
// local aux[] initializer / struct returns via a libc memcpy/memset call (an
// undefined symbol in this freestanding-style unit, and a libc call we want to avoid
// mid-guest). With these the worker emits only our own loops + svc. (The NDK debug
// build is -O0 and already emits zero such calls; this hardens release/-O2 too.)
__attribute__((used, noreturn, no_stack_protector, no_instrument_function,
               no_builtin("memcpy", "memset")))
void alr_inproc_reexec_worker(const char* target, char** argv,
                              char** envp, const char* rootfs) {
    diag("ALR-INPROC: worker target=");
    diag(target ? target : "(null)");
    diag("\n");
    if (!target || !target[0]) { diag("ALR-INPROC: no target\n"); sys_exit(EX_NO_TARGET); }
    if (!rootfs || !rootfs[0]) { diag("ALR-INPROC: no rootfs\n"); sys_exit(EX_NO_ROOTFS); }

    // ---- mine /proc/self/auxv + raw uid/gid for the synthesized guest auxv ----
    unsigned long my_hwcap = 0, my_hwcap2 = 0, my_random = 0, my_sysinfo = 0;
    unsigned long my_clktck = 100;
    read_auxv(&my_hwcap, &my_hwcap2, &my_random, &my_sysinfo, &my_clktck);
    g_hwcap = my_hwcap;  // map_elf_image's IRELATIVE resolvers read this (was hard 0)
    diag_hex("ALR-INPROC: hwcap=", my_hwcap);
    diag_hex("ALR-INPROC: pagesz=", g_pagesz);
    unsigned long my_uid  = (unsigned long)sys0(SYS_getuid);
    unsigned long my_euid = (unsigned long)sys0(SYS_geteuid);
    unsigned long my_gid  = (unsigned long)sys0(SYS_getgid);
    unsigned long my_egid = (unsigned long)sys0(SYS_getegid);

    // ---- read + validate the target ELF (host_target_path is already rootfs) --
    long tlen = read_file(target, g_target_buf, FILEBUF_CAP);
    if (tlen < 0) { diag("ALR-INPROC: target open/read fail\n"); sys_exit(EX_OPEN_TARGET); }
    if (tlen < (long)sizeof(Elf64_Ehdr) ||
        g_target_buf[0] != 0x7f || g_target_buf[1] != 'E' ||
        g_target_buf[2] != 'L'  || g_target_buf[3] != 'F') {
        diag("ALR-INPROC: target not ELF\n"); sys_exit(EX_ELF_TARGET);
    }

    // ---- find PT_INTERP. Dynamic targets carry a PT_INTERP (the guest ld.so);
    //      STATIC targets have none — we map them directly and jump to their own
    //      entry (no ld.so). Static-PIE (ET_DYN, no INTERP) self-relocates in its
    //      _start using AT_PHDR; static-ET_EXEC runs at its fixed vaddr. ----
    const Elf64_Ehdr* teh = (const Elf64_Ehdr*)g_target_buf;
    const Elf64_Phdr* tph = (const Elf64_Phdr*)(g_target_buf + teh->e_phoff);
    const char* interp_str = 0;
    for (int i = 0; i < teh->e_phnum; ++i) {
        if (tph[i].p_type == PT_INTERP) { interp_str = g_target_buf + tph[i].p_offset; break; }
    }
    const int is_static = (interp_str == 0);
    if (is_static) {
        diag("ALR-INPROC: static target (no PT_INTERP) — direct map+jump\n");
    } else {
        diag("ALR-INPROC: interp_str="); diag(interp_str); diag("\n");
    }

    // ---- map the target's PT_LOADs --------------------------------------------
    MappedImage prog = map_elf_image(g_target_buf, (size_t)tlen, "ALR-INPROC PROG IREL=");
    if (!prog.ok) { diag("ALR-INPROC: prog map fail\n"); sys_exit(EX_MAP_TARGET); }

    // ---- (dynamic only) build <rootfs><interp> and map the guest ld.so --------
    MappedImage interp;
    interp.base = 0; interp.entry = 0; interp.phdr = 0;
    interp.phent = 0; interp.phnum = 0; interp.ok = 1;
    if (!is_static) {
        size_t rl = s_len(rootfs);
        if (rl + s_len(interp_str) + 1 >= sizeof(g_interp_path)) {
            diag("ALR-INPROC: interp path too long\n"); sys_exit(EX_INTERP_PATH);
        }
        s_cpy(g_interp_path, rootfs);
        s_cpy(g_interp_path + rl, interp_str);   // interp_str begins with '/'
        diag("ALR-INPROC: interp="); diag(g_interp_path); diag("\n");
        long ilen = read_file(g_interp_path, g_interp_buf, FILEBUF_CAP);
        if (ilen < 0) { diag("ALR-INPROC: interp open/read fail\n"); sys_exit(EX_OPEN_INTERP); }
        if (ilen < (long)sizeof(Elf64_Ehdr) ||
            g_interp_buf[0] != 0x7f || g_interp_buf[1] != 'E') {
            diag("ALR-INPROC: interp not ELF\n"); sys_exit(EX_ELF_INTERP);
        }
        interp = map_elf_image(g_interp_buf, (size_t)ilen, "ALR-INPROC INTERP IREL=");
        if (!interp.ok) { diag("ALR-INPROC: interp map fail\n"); sys_exit(EX_MAP_INTERP); }
    }
    diag("ALR-INPROC: mapped\n");

    // ---- build the guest's SysV initial stack ---------------------------------
    // The guest's intended argv comes straight from x20 (we did NOT execve, so the
    // vector is still valid). argv[0] stays as-is (the guest's intended argv[0]).
    // envp comes straight from x21. AT_EXECFN points at host_target_path.
    const size_t stack_size = 512u * 1024u;
    void* stk = (void*)sys6_(SYS_mmap, 0, stack_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) { diag("ALR-INPROC: stack fail\n"); sys_exit(EX_STACK); }
    uintptr_t top = (uintptr_t)stk + stack_size;

    // 16-byte AT_RANDOM block on the new stack (copy ours if we mined one).
    top -= 16;
    if (my_random) m_cpy((void*)top, (void*)my_random, 16);
    else m_set((void*)top, 0x5a, 16);
    uintptr_t at_random = top;

    // AT_EXECFN string = host_target_path, on the new stack.
    {
        size_t n = s_len(target) + 1;
        top -= n; m_cpy((void*)top, target, n);
    }
    uintptr_t at_execfn = top;

    // Push argv strings (high -> low), preserving the guest's argv[0].
    #define MAX_ARGV 256
    uintptr_t argv_ptrs[MAX_ARGV];
    unsigned long g_argc = 0;
    for (char** a = argv; a && *a && g_argc < MAX_ARGV; ++a) ++g_argc;
    for (unsigned long i = g_argc; i-- > 0;) {
        size_t n = s_len(argv[i]) + 1;
        top -= n; m_cpy((void*)top, argv[i], n); argv_ptrs[i] = top;
    }

    // Push envp strings (high -> low).
    #define MAX_ENVP 512
    uintptr_t envp_ptrs[MAX_ENVP];
    unsigned long n_env = 0;
    for (char** e = envp; e && *e && n_env < MAX_ENVP; ++e) ++n_env;
    for (unsigned long i = n_env; i-- > 0;) {
        size_t n = s_len(envp[i]) + 1;
        top -= n; m_cpy((void*)top, envp[i], n); envp_ptrs[i] = top;
    }

    // auxv describing the PROGRAM to ld.so: AT_PHDR/AT_ENTRY = program's, AT_BASE =
    // interpreter load base. AT_EXECFN = host_target_path.
    uint64_t aux[] = {
        AT_PHDR, prog.phdr,
        AT_PHENT, prog.phent,
        AT_PHNUM, prog.phnum,
        AT_PAGESZ, g_pagesz,
        AT_BASE, interp.base,
        AT_FLAGS, 0,
        AT_ENTRY, prog.entry,
        AT_EXECFN, at_execfn,
        AT_UID, my_uid, AT_EUID, my_euid, AT_GID, my_gid, AT_EGID, my_egid,
        AT_HWCAP, my_hwcap, AT_HWCAP2, my_hwcap2,
        AT_CLKTCK, my_clktck,
        AT_RANDOM, at_random,
        AT_SECURE, 0,
        AT_SYSINFO_EHDR, my_sysinfo,
        AT_NULL, 0,
    };
    const size_t n_aux = sizeof(aux) / sizeof(aux[0]);   // includes the AT_NULL pair
    const size_t n_words = 1 + (g_argc + 1) + (n_env + 1) + n_aux;
    // Place the info block so its FIRST word (argc) is 16-byte aligned — the aarch64
    // SysV requirement for the initial SP at process entry (glibc _start assumes it;
    // a misaligned SP SIGBUS/SIGILLs the first SIMD spill in __libc_start_main).
    // Rounding `start` DOWN with & ~0xf both reserves >= n_words*8 bytes and lands
    // argc on a 16-byte boundary. n_words may be odd or even — the mask handles both
    // (it only ever moves `start` further down, never into the strings above `top`).
    uintptr_t start = (top - n_words * 8) & ~(uintptr_t)0xf;
    uint64_t* w = (uint64_t*)start;
    size_t k = 0;
    w[k++] = g_argc;
    for (unsigned long i = 0; i < g_argc; ++i) w[k++] = argv_ptrs[i];
    w[k++] = 0;
    for (unsigned long i = 0; i < n_env; ++i) w[k++] = envp_ptrs[i];
    w[k++] = 0;
    for (size_t i = 0; i < n_aux; ++i) w[k++] = aux[i];

    // Dynamic: jump to ld.so (it drives the program). Static: jump straight to the
    // program's own entry (AT_BASE=0 since interp.base==0 for the static case).
    uintptr_t jump_entry = is_static ? prog.entry : interp.entry;
    diag_hex("ALR-INPROC: argc=", (unsigned long)g_argc);
    diag_hex("ALR-INPROC: envc=", (unsigned long)n_env);
    diag_hex("ALR-INPROC: auxc=", (unsigned long)(n_aux / 2));  // # of AT_* pairs
    diag_hex("ALR-INPROC: at_phdr=", (unsigned long)prog.phdr);
    diag_hex("ALR-INPROC: at_entry=", (unsigned long)prog.entry);
    diag_hex("ALR-INPROC: at_base=", (unsigned long)interp.base);
    diag("ALR-INPROC: sp_align=");
    diag((start & 0xf) ? "BAD\n" : "ok\n");   // must be 'ok' (16-byte aligned)
    diag("ALR-INPROC: mapped, jumping entry=");
    diag_hex("", (unsigned long)jump_entry);
    diag_hex("ALR-INPROC sp@", (unsigned long)start);

    // ---- brk init (S2 / glibc BZ2066147) --------------------------------------
    // We did NOT execve, so the kernel never re-armed mm->brk for a fresh image: the
    // program break is wherever bionic left it. glibc's __libc_setup_tls calls
    // _dl_early_allocate -> __sbrk(0) to read the current break before its mmap
    // fallback; on a re-mapped (non-execve) guest a never-touched/zero break can fault
    // that path. A single brk(0) just READS the current break (brk with addr below the
    // minimum is a no-op that returns the current break) so the value is materialized
    // for glibc's first __sbrk. Best-effort: failure is non-fatal (glibc 2.36+ falls
    // back to mmap), so we only diag it.
    long cur_brk = sys1(SYS_brk, 0);
    diag_hex("ALR-INPROC: brk0=", (unsigned long)cur_brk);

    // ---- clean zeroed TCB (FIX: was msr tpidr_el0, xzr -> NULL deref) ----------
    // glibc's pre-TLS-init startup reads the stack canary via TPIDR_EL0; point it at a
    // fresh 16 KiB zero region (loader pattern, runtime_report.cpp L1888) so those
    // reads land in mapped-zero, not at NULL. center+8192 leaves slack on both sides.
    void* tcb_region = (void*)sys6_(SYS_mmap, 0, 16384, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* tcb = (tcb_region == MAP_FAILED) ? (void*)0 : (void*)((char*)tcb_region + 8192);
    diag_hex("ALR-INPROC: tcb@", (unsigned long)tcb);

    enter_guest((void*)start, (void*)jump_entry, tcb);
    sys_exit(99);  // unreachable
}

// ===========================================================================
//  alr_inproc_reexec_trampoline — the naked asm entry the supervisor PC-redirects
//  into. On entry x19-x22 hold the ABI (see header). We MUST NOT touch the stack
//  before the worker (the guest's current SP is valid; the worker uses it for its
//  frame). Move x19->x0, x20->x1, x21->x2, x22->x3 and tail-call the C worker.
// ===========================================================================
__attribute__((naked, used, noreturn))
void alr_inproc_reexec_trampoline(void) {
    __asm__ volatile(
        "mov x0, x19\n"   // host_target_path
        "mov x1, x20\n"   // argv
        "mov x2, x21\n"   // envp
        "mov x3, x22\n"   // rootfs_dir
        "b   alr_inproc_reexec_worker\n");
}

#else  // !__aarch64__

// Non-aarch64 ABIs: the trampoline can never run here (the device is arm64). It
// only needs to COMPILE + LINK so the 4-ABI APK builds. Trap if ever called.
__attribute__((noreturn)) void alr_inproc_reexec_trampoline(void) {
    __builtin_trap();
    for (;;) {}
}

#endif  // __aarch64__
