#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_exec.hpp"
#include "../app/src/main/cpp/alr_runtime/alr_wx.hpp"

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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("alr-wx-strategy-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bin");
    write_bytes(root / "bin" / "hello", std::string("\x7f""ELF", 4) + "fake-static-elf");

    auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {
            {"ALR_TRAMPOLINE_PATH", "/data/app/dev.chanwoo.androlinux/lib/arm64/libalr-runtime-trampoline.so"},
            {"PATH", "/usr/bin:/bin"},
        },
    };

    const auto resolution = alr::runtime::resolve_guest_executable(config, "/bin/hello");
    require(resolution.resolved, "guest binary resolved");

    const auto strategy = alr::runtime::build_wx_safe_exec_strategy(config, resolution);
    require(strategy.planned, "wx-safe strategy planned");
    require(strategy.entrypoint_is_packaged, "entrypoint is packaged, not rootfs");
    require(strategy.rejects_direct_rootfs_exec, "direct rootfs exec rejected");
    require(strategy.primary == alr::runtime::GuestLoadMethod::MemfdExecveat, "primary is memfd-execveat");
    require(strategy.proot_baseline_available, "proot baseline available as fallback");
    require(strategy.ranked_methods.size() == 3, "three ranked methods");
    require(strategy.rejected_methods.size() == 2, "two rejected methods");

    require(contains(strategy.report, "ALR WX-SAFE EXEC STRATEGY: PASS"), "strategy report pass");
    require(contains(strategy.report, "ALR WX-SAFE ENTRYPOINT IS PACKAGED: PASS"), "entrypoint report pass");
    require(contains(strategy.report, "ALR WX-SAFE DIRECT ROOTFS EXEC REJECTED: PASS"), "direct exec rejected report");
    require(contains(strategy.report, "ALR WX-SAFE FILE-BACKED MMAP EXEC REJECTED: PASS"), "mmap exec rejected report");
    require(contains(strategy.report, "ALR WX-SAFE PROOT BASELINE AVAILABLE: PASS"), "proot baseline report");
    require(contains(strategy.report, "alr wx primary method=memfd-execveat"), "primary method report");
    require(contains(strategy.report, "alr wx fallback chain=memfd-execveat,anon-mmap-loader,proot-baseline"),
        "fallback chain report");
    // The writable rootfs binary appears only as a diagnostic / rejection reason,
    // never as a chosen exec target.
    require(contains(strategy.report, "alr wx rejected[0]=direct-rootfs-execve"), "direct rootfs rejection listed");
    require(contains(strategy.report, root.string()), "target host path recorded as diagnostic");

    // A rootfs-internal entrypoint must be refused: that would be a W^X violation.
    auto bad_config = config;
    bad_config.env["ALR_TRAMPOLINE_PATH"] = (root / "bin" / "hello").string();
    const auto bad_strategy = alr::runtime::build_wx_safe_exec_strategy(bad_config, resolution);
    require(!bad_strategy.planned, "rootfs-internal entrypoint refused");
    require(!bad_strategy.entrypoint_is_packaged, "rootfs-internal entrypoint not packaged");
    require(contains(bad_strategy.report, "ALR WX-SAFE ENTRYPOINT IS PACKAGED: FAIL"), "bad entrypoint report fail");

    // A sibling path that merely shares a prefix with rootfs must not be treated
    // as inside it.
    auto sibling_config = config;
    sibling_config.env["ALR_TRAMPOLINE_PATH"] = root.string() + "-libs/libtramp.so";
    const auto sibling_strategy = alr::runtime::build_wx_safe_exec_strategy(sibling_config, resolution);
    require(sibling_strategy.entrypoint_is_packaged, "prefix-sibling entrypoint is packaged");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime wx native test ok\n";
    return EXIT_SUCCESS;
}
