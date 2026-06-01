#include <cstdlib>
#include <iostream>
#include <string>

#include "../app/src/main/cpp/runtime_plan.hpp"

int main() {
    const auto input = alr::RuntimeReportInput{
        .package_name = "dev.chanwoo.androlinux",
        .native_library_dir = "/data/app/pkg/lib/arm64",
        .app_files_dir = "/data/user/0/dev.chanwoo.androlinux/files",
        .app_cache_dir = "/data/user/0/dev.chanwoo.androlinux/cache",
        .rootfs_name = "debian-arm64",
        .program = "/bin/hello",
    };
    const auto backend = alr::select_execution_backend(alr::ExecutionBackendKind::PlanOnly);

    const bool ok =
        backend.kind == alr::ExecutionBackendKind::PlanOnly &&
        backend.name == "plan-only" &&
        backend.can_execute == false &&
        backend.reason.find("writable app-data rootfs binaries are not direct exec entrypoints") != std::string::npos;

    if (!ok) {
        std::cerr << backend.name << " " << backend.reason << "\n";
        return EXIT_FAILURE;
    }

    const auto report = alr::build_runtime_report(input, backend);
    if (report.text.find("execution backend: plan-only") == std::string::npos ||
        report.text.find("can execute: no") == std::string::npos ||
        report.text.find("ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS") == std::string::npos) {
        std::cerr << report.text << "\n";
        return EXIT_FAILURE;
    }

    const auto alr_backend = alr::select_execution_backend(alr::ExecutionBackendKind::AlrRuntime);
    if (alr_backend.kind != alr::ExecutionBackendKind::AlrRuntime ||
        alr_backend.name != "alr-runtime" ||
        alr_backend.can_execute ||
        alr_backend.reason.find("guest execution is not implemented yet") == std::string::npos) {
        std::cerr << alr_backend.name << " " << alr_backend.reason << "\n";
        return EXIT_FAILURE;
    }

    const auto absent_probe = alr::probe_optional_runtime_backend(input);
    if (absent_probe.framework_status != "PASS" || absent_probe.available_status != "SKIP" ||
        absent_probe.source != "none" || absent_probe.can_execute) {
        std::cerr << absent_probe.framework_status << " " << absent_probe.available_status << " "
                  << absent_probe.source << "\n";
        return EXIT_FAILURE;
    }

    auto external_input = input;
    external_input.optional_runtime_backend_name = "low-overhead-external";
    external_input.optional_runtime_backend_path = "/data/local/tmp/alr/proroot";
    const auto external_probe = alr::probe_optional_runtime_backend(external_input);
    if (external_probe.framework_status != "PASS" || external_probe.available_status != "PASS" ||
        external_probe.source != "external" || external_probe.backend != "low-overhead-external" ||
        external_probe.candidate_path != "/data/local/tmp/alr/proroot" || external_probe.can_execute) {
        std::cerr << external_probe.framework_status << " " << external_probe.available_status << " "
                  << external_probe.source << " " << external_probe.backend << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "native backend policy test ok\n";
    return EXIT_SUCCESS;
}
