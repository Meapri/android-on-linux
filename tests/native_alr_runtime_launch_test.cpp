#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_launch.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream os(path, std::ios::binary);
    os << text;
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
        ("alr-launch-attempt-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bin");
    write_text(root / "bin" / "hello", "#!/bin/sh\necho alr controlled hello \"$1\"\n");

    const auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {
            {"ALR_TRAMPOLINE_PATH", "/native/libalr_runtime_trampoline.so"},
            {"PATH", "/bin"},
        },
    };

    const auto blocked = alr::runtime::attempt_guest_launch(config, "/bin/hello");
    require(blocked.resolution.resolved, "blocked attempt resolves executable");
    require(blocked.policy_blocked, "default policy blocks direct exec");
    require(!blocked.attempted, "blocked attempt does not fork");
    require(blocked.exit_code == -1, "blocked exit code");
    require(blocked.report.find("ALR LAUNCH ATTEMPT: SKIP") != std::string::npos, "blocked attempt report");
    require(blocked.report.find("ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: SKIP") != std::string::npos, "blocked hello report");
    require(blocked.report.find("ALR EXECVE CHILD CONTINUITY PLAN: PASS") != std::string::npos, "blocked continuation report");

    const auto allowed = alr::runtime::attempt_guest_launch(
        config,
        "/bin/hello",
        {"world"},
        alr::runtime::LaunchAttemptPolicy{.allow_direct_host_exec = true});
    require(allowed.attempted, "allowed attempt forks");
    require(!allowed.policy_blocked, "allowed attempt not blocked");
    require(allowed.exit_code == 0, "allowed exit code");
    require(allowed.stdout_text == "alr controlled hello world", "allowed stdout");
    require(allowed.stderr_text.empty(), "allowed stderr");
    require(allowed.report.find("ALR LAUNCH ATTEMPT: PASS") != std::string::npos, "allowed attempt report");
    require(allowed.report.find("ALR LAUNCH MODE: direct-host-exec") != std::string::npos, "allowed mode report");
    require(allowed.report.find("ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: PASS") != std::string::npos, "allowed hello report");
    require(allowed.report.find("ALR EXEC CONTINUITY CONFIG HANDOFF: PASS") != std::string::npos, "allowed continuation handoff report");

    const auto missing = alr::runtime::attempt_guest_launch(
        config,
        "missing",
        {},
        alr::runtime::LaunchAttemptPolicy{.allow_direct_host_exec = true});
    require(!missing.attempted, "missing attempt does not fork");
    require(missing.exit_code == -1, "missing exit code");
    require(missing.report.find("ALR LAUNCH ATTEMPT: FAIL") != std::string::npos, "missing attempt report");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime launch native test ok\n";
    return EXIT_SUCCESS;
}
