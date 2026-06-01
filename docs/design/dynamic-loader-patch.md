# ALR dynamic-loader patch — run dynamically-linked glibc aarch64 binaries in-process

Status: ready-to-apply (hand-applied, then device-tested). All edits are confined to
`app/src/main/cpp/runtime_report.cpp`, inside `build_native_loader_probe`
(function starts at line 1024; child branch at lines 1086–1308; parent report
gates at lines 1435–1468). No header, JNI, or build-system changes. It rides the
existing `nativeAlrNativeLoaderProbe` path and reuses the entire multi-tracee
SIGSYS=`-ENOSYS` supervisor (lines 1314–1424) and the `alr_enter_guest`
trampoline (lines 1007–1017) unchanged.

The static path (`/bin/hello`, `/bin/mt-test`) stays byte-for-byte behaviorally
identical: we branch on the presence of `PT_INTERP`, and the static branch passes
exactly the same auxv values it does today (`AT_BASE=0`, `AT_ENTRY`/`AT_PHDR`
describe the program, jump target = program entry). The single new auxv tag added
in both cases (`AT_EXECFN`) is inert for static glibc startup.

This delegates dynamic linking entirely to the guest's own
`ld-linux-aarch64.so.1`: we do NOT walk `DT_NEEDED`, resolve symbols, or apply
`GLOB_DAT`/`JUMP_SLOT`/`TLS` relocations. We map LOAD segments + apply
`R_AARCH64_IRELATIVE` for each image (program and interpreter), build the auxv,
set `AT_BASE`, and jump to the interpreter's entry — exactly what the kernel's
`binfmt_elf` does.

---

## 0. Confirmed facts this patch relies on (verified against the tree)

- `diag_hex(int fd, unsigned long long v)` — line 988. Writes `0x<16 hex> `.
- `alr_enter_guest(void* sp, void* entry, void* tcb)` — line 1007. `x0=sp`,
  `x1=entry`, `x2=tcb`; sets `sp`, `msr tpidr_el0, x2`, clears `x0`, `br entry`.
  Unchanged.
- `config` is `alr::runtime::RuntimeConfig` (from `alr_runtime_config_from_input`,
  line 1028); `config.rootfs_dir` is `std::string` (declared `runtime_plan.hpp:32`).
  Because the child shares the parent's address space at `fork()`, `config` is
  readable in the child with no IPC.
- `guest_rel` (line 1032) is the guest program path, e.g. `/bin/hello` or
  `/bin/dash`. It is the correct `argv[0]` and `AT_EXECFN` string for the dynamic
  case (the current static code hardcodes `"/bin/hello"` at line 1219 — see §3.4,
  we switch to `guest_rel` so dynamic argv0 is correct; this is behavior-neutral
  for the hello test since `guest_rel` already is `/bin/hello`).
- Includes already present: `<elf.h>`, `<fcntl.h>` (`O_RDONLY|O_CLOEXEC`),
  `<sys/auxv.h>` (`getauxval`, all `AT_*` incl. `AT_EXECFN`=33), `<sys/mman.h>`,
  `<sys/syscall.h>`, `<cstring>`, `<string>`, `<unistd.h>`. `R_AARCH64_IRELATIVE`
  is `#define`d at line 26. No new includes needed.
- The child uses only `mmap`/`memcpy`/`open`/`read`/`close`/`write`/raw syscalls
  after `TRACEME` — `std::string interp_bytes` and `std::string interp_host` use
  the heap, but `std::string` allocation post-`fork` in this single-threaded child
  is exactly as safe as the existing parent's `elf` read (the same allocator is
  used at line 1043 before the fork; after the fork the child is single-threaded
  so no allocator lock can be held by another thread). This matches the existing
  code's risk posture.

---

## 1. EDIT 1 — add the `MappedImage` struct + `map_elf_image_into_execmem` helper

Insert this block **immediately before** `build_native_loader_probe`
(i.e. just before line 1024, after the comment block at lines 1019–1023 — put it
right after the `#endif` at line 1017 so it sits beside the trampoline and is
`#if defined(__aarch64__)`-guarded). It contains the existing static map+IRELATIVE
body, generalized to operate on an arbitrary in-memory ELF image and to compute
`AT_PHDR` per image, with the relocation walk extended to read `PT_DYNAMIC`'s
`DT_RELA` first (mandatory for `ld.so`, which is usually `-S` stripped of section
headers — the static program happened to keep `SHT_RELA`, but the interpreter
cannot be assumed to).

