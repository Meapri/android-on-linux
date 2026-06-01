#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../app/src/main/cpp/alr_runtime/alr_config.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
        .cwd = "/usr/../usr/bin",
        .program = "/bin/hello",
        .env = {
            {"ALR_BACKEND", "alr-runtime"},
            {"ALR_PROGRAM", "/bin/hello"},
            {"ALR_ROOTFS", "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64"},
            {"PATH", "/usr/bin:/bin"},
        },
        .binds = {
            alr::runtime::BindMount{.guest_path = "/sdcard", .host_path = "/storage/emulated/0"},
            alr::runtime::BindMount{.guest_path = "/tmp", .host_path = "/data/user/0/dev.chanwoo.androlinux/cache"},
        },
        .hook_path = "/data/app/pkg/lib/arm64/libalr_runtime_hook.so",
        .interposer_path = "/data/app/pkg/lib/arm64/libalr_runtime_interposer.so",
        .bridge_path = "/data/app/pkg/lib/arm64/libalr_runtime_bridge.so",
        .fake_root = true,
        .verbose = 2,
        .trace_path = true,
        .trace_exec = false,
    };

    const auto serialized = alr::runtime::serialize_runtime_config(config);
    require(serialized.text.find("alr-config-v1\n") == 0, "config header");
    require(serialized.text.find("field\tprogram\t/bin/hello\n") != std::string::npos, "program field");
    require(serialized.text.find("field\tcwd\t/usr/bin\n") != std::string::npos, "normalized cwd");
    require(serialized.text.find("env\tPATH\t/usr/bin:/bin\n") != std::string::npos, "env record");
    require(serialized.text.find("bind\t/sdcard\t/storage/emulated/0\n") != std::string::npos, "bind record");
    require(serialized.checksum_hex == alr::runtime::runtime_config_checksum_hex(serialized.text), "checksum matches");

    const auto parsed = alr::runtime::parse_runtime_config(serialized.text);
    require(parsed.package_name == config.package_name, "package round trip");
    require(parsed.rootfs_dir == config.rootfs_dir, "rootfs round trip");
    require(parsed.cwd == "/usr/bin", "cwd round trip");
    require(parsed.program == config.program, "program round trip");
    require(parsed.env.at("PATH") == "/usr/bin:/bin", "env round trip");
    require(parsed.binds.size() == 2, "bind count");
    require(parsed.hook_path == config.hook_path, "hook path round trip");
    require(parsed.interposer_path == config.interposer_path, "interposer path round trip");
    require(parsed.fake_root, "fake root flag");
    require(parsed.verbose == 2, "verbose flag");
    require(parsed.trace_path, "trace path flag");
    require(!parsed.trace_exec, "trace exec flag");

    bool rejected = false;
    try {
        (void)alr::runtime::serialize_runtime_config(alr::runtime::RuntimeConfig{
            .package_name = "dev.chanwoo.androlinux",
            .rootfs_dir = "relative/rootfs",
            .cwd = "/",
            .program = "/bin/hello",
            .env = {},
            .binds = {},
            .hook_path = "",
            .interposer_path = "",
            .bridge_path = "",
            .fake_root = false,
            .verbose = 0,
            .trace_path = false,
            .trace_exec = false,
        });
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "relative rootfs rejected");

    rejected = false;
    try {
        (void)alr::runtime::parse_runtime_config("bad-format\n");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "bad format rejected");

    std::cout << "alr runtime config native test ok\n";
    return EXIT_SUCCESS;
}
