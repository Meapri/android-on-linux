#include "alr_runtime/alr_elf.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "alr_runtime/alr_elf_format.hpp"

namespace alr::runtime {
namespace {

std::string errno_message(const char* action) {
    std::ostringstream out;
    out << action << " failed errno=" << errno << " message=" << std::strerror(errno);
    return out.str();
}

void read_exact(int fd, void* buffer, std::size_t size, std::uint64_t offset) {
    auto* out = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count = ::pread(fd, out + done, size - done, static_cast<off_t>(offset + done));
        if (count > 0) {
            done += static_cast<std::size_t>(count);
        } else if (count == 0) {
            throw std::runtime_error("unexpected EOF while reading ELF");
        } else if (errno != EINTR) {
            throw std::runtime_error(errno_message("pread ELF"));
        }
    }
}

std::string read_string_at(int fd, std::uint64_t offset, std::uint64_t size) {
    if (size == 0 || size > 4096) {
        throw std::runtime_error("invalid ELF string size");
    }
    std::vector<char> buffer(static_cast<std::size_t>(size));
    read_exact(fd, buffer.data(), buffer.size(), offset);
    std::string out(buffer.data(), strnlen(buffer.data(), buffer.size()));
    return out;
}

std::string type_name(std::uint16_t type) {
    switch (type) {
        case ET_EXEC:
            return "exec";
        case ET_DYN:
            return "dyn";
        default:
            return "unsupported";
    }
}

std::string machine_name(std::uint16_t machine) {
    return machine == EM_AARCH64 ? "aarch64" : "unsupported";
}

std::string hex_value(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string build_report(const ElfLoadPlan& plan) {
    std::ostringstream out;
    out << "ALR ELF LOAD PLAN: " << (plan.valid ? "PASS" : "FAIL");
    out << "\nALR ELF STATIC HELLO CANDIDATE: "
        << (plan.status == ElfLoadPlanStatus::StaticExecutable ? "PASS" : "SKIP");
    out << "\nalr elf class=" << plan.elf_class;
    out << "\nalr elf machine=" << plan.machine;
    out << "\nalr elf type=" << plan.type;
    out << "\nalr elf status=" << elf_load_plan_status_name(plan.status);
    out << "\nalr elf entry=" << hex_value(plan.entry);
    out << "\nalr elf min vaddr=" << hex_value(plan.min_vaddr);
    out << "\nalr elf max vaddr=" << hex_value(plan.max_vaddr);
    out << "\nalr elf interp=" << (plan.interpreter.empty() ? "none" : plan.interpreter);
    out << "\nalr elf load segments=" << plan.load_segment_count;
    if (!plan.error.empty()) {
        out << "\nalr elf error=" << plan.error;
    }
    return out.str();
}

}  // namespace

const char* elf_load_plan_status_name(ElfLoadPlanStatus status) {
    switch (status) {
        case ElfLoadPlanStatus::Unsupported:
            return "unsupported";
        case ElfLoadPlanStatus::StaticExecutable:
            return "static-executable";
        case ElfLoadPlanStatus::InterpreterNeeded:
            return "interpreter-needed";
    }
    return "unsupported";
}

ElfLoadPlan build_elf_load_plan(const std::string& host_path) {
    ElfLoadPlan plan;
    const int fd = ::open(host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        plan.error = errno_message("open ELF");
        plan.report = build_report(plan);
        return plan;
    }
    try {
        Elf64_Ehdr ehdr {};
        read_exact(fd, &ehdr, sizeof(ehdr), 0);
        if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
            throw std::runtime_error("not an ELF file");
        }
        plan.elf_class = ehdr.e_ident[EI_CLASS] == ELFCLASS64 ? "elf64" : "unsupported";
        if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
            ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
            ehdr.e_ident[EI_VERSION] != EV_CURRENT ||
            ehdr.e_machine != EM_AARCH64 ||
            ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
            plan.machine = machine_name(ehdr.e_machine);
            plan.type = type_name(ehdr.e_type);
            plan.error = "unsupported ELF header";
            plan.report = build_report(plan);
            ::close(fd);
            return plan;
        }

        plan.machine = "aarch64";
        plan.type = type_name(ehdr.e_type);
        plan.entry = ehdr.e_entry;
        std::uint64_t min_vaddr = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t max_vaddr = 0;
        bool has_load = false;

        for (std::uint16_t i = 0; i < ehdr.e_phnum; ++i) {
            Elf64_Phdr phdr {};
            read_exact(fd, &phdr, sizeof(phdr), ehdr.e_phoff + static_cast<std::uint64_t>(i) * ehdr.e_phentsize);
            if (phdr.p_type == PT_LOAD) {
                has_load = true;
                ++plan.load_segment_count;
                const auto segment_vaddr = static_cast<std::uint64_t>(phdr.p_vaddr);
                const auto segment_end = segment_vaddr + static_cast<std::uint64_t>(phdr.p_memsz);
                if (segment_end < segment_vaddr) {
                    throw std::runtime_error("ELF PT_LOAD segment address overflow");
                }
                min_vaddr = std::min(min_vaddr, segment_vaddr);
                max_vaddr = std::max(max_vaddr, segment_end);
            } else if (phdr.p_type == PT_INTERP) {
                plan.interpreter = read_string_at(fd, phdr.p_offset, phdr.p_filesz);
            }
        }

        if (!has_load) {
            plan.error = "ELF has no PT_LOAD segments";
        } else if (plan.type != "exec" && plan.type != "dyn") {
            plan.error = "unsupported ELF type";
        } else {
            plan.valid = true;
            plan.min_vaddr = min_vaddr;
            plan.max_vaddr = max_vaddr;
            plan.status = plan.interpreter.empty()
                ? ElfLoadPlanStatus::StaticExecutable
                : ElfLoadPlanStatus::InterpreterNeeded;
        }
    } catch (const std::exception& exc) {
        plan.error = exc.what();
    }
    ::close(fd);
    plan.report = build_report(plan);
    return plan;
}

}  // namespace alr::runtime