```cpp
#if defined(__aarch64__)
// Result of mapping one ELF image (program or interpreter) into execmem.
// All fields are value-initialized so the struct compiles clean under
// -Werror=missing-field-initializers when constructed as MappedImage{}.
struct MappedImage {
    uintptr_t base = 0;      // load bias: runtime_addr = base + p_vaddr
    uintptr_t entry = 0;     // base + e_entry
    uintptr_t phdr = 0;      // runtime address of THIS image's program headers
    uint16_t  phent = 0;     // e_phentsize
    uint16_t  phnum = 0;     // e_phnum
    bool      ok = false;
};

// Map every PT_LOAD of `img` (a full ELF file already read into memory, length
// `len`) into anonymous execmem via the v57-proven W^X-safe RW->memcpy->RX
// MAP_FIXED path, then apply R_AARCH64_IRELATIVE (IFUNC). Diagnostics go to fd
// `dg` with a short `tag` prefix ("PROG:"/"INTERP:") so the program and
// interpreter stages are distinguishable in the app report. ET_DYN images (PIE
// programs and ld.so) get a PROT_NONE reservation first for a contiguous base;
// ET_EXEC keeps base==0 (fixed vaddrs). Runs in the forked child after TRACEME:
// raw mmap/mprotect/memcpy + write() only. Returns ok=false (and a tagged diag)
// on any failure; the caller _exit()s with a stage code.
static MappedImage map_elf_image_into_execmem(const char* img, std::size_t len,
                                              int dg, const char* tag) {
    MappedImage R{};
    if (len < sizeof(Elf64_Ehdr)) {
        ::write(dg, tag, ::strlen(tag));
        ::write(dg, "SHORT;", 6);
        return R;
    }
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(img);
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(img + eh->e_phoff);

    uintptr_t min_v = ~static_cast<uintptr_t>(0);
    uintptr_t max_v = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        const uintptr_t s = static_cast<uintptr_t>(ph[i].p_vaddr) & ~static_cast<uintptr_t>(0xfff);
        const uintptr_t e = static_cast<uintptr_t>(ph[i].p_vaddr + ph[i].p_memsz);
        if (s < min_v) min_v = s;
        if (e > max_v) max_v = e;
    }
    if (min_v == ~static_cast<uintptr_t>(0)) {
        ::write(dg, tag, ::strlen(tag));
        ::write(dg, "NO_LOAD;", 8);
        return R;
    }

    const std::size_t span = (max_v - min_v + 0xfff) & ~static_cast<uintptr_t>(0xfff);
    uintptr_t base = 0;
    if (eh->e_type == ET_DYN) {
        void* reserve = ::mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserve == MAP_FAILED) {
            ::write(dg, tag, ::strlen(tag));
            ::write(dg, "RESERVE_FAIL;", 13);
            return R;
        }
        base = reinterpret_cast<uintptr_t>(reserve) - min_v;
    }

    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        const uintptr_t seg_start = base + (static_cast<uintptr_t>(ph[i].p_vaddr) & ~static_cast<uintptr_t>(0xfff));
        const uintptr_t v_off = static_cast<uintptr_t>(ph[i].p_vaddr) & static_cast<uintptr_t>(0xfff);
        const std::size_t map_len = (v_off + ph[i].p_memsz + 0xfff) & ~static_cast<uintptr_t>(0xfff);
        void* m = ::mmap(reinterpret_cast<void*>(seg_start), map_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED) {
            ::write(dg, tag, ::strlen(tag));
            ::write(dg, "SEG_MAP_FAIL;", 13);
            return R;
        }
        std::memcpy(reinterpret_cast<void*>(base + ph[i].p_vaddr), img + ph[i].p_offset, ph[i].p_filesz);
        int prot = 0;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        if (::mprotect(reinterpret_cast<void*>(seg_start), map_len, prot) != 0) {
            ::write(dg, tag, ::strlen(tag));
            ::write(dg, "SEG_PROT_FAIL;", 14);
            return R;
        }
        if (prot & PROT_EXEC) {
            __builtin___clear_cache(reinterpret_cast<char*>(seg_start),
                                    reinterpret_cast<char*>(seg_start + map_len));
        }
    }

    // Apply R_AARCH64_IRELATIVE. Prefer PT_DYNAMIC's DT_RELA/DT_RELASZ (required
    // for ld.so, which is typically section-header-stripped); fall back to
    // SHT_RELA section headers only if PT_DYNAMIC is absent (the static-program
    // case, unchanged from the original code). IRELATIVE must be applied BEFORE
    // the jump because glibc startup (ld.so's _dl_start and the program's
    // _start) calls IFUNC memcpy/memset/strcmp while parsing auxv.
    int irel = 0;
    const Elf64_Phdr* dynph = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dynph = &ph[i];
            break;
        }
    }
    if (dynph != nullptr) {
        const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(base + dynph->p_vaddr);
        uintptr_t rela = 0;
        std::size_t relasz = 0;
        std::size_t relaent = sizeof(Elf64_Rela);
        for (; dyn->d_tag != DT_NULL; ++dyn) {
            if (dyn->d_tag == DT_RELA)    rela    = base + dyn->d_un.d_ptr;
            if (dyn->d_tag == DT_RELASZ)  relasz  = static_cast<std::size_t>(dyn->d_un.d_val);
            if (dyn->d_tag == DT_RELAENT) relaent = static_cast<std::size_t>(dyn->d_un.d_val);
        }
        if (rela != 0 && relaent != 0) {
            for (std::size_t off = 0; off < relasz; off += relaent) {
                const auto* r = reinterpret_cast<const Elf64_Rela*>(rela + off);
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_IRELATIVE) {
                    continue;
                }
                struct IfuncArg {
                    unsigned long size;
                    unsigned long hwcap;
                    unsigned long hwcap2;
                } arg = {sizeof(IfuncArg), ::getauxval(AT_HWCAP), ::getauxval(AT_HWCAP2)};
                using Resolver = unsigned long (*)(unsigned long, const void*);
                Resolver resolver = reinterpret_cast<Resolver>(base + r->r_addend);
                *reinterpret_cast<uint64_t*>(base + r->r_offset) = resolver(arg.hwcap, &arg);
                ++irel;
            }
        }
    } else if (eh->e_shoff != 0 && eh->e_shentsize == sizeof(Elf64_Shdr)) {
        const auto* sh = reinterpret_cast<const Elf64_Shdr*>(img + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; ++i) {
            if (sh[i].sh_type != SHT_RELA || sh[i].sh_entsize != sizeof(Elf64_Rela)) {
                continue;
            }
            const auto* rl = reinterpret_cast<const Elf64_Rela*>(img + sh[i].sh_offset);
            const std::size_t cnt = sh[i].sh_size / sizeof(Elf64_Rela);
            for (std::size_t j = 0; j < cnt; ++j) {
                if (ELF64_R_TYPE(rl[j].r_info) != R_AARCH64_IRELATIVE) {
                    continue;
                }
                struct IfuncArg {
                    unsigned long size;
                    unsigned long hwcap;
                    unsigned long hwcap2;
                } arg = {sizeof(IfuncArg), ::getauxval(AT_HWCAP), ::getauxval(AT_HWCAP2)};
                using Resolver = unsigned long (*)(unsigned long, const void*);
                Resolver resolver = reinterpret_cast<Resolver>(base + rl[j].r_addend);
                *reinterpret_cast<uint64_t*>(base + rl[j].r_offset) = resolver(arg.hwcap, &arg);
                ++irel;
            }
        }
    }
    ::write(dg, tag, ::strlen(tag));
    ::write(dg, "IREL=", 5);
    diag_hex(dg, static_cast<unsigned long long>(irel));

    // AT_PHDR: prefer PT_PHDR; else the PT_LOAD covering e_phoff; else base+e_phoff.
    uintptr_t at_phdr = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) {
            at_phdr = base + ph[i].p_vaddr;
            break;
        }
    }
    if (at_phdr == 0) {
        for (int i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type == PT_LOAD && eh->e_phoff >= ph[i].p_offset &&
                eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                at_phdr = base + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }
    }
    if (at_phdr == 0) {
        at_phdr = base + eh->e_phoff;
    }

    R.base  = base;
    R.entry = base + eh->e_entry;
    R.phdr  = at_phdr;
    R.phent = eh->e_phentsize;
    R.phnum = eh->e_phnum;
    R.ok    = true;
    return R;
}
#endif  // __aarch64__
```

