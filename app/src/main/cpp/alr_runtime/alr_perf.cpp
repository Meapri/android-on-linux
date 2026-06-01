#include "alr_runtime/alr_perf.hpp"

#include <unistd.h>

#include <chrono>
#include <sstream>
#include <string>

#include "alr_runtime/alr_path.hpp"

namespace alr::runtime {
namespace {

long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Measure the in-process ALR guest->host path translation. The loop varies the
// guest path each iteration and folds the result into a sink so the optimizer
// cannot elide the work.
PerfSample measure_alr_path_xlate(const RuntimeConfig& config, long iterations) {
    PerfSample sample;
    sample.name = "alr-path-xlate";
    sample.iterations = iterations;
    volatile std::size_t sink = 0;
    const long long start = now_ns();
    for (long i = 0; i < iterations; ++i) {
        std::string guest = "/usr/bin/sub";
        guest += std::to_string(i & 0xff);
        guest += "/tool";
        const auto translation = translate_rootfs_path(config.rootfs_dir, config.cwd, guest);
        sink = sink + translation.host_path.size() + translation.guest_path.size();
    }
    const long long end = now_ns();
    (void)sink;
    sample.total_ns = end - start;
    sample.ns_per_op = iterations > 0 ? static_cast<double>(sample.total_ns) / static_cast<double>(iterations) : 0.0;
    sample.measured = iterations > 0 && sample.total_ns >= 0;
    return sample;
}

// Measure a real syscall round-trip. getppid is never cached by libc, so each
// call is a genuine user/kernel transition.
PerfSample measure_syscall_roundtrip(long iterations) {
    PerfSample sample;
    sample.name = "syscall-getppid";
    sample.iterations = iterations;
    volatile long sink = 0;
    const long long start = now_ns();
    for (long i = 0; i < iterations; ++i) {
        sink = sink + ::getppid();
    }
    const long long end = now_ns();
    (void)sink;
    sample.total_ns = end - start;
    sample.ns_per_op = iterations > 0 ? static_cast<double>(sample.total_ns) / static_cast<double>(iterations) : 0.0;
    sample.measured = iterations > 0 && sample.total_ns >= 0;
    return sample;
}

std::string status_line(const char* label, bool measured) {
    return std::string(label) + ": " + (measured ? "PASS" : "FAIL");
}

std::string format_double(double value) {
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << value;
    return out.str();
}

std::string build_report(const PerfComparison& comparison) {
    std::ostringstream out;
    out << "ALR PERF HARNESS: " << (comparison.ran ? "PASS" : "FAIL");
    out << "\n" << status_line("ALR PERF ALR PATH XLATE MEASURED", comparison.alr_path_xlate.measured);
    out << "\n" << status_line("ALR PERF SYSCALL ROUNDTRIP MEASURED", comparison.syscall_roundtrip.measured);
    out << "\nALR PERF PROOT PTRACE DEVICE BASELINE: "
        << (comparison.proot_device_baseline_pending ? "PENDING_DEVICE" : "PASS");
    out << "\nALR PERF PROOT VS ALR DEVICE COMPARISON: "
        << (comparison.comparison_device_pending ? "PENDING_DEVICE" : "PASS");
    out << "\nalr perf iterations=" << comparison.alr_path_xlate.iterations;
    out << "\nalr perf alr xlate total ns=" << comparison.alr_path_xlate.total_ns;
    out << "\nalr perf alr xlate ns/op=" << format_double(comparison.alr_path_xlate.ns_per_op);
    out << "\nalr perf syscall getppid total ns=" << comparison.syscall_roundtrip.total_ns;
    out << "\nalr perf syscall getppid ns/op=" << format_double(comparison.syscall_roundtrip.ns_per_op);
    out << "\nalr perf xlate cost in syscall units=" << format_double(comparison.xlate_cost_in_syscall_units);
    out << "\nalr perf interpretation=both PRoot and ALR pay the path-translation cost; ALR's win is avoiding PRoot's per-syscall ptrace stops, so the real delta is PENDING_DEVICE";
    out << "\nalr perf optimization target=reduce ALR path-translation allocations on the hot path";
    out << "\nalr perf note=PRoot ptrace per-syscall overhead is device/kernel specific and must be measured on-device";
    return out.str();
}

}  // namespace

PerfComparison run_perf_comparison(const RuntimeConfig& config, long iterations) {
    PerfComparison comparison;
    comparison.alr_path_xlate = measure_alr_path_xlate(config, iterations);
    comparison.syscall_roundtrip = measure_syscall_roundtrip(iterations);
    comparison.xlate_cost_in_syscall_units = comparison.syscall_roundtrip.ns_per_op > 0.0
        ? comparison.alr_path_xlate.ns_per_op / comparison.syscall_roundtrip.ns_per_op
        : 0.0;
    comparison.proot_device_baseline_pending = true;
    comparison.comparison_device_pending = true;
    comparison.ran = comparison.alr_path_xlate.measured && comparison.syscall_roundtrip.measured;
    comparison.report = build_report(comparison);
    return comparison;
}

}  // namespace alr::runtime
