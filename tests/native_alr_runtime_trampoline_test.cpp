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
#include "../app/src/main/cpp/alr_runtime/alr_launch.hpp"
#include "../app/src/main/cpp/alr_runtime/alr_trampoline.hpp"

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

std::vector<unsigned char> make_static_aarch64_elf() {
    std::vector<unsigned char> bytes(0x3000);

    Elf64_Ehdr ehdr {};
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_AARCH64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = 0x400080;
    ehdr.e_phoff = sizeof(Elf64_Ehdr);
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum = 1;
    write_at(bytes, 0, &ehdr, sizeof(ehdr));

    Elf64_Phdr load {};
    load.p_type = PT_LOAD;
    load.p_flags = PF_R | PF_X;
    load.p_offset = 0;
    load.p_vaddr = 0x400000;
    load.p_filesz = 0x1000;
    load.p_memsz = 0x1000;
    load.p_align = 0x10000;
    write_at(bytes, ehdr.e_phoff, &load, sizeof(load));
    return bytes;
}

void write_binary(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream os(path, std::ios::binary);
    os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_executable_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream os(path, std::ios::binary);
    os << text;
    os.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add);
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("alr-trampoline-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bin");
    write_binary(root / "bin" / "hello", make_static_aarch64_elf());

    const auto trampoline = root / "libalr_runtime_trampoline.so";
    write_executable_text(
        trampoline,
        "#!/bin/sh\n"
        "echo 'ALR TRAMPOLINE PREFLIGHT: PASS'\n"
        "echo \"alr trampoline test arg=$1\"\n");

    const auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {
            {"ALR_BACKEND", "alr-runtime"},
            {"ALR_PROGRAM", "/bin/hello"},
            {"ALR_ROOTFS", root.string()},
            {"ALR_TRAMPOLINE_PATH", trampoline.string()},
            {"PATH", "/bin"},
        },
    };

    const auto blocked = alr::runtime::attempt_guest_launch(config, "/bin/hello");
    require(blocked.policy_blocked, "default policy blocks direct rootfs exec");
    require(!blocked.trampoline.attempted, "default trampoline does not exec on host");
    require(blocked.report.find("ALR TRAMPOLINE AVAILABLE: PASS") != std::string::npos, "trampoline available");
    require(blocked.report.find("ALR TRAMPOLINE CONFIG HANDOFF: PASS") != std::string::npos, "config handoff");
    require(blocked.report.find("ALR TRAMPOLINE POLICY PREFLIGHT: PASS") != std::string::npos, "policy preflight");
    require(blocked.report.find("ALR STATIC HELLO VIA TRAMPOLINE: SKIP") != std::string::npos, "static hello not claimed");

    const auto resolution = alr::runtime::resolve_guest_executable(config, "/bin/hello");
    const auto elf_plan = alr::runtime::build_elf_load_plan(resolution.translation.host_path);
    const auto executed = alr::runtime::attempt_packaged_trampoline(
        config,
        "/bin/hello",
        resolution,
        elf_plan,
        alr::runtime::TrampolineAttemptPolicy{
            .packaged_trampoline_path = trampoline.string(),
            .allow_trampoline_exec = true,
        });
    require(executed.attempted, "explicit trampoline preflight execs packaged command");
    require(executed.exit_code == 0, "trampoline preflight exit");
    require(executed.stdout_text.find("ALR TRAMPOLINE PREFLIGHT: PASS") != std::string::npos, "trampoline stdout");
    require(executed.report.find("alr trampoline exit=0") != std::string::npos, "trampoline exit report");
    require(executed.report.find("ALR STATIC HELLO VIA TRAMPOLINE: SKIP") != std::string::npos, "no guest entry claim");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime trampoline native test ok\n";
    return EXIT_SUCCESS;
}