Notes on fidelity vs the original static loop:
- The body is identical except: (1) it reads from an arbitrary `img`/`len`
  instead of `elf_ptr`/`elf.size()`; (2) it tries `PT_DYNAMIC` `DT_RELA` before
  section-header `SHT_RELA`; (3) it returns the per-image `AT_PHDR`, `entry`,
  `phent`, `phnum`, `base` instead of computing them inline. The
  `first_seg_req`/`first_seg_got` debug captured at original lines 1115/1130–1133
  is intentionally dropped here (it only aided the very first static bring-up);
  the per-stage `PROG:`/`INTERP:` tags supersede it. If you want to keep the
  seg0 capture, add `uintptr_t* first_req` out-params — not required.

---

## 2. EDIT 2 — replace the inline static map body in the child with two helper calls

**Delete** the child block from the first line after the `ph` setup down to the
end of the IRELATIVE diagnostic — that is original lines **1090–1181** (from
`uintptr_t min_v = ...` at 1090 through `diag_hex(dg, static_cast<unsigned long long>(irel_applied));`
at 1181). Keep lines 1087–1088 (`ce`/`ph` set-up) — they stay because §3 uses
`ce`/`ph` to read `PT_INTERP`.

**Insert** in its place the following (detect dynamic, read the interpreter, map
both images). This goes after line 1088 and before the signal-reset block that is
currently at line 1183 (`// Reset all signal dispositions ...`).

```cpp
        // --- detect dynamic vs static: presence of PT_INTERP -----------------
        const char* interp_str = nullptr;   // guest path, e.g. "/lib/ld-linux-aarch64.so.1"
        for (int i = 0; i < ce->e_phnum; ++i) {
            if (ph[i].p_type == PT_INTERP) {
                interp_str = elf_ptr + ph[i].p_offset;  // NUL-terminated in the file image
                break;
            }
        }
        const bool dynamic = (interp_str != nullptr);
        if (dynamic) {
            ::write(dg, "DYN;", 4);
        } else {
            ::write(dg, "STATIC;", 7);
        }

        // --- read the interpreter image from the rootfs (dynamic only) -------
        std::string interp_bytes;
        if (dynamic) {
            // PT_INTERP is an absolute GUEST path ("/lib/..."); resolve it under
            // the rootfs the same way translate_rootfs_path() would, inline.
            std::string interp_host = config.rootfs_dir;
            interp_host += interp_str;  // guest path begins with '/'
            const int ifd = ::open(interp_host.c_str(), O_RDONLY | O_CLOEXEC);
            if (ifd < 0) {
                ::write(dg, "INTERP_OPEN_FAIL;", 17);
                _exit(75);
            }
            char ibuf[65536];
            ssize_t in = 0;
            while ((in = ::read(ifd, ibuf, sizeof(ibuf))) > 0) {
                interp_bytes.append(ibuf, static_cast<std::size_t>(in));
            }
            ::close(ifd);
            if (interp_bytes.size() < sizeof(Elf64_Ehdr) ||
                std::memcmp(interp_bytes.data(), "\x7f""ELF", 4) != 0) {
                ::write(dg, "INTERP_BAD_ELF;", 15);
                _exit(76);
            }
            ::write(dg, "INTERP_READ=", 12);
            diag_hex(dg, static_cast<unsigned long long>(interp_bytes.size()));
        }

        // --- map the PROGRAM (ET_DYN PIE, or rare ET_EXEC) -------------------
        MappedImage prog = map_elf_image_into_execmem(elf_ptr, elf.size(), dg, "PROG:");
        if (!prog.ok) {
            _exit(72);
        }

        // --- map the INTERPRETER (always ET_DYN) -----------------------------
        MappedImage interp{};
        if (dynamic) {
            interp = map_elf_image_into_execmem(interp_bytes.data(), interp_bytes.size(),
                                                dg, "INTERP:");
            if (!interp.ok) {
                _exit(77);
            }
        }
        ::write(dg, "MAPPED;", 7);
```

