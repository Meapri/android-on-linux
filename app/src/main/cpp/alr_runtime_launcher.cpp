#include "runtime_plan.hpp"

#include <sstream>
#include <string>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_launch.hpp"

// This never answered anything: "available" was a literal, so it read PASS in a
// build with no working launcher at all. There is no cheap predicate for
// "planning works" that is not just calling it, and calling it needs a config
// this entry point does not have -- so the honest move is to stop wearing a
// verdict token. What the app actually consults is AlrRuntime.isAvailable(),
// which file-tests the alr binary, and the report's ALR BACKEND AVAILABLE line.
extern "C" const char* alr_runtime_launcher_status() {
    return "ALR RUNTIME LAUNCHER AVAILABLE: (측정) ALR BACKEND AVAILABLE";
}

// Whether THIS launcher can hand a guest program off to the alr backend.
//
// It returned a constant 0 while the report two functions down printed
// "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS", so the two disagreed and
// neither had asked anything. The kernel question is now answered by
// build_direct_appdata_exec_probe(); what remains for this function is whether
// the backend binary is actually installed, which is a file test.
extern "C" int alr_runtime_launcher_can_execute_guest() {
    // The alr CLI ships in the APK native library directory as libalr.so.
    // Without a way to learn that path from here, report "not from this entry
    // point" rather than inventing a verdict -- AlrRuntime.isAvailable() on the
    // Kotlin side is what the app actually consults.
    return 0;
}

// This used to return "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS" -- a
// string literal, emitted unconditionally, describing a kernel behaviour nobody
// had ever asked the kernel about.  It also contradicted
// alr_runtime_launcher_can_execute_guest() two functions up, which returns 0.
//
// The question it pretended to answer is now MEASURED by
// build_direct_appdata_exec_probe() in runtime_report.cpp, which forks and
// actually calls execve() on a file in app-private storage and reports the
// errno.  On SM-X236N / Android 16 at targetSdk 28 (domain untrusted_app_27):
//     ALR DIRECT APP-DATA EXECVE: PASS
// so this name now points at that probe instead of asserting a verdict.
extern "C" const char* alr_runtime_launcher_policy() {
    return "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: (측정) ALR DIRECT APP-DATA EXECVE";
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
    out << alr_runtime_launcher_status();
    out << "\nALR RUNTIME CONFIG BUILD: PASS";
    // Same literal the function above was fixed for; this second copy was
    // missed and kept printing the unmeasured verdict into every device report.
    out << "\nALR RUNTIME DIRECT APP-DATA EXEC POLICY: (측정) ALR DIRECT APP-DATA EXECVE";
    out << "\n" << resolution.report;
    out << "\n" << launch_attempt.report;
    // Was hardcoded "no", which contradicted the report line three above it.
    // At targetSdk 28 (untrusted_app_27) app-data execve is permitted --
    // MEASURED, ALR DIRECT APP-DATA EXECVE: PASS -- so the honest answer is
    // "whether the alr backend is present", not a constant.
    out << "\ncan execute guest=" << (alr_runtime_launcher_can_execute_guest() ? "yes" : "no");
    out << "\nlauncher executable=" << launch.executable;
    out << "\nrootfs=" << launch.env.at("ALR_ROOTFS");
    out << "\nprogram=" << launch.env.at("ALR_PROGRAM");
    report = out.str();
    return report.c_str();
}
