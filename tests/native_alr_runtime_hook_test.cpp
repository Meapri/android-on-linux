#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_hook.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("alr-hook-smoke-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "etc");
    {
        std::ofstream os(root / "etc" / "os-release");
        os << "NAME=\"AndroLinux Tiny Rootfs\"\n";
    }

    const auto result = alr::runtime::run_path_hook_smoke(root.string(), "/", "/etc/os-release");
    require(result.translation.guest_path == "/etc/os-release", "guest path translated");
    require(result.translation.host_path == (root / "etc" / "os-release").string(), "host path translated");
    require(result.stated, "stat passed");
    require(result.opened, "open passed");
    require(result.size_bytes > 0, "size captured");
    require(result.first_bytes.find("AndroLinux Tiny Rootfs") != std::string::npos, "first bytes captured");
    require(result.report.find("ALR HOOK LOAD: PASS") != std::string::npos, "hook load report");
    require(result.report.find("ALR STAT ROOTFS FILE: PASS") != std::string::npos, "stat report");
    require(result.report.find("ALR OPEN ROOTFS FILE: PASS") != std::string::npos, "open report");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime hook native test ok\n";
    return EXIT_SUCCESS;
}