Rationale for the exit codes: `72` = program map fail (matches the original
`SEG_MAP_FAIL` exit), `75` = interpreter open fail, `76` = interpreter not an
ELF, `77` = interpreter map fail. The helper itself does not `_exit`; it writes a
tagged diag and returns `ok=false`, and the caller chooses the exit code — this
keeps the failure stage visible in both the diag string and the child exit code.

The original `::write(dg, "IREL=", ...)` at lines 1180–1181 is now emitted by the
helper as `PROG:IREL=...` / `INTERP:IREL=...`, so do not re-emit it here.

---

## 3. EDIT 3 — auxv differences and the jump target

This is the crux. The stack-building block (original lines 1204–1308) stays
structurally identical (same `at_random`, same `GLIBC_TUNABLES`, same 16-byte
alignment, same zeroed TCB via `tpidr_el0`, same `alr_enter_guest`). Only the
auxv **values**, one added envp string, the `argv0` source string, and the
**jump target** change. The `MAPPED;` write that originally lived at line 1202
(`SIGRESET;MAPPED;`) should be split: keep `SIGRESET;` after the signal reset, and
the `MAPPED;` is now emitted in EDIT 2 above. (Concretely: change the original
`::write(dg, "SIGRESET;MAPPED;", 16);` at line 1202 to `::write(dg, "SIGRESET;", 9);`.)

### 3.1 The auxv rule (what the kernel's binfmt_elf does, which we replicate)

| auxv tag | Static (existing) | **Dynamic (interp handoff)** |
|---|---|---|
| `AT_PHDR`  | program phdrs (`at_phdr`) | **program phdrs** `prog.phdr` — still the program's |
| `AT_PHENT` | program `e_phentsize` | program `e_phentsize` `prog.phent` |
| `AT_PHNUM` | program `e_phnum` | program `e_phnum` `prog.phnum` |
| `AT_ENTRY` | program entry | **program entry** `prog.entry` — interp jumps here after linking |
| `AT_BASE`  | `0` | **interpreter load base** `interp.base` — the key change |
| `AT_EXECFN`| (was absent) | **pointer to guest argv0 string** (added; inert for static) |
| everything else (`AT_PAGESZ`, `AT_FLAGS`, `AT_UID/EUID/GID/EGID`, `AT_HWCAP/2`, `AT_CLKTCK`, `AT_RANDOM`, `AT_SECURE=0`, `AT_SYSINFO_EHDR`) | as-is | identical |
| **jump target** | `prog.entry` | **`interp.entry`** |

There are exactly **three** auxv deltas plus the jump target:
`AT_BASE` flips `0` -> `interp.base`; `AT_ENTRY`/`AT_PHDR`/`AT_PHNUM`/`AT_PHENT`
keep describing the **program** (they do NOT switch to the interpreter); and we
**add `AT_EXECFN`**. `AT_PHDR` pointing at the program is what lets `ld.so` find
the main object's `PT_DYNAMIC`, walk its `DT_NEEDED`, and link it. `AT_BASE` being
the interpreter base is how `ld.so` computes its own load bias and self-relocates
its `R_AARCH64_RELATIVE` entries early in `_dl_start`.

### 3.2 argv0 / AT_EXECFN string (replace the hardcoded "/bin/hello")

The original code at lines 1219–1222 hardcodes `a0 = "/bin/hello"`. Replace it
with the real guest path captured in the child from `guest_rel` so dynamic argv0
and `AT_EXECFN` are correct (behavior-neutral for the hello test). `guest_rel` is
a `std::string` in the enclosing function; it is readable in the child.

Replace original lines 1219–1222:

```cpp
        const char* a0 = "/bin/hello";
        top -= 11;
        std::memcpy(reinterpret_cast<void*>(top), a0, 11);
        const uintptr_t argv0 = top;
```

with:

```cpp
        const char* a0 = guest_rel.c_str();        // e.g. "/bin/dash"
        const std::size_t a0n = guest_rel.size() + 1;
        top -= a0n;
        std::memcpy(reinterpret_cast<void*>(top), a0, a0n);
        const uintptr_t argv0 = top;               // also used as AT_EXECFN
```

### 3.3 LD_LIBRARY_PATH envp for the first dynamic test

The original pushes a single envp string `GLIBC_TUNABLES=...` (lines 1225–1228).
For the dynamic case we add a second envp entry, `LD_LIBRARY_PATH`, pointing at
the rootfs lib dirs as absolute Android paths so `ld.so`'s `openat` finds
`libc.so.6` without any path interposition (tier 1 in §4.2). Build it on the stack
the same way. Replace lines 1225–1228:

```cpp
        const char* e0 = "GLIBC_TUNABLES=glibc.pthread.rseq=0";
        top -= 36;  // strlen + 1
        std::memcpy(reinterpret_cast<void*>(top), e0, 36);
        const uintptr_t envp0 = top;
```

with:

