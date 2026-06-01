#pragma once

#include <map>
#include <string>
#include <vector>

namespace alr {

struct RuntimeReportInput {
    std::string package_name;
    std::string native_library_dir;
    std::string app_files_dir;
    std::string app_cache_dir;
    std::string rootfs_name;
    std::string program;
    std::string optional_runtime_backend_name{};
    std::string optional_runtime_backend_path{};
};

struct OptionalRuntimeBackendProbe {
    std::string framework_status;
    std::string available_status;
    std::string source;
    std::string backend;
    std::string candidate_path;
    bool can_execute;
    std::string reason;
};

struct RuntimeReport {
    std::string loader_executable;
    std::string rootfs_dir;
    std::string text;
};

struct LoaderLaunchPlan {
    std::string executable;
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
};

enum class ExecutionBackendKind {
    PlanOnly,
    AndroidNativeTestCommand,
    Proot,
    GlibcLoader,
    AlrRuntime,
};

struct ExecutionBackend {
    ExecutionBackendKind kind;
    std::string name;
    bool can_execute;
    std::string reason;
};

RuntimeReport build_runtime_report(const RuntimeReportInput& input);
RuntimeReport build_runtime_report(const RuntimeReportInput& input, const ExecutionBackend& backend);
LoaderLaunchPlan build_loader_launch_plan(const RuntimeReportInput& input);
LoaderLaunchPlan build_proot_launch_plan(const RuntimeReportInput& input);
LoaderLaunchPlan build_alr_runtime_launch_plan(const RuntimeReportInput& input);
ExecutionBackend select_execution_backend(ExecutionBackendKind kind);
OptionalRuntimeBackendProbe probe_optional_runtime_backend(const RuntimeReportInput& input);

}  // namespace alr
