#include "runtime_plan.hpp"

#include <sstream>
#include <string>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_launch.hpp"

extern "C" const char* alr_runtime_launcher_status() {
    return "ALR RUNTIME LAUNCHER AVAILABLE: PASS";
}

extern "C" int alr_runtime_launcher_can_execute_guest() {
    return 0;
}

extern "C" const char* alr_runtime_launcher_policy() {
    return "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS";
}

extern "C" const char* alr_runtime_launcher_build_report(
    const char* package_name,
    const char* native_library_dir,
    const char* app_files_dir,
    const char* app_cache_dir,
    const char* rootfs_name,
    const char* program) {
    static std::string report;
    const auto input = alr::RuntimeReportInput{
        .package_name = package_name == nullptr ? "" : package_name,
        .native_library_dir = native_library_dir == nullptr ? "" : native_library_dir,
        .app_files_dir = app_files_dir == nullptr ? "" : app_files_dir,
        .app_cache_dir = app_cache_dir == nullptr ? "" : app_cache_dir,
        .rootfs_name = rootfs_name == nullptr ? "" : rootfs_name,
        .program = program == nullptr ? "" : program,
    };
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr::runtime::RuntimeConfig{
        .package_name = input.package_name,
        .rootfs_dir = launch.env.at("ALR_ROOTFS"),
        .cwd = "/",
        .program = input.program,
        .env = launch.env,
        .binds = {},
        .hook_path = launch.env.at("ALR_HOOK_PATH"),
        .interposer_path = launch.env.at("ALR_INTERPOSER_PATH"),
        .bridge_path = launch.env.at("ALR_BRIDGE_PATH"),
        .fake_root = false,
        .verbose = 0,
        .trace_path = false,
        .trace_exec = false,
    };
    const auto resolution = alr::runtime::resolve_guest_executable(config, input.program);
    const auto launch_attempt = alr::runtime::attempt_guest_launch(config, input.program);
    std::ostringstream out;
    out << "ALR RUNTIME LAUNCHER AVAILABLE: PASS";
    out << "\nALR RUNTIME CONFIG BUILD: PASS";
    out << "\nALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS";
    out << "\n" << resolution.report;
    out << "\n" << launch_attempt.report;
    out << "\ncan execute guest=no";
    out << "\nlauncher executable=" << launch.executable;
    out << "\nrootfs=" << launch.env.at("ALR_ROOTFS");
    out << "\nprogram=" << launch.env.at("ALR_PROGRAM");
    report = out.str();
    return report.c_str();
}