```cpp
        const char* e0 = "GLIBC_TUNABLES=glibc.pthread.rseq=0";
        top -= 36;  // strlen + 1
        std::memcpy(reinterpret_cast<void*>(top), e0, 36);
        const uintptr_t envp0 = top;
        // Second envp entry (dynamic only): absolute Android LD_LIBRARY_PATH so
        // ld.so resolves DT_NEEDED libs from the extracted rootfs without needing
        // the live path-mediation hook. Standard Debian arm64 multiarch dirs.
        uintptr_t envp1 = 0;
        std::string ld_path;  // kept alive until after the stack copy
        if (dynamic) {
            ld_path = "LD_LIBRARY_PATH=";
            ld_path += config.rootfs_dir; ld_path += "/lib/aarch64-linux-gnu:";
            ld_path += config.rootfs_dir; ld_path += "/lib:";
            ld_path += config.rootfs_dir; ld_path += "/usr/lib/aarch64-linux-gnu:";
            ld_path += config.rootfs_dir; ld_path += "/usr/lib";
            const std::size_t ln = ld_path.size() + 1;
            top -= ln;
            std::memcpy(reinterpret_cast<void*>(top), ld_path.c_str(), ln);
            envp1 = top;
        }
```

Important alignment note: the stack-string pushes above happen before `start` is
re-aligned to 16 bytes at the original line 1276 (`uintptr_t start = (top - ... ) & ~0xf`),
so pushing a variable-length string does not break the final SP alignment — the
mask at line 1276 re-establishes it. No change to that masking line is needed.

### 3.4 The auxv array and word count (replace lines 1254–1286)

Replace the original `aux[]` (lines 1254–1273), `n_aux`/`n_words` (1274–1275), and
the `sp_words` packing loop (1276–1286) with the version below. Note `n_words`
must account for the optional second envp entry.

```cpp
        const uint64_t aux[] = {
            AT_PHDR,         prog.phdr,                      // PROGRAM phdrs (both cases)
            AT_PHENT,        prog.phent,
            AT_PHNUM,        prog.phnum,
            AT_PAGESZ,       4096,
            AT_BASE,         dynamic ? interp.base : 0,      // interp base when dynamic, else 0
            AT_FLAGS,        0,
            AT_ENTRY,        prog.entry,                     // PROGRAM entry (both cases)
            AT_EXECFN,       argv0,                          // added (inert for static)
            AT_UID,          ::getuid(),
            AT_EUID,         ::geteuid(),
            AT_GID,          ::getgid(),
            AT_EGID,         ::getegid(),
            AT_HWCAP,        ::getauxval(AT_HWCAP),
            AT_HWCAP2,       ::getauxval(AT_HWCAP2),
            AT_CLKTCK,       100,
            AT_RANDOM,       at_random,
            AT_SECURE,       0,
            AT_SYSINFO_EHDR, ::getauxval(AT_SYSINFO_EHDR),
            AT_NULL,         0,
        };
        const std::size_t n_aux = sizeof(aux) / sizeof(aux[0]);
        const std::size_t n_env = dynamic ? 2 : 1;          // envp0 [+ envp1]
        // argc + (argv0,NULL) + (env...,NULL) + auxv
        const std::size_t n_words = 1 + 2 + (n_env + 1) + n_aux;
        uintptr_t start = (top - n_words * 8) & ~static_cast<uintptr_t>(0xf);
        uint64_t* sp_words = reinterpret_cast<uint64_t*>(start);
        std::size_t k = 0;
        sp_words[k++] = 1;        // argc
        sp_words[k++] = argv0;    // argv[0]
        sp_words[k++] = 0;        // argv NULL
        sp_words[k++] = envp0;    // envp[0]
        if (dynamic) {
            sp_words[k++] = envp1;  // envp[1] = LD_LIBRARY_PATH
        }
        sp_words[k++] = 0;        // envp NULL
        for (std::size_t i = 0; i < n_aux; ++i) {
            sp_words[k++] = aux[i];
        }
```

### 3.5 The jump target + diagnostics (replace lines 1291–1308)

Replace the original `entry` definition and the pre-jump dump (lines 1291–1308)
with the version below. The TCB region setup at lines 1289–1290 is unchanged.

```cpp
        const uintptr_t jump_entry = dynamic ? interp.entry : prog.entry;
        // Pre-jump verification: entry addr + its first instruction word, the
        // PROGRAM's AT_PHDR + its first word, sp + argc, and (dynamic) the interp
        // base + program entry, so a device failure localizes to a precise stage.
        ::write(dg, "E@", 2);     diag_hex(dg, jump_entry);
        ::write(dg, "code=", 5);  diag_hex(dg, *reinterpret_cast<volatile uint64_t*>(jump_entry));
        ::write(dg, "P@", 2);     diag_hex(dg, prog.phdr);
        ::write(dg, "p0=", 3);    diag_hex(dg, *reinterpret_cast<volatile uint64_t*>(prog.phdr));
        ::write(dg, "sp@", 3);    diag_hex(dg, start);
        ::write(dg, "argc=", 5);  diag_hex(dg, sp_words[0]);
        if (dynamic) {
            ::write(dg, "BASE=", 5);  diag_hex(dg, interp.base);
            ::write(dg, "PROGE=", 6); diag_hex(dg, prog.entry);
            ::write(dg, "PROGB=", 6); diag_hex(dg, prog.base);
            ::write(dg, "INTRE@", 6); diag_hex(dg, interp.entry);
        }
        ::write(dg, "JUMPING;", 8);
        ::alarm(dynamic ? 8 : 5);  // ld.so + libc bring-up is heavier than a static _start
        alr_enter_guest(reinterpret_cast<void*>(start),
                        reinterpret_cast<void*>(jump_entry),
                        reinterpret_cast<void*>(tcb));
        _exit(99);  // unreachable
```

That is the complete set of child-branch edits. The `#else` (NOARCH) branch at
lines 1309–1312 is unchanged.

