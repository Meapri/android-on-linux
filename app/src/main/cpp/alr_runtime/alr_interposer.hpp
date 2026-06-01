#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_path.hpp"

namespace alr::runtime {

struct InterposerConfig {
    std::string rootfs_dir;
    std::string cwd;
};

// How an interposed guest path access is served. Procfs paths are answered from
// the synthesized guest view with no host /proc access; everything else maps to
// a translated rootfs host path.
enum class InterposedKind {
    RootfsPath,
    ProcSelfExe,
    ProcSelfStatus,
    ProcMounts,
};

struct InterposedResolution {
    InterposedKind kind = InterposedKind::RootfsPath;
    bool virtualized = false;            // served from the procfs plan, no host op
    std::string guest_path;              // normalized guest path that was requested
    std::string host_path;               // translated rootfs host path (RootfsPath only)
    std::string synthetic_link_target;   // guest /proc/self/exe target (ProcSelfExe)
    std::string synthetic_content;       // synthesized file body (status/mounts)
    std::string report;
};

const char* interposed_kind_name(InterposedKind kind);

// Decide how the in-process interposer answers a guest path access. For
// /proc/self/exe, /proc/<pid>/exe, /proc/self/status, and /proc/mounts (and the
// /proc/self and /proc/<pid> mounts aliases) it returns the synthesized guest
// view derived from the procfs virtualization plan, never touching the host
// /proc. All other paths fall back to rootfs translation. Clean-room: it does
// not install real LD_PRELOAD hooks yet.
InterposedResolution resolve_interposed_access(
    const RuntimeConfig& config,
    std::string_view requested_program,
    std::string_view access_path);

struct InterposedPathResult {
    PathTranslation translation;
    bool opened = false;
    bool stated = false;
    int open_errno = 0;
    int stat_errno = 0;
    long long size_bytes = -1;
    std::string first_bytes;
    std::string report;
};

// Clean-room interposer scaffold: resolve a guest path through the ALR rootfs
// mapper, then perform host file operations on the translated path. This is
// deliberately host-testable and does not claim complete LD_PRELOAD coverage.
InterposedPathResult run_interposer_path_smoke(
    const InterposerConfig& config,
    std::string_view path,
    std::size_t max_read_bytes = 128);

}  // namespace alr::runtime
