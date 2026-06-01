#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#include "../app/src/main/cpp/alr_runtime/alr_procfs.hpp"

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
        ("alr-procfs-plan-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bin");
    write_bytes(root / "bin" / "hello", std::string("\x7f""ELF", 4) + "fake-static-elf");

    // An android-like bind host path that must never leak into the guest view.
    const std::string android_bind_host = "/data/data/dev.chanwoo.androlinux/files/share";

    auto base_config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {
            {"ALR_TRAMPOLINE_PATH", "/native/libalr_runtime_trampoline.so"},
            {"PATH", "/usr/bin:/bin"},
        },
        .binds = {{"/mnt/share", android_bind_host}},
    };

    const alr::runtime::ProcfsHostFacts host_facts{
        .self_exe_truth = "/native/libalr_runtime_trampoline.so",
        .has_identity = true,
        .uid = 10234,
        .gid = 10234,
    };

    // Non-fakeroot plan: guest sees the real app identity but the guest-namespace
    // executable path, not the trampoline host path.
    const auto plan = alr::runtime::build_procfs_virtualization_plan(base_config, "/bin/hello", host_facts);
    require(plan.planned, "non-fakeroot procfs plan ready");
    require(plan.self_exe.guest_view == "/bin/hello", "self exe guest view is guest path");
    require(plan.self_exe.host_truth == "/native/libalr_runtime_trampoline.so", "self exe host truth is trampoline");
    require(plan.self_exe.requires_virtualization, "self exe requires virtualization");
    require(plan.self_status.name == "hello", "self status comm is exec basename");
    require(plan.self_status.uid == 10234, "non-fakeroot uid preserved");
    require(plan.self_status.gid == 10234, "non-fakeroot gid preserved");
    require(!plan.self_status.fakeroot, "non-fakeroot flag false");
    require(plan.mounts.has_guest_root, "mounts has guest root");
    require(plan.mounts.has_proc, "mounts has proc");
    require(plan.mounts.leaked_host_tokens.empty(), "mounts do not leak host tokens");

    require(contains(plan.report, "ALR PROCFS PLAN: PASS"), "report top pass");
    require(contains(plan.report, "ALR PROCFS SELF EXE PLAN: PASS"), "report self exe pass");
    require(contains(plan.report, "ALR PROCFS SELF STATUS FAKEROOT PLAN: PASS"), "report self status pass");
    require(contains(plan.report, "ALR PROCFS MOUNTS PLAN: PASS"), "report mounts pass");
    require(contains(plan.report, "ALR PROCFS HOST LEAK GUARD: PASS"), "report leak guard pass");
    require(contains(plan.report, "alr procfs self exe interpose syscalls=readlink,readlinkat,openat"),
        "report interpose syscalls");

    // The guest-visible mount text must present the rootfs and pseudo-filesystems
    // without exposing the real Android backing store or bind host path.
    const auto mounts_text = alr::runtime::render_guest_mounts(plan.mounts);
    require(contains(mounts_text, "alr_rootfs / overlay"), "guest mounts root line");
    require(contains(mounts_text, "proc /proc proc"), "guest mounts proc line");
    require(contains(mounts_text, "alr_bind /mnt/share none"), "guest mounts bind sanitized");
    require(!contains(mounts_text, android_bind_host), "guest mounts hide bind host path");
    require(!contains(mounts_text, root.string()), "guest mounts hide rootfs host path");
    require(!contains(mounts_text, "/data/"), "guest mounts hide android data path");

    // Fakeroot plan: guest identity collapses to root to match the PRoot baseline.
    auto fakeroot_config = base_config;
    fakeroot_config.fake_root = true;
    const auto fakeroot_plan =
        alr::runtime::build_procfs_virtualization_plan(fakeroot_config, "/bin/hello", host_facts);
    require(fakeroot_plan.planned, "fakeroot procfs plan ready");
    require(fakeroot_plan.self_status.fakeroot, "fakeroot flag true");
    require(fakeroot_plan.self_status.uid == 0, "fakeroot uid mapped to root");
    require(fakeroot_plan.self_status.gid == 0, "fakeroot gid mapped to root");
    require(contains(fakeroot_plan.report, "alr procfs self status uid=0"), "fakeroot uid report");
    require(contains(fakeroot_plan.report, "alr procfs self status fakeroot=true"), "fakeroot flag report");

    // Without a known host truth the self-exe virtualization gap is undefined, so
    // the plan must refuse rather than silently claim success.
    auto no_truth_config = base_config;
    no_truth_config.env.erase("ALR_TRAMPOLINE_PATH");
    const alr::runtime::ProcfsHostFacts no_truth_facts{
        .self_exe_truth = "",
        .has_identity = true,
        .uid = 10234,
        .gid = 10234,
    };
    const auto no_truth_plan =
        alr::runtime::build_procfs_virtualization_plan(no_truth_config, "/bin/hello", no_truth_facts);
    require(!no_truth_plan.planned, "missing host truth refuses plan");
    require(contains(no_truth_plan.report, "ALR PROCFS SELF EXE PLAN: FAIL"), "missing host truth self exe fail");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime procfs native test ok\n";
    return EXIT_SUCCESS;
}
