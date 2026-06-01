#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../app/src/main/cpp/alr_runtime/alr_env.hpp"
#include "../app/src/main/cpp/alr_runtime/alr_path.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_eq(const std::string& actual, const std::string& expected, const char* message) {
    if (actual != expected) {
        throw std::runtime_error(std::string(message) + ": expected '" + expected + "' got '" + actual + "'");
    }
}

}  // namespace

int main() {
    using alr::runtime::build_guest_environment;
    using alr::runtime::GuestEnvironmentInput;
    using alr::runtime::normalize_guest_path;
    using alr::runtime::translate_rootfs_path;

    require_eq(normalize_guest_path("/usr//bin/./bash", "/"), "/usr/bin/bash", "normalizes absolute paths");
    require_eq(normalize_guest_path("../lib/ld-linux-aarch64.so.1", "/usr/bin"), "/usr/lib/ld-linux-aarch64.so.1", "resolves relative paths against cwd");
    require_eq(normalize_guest_path("../../../../etc/passwd", "/"), "/etc/passwd", "clamps parent traversal at guest root");
    require_eq(normalize_guest_path("", "/var/tmp"), "/var/tmp", "empty relative path resolves to cwd");

    const auto translated = translate_rootfs_path(
        "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64/",
        "/usr/bin",
        "../../etc/hosts");
    require_eq(translated.guest_path, "/etc/hosts", "translation records normalized guest path");
    require_eq(translated.host_path, "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64/etc/hosts", "translation maps under rootfs");
    require(!translated.escaped_rootfs, "lexical traversal must not escape rootfs");

    bool threw = false;
    try {
        (void)translate_rootfs_path("relative/rootfs", "/", "/bin/sh");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "relative rootfs paths are rejected");

    const auto env = build_guest_environment(GuestEnvironmentInput{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
    });
    require_eq(env.values.at("ALR_PACKAGE"), "dev.chanwoo.androlinux", "environment includes package");
    require_eq(env.values.at("ALR_ROOTFS"), "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64", "environment includes rootfs");
    require_eq(env.values.at("HOME"), "/root", "environment includes default HOME");
    require_eq(env.values.at("TMPDIR"), "/tmp", "environment includes default TMPDIR");

    std::cout << "alr runtime path/env native test ok\n";
    return EXIT_SUCCESS;
}