---

## 4. (c) static vs dynamic detection and branching — summary

Detection: a single loop over the program's phdrs for `PT_INTERP` (EDIT 2). If
absent -> `dynamic == false`, and:
- `interp_bytes` is never populated, the interpreter map call is skipped,
- `AT_BASE == 0`, jump target == `prog.entry`, only one envp entry,
- `argv0`/`AT_EXECFN` point at `guest_rel` (== `/bin/hello` for the existing test).

This is exactly today's static behavior plus an inert `AT_EXECFN`, so `/bin/hello`
and `/bin/mt-test` are unaffected. The static program is still mapped by the same
code (now via `map_elf_image_into_execmem`, whose `ET_DYN` reserve branch is the
same static-PIE path the device already exercised, and whose `SHT_RELA` fallback
is the original static IRELATIVE walk verbatim).

---

## 5. (d) First on-device test

### 5.1 Compile a DYNAMIC glibc aarch64 hello with zig (NOT -static)

Toolchain/sysroot confirmed present:
- `zig` 0.16.0 at `/opt/homebrew/bin/zig`.
- Full Debian arm64 glibc sysroot at `/tmp/mtbuild/sysroot`, containing:
  - `/tmp/mtbuild/sysroot/lib/ld-linux-aarch64.so.1` (the interpreter the program
    will name in `PT_INTERP`),
  - `/tmp/mtbuild/sysroot/lib/aarch64-linux-gnu/libc.so.6` (the runtime libc),
  - `/tmp/mtbuild/sysroot/usr/lib/aarch64-linux-gnu/{Scrt1.o,crti.o,crtn.o,libc.so}`
    (link inputs).

Write `/tmp/dynhello.c`:

```c
#include <stdio.h>
int main(void) { puts("alr-dyn-ok"); return 0; }
```

Compile a **dynamically-linked PIE** (the default; explicitly no `-static`):

```sh
zig cc --target=aarch64-linux-gnu \
  --sysroot=/tmp/mtbuild/sysroot \
  -isystem /tmp/mtbuild/sysroot/usr/include \
  -isystem /tmp/mtbuild/sysroot/usr/include/aarch64-linux-gnu \
  -B /tmp/mtbuild/sysroot/usr/lib/aarch64-linux-gnu \
  -L /tmp/mtbuild/sysroot/usr/lib/aarch64-linux-gnu \
  -L /tmp/mtbuild/sysroot/lib/aarch64-linux-gnu \
  -Wl,-dynamic-linker,/lib/ld-linux-aarch64.so.1 \
  -Wl,-rpath-link,/tmp/mtbuild/sysroot/lib/aarch64-linux-gnu \
  -fPIE -pie \
  -o /tmp/dynhello /tmp/dynhello.c
```

Verify it is `ET_DYN` with the expected `PT_INTERP` and is NOT statically linked:

```sh
zig cc --target=aarch64-linux-gnu -E -dM - </dev/null >/dev/null 2>&1  # sanity: toolchain ok
llvm-readelf -hl /tmp/dynhello 2>/dev/null | grep -E 'Type:|INTERP|/lib/ld-linux' \
  || /opt/homebrew/opt/llvm/bin/llvm-readelf -hl /tmp/dynhello | grep -E 'Type:|INTERP|/lib/ld-linux'
```

