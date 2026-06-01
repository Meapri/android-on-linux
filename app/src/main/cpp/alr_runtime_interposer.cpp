#include "alr_runtime/alr_interposer.hpp"

#include <exception>
#include <string>

extern "C" const char* alr_runtime_interposer_status() {
    return "ALR INTERPOSER LOAD: PASS";
}

extern "C" const char* alr_runtime_interposer_smoke_report(
    const char* rootfs_dir,
    const char* cwd,
    const char* path) {
    static std::string report;
    try {
        const auto result = alr::runtime::run_interposer_path_smoke(
            alr::runtime::InterposerConfig{
                .rootfs_dir = rootfs_dir == nullptr ? "" : rootfs_dir,
                .cwd = cwd == nullptr ? "/" : cwd,
            },
            path == nullptr ? "" : path);
        report = result.report;
    } catch (const std::exception& exc) {
        report = std::string("ALR INTERPOSER LOAD: PASS\nALR INTERPOSER SMOKE: FAIL\nALR INTERPOSER ERROR: ") + exc.what();
    }
    return report.c_str();
}
