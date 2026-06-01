#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_elf_format.hpp"
#include "../app/src/main/cpp/alr_runtime/alr_elf.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_at(std::vector<unsigned char>& bytes, std::size_t offset, const void* data, std::size_t size) {
    if (offset + size > bytes.size()) {
        bytes.resize(offset + size);
    }
    std::memcpy(bytes.data() + offset, data, size);
}

void write_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream os(path, std::ios::binary);
    os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<unsigned char> make_elf(bool dynamic) {
    std::vector<unsigned char> bytes(0x3000);

    Elf64_Ehdr ehdr {};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = dynamic ? ET_DYN : ET_EXEC;
    ehdr.e_machine = EM_AARCH64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = dynamic ? 0x640 : 0x400080;
    ehdr.e_phoff = sizeof(Elf64_Ehdr);
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = dynamic ? 3 : 2;
    write_at(bytes, 0, &ehdr, sizeof(ehdr));

    Elf64_Phdr first_load {};
    first_load.p_type = PT_LOAD;
    first_load.p_flags = PF_R | PF_X;
    first_load.p_offset = 0;
    first_load.p_vaddr = dynamic ? 0 : 0x400000;
    first_load.p_filesz = 0x1000;
    first_load.p_memsz = 0x1000;
    first_load.p_align = 0x10000;
    write_at(bytes, ehdr.e_phoff, &first_load, sizeof(first_load));

    Elf64_Phdr second_load {};
    second_load.p_type = PT_LOAD;
    second_load.p_flags = PF_R | PF_W;
    second_load.p_offset = 0x2000;
    second_load.p_vaddr = dynamic ? 0x12000 : 0x480000;
    second_load.p_filesz = 0x100;
    second_load.p_memsz = 0x200;
    second_load.p_align = 0x10000;
    write_at(bytes, ehdr.e_phoff + sizeof(Elf64_Phdr), &second_load, sizeof(second_load));

    if (dynamic) {
        const char interp[] = "/lib/ld-linux-aarch64.so.1";
        Elf64_Phdr interp_phdr {};
        interp_phdr.p_type = PT_INTERP;
        interp_phdr.p_flags = PF_R;
        interp_phdr.p_offset = 0x238;
        interp_phdr.p_vaddr = 0x238;
        interp_phdr.p_filesz = sizeof(interp);
        interp_phdr.p_memsz = sizeof(interp);
        interp_phdr.p_align = 1;
        write_at(bytes, ehdr.e_phoff + sizeof(Elf64_Phdr) * 2, &interp_phdr, sizeof(interp_phdr));
        write_at(bytes, interp_phdr.p_offset, interp, sizeof(interp));
    }
    return bytes;
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("alr-elf-load-plan-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto static_path = dir / "hello-static";
    const auto dynamic_path = dir / "glibc-hello";
    const auto bad_path = dir / "not-elf";
    write_file(static_path, make_elf(false));
    write_file(dynamic_path, make_elf(true));
    write_file(bad_path, std::vector<unsigned char>{'n', 'o', 'p', 'e'});

    const auto static_plan = alr::runtime::build_elf_load_plan(static_path.string());
    require(static_plan.valid, "static ELF valid");
    require(static_plan.status == alr::runtime::ElfLoadPlanStatus::StaticExecutable, "static status");
    require(static_plan.elf_class == "elf64", "static class");
    require(static_plan.machine == "aarch64", "static machine");
    require(static_plan.type == "exec", "static type");
    require(static_plan.load_segment_count == 2, "static load count");
    require(static_plan.interpreter.empty(), "static no interp");
    require(static_plan.report.find("ALR ELF LOAD PLAN: PASS") != std::string::npos, "static report");
    require(static_plan.report.find("ALR ELF STATIC HELLO CANDIDATE: PASS") != std::string::npos, "static candidate report");

    const auto dynamic_plan = alr::runtime::build_elf_load_plan(dynamic_path.string());
    require(dynamic_plan.valid, "dynamic ELF valid");
    require(dynamic_plan.status == alr::runtime::ElfLoadPlanStatus::InterpreterNeeded, "dynamic status");
    require(dynamic_plan.type == "dyn", "dynamic type");
    require(dynamic_plan.interpreter == "/lib/ld-linux-aarch64.so.1", "dynamic interp");
    require(dynamic_plan.report.find("ALR ELF STATIC HELLO CANDIDATE: SKIP") != std::string::npos, "dynamic candidate report");

    const auto bad_plan = alr::runtime::build_elf_load_plan(bad_path.string());
    require(!bad_plan.valid, "bad ELF invalid");
    require(bad_plan.status == alr::runtime::ElfLoadPlanStatus::Unsupported, "bad status");
    require(bad_plan.report.find("ALR ELF LOAD PLAN: FAIL") != std::string::npos, "bad report");

    std::filesystem::remove_all(dir);
    std::cout << "alr runtime elf native test ok\n";
    return EXIT_SUCCESS;
}
