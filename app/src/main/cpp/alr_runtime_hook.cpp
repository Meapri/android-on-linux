#include "alr_runtime/alr_hook.hpp"

#include <exception>
#include <string>

extern "C" const char* alr_runtime_hook_status() {
    return "ALR HOOK LOAD: PASS";
}

extern "C" const char* alr_runtime_hook_smoke_report(
    const char* rootfs_dir,
    const char* cwd,
    const char* path) {
    static std::string report;
    try {
        const auto result = alr::runtime::run_path_hook_smoke(
            rootfs_dir == nullptr ? "" : rootfs_dir,
            cwd == nullptr ? "/" : cwd,
            path == nullptr ? "" : path);
        report = result.report;
    } catch (const std::exception& exc) {
        report = std::string("ALR HOOK LOAD: PASS\nALR HOOK SMOKE: FAIL\nALR HOOK ERROR: ") + exc.what();
    }
    return report.c_str();
}
