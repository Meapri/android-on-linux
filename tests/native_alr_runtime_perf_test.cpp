#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../app/src/main/cpp/alr_runtime/alr_perf.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = "/data/data/dev.chanwoo.androlinux/files/rootfs/debian-arm64",
        .cwd = "/",
        .program = "/bin/hello",
    };

    const long iterations = 50000;
    const auto comparison = alr::runtime::run_perf_comparison(config, iterations);

    require(comparison.ran, "perf harness ran");
    require(comparison.alr_path_xlate.measured, "alr path xlate measured");
    require(comparison.syscall_roundtrip.measured, "syscall roundtrip measured");
    require(comparison.alr_path_xlate.iterations == iterations, "alr iterations recorded");
    require(comparison.syscall_roundtrip.iterations == iterations, "syscall iterations recorded");
    require(comparison.alr_path_xlate.ns_per_op >= 0.0, "alr ns/op non-negative");
    require(comparison.syscall_roundtrip.ns_per_op >= 0.0, "syscall ns/op non-negative");
    require(comparison.xlate_cost_in_syscall_units >= 0.0, "ratio non-negative");

    // The host harness must never fabricate a PRoot ptrace number or a final
    // comparison: both stay pending until on-device data exists.
    require(comparison.proot_device_baseline_pending, "proot baseline pending on host");
    require(comparison.comparison_device_pending, "comparison pending on host");

    require(contains(comparison.report, "ALR PERF HARNESS: PASS"), "report harness pass");
    require(contains(comparison.report, "ALR PERF ALR PATH XLATE MEASURED: PASS"), "report alr measured");
    require(contains(comparison.report, "ALR PERF SYSCALL ROUNDTRIP MEASURED: PASS"), "report syscall measured");
    require(contains(comparison.report, "ALR PERF PROOT PTRACE DEVICE BASELINE: PENDING_DEVICE"),
        "report proot pending");
    require(contains(comparison.report, "ALR PERF PROOT VS ALR DEVICE COMPARISON: PENDING_DEVICE"),
        "report comparison pending");
    require(contains(comparison.report, "alr perf xlate cost in syscall units="), "report ratio present");
    require(contains(comparison.report, "ALR's win is avoiding PRoot's per-syscall ptrace stops"),
        "report honest interpretation present");
    require(contains(comparison.report, "optimization target"), "report optimization target present");

    std::cout << "alr runtime perf native test ok\n";
    return EXIT_SUCCESS;
}
