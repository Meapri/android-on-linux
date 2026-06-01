#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_interposer.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("alr-interposer-smoke-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "usr" / "bin");
    {
        std::ofstream os(root / "usr" / "bin" / "hello");
        os << "interposer hello\n";
    }

    const auto config = alr::runtime::InterposerConfig{
        .rootfs_dir = root.string(),
        .cwd = "/usr",
    };

    const auto relative = alr::runtime::run_interposer_path_smoke(config, "bin/hello");
    require(relative.translation.guest_path == "/usr/bin/hello", "relative guest path translated");
    require(relative.translation.host_path == (root / "usr" / "bin" / "hello").string(), "relative host path translated");
    require(relative.stated, "relative stat passed");
    require(relative.opened, "relative open passed");
    require(relative.first_bytes.find("interposer hello") != std::string::npos, "relative read captured");
    require(relative.report.find("ALR INTERPOSER LOAD: PASS") != std::string::npos, "interposer load report");
    require(relative.report.find("ALR INTERPOSER STAT PATH: PASS") != std::string::npos, "interposer stat report");
    require(relative.report.find("ALR INTERPOSER OPEN PATH: PASS") != std::string::npos, "interposer open report");

    const auto absolute = alr::runtime::run_interposer_path_smoke(config, "/usr/bin/hello");
    require(absolute.translation.guest_path == "/usr/bin/hello", "absolute guest path translated");
    require(absolute.opened, "absolute open passed");
    require(absolute.stated, "absolute stat passed");

    const auto missing = alr::runtime::run_interposer_path_smoke(config, "/missing");
    require(missing.translation.host_path == (root / "missing").string(), "missing host path translated");
    require(!missing.opened, "missing open failed");
    require(!missing.stated, "missing stat failed");
    require(missing.open_errno != 0, "missing open errno captured");
    require(missing.stat_errno != 0, "missing stat errno captured");
    require(missing.report.find("ALR INTERPOSER OPEN PATH: FAIL") != std::string::npos, "missing open report");
    require(missing.report.find("ALR INTERPOSER STAT PATH: FAIL") != std::string::npos, "missing stat report");

    // procfs-aware interposition: /proc paths are served from the synthesized
    // guest view with no host /proc access; everything else maps to a rootfs
    // host path.
    const auto rt_config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {{"ALR_TRAMPOLINE_PATH", "/data/app/dev.chanwoo.androlinux/lib/arm64/libtramp.so"}},
    };

    const auto self_exe = alr::runtime::resolve_interposed_access(rt_config, "/bin/hello", "/proc/self/exe");
    require(self_exe.kind == alr::runtime::InterposedKind::ProcSelfExe, "self exe kind");
    require(self_exe.virtualized, "self exe virtualized");
    require(self_exe.synthetic_link_target == "/bin/hello", "self exe target is guest path");
    require(self_exe.host_path.empty(), "self exe performs no host op");
    require(self_exe.report.find("ALR INTERPOSE KIND: proc-self-exe") != std::string::npos, "self exe report kind");

    const auto pid_exe = alr::runtime::resolve_interposed_access(rt_config, "/bin/hello", "/proc/1234/exe");
    require(pid_exe.kind == alr::runtime::InterposedKind::ProcSelfExe, "pid exe form matches");

    const auto status = alr::runtime::resolve_interposed_access(rt_config, "/bin/hello", "/proc/self/status");
    require(status.kind == alr::runtime::InterposedKind::ProcSelfStatus, "status kind");
    require(status.synthetic_content.find("Name:\thello") != std::string::npos, "status name synthesized");
    require(status.host_path.empty(), "status performs no host op");

    const auto mounts = alr::runtime::resolve_interposed_access(rt_config, "/bin/hello", "/proc/mounts");
    require(mounts.kind == alr::runtime::InterposedKind::ProcMounts, "mounts kind");
    require(mounts.synthetic_content.find("proc /proc proc") != std::string::npos, "mounts proc line synthesized");
    require(mounts.synthetic_content.find(root.string()) == std::string::npos, "mounts hide host rootfs path");

    const auto etc = alr::runtime::resolve_interposed_access(rt_config, "/bin/hello", "/etc/hostname");
    require(etc.kind == alr::runtime::InterposedKind::RootfsPath, "etc kind is rootfs path");
    require(!etc.virtualized, "etc not virtualized");
    require(etc.host_path == (root / "etc" / "hostname").string(), "etc host path translated");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime interposer native test ok\n";
    return EXIT_SUCCESS;
}
