#pragma once

#include <string>

#include "alr_runtime/alr_config.hpp"

namespace alr::runtime {

// Performance comparison scaffolding for the PRoot baseline vs the ALR low-
// overhead path. The honest split:
//   * ALR's in-process path translation runs identically on host and device, so
//     its per-op cost is measured directly here. BOTH PRoot and ALR pay this
//     same translation cost, so it is not where ALR wins.
//   * A real syscall round-trip (getppid) is measured as the device-portable
//     unit of the ptrace stop PRoot adds per intercepted guest syscall and that
//     ALR's in-process interposer avoids.
//   * The measured translation cost expressed in syscall-round-trip units shows
//     whether ALR's hot path is allocation-bound and worth optimizing; it is NOT
//     a PRoot comparison.
//   * PRoot's actual ptrace per-syscall overhead is device/kernel specific and
//     cannot be measured on this host, so it stays PENDING_DEVICE. The real
//     PRoot-vs-ALR comparison is filled in only from on-device data.

struct PerfSample {
    std::string name;
    long iterations = 0;
    long long total_ns = 0;
    double ns_per_op = 0.0;
    bool measured = false;
};

struct PerfComparison {
    PerfSample alr_path_xlate;     // in-process ALR guest->host path translation
    PerfSample syscall_roundtrip;  // getppid() real syscall round-trip
    double xlate_cost_in_syscall_units = 0.0;  // ALR ns/op divided by syscall ns/op
    bool proot_device_baseline_pending = true;  // ptrace cost is unmeasurable on host
    bool comparison_device_pending = true;      // need on-device PRoot numbers
    bool ran = false;
    std::string report;
};

// Run the host-measurable half of the comparison over `iterations` operations.
// Clean measurement only; it does not launch PRoot or the guest.
PerfComparison run_perf_comparison(const RuntimeConfig& config, long iterations);

}  // namespace alr::runtime
