#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_path.hpp"

namespace alr::runtime {

enum class ExecutableKind {
    Missing,
    Elf,
    Shebang,
    Unsupported,
};

enum class ExecContinuationKind {
    Execve,
    Execvp,
    PosixSpawn,
};

struct ShebangInfo {
    std::string interpreter;
    std::string argument;
};

struct ExecutableResolution {
    PathTranslation translation;
    ExecutableKind kind = ExecutableKind::Missing;
    bool resolved = false;
    bool classified = false;
    ShebangInfo shebang;
    std::string strategy = "plan-only";
    std::string error;
    std::string report;
};

struct ExecContinuationPlan {
    ExecutableResolution resolution;
    ExecContinuationKind kind = ExecContinuationKind::Execve;
    bool planned = false;
    bool path_lookup = false;
    bool uses_packaged_trampoline = false;
    bool config_handoff = false;
    std::vector<std::string> argv;
    std::vector<std::string> env_overrides;
    std::string error;
    std::string report;
};

const char* executable_kind_name(ExecutableKind kind);

const char* exec_continuation_kind_name(ExecContinuationKind kind);

// Resolve and classify the first executable path that ALR would hand to a
// future guest loader. This is clean-room planning logic only; it deliberately
// does not exec the guest program.
ExecutableResolution resolve_guest_executable(
    const RuntimeConfig& config,
    std::string_view requested_program);

// Build the deterministic child-process continuation plan that future
// execve/execvp/posix_spawn hooks will use to re-enter the packaged ALR
// trampoline without directly executing writable rootfs files.
ExecContinuationPlan build_exec_continuation_plan(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const std::vector<std::string>& arguments = {},
    ExecContinuationKind kind = ExecContinuationKind::Execve);

}  // namespace alr::runtime
