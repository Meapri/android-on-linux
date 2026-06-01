#pragma once

#include <cstdint>

#if defined(__has_include)
#  if __has_include(<elf.h>)
#    include <elf.h>
#    define ALR_HAVE_SYSTEM_ELF_H 1
#  endif
#endif

#ifndef ALR_HAVE_SYSTEM_ELF_H

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6

#define ELFMAG "\177ELF"
#define SELFMAG 4

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

#define ET_EXEC 2
#define ET_DYN 3

#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_INTERP 3

#define PF_X 1
#define PF_W 2
#define PF_R 4

using Elf64_Half = std::uint16_t;
using Elf64_Word = std::uint32_t;
using Elf64_Addr = std::uint64_t;
using Elf64_Off = std::uint64_t;

struct Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

struct Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    std::uint64_t p_filesz;
    std::uint64_t p_memsz;
    std::uint64_t p_align;
};

#endif
