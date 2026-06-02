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

// === ADR-003 (B-3): execve/execveat child envp re-injection decision model ===
//
// B-1 rewrites the exec'd program path into the rootfs so the kernel loads the
// rootfs ELF under the inherited seccomp+SEIZE net. But the exec'd child also
// inherits the guest's envp ARRAY VERBATIM (execve(path, argv, envp) — x2 for
// execve, x3 for execveat). When that envp lacks LD_PRELOAD=<abs rootfs interpose
// .so> and ALR_ROOTFS=<rootfs>, the new image's in-process path interposer never
// loads: dpkg → maintainer scripts → sh → dpkg-deb run their file/path syscalls
// against the BARE Android filesystem and the unpack silently fails
// (apt-install: unpacked=false). The supervisor's ptrace path-rewrite is only a
// backstop; the fast-path interposer is what mediates the chain at libc level, so
// every exec MUST re-inject these two vars (and PRESERVE — not clobber — any
// LD_PRELOAD the guest itself set, per the project's R3 finding that LD_PRELOAD
// must be an ABSOLUTE ROOTFS path).
//
// This struct + function are the PURE, host-testable kernel of that decision:
// given the env strings already present in the child's envp (read by the
// supervisor from tracee memory) plus the run's rootfs_dir, they decide exactly
// which NEW env strings the supervisor must append to the rebuilt envp and which
// EXISTING entry (if any) the supervisor must drop+replace. The supervisor owns
// the tracee-memory plumbing (read the char** array at x2/x3, build an augmented
// array + string blob in a scratch region, point the reg at it); keeping the
// decision here lets host tests prove the var-set logic with fixture envps and no
// real ptrace.
struct ExecEnvpInjection {
    // The absolute rootfs interpose .so path that LD_PRELOAD must carry, and the
    // rootfs path ALR_ROOTFS must carry (both derived purely from rootfs_dir).
    std::string interpose_so;   // "<rootfs>/usr/lib/androlinux/libalr_interpose.so"
    std::string rootfs_value;   // "<rootfs>"

    // New "KEY=VALUE" strings the supervisor must APPEND to the rebuilt envp
    // (entries the child's envp did not already satisfy). Empty => nothing to add.
    std::vector<std::string> add_entries;

    // True iff an LD_PRELOAD entry already in the child's envp must be DROPPED from
    // the rebuilt array (because we are replacing it with a prepended value that
    // preserves the guest's own preloads — see ld_preload_value). When true, the
    // supervisor copies every guest envp entry EXCEPT the existing LD_PRELOAD, then
    // appends the entries in add_entries. When false, all guest entries are kept
    // verbatim and add_entries are appended.
    bool replace_ld_preload = false;

    // The full LD_PRELOAD value the rebuilt envp should carry, with the interpose
    // .so guaranteed present. If the guest had its own LD_PRELOAD that did NOT
    // already contain the interpose .so, this is "<interpose_so>:<guest value>"
    // (interpose FIRST so its wrappers win), and replace_ld_preload is true. If the
    // guest had no LD_PRELOAD, this is just "<interpose_so>" and it appears as a
    // fresh add_entry (replace_ld_preload false). Empty iff LD_PRELOAD already
    // satisfied (interpose .so already in the guest value).
    std::string ld_preload_value;

    // True iff ANY mutation is required (add_entries non-empty OR replace_ld_preload).
    // When false the child's envp already re-enters ALR and the supervisor leaves
    // x2/x3 untouched (idempotency across the dpkg→sh→dpkg-deb chain: a child whose
    // parent we already injected inherits a satisfied envp and is a no-op).
    bool should_inject = false;

    // Diagnostic reason for the one-line supervisor log / host-test assertions:
    //   "inject-both"      both LD_PRELOAD and ALR_ROOTFS were missing/unsatisfied
    //   "inject-ld"        only LD_PRELOAD needed (ALR_ROOTFS already present)
    //   "inject-rootfs"    only ALR_ROOTFS needed (LD_PRELOAD already satisfied)
    //   "prepend-ld"       guest had its own LD_PRELOAD; interpose prepended to it
    //   "already"          both already satisfied — no-op (idempotent re-exec)
    std::string reason;
};

// Decide what (if anything) must be injected into an exec'd child's envp so the
// new image re-enters ALR's interpose mediation. Pure in (rootfs_dir, env_entries);
// performs NO filesystem access. `env_entries` is the child's current envp as a
// list of "KEY=VALUE" (and bare "KEY") strings exactly as the supervisor read them
// from tracee memory. Rules (ADR-003 §3 B-3):
//   - LD_PRELOAD absent            -> add "LD_PRELOAD=<interpose_so>"
//   - LD_PRELOAD present, interpose
//     .so NOT in its colon list    -> replace with "<interpose_so>:<old value>"
//                                     (replace_ld_preload=true; preserves guest preloads)
//   - LD_PRELOAD present, interpose
//     .so already in it            -> leave (idempotent)
//   - ALR_ROOTFS absent            -> add "ALR_ROOTFS=<rootfs>"
//   - ALR_ROOTFS present           -> leave (NOT re-validated; the guest/parent set
//                                     it, idempotency across the exec chain)
// When rootfs_dir is empty the interposer self-disables anyway, so this returns a
// no-op (should_inject=false, reason="already").
//
// When `fakeroot` is true (dpkg/apt drain under the ALR_FAKEROOT gate) the desired
// LD_PRELOAD chain becomes "<fakeroot_so>:<interpose_so>" (fakeroot FIRST, interpose
// KEPT), so an exec'd child (zstd/sh/dpkg-deb) inherits the fakeroot credential
// wrappers too. The chain is de-duped and the guest's own preloads are preserved,
// matching chain_ld_preload() in tools/aptdrain_env_model.py. Default false keeps
// every existing caller byte-identical.
ExecEnvpInjection decide_exec_envp_injection(
    std::string_view rootfs_dir,
    const std::vector<std::string>& env_entries,
    bool fakeroot = false);

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
