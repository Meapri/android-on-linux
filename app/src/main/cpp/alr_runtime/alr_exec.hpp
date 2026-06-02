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

// === ADR-003 (B-1): execve-argument path mediation decision model ===
//
// When a guest issues execve(path, argv, envp) / execveat under the loader's
// RET_TRACE filter, the supervisor must rewrite the *program path* into the
// rootfs so the kernel loads the rootfs ELF (which keeps the new image under the
// same seccomp filter + SEIZE trace — ADR-003 §2). Unlike the 9 path syscalls,
// execve carries the pathname in x0 (NOT x1=argv), so this is a separate branch.
//
// This struct + function are the PURE, host-testable kernel of that branch: they
// take the guest path string read from the tracee and decide whether/what to
// rewrite, applying the exact same exclusions the path-family rewrite uses
// (kernel virtual dirs are left native; an already-rootfs path is idempotent).
// The supervisor consumes ExecPathMediation to drive the ptrace pread/pwrite of
// x0; the register plumbing (which reg holds the path, scratch placement) stays
// in runtime_report.cpp. Keeping the decision here lets host tests prove the
// classification with fixture strings and no real ptrace.
//
// Crucially this NEVER touches argv (x1) or envp (x2): argv[0] may stay a guest
// virtual name while only x0 (the kernel's load path) is rewritten to the host
// ELF — see ADR-003 §3 "argv[0] 불변".
struct ExecPathMediation {
    // The guest program path as read from the tracee (unchanged input echo).
    std::string guest_path;
    // The rootfs host path the supervisor should write into x0. Empty when
    // should_rewrite is false.
    std::string host_path;
    // True iff the supervisor should pwrite host_path into a scratch buffer and
    // point x0 at it. False means "leave x0 exactly as the guest set it".
    bool should_rewrite = false;
    // Diagnostic reason for the decision (for the supervisor's one-line log and
    // host-test assertions). One of: "rewrite", "relative", "sysdir",
    // "already-host", "empty".
    std::string reason;
};

// Decide how (if at all) to mediate an execve/execveat program path. Pure in
// (rootfs_dir, guest_path); performs NO filesystem access (mirrors the
// supervisor hot path, which must not stat). Rules (ADR-003 §3):
//   - empty guest_path                      -> no rewrite (reason="empty")
//   - relative path (no leading '/')        -> no rewrite (reason="relative";
//                                              resolved against guest cwd, native)
//   - /proc, /sys, /dev (and subpaths)      -> no rewrite (reason="sysdir";
//                                              kernel virtual fs incl /proc/self/exe)
//   - already under rootfs_dir              -> no rewrite (reason="already-host";
//                                              idempotency guard)
//   - any other absolute path               -> rewrite to <rootfs_dir><guest_path>
//                                              (reason="rewrite")
ExecPathMediation decide_exec_path_mediation(
    std::string_view rootfs_dir,
    std::string_view guest_path);

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
