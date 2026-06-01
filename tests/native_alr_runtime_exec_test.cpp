#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_exec.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream os(path, std::ios::binary);
    os << bytes;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("alr-exec-resolution-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bin");
    std::filesystem::create_directories(root / "usr" / "bin");

    write_bytes(root / "bin" / "hello", std::string("\x7f""ELF", 4) + "fake-static-elf");
    write_bytes(root / "usr" / "bin" / "script", "#!/bin/sh -e\necho script\n");
    write_bytes(root / "usr" / "bin" / "plain", "plain text\n");

    const auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {
            {"ALR_TRAMPOLINE_PATH", "/native/libalr_runtime_trampoline.so"},
            {"PATH", "/usr/bin:/bin"},
        },
    };

    const auto elf = alr::runtime::resolve_guest_executable(config, "/bin/hello");
    require(elf.resolved, "elf resolved");
    require(elf.classified, "elf classified");
    require(elf.kind == alr::runtime::ExecutableKind::Elf, "elf kind");
    require(elf.translation.guest_path == "/bin/hello", "elf guest path");
    require(elf.report.find("ALR EXEC RESOLVE: PASS") != std::string::npos, "elf resolve report");
    require(elf.report.find("alr exec kind=elf") != std::string::npos, "elf kind report");

    const auto by_path = alr::runtime::resolve_guest_executable(config, "script");
    require(by_path.resolved, "PATH command resolved");
    require(by_path.kind == alr::runtime::ExecutableKind::Shebang, "script kind");
    require(by_path.translation.guest_path == "/usr/bin/script", "PATH command guest path");
    require(by_path.shebang.interpreter == "/bin/sh", "shebang interpreter");
    require(by_path.shebang.argument == "-e", "shebang argument");
    require(by_path.report.find("alr exec shebang interpreter=/bin/sh") != std::string::npos, "shebang report");

    const auto plain = alr::runtime::resolve_guest_executable(config, "/usr/bin/plain");
    require(plain.resolved, "plain resolved");
    require(plain.kind == alr::runtime::ExecutableKind::Unsupported, "plain unsupported");
    require(plain.report.find("alr exec kind=unsupported") != std::string::npos, "unsupported report");

    const auto missing = alr::runtime::resolve_guest_executable(config, "missing");
    require(!missing.resolved, "missing not resolved");
    require(!missing.classified, "missing not classified");
    require(missing.kind == alr::runtime::ExecutableKind::Missing, "missing kind");
    require(missing.report.find("ALR EXEC RESOLVE: FAIL") != std::string::npos, "missing report");
    require(missing.report.find("alr exec kind=missing") != std::string::npos, "missing kind report");

    const auto execve_plan = alr::runtime::build_exec_continuation_plan(
        config,
        "/bin/hello",
        {"--smoke"},
        alr::runtime::ExecContinuationKind::Execve);
    require(execve_plan.planned, "execve continuation planned");
    require(execve_plan.uses_packaged_trampoline, "execve uses packaged trampoline");
    require(execve_plan.config_handoff, "execve config handoff");
    require(execve_plan.argv.front() == "/native/libalr_runtime_trampoline.so", "execve trampoline argv0");
    require(execve_plan.argv.back() == "--smoke", "execve child argument preserved");
    require(execve_plan.report.find("ALR EXECVE CHILD CONTINUITY PLAN: PASS") != std::string::npos, "execve continuity report");
    require(execve_plan.report.find("ALR EXEC CONTINUITY TRAMPOLINE: PASS") != std::string::npos, "trampoline continuity report");

    const auto execvp_plan = alr::runtime::build_exec_continuation_plan(
        config,
        "script",
        {},
        alr::runtime::ExecContinuationKind::Execvp);
    require(execvp_plan.planned, "execvp continuation planned");
    require(execvp_plan.path_lookup, "execvp path lookup");
    require(execvp_plan.resolution.translation.guest_path == "/usr/bin/script", "execvp guest path");
    require(execvp_plan.report.find("ALR EXECVP PATH LOOKUP PLAN: PASS") != std::string::npos, "execvp continuity report");

    const auto spawn_plan = alr::runtime::build_exec_continuation_plan(
        config,
        "/bin/hello",
        {},
        alr::runtime::ExecContinuationKind::PosixSpawn);
    require(spawn_plan.planned, "posix_spawn continuation planned");
    require(spawn_plan.report.find("ALR POSIX_SPAWN CHILD PLAN: PASS") != std::string::npos, "posix_spawn continuity report");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime exec native test ok\n";
    return EXIT_SUCCESS;
}
