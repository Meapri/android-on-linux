#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_trampoline.hpp"

namespace alr::runtime {

struct LaunchAttemptPolicy {
    bool allow_direct_host_exec = false;
    TrampolineAttemptPolicy trampoline;
};

struct LaunchAttemptResult {
    ExecutableResolution resolution;
    bool attempted = false;
    bool policy_blocked = false;
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    std::string error;
    TrampolineAttemptResult trampoline;
    ExecContinuationPlan continuation;
    std::string report;
};

// First controlled ALR launch attempt. By default this performs only a policy
// preflight and refuses direct rootfs exec; host-native tests can explicitly
// enable direct host exec to prove child process/output capture plumbing.
LaunchAttemptResult attempt_guest_launch(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const std::vector<std::string>& arguments = {},
    LaunchAttemptPolicy policy = {});

}  // namespace alr::runtime