Expected: `Type: DYN (Position-Independent Executable file)` and an `INTERP`
segment requesting `/lib/ld-linux-aarch64.so.1`. If `zig cc` cannot find the crt
objects, add `-nostartfiles` is NOT wanted — instead ensure `-B .../aarch64-linux-gnu`
points at the dir holding `Scrt1.o` (it does, per the listing above). As a
fallback that sidesteps zig's own libc shims entirely, use the cached gcc
cross-toolchain if present under `/tmp/glibc-arm64/gcc` (a `aarch64-linux-gnu-gcc`
there will already default to this sysroot's dynamic linker).

Then push it into the rootfs that the app extracts (so it is reachable as a guest
path) and run the probe against it as the guest program `/bin/dynhello`:

```sh
# stage into the build rootfs that gets packaged/extracted to filesDir on device
cp /tmp/dynhello "$ROOTFS_SRC/bin/dynhello"
# ensure the interpreter + libc exist in the SAME rootfs (they are part of the
# normal rootfs; if testing a minimal rootfs, copy them in):
#   $ROOTFS_SRC/lib/ld-linux-aarch64.so.1
#   $ROOTFS_SRC/lib/aarch64-linux-gnu/{libc.so.6,ld-linux-aarch64.so.1}
```

Drive the probe with `input.program = "/bin/dynhello"` (the same entry point that
selects `/bin/hello`/`/bin/mt-test` today). The smallest success signal is the
program's own stdout: the existing `ran = (code == 0 && !guest_stdout.empty())`
gate (line 1441) plus stdout containing `alr-dyn-ok`. Even before the program runs
to completion, the staged diags below prove the interpreter was entered.

### 5.2 How the in-process ld.so finds libc.so.6

Tier 1 (this first test, zero interposition): the child pushes
`LD_LIBRARY_PATH=<rootfs>/lib/aarch64-linux-gnu:<rootfs>/lib:<rootfs>/usr/lib/aarch64-linux-gnu:<rootfs>/usr/lib`
onto the guest envp (EDIT 3.3), where `<rootfs>` is `config.rootfs_dir` — the
absolute Android path the rootfs extracts to under `filesDir`. `ld.so` then
`openat`s `libc.so.6` at a real Android-visible absolute path with no path
rewriting. The program's `PT_INTERP` string (`/lib/ld-linux-aarch64.so.1`) is
**never used at runtime** — we already chose, mapped, and jump to the interpreter
ourselves; `ld.so` uses `LD_LIBRARY_PATH`/`DT_RUNPATH`/default dirs only for the
*libraries*. This depends on path-mediation being proven only insofar as the
extracted rootfs lib files exist at those absolute paths (they do); no live hook
is required for tier 1.

`/etc/ld.so.cache`: for tier 1 it must be **absent** (or it will send `openat` to
guest-absolute paths under the Android root and fail). glibc falls back to
`LD_LIBRARY_PATH` + default dirs when `openat("/etc/ld.so.cache")` returns
`-ENOENT`. Simplest first-test policy: do not ship `ld.so.cache` in the test
rootfs. (Tier 2, the live `LD_PRELOAD`/`SECCOMP_USER_NOTIF` path-mediation hook —
already on the roadmap — translates `/etc/ld.so.cache` and all guest-absolute
`openat` paths to `rootfs_dir + path`, after which arbitrary rootfs programs
resolve libs exactly as on real Debian with no `LD_LIBRARY_PATH` hack.)

### 5.3 The one real W^X nuance after the jump

We map **our** two images (program + interpreter) into anonymous execmem
(RW->memcpy->RX), the device-proven W^X-safe path. But once running, `ld.so`
itself maps `libc.so.6` etc. with **file-backed** `mmap(PROT_READ|PROT_EXEC,
MAP_PRIVATE, fd)`. On `untrusted_app`, `memfd`-exec is EACCES and file-backed
`PROT_EXEC` of a file on the app-data filesystem **may** be SELinux-denied
(`execute`/`execmod` on `app_data_file`). This is the principal device risk for
the dynamic path (Risk 1 below). If the first smoke faults inside an
`ld.so`-issued `mmap`/`mprotect` (fault PC resolves into `ld.so`'s mapped text,
not our execmem), copy `ld-linux-aarch64.so.1` + `libc.so.6` + the program's
direct `DT_NEEDED` set into an executable-mappable directory (the app's
`nativeLibraryDir`/`code_cache`, where the app's own `.so`s are exec-mappable) and
point `LD_LIBRARY_PATH` there. The production answer is to interpose those
library maps into the same anon-RW->memcpy->RX path (the planned `mmap`-interpose
layer) — out of scope for the first smoke.

---

## 6. (e) Ordered risks and the diagnostics that debug each from the app report

Diagnostics are emitted to the `dg` (diag) pipe and surfaced via
`out << "\nalr native loader diag=" << diag;` (line 1466). Per-stage tags make a
device failure localizable without a debugger; the parent's ptrace fault-capture
(`fault pc/addr/lr/x0/x8`, lines 1447–1453) pinpoints where a crash landed, and
`fault pc` can be resolved against `/proc/<pid>/maps` to our execmem vs an
`ld.so`-issued lib map vs bionic.

1. **File-backed `PROT_EXEC` library `mmap` denied by SELinux (highest risk).**
   Symptom: diag shows `...JUMPING;` then a captured fault with `fault pc`
   resolving into an `ld.so` text mapping (not our `PROG:`/`INTERP:` execmem), or
   `ld.so` aborts after a failed `mmap`. Diagnostic already present: per-image
   `PROG:`/`INTERP:` tags confirm OUR maps succeeded, so a fault past `MAPPED;` is
   in `ld.so`'s own library mapping. Mitigation: §5.3 (copy libs to exec-mappable
   dir, or the mmap-interpose layer).

2. **`ld.so` cannot find `libc.so.6` (`openat` path resolution).** Symptom:
   `JUMPING;` then the program exits nonzero / no stdout, often with the guest
   dying quickly; the emulated-syscall list shows repeated `openat` (`__NR_openat`
   = 56) returning failures. Diagnostic: add nothing — the existing
   `seccomp-emulated syscalls nums=` line plus exit code tells the story; if
   needed, temporarily run `ld.so` with `LD_DEBUG=libs` via a third envp entry to
   make it print its search to the captured stderr/stdout. Mitigation: tier-1
   `LD_LIBRARY_PATH` (already in EDIT 3.3); ensure no `/etc/ld.so.cache`.

3. **Interpreter open/parse failure (rootfs path or symlink escape).** Symptom:
   diag shows `DYN;` then `INTERP_OPEN_FAIL;` (exit 75) or `INTERP_BAD_ELF;`
   (exit 76). Cause: `config.rootfs_dir + "/lib/ld-linux-aarch64.so.1"` does not
   resolve, or the symlink target is absolute (`/lib/aarch64-linux-gnu/...`) and
   escapes to the Android root. Mitigation: ensure the interpreter (real file or a
   rootfs-relative symlink) exists under the extracted rootfs; if the link is
   absolute, point `PT_INTERP`/the open at the concrete
   `lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`.

4. **Interpreter IRELATIVE not applied (section-stripped ld.so).** Symptom: diag
   shows `INTERP:IREL=0x0` and then a fault inside `ld.so` very early (calling an
   IFUNC `memcpy`/`strcmp`). Diagnostic already present: `INTERP:IREL=` prints the
   count; `0` on a real `ld.so` means the `PT_DYNAMIC` `DT_RELA` walk found
   nothing. The helper's `PT_DYNAMIC`-first walk (EDIT 1) is specifically the fix;
   if it still reads 0, dump `DT_RELA`/`DT_RELASZ` by temporarily writing them to
   `dg` inside the helper.

5. **Wrong auxv handoff (program not linked / segfault in `__libc_start_main`).**
   Symptom: `JUMPING;` with `BASE=`, `PROGE=`, `PROGB=`, `INTRE@` printed, then a
   fault whose `pc` is inside the program's mapped text or libc. Diagnostic
   already present: compare `BASE` vs the interp execmem range, `PROGE` vs the
   program execmem range, and `P@`/`p0=` (the program's `AT_PHDR` and its first
   word — should look like a valid `Elf64_Phdr`). A mismatch (e.g. `AT_PHDR`
   pointing at the interpreter, or `AT_BASE==0` while dynamic) is the bug.

6. **TLS/robust-list coexistence (same as static, carried over).** Symptom:
   handful of `SIGSYS` emulations for `set_robust_list`/`rseq`/`faccessat2`
   returning `-ENOSYS` — expected and benign (the supervisor already does this).
   Diagnostic: the `seccomp-emulated syscalls nums=` line. Only a concern if the
   count explodes (the supervisor SIGKILLs past 8192, line 1401).

7. **Threads/forks from the program.** Symptom: `guest threads spawned=` > 0;
   handled by the existing `PTRACE_O_TRACECLONE|FORK|VFORK|EXEC`. No change.

---

## 7. EDIT 4 (optional) — parent report gate for the link mode

Purely cosmetic surfacing in the report. After the existing `mapped`/`jumped`
booleans (lines 1436–1437) add, and emit alongside the existing MAP/EXEC lines
(near line 1442):

```cpp
    const bool is_dyn    = diag.find("DYN;") != std::string::npos;
    const bool interp_ok = diag.find("INTERP:IREL=") != std::string::npos;  // interp mapped+reloc'd
    out << "\nALR NATIVE LOADER LINK MODE: " << (is_dyn ? "DYNAMIC(interp-handoff)" : "STATIC");
    out << "\nALR NATIVE LOADER INTERP MAP: "
        << (!is_dyn ? "SKIP" : interp_ok ? "PASS" : "FAIL");
```

The existing `GUEST EXEC` gate (`ran = code==0 && !guest_stdout.empty()`,
line 1441) already covers the dynamic success case unchanged: run the guest as
`/bin/dynhello` (or later `/bin/dash -c 'echo alr-dyn-ok'`) and assert stdout
contains `alr-dyn-ok`.

---

## 8. Compile-cleanliness checklist (-std=c++20 -Wall -Wextra -Werror)

- `MappedImage` uses default member initializers and is constructed as
  `MappedImage{}` / returned by value — no `missing-field-initializers`.
- The `aux[]` array is fully initialized (every entry is a `{tag, value}` pair),
  same as the original.
- `interp{}` and `prog` are both fully initialized (`prog` from a function return,
  `interp` via `{}` then conditionally assigned).
- `ld_path`/`interp_bytes`/`interp_host` are `std::string` and used; `envp1` is
  always initialized to `0` and only read when `dynamic` (the packing loop guards
  on `dynamic`). No unused variables: `interp` is read via `interp.base`/`.entry`
  only under `dynamic`, but the variable itself is always referenced (assigned),
  so no `-Wunused-variable`. If the compiler warns that `interp` is unused in a
  pure-static translation (it cannot, since it is assigned and read), add
  `(void)interp;` — not expected to be necessary.
- All casts are explicit `static_cast`/`reinterpret_cast` as in the surrounding
  code. `ssize_t in` compared against `0`; `::read` return cast to `std::size_t`
  only after the `> 0` check, matching the existing read loop at lines 1046–1049.
- `diag_hex` takes `unsigned long long`; all counts/sizes are cast accordingly.

---

## 9. Exact diff summary (what changes, by original line range)

1. **Before line 1024**: add `MappedImage` + `map_elf_image_into_execmem` (EDIT 1).
2. **Lines 1090–1181** (child static map+IRELATIVE body): replace with
   PT_INTERP detection + interpreter read + two helper calls (EDIT 2). Keep
   1087–1088 (`ce`/`ph`).
3. **Line 1202**: `"SIGRESET;MAPPED;"` (16) -> `"SIGRESET;"` (9); `MAPPED;` now
   emitted in EDIT 2.
4. **Lines 1219–1222**: hardcoded `"/bin/hello"` argv0 -> `guest_rel` (EDIT 3.2).
5. **Lines 1225–1228**: single envp -> envp0 + optional `LD_LIBRARY_PATH` envp1
   (EDIT 3.3).
6. **Lines 1254–1286**: `aux[]` (`AT_BASE`=`dynamic?interp.base:0`, add
   `AT_EXECFN`, use `prog.*`), `n_env`/`n_words`, and the packing loop with the
   optional second envp (EDIT 3.4).
7. **Lines 1291–1308**: `entry` -> `jump_entry = dynamic?interp.entry:prog.entry`,
   staged dynamic diagnostics, `alarm(dynamic?8:5)` (EDIT 3.5).
8. **Lines 1436–1442** (parent): optional LINK MODE / INTERP MAP report lines
   (EDIT 4).

**Unchanged and reused:** the `PROT_NONE` reserve + `MAP_FIXED` RW->memcpy->RX
per-`PT_LOAD` (now inside the helper), raw `rt_sigaction` SIG_DFL reset +
`rt_sigprocmask` unblock (1190–1201), clean zeroed TCB via `tpidr_el0`
(1289–1290), `alr_enter_guest` (1007–1017), and the entire
`waitpid(-1, __WALL)` multi-tracee SIGSYS=`-ENOSYS` supervisor (1314–1424).
