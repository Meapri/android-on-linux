#pragma once

#include <string>
#include <string_view>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_elf.hpp"
#include "alr_runtime/alr_exec.hpp"

namespace alr::runtime {

struct TrampolineAttemptPolicy {
    std::string packaged_trampoline_path;
    bool allow_trampoline_exec = false;
};

struct TrampolineAttemptResult {
    bool available = false;
    bool config_handoff = false;
    bool policy_preflight = false;
    bool attempted = false;
    bool static_hello_executed = false;
    int exit_code = -1;
    std::string path;
    std::string stdout_text;
    std::string stderr_text;
    std::string error;
    std::string report;
};

TrampolineAttemptResult attempt_packaged_trampoline(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const ExecutableResolution& resolution,
    const ElfLoadPlan& elf_plan,
    TrampolineAttemptPolicy policy = {});

}  // namespace alr::runtime
